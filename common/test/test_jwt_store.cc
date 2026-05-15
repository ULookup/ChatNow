// 需要本地 Redis 127.0.0.1:6379 db=15；CI 容器中已提供。
#include "auth/jwt_store.hpp"

#include <gtest/gtest.h>
#include <sw/redis++/redis++.h>

#include <chrono>
#include <thread>

namespace {
std::shared_ptr<sw::redis::Redis> make_redis() {
    sw::redis::ConnectionOptions opt;
    opt.host = "127.0.0.1";
    opt.port = 6379;
    opt.db = 15;
    auto c = std::make_shared<sw::redis::Redis>(opt);
    c->flushdb();
    return c;
}
}  // namespace

using chatnow::auth::JwtStore;

TEST(JwtStore, RevokeAndCheck) {
    auto r = make_redis();
    JwtStore store(r);
    EXPECT_FALSE(store.is_revoked("jti_x"));
    store.revoke("jti_x", 60);
    EXPECT_TRUE(store.is_revoked("jti_x"));
}

TEST(JwtStore, RevokeTtlExpires) {
    auto r = make_redis();
    JwtStore store(r);
    store.revoke("jti_y", 1);
    EXPECT_TRUE(store.is_revoked("jti_y"));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_FALSE(store.is_revoked("jti_y"));
}

TEST(JwtStore, ActiveRefreshLifecycle) {
    auto r = make_redis();
    JwtStore store(r);
    EXPECT_TRUE(store.get_active_refresh("u1", "d1").empty());
    store.put_active_refresh("u1", "d1", "rt_jti_1", 3600);
    EXPECT_EQ(store.get_active_refresh("u1", "d1"), "rt_jti_1");
    store.clear_active_refresh("u1", "d1");
    EXPECT_TRUE(store.get_active_refresh("u1", "d1").empty());
}

TEST(JwtStore, RotateOk) {
    auto r = make_redis();
    JwtStore store(r);
    store.put_active_refresh("u1", "d1", "old_jti", 3600);
    auto res = store.rotate_refresh_or_detect_reuse(
        "u1", "d1", "old_jti", "new_jti", 3600);
    EXPECT_EQ(res, JwtStore::RotateResult::kOk);
    EXPECT_EQ(store.get_active_refresh("u1", "d1"), "new_jti");
}

TEST(JwtStore, RotateDetectsReuse) {
    auto r = make_redis();
    JwtStore store(r);
    auto res1 = store.rotate_refresh_or_detect_reuse(
        "u1", "d1", "old_jti", "new_jti_1", 3600);
    EXPECT_EQ(res1, JwtStore::RotateResult::kOk);
    auto res2 = store.rotate_refresh_or_detect_reuse(
        "u1", "d1", "old_jti", "new_jti_2", 3600);
    EXPECT_EQ(res2, JwtStore::RotateResult::kReuseDetected);
}
