#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace chatnow {

class InflightRegistry {
public:
    using ptr = std::shared_ptr<InflightRegistry>;

    class Guard {
    public:
        Guard() = default;
        Guard(std::shared_ptr<std::mutex> m, std::string k, InflightRegistry *r)
            : mu(std::move(m)), key(std::move(k)), registry(r) {}

        ~Guard() { if (registry) registry->release(key); }

        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;
        Guard(Guard &&o) noexcept
            : mu(std::move(o.mu)), key(std::move(o.key)), registry(o.registry) {
            o.registry = nullptr;
        }
        Guard &operator=(Guard &&o) noexcept {
            if (this != &o) {
                if (registry) registry->release(key);
                mu = std::move(o.mu);
                key = std::move(o.key);
                registry = o.registry;
                o.registry = nullptr;
            }
            return *this;
        }

        std::shared_ptr<std::mutex> mu;
        std::string key;

    private:
        InflightRegistry *registry = nullptr;
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
