#pragma once

/**
 * ===========================================================================
 * 用户媒体配额表 (media_user_quota) —— P4
 * ---------------------------------------------------------------------------
 * 业务定位：
 *   - 默认 5 GB 上限，可在运营侧调高
 *   - ApplyUpload / InitMultipart 时校验：used + new + 其他 pending ≤ quota
 *   - CompleteUpload 时 used += file_size
 *   - cleanup unref_blob_gc 真正删除时 used -= file_size
 *
 * 字段：
 *   _user_id      主键，sender 端 user_id
 *   _used_bytes   已使用字节数
 *   _quota_bytes  配额上限（默认 5 GB）
 *   _updated_at   最近一次变更时间，便于审计 / 监控
 * ===========================================================================
 */

#include <cstdint>
#include <string>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace chatnow {

inline constexpr int64_t kMediaDefaultQuotaBytes = 5LL * 1024 * 1024 * 1024;  // 5 GB

#pragma db object table("media_user_quota")
class MediaUserQuota
{
public:
    MediaUserQuota() = default;

    std::string user_id() const { return _user_id; }
    void user_id(const std::string& v) { _user_id = v; }

    int64_t used_bytes() const { return _used_bytes; }
    void used_bytes(int64_t v) { _used_bytes = v; }

    int64_t quota_bytes() const { return _quota_bytes; }
    void quota_bytes(int64_t v) { _quota_bytes = v; }

    boost::posix_time::ptime updated_at() const { return _updated_at; }
    void updated_at(const boost::posix_time::ptime& v) { _updated_at = v; }

private:
    friend class odb::access;

    #pragma db id type("varchar(32)")
    std::string _user_id;

    #pragma db type("bigint")
    int64_t _used_bytes {0};

    #pragma db type("bigint")
    int64_t _quota_bytes {kMediaDefaultQuotaBytes};

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _updated_at;
};

}  // namespace chatnow
