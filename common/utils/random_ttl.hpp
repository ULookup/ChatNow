#pragma once
#include <chrono>
#include <random>

namespace chatnow {
inline std::chrono::seconds randomized_ttl(std::chrono::seconds base) {
    long base_sec = base.count();
    long jitter = base_sec / 5;
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<long> dist(-jitter, jitter);
    return std::chrono::seconds(base_sec + dist(rng));
}
} // namespace chatnow
