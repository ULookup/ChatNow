#pragma once

#include "logger.hpp"
#include "mysql.hpp"
#include "chat_session_member.hxx"
#include "chat_session_member-odb.hxx"
#include "chat_session_view.hxx"
#include "chat_session_view-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

namespace chatnow
{

// ========================== 消息转发-会话成员表 ========================== //
class ChatSessionMemberTable
{
public:
    using ptr = std::shared_ptr<ChatSessionMemberTable>;
    ChatSessionMemberTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}
    /* brief: 向指定会话添加单个成员 ------ ssid & uid */
    bool append(ChatSessionMember &csm) {
        try {
            //获取事务对象，开启事务
            odb::transaction trans(_db->begin());
            //1. 插入成员记录
            _db->persist(csm);
            //2. 更新会话人数
            _update_session_member_count(csm.session_id(), 1);
            //提交事务
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("新增单会话成员失败: {}:{} - {}", csm.session_id(), csm.user_id(), e.what());
            return false;
        }
        return true;
    }
    /* brief: 向指定会话添加多个成员 ------ ssid & uids */
    bool append(std::vector<ChatSessionMember> &csm_list) {
        if(csm_list.empty()) return true;
        try {
            //获取事务对象，开启事务
            odb::transaction trans(_db->begin());

            std::map<std::string, int> session_delta;

            //1. 批量插入
            for(auto &csm : csm_list) {
                _db->persist(csm);
                session_delta[csm.session_id()]++;
            }

            //2. 批量更新计数
            for(const auto &[ssid, count] : session_delta) {
                _update_session_member_count(ssid, count);
            }

            //提交事务
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("新增多会话成员失败: {}:{} - {}", csm_list[0].session_id(), csm_list.size(), e.what());
            return false;
        }
        return true;
    }
    /* brief: 移除指定会话单个成员 ------ ssid & uid */
    bool remove(ChatSessionMember &csm) {
        try {
            //获取事务对象，开启事务
            odb::transaction trans(_db->begin());

            typedef odb::query<ChatSessionMember> query;
            typedef odb::result<ChatSessionMember> result;

            unsigned long long deleted = _db->erase_query<ChatSessionMember>(query::session_id == csm.session_id() && query::user_id == csm.user_id());

            // 2. 如果真的删除了成员，才更新计数 (-1)
            if (deleted > 0) {
                _update_session_member_count(csm.session_id(), -1);
            } else {
                LOG_WARN("尝试移除不存在的成员，跳过计数更新: {}:{}", csm.session_id(), csm.user_id());
            }

            //提交事务
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("删除指定会话成员失败: {}:{} - {}", csm.session_id(), csm.user_id(), e.what());
            return false;
        }
        return true;
    }
    /* brief: 移除指定会话多个成员 ------ ssid & uid */
    bool remove(std::vector<ChatSessionMember> &csm_list) {
        try {
            //获取事务对象，开启事务
            odb::transaction trans(_db->begin());
            typedef odb::query<ChatSessionMember> query;
            typedef odb::result<ChatSessionMember> result;

            std::map<std::string, int> session_delta;

            //1. 循环删除
            for(auto &csm : csm_list) {
                unsigned long long deleted = _db->erase_query<ChatSessionMember>(query::session_id == csm.session_id() && query::user_id == csm.user_id());
                if(deleted > 0) {
                    session_delta[csm.session_id()]++;
                }
            }
            //2. 批量更新计数
            for(const auto &[ssid, count] : session_delta) {
                _update_session_member_count(ssid, -count);
            }
            //提交事务
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("批量删除指定会话成员失败: {}", e.what());
            return false;
        }
        return true;
    }
    /* brief: 移除指定会话所有信息 ------ ssid */
    bool remove(const std::string &ssid) {
        try {
            //获取事务对象，开启事务
            odb::transaction trans(_db->begin());
            typedef odb::query<ChatSessionMember> query;
            typedef odb::result<ChatSessionMember> result;
            _db->erase_query<ChatSessionMember>(query::session_id == ssid);
            //提交事务
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("删除会话失败: {} - {}", ssid, e.what());
            return false;
        }
        return true;
    }
    /* brief: 获取会话所有成员 */
    std::vector<std::string> members(const std::string &ssid) {
        std::vector<std::string> res;
        try {
            //获取事务对象，开启事务
            odb::transaction trans(_db->begin());
            typedef odb::query<ChatSessionMember> query;
            typedef odb::result<ChatSessionMember> result;

            result r(_db->query<ChatSessionMember>(query::session_id == ssid));
            for(result::iterator i(r.begin()); i != r.end(); ++i) {
                res.push_back(i->user_id());
            }
            //提交事务
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("获取会话成员失败: {} - {}", ssid, e.what());
        }
        return res;
    }
    //=======================================================================================
    //======================================= V2.0 ==========================================
    //=======================================================================================
    /* brief: 批量查询会话成员 */
    std::vector<ChatSessionMember> list(const std::vector<std::string> &uid_list) {
        std::vector<ChatSessionMember> res;
        if(uid_list.empty()) {
            return res;
        }

        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<ChatSessionMember>;
            using result = odb::result<ChatSessionMember>;

            std::stringstream ss;
            ss << "user_id in (";
            for(const auto &uid : uid_list) {
                ss << "'" << uid << "',";
            }

            std::string condition = ss.str();
            condition.pop_back();  // 去掉最后一个逗号
            condition += ")";

            result r(_db->query<ChatSessionMember>(condition));
            for(auto &row : r) {
                res.push_back(row);
            }

            trans.commit();
        }
        catch (const std::exception &e) {
            LOG_ERROR("批量查询会话成员失败: {}", e.what());
        }

        return res;
    }
    /* brief: 查找某会话是否存在某用户 */
    bool exists(const std::string &ssid, const std::string &uid) {
        bool found = false;
        try {
            odb::transaction trans(_db->begin()); // 获取事务对象，开启事务

            using query  = odb::query<ChatSessionMember>;
            using result = odb::result<ChatSessionMember>;

            result r(_db->query<ChatSessionMember>((query::session_id == ssid && query::user_id == uid) +
                                                    " LIMIT 1"));
            
            found = (r.begin() != r.end());

            trans.commit(); // 提交事务
        } catch(std::exception &e) {
            LOG_ERROR("判断会话成员是否存在失败 {}-{}:{}", ssid, uid, e.what());
            return false;
        }
        return found;
    }
    /* brief: 批量查询会话里的成员 */
    std::vector<ChatSessionMember> select(const std::string& ssid, const std::vector<std::string>& uids) {
        std::vector<ChatSessionMember> res;
        // 如果传入列表为空，直接返回，防止生成错误的 SQL
        if (uids.empty()) {
            return res;
        }
        try {
            odb::transaction trans(_db->begin());

            using query = odb::query<ChatSessionMember>;

            // 利用 in_range 生成 WHERE IN 语句
            // SQL: SELECT * FROM chat_session_member WHERE session_id = ? AND user_id IN (?, ?, ...)
            auto result = _db->query<ChatSessionMember>(
                query::session_id == ssid && 
                query::user_id.in_range(uids.begin(), uids.end())
            );

            for (auto& row : result) {
                res.push_back(row);
            }

            trans.commit();
        }
        catch (const std::exception& e) {
            LOG_ERROR("批量获取会话成员失败 ssid:{} count:{} - {}", ssid, uids.size(), e.what());
        }
        
        // 调试日志
        LOG_DEBUG("批量查询期望获取 {} 人，实际获取 {} 人", uids.size(), res.size());
        
        return res;
    }
    /* brief: 查询会话成员 */
    std::shared_ptr<ChatSessionMember> select(const std::string &ssid, const std::string &uid) {
        std::shared_ptr<ChatSessionMember> res;
        try {
            odb::transaction trans(_db->begin()); // 获取事务对象，开启事务

            using query = odb::query<ChatSessionMember>;
            using result = odb::result<ChatSessionMember>;

            res.reset(_db->query_one<ChatSessionMember>(query::session_id == ssid && query::user_id == uid));

            trans.commit(); // 提交事务
        } catch(std::exception &e) {
            LOG_ERROR("查询会话用户失败: {}-{}:{}", ssid, uid, e.what());
        }
        return res;
    }
    /* brief: 更新会话成员 */
    bool update(const std::shared_ptr<ChatSessionMember> &csm) {
        try {
            odb::transaction trans(_db->begin());// 获取事务对象，开启事务

            _db->update(*csm);

            trans.commit(); // 提交事务
        } catch(std::exception &e) {
            LOG_ERROR("更新会话成员信息失败 {}-{}:{}", csm->session_id(), csm->user_id(), e.what());
            return false;
        }
        return true;
    }
    /* brief: 批量更新会话成员 */
    bool update(const std::vector<std::shared_ptr<ChatSessionMember>> &csm_list) {
        if(csm_list.size() == 0) {
            LOG_WARN("传入的数组为空");
            return true;
        }
        try {
            odb::transaction trans(_db->begin());

            for(auto &csm : csm_list) {
                _db->update(*csm);
            }

            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("批量更新会话成员信息失败: {}", e.what());
            return false;
        }
        return true;
    }
    /* brief: 根据用户ID按照置顶、最近消息时间的顺序取出会话列表 */
    std::vector<OrderedChatSessionView> list_ordered_by_user(const std::string &uid)
    {
        std::vector<OrderedChatSessionView> res;
        try {
            odb::transaction trans(_db->begin());

            using query  = odb::query<OrderedChatSessionView>;
            using result = odb::result<OrderedChatSessionView>;

            std::ostringstream oss;
            oss << "cm.user_id = '" << uid << "'"
                << " AND cm.visible = true "
                << " ORDER BY "
                << " cm.pin_time IS NOT NULL DESC, "
                << " cm.pin_time DESC, "
                << " COALESCE(cs.last_message_time, cs.create_time) DESC"
                << " LIMIT 50";
            /* 排序规则: 置顶(按置顶时间排序) -> 非置顶(新创建且没消息的会话) -> 非置顶(按最近消息的时间排序) */

            result r(_db->query<OrderedChatSessionView>(oss.str()));

            for (auto &e : r) {
                res.push_back(e);
            }

            trans.commit();
        } catch (std::exception &e) {
            LOG_ERROR("获取用户 {} 排序会话列表失败: {}", uid, e.what());
        }
        return res;
    }
    /* brief: 读取会话成员列表 */
    std::vector<ChatSessionMemberRoleView> list_member_roles(const std::string& session_id)
    {
        std::vector<ChatSessionMemberRoleView> res;
        try {
            odb::transaction trans(_db->begin());

            using query  = odb::query<ChatSessionMemberRoleView>;
            using result = odb::result<ChatSessionMemberRoleView>;

            result r(_db->query<ChatSessionMemberRoleView>(
                (query::session_id == session_id) +
                " ORDER BY "
                " cm.role DESC, "         // 群主(2) > 管理员(1) > 普通(0)
                " cm.join_time ASC "      // 同角色按入群时间排序
            ));

            for (auto& row : r) {
                res.push_back(row);
            }
            LOG_DEBUG("查询到 {} 名成员", res.size());

            trans.commit();
        }
        catch (const std::exception& e) {
            LOG_ERROR("获取会话 {} 成员角色列表失败: {}", session_id, e.what());
        }
        LOG_DEBUG("获取到 {} 名会话成员", res.size());
        return res;
    }
    bool update_last_read_msg(const std::string &user_id, 
                              const std::string &session_id, 
                              unsigned long new_msg_id) 
    {
        try {
            odb::transaction trans(_db->begin());
            
            // 1. 先查询当前值，防止“回滚”（比如用户在另一台设备看了旧消息）
            using query = odb::query<ChatSessionMember>;
            auto result = _db->query_one<ChatSessionMember>(
                query::user_id == user_id && query::session_id == session_id
            );

            if (result) {
                // 只有当新 ID 比旧 ID 大时才更新
                if (result->last_read_msg() < new_msg_id) {
                    result->last_read_msg(new_msg_id);
                    _db->update(*result);
                }
            } else {
                // 极端情况：成员关系不存在
                return false; 
            }
            
            trans.commit();
            return true;
        } catch (std::exception &e) {
            LOG_ERROR("更新已读游标失败: uid={}, session={}, err={}", user_id, session_id, e.what());
            return false;
        }
    }
private:
    void _update_session_member_count(const std::string& ssid, int delta) {
        using SessionQuery = odb::query<ChatSession>;
        
        // 1. 查询并锁定会话行 (FOR UPDATE)
        // 关键点：使用 FOR UPDATE 锁住这行记录，直到事务提交，防止其他人同时修改计数
        // 注意：这里必须加括号 (query == ssid)，否则会因为优先级问题导致查询失败
        std::shared_ptr<ChatSession> session(
            _db->query_one<ChatSession>(
                (SessionQuery::chat_session_id == ssid) + " FOR UPDATE"
            )
        );

        if (session) {
            // 2. 计算新数量
            int new_count = session->member_count() + delta;
            if (new_count < 0) new_count = 0; // 防御性保护，防止负数
            
            // 3. 更新内存对象
            session->member_count(new_count);

            // 4. 更新数据库
            _db->update(*session);
            
            LOG_DEBUG("会话[{}] 人数变更: {} -> {}", ssid, new_count - delta, new_count);
        } else {
            // 如果找不到会话，通常意味着逻辑错误（给不存在的群加人），可以选择抛错让事务回滚
            std::string err = "更新计数失败，未找到会话: " + ssid;
            LOG_ERROR(err);
            throw std::runtime_error(err);
        }
    }
private:
    std::shared_ptr<odb::core::database> _db;
};

}