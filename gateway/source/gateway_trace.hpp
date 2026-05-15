#pragma once

/**
 * gateway_setup_trace
 * ---
 * Gateway 每个 HTTP handler 入口三件套：
 *   1. 从 HTTP 请求读 X-Trace-Id；不合法则现生成 32 字符 hex
 *   2. 写到 brpc Controller 的 HTTP header（key 为 x-trace-id）
 *      传给后端 RPC；后端 extract_auth() 从 metadata 取出来
 *   3. LogContext::set(trace_id, user_id, device_id)
 *      让 Gateway 自身的 LOG_xxx 输出也带 trace_id
 *
 * user_id / device_id 在 P2 JWT 实施前可填空串；P2 之后改为 JWT payload 取值。
 *
 * Handler 退出时调 LogContext::clear()——本助手不接管，handler 用 RAII 守卫。
 */

#include "auth/metadata_keys.hpp"
#include "log/log_context.hpp"
#include "utils/trace_id.hpp"

#include <brpc/controller.h>
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

/* brief: 一行接入：解析 trace_id → 写 cntl metadata → 写 LogContext
 *   返回 trace_id（调用方按需用，例如填回 HTTP response header 给客户端）
 *   user_id/device_id 为空字符串时不写入 LogContext 字段（但仍会被 set 覆盖
 *   为空，因此调用方应保证一次 handler 内只调一次本函数）
 */
inline std::string gateway_setup_trace(const httplib::Request& req,
                                       brpc::Controller& cntl,
                                       const std::string& user_id = "",
                                       const std::string& device_id = "")
{
    std::string trace_id = resolve_trace_id(req);
    cntl.http_request().SetHeader(::chatnow::auth::kMetaTraceId, trace_id);
    if (!user_id.empty()) {
        cntl.http_request().SetHeader(::chatnow::auth::kMetaUserId, user_id);
    }
    if (!device_id.empty()) {
        cntl.http_request().SetHeader(::chatnow::auth::kMetaDeviceId, device_id);
    }
    ::chatnow::log::LogContext::set(trace_id, user_id, device_id);
    return trace_id;
}

/* brief: handler 退出 RAII 守卫；脱离作用域时 clear LogContext */
struct LogContextScope {
    ~LogContextScope() { ::chatnow::log::LogContext::clear(); }
};

}  // namespace chatnow::gateway
