#pragma once

#include <bvar/bvar.h>
#include "utils/local_cache.hpp"

namespace chatnow::metrics {

// fail-soft 降级计数器
inline bvar::Adder<long> g_degraded_identity_total("degraded_identity_total");
inline bvar::Adder<long> g_degraded_message_total("degraded_message_total");
inline bvar::Adder<long> g_degraded_es_write_total("degraded_es_write_total");
inline bvar::Adder<long> g_es_retry_total("es_retry_total");

// cache observability counters
inline bvar::Adder<long> g_local_cache_hit_total("local_cache_hit_total");
inline bvar::Adder<long> g_local_cache_miss_total("local_cache_miss_total");
inline bvar::Adder<long> g_local_cache_eviction_total("local_cache_eviction_total");
inline bvar::Adder<long> g_local_cache_expired_total("local_cache_expired_total");
inline bvar::Adder<long> g_members_cache_stale_l1_total("members_cache_stale_l1_total");
inline bvar::Adder<long> g_members_cache_snapshot_race_total("members_cache_snapshot_race_total");
inline bvar::Adder<long> g_members_cache_version_conflict_total("members_cache_version_conflict_total");
inline bvar::Adder<long> g_user_info_l1_hit_total("user_info_l1_hit_total");
inline bvar::Adder<long> g_user_info_l2_hit_total("user_info_l2_hit_total");
inline bvar::Adder<long> g_user_info_rpc_total("user_info_rpc_total");
inline bvar::Adder<long> g_user_info_invalidation_failure_total("user_info_invalidation_failure_total");
inline bvar::Adder<long> g_redis_circuit_open_total("redis_circuit_open_total");
inline bvar::Adder<long> g_redis_circuit_rejected_total("redis_circuit_rejected_total");
inline bvar::Adder<long> g_redis_circuit_recovered_total("redis_circuit_recovered_total");
inline bvar::Adder<long> g_redis_call_failure_total("redis_call_failure_total");
inline bvar::Adder<long> g_redis_mutex_retry_total("redis_mutex_retry_total");
inline bvar::Adder<long> g_rate_limit_local_fallback_total("rate_limit_local_fallback_total");
inline bvar::Adder<long> g_rate_limit_local_rejected_total("rate_limit_local_rejected_total");
inline bvar::Adder<long> g_push_unacked_persist_failure_total("push_unacked_persist_failure_total");
inline bvar::Adder<long> g_push_message_requeue_total("push_message_requeue_total");
inline bvar::Adder<long> g_push_auth_revoked_total("push_auth_revoked_total");
inline bvar::Adder<long> g_push_auth_revocation_unavailable_total(
    "push_auth_revocation_unavailable_total");

template <typename V>
inline typename ::chatnow::LocalCache<V>::MetricsSink local_cache_metrics_sink() {
    typename ::chatnow::LocalCache<V>::MetricsSink sink;
    sink.on_hit = []() { g_local_cache_hit_total << 1; };
    sink.on_miss = []() { g_local_cache_miss_total << 1; };
    sink.on_eviction = []() { g_local_cache_eviction_total << 1; };
    sink.on_expired = []() { g_local_cache_expired_total << 1; };
    return sink;
}

}  // namespace chatnow::metrics
