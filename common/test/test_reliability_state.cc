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
    assert(legacy.status == IdempotencyStatus::Accepted);
    assert(legacy.message_id == 42);

    auto corrupt = parse_idempotency_state("not-a-state");
    assert(corrupt.status == IdempotencyStatus::Corrupt);

    assert(serialize_idempotency_state({IdempotencyStatus::Pending, 0}) == "pending");
    assert(serialize_idempotency_state({IdempotencyStatus::Accepted, 42}) == "accepted:42");
    assert(serialize_idempotency_state({IdempotencyStatus::Persisted, 99}) == "persisted:99");
    assert(idempotency_key_for("u1", "c1") == "im:msg:idem:u1:c1");

    assert(!should_remove_cross_outbox(false, true));
    assert(!should_remove_cross_outbox(true, false));
    assert(should_remove_cross_outbox(true, true));

    std::cout << "reliability state tests passed\n";
    return 0;
}
