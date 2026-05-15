#pragma once

/**
 * MultipartHandler —— 大文件分片上传业务
 * ---
 *   init()        ：InitMultipartUpload —— 同 ApplyUpload 校验 + 调 s3.init_multipart
 *   apply_part()  ：ApplyPartUpload —— 通过 upload_id 反查 + 签 part presigned URL
 *   complete()    ：CompleteMultipartUpload —— s3.complete + HEAD + ref_count++ + quota++
 *   abort()       ：AbortMultipartUpload —— s3.abort + 行置 deleted（幂等）
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/error.pb.h"
#include "dao/mysql_media_blob_ref.hpp"
#include "dao/mysql_media_file.hpp"
#include "dao/mysql_media_user_quota.hpp"
#include "error/error_codes.hpp"
#include "error/service_error.hpp"
#include "infra/logger.hpp"
#include "infra/s3_client.hpp"
#include "media/media_service.pb.h"
#include "utils/content_hash.hpp"
#include "utils/mime_whitelist.hpp"
#include "utils/object_key.hpp"
#include "upload_handler.hpp"   // 共享 helper：pick_bucket / purpose_prefix / now_* / next_file_id_hex

namespace chatnow {

class MultipartHandler {
public:
    MultipartHandler(std::shared_ptr<S3Client>            s3,
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

    /* brief: InitMultipartUpload —— 校验 + s3.init_multipart + 落 pending 行 */
    void init(const std::string& user_id,
              const ::chatnow::media::InitMultipartReq& req,
              ::chatnow::media::InitMultipartRsp* rsp) {
        if (!content_hash::is_valid(req.content_hash())) {
            throw ServiceError(::chatnow::error::kMediaHashMismatch, "bad content_hash");
        }
        if (!_mime->is_allowed(req.mime_type(), req.file_size())) {
            if (_mime->max_size(req.mime_type()) < 0) {
                throw ServiceError(::chatnow::error::kMediaUnsupportedFormat, "mime not allowed");
            }
            throw ServiceError(::chatnow::error::kMediaFileTooLarge, "file too large for mime");
        }
        auto qrow = _quota->ensure(user_id);
        int64_t pending_others = _files->pending_bytes_of_owner(user_id);
        if (qrow.used_bytes() + req.file_size() + pending_others > qrow.quota_bytes()) {
            throw ServiceError(::chatnow::error::kMediaQuotaExceeded, "user quota exceeded");
        }

        auto bucket = pick_bucket(req.purpose(), _pub_b, _pri_b);
        auto prefix = purpose_prefix(req.purpose());
        auto now_ms = now_epoch_ms();
        std::string key;
        if (req.purpose() == ::chatnow::media::CHAT) {
            key = object_key::build(prefix, req.content_hash(), now_ms);
        } else {
            key = object_key::build_flat(prefix, req.content_hash());
        }

        auto upload_id = _s3->init_multipart(bucket, key, req.mime_type());
        auto file_id   = next_file_id_hex();

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
        r.upload_id(upload_id);
        if (!_files->insert(r)) {
            try { _s3->abort_multipart(bucket, key, upload_id); } catch (...) {}
            throw ServiceError(::chatnow::error::kMediaUploadFailed, "media_file insert");
        }

        rsp->set_file_id(file_id);
        rsp->set_upload_id(upload_id);
        rsp->set_recommended_part_size_bytes(8 * 1024 * 1024);  // 8 MB

        LOG_INFO("init_multipart user={} file={} upload_id={} bucket={} size={}",
                 user_id, file_id, upload_id, bucket, req.file_size());
    }

    // apply_part / complete / abort 在 Task 18 / 19 追加

protected:
    std::shared_ptr<S3Client>            _s3;
    std::shared_ptr<MimeWhitelist>       _mime;
    std::shared_ptr<MediaFileTable>      _files;
    std::shared_ptr<MediaBlobRefTable>   _blobs;
    std::shared_ptr<MediaUserQuotaTable> _quota;
    std::string                          _pub_b, _pri_b;
    int                                  _presign;
};

}  // namespace chatnow
