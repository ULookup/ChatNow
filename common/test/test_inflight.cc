#include "utils/inflight.hpp"

#include <cassert>
#include <iostream>

int main() {
    chatnow::InflightRegistry registry;

    auto first = registry.acquire("k");
    auto second = registry.acquire("k");
    assert(first.mu == second.mu);
    assert(registry.size() == 1);

    first = chatnow::InflightRegistry::Guard{};
    assert(registry.size() == 1);

    auto third = registry.acquire("k");
    assert(third.mu == second.mu);
    assert(registry.size() == 1);

    second = chatnow::InflightRegistry::Guard{};
    assert(registry.size() == 1);

    third = chatnow::InflightRegistry::Guard{};
    assert(registry.size() == 0);

    std::cout << "inflight tests passed\n";
    return 0;
}
