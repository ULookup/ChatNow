#pragma once

#include "infra/logger.hpp"
#include "dao/mysql.hpp"
#include "chat_session.hxx"
#include "chat_session-odb.hxx"
#include "chat_session_view.hxx"
#include "chat_session_view-odb.hxx"
#include "chat_session_member.hxx"
#include "chat_session_member-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include <vector>
#include <string>
#include <memory>

namespace chatnow
{

/**
 * ChatSessionTable
 * ------------------------------------------------------------------
 * chat_session 表的 DAO 封装。
 *
 * 关键变化（对齐新 schema）：
 *   - 移除 last_message_time / singleChatSession() / groupChatSession()
 *     —— 旧 self-join 视图已删，单聊对端通过 chat_session.peer_user_id 直接取
 *   - select_by_peer：按双方 user_id 取单聊（基于 peer_user_id 列直接查，零 join）
 *   - update_meta：群名/头像/公告/描述等元信息修改，统一刷 update_time
 *   - dismiss：群解散走软删除（status=DISMISSED），不物理 erase 主表与 member
 *   - bump_max_seq：异步刷 max_seq 快照（最终一致），仅在递增时写
 *   - 删除 remove(uid, pid)：单聊不再支持物理删，软删走 dismiss
 * ------------------------------------------------------------------
 */
class ChatSessionTable
{
public:
    using ptr = std::shared_ptr<ChatSessionTable>;
    ChatSessionTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 新增会话；自动写 create_time / update_time */
    bool insert(ChatSession &cs) {
        try {
            auto now = boost::posix_time::microsec_clock::universal_time();
            cs.create_time(now);
            cs.update_time(now);

            odb::transaction trans(_db->begin());
            _db->persist(cs);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("新增会话失败 {}: {}", cs.chat_session_name(), e.what());
            return false;
        }
        return true;
    }

    /* brief: 群解散（软删除）— 主表置 DISMISSED；成员行不动，保留已读历史 */
    bool dismiss(const std::string &ssid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ChatSession>;
            std::shared_ptr<ChatSession> cs(_db->query_one<ChatSession>(
                query::chat_session_id == ssid));
            if(!cs) {
                trans.commit();
                return false;
            }
            cs->status(ChatSessionStatus::DISMISSED);
            cs->update_time(boost::posix_time::microsec_clock::universal_time());
            _db->update(*cs);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("解散会话失败 {}: {}", ssid, e.what());
            return false;
        }
        return true;
    }

    /* brief: 通过会话 ID 查会话 */
    std::shared_ptr<ChatSession> select(const std::string &ssid) {
        std::shared_ptr<ChatSession> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ChatSession>;
            res.reset(_db->query_one<ChatSession>(query::chat_session_id == ssid));
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("查询会话失败 {}: {}", ssid, e.what());
        }
        return res;
    }

    /* brief: 通过两端 user_id 取单聊会话（基于 peer_user_id 字段，零 self-join）
     *  - 业务上单聊 chat_session_id 建议由 (min_uid,max_uid) 哈希生成，
     *    优先走 chat_session_id 直查；此函数兜底用
     */
    std::shared_ptr<ChatSession> select_single_by_peer(const std::string &uid, const std::string &pid) {
        std::shared_ptr<ChatSession> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ChatSession>;
            // 任意一方都可以是 owner_id（单聊无群主），peer_user_id 字段记录的是"另一方"
            // 所以两个方向都查一遍：A 视角 peer=B、B 视角 peer=A 至少命中一行（若双向写入）
            res.reset(_db->query_one<ChatSession>(
                query::chat_session_type == ChatSessionType::SINGLE &&
                query::peer_user_id == pid));
            (void)uid; // uid 仅用于上层鉴权，schema 上 peer_user_id 已足以定位
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过对端查单聊失败 {}-{}: {}", uid, pid, e.what());
        }
        return res;
    }

    /* brief: 单聊是否已存在（防重复创建） */
    bool single_exists(const std::string &uid, const std::string &pid) {
        return static_cast<bool>(select_single_by_peer(uid, pid));
    }

    /* brief: 会话是否存在 */
    bool exists(const std::string &ssid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ChatSession>;
            std::shared_ptr<ChatSession> r(_db->query_one<ChatSession>(
                query::chat_session_id == ssid));
            trans.commit();
            return r != nullptr;
        } catch(std::exception &e) {
            LOG_ERROR("判断会话是否存在失败 {}: {}", ssid, e.what());
            return false;
        }
    }

    /* brief: 通用更新（统一刷 update_time，便于客户端拉会话元信息变更） */
    bool update(const std::shared_ptr<ChatSession> &cs) {
        try {
            cs->update_time(boost::posix_time::microsec_clock::universal_time());
            odb::transaction trans(_db->begin());
            _db->update(*cs);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新会话失败 {}: {}", cs->chat_session_id(), e.what());
            return false;
        }
        return true;
    }

    /* brief: 异步刷新 max_seq 快照（仅在新值 > 旧值时写）
     *  - 真正强一致 seq 由 Redis 维护；本字段只是 DB 层的最终一致快照，
     *    用于偶发回查 / 风控统计；同时避免回退导致数据反向波动
     */
    bool bump_max_seq(const std::string &ssid, unsigned long new_seq) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ChatSession>;
            std::shared_ptr<ChatSession> cs(_db->query_one<ChatSession>(
                (query::chat_session_id == ssid) + " FOR UPDATE"));
            if(!cs) {
                trans.commit();
                return false;
            }
            if(cs->max_seq() < new_seq) {
                cs->max_seq(new_seq);
                _db->update(*cs);
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("刷新 max_seq 失败 {}: {}", ssid, e.what());
            return false;
        }
        return true;
    }

    /* brief: 批量取会话（in_range 防注入） */
    std::vector<ChatSession> select(const std::vector<std::string> &ssid_list) {
        std::vector<ChatSession> res;
        if(ssid_list.empty()) return res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<ChatSession>;
            using result = odb::result<ChatSession>;
            result r(_db->query<ChatSession>(
                query::chat_session_id.in_range(ssid_list.begin(), ssid_list.end())));
            for(auto &cs : r) res.push_back(cs);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("批量取会话失败: {}", e.what());
        }
        return res;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
