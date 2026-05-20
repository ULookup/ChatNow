#pragma once

/**
 * gateway_setup_trace
 * ---
 * Gateway 每个 HTTP handler 入口三件套：
 *   1. 从 HTTP 请求读 X-Trace-Id；不合法则现生成 32 字符 hex
 *   2. 写到 RpcMetadata（传引用，与 apply_auth_to_brpc 共享同一对象）
 *   3. LogContext::set(trace_id, user_id, device_id)
 *      让 Gateway 自身的 LOG_xxx 输出也带 trace_id
 */

#include "log/log_context.hpp"
#include "utils/trace_id.hpp"
#include "common/auth/metadata.pb.h"

#include "httplib.h"
#include <string>

namespace chatnow::gateway {

/* brief: 解析 X-Trace-Id；不合法或缺失则现生成 */
inline std::string resolve_trace_id(const httplib::Request& req) {
    auto it = req.headers.find("X-Trace-Id");
    if (it != req.headers.end()) {
        if (::chatnow::utils::is_valid_trace_id(it->second)) {
            return it->second;
        }
    }
    return ::chatnow::utils::gen_trace_id();
}

/* brief: 一行接入：解析 trace_id → 填 RpcMetadata → 写 LogContext
 *   返回 trace_id（调用方按需用，例如填回 HTTP response header 给客户端）
 *   user_id/device_id 可空；非空时也填入 meta。
 */
inline std::string gateway_setup_trace(const httplib::Request& req,
                                       ::chatnow::rpc::RpcMetadata& meta,
                                       const std::string& user_id = "",
                                       const std::string& device_id = "")
{
    std::string trace_id = resolve_trace_id(req);
    meta.set_trace_id(trace_id);
    if (!user_id.empty()) {
        meta.set_user_id(user_id);
    }
    if (!device_id.empty()) {
        meta.set_device_id(device_id);
    }
    ::chatnow::log::LogContext::set(trace_id, user_id, device_id);
    return trace_id;
}

/* brief: handler 退出 RAII 守卫；脱离作用域时 clear LogContext */
struct LogContextScope {
    ~LogContextScope() { ::chatnow::log::LogContext::clear(); }
};

}  // namespace chatnow::gateway
