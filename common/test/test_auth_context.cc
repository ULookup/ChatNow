#include "auth/auth_context.hpp"
#include "auth/metadata_keys.hpp"
#include "error/service_error.hpp"
#include "error/error_codes.hpp"
#include <brpc/controller.h>
#include <gtest/gtest.h>

using namespace chatnow::auth;

namespace {
// brpc::Controller 不可拷贝/移动，因此用 in-place 填充而非返回值
void fill_cntl(brpc::Controller& cntl,
               std::initializer_list<std::pair<std::string, std::string>> headers) {
    for (const auto& kv : headers) {
        cntl.http_request().SetHeader(kv.first, kv.second);
    }
}
}

TEST(ExtractAuth, AllFieldsPresent) {
    brpc::Controller cntl;
    fill_cntl(cntl, {
        {kMetaUserId,   "u_1"},
        {kMetaDeviceId, "d_1"},
        {kMetaTraceId,  "t_1"},
        {kMetaJwtJti,   "jti_1"},
    });
    AuthContext ctx = extract_auth(&cntl);
    EXPECT_EQ(ctx.user_id,   "u_1");
    EXPECT_EQ(ctx.device_id, "d_1");
    EXPECT_EQ(ctx.trace_id,  "t_1");
    EXPECT_EQ(ctx.jwt_jti,   "jti_1");
}

TEST(ExtractAuth, TraceIdOptional) {
    brpc::Controller cntl;
    fill_cntl(cntl, {
        {kMetaUserId,   "u_1"},
        {kMetaDeviceId, "d_1"},
    });
    AuthContext ctx = extract_auth(&cntl);
    EXPECT_EQ(ctx.user_id,  "u_1");
    EXPECT_EQ(ctx.trace_id, "");
}

TEST(ExtractAuth, MissingUserIdThrows) {
    brpc::Controller cntl;
    fill_cntl(cntl, {
        {kMetaDeviceId, "d_1"},
        {kMetaTraceId,  "t_1"},
    });
    try {
        extract_auth(&cntl);
        FAIL() << "expected throw";
    } catch (const chatnow::ServiceError& e) {
        EXPECT_EQ(e.code(), chatnow::error::kSystemInternalError);
    }
}

TEST(ExtractAuth, MissingDeviceIdThrows) {
    brpc::Controller cntl;
    fill_cntl(cntl, {
        {kMetaUserId,  "u_1"},
        {kMetaTraceId, "t_1"},
    });
    EXPECT_THROW(extract_auth(&cntl), chatnow::ServiceError);
}

TEST(ExtractAuth, NullControllerThrows) {
    EXPECT_THROW(extract_auth(nullptr), chatnow::ServiceError);
}
