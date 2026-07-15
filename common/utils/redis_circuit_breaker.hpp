#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>

namespace chatnow {

class RedisCircuitOpen final : public std::runtime_error {
public:
    RedisCircuitOpen() : std::runtime_error("redis circuit open") {}
};

class RedisCircuitBreaker {
public:
    enum class State : uint8_t { Closed, Open, HalfOpen };
    enum class Transition : uint8_t { None, Opened, Recovered };
    struct Permit {
        uint64_t generation = 0;
        bool probe = false;
    };

    Permit before_call();
    Transition on_success(Permit permit) noexcept;
    Transition on_connection_failure(Permit permit) noexcept;
    Transition on_abandoned(Permit permit) noexcept;
    State state() const noexcept { return _state.load(std::memory_order_acquire); }

private:
    static constexpr uint32_t kFailureThreshold = 3;
    static constexpr int64_t kOpenForMs = 1000;
    static int64_t now_ms() noexcept;
    Transition open_locked() noexcept;

    mutable std::mutex _mutex;
    std::atomic<State> _state{State::Closed};
    uint64_t _generation = 0;
    uint32_t _consecutive_failures = 0;
    int64_t _open_until_ms = 0;
};

inline int64_t RedisCircuitBreaker::now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline RedisCircuitBreaker::Transition RedisCircuitBreaker::open_locked() noexcept {
    if (_state.load(std::memory_order_relaxed) == State::Open) {
        return Transition::None;
    }
    ++_generation;
    _consecutive_failures = 0;
    _open_until_ms = now_ms() + kOpenForMs;
    _state.store(State::Open, std::memory_order_release);
    return Transition::Opened;
}

inline RedisCircuitBreaker::Permit RedisCircuitBreaker::before_call() {
    std::lock_guard<std::mutex> lock(_mutex);
    const auto current = _state.load(std::memory_order_relaxed);
    if (current == State::Closed) return {_generation, false};
    if (current == State::HalfOpen || now_ms() < _open_until_ms) {
        throw RedisCircuitOpen();
    }
    _state.store(State::HalfOpen, std::memory_order_release);
    return {_generation, true};
}

inline RedisCircuitBreaker::Transition
RedisCircuitBreaker::on_success(Permit permit) noexcept {
    std::lock_guard<std::mutex> lock(_mutex);
    if (permit.generation != _generation) return Transition::None;

    const auto current = _state.load(std::memory_order_relaxed);
    if (current == State::Closed && !permit.probe) {
        _consecutive_failures = 0;
        return Transition::None;
    }
    if (current == State::HalfOpen && permit.probe) {
        _consecutive_failures = 0;
        _state.store(State::Closed, std::memory_order_release);
        return Transition::Recovered;
    }
    return Transition::None;
}

inline RedisCircuitBreaker::Transition
RedisCircuitBreaker::on_connection_failure(Permit permit) noexcept {
    std::lock_guard<std::mutex> lock(_mutex);
    if (permit.generation != _generation) return Transition::None;

    const auto current = _state.load(std::memory_order_relaxed);
    if (current == State::HalfOpen && permit.probe) return open_locked();
    if (current != State::Closed || permit.probe) return Transition::None;
    if (++_consecutive_failures < kFailureThreshold) return Transition::None;
    return open_locked();
}

inline RedisCircuitBreaker::Transition
RedisCircuitBreaker::on_abandoned(Permit permit) noexcept {
    std::lock_guard<std::mutex> lock(_mutex);
    if (permit.generation != _generation || !permit.probe ||
        _state.load(std::memory_order_relaxed) != State::HalfOpen) {
        return Transition::None;
    }
    return open_locked();
}

}  // namespace chatnow
