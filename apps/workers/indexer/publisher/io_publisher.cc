#include "indexer/publisher/io_publisher.h"

#include "indexer/queue/parsed_batch_queue.h"
#include "indexer/source/source.h"

#include <thread>
#include <vector>

namespace yikv::indexer {

void IoPublisher::Run(Source& src, unsigned num_workers, BoundedParsedBatchQueue& q, ErrorState& err) {
    if (num_workers < 1) num_workers = 1;
    std::vector<std::thread> threads;
    threads.reserve(num_workers);
    for (unsigned i = 0; i < num_workers; ++i) {
        threads.emplace_back([&src, &q, &err]() { src.ProduceLoop(q, err); });
    }
    for (auto& th : threads) th.join();
}

}  // namespace yikv::indexer
