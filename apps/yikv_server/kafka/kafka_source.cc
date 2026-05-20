#include "kafka/kafka_source.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>

#include <librdkafka/rdkafka.h>
#include <nlohmann/json.hpp>

#include "metrics/metrics.h"
#include "stream/json_stream_ingest.h"

namespace yikv_server::kafka {

using yikv::index::KVIndex;
using yikv::schema::Schema;

// ─── helpers ──────────────────────────────────────────────────────────────────

static std::string WallTs() {
    auto        now = std::chrono::system_clock::now();
    std::time_t t   = std::chrono::system_clock::to_time_t(now);
    char        buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

#define LOG_ERR(msg) std::cerr << "[" << WallTs() << "][kafka] ERROR: " << msg << "\n"
#define LOG_INF(msg) std::cerr << "[" << WallTs() << "][kafka] " << msg << "\n"

// ─── KafkaSource ─────────────────────────────────────────────────────────────

KafkaSource::KafkaSource(KVIndex* idx, const Schema* schema, Config cfg)
    : idx_(idx), schema_(schema), cfg_(std::move(cfg)) {}

KafkaSource::~KafkaSource() {
    Stop();
}

void KafkaSource::Start() {
    thread_ = std::thread(&KafkaSource::ConsumeLoop, this);
}

void KafkaSource::Stop() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
}

// ─── Offset file ─────────────────────────────────────────────────────────────

int64_t KafkaSource::LoadOffset() const {
    std::ifstream f(cfg_.offset_file);
    if (!f) return RD_KAFKA_OFFSET_BEGINNING;
    int64_t off = RD_KAFKA_OFFSET_BEGINNING;
    f >> off;
    // Resume from the next message after the last committed one.
    return (off >= 0) ? off + 1 : RD_KAFKA_OFFSET_BEGINNING;
}

void KafkaSource::SaveOffset(int64_t offset) const {
    std::ofstream f(cfg_.offset_file, std::ios::trunc);
    if (!f) {
        LOG_ERR("cannot write offset file: " << cfg_.offset_file);
        return;
    }
    f << offset << "\n";
}

// ─── Consume loop ─────────────────────────────────────────────────────────────

void KafkaSource::ConsumeLoop() {
    char errstr[512];

    rd_kafka_conf_t* conf = rd_kafka_conf_new();
    if (rd_kafka_conf_set(conf, "bootstrap.servers", cfg_.brokers.c_str(),
                          errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        LOG_ERR("conf bootstrap.servers: " << errstr);
        rd_kafka_conf_destroy(conf);
        return;
    }
    // Disable auto-commit; we manage offsets manually via the offset file.
    rd_kafka_conf_set(conf, "enable.auto.commit", "false", nullptr, 0);
    rd_kafka_conf_set(conf, "auto.offset.reset", "earliest", nullptr, 0);

    rd_kafka_t* rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
    if (!rk) {
        LOG_ERR("rd_kafka_new: " << errstr);
        return;
    }
    // conf is now owned by rk.

    int64_t start_offset = LoadOffset();

    rd_kafka_topic_t* rkt = rd_kafka_topic_new(rk, cfg_.topic.c_str(), nullptr);
    if (!rkt) {
        LOG_ERR("rd_kafka_topic_new failed for topic: " << cfg_.topic);
        rd_kafka_destroy(rk);
        return;
    }

    if (rd_kafka_consume_start(rkt, cfg_.partition, start_offset) == -1) {
        LOG_ERR("rd_kafka_consume_start: " << rd_kafka_err2str(rd_kafka_last_error()));
        rd_kafka_topic_destroy(rkt);
        rd_kafka_destroy(rk);
        return;
    }

    LOG_INF("consuming topic=" << cfg_.topic << " partition=" << cfg_.partition
                                 << " start_offset=" << start_offset);

    stream::LogFn slog = [](std::string_view m) { LOG_ERR(m); };

    // Use the topic as the table label. KafkaSource is per-topic anyway.
    auto& M = metrics::Metrics::instance();
    const std::string& tlabel = cfg_.topic;

    while (!stop_.load(std::memory_order_relaxed)) {
        rd_kafka_message_t* msg = rd_kafka_consume(rkt, cfg_.partition, /*timeout_ms=*/200);
        if (!msg) continue;

        if (msg->err) {
            if (msg->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
                rd_kafka_message_destroy(msg);
                continue;
            }
            LOG_ERR("consume error: " << rd_kafka_message_errstr(msg));
            rd_kafka_message_destroy(msg);
            continue;
        }

        std::string_view payload(static_cast<const char*>(msg->payload),
                                 static_cast<size_t>(msg->len));
        const int64_t offset = msg->offset;
        rd_kafka_message_destroy(msg);

        const auto t_apply = std::chrono::steady_clock::now();
        bool       parse_ok  = true;
        bool       apply_ok  = true;

        try {
            auto j = nlohmann::json::parse(payload);
            if (j.is_array()) {
                for (const auto& item : j) {
                    if (!item.is_object()) {
                        LOG_ERR("batch element is not a JSON object, skipping");
                        apply_ok = false;
                        continue;
                    }
                    if (!stream::ApplyStreamJsonObject(idx_, schema_, item, slog))
                        apply_ok = false;
                }
            } else if (j.is_object()) {
                if (!stream::ApplyStreamJsonObject(idx_, schema_, j, slog))
                    apply_ok = false;
            } else {
                LOG_ERR("message is neither object nor array, offset=" << offset);
                apply_ok = false;
            }
        } catch (const nlohmann::json::exception& e) {
            LOG_ERR("JSON parse failed at offset=" << offset << ": " << e.what());
            parse_ok = false;
        }

        const auto apply_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - t_apply).count();

        if (!parse_ok) {
            M.kafka.parse_errors_total.IncFor(tlabel);
        } else {
            if (!apply_ok) M.kafka.apply_errors_total.IncFor(tlabel);
            M.kafka.messages_consumed_total.IncFor(tlabel);
            M.kafka.apply_latency_us.RecordFor(static_cast<int64_t>(apply_us), tlabel);
        }

        SaveOffset(offset);
        M.kafka.committed_offset.SetFor(offset, tlabel);
    }

    rd_kafka_consume_stop(rkt, cfg_.partition);
    rd_kafka_topic_destroy(rkt);
    rd_kafka_destroy(rk);

    LOG_INF("consumer stopped for topic=" << cfg_.topic);
}

}  // namespace yikv_server::kafka
