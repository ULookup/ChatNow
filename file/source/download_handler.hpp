#pragma once

/**
 * DownloadHandler —— ApplyDownload + GetFileInfo
 * ---
 *   apply()：签 GET presigned URL（committed 行才放）
 *   info() ：仅返回 FileInfo（不签 URL）
 *
 * 访问控制：P4 v1 不引入跨服务鉴权，仅记录访问者；调用方
 * （Conversation/Message）保证私密资源不暴露给非成员。
 */

#include <memory>
#include <string>

#include "common/error.pb.h"
#include "dao/mysql_media_file.hpp"
#include "error/error_codes.hpp"
#include "error/service_error.hpp"
#include "infra/logger.hpp"
#include "infra/s3_client.hpp"
#include "media/media_service.pb.h"
#include "upload_handler.hpp"  // fill_file_info

namespace chatnow {

class DownloadHandler {
public:
    DownloadHandler(std::shared_ptr<S3Client>       s3,
                    std::shared_ptr<MediaFileTable> files,
                    int                             presign_seconds)
        : _s3(std::move(s3)), _files(std::move(files)), _presign(presign_seconds) {}

    void apply(const std::string& user_id,
               const ::chatnow::media::ApplyDownloadReq& req,
               ::chatnow::media::ApplyDownloadRsp* rsp) {
        auto file = _files->select_by_file_id(req.file_id());
        if (!file) {
            throw ServiceError(::chatnow::error::kMediaFileNotFound, "no such file");
        }
        if (file->status() != MediaFileStatus::COMMITTED) {
            // pending / deleted / quarantined 都不暴露
            throw ServiceError(::chatnow::error::kMediaFileNotFound, "not committed");
        }
        auto url = _s3->presigned_get(file->bucket(), file->object_key(), _presign);
        rsp->set_download_url(url);
        rsp->set_expires_in_sec(_presign);
        fill_file_info(*file, rsp->mutable_file_info());
        LOG_INFO("apply_download user={} file={} size={}",
                 user_id, file->file_id(), file->file_size());
    }

    void info(const std::string& /*user_id*/,
              const ::chatnow::media::GetFileInfoReq& req,
              ::chatnow::media::GetFileInfoRsp* rsp) {
        auto file = _files->select_by_file_id(req.file_id());
        if (!file || file->status() != MediaFileStatus::COMMITTED) {
            throw ServiceError(::chatnow::error::kMediaFileNotFound, "no such file");
        }
        fill_file_info(*file, rsp->mutable_file_info());
    }

private:
    std::shared_ptr<S3Client>       _s3;
    std::shared_ptr<MediaFileTable> _files;
    int                             _presign;
};

}  // namespace chatnow
