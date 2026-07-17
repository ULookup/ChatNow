#pragma once

/**
 * forward_auth_metadata(in, out)
 * ---
 * 服务间内部 RPC 调用前调用此函数：把入站 Controller 的 request_attachment
 * 原样复制到出站 Controller。
 *
 * 用于场景：A 服务的 RPC handler 中需要调 B 服务的 RPC，B 服务的 handler
 * 需要知道"原始客户端身份"。透传后 B 服务的 extract_auth(out_cntl)
 * 就能拿到与 A handler 相同的 user_id / device_id。
 */

#include <brpc/controller.h>

namespace chatnow::auth {

inline void forward_auth_metadata(brpc::Controller* in, brpc::Controller* out) {
    if (!in || !out) return;
    out->request_attachment() = in->request_attachment();
}

}  // namespace chatnow::auth
