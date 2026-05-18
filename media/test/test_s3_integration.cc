/**
 * MinIO 集成测试 —— 仅当环境变量 MINIO_TEST=1 时才跑
 * ---
 * 前置：docker compose up -d minio minio-init
 *       （bucket chatnow-media-public / chatnow-media-private 已创建）
 *
 * 运行：
 *   MINIO_TEST=1 ctest -R s3_integration --output-on-failure
 *
 * 测试覆盖：
 *   - presigned PUT + HEAD + Range GET + DELETE
 *   - init/abort multipart
 *   - list_multipart_uploads
 */

#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <string>

#include <curl/curl.h>
#include <aws/core/Aws.h>
#include "infra/s3_client.hpp"

using namespace chatnow;

static bool minio_enabled() {
    const char* e = std::getenv("MINIO_TEST");
    return e && std::strcmp(e, "1") == 0;
}

static S3Options test_opts() {
    return S3Options{
        /*endpoint*/   "http://127.0.0.1:9000",
        /*region*/     "us-east-1",
        /*access_key*/ "minioadmin",
        /*secret_key*/ "minioadmin",
        /*path_style*/ true
    };
}

static int curl_put(const std::string& url, const std::string& body, const std::string& mime) {
    CURL* h = curl_easy_init();
    EXPECT_NE(h, nullptr);
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    struct curl_slist* hl = nullptr;
    std::string ct = "Content-Type: " + mime;
    hl = curl_slist_append(hl, ct.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hl);
    auto rc = curl_easy_perform(h);
    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(h);
    curl_slist_free_all(hl);
    return rc == CURLE_OK ? static_cast<int>(code) : -1;
}

class S3IntegrationFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        Aws::SDKOptions opt;
        Aws::InitAPI(opt);
        _sdk = opt;
    }
    static void TearDownTestSuite() {
        Aws::ShutdownAPI(_sdk);
    }
    inline static Aws::SDKOptions _sdk{};
};

TEST_F(S3IntegrationFixture, PutGetRoundtrip) {
    if (!minio_enabled()) GTEST_SKIP() << "MINIO_TEST!=1";

    S3Client c(test_opts());
    const std::string bucket = "chatnow-media-private";
    const std::string key    = "test/k1";
    auto put = c.presigned_put(bucket, key, 60, {{"Content-Type", "text/plain"}});
    ASSERT_FALSE(put.empty());

    EXPECT_EQ(curl_put(put, "hello", "text/plain"), 200);

    auto head = c.head_object(bucket, key);
    EXPECT_EQ(head.content_length, 5);
    EXPECT_FALSE(head.etag.empty());

    auto data = c.get_range(bucket, key, 5);
    EXPECT_EQ(data, "hello");

    c.delete_object(bucket, key);
}

TEST_F(S3IntegrationFixture, MultipartInitAbort) {
    if (!minio_enabled()) GTEST_SKIP() << "MINIO_TEST!=1";

    S3Client c(test_opts());
    const std::string bucket = "chatnow-media-private";
    const std::string key    = "test/multipart_abort";

    auto upload_id = c.init_multipart(bucket, key, "application/octet-stream");
    ASSERT_FALSE(upload_id.empty());

    auto part_url = c.presigned_part(bucket, key, upload_id, 1, 60);
    ASSERT_FALSE(part_url.empty());

    auto ups = c.list_multipart_uploads(bucket);
    bool seen = false;
    for (const auto& u : ups) {
        if (u.upload_id == upload_id) { seen = true; break; }
    }
    EXPECT_TRUE(seen);

    c.abort_multipart(bucket, key, upload_id);
}
