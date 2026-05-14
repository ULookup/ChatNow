#pragma once

#include "logger.hpp"
#include "mysql.hpp"
#include "chat_session.hxx"
#include "chat_session-odb.hxx"
#include "chat_session_member.hxx"
#include "chat_session_member-odb.hxx"
#include "chat_session_view.hxx"
#include "chat_session_view-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace chatnow
{

/**
 * ChatSessionMemberTable
 * ------------------------------------------------------------------
 * chat_session_member 表的 DAO 封装。
 *
 * 关键变化（对齐新 schema）：
 *   - 退群改软删除：set_quit() 置 is_quit=true + quit_time，行不删
 *   - 二次入群走 update：rejoin() 复用现有行（uk_session_user 唯一约束保证）
 *   - 已读 / 已收游标改 seq 维度：update_last_read_seq / update_last_ack_seq
 *   - 列表查询的 ORDER BY 不再依赖 cs.last_message_time（已删字段）
 *     —— 改用 max_seq 兜底；客户端最终展示顺序由 Redis chat:last:{ssid} 决定
 *   - members() 返回活跃成员（排除 is_quit=true），并提供 all_members()
 *     给"已退群"审计场景使用
 *   - update_role / set_mute_until / set_alias 等细粒度接口替代裸 update
 * ------------------------------------------------------------------
 */
class ChatSessionMemberTable
{
public:
    using ptr = std::shared_ptr<ChatSessionMemberTable>;
    ChatSessionMemberTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 单成员入群 — 配合 _update_session_member_count 维护 chat_session.member_count */
    bool append(ChatSessionMember &csm) {
        try {
            if(csm.join_time().is_not_a_date_time()) {
                csm.join_time(boost::posix_time::microsec_clock::universal_time());
            }
            odb::transaction trans(_db->begin());
            _db->persist(csm);
            _update_session_member_count(csm.session_id(), 1);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("新增单会话成员失败: {}:{} - {}", csm.session_id(), csm.user_id(), e.what());
            return false;
        }
        return true;
    }

    /* brief: 批量入群（伴随 chat_session.member_count 调整） */
    bool append(std::vector<ChatSessionMember> &csm_list) {
        if(csm_list.empty()) return true;
        try {
            auto now = boost::posix_time::microsec_clock::universal_time();
            odb::transaction trans(_db->begin());
            std::map<std::string, int> session_delta;
            for(auto &csm : csm_list) {
                if(csm.join_time().is_not_a_date_time()) csm.join_time(now);
                _db->persist(csm);
                session_delta[csm.session_id()]++;
            }
            for(const auto &[ssid, count] : session_delta) {
                _update_session_member_count(ssid, count);
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("新增多会话成员失败: {} - {}", csm_list.size(), e.what());
            return false;
        }
        return true;
    }

    /* brief: 创建会话后批量补成员（不调整 member_count，由调用方在创建会话时一次性写入正确值） */
    bool append_after_create(std::vector<ChatSessionMember> &csm_list) {
        if(csm_list.empty()) return true;
        try {
            auto now = boost::posix_time::microsec_clock::universal_time();
            odb::transaction trans(_db->begin());
            for(auto &csm : csm_list) {
                if(csm.join_time().is_not_a_date_time()) csm.join_time(now);
                _db->persist(csm);
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("批量补成员失败: {}", e.what());
            return false;
        }
        return true;
    }

    /* brief: 退群（软删除）— is_quit=true + quit_time；member_count -1
     *  - 不物理 erase：保留已读游标 / 邀请回群识别 / 风控审计
     */
    bool set_quit(const std::string &ssid, const std::string &uid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ChatSessionMember>;
            std::shared_ptr<ChatSessionMember> m(_db->query_one<ChatSessionMember>(
                query::session_id == ssid && query::user_id == uid));
            if(!m || m->is_quit()) {
                trans.commit();
                return false; // 已经退过群或不存在
            }
            m->is_quit(true);
            m->quit_time(boost::posix_time::microsec_clock::universal_time());
            _db->update(*m);
            _update_session_member_count(ssid, -1);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("退群失败 {}-{}: {}", ssid, uid, e.what());
            return false;
        }
        return true;
    }

    /* brief: 二次入群（复用旧行）— 重置 join_time / role / inviter_id / is_quit
     *  - uk_session_user 唯一约束保证一对一行；新成员请用 append()
     */
    bool rejoin(const std::string &ssid, const std::string &uid,
                ChatSessionRole role,
                const std::string &inviter_id,
                JoinSource source)
    {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ChatSessionMember>;
            std::shared_ptr<ChatSessionMember> m(_db->query_one<ChatSessionMember>(
                query::session_id == ssid && query::user_id == uid));
            if(!m) {
                trans.commit();
                return false; // 不是旧成员
            }
            m->is_quit(false);
            m->join_time(boost::posix_time::microsec_clock::universal_time());
            m->role(role);
            if(!inviter_id.empty()) m->inviter_id(inviter_id);
            m->join_source(source);
            // 已读游标 / 群昵称等保留，符合产品预期
            _db->update(*m);
            if(m->is_quit() == false) _update_session_member_count(ssid, 1);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("二次入群失败 {}-{}: {}", ssid, uid, e.what());
            return false;
        }
        return true;
    }

    /* brief: 物理移除会话所有成员（仅在解散群且需清理脏数据时使用，正常场景请 dismiss）*/
    bool remove_all(const std::string &ssid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ChatSessionMember>;
            _db->erase_query<ChatSessionMember>(query::session_id == ssid);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("清理会话成员失败 {}: {}", ssid, e.what());
            return false;
        }
        return true;
    }

    /* brief: 取活跃成员 ID 列表（默认排除已退群） */
    std::vector<std::string> members(const std::string &ssid) {
        std::vector<std::string> res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<ChatSessionMember>;
            using result = odb::result<ChatSessionMember>;
            result r(_db->query<ChatSessionMember>(
                query::session_id == ssid && query::is_quit == false));
            for(auto &row : r) res.push_back(row.user_id());
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("获取会话活跃成员失败 {}: {}", ssid, e.what());
        }
        return res;
    }

    /* brief: 取所有成员（含已退群）— 审计 / 邀请回群识别用 */
    std::vector<std::string> all_members(const std::string &ssid) {
        std::vector<std::string> res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<ChatSessionMember>;
            using result = odb::result<ChatSessionMember>;
            result r(_db->query<ChatSessionMember>(query::session_id == ssid));
            for(auto &row : r) res.push_back(row.user_id());
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("获取会话所有成员失败 {}: {}", ssid, e.what());
        }
        return res;
    }

    /* brief: 是否在群（活跃成员）*/
    bool exists(const std::string &ssid, const std::string &uid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ChatSessionMember>;
            std::shared_ptr<ChatSessionMember> m(_db->query_one<ChatSessionMember>(
                query::session_id == ssid && query::user_id == uid && query::is_quit == false));
            trans.commit();
            return m != nullptr;
        } catch(std::exception &e) {
            LOG_ERROR("判断会话成员失败 {}-{}: {}", ssid, uid, e.what());
            return false;
        }
    }

    /* brief: 取单条成员行（含已退群） */
    std::shared_ptr<ChatSessionMember> select(const std::string &ssid, const std::string &uid) {
        std::shared_ptr<ChatSessionMember> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ChatSessionMember>;
            res.reset(_db->query_one<ChatSessionMember>(
                query::session_id == ssid && query::user_id == uid));
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("查询会话成员失败 {}-{}: {}", ssid, uid, e.what());
        }
        return res;
    }

    /* brief: 批量按 user_id 查（in_range 防注入） */
    std::vector<ChatSessionMember> select(const std::string &ssid, const std::vector<std::string> &uids) {
        std::vector<ChatSessionMember> res;
        if(uids.empty()) return res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<ChatSessionMember>;
            using result = odb::result<ChatSessionMember>;
            result r(_db->query<ChatSessionMember>(
                query::session_id == ssid &&
                query::user_id.in_range(uids.begin(), uids.end())));
            for(auto &row : r) res.push_back(row);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("批量获取会话成员失败 ssid:{} count:{} - {}", ssid, uids.size(), e.what());
        }
        return res;
    }

    /* brief: 通用 update（高频细粒度修改请用下方专用接口）
     *  - 注意：ODB 的 update 会写全行。chatsession 路径写回的 last_ack_seq /
     *    last_read_seq 是 chatsession 进入事务时的快照值，可能已被 push 推进。
     *  - 设计取舍：last_ack_seq / last_read_seq 的写权威在 push / message 服务，
     *    它们走的是原子 UPDATE GREATEST(...)，永远以 DB 现值为准；
     *    即使 chatsession 这次 update 把字段瞬时覆盖回旧值，下一条 ACK 到来时
     *    GREATEST 会立即纠正，最坏一个 ACK 周期的偏差，业务可接受。
     *  - 因此本路径不再额外加 SELECT FOR UPDATE 行锁防御，避免无谓的事务等待。
     */
    bool update(const std::shared_ptr<ChatSessionMember> &csm) {
        if(!csm) return false;
        try {
            odb::transaction trans(_db->begin());
            _db->update(*csm);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新会话成员失败 {}-{}: {}", csm->session_id(), csm->user_id(), e.what());
            return false;
        }
        return true;
    }

    /* brief: 批量 update — 同事务内写多行，用于转让群主等需原子的多行变更 */
    bool update(const std::vector<std::shared_ptr<ChatSessionMember>> &items) {
        try {
            odb::transaction trans(_db->begin());
            for(const auto &csm : items) {
                if(!csm) continue;
                _db->update(*csm);
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("批量更新会话成员失败: {}", e.what());
            return false;
        }
        return true;
    }

    /* brief: 推进已读游标（仅在新值 > 旧值时写）— 防止多端回滚 */
    /* brief: 推进已读游标（多端已读，单调递增）— 原子 UPDATE GREATEST */
    bool update_last_read_seq(const std::string &ssid, const std::string &uid, unsigned long new_seq) {
        return _atomic_advance_seq("last_read_seq", ssid, uid, new_seq);
    }

    /* brief: 推进送达游标（多端送达回执，单调递增）— 原子 UPDATE GREATEST
     *  - DB 层强保证单调；不会被其它服务的全行 UPDATE 覆盖回退
     *  - 返回 true 表示 SQL 执行无异常（包括 GREATEST 等值不推进的幂等场景）
     *  - 不区分"行不存在 vs 等值不推进"：调用方对两者均不重试（已退群 / 幂等重复）
     */
    bool update_last_ack_seq(const std::string &ssid, const std::string &uid, unsigned long new_seq) {
        return _atomic_advance_seq("last_ack_seq", ssid, uid, new_seq);
    }

private:
    /* brief: 单字段原子推进 — UPDATE chat_session_member SET <col>=GREATEST(<col>, n) WHERE ssid=? AND uid=?
     *  - column 必须是白名单常量字符串（来自代码内部，无外部输入），不需要转义
     *  - ssid / uid 走最小转义（仅 ' 和 \）防御性兜底，避免上游脏数据触发注入
     */
    bool _atomic_advance_seq(const char *column, const std::string &ssid,
                             const std::string &uid, unsigned long new_seq)
    {
        try {
            odb::transaction trans(_db->begin());
            std::ostringstream sql;
            sql << "UPDATE chat_session_member SET " << column
                << " = GREATEST(" << column << ", " << new_seq << ")"
                << " WHERE session_id = '" << _escape_id(ssid) << "'"
                << "   AND user_id = '"    << _escape_id(uid)  << "'";
            _db->execute(sql.str());
            trans.commit();
            return true;
        } catch(std::exception &e) {
            LOG_ERROR("推进 {} 失败 {}-{}: {}", column, ssid, uid, e.what());
            return false;
        }
    }

    static std::string _escape_id(const std::string &s) {
        std::string out;
        out.reserve(s.size());
        for(char c : s) {
            if(c == '\'' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    }

public:

    /* brief: 设置 / 取消禁言 — 通过 mute_until 自然过期，不需后台清理 */
    bool set_mute_until(const std::string &ssid, const std::string &uid,
                       const boost::posix_time::ptime &mute_until)
    {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ChatSessionMember>;
            std::shared_ptr<ChatSessionMember> m(_db->query_one<ChatSessionMember>(
                query::session_id == ssid && query::user_id == uid));
            if(!m) {
                trans.commit();
                return false;
            }
            m->mute_until(mute_until);
            _db->update(*m);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("设置禁言到期失败 {}-{}: {}", ssid, uid, e.what());
            return false;
        }
        return true;
    }

    /* brief: 修改群昵称 */
    bool set_alias(const std::string &ssid, const std::string &uid, const std::string &alias) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ChatSessionMember>;
            std::shared_ptr<ChatSessionMember> m(_db->query_one<ChatSessionMember>(
                query::session_id == ssid && query::user_id == uid));
            if(!m) {
                trans.commit();
                return false;
            }
            m->alias(alias);
            _db->update(*m);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("设置群昵称失败 {}-{}: {}", ssid, uid, e.what());
            return false;
        }
        return true;
    }

    /* brief: 修改成员角色（管理员调整 / 转让群主） */
    bool update_role(const std::string &ssid, const std::string &uid, ChatSessionRole role) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<ChatSessionMember>;
            std::shared_ptr<ChatSessionMember> m(_db->query_one<ChatSessionMember>(
                query::session_id == ssid && query::user_id == uid));
            if(!m) {
                trans.commit();
                return false;
            }
            m->role(role);
            _db->update(*m);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新成员角色失败 {}-{}: {}", ssid, uid, e.what());
            return false;
        }
        return true;
    }

    /* brief: 我的会话列表（置顶 -> 最近活跃；过滤已退群与隐藏）
     *  - ORDER BY 不再依赖已删字段 cs.last_message_time
     *  - 用 cs.max_seq 排序，保证活跃群优先（max_seq 越大越活跃）
     *  - 真正"按最新消息时间排序"的展示顺序在客户端结合 Redis 预览缓存计算
     */
    std::vector<OrderedChatSessionView> list_ordered_by_user(const std::string &uid)
    {
        std::vector<OrderedChatSessionView> res;
        try {
            odb::transaction trans(_db->begin());
            using result = odb::result<OrderedChatSessionView>;

            std::ostringstream oss;
            oss << "cm.user_id = '" << uid << "'"
                << " AND cm.is_quit = 0"
                << " AND cm.visible = 1"
                << " ORDER BY"
                << " cm.pin_time IS NOT NULL DESC,"  // 置顶优先
                << " cm.pin_time DESC,"              // 多个置顶按时间倒序
                << " cs.max_seq DESC,"               // 非置顶按活跃度兜底
                << " cs.update_time DESC"
                << " LIMIT 200";

            result r(_db->query<OrderedChatSessionView>(oss.str()));
            for(auto &row : r) res.push_back(row);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("获取用户 {} 排序会话列表失败: {}", uid, e.what());
        }
        return res;
    }

private:
    /* brief: 维护 chat_session.member_count；FOR UPDATE 防并发写计数飘移 */
    void _update_session_member_count(const std::string &ssid, int delta) {
        using SessionQuery = odb::query<ChatSession>;
        std::shared_ptr<ChatSession> session(
            _db->query_one<ChatSession>(
                (SessionQuery::chat_session_id == ssid) + " FOR UPDATE"));
        if(!session) {
            std::string err = "更新计数失败，未找到会话: " + ssid;
            LOG_ERROR(err);
            throw std::runtime_error(err);
        }
        int new_count = session->member_count() + delta;
        if(new_count < 0) new_count = 0;
        session->member_count(new_count);
        session->update_time(boost::posix_time::microsec_clock::universal_time());
        _db->update(*session);
        LOG_DEBUG("会话[{}] 人数变更: {} -> {}", ssid, new_count - delta, new_count);
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
