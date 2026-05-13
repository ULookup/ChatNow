#pragma once

/**
 * ===========================================================================
 * 业务级 ES 索引封装
 * ---------------------------------------------------------------------------
 * 当前 3 个索引（与 MySQL 主表对齐）：
 *   - user         用户搜索（昵称模糊 / 邮箱精确 / 用户ID 精确）
 *   - message      消息全文检索（仅文本消息进入索引）
 *   - chat_session 群名称模糊搜索 / 类型筛选
 *
 * 设计要点（在原版基础上的修订）：
 *   1. ESUser 增补 phone / status 字段，匹配新 schema
 *   2. ESMessage 增补 seq_id / status 字段，对齐 message 表结构变化；
 *      搜索结果反序列化时也写回这些字段
 *   3. ES status 0 = NORMAL（正常）；REVOKED/DELETED 不参与全文检索
 *      append_data() 写入前应在调用方判断 status
 *   4. search() 全部支持 size 限制，避免一次性全量返回
 *   5. ESChatSession 索引补 update_time 用于按变更时间排序
 *   6. 旧 createIndex 与 create_index 命名混用 — 统一改 create_index
 * ===========================================================================
 */

#include "icsearch.hpp"
#include "user.hxx"
#include "message.hxx"
#include "chat_session.hxx"
#include <optional>

namespace chatnow
{

class ESClientFactory
{
public:
    static std::shared_ptr<elasticlient::Client> create(const std::vector<std::string> host_list) {
        return std::make_shared<elasticlient::Client>(host_list);
    }
};

// =============================================================================
// 用户索引：昵称模糊 / 邮箱手机号精确 / 用户ID 精确
// =============================================================================
class ESUser
{
public:
    using ptr = std::shared_ptr<ESUser>;
    explicit ESUser(const std::shared_ptr<elasticlient::Client> &client) : _client(client) {}

    bool create_index() {
        bool ret = ESIndex(_client, "user")
            .append("user_id",     "keyword", "standard", true)
            .append("nickname")
            .append("mail",        "keyword", "standard", true)
            .append("phone",       "keyword", "standard", true)
            .append("description", "text",    "standard", false)
            .append("avatar_id",   "keyword", "standard", false)
            .append("status",      "integer", "standard", false)
            .create();
        if(!ret) {
            LOG_ERROR("用户信息索引创建失败");
            return false;
        }
        LOG_INFO("用户信息索引创建成功");
        return true;
    }

    /* brief: upsert 用户搜索数据；登录或资料修改时调用 */
    bool append_data(const std::string &uid,
                     const std::string &mail,
                     const std::string &phone,
                     const std::string &nickname,
                     const std::string &description,
                     const std::string &avatar_id,
                     int status = 0)
    {
        bool ret = ESInsert(_client, "user")
            .append("user_id",     uid)
            .append("nickname",    nickname)
            .append("mail",        mail)
            .append("phone",       phone)
            .append("description", description)
            .append("avatar_id",   avatar_id)
            .append("status",      status)
            .insert(uid);
        if(!ret) {
            LOG_ERROR("用户数据插入/更新失败 uid={}", uid);
            return false;
        }
        return true;
    }

    /* brief: 模糊搜索；排除已知用户 ID（自己 + 已是好友） */
    std::vector<User> search(const std::string &key,
                             const std::vector<std::string> &uid_list,
                             int size = 20)
    {
        std::vector<User> res;
        Json::Value json_user = ESSearch(_client, "user")
            .append_should_match("mail.keyword", key)
            .append_should_match("phone.keyword", key)
            .append_should_match("user_id.keyword", key)
            .append_should_match("nickname", key)
            .append_must_not_terms("user_id.keyword", uid_list)
            .page(0, size)
            .search();
        if(!json_user.isArray()) return res;

        for(int i = 0; i < (int)json_user.size(); ++i) {
            const auto &src = json_user[i]["_source"];
            User u;
            u.user_id(src["user_id"].asString());
            u.nickname(src["nickname"].asString());
            u.description(src["description"].asString());
            u.mail(src["mail"].asString());
            u.phone(src["phone"].asString());
            u.avatar_id(src["avatar_id"].asString());
            res.push_back(u);
        }
        return res;
    }

private:
    std::shared_ptr<elasticlient::Client> _client;
};

// =============================================================================
// 消息全文索引（仅文本消息进入；状态 != NORMAL 不索引）
// =============================================================================
class ESMessage
{
public:
    using ptr = std::shared_ptr<ESMessage>;
    explicit ESMessage(const std::shared_ptr<elasticlient::Client> &client) : _client(client) {}

    bool create_index() {
        bool ret = ESIndex(_client, "message")
            .append("user_id",         "keyword", "standard", false)
            .append("message_id",      "long",    "standard", false)
            .append("seq_id",          "long",    "standard", false)
            .append("create_time",     "long",    "standard", false)
            .append("chat_session_id", "keyword", "standard", true)
            .append("content")    // 文本消息内容，参与中文分词
            .append("status",          "integer", "standard", false)
            .create();
        if(!ret) {
            LOG_ERROR("消息索引创建失败");
            return false;
        }
        LOG_INFO("消息索引创建成功");
        return true;
    }
    /* brief: 兼容旧名 */
    bool createIndex() { return create_index(); }

    bool appendData(const std::string &user_id,
                    unsigned long message_id,
                    unsigned long seq_id,
                    long create_time_seconds,
                    const std::string &chat_session_id,
                    const std::string &content,
                    int status = 0)
    {
        bool ret = ESInsert(_client, "message")
            .append("user_id",         user_id)
            .append("message_id",      message_id)
            .append("seq_id",          seq_id)
            .append("create_time",     create_time_seconds)
            .append("chat_session_id", chat_session_id)
            .append("content",         content)
            .append("status",          status)
            .insert(std::to_string(message_id));
        if(!ret) {
            LOG_ERROR("消息数据插入/更新失败 mid={}", message_id);
            return false;
        }
        return true;
    }

    bool remove(unsigned long mid) {
        return ESRemove(_client, "message").remove(std::to_string(mid));
    }

    /* brief: 关键字搜索某会话内的文本消息 */
    std::vector<Message> search(const std::string &key, const std::string &ssid, int size = 50) {
        std::vector<Message> res;
        Json::Value json_msg = ESSearch(_client, "message")
            .append_must_term("chat_session_id.keyword", ssid)
            .append_must_match("content", key)
            .append_must_term("status", std::to_string(0))      // 仅 NORMAL
            .sort_by("create_time", "desc")
            .page(0, size)
            .search();
        if(!json_msg.isArray()) return res;

        for(int i = 0; i < (int)json_msg.size(); ++i) {
            const auto &src = json_msg[i]["_source"];
            Message m;
            m.user_id(src["user_id"].asString());
            m.message_id(src["message_id"].asUInt64());
            m.seq_id(src["seq_id"].asUInt64());
            boost::posix_time::ptime ct(boost::posix_time::from_time_t(src["create_time"].asInt64()));
            m.create_time(ct);
            m.session_id(src["chat_session_id"].asString());
            m.content(src["content"].asString());
            res.push_back(m);
        }
        return res;
    }

private:
    std::shared_ptr<elasticlient::Client> _client;
};

// =============================================================================
// 群名称索引（搜索群、按类型筛选）
// =============================================================================
class ESChatSession
{
public:
    using ptr = std::shared_ptr<ESChatSession>;
    explicit ESChatSession(const std::shared_ptr<elasticlient::Client> &client) : _client(client) {}

    bool create_index() {
        bool ret = ESIndex(_client, "chat_session")
            .append("chat_session_id",   "keyword", "standard", true)
            .append("chat_session_name")  // 中文分词
            .append("chat_session_type", "integer", "standard", false)
            .append("avatar_id",         "keyword", "standard", false)
            .append("status",            "integer", "standard", false)
            .append("update_time",       "long",    "standard", false)
            .create();
        if(!ret) {
            LOG_ERROR("会话搜索索引创建失败");
            return false;
        }
        LOG_INFO("会话搜索索引创建成功");
        return true;
    }

    bool append_data(const chatnow::ChatSession &cs) {
        // 把 update_time 转成秒级时间戳便于排序
        static const boost::posix_time::ptime epoch(boost::gregorian::date(1970, 1, 1));
        long ts = (cs.update_time() - epoch).total_seconds();
        bool ret = ESInsert(_client, "chat_session")
            .append("chat_session_id",   cs.chat_session_id())
            .append("chat_session_name", cs.chat_session_name())
            .append("chat_session_type", static_cast<int>(cs.chat_session_type()))
            .append("avatar_id",         cs.avatar_id())
            .append("status",            static_cast<int>(cs.status()))
            .append("update_time",       ts)
            .insert(cs.chat_session_id());
        if(!ret) {
            LOG_ERROR("会话搜索数据插入/更新失败 ssid={}", cs.chat_session_id());
            return false;
        }
        return true;
    }

    bool remove(const std::string &ssid) {
        return ESRemove(_client, "chat_session").remove(ssid);
    }

    std::vector<std::string> search(const std::string &key,
                                    std::optional<chatnow::ChatSessionType> type = std::nullopt,
                                    int size = 20)
    {
        std::vector<std::string> res;
        ESSearch builder(_client, "chat_session");
        builder.append_must_match("chat_session_name", key)
               .append_must_term("status", std::to_string(0)) // 仅正常
               .sort_by("update_time", "desc")
               .page(0, size);
        if(type.has_value()) {
            builder.append_must_term("chat_session_type",
                                     std::to_string(static_cast<int>(type.value())));
        }
        Json::Value json_session = builder.search();
        if(!json_session.isArray()) return res;
        for(int i = 0; i < (int)json_session.size(); ++i) {
            res.push_back(json_session[i]["_source"]["chat_session_id"].asString());
        }
        return res;
    }

private:
    std::shared_ptr<elasticlient::Client> _client;
};

} // namespace chatnow
