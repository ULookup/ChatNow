#pragma once

#include <string>
#include <algorithm>
#include <cstdint>

namespace chatnow {

enum class IdempotencyStatus {
    Empty = 0,
    Pending,
    Accepted,
    Persisted,
    Corrupt,
};

struct IdempotencyState {
    IdempotencyStatus status{IdempotencyStatus::Empty};
    unsigned long long message_id{0};
};

inline IdempotencyState parse_idempotency_state(const std::string &value) {
    if (value.empty()) return {IdempotencyStatus::Empty, 0};
    if (value == "pending") return {IdempotencyStatus::Pending, 0};

    auto parse_with_prefix = [&](const std::string &prefix,
                                 IdempotencyStatus status) -> IdempotencyState {
        if (value.rfind(prefix, 0) != 0) return {IdempotencyStatus::Empty, 0};
        auto raw_id = value.substr(prefix.size());
        if (raw_id.empty() ||
            !std::all_of(raw_id.begin(), raw_id.end(),
                         [](char c) { return c >= '0' && c <= '9'; })) {
            return {IdempotencyStatus::Corrupt, 0};
        }
        try {
            auto id = std::stoull(raw_id);
            if (id == 0) return {IdempotencyStatus::Corrupt, 0};
            return {status, id};
        } catch (...) {
            return {IdempotencyStatus::Corrupt, 0};
        }
    };

    auto accepted = parse_with_prefix("accepted:", IdempotencyStatus::Accepted);
    if (accepted.status != IdempotencyStatus::Empty) return accepted;
    auto persisted = parse_with_prefix("persisted:", IdempotencyStatus::Persisted);
    if (persisted.status != IdempotencyStatus::Empty) return persisted;

    return {IdempotencyStatus::Corrupt, 0};
}

inline std::string serialize_idempotency_state(const IdempotencyState &state) {
    switch (state.status) {
        case IdempotencyStatus::Pending:
            return "pending";
        case IdempotencyStatus::Accepted:
            return "accepted:" + std::to_string(state.message_id);
        case IdempotencyStatus::Persisted:
            return "persisted:" + std::to_string(state.message_id);
        case IdempotencyStatus::Empty:
        case IdempotencyStatus::Corrupt:
        default:
            return "";
    }
}

inline std::string idempotency_key_for(const std::string &uid,
                                       const std::string &client_msg_id) {
    return "im:msg:idem:" + uid + ":" + client_msg_id;
}

inline bool should_remove_cross_outbox(bool rpc_started, bool rpc_succeeded) {
    return rpc_started && rpc_succeeded;
}

struct TokenBucketDecision {
    bool allowed{false};
    int tokens{0};
    int64_t reset_at_ms{0};
};

inline TokenBucketDecision compute_token_bucket(int current_tokens,
                                                int64_t last_refill_ms,
                                                int64_t now_ms,
                                                int window_sec,
                                                int capacity) {
    if (capacity <= 0 || window_sec <= 0) return {true, capacity, now_ms};
    if (last_refill_ms <= 0 || now_ms < last_refill_ms) {
        current_tokens = capacity;
        last_refill_ms = now_ms;
    }

    int64_t interval_ms = static_cast<int64_t>(window_sec) * 1000 / capacity;
    if (interval_ms <= 0) interval_ms = 1;
    int64_t elapsed_ms = now_ms - last_refill_ms;
    int refill = static_cast<int>(elapsed_ms / interval_ms);
    if (refill > 0) {
        current_tokens = std::min(capacity, current_tokens + refill);
        last_refill_ms += static_cast<int64_t>(refill) * interval_ms;
    }

    if (current_tokens <= 0) return {false, 0, last_refill_ms};
    return {true, current_tokens - 1, last_refill_ms};
}

inline uint64_t resolve_ack_session_seq(uint64_t message_session_seq) {
    return message_session_seq;
}

inline bool is_valid_push_ack_ids(uint64_t user_seq, int64_t message_id) {
    return user_seq > 0 && message_id > 0;
}

} // namespace chatnow
