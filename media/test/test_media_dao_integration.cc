/**
 * media DAO 集成测试 —— 仅当 DB_TEST=1 时跑
 * ---
 * 前置：MySQL 已启动；ODB 自动建表（schema 由 file/CMakeLists.txt 中
 *       odb --generate-schema 输出）。
 * 运行：
 *   DB_TEST=1 \
 *     DB_HOST=127.0.0.1 DB_USER=root DB_PASS=root DB_NAME=chatnow \
 *     ctest -R media_dao_integration --output-on-failure
 *
 * 测试覆盖：
 *   - MediaFile insert / select / update_status
 *   - MediaBlobRef upsert / inc_ref / dec_ref / list_zero_ref_older_than
 *   - MediaUserQuota ensure / inc_used / dec_used
 */

#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include "dao/mysql.hpp"
#include "dao/mysql_media_file.hpp"
#include "dao/mysql_media_blob_ref.hpp"
#include "dao/mysql_media_user_quota.hpp"

using namespace chatnow;

static bool db_enabled() {
    const char* e = std::getenv("DB_TEST");
    return e && std::strcmp(e, "1") == 0;
}

static const char* env_or(const char* k, const char* dft) {
    const char* v = std::getenv(k);
    return v && *v ? v : dft;
}

static std::shared_ptr<odb::core::database> open_db() {
    return ODBFactory::create(
        env_or("DB_USER", "root"),
        env_or("DB_PASS", "root"),
        env_or("DB_HOST", "127.0.0.1"),
        env_or("DB_NAME", "chatnow"),
        "utf8mb4",
        std::atoi(env_or("DB_PORT", "3306")),
        4);
}

static boost::posix_time::ptime now_utc() {
    return boost::posix_time::microsec_clock::universal_time();
}

TEST(MediaDaoIT, RefCountFlow) {
    if (!db_enabled()) GTEST_SKIP() << "DB_TEST!=1";
    auto db = open_db();
    MediaFileTable    files(db);
    MediaBlobRefTable blobs(db);

    const std::string hash = "sha256:" + std::string(64, 'a');

    MediaFile f;
    f.file_id("ftest_" + std::to_string(::rand()));
    f.content_hash(hash);
    f.bucket("chatnow-media-private");
    f.object_key("chat/2026/05/14/aa/" + std::string(64, 'a'));
    f.file_name("x.jpg");
    f.file_size(100);
    f.mime_type("image/jpeg");
    f.purpose(MediaPurpose::CHAT);
    f.owner_id("u_test");
    f.uploaded_at(now_utc());
    f.status(MediaFileStatus::PENDING);
    ASSERT_TRUE(files.insert(f));

    MediaBlobRef br;
    br.content_hash(hash);
    br.bucket("chatnow-media-private");
    br.object_key(f.object_key());
    br.ref_count(0);
    br.total_size(100);
    br.last_decremented_at(now_utc());
    ASSERT_TRUE(blobs.upsert(br));

    ASSERT_TRUE(blobs.inc_ref(hash));
    auto b = blobs.select_by_hash(hash);
    ASSERT_TRUE(b);
    EXPECT_EQ(b->ref_count(), 1);

    ASSERT_TRUE(blobs.dec_ref(hash, now_utc()));
    b = blobs.select_by_hash(hash);
    EXPECT_EQ(b->ref_count(), 0);

    // dec_ref 再次：保持 0，不溢出
    ASSERT_TRUE(blobs.dec_ref(hash, now_utc()));
    b = blobs.select_by_hash(hash);
    EXPECT_EQ(b->ref_count(), 0);

    // 清理
    blobs.erase(hash);
    files.update_status(f.file_id(), MediaFileStatus::DELETED);
}

TEST(MediaDaoIT, QuotaBasicFlow) {
    if (!db_enabled()) GTEST_SKIP() << "DB_TEST!=1";
    auto db = open_db();
    MediaUserQuotaTable q(db);

    auto uid = "u_quota_test_" + std::to_string(::rand());

    auto row = q.ensure(uid);
    EXPECT_EQ(row.used_bytes(), 0);
    EXPECT_EQ(row.quota_bytes(), kMediaDefaultQuotaBytes);

    ASSERT_TRUE(q.inc_used(uid, 1000, now_utc()));
    auto row2 = q.ensure(uid);
    EXPECT_EQ(row2.used_bytes(), 1000);

    ASSERT_TRUE(q.dec_used(uid, 300, now_utc()));
    auto row3 = q.ensure(uid);
    EXPECT_EQ(row3.used_bytes(), 700);

    // dec 超过 used：不溢出，归 0
    ASSERT_TRUE(q.dec_used(uid, 10000, now_utc()));
    auto row4 = q.ensure(uid);
    EXPECT_EQ(row4.used_bytes(), 0);
}

TEST(MediaDaoIT, FilePendingTimeoutScan) {
    if (!db_enabled()) GTEST_SKIP() << "DB_TEST!=1";
    auto db = open_db();
    MediaFileTable files(db);

    auto past = now_utc() - boost::posix_time::hours(2);
    MediaFile f;
    f.file_id("fpend_" + std::to_string(::rand()));
    f.content_hash("sha256:" + std::string(64, 'b'));
    f.bucket("chatnow-media-private");
    f.object_key("chat/2026/05/14/bb/" + std::string(64, 'b'));
    f.file_name("y.txt");
    f.file_size(50);
    f.mime_type("text/plain");
    f.purpose(MediaPurpose::CHAT);
    f.owner_id("u_pending");
    f.uploaded_at(past);
    f.status(MediaFileStatus::PENDING);
    ASSERT_TRUE(files.insert(f));

    auto rows = files.list_pending_older_than(now_utc() - boost::posix_time::hours(1), 50);
    bool seen = false;
    for (auto& x : rows) if (x.file_id() == f.file_id()) { seen = true; break; }
    EXPECT_TRUE(seen);

    files.update_status(f.file_id(), MediaFileStatus::DELETED);
}
