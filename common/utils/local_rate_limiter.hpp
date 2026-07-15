#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace chatnow {

class LocalRateLimiter {
public:
    bool allow(const std::string &key, int capacity, int window_sec) {
        const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return allow(key, capacity, window_sec, now_ms);
    }

    bool allow(const std::string &key, int capacity, int window_sec, int64_t now_ms) {
        if (capacity <= 0 || window_sec <= 0) return true;

        const size_t shard_index = std::hash<std::string>{}(key) % kShardCount;
        Shard &shard = _shards[shard_index];
        std::lock_guard<std::mutex> lock(shard.mu);

        const uint64_t operation = _operations.fetch_add(1, std::memory_order_relaxed) + 1;
        if (operation % 1024 == 0) {
            cleanup_(shard, now_ms, window_sec);
        }

        auto found = shard.buckets.find(key);
        if (found == shard.buckets.end()) {
            if (!reserve_bucket_()) return false;
            try {
                found = shard.buckets.emplace(
                    key, Bucket{static_cast<double>(capacity - 1), now_ms, now_ms}).first;
            } catch (...) {
                _bucket_count.fetch_sub(1, std::memory_order_relaxed);
                throw;
            }
            return true;
        }

        Bucket &bucket = found->second;
        const int64_t elapsed_ms = now_ms > bucket.refill_ms ? now_ms - bucket.refill_ms : 0;
        const double refill = static_cast<double>(elapsed_ms) * capacity /
                              (static_cast<double>(window_sec) * 1000.0);
        bucket.tokens = std::min(static_cast<double>(capacity), bucket.tokens + refill);
        bucket.refill_ms = now_ms;
        bucket.seen_ms = now_ms;
        if (bucket.tokens < 1.0) return false;
        bucket.tokens -= 1.0;
        return true;
    }

private:
    struct Bucket {
        double tokens;
        int64_t refill_ms;
        int64_t seen_ms;
    };
    struct Shard {
        std::mutex mu;
        std::unordered_map<std::string, Bucket> buckets;
    };

    static constexpr size_t kShardCount = 64;
    static constexpr size_t kMaxBuckets = 65536;

    bool reserve_bucket_() {
        size_t count = _bucket_count.load(std::memory_order_relaxed);
        while (count < kMaxBuckets) {
            if (_bucket_count.compare_exchange_weak(
                    count, count + 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void cleanup_(Shard &shard, int64_t now_ms, int window_sec) {
        const int64_t idle_limit_ms = static_cast<int64_t>(window_sec) * 2000;
        size_t removed = 0;
        for (auto it = shard.buckets.begin(); it != shard.buckets.end();) {
            if (now_ms > it->second.seen_ms &&
                now_ms - it->second.seen_ms > idle_limit_ms) {
                it = shard.buckets.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        if (removed != 0) {
            _bucket_count.fetch_sub(removed, std::memory_order_relaxed);
        }
    }

    std::array<Shard, kShardCount> _shards;
    std::atomic<size_t> _bucket_count{0};
    std::atomic<uint64_t> _operations{0};
};

}  // namespace chatnow
