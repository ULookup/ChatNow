#include "utils/local_cache.hpp"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

int main() {
    using chatnow::LocalCache;
    using namespace std::chrono_literals;

    LocalCache<int> cache(2);
    cache.set("a", 1, 60s);
    cache.set("b", 2, 60s);

    auto a = cache.get("a");
    assert(a.has_value() && *a == 1);

    cache.set("c", 3, 60s);
    assert(cache.size() == 2);
    assert(cache.get("a").has_value());
    assert(!cache.get("b").has_value());
    assert(cache.get("c").has_value());

    cache.set("short", 4, 1s);
    std::this_thread::sleep_for(1100ms);
    assert(!cache.get("short").has_value());
    assert(cache.size() <= 2);

    auto stats = cache.stats();
    assert(stats.hits >= 3);
    assert(stats.misses >= 2);
    assert(stats.evictions >= 1);
    assert(stats.expired >= 1);

    std::cout << "local cache tests passed\n";
    return 0;
}
