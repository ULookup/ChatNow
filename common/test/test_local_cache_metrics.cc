#include "utils/local_cache.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using chatnow::LocalCache;
    using namespace std::chrono_literals;

    int hits = 0;
    int misses = 0;
    int evictions = 0;
    int expired = 0;

    LocalCache<int>::MetricsSink sink;
    sink.on_hit = [&]() { ++hits; };
    sink.on_miss = [&]() { ++misses; };
    sink.on_eviction = [&]() { ++evictions; };
    sink.on_expired = [&]() { ++expired; };

    LocalCache<int> cache(1, sink);
    cache.set("a", 1, 60s);
    assert(cache.get("a").has_value());
    assert(hits == 1);

    assert(!cache.get("missing").has_value());
    assert(misses == 1);

    cache.set("b", 2, 60s);
    assert(evictions == 1);

    cache.set("short", 3, 1s);
    std::this_thread::sleep_for(1100ms);
    assert(!cache.get("short").has_value());
    assert(expired == 1);
    assert(misses == 2);

    std::cout << "local cache metrics tests passed\n";
    return 0;
}
