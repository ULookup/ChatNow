#pragma once

/**
 * MessageReactionTable —— message_reaction 表 DAO
 * ---
 * 表已通过 odb/message_reaction.hxx + ODB 自动建表。本类仅封装 CRUD。
 *
 * 索引：
 *   uk_msg_user_emoji (message_id, user_id, emoji)  unique
 *   idx_msg            (message_id)
 *
 * 一表一 DAO 文件惯例（与 mysql_user_block.hpp 同模式）。
 */

#include <odb/database.hxx>
#include <odb/transaction.hxx>
#include <odb/result.hxx>
#include <odb/mysql/database.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

#include "../infra/logger.hpp"
#include "message_reaction.hxx"
#include "message_reaction-odb.hxx"

namespace chatnow {

struct ReactionRow {
    unsigned long id;
    unsigned long message_id;
    std::string user_id;
    std::string emoji;
};

class MessageReactionTable {
public:
    using ptr = std::shared_ptr<MessageReactionTable>;
    explicit MessageReactionTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 插入一条 reaction；唯一索引冲突视幂等成功 */
    bool insert(unsigned long mid, const std::string &uid, const std::string &emoji) {
        try {
            odb::transaction trans(_db->begin());
            MessageReaction r(mid, uid, emoji);
            r.create_time(boost::posix_time::microsec_clock::universal_time());
            _db->persist(r);
            trans.commit();
            return true;
        } catch(const odb::object_already_persistent &) {
            return true;
        } catch(const odb::database_exception &e) {
            std::string what = e.what();
            if(what.find("Duplicate") != std::string::npos ||
               what.find("1062") != std::string::npos) return true;
            LOG_ERROR("MessageReaction.insert mid={} uid={} emoji={}: {}", mid, uid, emoji, what);
            return false;
        } catch(std::exception &e) {
            LOG_ERROR("MessageReaction.insert mid={} uid={} emoji={}: {}", mid, uid, emoji, e.what());
            return false;
        }
    }

    /* brief: 删除一条 reaction；不存在返回 true（幂等） */
    bool remove(unsigned long mid, const std::string &uid, const std::string &emoji) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessageReaction>;
            (void)_db->erase_query<MessageReaction>(
                query::message_id == mid &&
                query::user_id == uid &&
                query::emoji == emoji);
            trans.commit();
            return true;
        } catch(std::exception &e) {
            LOG_ERROR("MessageReaction.remove mid={} uid={} emoji={}: {}", mid, uid, emoji, e.what());
            return false;
        }
    }

    /* brief: 取单条消息的所有 reaction 行（给服务层 GROUP BY emoji 聚合） */
    std::vector<ReactionRow> select_by_message(unsigned long mid) {
        std::vector<ReactionRow> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessageReaction>;
            odb::result<MessageReaction> r(_db->query<MessageReaction>(
                (query::message_id == mid) + " ORDER BY id ASC"));
            for(auto it = r.begin(); it != r.end(); ++it) {
                res.push_back({0UL, it->message_id(), it->user_id(), it->emoji()});
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("MessageReaction.select_by_message mid={}: {}", mid, e.what());
        }
        return res;
    }

    /* brief: 批量取多条消息的所有 reaction 行（GetHistory/SyncMessages 用） */
    std::vector<ReactionRow> select_by_messages(const std::vector<unsigned long> &mids) {
        std::vector<ReactionRow> res;
        if(mids.empty()) return res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessageReaction>;
            odb::result<MessageReaction> r(_db->query<MessageReaction>(
                query::message_id.in_range(mids.begin(), mids.end()) +
                " ORDER BY message_id ASC, id ASC"));
            for(auto it = r.begin(); it != r.end(); ++it) {
                res.push_back({0UL, it->message_id(), it->user_id(), it->emoji()});
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("MessageReaction.select_by_messages size={}: {}", mids.size(), e.what());
        }
        return res;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
