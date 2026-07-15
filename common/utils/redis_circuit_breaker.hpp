#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace chatnow {

class RedisCircuitOpen final : public std::runtime_error {
public:
    RedisCircuitOpen() : std::runtime_error("redis circuit open") {}
};

class RedisCircuitBreaker {
public:
    enum class State : uint8_t { Closed, Open, HalfOpen };
    struct Permit { bool probe = false; };

    Permit before_call();
    void on_success(Permit permit) noexcept;
    void on_connection_failure(Permit permit) noexcept;
    State state() const noexcept { return _state.load(std::memory_order_acquire); }

private:
    static constexpr uint32_t kFailureThreshold = 3;
    static constexpr int64_t kOpenForMs = 1000;
    static int64_t now_ms() noexcept;
    void open() noexcept;

    std::atomic<State> _state{State::Closed};
    std::atomic<uint32_t> _consecutive_failures{0};
    std::atomic<int64_t> _open_until_ms{0};
};

inline int64_t RedisCircuitBreaker::now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void RedisCircuitBreaker::open() noexcept {
    _open_until_ms.store(now_ms() + kOpenForMs, std::memory_order_release);
    _state.store(State::Open, std::memory_order_release);
}

inline RedisCircuitBreaker::Permit RedisCircuitBreaker::before_call() {
    auto current = _state.load(std::memory_order_acquire);
    if (current == State::Closed) return {};
    if (current == State::HalfOpen) throw RedisCircuitOpen();
    if (now_ms() < _open_until_ms.load(std::memory_order_acquire)) {
        throw RedisCircuitOpen();
    }
    auto expected = State::Open;
    if (_state.compare_exchange_strong(expected, State::HalfOpen,
                                       std::memory_order_acq_rel)) {
        return Permit{true};
    }
    throw RedisCircuitOpen();
}

inline void RedisCircuitBreaker::on_success(Permit) noexcept {
    _consecutive_failures.store(0, std::memory_order_relaxed);
    _state.store(State::Closed, std::memory_order_release);
}

inline void RedisCircuitBreaker::on_connection_failure(Permit permit) noexcept {
    if (permit.probe ||
        _consecutive_failures.fetch_add(1, std::memory_order_relaxed) + 1 >=
            kFailureThreshold) {
        open();
    }
}

}  // namespace chatnow
