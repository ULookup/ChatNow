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
            _db->persist(csm);
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
        try {
            //获取事务对象，开启事务
            odb::transaction trans(_db->begin());
            for(auto &csm : csm_list) {
                _db->persist(csm);
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
            _db->erase_query<ChatSessionMember>(query::session_id == csm.session_id() && query::user_id == csm.user_id());
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

            for(auto &csm : csm_list) {
                _db->erase_query<ChatSessionMember>(query::session_id == csm.session_id() && query::user_id == csm.user_id());
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
        if (uid_list.empty()) return res;
        try {
            odb::transaction trans(_db->begin()); //获取事务对象，开启事务

            using query  = odb::query<ChatSessionMember>;
            using result = odb::result<ChatSessionMember>;

            // 构造 IN 查询条件
            query q;
            q = query::user_id.in_range(uid_list.begin(), uid_list.end());

            result r(_db->query<ChatSessionMember>(q));
            for (auto &row : r) {
                res.push_back(row);
            }

            trans.commit();// 提交事务
        } catch(std::exception &e) {
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
                << " COALESCE(cs.last_message_time, cs.create_time) DESC";
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
                query::session_id == session_id +
                " ORDER BY "
                " cm._role DESC, "         // 群主(2) > 管理员(1) > 普通(0)
                " cm._join_time ASC "      // 同角色按入群时间排序
            ));


            for (auto& row : r) {
                res.push_back(row);
            }

            trans.commit();
        }
        catch (const std::exception& e) {
            LOG_ERROR("获取会话 {} 成员角色列表失败: {}", session_id, e.what());
        }
        return res;
    }
private:
    std::shared_ptr<odb::core::database> _db;
};

}