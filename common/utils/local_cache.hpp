#pragma once

#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace chatnow {

// 线程安全的进程内本地缓存。TTL 自动过期，带硬容量和 LRU 淘汰。
template <typename V>
class LocalCache {
public:
    using ptr = std::shared_ptr<LocalCache<V>>;

    struct Stats {
        size_t hits = 0;
        size_t misses = 0;
        size_t evictions = 0;
        size_t expired = 0;
    };

    struct MetricsSink {
        std::function<void()> on_hit;
        std::function<void()> on_miss;
        std::function<void()> on_eviction;
        std::function<void()> on_expired;
    };

    explicit LocalCache(size_t max_entries = 4096)
        : _max_entries(max_entries) {
        _map.reserve(_max_entries);
    }

    LocalCache(size_t max_entries, MetricsSink metrics)
        : _max_entries(max_entries),
          _metrics(std::move(metrics)) {
        _map.reserve(_max_entries);
    }

    std::optional<V> get(const std::string &key) {
        std::optional<V> value;
        bool hit = false;
        bool miss = false;
        bool expired = false;
        {
            std::unique_lock lk(_mu);
            auto it = _map.find(key);
            if (it == _map.end()) {
                ++_stats.misses;
                miss = true;
            } else if (std::chrono::steady_clock::now() > it->second.expires_at) {
                erase_entry_(it);
                ++_stats.misses;
                ++_stats.expired;
                miss = true;
                expired = true;
            } else {
                touch_(it);
                ++_stats.hits;
                hit = true;
                value = it->second.value;
            }
        }
        if (hit) emit_(_metrics.on_hit);
        if (miss) emit_(_metrics.on_miss);
        if (expired) emit_(_metrics.on_expired);
        return value;
    }

    void set(const std::string &key, const V &value, std::chrono::seconds ttl) {
        if (ttl <= std::chrono::seconds::zero()) {
            invalidate(key);
            return;
        }
        if (_max_entries == 0) return;
        size_t evictions = 0;
        size_t expired = 0;
        {
            std::unique_lock lk(_mu);
            auto now = std::chrono::steady_clock::now();
            auto expires_at = now + ttl;
            auto it = _map.find(key);
            if (it != _map.end()) {
                if (now > it->second.expires_at) {
                    erase_entry_(it);
                    ++_stats.expired;
                    ++expired;
                } else {
                    it->second.value = value;
                    it->second.expires_at = expires_at;
                    touch_(it);
                    return;
                }
            }
            expired += erase_expired_(now);
            _lru.push_front(key);
            _map.emplace(key, Entry{value, expires_at, _lru.begin()});
            evictions = evict_over_capacity_();
        }
        emit_n_(_metrics.on_expired, expired);
        emit_n_(_metrics.on_eviction, evictions);
    }

    // CAS: 仅当 key 不存在或已过期时设置
    bool set_if_absent(const std::string &key, const V &value,
                       std::chrono::seconds ttl) {
        if (ttl <= std::chrono::seconds::zero()) return false;
        if (_max_entries == 0) return false;
        bool expired = false;
        size_t expired_pruned = 0;
        size_t evictions = 0;
        {
            std::unique_lock lk(_mu);
            auto it = _map.find(key);
            auto now = std::chrono::steady_clock::now();
            if (it != _map.end()) {
                if (now <= it->second.expires_at) {
                    touch_(it);
                    return false;
                }
                erase_entry_(it);
                ++_stats.expired;
                expired = true;
            }
            expired_pruned = erase_expired_(now);
            _lru.push_front(key);
            _map.emplace(key, Entry{value, now + ttl, _lru.begin()});
            evictions = evict_over_capacity_();
        }
        if (expired) emit_(_metrics.on_expired);
        emit_n_(_metrics.on_expired, expired_pruned);
        emit_n_(_metrics.on_eviction, evictions);
        return true;
    }

    void invalidate(const std::string &key) {
        std::unique_lock lk(_mu);
        auto it = _map.find(key);
        if (it != _map.end()) erase_entry_(it);
    }

    size_t size() const {
        std::shared_lock lk(_mu);
        return _map.size();
    }

    size_t evict_expired() {
        size_t removed = 0;
        {
            std::unique_lock lk(_mu);
            removed = erase_expired_(std::chrono::steady_clock::now());
        }
        emit_n_(_metrics.on_expired, removed);
        return removed;
    }

    Stats stats() const {
        std::shared_lock lk(_mu);
        return _stats;
    }

private:
    struct Entry {
        V value;
        std::chrono::steady_clock::time_point expires_at;
        typename std::list<std::string>::iterator lru_it;
    };

    using Map = std::unordered_map<std::string, Entry>;

    void touch_(typename Map::iterator it) {
        _lru.splice(_lru.begin(), _lru, it->second.lru_it);
        it->second.lru_it = _lru.begin();
    }

    typename Map::iterator erase_entry_(typename Map::iterator it) {
        _lru.erase(it->second.lru_it);
        return _map.erase(it);
    }

    size_t erase_expired_(std::chrono::steady_clock::time_point now) {
        size_t removed = 0;
        for (auto it = _map.begin(); it != _map.end(); ) {
            if (now > it->second.expires_at) {
                it = erase_entry_(it);
                ++removed;
            } else {
                ++it;
            }
        }
        _stats.expired += removed;
        return removed;
    }

    size_t evict_over_capacity_() {
        size_t evictions = 0;
        while (_map.size() > _max_entries && !_lru.empty()) {
            auto victim = _lru.back();
            auto it = _map.find(victim);
            if (it != _map.end()) {
                erase_entry_(it);
                ++_stats.evictions;
                ++evictions;
            } else {
                _lru.pop_back();
            }
        }
        return evictions;
    }

    static void emit_(const std::function<void()> &fn) {
        if (!fn) return;
        try {
            fn();
        } catch (...) {
            // Metrics callbacks must never break cache reads/writes.
        }
    }

    static void emit_n_(const std::function<void()> &fn, size_t n) {
        for (size_t i = 0; i < n; ++i) emit_(fn);
    }

    size_t _max_entries;
    mutable std::shared_mutex _mu;
    Map _map;
    std::list<std::string> _lru;
    Stats _stats;
    MetricsSink _metrics;
};

} // namespace chatnow
