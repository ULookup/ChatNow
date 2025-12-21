#include "user.hxx"
#include "user-odb.hxx"
#include "chat_session_member.hxx"
#include "chat_session_member-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>
#include "logger.hpp"

//用户注册/登录，验证码获取，邮箱注册/登录，获取用户信息，用户信息修改
//用信息新增；通过昵称获取用户信息，通过邮箱获取用户信息，通过用户ID获取用户信息，通过多个用户ID获取多个用户信息；信息修改

namespace chatnow
{

class ODBFactory
{
public:
    static std::shared_ptr<odb::core::database> create(const std::string &user,
                                                    const std::string &password,
                                                    const std::string &host,
                                                    const std::string &db,
                                                    const std::string &cset,
                                                    uint16_t port,
                                                    int conn_pool_count) 
    {
       std::unique_ptr<odb::mysql::connection_pool_factory> factory(new odb::mysql::connection_pool_factory(conn_pool_count, 0));
       auto res = std::make_shared<odb::mysql::database>(user, password, db, host, port, "", cset, 0, std::move(factory));
       return res;
    }
};

class UserTable
{
public:
    using ptr = std::shared_ptr<UserTable>;
    
    UserTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}
    bool insert(std::shared_ptr<User> &user) {
        try {
            //获取事务对象，开启事务
            odb::transaction trans(_db->begin());
            _db->persist(*user);
            //提交事务
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("新增用户失败: {}-{}", user->nickname(), e.what());
            return false;
        }
        return true;
    }
    bool update(const std::shared_ptr<User> &user) {
        try {
            //获取事务对象，开启事务
            odb::transaction trans(_db->begin());
            _db->update(*user);
            //提交事务
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新用户失败: {}-{}", user->nickname(), e.what());
            return false;
        }
        return true;
    }
    std::shared_ptr<User> select_by_nickname(const std::string &nickname) {
        std::shared_ptr<User> res;
        try {
            //获取事务对象，开启事务
            odb::transaction trans(_db->begin());
            typedef odb::query<User> query;
            typedef odb::result<User> result;
            res.reset(_db->query_one<User>(query::nickname == nickname));
            //提交事务
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过昵称查询用户失败: {}-{}", nickname, e.what());
        }
        return res;
    }
    std::shared_ptr<User> select_by_mail(const std::string &mail) {
        std::shared_ptr<User> res;
        try {
            //获取事务对象，开启事务
            odb::transaction trans(_db->begin());
            typedef odb::query<User> query;
            typedef odb::result<User> result;
            res.reset(_db->query_one<User>(query::mail == mail));
            //提交事务
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过邮箱查询用户失败: {}-{}", mail, e.what());
        }
        return res;
    }
    std::shared_ptr<User> select_by_id(const std::string &user_id) {
        std::shared_ptr<User> res;
        try {
            //获取事务对象，开启事务
            odb::transaction trans(_db->begin());
            typedef odb::query<User> query;
            typedef odb::result<User> result;
            res.reset(_db->query_one<User>(query::user_id == user_id));
            //提交事务
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过用户ID查询用户失败: {}-{}", user_id, e.what());
        }
        return res;
    }
    std::vector<User> select_multi_users(const std::vector<std::string> &id_list) {
        // selcet * from user where id in ('id1', '1d2', ...)
        std::vector<User> res;
        try {
            //获取事务对象，开启事务
            odb::transaction trans(_db->begin());
            typedef odb::query<User> query;
            typedef odb::result<User> result;
            std::stringstream ss;
            ss << "user_id in (";
            for(const auto &id : id_list) {
                ss << "'"<< id << "',";
            }
            std::string condition = ss.str();
            condition.pop_back();
            condition += ")";

            result r(_db->query<User>(condition));
            for(result::iterator i(r.begin()); i != r.end(); ++i) {
                res.push_back(*i);
            }
            //提交事务
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过多个用户ID查询多个用户失败: {}", e.what());
        }
        return res;
    }
private:
    std::shared_ptr<odb::core::database> _db;
};

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
private:
    std::shared_ptr<odb::core::database> _db;
};

}