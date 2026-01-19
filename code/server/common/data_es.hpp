#pragma once

#include "icsearch.hpp"
#include "user.hxx"
#include "message.hxx"
#include "chat_session.hxx"

namespace chatnow
{

class ESClientFactory
{
public:
    static std::shared_ptr<elasticlient::Client> create(const std::vector<std::string> host_list) {
        return std::make_shared<elasticlient::Client>(host_list);
    }
};

class ESUser
{
public:
    using ptr = std::shared_ptr<ESUser>;

    ESUser(const std::shared_ptr<elasticlient::Client> &client) : _client(client) {}
    bool create_index() {
        bool ret = ESIndex(_client, "user")
            .append("user_id", "keyword", "standard", true)
            .append("nickname")
            .append("mail", "keyword", "standard", true)
            .append("description", "text", "standard", false)
            .append("avatar_id", "keyword", "standard", false)
            .create();
        if(ret == false) {
            LOG_ERROR("用户信息索引创建失败");
            return false;
        }
        LOG_INFO("用户信息索引创建成功");
        return true;
    }
    bool append_data(const std::string &uid, 
                    const std::string &mail, 
                    const std::string &nickname,
                    const std::string &description,
                    const std::string &avatar_id) {
        auto ret = ESInsert(_client, "user")
            .append("user_id", uid)
            .append("nickname", nickname)
            .append("mail", mail)
            .append("description", description)
            .append("avatar_id", avatar_id)
            .insert(uid);
        if(ret == false) {
            LOG_ERROR("用户数据插入/更新失败");
            return false;
        }
        LOG_INFO("用户数据新增/更新成功");
        return true;
    }
    std::vector<User> search(const std::string &key, const std::vector<std::string> &uid_list) {
        std::vector<User> res;
        Json::Value json_user = ESSearch(_client, "user")
            .append_should_match("mail.keyword", key)
            .append_should_match("user_id.keyword", key)
            .append_should_match("nickname", key)
            .append_must_not_terms("user_id.keyword", uid_list)
            .search();
        if(json_user.isArray() == false) {
            LOG_ERROR("用户搜索结构不是数组类型");
            return res;
        }
        int sz = json_user.size();
        LOG_DEBUG("检索结果条目数量: {}", sz);
        for(int i = 0; i < sz; ++i) {
            User user;
            user.user_id(json_user[i]["_source"]["user_id"].asString());
            user.nickname(json_user[i]["_source"]["nickname"].asString());
            user.description(json_user[i]["_source"]["description"].asString());
            user.mail(json_user[i]["_source"]["mail"].asString());
            user.avatar_id(json_user[i]["_source"]["avatar_id"].asString());
            res.push_back(user);
        }
        return res;
    }
private:
    std::shared_ptr<elasticlient::Client> _client;
};

class ESMessage
{
public:
    using ptr = std::shared_ptr<ESMessage>;
    ESMessage(const std::shared_ptr<elasticlient::Client> &client) : _client(client) {}
    
    bool createIndex() {
        bool ret = ESIndex(_client, "message")
            .append("user_id", "keyword", "standard", false)
            .append("message_id", "long", "standard", false)
            .append("create_time", "long", "standard", false)
            .append("chat_session_id", "keyword", "standard", true)
            .append("content")
            .create();
        if(ret == false) {
            LOG_INFO("消息信息索引创建失败");
            return false;
        }
        LOG_INFO("消息信息索引创建成功");
        return true;
    }

    bool appendData(const std::string &user_id,
                    unsigned long message_id,
                    const long create_time,
                    const std::string &chat_session_id,
                    const std::string &content)
    {
        bool ret = ESInsert(_client, "message")
            .append("user_id", user_id)
            .append("message_id", message_id)
            .append("create_time", create_time)
            .append("chat_session_id", chat_session_id)
            .append("content", content)
            .insert(std::to_string(message_id));
        if(ret == false) {
            LOG_ERROR("消息数据插入/更新失败");
            return false;
        }
        LOG_INFO("消息数据新增/更新成功");
        return true;
    }

    bool remove(unsigned long mid) {
        bool ret = ESRemove(_client, "message").remove(std::to_string(mid));
        if(ret == false) {
            LOG_ERROR("消息删除失败");
            return false;
        }
        LOG_INFO("消息删除成功");
        return true;
    }

    std::vector<Message> search(const std::string &key, const std::string &ssid) {
        std::vector<Message> res;
        Json::Value json_message = ESSearch(_client, "message")
            .append_must_term("chat_session_id.keyword", ssid)
            .append_must_match("content", key)
            .search();
        if(json_message.isArray() == false) {
            LOG_ERROR("用户搜索结果为空，或者结果不是数组类型");
            return res;
        }
        int sz = json_message.size();
        LOG_DEBUG("检索结果条目数量: {}", sz);
        for(int i = 0; i < sz; ++i) {
            Message message;
            message.user_id(json_message[i]["_source"]["user_id"].asString());
            message.message_id(json_message[i]["_source"]["message_id"].asUInt64());
            boost::posix_time::ptime ctime(boost::posix_time::from_time_t(json_message[i]["_source"]["create_time"].asInt64()));
            message.create_time(ctime);
            message.session_id(json_message[i]["_source"]["chat_session_id"].asString());
            message.content(json_message[i]["_source"]["content"].asString());
            res.push_back(message);
        }
        return res;
    }
private:
    std::shared_ptr<elasticlient::Client> _client;
};

class ESChatSession
{
public:
    using ptr = std::shared_ptr<ESChatSession>;
    ESChatSession(const std::shared_ptr<elasticlient::Client> &client)
        : _client(client) {}

    bool create_index() {
        bool ret = ESIndex(_client, "chat_session")
            .append("chat_session_id", "keyword", "standard", true)
            .append("chat_session_name")                 // text + standard analyzer
            .append("chat_session_type", "integer", "standard", false)
            .append("avatar_id", "keyword", "standard", false)
            .append("status", "integer", "standard", false)
            .create();

        if(!ret) {
            LOG_ERROR("会话搜索索引创建失败");
            return false;
        }
        LOG_INFO("会话搜索索引创建成功");
        return true;
    }

    bool append_data(const chatnow::ChatSession &cs) {
        bool ret = ESInsert(_client, "chat_session")
            .append("chat_session_id", cs.chat_session_id())
            .append("chat_session_name", cs.chat_session_name())
            .append("chat_session_type", static_cast<int>(cs.chat_session_type()))
            .append("avatar_id", cs.avatar_id())
            .append("status", cs.status())
            .insert(cs.chat_session_id());

        if(!ret) {
            LOG_ERROR("会话搜索数据插入/更新失败");
            return false;
        }
        LOG_INFO("会话搜索数据新增/更新成功");
        return true;
    }

    std::vector<std::string> search(const std::string &key, std::optional<chatnow::ChatSessionType> type = std::nullopt) {
        std::vector<std::string> res;

        auto search_builder = ESSearch(_client, "chat_session")
            .append_must_match("chat_session_name", key)
            .append_must_term("status", std::to_string(0));

        if (type.has_value()) {
            search_builder.append_must_term(
                "chat_session_type",
                std::to_string(static_cast<int>(type.value()))
            );
        }

        Json::Value json_session = search_builder.search();

        if (!json_session.isArray()) {
            LOG_ERROR("会话搜索结果不是数组");
            return res;
        }

        for (int i = 0; i < json_session.size(); ++i) {
            res.push_back(
                json_session[i]["_source"]["chat_session_id"].asString()
            );
        }

        return res;
    }


private:
    std::shared_ptr<elasticlient::Client> _client;
};


} // namespace chatnow