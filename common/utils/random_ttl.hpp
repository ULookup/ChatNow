#pragma once
#include <chrono>
#include <cmath>
#include <random>

namespace chatnow {
inline std::chrono::seconds randomized_ttl(std::chrono::seconds base) {
    long base_sec = base.count();
    double jitter_sec = static_cast<double>(base_sec) * 0.20;
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<double> dist(-jitter_sec, jitter_sec);
    long adjusted = base_sec + static_cast<long>(std::round(dist(rng)));
    if (adjusted < 1) adjusted = 1;
    return std::chrono::seconds(adjusted);
}
} // namespace chatnow
