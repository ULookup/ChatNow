#pragma once

/**
 * MQ trace headers 助手
 * ---
 * - 发布侧：从当前 LogContext 取 trace_id 注入 std::map<string,string>
 *   作为 publish 的 headers 入参
 * - 消费侧：从 std::map（rabbitmq.hpp 改造后回调暴露）中读 trace_id；
 *   缺失时返回空串
 *
 * 不直接依赖 AMQP-CPP 类型，便于单测与跨 MQ 实现复用。
 */

#include "log/log_context.hpp"
#include <map>
#include <string>

namespace chatnow::mq {

inline constexpr const char* kTraceHeader = "trace_id";

/* brief: 把当前 LogContext 的 trace_id 注入 headers map（若非空）
 *  使用：发布前调 mq_inject_trace_headers(headers); publisher.publish_confirm(body, headers, cb);
 */
inline void mq_inject_trace_headers(std::map<std::string, std::string>& headers) {
    const auto& trace_id = ::chatnow::log::LogContext::current().trace_id;
    if (!trace_id.empty()) {
        headers[kTraceHeader] = trace_id;
    }
}

/* brief: 从 headers map 中读 trace_id；缺失返回 "" */
inline std::string mq_extract_trace_id(const std::map<std::string, std::string>& headers) {
    auto it = headers.find(kTraceHeader);
    if (it == headers.end()) return "";
    return it->second;
}

}  // namespace chatnow::mq
