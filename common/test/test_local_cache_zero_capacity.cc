#include "utils/local_cache.hpp"

#include <cassert>
#include <chrono>
#include <iostream>

int main() {
    using chatnow::LocalCache;
    using namespace std::chrono_literals;

    LocalCache<int> cache(0);

    cache.set("a", 1, 60s);
    assert(cache.size() == 0);
    assert(!cache.get("a").has_value());

    auto inserted = cache.set_if_absent("b", 2, 60s);
    assert(!inserted);
    assert(cache.size() == 0);
    assert(!cache.get("b").has_value());

    std::cout << "local cache zero capacity tests passed\n";
    return 0;
}
