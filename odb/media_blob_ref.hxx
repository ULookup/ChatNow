#pragma once

/**
 * ===========================================================================
 * 物理 blob 引用计数表 (media_blob_ref) —— P4
 * ---------------------------------------------------------------------------
 * 业务定位：
 *   - 同 content_hash 多 file_id 共享同一份 MinIO 对象
 *   - ref_count 归零 + GC 缓冲（默认 7 天）后真正删 MinIO 对象
 *   - last_decremented_at 用于"延后 GC"，避免短暂归零的 race（用户秒删秒传）
 *
 * 字段：
 *   _content_hash         主键（"sha256:<64hex>"）
 *   _bucket / _object_key 物理位置
 *   _ref_count            引用计数；CompleteUpload 时 +1，文件 deleted 时 -1
 *   _total_size           物理对象大小（字节），用于 GC 时统计释放量
 *   _last_decremented_at  最近一次 -1 的时刻，>=7 天前才允许真正 GC
 *
 * 索引：
 *   idx_refcount_decremented  cleanup unref_blob_gc 扫表
 * ===========================================================================
 */

#include <cstdint>
#include <string>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace chatnow {

#pragma db object table("media_blob_ref")
class MediaBlobRef
{
public:
    MediaBlobRef() = default;

    std::string content_hash() const { return _content_hash; }
    void content_hash(const std::string& v) { _content_hash = v; }

    std::string bucket() const { return _bucket; }
    void bucket(const std::string& v) { _bucket = v; }

    std::string object_key() const { return _object_key; }
    void object_key(const std::string& v) { _object_key = v; }

    int32_t ref_count() const { return _ref_count; }
    void ref_count(int32_t v) { _ref_count = v; }

    int64_t total_size() const { return _total_size; }
    void total_size(int64_t v) { _total_size = v; }

    boost::posix_time::ptime last_decremented_at() const { return _last_decremented_at; }
    void last_decremented_at(const boost::posix_time::ptime& v) { _last_decremented_at = v; }

private:
    friend class odb::access;

    #pragma db id type("varchar(72)")
    std::string _content_hash;

    #pragma db type("varchar(64)")
    std::string _bucket;

    #pragma db type("varchar(255)")
    std::string _object_key;

    #pragma db type("int")
    int32_t _ref_count {0};

    #pragma db type("bigint")
    int64_t _total_size {0};

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _last_decremented_at;

    #pragma db index("idx_refcount_decremented") members(_ref_count, _last_decremented_at)
};

}  // namespace chatnow
