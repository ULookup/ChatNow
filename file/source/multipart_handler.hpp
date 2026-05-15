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

    /* brief: ApplyPartUpload —— 通过 upload_id 反查行 + 签 part presigned URL */
    void apply_part(const std::string& user_id,
                    const ::chatnow::media::ApplyPartReq& req,
                    ::chatnow::media::ApplyPartRsp* rsp) {
        if (req.part_number() < 1 || req.part_number() > 10000) {
            throw ServiceError(::chatnow::error::kSystemInvalidArgument, "part_number out of range");
        }
        auto file = _files->select_by_upload_id(req.upload_id());
        if (!file) {
            throw ServiceError(::chatnow::error::kMediaFileNotFound, "upload not found");
        }
        if (file->owner_id() != user_id) {
            throw ServiceError(::chatnow::error::kMediaFileNotFound, "owner mismatch");
        }
        if (file->status() != MediaFileStatus::PENDING) {
            throw ServiceError(::chatnow::error::kMediaUploadFailed, "bad status");
        }
        auto url = _s3->presigned_part(file->bucket(), file->object_key(),
                                       req.upload_id(), req.part_number(), _presign);
        rsp->set_upload_url(url);
        rsp->set_expires_in_sec(_presign);
    }

    /* brief: CompleteMultipartUpload —— S3 complete + HEAD + ref_count++ + quota++ */
    void complete(const std::string& user_id,
                  const ::chatnow::media::CompleteMultipartReq& req,
                  ::chatnow::media::CompleteMultipartRsp* rsp) {
        auto file = _files->select_by_upload_id(req.upload_id());
        if (!file) {
            throw ServiceError(::chatnow::error::kMediaFileNotFound, "upload not found");
        }
        if (file->owner_id() != user_id) {
            throw ServiceError(::chatnow::error::kMediaFileNotFound, "owner mismatch");
        }
        if (file->status() == MediaFileStatus::COMMITTED) {
            // 幂等
            fill_file_info(*file, rsp->mutable_file_info());
            return;
        }
        if (file->status() != MediaFileStatus::PENDING) {
            throw ServiceError(::chatnow::error::kMediaUploadFailed, "bad status");
        }

        std::vector<S3Client::PartETag> parts;
        parts.reserve(req.parts_size());
        for (const auto& p : req.parts()) {
            S3Client::PartETag pe;
            pe.part_number = p.part_number();
            pe.etag        = p.etag();
            parts.push_back(std::move(pe));
        }
        if (parts.empty()) {
            throw ServiceError(::chatnow::error::kMediaPartNotFound, "no parts to complete");
        }

        _s3->complete_multipart(file->bucket(), file->object_key(),
                                req.upload_id(), parts);

        // HEAD 校验 size
        auto head = _s3->head_object(file->bucket(), file->object_key());
        if (head.content_length != file->file_size()) {
            try { _s3->delete_object(file->bucket(), file->object_key()); }
            catch (...) { LOG_WARN("complete_multipart: delete after size mismatch failed file={}", file->file_id()); }
            _files->update_status(file->file_id(), MediaFileStatus::DELETED);
            throw ServiceError(::chatnow::error::kMediaHashMismatch, "size mismatch");
        }

        // blob_ref + status + quota
        if (!_blobs->select_by_hash(file->content_hash())) {
            MediaBlobRef br;
            br.content_hash(file->content_hash());
            br.bucket(file->bucket());
            br.object_key(file->object_key());
            br.ref_count(0);
            br.total_size(file->file_size());
            br.last_decremented_at(now_ptime());
            _blobs->upsert(br);
        }
        _blobs->inc_ref(file->content_hash());
        _files->update_status(file->file_id(), MediaFileStatus::COMMITTED);
        _quota->inc_used(user_id, file->file_size(), now_ptime());

        file->status(MediaFileStatus::COMMITTED);
        fill_file_info(*file, rsp->mutable_file_info());
        LOG_INFO("complete_multipart user={} file={} parts={} size={}",
                 user_id, file->file_id(), req.parts_size(), file->file_size());
    }

    /* brief: AbortMultipartUpload —— 中止 + 行置 deleted（幂等） */
    void abort(const std::string& user_id,
               const ::chatnow::media::AbortMultipartReq& req,
               ::chatnow::media::AbortMultipartRsp* /*rsp*/) {
        auto file = _files->select_by_upload_id(req.upload_id());
        if (!file) return;  // 幂等
        if (file->owner_id() != user_id) {
            throw ServiceError(::chatnow::error::kMediaFileNotFound, "owner mismatch");
        }
        try { _s3->abort_multipart(file->bucket(), file->object_key(), req.upload_id()); }
        catch (...) { LOG_WARN("abort_multipart: s3 abort failed file={}", file->file_id()); }
        _files->update_status(file->file_id(), MediaFileStatus::DELETED);
        LOG_INFO("abort_multipart user={} file={}", user_id, file->file_id());
    }

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
