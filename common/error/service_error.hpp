#pragma once

/**
 * ServiceError —— 业务异常基类
 * ---
 * 业务代码不直接修改 ResponseHeader，而是 throw ServiceError(code, msg)。
 * 由 HANDLE_RPC 宏统一捕获并填充响应。
 *
 * 设计：
 *   - 拷贝 message（非持有 const char*），避免 throw 后字符串失效
 *   - code() 返回原始 int32_t；调用方按需 cast 到 ErrorCode enum
 *     （不依赖 error.pb.h，避免基础设施 header 被 protobuf 污染）
 */

#include <exception>
#include <string>
#include <utility>

namespace chatnow {

class ServiceError : public std::exception {
public:
    ServiceError(int32_t code, std::string message)
        : _code(code), _message(std::move(message)) {}

    int32_t code() const noexcept { return _code; }
    const std::string& message() const noexcept { return _message; }
    const char* what() const noexcept override { return _message.c_str(); }

private:
    int32_t _code;
    std::string _message;
};

}  // namespace chatnow
