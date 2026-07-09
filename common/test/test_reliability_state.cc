#include "utils/reliability_state.hpp"

#include <cassert>
#include <iostream>

int main() {
    using chatnow::IdempotencyState;
    using chatnow::IdempotencyStatus;
    using chatnow::parse_idempotency_state;
    using chatnow::serialize_idempotency_state;
    using chatnow::should_remove_cross_outbox;
    using chatnow::idempotency_key_for;
    using chatnow::compute_token_bucket;
    using chatnow::resolve_ack_session_seq;

    auto pending = parse_idempotency_state("pending");
    assert(pending.status == IdempotencyStatus::Pending);
    assert(pending.message_id == 0);

    auto accepted = parse_idempotency_state("accepted:42");
    assert(accepted.status == IdempotencyStatus::Accepted);
    assert(accepted.message_id == 42);

    auto persisted = parse_idempotency_state("persisted:99");
    assert(persisted.status == IdempotencyStatus::Persisted);
    assert(persisted.message_id == 99);

    auto legacy = parse_idempotency_state("42");
    assert(legacy.status == IdempotencyStatus::Corrupt);

    auto corrupt = parse_idempotency_state("not-a-state");
    assert(corrupt.status == IdempotencyStatus::Corrupt);

    assert(serialize_idempotency_state({IdempotencyStatus::Pending, 0}) == "pending");
    assert(serialize_idempotency_state({IdempotencyStatus::Accepted, 42}) == "accepted:42");
    assert(serialize_idempotency_state({IdempotencyStatus::Persisted, 99}) == "persisted:99");
    assert(idempotency_key_for("u1", "c1") == "im:msg:idem:u1:c1");

    assert(!should_remove_cross_outbox(false, true));
    assert(!should_remove_cross_outbox(true, false));
    assert(should_remove_cross_outbox(true, true));

    auto empty = compute_token_bucket(0, 0, 1000, 60, 10);
    assert(empty.allowed);
    assert(empty.tokens == 9);
    assert(empty.reset_at_ms == 1000);

    auto refilled = compute_token_bucket(5, 1000, 61 * 1000, 60, 10);
    assert(refilled.allowed);
    assert(refilled.tokens == 9);
    assert(refilled.reset_at_ms == 61 * 1000);

    auto denied = compute_token_bucket(0, 1000, 2 * 1000, 60, 10);
    assert(!denied.allowed);
    assert(denied.tokens == 0);
    assert(denied.reset_at_ms == 1000);

    assert(resolve_ack_session_seq(77) == 77);
    assert(resolve_ack_session_seq(0) == 0);

    std::cout << "reliability state tests passed\n";
    return 0;
}
