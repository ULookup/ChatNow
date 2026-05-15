#pragma once

/**
 * S3Client —— aws-sdk-cpp S3 module 的轻包装
 * ---
 * 仅暴露 P4 媒体子系统需要的子集：
 *   - presigned_put / presigned_get：签发预签名 URL，客户端直传 MinIO，
 *     服务端不过 bytes（除 SpeechRecognition 外）
 *   - head_object / delete_object：CompleteUpload size 比对 / 失败回滚
 *   - get_range：cleanup magic_sniff 任务 Range GET 前 512B
 *   - init_multipart / presigned_part / complete_multipart / abort_multipart
 *     / list_multipart_uploads：multipart 上传 + 孤儿清理
 *
 * 错误处理：所有 AWS 失败统一映射成 ServiceError(kMediaUploadFailed)，
 * 不向调用方泄漏 AWS 异常类型；具体错误进 LOG_ERROR。
 *
 * 预期生命周期：MediaServiceImpl 持一份 shared_ptr<S3Client>；
 * Aws::InitAPI / Aws::ShutdownAPI 在 main 中调用，本类不管。
 */

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListMultipartUploadsRequest.h>

#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "error/error_codes.hpp"
#include "error/service_error.hpp"
#include "infra/logger.hpp"

namespace chatnow {

struct S3Options {
    std::string endpoint;
    std::string region {"us-east-1"};
    std::string access_key;
    std::string secret_key;
    bool use_path_style {true};   // MinIO 必须 true
};

class S3Client {
public:
    explicit S3Client(const S3Options& o) : _opt(o) {
        Aws::Client::ClientConfiguration cfg;
        cfg.endpointOverride = o.endpoint;
        cfg.scheme = (o.endpoint.rfind("https", 0) == 0)
                   ? Aws::Http::Scheme::HTTPS : Aws::Http::Scheme::HTTP;
        cfg.region = o.region;
        cfg.verifySSL = false;
        _client = std::make_shared<Aws::S3::S3Client>(
            Aws::Auth::AWSCredentials(o.access_key, o.secret_key),
            cfg,
            Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
            o.use_path_style);
    }

    /* brief: 签发 PUT presigned URL，客户端按 headers 直传 */
    std::string presigned_put(const std::string& bucket, const std::string& key,
                              int seconds,
                              const std::map<std::string, std::string>& headers) const {
        Aws::Http::HeaderValueCollection h;
        for (const auto& kv : headers) h.emplace(kv.first, kv.second);
        auto url = _client->GeneratePresignedUrlWithSSEC(
            bucket, key, Aws::Http::HttpMethod::HTTP_PUT, h, /*sseKey*/"", seconds);
        if (url.empty()) throw_failed("presigned_put empty url");
        return url;
    }

    /* brief: 签发 GET presigned URL（公共/私密资源都用） */
    std::string presigned_get(const std::string& bucket, const std::string& key, int seconds) const {
        auto url = _client->GeneratePresignedUrl(
            bucket, key, Aws::Http::HttpMethod::HTTP_GET, seconds);
        if (url.empty()) throw_failed("presigned_get empty url");
        return url;
    }

    struct HeadResult {
        int64_t content_length;
        std::string etag;          // 已剥两端引号
        std::string content_type;
    };

    /* brief: HEAD 对象，CompleteUpload 用于 size 比对 */
    HeadResult head_object(const std::string& bucket, const std::string& key) const {
        Aws::S3::Model::HeadObjectRequest req;
        req.SetBucket(bucket);
        req.SetKey(key);
        auto out = _client->HeadObject(req);
        if (!out.IsSuccess()) {
            throw_failed("head_object: " + out.GetError().GetMessage());
        }
        const auto& r = out.GetResult();
        std::string etag = r.GetETag();
        if (etag.size() >= 2 && etag.front() == '"' && etag.back() == '"') {
            etag = etag.substr(1, etag.size() - 2);
        }
        return HeadResult{ r.GetContentLength(), etag, r.GetContentType() };
    }

    /* brief: 删除对象（pending 超时 / size 不匹配 / cleanup 路径） */
    void delete_object(const std::string& bucket, const std::string& key) const {
        Aws::S3::Model::DeleteObjectRequest req;
        req.SetBucket(bucket);
        req.SetKey(key);
        auto out = _client->DeleteObject(req);
        if (!out.IsSuccess()) {
            throw_failed("delete_object: " + out.GetError().GetMessage());
        }
    }

    /* brief: Range GET 前 N 字节（cleanup magic_sniff 用） */
    std::string get_range(const std::string& bucket, const std::string& key,
                          int64_t bytes_to_read) const {
        Aws::S3::Model::GetObjectRequest req;
        req.SetBucket(bucket);
        req.SetKey(key);
        req.SetRange("bytes=0-" + std::to_string(bytes_to_read - 1));
        auto out = _client->GetObject(req);
        if (!out.IsSuccess()) {
            throw_failed("get_range: " + out.GetError().GetMessage());
        }
        auto& body = out.GetResultWithOwnership().GetBody();
        std::string data((std::istreambuf_iterator<char>(body)),
                          std::istreambuf_iterator<char>());
        return data;
    }

    // ===== multipart =====

    /* brief: 初始化 multipart upload，返回 upload_id */
    std::string init_multipart(const std::string& bucket, const std::string& key,
                               const std::string& content_type) const {
        Aws::S3::Model::CreateMultipartUploadRequest req;
        req.SetBucket(bucket);
        req.SetKey(key);
        req.SetContentType(content_type);
        auto out = _client->CreateMultipartUpload(req);
        if (!out.IsSuccess()) {
            throw_failed("init_multipart: " + out.GetError().GetMessage());
        }
        return out.GetResult().GetUploadId();
    }

    /* brief: 签发某个 part 的 PUT presigned URL */
    std::string presigned_part(const std::string& bucket, const std::string& key,
                               const std::string& upload_id, int part_number,
                               int seconds) const {
        Aws::Http::QueryStringParameterCollection q;
        q.emplace("partNumber", std::to_string(part_number));
        q.emplace("uploadId", upload_id);
        auto url = _client->GeneratePresignedUrl(
            bucket, key, Aws::Http::HttpMethod::HTTP_PUT, q, seconds);
        if (url.empty()) throw_failed("presigned_part empty url");
        return url;
    }

    struct PartETag {
        int part_number;
        std::string etag;
    };

    /* brief: 完成 multipart，提交 parts 列表（part_number 升序） */
    void complete_multipart(const std::string& bucket, const std::string& key,
                            const std::string& upload_id,
                            const std::vector<PartETag>& parts) const {
        Aws::S3::Model::CompletedMultipartUpload c;
        for (const auto& p : parts) {
            Aws::S3::Model::CompletedPart cp;
            cp.SetPartNumber(p.part_number);
            cp.SetETag(p.etag);
            c.AddParts(cp);
        }
        Aws::S3::Model::CompleteMultipartUploadRequest req;
        req.SetBucket(bucket);
        req.SetKey(key);
        req.SetUploadId(upload_id);
        req.SetMultipartUpload(c);
        auto out = _client->CompleteMultipartUpload(req);
        if (!out.IsSuccess()) {
            throw_failed("complete_multipart: " + out.GetError().GetMessage());
        }
    }

    /* brief: 中止 multipart（cleanup 孤儿 / 客户端主动放弃） */
    void abort_multipart(const std::string& bucket, const std::string& key,
                         const std::string& upload_id) const {
        Aws::S3::Model::AbortMultipartUploadRequest req;
        req.SetBucket(bucket);
        req.SetKey(key);
        req.SetUploadId(upload_id);
        auto out = _client->AbortMultipartUpload(req);
        if (!out.IsSuccess()) {
            throw_failed("abort_multipart: " + out.GetError().GetMessage());
        }
    }

    struct OrphanUpload {
        std::string key;
        std::string upload_id;
        int64_t initiated_ms;
    };

    /* brief: 列出 bucket 内所有进行中的 multipart upload（cleanup 孤儿用） */
    std::vector<OrphanUpload> list_multipart_uploads(const std::string& bucket) const {
        Aws::S3::Model::ListMultipartUploadsRequest req;
        req.SetBucket(bucket);
        auto out = _client->ListMultipartUploads(req);
        if (!out.IsSuccess()) {
            throw_failed("list_multipart_uploads: " + out.GetError().GetMessage());
        }
        std::vector<OrphanUpload> v;
        for (const auto& u : out.GetResult().GetUploads()) {
            v.push_back(OrphanUpload{
                u.GetKey(),
                u.GetUploadId(),
                u.GetInitiated().Millis()
            });
        }
        return v;
    }

protected:
    [[noreturn]] static void throw_failed(const std::string& m) {
        LOG_ERROR("s3 error: {}", m);
        throw ServiceError(::chatnow::error::kMediaUploadFailed, "media storage error");
    }

    S3Options _opt;
    std::shared_ptr<Aws::S3::S3Client> _client;
};

}  // namespace chatnow
