#include "auth/forward_auth.hpp"
#include "auth/metadata_keys.hpp"
#include <brpc/controller.h>
#include <gtest/gtest.h>

using namespace chatnow::auth;

namespace {
const std::string* hdr(const brpc::Controller& c, const char* k) {
    return c.http_request().GetHeader(k);
}
std::string read(const brpc::Controller& c, const char* k) {
    auto* v = hdr(c, k);
    return v ? *v : "";
}
}

TEST(ForwardAuthMetadata, CopiesAllFourFields) {
    brpc::Controller in, out;
    in.http_request().SetHeader(kMetaTraceId,  "t");
    in.http_request().SetHeader(kMetaUserId,   "u");
    in.http_request().SetHeader(kMetaDeviceId, "d");
    in.http_request().SetHeader(kMetaJwtJti,   "j");

    forward_auth_metadata(&in, &out);

    EXPECT_EQ(read(out, kMetaTraceId),  "t");
    EXPECT_EQ(read(out, kMetaUserId),   "u");
    EXPECT_EQ(read(out, kMetaDeviceId), "d");
    EXPECT_EQ(read(out, kMetaJwtJti),   "j");
}

TEST(ForwardAuthMetadata, SkipsMissingFields) {
    brpc::Controller in, out;
    in.http_request().SetHeader(kMetaTraceId, "t");

    forward_auth_metadata(&in, &out);

    EXPECT_EQ(read(out, kMetaTraceId),  "t");
    EXPECT_EQ(hdr(out, kMetaUserId),    nullptr);
    EXPECT_EQ(hdr(out, kMetaDeviceId),  nullptr);
    EXPECT_EQ(hdr(out, kMetaJwtJti),    nullptr);
}

TEST(ForwardAuthMetadata, NullSafetyInOrOut) {
    brpc::Controller cntl;
    EXPECT_NO_THROW(forward_auth_metadata(nullptr, &cntl));
    EXPECT_NO_THROW(forward_auth_metadata(&cntl, nullptr));
    EXPECT_NO_THROW(forward_auth_metadata(nullptr, nullptr));
}
