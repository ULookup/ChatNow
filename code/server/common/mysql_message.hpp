#pragma once

#include "logger.hpp"
#include "mysql.hpp"
#include "message.hxx"
#include "message-odb.hxx"

#include <memory>
#include <string>
#include <vector>

namespace chatnow
{

/**
 * MessageTable
 * ------------------------------------------------------------------
 * message 表的 DAO 封装。
 *
 * 关键变化（对齐新 schema）：
 *   - 查询路径从 message_id 全局排序改为 (session_id, seq_id) 范围扫
 *     —— 走 uk_session_seq 唯一索引，免回表
 *   - 新增基于 seq 的常用接口：recent_by_seq / range_by_seq / max_seq_of_session
 *   - 新增 client_msg_id 幂等去重接口：select_by_client_msg
 *   - 新增 mark_revoked / mark_deleted：状态机变更而非物理 erase
 *   - insert / 重要写操作保留"外部事务感知"：服务层有 ODB 事务时复用
 * ------------------------------------------------------------------
 */
class MessageTable
{
public:
    using ptr = std::shared_ptr<MessageTable>;
    MessageTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}
    ~MessageTable() = default;

    /* brief: 单条插入；外部已有事务则挂载，否则本地起一个 */
    bool insert(Message &msg) {
        try {
            bool has_external_trans = odb::transaction::has_current();
            std::unique_ptr<odb::transaction> local_trans;
            if(!has_external_trans) {
                local_trans.reset(new odb::transaction(_db->begin()));
            }
            _db->persist(msg);
            if(!has_external_trans) local_trans->commit();
        } catch(std::exception &e) {
            LOG_ERROR("插入消息失败: {}", e.what());
            throw; // 让外部事务回滚
        }
        return true;
    }

    /* brief: 批量插入（典型场景：转发多条 / 离线补偿）；事务感知 */
    bool insert(std::vector<Message> &msgs) {
        if(msgs.empty()) return true;
        try {
            bool has_external_trans = odb::transaction::has_current();
            std::unique_ptr<odb::transaction> local_trans;
            if(!has_external_trans) {
                local_trans.reset(new odb::transaction(_db->begin()));
            }
            for(auto &m : msgs) _db->persist(m);
            if(!has_external_trans) local_trans->commit();
        } catch(std::exception &e) {
            LOG_ERROR("批量插入消息失败 count={}: {}", msgs.size(), e.what());
            throw;
        }
        return true;
    }

    /* brief: 客户端断网重发幂等查找
     *  - 走 uk_client_msg(user_id, client_msg_id) 唯一索引
     *  - 命中即视为重复发送，直接复用旧 message 不再写库
     */
    std::shared_ptr<Message> select_by_client_msg(const std::string &user_id,
                                                  const std::string &client_msg_id)
    {
        std::shared_ptr<Message> res;
        if(client_msg_id.empty()) return res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Message>;
            res.reset(_db->query_one<Message>(
                query::user_id == user_id && query::client_msg_id == client_msg_id));
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过 client_msg_id 查询失败 {}-{}: {}", user_id, client_msg_id, e.what());
        }
        return res;
    }

    /* brief: 全局消息 ID 查询（跨会话引用 / 转发回查） */
    std::shared_ptr<Message> select_by_id(unsigned long message_id) {
        std::shared_ptr<Message> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Message>;
            res.reset(_db->query_one<Message>(query::message_id == message_id));
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("按 message_id 查询失败 {}: {}", message_id, e.what());
        }
        return res;
    }

    /* brief: 批量按 message_id 查（in_range，免拼字符串） */
    std::vector<Message> select_by_ids(const std::vector<unsigned long> &ids) {
        std::vector<Message> res;
        if(ids.empty()) return res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<Message>;
            using result = odb::result<Message>;
            // 排序按 seq_id 升序，保证下游展示顺序确定
            result r(_db->query<Message>(
                query::message_id.in_range(ids.begin(), ids.end()) +
                " ORDER BY seq_id ASC"));
            for(auto &m : r) res.push_back(m);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("批量按 message_id 查询失败: {}", e.what());
        }
        return res;
    }

    /* brief: 取会话最近 N 条；按 seq_id 倒序后反转保证从旧到新展示
     *  - 走 uk_session_seq 索引；无 ORDER BY create_time，避免索引外排序
     */
    std::vector<Message> recent_by_seq(const std::string &ssid, int count) {
        std::vector<Message> res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<Message>;
            using result = odb::result<Message>;
            std::stringstream cond;
            cond << "session_id='" << ssid << "' "
                 << "ORDER BY seq_id DESC LIMIT " << count;
            result r(_db->query<Message>(cond.str()));
            for(auto &m : r) res.push_back(m);
            std::reverse(res.begin(), res.end());
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("取最近消息失败 {}-{}: {}", ssid, count, e.what());
        }
        return res;
    }

    /* brief: 取会话内 [start_seq, end_seq] 范围消息（按 seq 升序）
     *  - 客户端按 seq 增量同步的核心查询路径
     */
    std::vector<Message> range_by_seq(const std::string &ssid,
                                      unsigned long start_seq,
                                      unsigned long end_seq)
    {
        std::vector<Message> res;
        if(start_seq > end_seq) return res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<Message>;
            using result = odb::result<Message>;
            result r(_db->query<Message>(
                (query::session_id == ssid &&
                 query::seq_id >= start_seq &&
                 query::seq_id <= end_seq) +
                " ORDER BY seq_id ASC"));
            for(auto &m : r) res.push_back(m);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("按 seq 范围查询失败 {} [{}-{}]: {}", ssid, start_seq, end_seq, e.what());
        }
        return res;
    }

    /* brief: 取会话当前最大 seq（DB 层，最终一致；强一致用 Redis） */
    unsigned long max_seq_of_session(const std::string &ssid) {
        unsigned long max_seq = 0;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<Message>;
            using result = odb::result<Message>;
            result r(_db->query<Message>(
                (query::session_id == ssid) + " ORDER BY seq_id DESC LIMIT 1"));
            auto it = r.begin();
            if(it != r.end()) max_seq = it->seq_id();
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("取会话最大 seq 失败 {}: {}", ssid, e.what());
        }
        return max_seq;
    }

    /* brief: 兼容旧接口 — 按时间范围（保留以平滑过渡，建议改用 range_by_seq） */
    std::vector<Message> range(const std::string &ssid,
                               boost::posix_time::ptime stime,
                               boost::posix_time::ptime etime)
    {
        std::vector<Message> res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<Message>;
            using result = odb::result<Message>;
            result r(_db->query<Message>(
                (query::session_id == ssid &&
                 query::create_time >= stime &&
                 query::create_time <= etime) +
                " ORDER BY seq_id ASC"));
            for(auto &m : r) res.push_back(m);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("按时间范围查询失败 {}: {}", ssid, e.what());
        }
        return res;
    }

    /* brief: 撤回消息（改状态而非删行） */
    bool mark_revoked(unsigned long message_id, const std::string &operator_id) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Message>;
            std::shared_ptr<Message> m(_db->query_one<Message>(query::message_id == message_id));
            if(!m) {
                trans.commit();
                return false;
            }
            m->status(MessageStatus::REVOKED);
            m->revoke_time(boost::posix_time::microsec_clock::universal_time());
            m->revoke_by(operator_id);
            _db->update(*m);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("撤回消息失败 {}: {}", message_id, e.what());
            return false;
        }
        return true;
    }

    /* brief: 软删除（运营 / 风控）— 保留行做审计 */
    bool mark_deleted(unsigned long message_id) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Message>;
            std::shared_ptr<Message> m(_db->query_one<Message>(query::message_id == message_id));
            if(!m) {
                trans.commit();
                return false;
            }
            m->status(MessageStatus::DELETED);
            _db->update(*m);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("软删除消息失败 {}: {}", message_id, e.what());
            return false;
        }
        return true;
    }

    /* brief: 物理删除会话所有消息（仅在群解散且需清理冷数据时使用） */
    bool remove_by_session(const std::string &ssid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<Message>;
            _db->erase_query<Message>(query::session_id == ssid);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("清理会话消息失败 {}: {}", ssid, e.what());
            return false;
        }
        return true;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
