#pragma once

#include "infra/logger.hpp"
#include "dao/mysql.hpp"
#include "user_timeline.hxx"
#include "user_timeline-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>
#include <odb/mysql/connection.hxx>
#include <mysql/mysql.h>

#include <memory>
#include <string>
#include <vector>

namespace chatnow
{

/**
 * UserTimeLineTable
 * ------------------------------------------------------------------
 * user_timeline 表的 DAO 封装（写扩散收件箱）。
 *
 * 关键变化（对齐新 schema）：
 *   - 全局增量同步改用 user_seq 而非 message_id
 *     —— 客户端只需保留一个游标即可拉跨会话所有新消息
 *   - 会话维度查询改用 session_seq
 *   - 投递状态从 bool is_read 合并到 deliver_status 枚举
 *   - 新增 mark_delivered / mark_read：按 timeline 行更新投递状态
 *   - latest_user_seq / unread_count_by_seq：客户端起始游标 / 未读计算入口
 *   - 写操作保持"外部事务感知"：消息存储服务的"写消息 + 写 Timeline"是一个事务
 * ------------------------------------------------------------------
 */
class UserTimeLineTable
{
public:
    using ptr = std::shared_ptr<UserTimeLineTable>;
    UserTimeLineTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 单行插入；事务感知 */
    bool insert(UserTimeline &tl) {
        try {
            bool has_external_trans = odb::transaction::has_current();
            std::unique_ptr<odb::transaction> local_trans;
            if(!has_external_trans) {
                local_trans.reset(new odb::transaction(_db->begin()));
            }
            _db->persist(tl);
            if(!has_external_trans) local_trans->commit();
        } catch(std::exception &e) {
            LOG_ERROR("插入 timeline 失败: {}", e.what());
            throw;
        }
        return true;
    }

    /* brief: 批量插入（写扩散核心路径，群聊一条消息扩散 N 行）
     *  - ODB 会自动复用 prepared statement
     *  - 海量群（>1k 成员）建议在调用方按 user_id 分片落不同分片库
     */
    bool insert(std::vector<UserTimeline> &timelines) {
        if(timelines.empty()) return true;
        try {
            bool has_external_trans = odb::transaction::has_current();
            std::unique_ptr<odb::transaction> local_trans;
            if(!has_external_trans) {
                local_trans.reset(new odb::transaction(_db->begin()));
            }
            for(auto &tl : timelines) _db->persist(tl);
            if(!has_external_trans) local_trans->commit();
        } catch(std::exception &e) {
            LOG_ERROR("批量插入 Timeline 失败 count={}: {}", timelines.size(), e.what());
            throw;
        }
        return true;
    }

    /* brief: 全局增量拉取 — 客户端跨会话的统一入口
     *  - 走 idx_user_seq(user_id, user_seq) 索引
     *  - 拉到 user_seq > after_user_seq 的 limit 条；ASC 保证顺序
     */
    std::vector<UserTimeline> list_global_after(const std::string &user_id,
                                                unsigned long after_user_seq,
                                                size_t limit)
    {
        std::vector<UserTimeline> records;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<UserTimeline>;
            using result = odb::result<UserTimeline>;
            result r(_db->query<UserTimeline>(
                (query::user_id == user_id && query::user_seq > after_user_seq) +
                (" ORDER BY " + query::user_seq + " ASC LIMIT " + std::to_string(limit))));
            for(auto &row : r) records.push_back(row);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("全局增量拉取失败 uid={} after={}: {}", user_id, after_user_seq, e.what());
        }
        return records;
    }

    /* brief: 会话内增量拉取（指定会话 seq 之后） */
    std::vector<UserTimeline> list_session_after(const std::string &user_id,
                                                 const std::string &session_id,
                                                 unsigned long after_session_seq,
                                                 size_t limit)
    {
        std::vector<UserTimeline> records;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<UserTimeline>;
            using result = odb::result<UserTimeline>;
            result r(_db->query<UserTimeline>(
                (query::user_id == user_id &&
                 query::session_id == session_id &&
                 query::session_seq > after_session_seq) +
                (" ORDER BY " + query::session_seq + " ASC LIMIT " + std::to_string(limit))));
            for(auto &row : r) records.push_back(row);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("会话增量拉取失败 uid={} ssid={}: {}", user_id, session_id, e.what());
        }
        return records;
    }

    /* brief: 会话内倒翻历史（在某个 seq 之前 N 条） */
    std::vector<UserTimeline> list_session_before(const std::string &user_id,
                                                  const std::string &session_id,
                                                  unsigned long before_session_seq,
                                                  size_t limit)
    {
        std::vector<UserTimeline> records;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<UserTimeline>;
            using result = odb::result<UserTimeline>;
            result r(_db->query<UserTimeline>(
                (query::user_id == user_id &&
                 query::session_id == session_id &&
                 query::session_seq < before_session_seq) +
                (" ORDER BY " + query::session_seq + " DESC LIMIT " + std::to_string(limit))));
            for(auto &row : r) records.push_back(row);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("会话历史拉取失败 uid={} ssid={}: {}", user_id, session_id, e.what());
        }
        std::reverse(records.begin(), records.end()); // 客户端按从旧到新展示
        return records;
    }

    /* brief: 取会话内最近 N 条（首屏渲染） */
    std::vector<UserTimeline> list_session_latest(const std::string &user_id,
                                                  const std::string &session_id,
                                                  size_t limit)
    {
        std::vector<UserTimeline> records;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<UserTimeline>;
            using result = odb::result<UserTimeline>;
            result r(_db->query<UserTimeline>(
                (query::user_id == user_id && query::session_id == session_id) +
                (" ORDER BY " + query::session_seq + " DESC LIMIT " + std::to_string(limit))));
            for(auto &row : r) records.push_back(row);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("会话最近消息拉取失败 uid={} ssid={}: {}", user_id, session_id, e.what());
        }
        std::reverse(records.begin(), records.end());
        return records;
    }

    /* brief: 时间段查询（兼容旧接口；新功能尽量按 seq） */
    std::vector<UserTimeline> range(const std::string &user_id,
                                    const std::string &session_id,
                                    const boost::posix_time::ptime &stime,
                                    const boost::posix_time::ptime &etime)
    {
        std::vector<UserTimeline> records;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<UserTimeline>;
            using result = odb::result<UserTimeline>;
            result r(_db->query<UserTimeline>(
                query::user_id == user_id &&
                query::session_id == session_id &&
                query::message_time >= stime &&
                query::message_time <= etime));
            for(auto &row : r) records.push_back(row);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("时间段查询失败: {}", e.what());
        }
        return records;
    }

    /* brief: 取该用户当前最大 user_seq — 客户端首次连入时用作起始游标
     *  - 也可用于 max(user_seq) 监控告警
     */
    unsigned long latest_user_seq(const std::string &user_id) {
        unsigned long max_seq = 0;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<UserTimeline>;
            using result = odb::result<UserTimeline>;
            result r(_db->query<UserTimeline>(
                (query::user_id == user_id) +
                (" ORDER BY " + query::user_seq + " DESC LIMIT 1")));
            auto it = r.begin();
            if(it != r.end()) max_seq = it->user_seq();
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("取用户最大 user_seq 失败 {}: {}", user_id, e.what());
        }
        return max_seq;
    }

    /* brief: 取该用户在会话内的最大 session_seq */
    unsigned long latest_session_seq(const std::string &user_id, const std::string &session_id) {
        unsigned long max_seq = 0;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<UserTimeline>;
            using result = odb::result<UserTimeline>;
            result r(_db->query<UserTimeline>(
                (query::user_id == user_id && query::session_id == session_id) +
                (" ORDER BY " + query::session_seq + " DESC LIMIT 1")));
            auto it = r.begin();
            if(it != r.end()) max_seq = it->session_seq();
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("取会话最大 session_seq 失败 {}-{}: {}", user_id, session_id, e.what());
        }
        return max_seq;
    }

    /* brief: 计算 last_read_seq 之后的未读条数 */
    int unread_count_by_seq(const std::string &user_id,
                            const std::string &session_id,
                            unsigned long last_read_seq)
    {
        int count = 0;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<CountView>;
            using result = odb::result<CountView>;
            result r(_db->query<CountView>(
                odb::query<UserTimeline>::user_id == user_id &&
                odb::query<UserTimeline>::session_id == session_id &&
                odb::query<UserTimeline>::session_seq > last_read_seq));
            auto it = r.begin();
            if(it != r.end()) count = static_cast<int>(it->count);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("未读计算失败: {}", e.what());
        }
        return count;
    }

    /* brief: 标送达 — 多端送达回执 */
    bool mark_delivered(const std::string &user_id, unsigned long message_id) {
        return _set_status(user_id, message_id, TimelineDeliverStatus::DELIVERED);
    }

    /* brief: 标已读 — 单聊 / 被 @ 等场景 */
    bool mark_read(const std::string &user_id, unsigned long message_id) {
        return _set_status(user_id, message_id, TimelineDeliverStatus::READ);
    }

    /* brief: 物理清理某用户某会话的 Timeline（用户主动"清空聊天记录"） */
    bool remove_by_user_session(const std::string &user_id, const std::string &session_id) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserTimeline>;
            _db->erase_query<UserTimeline>(
                query::user_id == user_id && query::session_id == session_id);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("清理用户会话 Timeline 失败 {}-{}: {}", user_id, session_id, e.what());
            return false;
        }
        return true;
    }

    /* brief: 批量删除 user_timeline 中指定 message_id 的行（仅删调用方自己的） */
    int delete_by_message_ids(const std::string &uid,
                              const std::string &cid,
                              const std::vector<unsigned long> &mids) {
        if(mids.empty()) return 0;
        int n = 0;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserTimeline>;
            for(auto mid : mids) {
                n += static_cast<int>(_db->erase_query<UserTimeline>(
                    query::user_id == uid &&
                    query::session_id == cid &&
                    query::message_id == mid));
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("delete_by_message_ids uid={} cid={} size={}: {}",
                      uid, cid, mids.size(), e.what());
        }
        return n;
    }

    /* brief: 清空 user_timeline 中该会话所有行（仅删调用方自己的） */
    int delete_by_conversation(const std::string &uid, const std::string &cid) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserTimeline>;
            int n = static_cast<int>(_db->erase_query<UserTimeline>(
                query::user_id == uid && query::session_id == cid));
            trans.commit();
            return n;
        } catch(std::exception &e) {
            LOG_ERROR("delete_by_conversation uid={} cid={}: {}", uid, cid, e.what());
            return 0;
        }
    }

    /* brief: 取所有用户的 max(user_seq)，给 SeqGen 启动回填用 */
    std::vector<std::pair<std::string, unsigned long>> select_max_user_seq_per_user() {
        std::vector<std::pair<std::string, unsigned long>> res;
        try {
            odb::transaction trans(_db->begin());
            using view = odb::query<UserTimeline>;
            odb::result<UserTimeline> r(_db->query<UserTimeline>(
                "GROUP BY " + view::user_id));
            std::set<std::string> uids;
            for(auto it = r.begin(); it != r.end(); ++it) uids.insert(it->user_id());
            for(const auto &u : uids) {
                std::shared_ptr<UserTimeline> m(_db->query_one<UserTimeline>(
                    (view::user_id == u) + " ORDER BY " + view::user_seq + " DESC"));
                if(m) res.emplace_back(u, m->user_seq());
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("select_max_user_seq_per_user: {}", e.what());
        }
        return res;
    }

    /* brief: 获取所有用户的最大 user_seq（用于启动回填） */
    std::vector<std::pair<std::string, unsigned long>> select_max_user_seq() {
        std::vector<std::pair<std::string, unsigned long>> res;
        try {
            auto &mysql_db = dynamic_cast<odb::mysql::database&>(*_db);
            auto conn = mysql_db.connection();
            MYSQL* handle = conn->handle();
            if (mysql_query(handle,
                    "SELECT user_id, MAX(user_seq) AS max_seq FROM user_timeline GROUP BY user_id") == 0) {
                MYSQL_RES* result = mysql_store_result(handle);
                if (result) {
                    MYSQL_ROW row;
                    while ((row = mysql_fetch_row(result))) {
                        unsigned long* lengths = mysql_fetch_lengths(result);
                        std::string uid(row[0] ? row[0] : "", row[0] ? lengths[0] : 0);
                        unsigned long max_seq = row[1] ? strtoul(row[1], nullptr, 10) : 0;
                        res.emplace_back(std::move(uid), max_seq);
                    }
                    mysql_free_result(result);
                }
            }
        } catch(std::exception &e) {
            LOG_ERROR("获取所有用户最大user_seq失败: {}", e.what());
        }
        return res;
    }

private:
    bool _set_status(const std::string &user_id, unsigned long message_id, TimelineDeliverStatus s) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserTimeline>;
            std::shared_ptr<UserTimeline> tl(_db->query_one<UserTimeline>(
                query::user_id == user_id && query::message_id == message_id));
            if(!tl) {
                trans.commit();
                return false;
            }
            // 仅允许状态向前推进：PENDING < DELIVERED < READ
            if(static_cast<int>(s) > static_cast<int>(tl->deliver_status())) {
                tl->deliver_status(s);
                _db->update(*tl);
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新 timeline 状态失败 {}-{}: {}", user_id, message_id, e.what());
            return false;
        }
        return true;
    }

    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
