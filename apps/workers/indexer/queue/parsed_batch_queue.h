#pragma once

#include <arrow/api.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace yikv::indexer {

struct ParsedBatch {
    std::shared_ptr<arrow::RecordBatch> batch;
    std::string                         path;
    std::size_t                         file_index_1based = 0;
};

struct ErrorState {
    std::atomic<bool> failed{false};
    std::mutex        mu;
    std::string       message;

    void Set(arrow::Status st) {
        if (st.ok()) return;
        std::lock_guard<std::mutex> lk(mu);
        if (!failed.exchange(true)) message = st.ToString();
    }
};

class BoundedParsedBatchQueue {
public:
    explicit BoundedParsedBatchQueue(std::size_t capacity);

    void                         Push(ParsedBatch item);
    void                         PushEos();
    void                         Abort();
    std::optional<ParsedBatch>   Pop();

private:
    std::mutex                   mu_;
    std::condition_variable      not_full_;
    std::condition_variable      not_empty_;
    std::deque<ParsedBatch>      q_;
    std::size_t                  cap_;
    bool                         eos_{false};
    bool                         aborted_{false};
};

}  // namespace yikv::indexer
