#include "indexer/queue/parsed_batch_queue.h"

namespace yikv::indexer {

BoundedParsedBatchQueue::BoundedParsedBatchQueue(std::size_t capacity) : cap_(capacity ? capacity : 1) {}

void BoundedParsedBatchQueue::Push(ParsedBatch item) {
    std::unique_lock<std::mutex> lk(mu_);
    not_full_.wait(lk, [&] { return q_.size() < cap_ || aborted_; });
    if (aborted_) return;
    q_.push_back(std::move(item));
    not_empty_.notify_one();
}

void BoundedParsedBatchQueue::PushEos() {
    std::lock_guard<std::mutex> lk(mu_);
    eos_ = true;
    not_empty_.notify_all();
}

void BoundedParsedBatchQueue::Abort() {
    std::lock_guard<std::mutex> lk(mu_);
    aborted_ = true;
    eos_     = true;
    not_empty_.notify_all();
    not_full_.notify_all();
}

std::optional<ParsedBatch> BoundedParsedBatchQueue::Pop() {
    std::unique_lock<std::mutex> lk(mu_);
    not_empty_.wait(lk, [&] { return !q_.empty() || eos_ || aborted_; });
    if (q_.empty()) return std::nullopt;
    ParsedBatch out = std::move(q_.front());
    q_.pop_front();
    not_full_.notify_all();
    return out;
}

}  // namespace yikv::indexer
