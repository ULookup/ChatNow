#include "auth/auth_context.hpp"
#include "common/auth/metadata.pb.h"
#include "error/service_error.hpp"
#include "error/error_codes.hpp"
#include <brpc/controller.h>
#include <gtest/gtest.h>

using namespace chatnow::auth;

namespace {
template<typename F>
void fill_cntl(brpc::Controller& cntl, F fill) {
    chatnow::rpc::RpcMetadata meta;
    fill(meta);
    std::string data;
    meta.SerializeToString(&data);
    cntl.request_attachment().append(data);
}
}

TEST(ExtractAuth, AllFieldsPresent) {
    brpc::Controller cntl;
    fill_cntl(cntl, [](chatnow::rpc::RpcMetadata& m) {
        m.set_user_id("u_1");
        m.set_device_id("d_1");
        m.set_trace_id("t_1");
        m.set_jwt_jti("jti_1");
    });
    AuthContext ctx = extract_auth(&cntl);
    EXPECT_EQ(ctx.user_id,   "u_1");
    EXPECT_EQ(ctx.device_id, "d_1");
    EXPECT_EQ(ctx.trace_id,  "t_1");
    EXPECT_EQ(ctx.jwt_jti,   "jti_1");
}

TEST(ExtractAuth, TraceIdOptional) {
    brpc::Controller cntl;
    fill_cntl(cntl, [](chatnow::rpc::RpcMetadata& m) {
        m.set_user_id("u_1");
        m.set_device_id("d_1");
    });
    AuthContext ctx = extract_auth(&cntl);
    EXPECT_EQ(ctx.user_id,  "u_1");
    EXPECT_EQ(ctx.trace_id, "");
}

TEST(ExtractAuth, MissingUserIdThrows) {
    brpc::Controller cntl;
    fill_cntl(cntl, [](chatnow::rpc::RpcMetadata& m) {
        m.set_device_id("d_1");
        m.set_trace_id("t_1");
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
    fill_cntl(cntl, [](chatnow::rpc::RpcMetadata& m) {
        m.set_user_id("u_1");
        m.set_trace_id("t_1");
    });
    EXPECT_THROW(extract_auth(&cntl), chatnow::ServiceError);
}

TEST(ExtractAuth, NullControllerThrows) {
    EXPECT_THROW(extract_auth(nullptr), chatnow::ServiceError);
}

TEST(ExtractAuth, EmptyAttachmentThrows) {
    brpc::Controller cntl;
    EXPECT_THROW(extract_auth(&cntl), chatnow::ServiceError);
}

TEST(ExtractAuth, CorruptedAttachmentThrows) {
    brpc::Controller cntl;
    cntl.request_attachment().append("\x01");
    EXPECT_THROW(extract_auth(&cntl), chatnow::ServiceError);
}
