#pragma once

#include <bvar/bvar.h>

namespace chatnow::metrics {

// fail-soft 降级计数器
inline bvar::Adder<long> g_degraded_identity_total;
inline bvar::Adder<long> g_degraded_message_total;
inline bvar::Adder<long> g_degraded_es_write_total;
inline bvar::Adder<long> g_es_retry_total;

}  // namespace chatnow::metrics
