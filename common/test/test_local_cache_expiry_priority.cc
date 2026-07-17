#include "utils/local_cache.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using chatnow::LocalCache;
    using namespace std::chrono_literals;

    LocalCache<int> cache(2);
    cache.set("short", 1, 1s);
    cache.set("long", 2, 60s);

    assert(cache.get("short").has_value());
    std::this_thread::sleep_for(1100ms);

    cache.set("new", 3, 60s);

    assert(!cache.get("short").has_value());
    assert(cache.get("long").has_value());
    assert(cache.get("new").has_value());
    assert(cache.size() == 2);

    LocalCache<int>::MetricsSink sink;
    int expired_callbacks = 0;
    sink.on_expired = [&]() { ++expired_callbacks; };

    LocalCache<int> overwrite_cache(2, sink);
    overwrite_cache.set("same", 1, 1s);
    std::this_thread::sleep_for(1100ms);
    overwrite_cache.set("same", 2, 60s);

    auto same = overwrite_cache.get("same");
    assert(same.has_value() && *same == 2);
    assert(overwrite_cache.stats().expired == 1);
    assert(expired_callbacks == 1);

    std::cout << "local cache expiry priority tests passed\n";
    return 0;
}
