#pragma once

/**
 * MediaBlobRefTable —— media_blob_ref 表 DAO 包装
 * ---
 * 业务约束：
 *   - 同 content_hash 多 file_id 共享同一份 MinIO 对象
 *   - inc_ref / dec_ref 需在事务里读 → 改 → 写，避免并发漂移
 *   - dec_ref 时刷新 last_decremented_at，cleanup 7 天 GC 缓冲依赖该字段
 */

#include "infra/logger.hpp"
#include "dao/mysql.hpp"
#include "media_blob_ref.hxx"
#include "media_blob_ref-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <memory>
#include <string>
#include <vector>

namespace chatnow {

class MediaBlobRefTable {
public:
    using ptr = std::shared_ptr<MediaBlobRefTable>;

    explicit MediaBlobRefTable(const std::shared_ptr<odb::core::database>& db) : _db(db) {}

    std::shared_ptr<MediaBlobRef> select_by_hash(const std::string& hash) {
        std::shared_ptr<MediaBlobRef> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaBlobRef>;
            res.reset(_db->query_one<MediaBlobRef>(query::content_hash == hash));
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("blob_ref 查询失败: hash={} err={}", hash, e.what());
        }
        return res;
    }

    /* brief: 不存在则插入（ref_count 初始 0），存在则不动；并发安全靠主键唯一约束 */
    bool upsert(const MediaBlobRef& r) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaBlobRef>;
            auto existing = _db->query_one<MediaBlobRef>(query::content_hash == r.content_hash());
            if (!existing) {
                MediaBlobRef tmp = r;
                _db->persist(tmp);
            }
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("blob_ref upsert 失败: hash={} err={}", r.content_hash(), e.what());
            return false;
        }
        return true;
    }

    /* brief: ref_count += 1（事务内 SELECT FOR UPDATE） */
    bool inc_ref(const std::string& hash) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaBlobRef>;
            std::shared_ptr<MediaBlobRef> b(
                _db->query_one<MediaBlobRef>((query::content_hash == hash) + " FOR UPDATE"));
            if (!b) { trans.commit(); return false; }
            b->ref_count(b->ref_count() + 1);
            _db->update(*b);
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("blob_ref inc_ref 失败: hash={} err={}", hash, e.what());
            return false;
        }
        return true;
    }

    /* brief: ref_count = max(0, ref_count - 1)，刷新 last_decremented_at */
    bool dec_ref(const std::string& hash, boost::posix_time::ptime now) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaBlobRef>;
            std::shared_ptr<MediaBlobRef> b(
                _db->query_one<MediaBlobRef>((query::content_hash == hash) + " FOR UPDATE"));
            if (!b) { trans.commit(); return false; }
            int32_t v = b->ref_count() - 1;
            if (v < 0) v = 0;
            b->ref_count(v);
            b->last_decremented_at(now);
            _db->update(*b);
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("blob_ref dec_ref 失败: hash={} err={}", hash, e.what());
            return false;
        }
        return true;
    }

    /* brief: ref_count==0 且 last_decremented_at < cutoff 的 blob —— GC 候选 */
    std::vector<MediaBlobRef> list_zero_ref_older_than(boost::posix_time::ptime cutoff, int limit) {
        std::vector<MediaBlobRef> v;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaBlobRef>;
            auto r = _db->query<MediaBlobRef>(
                ((query::ref_count == 0) &&
                 (query::last_decremented_at < cutoff)) +
                (" LIMIT " + std::to_string(limit)));
            for (auto& b : r) v.push_back(b);
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("blob_ref list_zero_ref_older_than 失败: err={}", e.what());
        }
        return v;
    }

    bool erase(const std::string& hash) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaBlobRef>;
            auto b = _db->query_one<MediaBlobRef>(query::content_hash == hash);
            if (b) _db->erase<MediaBlobRef>(b->content_hash());
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("blob_ref erase 失败: hash={} err={}", hash, e.what());
            return false;
        }
        return true;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

}  // namespace chatnow
