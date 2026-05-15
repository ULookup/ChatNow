#pragma once

/**
 * HANDLE_RPC 宏 —— RPC handler 统一脚手架
 * ---
 * 用法：
 *   void MyServiceImpl::DoX(google::protobuf::RpcController* base_cntl,
 *                           const DoXReq* req, DoXRsp* rsp,
 *                           google::protobuf::Closure* done)
 *   {
 *       brpc::ClosureGuard done_guard(done);
 *       auto* cntl = static_cast<brpc::Controller*>(base_cntl);
 *       HANDLE_RPC(cntl, req, rsp, {
 *           // 业务代码：可使用 auth.user_id / auth.device_id / auth.trace_id
 *           // 失败 throw ServiceError(code, msg)；不要手填 rsp->header()
 *           if (req->name().empty()) {
 *               throw chatnow::ServiceError(9004, "name required");
 *           }
 *           rsp->set_data("hello");
 *       });
 *   }
 *
 * 行为：
 *   1. 入口：extract_auth(cntl) → 缺字段抛 ServiceError(9001)
 *   2. LogContext::set 写入 thread_local
 *   3. body 执行：
 *      - 成功 → header.success=true / error_code=OK / request_id 回填
 *      - throw ServiceError → header.success=false / error_code=e.code() /
 *        error_message=e.message() / WARN 日志
 *      - throw 其他 std::exception → header.error_code=9001 /
 *        error_message="internal error"（不泄漏 what()）/ ERROR 日志
 *   4. LogContext::clear（finally 语义，无论成功/失败/异常都执行）
 *
 * 注意：
 *   - 宏内访问 req->request_id()，要求 req 类型必须有 request_id 字段
 *     （所有 ChatNow 业务 Req 都有此字段）
 *   - 宏内访问 rsp->mutable_header()，要求 rsp 类型必须有 header 字段
 *     （所有 ChatNow 业务 Rsp 都有此字段）
 *   - 不接管 done_guard：调用方仍需 brpc::ClosureGuard done_guard(done)
 */

#include "auth/auth_context.hpp"
#include "error/service_error.hpp"
#include "log/log_context.hpp"
#include "infra/logger.hpp"

#include <exception>

namespace chatnow::detail {

/* finally 守卫：作用域结束时调用 LogContext::clear，异常时也保证执行 */
struct LogContextGuard {
    ~LogContextGuard() { ::chatnow::log::LogContext::clear(); }
};

}  // namespace chatnow::detail

#define HANDLE_RPC(cntl_ptr, req_ptr, rsp_ptr, body)                        \
    do {                                                                    \
        ::chatnow::detail::LogContextGuard _ctx_guard;                      \
        try {                                                               \
            ::chatnow::auth::AuthContext auth =                             \
                ::chatnow::auth::extract_auth(cntl_ptr);                    \
            ::chatnow::log::LogContext::set(                                \
                auth.trace_id, auth.user_id, auth.device_id);               \
            try {                                                           \
                body                                                        \
                (rsp_ptr)->mutable_header()->set_success(true);             \
                (rsp_ptr)->mutable_header()->set_error_code(0); /*OK*/      \
                (rsp_ptr)->mutable_header()->set_request_id(                \
                    (req_ptr)->request_id());                               \
            } catch (const ::chatnow::ServiceError& e) {                    \
                (rsp_ptr)->mutable_header()->set_success(false);            \
                (rsp_ptr)->mutable_header()->set_error_code(e.code());      \
                (rsp_ptr)->mutable_header()->set_error_message(e.message());\
                (rsp_ptr)->mutable_header()->set_request_id(                \
                    (req_ptr)->request_id());                               \
                LOG_WARN("rpc_failed code={} msg={}", e.code(),             \
                         e.message().c_str());                              \
            } catch (const std::exception& e) {                             \
                (rsp_ptr)->mutable_header()->set_success(false);            \
                (rsp_ptr)->mutable_header()->set_error_code(9001);          \
                (rsp_ptr)->mutable_header()->set_error_message(             \
                    "internal error");                                      \
                (rsp_ptr)->mutable_header()->set_request_id(                \
                    (req_ptr)->request_id());                               \
                LOG_ERROR("rpc_exception what={}", e.what());               \
            }                                                               \
        } catch (const ::chatnow::ServiceError& e) {                        \
            /* extract_auth 抛出（metadata 缺失） */                        \
            (rsp_ptr)->mutable_header()->set_success(false);                \
            (rsp_ptr)->mutable_header()->set_error_code(e.code());          \
            (rsp_ptr)->mutable_header()->set_error_message(e.message());    \
            (rsp_ptr)->mutable_header()->set_request_id(                    \
                (req_ptr)->request_id());                                   \
            LOG_ERROR("rpc_auth_missing code={} msg={}", e.code(),          \
                      e.message().c_str());                                 \
        }                                                                   \
    } while (0)
