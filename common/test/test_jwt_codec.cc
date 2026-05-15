#include "auth/jwt_codec.hpp"
#include "error/error_codes.hpp"
#include "error/service_error.hpp"

#include <gtest/gtest.h>
#include <thread>

using namespace chatnow::auth;

namespace {
JwtConfig make_cfg() {
    JwtConfig cfg;
    cfg.current_kid = "v1";
    cfg.keys["v1"] = std::string(32, 'A');
    cfg.keys["v2"] = std::string(40, 'B');
    cfg.access_ttl_sec  = 60;
    cfg.refresh_ttl_sec = 600;
    return cfg;
}
}  // namespace

TEST(JwtConfig, ValidateRejectsShortKey) {
    JwtConfig cfg;
    cfg.current_kid = "v1";
    cfg.keys["v1"]  = std::string(31, 'A');
    EXPECT_THROW(cfg.validate_or_throw(), std::runtime_error);
}

TEST(JwtConfig, ValidateRejectsMissingCurrentKid) {
    JwtConfig cfg;
    cfg.current_kid = "v9";
    cfg.keys["v1"]  = std::string(32, 'A');
    EXPECT_THROW(cfg.validate_or_throw(), std::runtime_error);
}

TEST(JwtConfig, ValidateRejectsBadTtl) {
    JwtConfig cfg = make_cfg();
    cfg.access_ttl_sec = 0;
    EXPECT_THROW(cfg.validate_or_throw(), std::runtime_error);
}

TEST(JwtCodec, AccessRoundtrip) {
    JwtCodec codec(make_cfg());
    auto tok = codec.sign_access("u_1", "d_1", "jti_x");
    auto claims = codec.verify(tok);
    EXPECT_EQ(claims.sub, "u_1");
    EXPECT_EQ(claims.did, "d_1");
    EXPECT_EQ(claims.jti, "jti_x");
    EXPECT_EQ(claims.kid, "v1");
    EXPECT_FALSE(claims.is_refresh);
    EXPECT_GT(claims.exp_sec, claims.iat_sec);
}

TEST(JwtCodec, RefreshRoundtrip) {
    JwtCodec codec(make_cfg());
    auto tok = codec.sign_refresh("u_1", "d_1");
    auto claims = codec.verify(tok, /*require_refresh=*/true);
    EXPECT_TRUE(claims.is_refresh);
    EXPECT_EQ(claims.sub, "u_1");
    EXPECT_FALSE(claims.jti.empty());
}

TEST(JwtCodec, VerifyRejectsAccessWhenRefreshRequired) {
    JwtCodec codec(make_cfg());
    auto tok = codec.sign_access("u", "d");
    EXPECT_THROW(codec.verify(tok, /*require_refresh=*/true), chatnow::ServiceError);
}

TEST(JwtCodec, VerifyRejectsRefreshWhenAccessExpected) {
    JwtCodec codec(make_cfg());
    auto tok = codec.sign_refresh("u", "d");
    EXPECT_THROW(codec.verify(tok, /*require_refresh=*/false), chatnow::ServiceError);
}

TEST(JwtCodec, VerifyRejectsTamperedSignature) {
    JwtCodec codec(make_cfg());
    auto tok = codec.sign_access("u", "d");
    tok[tok.size() - 2] = (tok[tok.size() - 2] == 'A') ? 'B' : 'A';
    try {
        codec.verify(tok);
        FAIL() << "should throw";
    } catch (const chatnow::ServiceError& e) {
        EXPECT_EQ(e.code(), chatnow::error::kAuthTokenInvalid);
    }
}

TEST(JwtCodec, VerifyRejectsExpired) {
    JwtConfig cfg = make_cfg();
    cfg.access_ttl_sec = 1;
    JwtCodec codec(cfg);
    auto tok = codec.sign_access("u", "d");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    try {
        codec.verify(tok);
        FAIL() << "should throw";
    } catch (const chatnow::ServiceError& e) {
        EXPECT_EQ(e.code(), chatnow::error::kAuthTokenExpired);
    }
}

TEST(JwtCodec, VerifyRejectsUnknownKid) {
    JwtCodec codec(make_cfg());
    auto tok = codec.sign_access("u", "d");

    JwtConfig cfg2;
    cfg2.current_kid = "v9";
    cfg2.keys["v9"]  = std::string(32, 'C');
    cfg2.access_ttl_sec = 60; cfg2.refresh_ttl_sec = 600;
    JwtCodec codec2(cfg2);
    try {
        codec2.verify(tok);
        FAIL() << "should throw";
    } catch (const chatnow::ServiceError& e) {
        EXPECT_EQ(e.code(), chatnow::error::kAuthTokenInvalid);
    }
}

TEST(JwtCodec, MultiKidVerifyByHeader) {
    auto cfg = make_cfg();   // current_kid=v1
    JwtCodec codec_v1(cfg);
    auto tok_v1 = codec_v1.sign_access("u", "d");
    EXPECT_EQ(codec_v1.verify(tok_v1).kid, "v1");

    cfg.current_kid = "v2";
    JwtCodec codec_v2(cfg);
    auto tok_v2 = codec_v2.sign_access("u", "d");

    // codec_v1 持有 v1+v2，应能验签 v2 token
    auto c = codec_v1.verify(tok_v2);
    EXPECT_EQ(c.kid, "v2");
}

TEST(JwtCodec, RandomJtiUnique) {
    auto a = JwtCodec::random_jti();
    auto b = JwtCodec::random_jti();
    EXPECT_EQ(a.size(), 32u);
    EXPECT_NE(a, b);
}
