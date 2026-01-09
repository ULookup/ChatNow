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
            odb::transaction trans(_db->begin()); // 获取事务对象，开启事务
            
            _db->persist(userTimeline);          // 插入（持久化）对象
        
            trans.commit();                       //提交事务
        } catch(std::exception &e) {
            LOG_ERROR("向用户信箱插入新消息失败: {}-{}", userTimeline.message_id(), e.what());
            return false;
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
private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow