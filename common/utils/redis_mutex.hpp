#pragma once

#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include "common/dao/data_redis.hpp"
#include "infra/logger.hpp"

namespace chatnow {

// 基于 "SET key token NX PX ttl_ms" + Lua CAS unlock 的短时互斥锁。
// 用于 cache warm 互斥、backfill 协调等毫秒-秒级场景。
class RedisMutex {
public:
    RedisMutex(RedisClient::ptr redis, const std::string &key, int ttl_ms = 5000)
        : _redis(std::move(redis)), _key("im:lock:" + key), _ttl_ms(ttl_ms)
    {
        _token = generate_token_();
    }

    bool try_lock(std::chrono::milliseconds timeout = std::chrono::milliseconds(100)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            bool ok = _redis->set(_key, _token, std::chrono::milliseconds(_ttl_ms),
                                  sw::redis::UpdateType::NOT_EXIST);
            if (ok) { _locked = true; return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    void unlock() {
        if (!_locked) return;
        static const char *kUnlockLua =
            "if redis.call('GET', KEYS[1]) == ARGV[1] then "
            "    return redis.call('DEL', KEYS[1]) "
            "end "
            "return 0";
        try {
            std::vector<std::string> keys = {_key};
            std::vector<std::string> args = {_token};
            _redis->eval<long long>(kUnlockLua, keys.begin(), keys.end(),
                                    args.begin(), args.end());
        } catch (std::exception &e) {
            LOG_WARN("RedisMutex.unlock 失败 {}: {}", _key, e.what());
        }
        _locked = false;
    }

private:
    static std::string generate_token_() {
        static thread_local std::mt19937_64 rng(std::random_device{}());
        std::uniform_int_distribution<unsigned long long> dist;
        return std::to_string(dist(rng));
    }

    RedisClient::ptr _redis;
    std::string _key;
    std::string _token;
    int _ttl_ms;
    bool _locked = false;
};

} // namespace chatnow
