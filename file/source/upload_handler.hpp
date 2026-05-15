#pragma once

/**
 * UploadHandler —— 单段三步上传业务逻辑
 * ---
 *   apply()    ：ApplyUpload —— mime/size/quota/dedup → 签 PUT presigned URL
 *   complete() ：CompleteUpload —— HEAD 比对 + ref_count++ + quota++
 *
 * 与 MultipartHandler 共享一些 helper（pick_bucket / purpose_prefix / now_ms /
 * snowflake hex）。这些都放在本头文件中以避免新建一个 misc utils。
 */

#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>

#include "auth/auth_context.hpp"
#include "common/error.pb.h"
#include "dao/mysql_media_blob_ref.hpp"
#include "dao/mysql_media_file.hpp"
#include "dao/mysql_media_user_quota.hpp"
#include "error/error_codes.hpp"
#include "error/service_error.hpp"
#include "infra/logger.hpp"
#include "infra/s3_client.hpp"
#include "infra/snowflake.hpp"
#include "media/media_service.pb.h"
#include "utils/content_hash.hpp"
#include "utils/mime_whitelist.hpp"
#include "utils/object_key.hpp"

namespace chatnow {

// ========== Helper：bucket / prefix / time / id ==========

inline std::string pick_bucket(::chatnow::media::MediaPurpose p,
                               const std::string& pub_b, const std::string& pri_b) {
    switch (p) {
        case ::chatnow::media::AVATAR:
        case ::chatnow::media::GROUP_AVATAR:
        case ::chatnow::media::STICKER:
            return pub_b;
        case ::chatnow::media::CHAT:
        default:
            return pri_b;
    }
}

inline std::string purpose_prefix(::chatnow::media::MediaPurpose p) {
    switch (p) {
        case ::chatnow::media::AVATAR:        return "avatar";
        case ::chatnow::media::GROUP_AVATAR:  return "group_avatar";
        case ::chatnow::media::STICKER:       return "sticker";
        case ::chatnow::media::CHAT:
        default:                              return "chat";
    }
}

inline int64_t now_epoch_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

inline boost::posix_time::ptime now_ptime() {
    return boost::posix_time::microsec_clock::universal_time();
}

/* brief: 模块全局 SnowflakeId（worker_id 取自 pid 低 10 bits），
 *        多副本部署需要把 worker_id 做成配置项；P4 v1 单副本可用。 */
inline SnowflakeId& shared_snowflake() {
    static SnowflakeId inst{static_cast<uint64_t>(::getpid()) & SnowflakeId::kMaxWorkerId};
    return inst;
}

inline std::string next_file_id_hex() {
    auto v = shared_snowflake().Next();
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016lx", static_cast<unsigned long>(v));
    return std::string(buf);
}

// ========== UploadHandler ==========

class UploadHandler {
public:
    UploadHandler(std::shared_ptr<S3Client>            s3,
                  std::shared_ptr<MimeWhitelist>       mime,
                  std::shared_ptr<MediaFileTable>      files,
                  std::shared_ptr<MediaBlobRefTable>   blobs,
                  std::shared_ptr<MediaUserQuotaTable> quota,
                  std::string                          public_bucket,
                  std::string                          private_bucket,
                  int                                  presign_seconds)
        : _s3(std::move(s3)),
          _mime(std::move(mime)),
          _files(std::move(files)),
          _blobs(std::move(blobs)),
          _quota(std::move(quota)),
          _pub_b(std::move(public_bucket)),
          _pri_b(std::move(private_bucket)),
          _presign(presign_seconds) {}

    /* brief: ApplyUpload —— 校验 + 去重 + 签 presigned PUT URL */
    void apply(const std::string& user_id,
               const ::chatnow::media::ApplyUploadReq& req,
               ::chatnow::media::ApplyUploadRsp* rsp) {
        // 1) content_hash 形式
        if (!content_hash::is_valid(req.content_hash())) {
            throw ServiceError(::chatnow::error::kMediaHashMismatch, "bad content_hash");
        }
        // 2) mime + size
        if (!_mime->is_allowed(req.mime_type(), req.file_size())) {
            if (_mime->max_size(req.mime_type()) < 0) {
                throw ServiceError(::chatnow::error::kMediaUnsupportedFormat, "mime not allowed");
            }
            throw ServiceError(::chatnow::error::kMediaFileTooLarge, "file too large for mime");
        }
        // 3) 配额（已用 + 本次 + 其他 pending ≤ quota）
        auto qrow = _quota->ensure(user_id);
        int64_t pending_others = _files->pending_bytes_of_owner(user_id);
        if (qrow.used_bytes() + req.file_size() + pending_others > qrow.quota_bytes()) {
            throw ServiceError(::chatnow::error::kMediaQuotaExceeded, "user quota exceeded");
        }
        // 4) 去重命中已 committed blob：直接登记新 file_id 并标 already_exists
        if (auto blob = _blobs->select_by_hash(req.content_hash());
            blob && blob->ref_count() > 0)
        {
            auto file_id = next_file_id_hex();
            MediaFile r;
            r.file_id(file_id);
            r.content_hash(req.content_hash());
            r.bucket(blob->bucket());
            r.object_key(blob->object_key());
            r.file_name(req.file_name());
            r.file_size(req.file_size());
            r.mime_type(req.mime_type());
            r.purpose(static_cast<MediaPurpose>(req.purpose()));
            r.owner_id(user_id);
            r.uploaded_at(now_ptime());
            r.status(MediaFileStatus::PENDING);  // CompleteUpload 时再置 committed
            if (!_files->insert(r)) {
                throw ServiceError(::chatnow::error::kMediaUploadFailed, "media_file insert");
            }
            rsp->set_file_id(file_id);
            rsp->set_already_exists(true);
            rsp->set_expires_in_sec(0);
            LOG_INFO("apply_upload(dedup) user={} file={} hash={} size={}",
                     user_id, file_id, req.content_hash(), req.file_size());
            return;
        }
        // 5) 新建 pending 行 + 签 presigned URL
        auto bucket = pick_bucket(req.purpose(), _pub_b, _pri_b);
        auto prefix = purpose_prefix(req.purpose());
        auto now_ms = now_epoch_ms();
        std::string key;
        if (req.purpose() == ::chatnow::media::CHAT) {
            key = object_key::build(prefix, req.content_hash(), now_ms);
        } else {
            key = object_key::build_flat(prefix, req.content_hash());
        }
        auto file_id = next_file_id_hex();
        MediaFile r;
        r.file_id(file_id);
        r.content_hash(req.content_hash());
        r.bucket(bucket);
        r.object_key(key);
        r.file_name(req.file_name());
        r.file_size(req.file_size());
        r.mime_type(req.mime_type());
        r.purpose(static_cast<MediaPurpose>(req.purpose()));
        r.owner_id(user_id);
        r.uploaded_at(now_ptime());
        r.status(MediaFileStatus::PENDING);
        if (!_files->insert(r)) {
            throw ServiceError(::chatnow::error::kMediaUploadFailed, "media_file insert");
        }

        std::map<std::string, std::string> headers;
        headers["Content-Type"]   = req.mime_type();
        headers["Content-Length"] = std::to_string(req.file_size());

        auto url = _s3->presigned_put(bucket, key, _presign, headers);

        rsp->set_file_id(file_id);
        rsp->set_already_exists(false);
        rsp->set_upload_url(url);
        for (const auto& kv : headers) (*rsp->mutable_headers())[kv.first] = kv.second;
        rsp->set_expires_in_sec(_presign);

        LOG_INFO("apply_upload user={} file={} size={} mime={} bucket={} hash={}",
                 user_id, file_id, req.file_size(), req.mime_type(), bucket, req.content_hash());
    }

    // complete() 在 Task 15 追加

protected:
    static void fill_info(const MediaFile& f, ::chatnow::media::FileInfo* info) {
        info->set_file_id(f.file_id());
        info->set_file_name(f.file_name());
        info->set_file_size(f.file_size());
        info->set_mime_type(f.mime_type());
        // ptime → epoch_ms：从 1970-01-01 起算秒
        auto epoch = boost::posix_time::ptime(boost::gregorian::date(1970, 1, 1));
        auto diff = f.uploaded_at() - epoch;
        info->set_uploaded_at_ms(diff.total_milliseconds());
    }

    std::shared_ptr<S3Client>            _s3;
    std::shared_ptr<MimeWhitelist>       _mime;
    std::shared_ptr<MediaFileTable>      _files;
    std::shared_ptr<MediaBlobRefTable>   _blobs;
    std::shared_ptr<MediaUserQuotaTable> _quota;
    std::string                          _pub_b, _pri_b;
    int                                  _presign;
};

}  // namespace chatnow
