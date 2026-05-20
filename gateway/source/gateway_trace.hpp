#pragma once

/**
 * gateway_setup_trace
 * ---
 * Gateway 每个 HTTP handler 入口两件事：
 *   1. 从 HTTP 请求读 X-Trace-Id；不合法则现生成 32 字符 hex
 *   2. 写到 RpcMetadata + LogContext（trace_id 写入，身份字段留空，
 *      由 apply_auth_to_brpc 后续补填）
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
 */
inline std::string gateway_setup_trace(const httplib::Request& req,
                                       ::chatnow::rpc::RpcMetadata& meta)
{
    std::string trace_id = resolve_trace_id(req);
    meta.set_trace_id(trace_id);
    ::chatnow::log::LogContext::set(trace_id, "", "");
    return trace_id;
}

/* brief: handler 退出 RAII 守卫；脱离作用域时 clear LogContext */
struct LogContextScope {
    ~LogContextScope() { ::chatnow::log::LogContext::clear(); }
};

}  // namespace chatnow::gateway
