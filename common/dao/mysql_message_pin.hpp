#pragma once

/**
 * MessagePinTable —— message_pin 表 DAO
 * ---
 * 唯一索引：uk_conv_msg (session_id, message_id)
 *
 * 注：odb/message_pin.hxx 内字段名仍是 _session_id（legacy），
 *    DAO 对外签名用 cid，内部 query 用 session_id 列名。
 */

#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/result.hxx>
#include <odb/mysql/database.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

#include "../infra/logger.hpp"
#include "message_pin.hxx"
#include "message_pin-odb.hxx"

namespace chatnow {

class MessagePinTable {
public:
    using ptr = std::shared_ptr<MessagePinTable>;
    explicit MessagePinTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 写一条 pin 行；唯一索引冲突视幂等成功 */
    bool insert(const std::string &cid, unsigned long mid, const std::string &pinner_uid) {
        try {
            odb::transaction trans(_db->begin());
            MessagePin p(cid, mid, pinner_uid);
            p.pinned_at(boost::posix_time::microsec_clock::universal_time());
            _db->persist(p);
            trans.commit();
            return true;
        } catch(const odb::object_already_persistent &) {
            return true;
        } catch(const odb::database_exception &e) {
            std::string what = e.what();
            if(what.find("Duplicate") != std::string::npos ||
               what.find("1062") != std::string::npos) return true;
            LOG_ERROR("MessagePin.insert cid={} mid={} by={}: {}", cid, mid, pinner_uid, what);
            return false;
        } catch(std::exception &e) {
            LOG_ERROR("MessagePin.insert cid={} mid={} by={}: {}", cid, mid, pinner_uid, e.what());
            return false;
        }
    }

    bool remove(const std::string &cid, unsigned long mid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessagePin>;
            (void)_db->erase_query<MessagePin>(
                query::session_id == cid && query::message_id == mid);
            trans.commit();
            return true;
        } catch(std::exception &e) {
            LOG_ERROR("MessagePin.remove cid={} mid={}: {}", cid, mid, e.what());
            return false;
        }
    }

    int count_by_conversation(const std::string &cid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessagePin>;
            odb::result<MessagePin> r(_db->query<MessagePin>(query::session_id == cid));
            int n = 0;
            for(auto it = r.begin(); it != r.end(); ++it) ++n;
            trans.commit();
            return n;
        } catch(std::exception &e) {
            LOG_ERROR("MessagePin.count_by_conversation cid={}: {}", cid, e.what());
            return 0;
        }
    }

    std::vector<unsigned long> list_by_conversation(const std::string &cid, int limit = 10) {
        std::vector<unsigned long> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessagePin>;
            odb::result<MessagePin> r(_db->query<MessagePin>(
                (query::session_id == cid) + " ORDER BY pinned_at DESC LIMIT " +
                std::to_string(limit)));
            for(auto it = r.begin(); it != r.end(); ++it) res.push_back(it->message_id());
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("MessagePin.list_by_conversation cid={}: {}", cid, e.what());
        }
        return res;
    }

    /* brief: 该 cid 的 mid 集合中已 pin 的子集，给 fill_pin_flag 批量用 */
    std::vector<unsigned long> list_pinned_in(const std::string &cid,
                                              const std::vector<unsigned long> &mids) {
        std::vector<unsigned long> res;
        if(mids.empty()) return res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessagePin>;
            odb::result<MessagePin> r(_db->query<MessagePin>(
                query::session_id == cid &&
                query::message_id.in_range(mids.begin(), mids.end())));
            for(auto it = r.begin(); it != r.end(); ++it) res.push_back(it->message_id());
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("MessagePin.list_pinned_in cid={} size={}: {}", cid, mids.size(), e.what());
        }
        return res;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
