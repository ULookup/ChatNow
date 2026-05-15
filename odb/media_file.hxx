#pragma once

/**
 * ===========================================================================
 * 媒体文件元数据表 (media_file) —— P4
 * ---------------------------------------------------------------------------
 * 业务定位：
 *   - 每次 ApplyUpload / InitMultipartUpload 都先在此创建一行 pending
 *   - CompleteUpload 校验通过后置 committed；失败/超时置 deleted
 *   - magic_sniff 异步检测异常 → 置 quarantined
 *
 * 字段速览：
 *   _file_id        业务唯一 ID（snowflake 生成的 16 字节 hex，32 char）
 *   _content_hash   "sha256:<64hex>"，与 media_blob_ref 关联
 *   _bucket         所在 MinIO/S3 bucket（public / private 双轨）
 *   _object_key     对象 Key
 *   _file_name      上传时声明的文件名（用户展示用，不强制唯一）
 *   _file_size      文件大小（字节）
 *   _mime_type      上传时声明的 mime（受白名单约束）
 *   _purpose        media::MediaPurpose 枚举
 *   _owner_id       上传者 user_id
 *   _uploaded_at    申请上传时间
 *   _status         0 pending / 1 committed / 2 deleted / 3 quarantined
 *   _upload_id      multipart 时的 S3 upload_id；单段上传为空
 *
 * 索引策略：
 *   uq_file_id              （unique）—— RPC 主路径按 file_id 查
 *   idx_hash                       —— 去重检查
 *   idx_owner_uploaded             —— 用户列表 / 配额扫描
 *   idx_status_uploaded            —— cleanup pending/quarantined 扫表
 *   idx_upload_id                  —— multipart RPC 通过 upload_id 反查
 * ===========================================================================
 */

#include <cstdint>
#include <string>
#include <odb/core.hxx>
#include <odb/nullable.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace chatnow {

// MediaFileStatus 与 proto MediaPurpose 不同：是表内部状态机
enum class MediaFileStatus : unsigned char {
    PENDING     = 0,
    COMMITTED   = 1,
    DELETED     = 2,
    QUARANTINED = 3
};

// MediaPurpose 镜像 proto media::MediaPurpose enum；存数据库时用 tinyint
enum class MediaPurpose : unsigned char {
    UNSPECIFIED  = 0,
    AVATAR       = 1,
    GROUP_AVATAR = 2,
    CHAT         = 3,
    STICKER      = 4
};

#pragma db object table("media_file")
class MediaFile
{
public:
    MediaFile() = default;

    std::string file_id() const { return _file_id; }
    void file_id(const std::string& v) { _file_id = v; }

    std::string content_hash() const { return _content_hash; }
    void content_hash(const std::string& v) { _content_hash = v; }

    std::string bucket() const { return _bucket; }
    void bucket(const std::string& v) { _bucket = v; }

    std::string object_key() const { return _object_key; }
    void object_key(const std::string& v) { _object_key = v; }

    std::string file_name() const { return _file_name; }
    void file_name(const std::string& v) { _file_name = v; }

    int64_t file_size() const { return _file_size; }
    void file_size(int64_t v) { _file_size = v; }

    std::string mime_type() const { return _mime_type; }
    void mime_type(const std::string& v) { _mime_type = v; }

    MediaPurpose purpose() const { return _purpose; }
    void purpose(MediaPurpose v) { _purpose = v; }

    std::string owner_id() const { return _owner_id; }
    void owner_id(const std::string& v) { _owner_id = v; }

    boost::posix_time::ptime uploaded_at() const { return _uploaded_at; }
    void uploaded_at(const boost::posix_time::ptime& v) { _uploaded_at = v; }

    MediaFileStatus status() const { return _status; }
    void status(MediaFileStatus v) { _status = v; }

    std::string upload_id() const { return _upload_id ? *_upload_id : std::string(); }
    void upload_id(const std::string& v) { _upload_id = v; }
    bool has_upload_id() const { return static_cast<bool>(_upload_id); }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("varchar(32)") unique
    std::string _file_id;

    #pragma db type("varchar(72)")
    std::string _content_hash;

    #pragma db type("varchar(64)")
    std::string _bucket;

    #pragma db type("varchar(255)")
    std::string _object_key;

    #pragma db type("varchar(255)")
    std::string _file_name;

    #pragma db type("bigint")
    int64_t _file_size {0};

    #pragma db type("varchar(128)")
    std::string _mime_type;

    #pragma db type("tinyint unsigned")
    MediaPurpose _purpose {MediaPurpose::UNSPECIFIED};

    #pragma db type("varchar(32)")
    std::string _owner_id;

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _uploaded_at;

    #pragma db type("tinyint unsigned")
    MediaFileStatus _status {MediaFileStatus::PENDING};

    #pragma db type("varchar(128)")
    odb::nullable<std::string> _upload_id;

    #pragma db index("idx_hash")            members(_content_hash)
    #pragma db index("idx_owner_uploaded")  members(_owner_id, _uploaded_at)
    #pragma db index("idx_status_uploaded") members(_status, _uploaded_at)
    #pragma db index("idx_upload_id")       members(_upload_id)
};

}  // namespace chatnow
