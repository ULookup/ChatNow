#include "utils/local_cache.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

int main() {
    using namespace std::chrono_literals;

    std::shared_ptr<chatnow::LocalCache<int>> cache;
    chatnow::LocalCache<int>::MetricsSink sink;
    std::atomic<bool> callback_completed{false};

    sink.on_miss = [&]() {
        (void)cache->size();
        callback_completed = true;
    };
    cache = std::make_shared<chatnow::LocalCache<int>>(2, sink);

    std::thread worker([&]() {
        (void)cache->get("missing");
    });

    for (int i = 0; i < 50 && !callback_completed.load(); ++i) {
        std::this_thread::sleep_for(10ms);
    }

    assert(callback_completed.load());
    worker.join();

    std::cout << "local cache callback tests passed\n";
    return 0;
}
