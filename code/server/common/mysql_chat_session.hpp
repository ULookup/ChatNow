#pragma once

#include "logger.hpp"
#include "mysql.hpp"
#include "chat_session.hxx"
#include "chat_session-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

namespace chatnow 
{

class ChatSessionTable
{
public:
    using ptr = std::shared_ptr<ChatSessionTable>;
    ChatSessionTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    bool insert(ChatSession &cs) {
        try {
            odb::transaction trans(_db->begin());

            _db->persist(cs);

            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("新增会话失败 {}: {}", cs.chat_session_name(), e.what());
            return false;
        }
        return true;
    }
    bool remove(const std::string &ssid) {
        try {
            odb::transaction trans(_db->begin());
            
            typedef odb::query<ChatSession> query;
            typedef odb::result<ChatSession> result;
            _db->erase_query<ChatSession>(query::chat_session_id == ssid);

            //step2: 删除会话成员里的对应数据
            typedef odb::query<ChatSessionMember> mquery;
            _db->erase_query<ChatSessionMember>(mquery::session_id == ssid);

            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("删除会话失败 {}: {}", ssid, e.what());
            return false;
        }
        return true;
    }
    /* brief: 单聊会话的删除 -- 根据单聊会话的两个成员 */
    bool remove(const std::string &uid, const std::string &pid) {
        try {
            odb::transaction trans(_db->begin());
            
            typedef odb::query<SingleChatSession> query;
            auto res = _db->query_one<SingleChatSession>(query::csm1::user_id == uid && 
                                                        query::csm2::user_id == pid &&
                                                        query::css::chat_session_type == ChatSessionType::SINGLE);
            std::string cssid = res->chat_session_id;
            //step2: 删除会话成员里的对应数据
            typedef odb::query<ChatSession> cquery;
            _db->erase_query<ChatSession>(cquery::chat_session_id == cssid);
            typedef odb::query<ChatSessionMember> mquery;
            _db->erase_query<ChatSessionMember>(mquery::session_id == cssid);

            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("删除会话失败 {}-{}: {}", uid, pid, e.what());
            return false;
        }
        return true;
    }
    /* brief: 通过会话ID获取会话信息 */
    std::shared_ptr<ChatSession> select(const std::string &ssid) {
        std::shared_ptr<ChatSession> res;
        try {
            odb::transaction trans(_db->begin());

            typedef odb::query<ChatSession> query;
            typedef odb::result<ChatSession> result;
            res.reset(_db->query_one<ChatSession>(query::chat_session_id == ssid));

            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过会话ID获取会话信息失败 {}:{}", ssid, e.what());
        }
        return res;
    }
    /* brief: 通过用户ID获取到他的单聊会话信息 */
    std::vector<SingleChatSession> singleChatSession(const std::string &uid) {
        std::vector<SingleChatSession> res;
        try {
            odb::transaction trans(_db->begin());

            typedef odb::query<SingleChatSession> query;
            typedef odb::result<SingleChatSession> result;
            result r(_db->query<SingleChatSession>(query::css::chat_session_type == ChatSessionType::SINGLE && 
                                                query::csm1::user_id == uid && 
                                                query::csm2::user_id != query::csm1::user_id));
            for(result::iterator i(r.begin()); i != r.end(); ++i) {
                res.push_back(*i);
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("获取用户 {} 单聊会话失败 {}:{}", uid, e.what());
        }
        return res;
    }
    /* brief: 通过用户ID获取到他的群聊会话信息*/
    std::vector<GroupChatSession> groupChatSession(const std::string &uid) {
        std::vector<GroupChatSession> res;
        try {
            odb::transaction trans(_db->begin());

            typedef odb::query<GroupChatSession> query;
            typedef odb::result<GroupChatSession> result;
            result r(_db->query<GroupChatSession>(query::css::chat_session_type == ChatSessionType::GROUP && 
                                                query::csm::user_id == uid));
            for(result::iterator i(r.begin()); i != r.end(); ++i) {
                res.push_back(*i);
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("获取用户 {} 群聊会话失败 {}:{}", uid, e.what());
        }
        return res;
    }
    //===============================================================
    //========================== V2.0 ===============================
    //===============================================================
    /* brief: 判断是否已经存在单聊会话，防止重复创建（幂等性） */
    bool singleExists(std::string &uid, std::string &pid) {
        bool exists = false;
        try {
            odb::transaction trans(_db->begin()); // 获取事务对象，开启事务

            using query  = odb::query<SingleChatSession>;
            using result = odb::result<SingleChatSession>;

            result r(_db->query<SingleChatSession>((query::css::chat_session_type == ChatSessionType::SINGLE &&
                                                    query::csm1::user_id == uid &&
                                                    query::csm2::user_id == pid) +
                                                    " LIMIT 1"));
            exists = (r.begin() != r.end());

            trans.commit(); // 结束事务，提交
        } catch(std::exception &e) {
            LOG_ERROR("判断是否存在单聊会话失败 {}-{}:{}", uid, pid, e.what());
            return false;
        }
        return exists;
    }
    /* brief: 判断会话是否已经存在 */
    bool exists(std::string &ssid) {
        bool found = false;
        try {
            odb::transaction trans(_db->begin()); // 获取事务对象，开启事务

            using query  = odb::query<ChatSession>;
            using result = odb::result<ChatSession>;

            result r(_db->query<ChatSession>((query::chat_session_id == ssid) +
                                            " LIMIT 1"));
            
            found = (r.begin() != r.end());

            trans.commit(); // 提交事务
        } catch(std::exception &e) {
            LOG_ERROR("判断会话是否存在失败 {}:{}", ssid, e.what());
            return false;
        }
        return found;
    }
    /* brief: 更新会话（上层要做好鉴权） */
    bool update(const std::shared_ptr<ChatSession> &chatsession) {
        try {
            using query = odb::query<ChatSession>;

            odb::transaction trans(_db->begin()); // 获取事务对象，开启事务

            _db->update(*chatsession); // 更新表

            trans.commit(); //提交事务
        } catch(std::exception &e) {
            LOG_ERROR("更新会话信息失败: {}:{}", chatsession->chat_session_id(), e.what());
            return false;
        }
        return true;
    }
private:
    std::shared_ptr<odb::core::database> _db;
};

}