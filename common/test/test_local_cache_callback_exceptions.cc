#include "utils/local_cache.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

int main() {
    using chatnow::LocalCache;
    using namespace std::chrono_literals;

    LocalCache<int>::MetricsSink sink;
    sink.on_hit = []() { throw std::runtime_error("hit metric failed"); };
    sink.on_miss = []() { throw std::runtime_error("miss metric failed"); };
    sink.on_eviction = []() { throw std::runtime_error("evict metric failed"); };
    sink.on_expired = []() { throw std::runtime_error("expired metric failed"); };

    LocalCache<int> cache(1, sink);

    cache.set("a", 1, 60s);
    assert(cache.get("a").has_value());
    assert(!cache.get("missing").has_value());

    cache.set("b", 2, 60s);
    assert(cache.get("b").has_value());

    cache.set("short", 3, 1s);
    std::this_thread::sleep_for(1100ms);
    assert(!cache.get("short").has_value());

    std::cout << "local cache callback exception tests passed\n";
    return 0;
}
