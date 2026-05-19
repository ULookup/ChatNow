#pragma once

#include <chrono>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace chatnow {

// 线程安全的进程内本地缓存。TTL 自动过期（lazy eviction），读写锁保护。
template <typename V>
class LocalCache {
public:
    using ptr = std::shared_ptr<LocalCache<V>>;

    explicit LocalCache(size_t size_hint = 4096) { _map.reserve(size_hint); }

    std::optional<V> get(const std::string &key) {
        std::shared_lock lk(_mu);
        auto it = _map.find(key);
        if (it == _map.end()) return std::nullopt;
        if (std::chrono::steady_clock::now() > it->second.expires_at)
            return std::nullopt;
        return it->second.value;
    }

    void set(const std::string &key, const V &value, std::chrono::seconds ttl) {
        std::unique_lock lk(_mu);
        _map[key] = {value, std::chrono::steady_clock::now() + ttl};
    }

    // CAS: 仅当 key 不存在或已过期时设置
    bool set_if_absent(const std::string &key, const V &value,
                       std::chrono::seconds ttl) {
        std::unique_lock lk(_mu);
        auto it = _map.find(key);
        if (it != _map.end() &&
            std::chrono::steady_clock::now() <= it->second.expires_at) {
            return false;
        }
        _map[key] = {value, std::chrono::steady_clock::now() + ttl};
        return true;
    }

    void invalidate(const std::string &key) {
        std::unique_lock lk(_mu);
        _map.erase(key);
    }

    size_t size() const {
        std::shared_lock lk(_mu);
        return _map.size();
    }

    size_t evict_expired() {
        std::unique_lock lk(_mu);
        auto now = std::chrono::steady_clock::now();
        size_t removed = 0;
        for (auto it = _map.begin(); it != _map.end(); ) {
            if (now > it->second.expires_at) { it = _map.erase(it); ++removed; }
            else { ++it; }
        }
        return removed;
    }

private:
    struct Entry {
        V value;
        std::chrono::steady_clock::time_point expires_at;
    };
    mutable std::shared_mutex _mu;
    std::unordered_map<std::string, Entry> _map;
};

} // namespace chatnow
