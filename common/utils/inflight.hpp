#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace chatnow {

// 进程内 per-key 互斥注册表：用于合并同一 key 的并发缓存穿透请求。
// 第一个 miss 的请求 acquire(key) 获取互斥锁，unique_lock 锁定后穿透后端，
// warm 缓存，然后 release(key)。后续相同 key 的请求 acquire() 拿到同一个
// mutex，在 unique_lock 上阻塞直到第一个请求完成并 unlock。
class InflightRegistry {
public:
    using ptr = std::shared_ptr<InflightRegistry>;

    struct Guard {
        std::shared_ptr<std::mutex> mu;
        std::string key;
        InflightRegistry *registry;
    };

    Guard acquire(const std::string &key) {
        std::shared_ptr<std::mutex> mu;
        {
            std::lock_guard lk(_mu);
            auto it = _inflight.find(key);
            if (it == _inflight.end()) {
                mu = std::make_shared<std::mutex>();
                _inflight[key] = mu;
            } else {
                mu = it->second;
            }
        }
        return {mu, key, this};
    }

    void release(const std::string &key) {
        std::lock_guard lk(_mu);
        _inflight.erase(key);
    }

private:
    std::mutex _mu;
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> _inflight;
};

} // namespace chatnow
