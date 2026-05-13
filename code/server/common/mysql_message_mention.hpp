#pragma once

#include "logger.hpp"
#include "mysql.hpp"
#include "message_mention.hxx"
#include "message_mention-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include <memory>
#include <string>
#include <vector>

namespace chatnow
{

/**
 * MessageMentionTable
 * ------------------------------------------------------------------
 * message_mention 表的 DAO 封装。
 *
 * 设计要点：
 *   - 用约定的 user_id="*" 表示 @全体，避免群人数膨胀写放大
 *   - 客户端拉取被 @ 的消息时同时匹配自己的 user_id 与 "*"
 *   - uk_msg_user 唯一约束让 ack 写入幂等
 *
 * 索引使用：
 *   - uk_msg_user(message_id, user_id) — 写时去重
 *   - idx_user_msg(user_id, message_id) — "我被 @ 的所有消息"
 * ------------------------------------------------------------------
 */
class MessageMentionTable
{
public:
    using ptr = std::shared_ptr<MessageMentionTable>;
    MessageMentionTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 批量写入 — 消息发送时一次写入所有被 @ 用户行；事务感知 */
    bool insert(unsigned long message_id, const std::vector<std::string> &user_ids) {
        if(user_ids.empty()) return true;
        try {
            bool has_external_trans = odb::transaction::has_current();
            std::unique_ptr<odb::transaction> local_trans;
            if(!has_external_trans) {
                local_trans.reset(new odb::transaction(_db->begin()));
            }
            for(const auto &uid : user_ids) {
                MessageMention mm(message_id, uid);
                try {
                    _db->persist(mm);
                } catch(odb::exception &) {
                    // 唯一约束冲突视为幂等，跳过
                }
            }
            if(!has_external_trans) local_trans->commit();
        } catch(std::exception &e) {
            LOG_ERROR("批量插入 @ 提及失败 mid={}: {}", message_id, e.what());
            throw;
        }
        return true;
    }

    /* brief: 标记 @ 全体（单行 user_id="*"） */
    bool insert_at_all(unsigned long message_id) {
        return insert(message_id, std::vector<std::string>{"*"});
    }

    /* brief: 取一条消息的 @ 清单 */
    std::vector<std::string> list_of(unsigned long message_id) {
        std::vector<std::string> res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<MessageMention>;
            using result = odb::result<MessageMention>;
            result r(_db->query<MessageMention>(query::message_id == message_id));
            for(auto &m : r) res.push_back(m.user_id());
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("查询 @ 清单失败 mid={}: {}", message_id, e.what());
        }
        return res;
    }

    /* brief: 我是否被 @（包含 "*" 全体）*/
    bool is_mentioned(unsigned long message_id, const std::string &user_id) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessageMention>;
            std::shared_ptr<MessageMention> r(_db->query_one<MessageMention>(
                query::message_id == message_id &&
                (query::user_id == user_id || query::user_id == "*")));
            trans.commit();
            return r != nullptr;
        } catch(std::exception &e) {
            LOG_ERROR("查询是否被 @ 失败 mid={} uid={}: {}", message_id, user_id, e.what());
            return false;
        }
    }

    /* brief: "我被 @ 的所有消息 ID" — 走 idx_user_msg 索引
     *  - 同时匹配自己 ID 与 "*"，按 message_id 倒序分页（消息 ID 雪花趋势递增）
     */
    std::vector<unsigned long> list_my_mentions(const std::string &user_id, size_t limit = 100) {
        std::vector<unsigned long> res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<MessageMention>;
            using result = odb::result<MessageMention>;
            result r(_db->query<MessageMention>(
                (query::user_id == user_id || query::user_id == "*") +
                " ORDER BY message_id DESC LIMIT " + std::to_string(limit)));
            for(auto &m : r) res.push_back(m.message_id());
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("查询我被 @ 列表失败 uid={}: {}", user_id, e.what());
        }
        return res;
    }

    /* brief: 撤回 / 删除消息时联动清理 */
    bool remove_by_message(unsigned long message_id) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<MessageMention>;
            _db->erase_query<MessageMention>(query::message_id == message_id);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("删除 @ 记录失败 mid={}: {}", message_id, e.what());
            return false;
        }
        return true;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
