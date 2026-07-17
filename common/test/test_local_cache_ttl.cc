#include "utils/local_cache.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

int main() {
    using chatnow::LocalCache;
    using namespace std::chrono_literals;

    LocalCache<int> cache(2);

    cache.set("zero", 1, 0s);
    assert(cache.size() == 0);
    assert(!cache.get("zero").has_value());

    bool inserted = cache.set_if_absent("also_zero", 2, 0s);
    assert(!inserted);
    assert(cache.size() == 0);
    assert(!cache.get("also_zero").has_value());

    cache.set("valid", 3, 60s);
    assert(cache.get("valid").has_value());

    std::cout << "local cache ttl tests passed\n";
    return 0;
}
