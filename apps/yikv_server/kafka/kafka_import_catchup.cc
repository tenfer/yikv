#include "kafka/kafka_import_catchup.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <librdkafka/rdkafka.h>
#include <nlohmann/json.hpp>

#include "stream/json_stream_ingest.h"

#include "index/kv_index.h"
#include "schema/schema.h"

namespace yikv_server::kafka {

namespace {

static std::string WallTs() {
    auto          now = std::chrono::system_clock::now();
    std::time_t   t   = std::chrono::system_clock::to_time_t(now);
    char          buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

static void SaveOffsetFile(const std::filesystem::path& table_dir, int64_t offset) {
    const auto path = table_dir / "kafka.offset";
    std::ofstream f(path, std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write kafka.offset: " + path.string());
    f << offset << "\n";
}

static void WriteMetaFile(const std::filesystem::path& table_dir,
                          const KafkaImportCatchupOptions& opt, int64_t last_offset) {
    const auto path = table_dir / "kafka_meta.json";
    nlohmann::json j;
    j["topic"]                   = opt.topic;
    j["partition"]               = opt.partition;
    j["brokers"]                 = opt.brokers;
    j["offline_watermark_sec"]   = opt.offline_watermark_sec;
    j["rewind_minutes"]          = opt.rewind_minutes;
    j["last_committed_offset"]   = last_offset;
    j["catchup_completed_unix"]  = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::ofstream f(path, std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write kafka_meta.json: " + path.string());
    f << j.dump(2) << "\n";
}

}  // namespace

bool RunKafkaImportCatchup(const std::filesystem::path& table_dir,
                           yikv::index::KVIndex*        idx,
                           const yikv::schema::Schema*  schema,
                           const KafkaImportCatchupOptions& opt) {
    auto inf = [&](std::string_view s) {
        if (opt.log_info) opt.log_info(s);
        else std::cerr << "[" << WallTs() << "][kafka_catchup] " << s << "\n";
    };
    auto err = [&](std::string_view s) {
        if (opt.log_err) opt.log_err(s);
        else std::cerr << "[" << WallTs() << "][kafka_catchup] ERROR: " << s << "\n";
    };

    if (opt.brokers.empty()) {
        err("brokers empty");
        return false;
    }
    if (opt.topic.empty()) {
        err("topic empty");
        return false;
    }

    char errstr[512];
    rd_kafka_conf_t* conf = rd_kafka_conf_new();
    if (rd_kafka_conf_set(conf, "bootstrap.servers", opt.brokers.c_str(), errstr,
                          sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        err(errstr);
        rd_kafka_conf_destroy(conf);
        return false;
    }
    rd_kafka_conf_set(conf, "enable.auto.commit", "false", nullptr, 0);
    rd_kafka_conf_set(conf, "auto.offset.reset", "earliest", nullptr, 0);
    // Emit EOF event so the one-shot loop can terminate cleanly after draining.
    rd_kafka_conf_set(conf, "enable.partition.eof", "true", nullptr, 0);

    rd_kafka_t* rk = rd_kafka_new(RD_KAFKA_CONSUMER, conf, errstr, sizeof(errstr));
    if (!rk) {
        err(errstr);
        return false;
    }

    int64_t ts_ms = (opt.offline_watermark_sec - static_cast<int64_t>(opt.rewind_minutes) * 60) *
                    1000;
    if (ts_ms < 0) ts_ms = 0;

    rd_kafka_topic_partition_list_t* tpl = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(tpl, opt.topic.c_str(), opt.partition);
    tpl->elems[0].offset = ts_ms;

    rd_kafka_resp_err_t qerr =
        rd_kafka_offsets_for_times(rk, tpl, opt.offsets_query_timeout_ms);
    if (qerr) {
        std::ostringstream oss;
        oss << "offsets_for_times: " << rd_kafka_err2str(qerr);
        err(oss.str());
        rd_kafka_topic_partition_list_destroy(tpl);
        rd_kafka_destroy(rk);
        return false;
    }
    if (tpl->elems[0].err) {
        std::ostringstream oss;
        oss << "partition error after offsets_for_times: "
            << rd_kafka_err2str(static_cast<rd_kafka_resp_err_t>(tpl->elems[0].err));
        err(oss.str());
        rd_kafka_topic_partition_list_destroy(tpl);
        rd_kafka_destroy(rk);
        return false;
    }

    int64_t start_offset = tpl->elems[0].offset;
    rd_kafka_topic_partition_list_destroy(tpl);

    if (start_offset < 0) start_offset = RD_KAFKA_OFFSET_BEGINNING;

    {
        std::ostringstream oss;
        oss << "seek topic=" << opt.topic << " partition=" << opt.partition
            << " start_offset=" << start_offset << " (from ts_ms=" << ts_ms << ")";
        inf(oss.str());
    }

    rd_kafka_topic_t* rkt = rd_kafka_topic_new(rk, opt.topic.c_str(), nullptr);
    if (!rkt) {
        err("rd_kafka_topic_new failed");
        rd_kafka_destroy(rk);
        return false;
    }

    if (rd_kafka_consume_start(rkt, opt.partition, start_offset) == -1) {
        std::ostringstream oss;
        oss << "rd_kafka_consume_start: " << rd_kafka_err2str(rd_kafka_last_error());
        err(oss.str());
        rd_kafka_topic_destroy(rkt);
        rd_kafka_destroy(rk);
        return false;
    }

    stream::LogFn slog = [&err](std::string_view m) { err(m); };

    int64_t last_offset = start_offset - 1;
    int     silence     = 0;
    bool    saw_eof     = false;
    const int max_loops =
        opt.max_silence_loops > 0 ? opt.max_silence_loops : 3;
    const int consume_tmo =
        opt.consume_timeout_ms > 0 ? opt.consume_timeout_ms : 200;
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(opt.max_wall_seconds > 0 ? opt.max_wall_seconds : 7200);

    int idle_polls = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        rd_kafka_message_t* msg = rd_kafka_consume(rkt, opt.partition, consume_tmo);
        if (!msg) {
            if (saw_eof) {
                if (++silence >= max_loops) break;
            } else if (++idle_polls > 50'000) {
                err("catch-up idle timeout (no EOF, aborting)");
                rd_kafka_consume_stop(rkt, opt.partition);
                rd_kafka_topic_destroy(rkt);
                rd_kafka_destroy(rk);
                return false;
            }
            continue;
        }
        idle_polls = 0;

        if (msg->err) {
            if (msg->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
                saw_eof = true;
                rd_kafka_message_destroy(msg);
                continue;
            }
            std::ostringstream oss;
            oss << "consume error: " << rd_kafka_message_errstr(msg);
            err(oss.str());
            rd_kafka_message_destroy(msg);
            rd_kafka_consume_stop(rkt, opt.partition);
            rd_kafka_topic_destroy(rkt);
            rd_kafka_destroy(rk);
            return false;
        }

        silence = 0;

        std::string_view payload(static_cast<const char*>(msg->payload),
                                 static_cast<size_t>(msg->len));
        const int64_t offset = msg->offset;
        rd_kafka_message_destroy(msg);

        try {
            auto j = nlohmann::json::parse(payload);
            if (j.is_array()) {
                for (const auto& item : j) {
                    if (!item.is_object()) {
                        err("batch element is not a JSON object, skipping");
                        continue;
                    }
                    if (!stream::ApplyStreamJsonObject(idx, schema, item, slog)) {
                        // logged inside
                    }
                }
            } else if (j.is_object()) {
                stream::ApplyStreamJsonObject(idx, schema, j, slog);
            } else {
                err("message is neither object nor array");
            }
        } catch (const nlohmann::json::exception& e) {
            std::ostringstream oss;
            oss << "JSON parse failed at offset=" << offset << ": " << e.what();
            err(oss.str());
        }

        last_offset = offset;
    }

    if (std::chrono::steady_clock::now() >= deadline)
        err("catch-up wall-clock limit reached; committing partial progress");

    rd_kafka_consume_stop(rkt, opt.partition);
    rd_kafka_topic_destroy(rkt);
    rd_kafka_destroy(rk);

    try {
        SaveOffsetFile(table_dir, last_offset);
        WriteMetaFile(table_dir, opt, last_offset);
    } catch (const std::exception& e) {
        err(e.what());
        return false;
    }

    {
        std::ostringstream oss;
        oss << "catch-up finished last_offset=" << last_offset;
        inf(oss.str());
    }
    return true;
}

}  // namespace yikv_server::kafka
