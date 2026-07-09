#pragma once

#include <string>
#include <algorithm>

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
        try {
            auto id = std::stoull(value.substr(prefix.size()));
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

    if (std::all_of(value.begin(), value.end(),
                    [](char c) { return c >= '0' && c <= '9'; })) {
        try {
            auto id = std::stoull(value);
            if (id > 0) return {IdempotencyStatus::Accepted, id};
        } catch (...) {
        }
    }
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

} // namespace chatnow
