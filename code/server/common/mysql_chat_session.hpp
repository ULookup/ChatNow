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
private:
    std::shared_ptr<odb::core::database> _db;
};

}