#include "icsearch.hpp"
#include "user.hxx"

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

}