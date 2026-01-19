#pragma once

#include "logger.hpp"
#include "mysql.hpp"
#include "user_timeline.hxx"
#include "user_timeline-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

namespace chatnow
{

class UserTimeLineTable
{
public:
    using ptr = std::shared_ptr<UserTimeLineTable>;
    UserTimeLineTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}
    /* brief: 插入（userTimeLine对象) */
    bool insert(UserTimeline &userTimeline) {
        try {
            // 1. 检查外部是否已经开启了事务
            bool has_external_trans = odb::transaction::has_current();

            // 2. 如果外部没有事务，我们才开启一个本地事务；否则只创建一个空的 holder
            // std::unique_ptr 用于管理事务对象的生命周期
            std::unique_ptr<odb::transaction> local_trans;
            
            if (!has_external_trans) {
                // 只有在没事务的时候，才由 DAO 开启
                local_trans.reset(new odb::transaction(_db->begin()));
            }

            // 3. 执行持久化操作 (ODB 会自动挂载到当前线程的活跃事务上)
            _db->persist(userTimeline);

            // 4. 只有当我们自己开启了事务，我们才负责提交
            if (!has_external_trans) {
                local_trans->commit();
            }
            
            // 如果是外部事务，这里什么都不做，等外部 Service 去 commit

        } catch(std::exception &e) {
            LOG_ERROR("插入失败: {}", e.what());
            throw; // 异常会向上传递，导致外部事务回滚
        }
        return true;
    }
    /* brief: 批量插入用户 Timeline (写扩散核心优化) */
    bool insert(std::vector<UserTimeline> &timelines) {
        if (timelines.empty()) {
            return true;
        }

        try {
            // 1. 检查外部是否已经开启了事务
            bool has_external_trans = odb::transaction::has_current();
            // 2. 如果外部没有事务，我们才开启一个本地事务；否则只创建一个空的 holder
            // std::unique_ptr 用于管理事务对象的生命周期
            std::unique_ptr<odb::transaction> local_trans;
            
            if (!has_external_trans) {
                // 只有在没事务的时候，才由 DAO 开启
                local_trans.reset(new odb::transaction(_db->begin()));
            }

            // 2. 循环持久化 (ODB 会自动优化 Prepared Statements)
            for (auto &timeline : timelines) {
                _db->persist(timeline);
            }

            // 4. 只有当我们自己开启了事务，我们才负责提交
            if (!has_external_trans) {
                local_trans->commit();
            }

            // 如果是外部事务，这里什么都不做，等外部 Service 去 commit
        } catch(std::exception &e) {
            LOG_ERROR("批量插入 Timeline 失败: 数量 {}, 错误: {}", timelines.size(), e.what());
            throw;
        }
        return true;
    }
    /* brief: 拉取最近未读消息, session_id 会话发给 user_id 的在 after_message_id 后的 limit 条消息 */
    std::vector<UserTimeline> list_after(const std::string &user_id,
                                        const std::string &session_id,
                                        const unsigned long after_message_id,
                                        size_t limit)
    {
        using query = odb::query<UserTimeline>;
        using result = odb::result<UserTimeline>;

        std::vector<UserTimeline> records;

        try {
            odb::transaction trans(_db->begin()); //获取事务对象，开启事务

            result r(_db->query<UserTimeline>((query::user_id == user_id &&
                                            query::session_id == session_id &&
                                            query::message_id > after_message_id) +
                                            (" ORDER BY " + query::message_id + " ASC" +
                                            " LIMIT " + std::to_string(limit))));
            // 在数据库利用建的复合索引读取出对应的 timeline

            for(auto it = r.begin(); it != r.end(); ++it) {
                records.emplace_back(*it);
            }

            trans.commit(); //提交事务
        } catch(std::exception &e) {
            LOG_ERROR("获取最近未读新消息失败: {}-{}", after_message_id, e.what());
            return std::vector<UserTimeline>();
        }
        
        return records;
    }
    /* brief: 拉取历史消息, session_id 会话发给 user_id 的在 before_message_id 前的 limit 条历史消息 */
    std::vector<UserTimeline> list_before(const std::string &user_id,
                                        const std::string &session_id,
                                        const unsigned long before_message_id,
                                        size_t limit)
    {
        using query = odb::query<UserTimeline>;
        using result = odb::result<UserTimeline>;

        std::vector<UserTimeline> records;

        try {
            odb::transaction trans(_db->begin()); //获取事务对象，开启事务

            result r(_db->query<UserTimeline>((query::user_id == user_id &&
                                            query::session_id == session_id &&
                                            query::message_id < before_message_id) +
                                            (" ORDER BY " + query::message_id + " DESC" +
                                            " LIMIT " + std::to_string(limit))));
            // 在数据库利用建的复合索引读取出对应的 timeline
            for(auto it = r.begin(); it != r.end(); ++it) {
                records.emplace_back(*it);
            }

            trans.commit(); //提交事务
        } catch(std::exception &e) {
            LOG_ERROR("获取历史消息失败: {}-{}", before_message_id, e.what());
            return std::vector<UserTimeline>();
        }

        std::reverse(records.begin(), records.end());  // 这里需要反转一下，因为SQL查出来是新的前面，而客户端需要新消息追加在后面

        return records;
    }
    /* brief: 拉取最近消息, session_id 会话发给 user_id 的离当前时间最近的 limit 条消息 */
    std::vector<UserTimeline> list_latest(const std::string &user_id,
                                        const std::string &session_id,
                                        size_t limit)
    {
        using query = odb::query<UserTimeline>;
        using result = odb::result<UserTimeline>;

        std::vector<UserTimeline> records;

        try {
            odb::transaction trans(_db->begin()); //获取事务对象，开启事务

            result r(_db->query<UserTimeline>((query::user_id == user_id &&
                                            query::session_id == session_id) +
                                            (" ORDER BY " + query::message_id + " DESC" +
                                            " LIMIT " + std::to_string(limit))));
            // 在数据库利用建的复合索引读取出对应的 timeline
            for(auto it = r.begin(); it != r.end(); ++it) {
                records.emplace_back(*it);
            }                           
            
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("获取最近消息失败: {}", e.what());
            return std::vector<UserTimeline>();
        }

        std::reverse(records.begin(), records.end()); //这次 DESC 从最后的数据拿 140 139 138... 方便索引命中, 因此需要反转一次保证顺序

        return records;
    }
    /* brief: 判断timeline是否覆盖某个区间, 后期如果某时间点后的冷数据归档了，就需要降级从消息表取 */
    bool exists(const std::string &user_id,
                const std::string &session_id,
                const unsigned long message_id)
    {
        using query = odb::query<UserTimeline>;
        using result = odb::result<UserTimeline>;

        std::vector<UserTimeline> records;
        bool found = false;

        try{
            odb::transaction trans(_db->begin()); //获取事务对象，开启事务

            result r(_db->query<UserTimeline>((query::user_id == user_id &&
                                            query::session_id == session_id &&
                                            query::message_id == message_id) + " LIMIT 1"));
            
            found = r.begin() != r.end();

            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("判断 timeline 是否覆盖某个区间失败: {}", e.what());
            return false;
        }

        return found;
    }
    std::vector<UserTimeline> range(const std::string &user_id,
                                const std::string &session_id,
                                const boost::posix_time::ptime &stime,
                                const boost::posix_time::ptime &etime)
    {
        using query = odb::query<UserTimeline>;
        using result = odb::result<UserTimeline>;
        std::vector<UserTimeline> records;

        try {
            odb::transaction trans(_db->begin());
            
            // 查询条件：指定用户 + 指定会话 + 时间在 [stime, etime] 之间
            // 注意：这里利用了 UserTimeline 对象里的 _message_time 字段
            result r(_db->query<UserTimeline>(
                query::user_id == user_id &&
                query::session_id == session_id &&
                query::message_time >= stime &&
                query::message_time <= etime
            ));

            for (auto it = r.begin(); it != r.end(); ++it) {
                records.emplace_back(*it);
            }
            trans.commit();
        } catch (std::exception &e) {
            LOG_ERROR("获取时间段Timeline失败: {} - {}: {}", 
                boost::posix_time::to_simple_string(stime), 
                boost::posix_time::to_simple_string(etime), e.what());
        }
        return records;
    }
    /* brief: 全局增量拉取。获取该用户(跨所有会话)在 after_message_id 之后的消息 */
    std::vector<UserTimeline> list_global_after(const std::string &user_id,
                                                unsigned long after_message_id,
                                                size_t limit)
    {
        using query = odb::query<UserTimeline>;
        using result = odb::result<UserTimeline>;
        std::vector<UserTimeline> records;

        try {
            odb::transaction trans(_db->begin());

            // 核心查询：只看 user_id，忽略 session_id
            // 必须按 message_id 升序 (ASC)，因为是拉取“新”消息
            result r(_db->query<UserTimeline>(
                (query::user_id == user_id && query::message_id > after_message_id) +
                (" ORDER BY " + query::message_id + " ASC" +
                 " LIMIT " + std::to_string(limit))
            ));

            for(auto it = r.begin(); it != r.end(); ++it) {
                records.emplace_back(*it);
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("全局增量拉取失败: uid={}, last_id={}, err={}", user_id, after_message_id, e.what());
        }
        return records;
    }
    // 【新增】获取该会话中最新的消息 ID
    unsigned long get_latest_msg_id(const std::string &user_id, const std::string &session_id) {
        using query = odb::query<LatestIdView>;
        using result = odb::result<LatestIdView>;
        
        unsigned long max_id = 0;
        try {
            odb::transaction trans(_db->begin());
            // 查询：user_id + session_id 下的最大 message_id
            result r(_db->query<LatestIdView>(
                odb::query<UserTimeline>::user_id == user_id && 
                odb::query<UserTimeline>::session_id == session_id
            ));
            
            if (!r.empty()) {
                auto v = *r.begin();
                if (!v.max_id.null()) {
                    max_id = *v.max_id;
                }
            }
            trans.commit();
        } catch (std::exception &e) {
            LOG_ERROR("获取最新ID失败: {}", e.what());
        }
        return max_id;
    }

    // 【新增】统计 last_read_id 之后的消息数量
    int get_unread_count(const std::string &user_id, 
                         const std::string &session_id, 
                         unsigned long last_read_id) 
    {
        using query = odb::query<CountView>;
        using result = odb::result<CountView>;

        int count = 0;
        try {
            odb::transaction trans(_db->begin());
            // 查询：user_id + session_id 且 message_id > last_read_id 的数量
            result r(_db->query<CountView>(
                odb::query<UserTimeline>::user_id == user_id && 
                odb::query<UserTimeline>::session_id == session_id &&
                odb::query<UserTimeline>::message_id > last_read_id
            ));

            if (!r.empty()) {
                count = (int)r.begin()->count;
            }
            trans.commit();
        } catch (std::exception &e) {
            LOG_ERROR("获取未读数失败: {}", e.what());
        }
        return count;
    }
private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow