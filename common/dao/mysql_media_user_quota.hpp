#pragma once

/**
 * MediaUserQuotaTable —— media_user_quota 表 DAO 包装
 * ---
 * 业务流：
 *   - ensure(uid)        ：取行；不存在按默认 5 GB 创建并返回
 *   - inc_used(uid, delta, now) ：CompleteUpload 后累计 used
 *   - dec_used(uid, delta, now) ：blob 真正删除时退还 used
 */

#include "infra/logger.hpp"
#include "dao/mysql.hpp"
#include "media_user_quota.hxx"
#include "media_user_quota-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include <boost/date_time/posix_time/posix_time.hpp>
#include <memory>
#include <string>

namespace chatnow {

class MediaUserQuotaTable {
public:
    using ptr = std::shared_ptr<MediaUserQuotaTable>;

    explicit MediaUserQuotaTable(const std::shared_ptr<odb::core::database>& db) : _db(db) {}

    /* brief: 不存在则按默认 5 GB 创建并返回 */
    MediaUserQuota ensure(const std::string& user_id) {
        MediaUserQuota row;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaUserQuota>;
            auto p = _db->query_one<MediaUserQuota>(query::user_id == user_id);
            if (!p) {
                MediaUserQuota fresh;
                fresh.user_id(user_id);
                fresh.used_bytes(0);
                fresh.quota_bytes(kMediaDefaultQuotaBytes);
                fresh.updated_at(boost::posix_time::microsec_clock::universal_time());
                _db->persist(fresh);
                row = fresh;
            } else {
                row = *p;
            }
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("user_quota ensure 失败: uid={} err={}", user_id, e.what());
        }
        return row;
    }

    /* brief: used_bytes += delta；事务内 SELECT FOR UPDATE */
    bool inc_used(const std::string& user_id, int64_t delta, boost::posix_time::ptime now) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaUserQuota>;
            std::shared_ptr<MediaUserQuota> p(
                _db->query_one<MediaUserQuota>((query::user_id == user_id) + " FOR UPDATE"));
            if (!p) {
                MediaUserQuota fresh;
                fresh.user_id(user_id);
                fresh.used_bytes(delta);
                fresh.quota_bytes(kMediaDefaultQuotaBytes);
                fresh.updated_at(now);
                _db->persist(fresh);
            } else {
                p->used_bytes(p->used_bytes() + delta);
                p->updated_at(now);
                _db->update(*p);
            }
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("user_quota inc_used 失败: uid={} err={}", user_id, e.what());
            return false;
        }
        return true;
    }

    /* brief: used_bytes = max(0, used_bytes - delta) */
    bool dec_used(const std::string& user_id, int64_t delta, boost::posix_time::ptime now) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MediaUserQuota>;
            std::shared_ptr<MediaUserQuota> p(
                _db->query_one<MediaUserQuota>((query::user_id == user_id) + " FOR UPDATE"));
            if (!p) { trans.commit(); return false; }
            int64_t v = p->used_bytes() - delta;
            if (v < 0) v = 0;
            p->used_bytes(v);
            p->updated_at(now);
            _db->update(*p);
            trans.commit();
        } catch (std::exception& e) {
            LOG_ERROR("user_quota dec_used 失败: uid={} err={}", user_id, e.what());
            return false;
        }
        return true;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

}  // namespace chatnow
