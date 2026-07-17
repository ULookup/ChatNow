#pragma once

/**
 * MediaFileTable —— media_file 表 DAO 包装
 * ---
 * RPC 主路径：
 *   - insert: ApplyUpload / InitMultipart 时新建 pending 行
 *   - select_by_file_id: CompleteUpload / ApplyDownload / GetFileInfo
 *   - select_by_upload_id: multipart RPC 通过 upload_id 反查
 *   - update_status: pending → committed / deleted / quarantined
 * cleanup 路径：
 *   - list_pending_older_than: pending 超时清理
 *   - list_quarantined_older_than: quarantined 7 天后清理
 *   - list_committed_after: magic_sniff 异步嗅探游标
 *   - list_files_by_hash_status: blob GC 时反查 owner，扣减 quota
 *
 * 时间字段统一用 boost::posix_time::ptime（与 user/message 表一致）；
 * 调用方传 epoch_ms 时由 helper 转换。
 */

#include "infra/logger.hpp"
#include "dao/mysql.hpp"
#include "media_file.hxx"
#include "media_file-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <memory>
#include <string>
#include <vector>

namespace chatnow {

inline boost::posix_time::ptime ptime_from_ms(int64_t epoch_ms) {
    return boost::posix_time::from_time_t(epoch_ms / 1000)
         + boost::posix_time::milliseconds(epoch_ms % 1000);
}

class MediaFileTable {
public:
    using ptr = std::shared_ptr<MediaFileTable>;

    explicit MediaFileTable(const std::shared_ptr<odb::core::database>& db) : _db(db) {}

    /* brief: 新建 pending 行 */
    bool insert(MediaFile& f) {
        try {
            odb::transaction trans(_db->begin());
            _db->persist(f);
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("media_file insert 失败: file_id={} err={}", f.file_id(), e.what());
            return false;
        }
        return true;
    }

    /* brief: 按业务 file_id 取行 */
    std::shared_ptr<MediaFile> select_by_file_id(const std::string& file_id) {
        std::shared_ptr<MediaFile> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaFile>;
            res.reset(_db->query_one<MediaFile>(query::file_id == file_id));
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("media_file 查询失败: file_id={} err={}", file_id, e.what());
        }
        return res;
    }

    /* brief: 按 multipart upload_id 反查 file 行 */
    std::shared_ptr<MediaFile> select_by_upload_id(const std::string& upload_id) {
        std::shared_ptr<MediaFile> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaFile>;
            res.reset(_db->query_one<MediaFile>(query::upload_id == upload_id));
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("media_file by upload_id 失败: upload_id={} err={}", upload_id, e.what());
        }
        return res;
    }

    /* brief: 更新 bucket + object_key（并发去重改名时用） */
    bool update_bucket_key(const std::string& file_id, const std::string& bucket, const std::string& object_key) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaFile>;
            auto row = _db->query_one<MediaFile>(query::file_id == file_id);
            if (!row) { trans.commit(); return false; }
            row->bucket(bucket);
            row->object_key(object_key);
            _db->update(*row);
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("media_file update_bucket_key 失败: file_id={} err={}", file_id, e.what());
            return false;
        }
        return true;
    }

    /* brief: 更新 status，调用方在外层完成 ref_count++/quota++ 等业务动作 */
    bool update_status(const std::string& file_id, MediaFileStatus status) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaFile>;
            auto row = _db->query_one<MediaFile>(query::file_id == file_id);
            if (!row) { trans.commit(); return false; }
            row->status(status);
            _db->update(*row);
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("media_file update_status 失败: file_id={} err={}", file_id, e.what());
            return false;
        }
        return true;
    }

    /* brief: 取 status=PENDING 且 uploaded_at < cutoff 的行（cleanup 用） */
    std::vector<MediaFile> list_pending_older_than(boost::posix_time::ptime cutoff, int limit) {
        std::vector<MediaFile> v;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaFile>;
            auto r = _db->query<MediaFile>(
                ((query::status == MediaFileStatus::PENDING) &&
                 (query::uploaded_at < cutoff)) +
                (" LIMIT " + std::to_string(limit)));
            for (auto& f : r) v.push_back(f);
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("media_file list_pending_older_than 失败: err={}", e.what());
        }
        return v;
    }

    /* brief: 取 status=QUARANTINED 且 uploaded_at < cutoff 的行 */
    std::vector<MediaFile> list_quarantined_older_than(boost::posix_time::ptime cutoff, int limit) {
        std::vector<MediaFile> v;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaFile>;
            auto r = _db->query<MediaFile>(
                ((query::status == MediaFileStatus::QUARANTINED) &&
                 (query::uploaded_at < cutoff)) +
                (" LIMIT " + std::to_string(limit)));
            for (auto& f : r) v.push_back(f);
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("media_file list_quarantined_older_than 失败: err={}", e.what());
        }
        return v;
    }

    /* brief: 取 status=COMMITTED 且 uploaded_at > cursor 的行（magic_sniff 游标） */
    std::vector<MediaFile> list_committed_after(boost::posix_time::ptime cursor, int limit) {
        std::vector<MediaFile> v;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaFile>;
            auto r = _db->query<MediaFile>(
                ((query::status == MediaFileStatus::COMMITTED) &&
                 (query::uploaded_at > cursor)) +
                " ORDER BY " + query::uploaded_at +
                (" LIMIT " + std::to_string(limit)));
            for (auto& f : r) v.push_back(f);
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("media_file list_committed_after 失败: err={}", e.what());
        }
        return v;
    }

    /* brief: 同 hash + status in {DELETED,QUARANTINED} —— blob GC 时按 owner 退配额 */
    std::vector<MediaFile> list_files_by_hash_dead(const std::string& hash) {
        std::vector<MediaFile> v;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaFile>;
            auto r = _db->query<MediaFile>(
                (query::content_hash == hash) &&
                ((query::status == MediaFileStatus::DELETED) ||
                 (query::status == MediaFileStatus::QUARANTINED)));
            for (auto& f : r) v.push_back(f);
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("media_file list_files_by_hash_dead 失败: hash={} err={}", hash, e.what());
        }
        return v;
    }

    /* brief: SUM(file_size) WHERE owner_id=? AND status=PENDING — 配额校验用 */
    int64_t pending_bytes_of_owner(const std::string& owner_id) {
        int64_t total = 0;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaFile>;
            auto r = _db->query<MediaFile>(
                (query::owner_id == owner_id) &&
                (query::status == MediaFileStatus::PENDING));
            for (auto& f : r) total += f.file_size();
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("media_file pending_bytes_of_owner 失败: owner={} err={}", owner_id, e.what());
        }
        return total;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

}  // namespace chatnow
