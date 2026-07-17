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
    std::atomic<uint64_t> _generation{0};
    std::atomic<uint32_t> _consecutive_failures{0};
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
    _consecutive_failures.store(0, std::memory_order_relaxed);
    _open_until_ms = now_ms() + kOpenForMs;
    _state.store(State::Open, std::memory_order_release);
    _generation.fetch_add(1, std::memory_order_acq_rel);
    return Transition::Opened;
}

inline RedisCircuitBreaker::Permit RedisCircuitBreaker::before_call() {
    const auto generation = _generation.load(std::memory_order_acquire);
    const auto current = _state.load(std::memory_order_acquire);
    if (current == State::Closed) {
        return {generation, false};
    }
    std::lock_guard<std::mutex> lock(_mutex);
    const auto locked_current = _state.load(std::memory_order_relaxed);
    if (locked_current == State::Closed) {
        return {_generation.load(std::memory_order_acquire), false};
    }
    if (locked_current == State::HalfOpen || now_ms() < _open_until_ms) {
        throw RedisCircuitOpen();
    }
    _state.store(State::HalfOpen, std::memory_order_release);
    return {_generation.load(std::memory_order_acquire), true};
}

inline RedisCircuitBreaker::Transition
RedisCircuitBreaker::on_success(Permit permit) noexcept {
    const auto current = _state.load(std::memory_order_acquire);
    if (current == State::Closed && !permit.probe) {
        if (permit.generation == _generation.load(std::memory_order_acquire)) {
            _consecutive_failures.store(0, std::memory_order_relaxed);
        }
        return Transition::None;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    if (permit.generation != _generation.load(std::memory_order_relaxed)) return Transition::None;
    if (current == State::HalfOpen && permit.probe) {
        _consecutive_failures.store(0, std::memory_order_relaxed);
        _state.store(State::Closed, std::memory_order_release);
        return Transition::Recovered;
    }
    return Transition::None;
}

inline RedisCircuitBreaker::Transition
RedisCircuitBreaker::on_connection_failure(Permit permit) noexcept {
    if (permit.generation != _generation.load(std::memory_order_acquire)) return Transition::None;
    const auto current = _state.load(std::memory_order_acquire);
    if (current == State::Closed && !permit.probe) {
        const auto failures = _consecutive_failures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failures < kFailureThreshold) return Transition::None;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    if (permit.generation != _generation.load(std::memory_order_relaxed)) return Transition::None;
    const auto locked_current = _state.load(std::memory_order_relaxed);
    if (locked_current == State::HalfOpen && permit.probe) return open_locked();
    if (locked_current != State::Closed || permit.probe) return Transition::None;
    if (_consecutive_failures.load(std::memory_order_relaxed) < kFailureThreshold) return Transition::None;
    return open_locked();
}

inline RedisCircuitBreaker::Transition
RedisCircuitBreaker::on_abandoned(Permit permit) noexcept {
    std::lock_guard<std::mutex> lock(_mutex);
    if (permit.generation != _generation.load(std::memory_order_relaxed) || !permit.probe ||
        _state.load(std::memory_order_relaxed) != State::HalfOpen) {
        return Transition::None;
    }
    return open_locked();
}

}  // namespace chatnow
