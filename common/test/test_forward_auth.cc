#include "auth/forward_auth.hpp"
#include "auth/auth_context.hpp"
#include "common/auth/metadata.pb.h"
#include <brpc/controller.h>
#include <gtest/gtest.h>

using namespace chatnow::auth;

namespace {
template<typename F>
void set_meta(brpc::Controller& c, F fill) {
    chatnow::rpc::RpcMetadata meta;
    fill(meta);
    std::string data;
    meta.SerializeToString(&data);
    c.request_attachment().append(data);
}

bool meta_equal(const brpc::Controller& a, const brpc::Controller& b) {
    return a.request_attachment() == b.request_attachment();
}
}

TEST(ForwardAuthMetadata, CopiesAllFourFields) {
    brpc::Controller in, out;
    set_meta(in, [](chatnow::rpc::RpcMetadata& m) {
        m.set_trace_id("t");
        m.set_user_id("u");
        m.set_device_id("d");
        m.set_jwt_jti("j");
    });

    forward_auth_metadata(&in, &out);

    EXPECT_TRUE(meta_equal(in, out));
}

TEST(ForwardAuthMetadata, CopiesEmptyAttachment) {
    brpc::Controller in, out;
    forward_auth_metadata(&in, &out);
    EXPECT_TRUE(meta_equal(in, out));
}

TEST(ForwardAuthMetadata, RoundTripViaExtractAuth) {
    brpc::Controller in, out;
    set_meta(in, [](chatnow::rpc::RpcMetadata& m) {
        m.set_user_id("u");
        m.set_device_id("d");
        m.set_trace_id("t");
        m.set_jwt_jti("j");
    });

    forward_auth_metadata(&in, &out);

    AuthContext ctx = extract_auth(&out);
    EXPECT_EQ(ctx.user_id, "u");
    EXPECT_EQ(ctx.device_id, "d");
    EXPECT_EQ(ctx.trace_id, "t");
    EXPECT_EQ(ctx.jwt_jti, "j");
}

TEST(ForwardAuthMetadata, NullSafetyInOrOut) {
    brpc::Controller cntl;
    EXPECT_NO_THROW(forward_auth_metadata(nullptr, &cntl));
    EXPECT_NO_THROW(forward_auth_metadata(&cntl, nullptr));
    EXPECT_NO_THROW(forward_auth_metadata(nullptr, nullptr));
}
