#pragma once

#include "infra/logger.hpp"
#include "dao/mysql.hpp"
#include "user.hxx"
#include "user-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include <memory>
#include <vector>
#include <string>

namespace chatnow
{

/**
 * UserTable
 * ------------------------------------------------------------------
 * user 表的 DAO 封装。新版 schema 引入 phone / status / 登录信息 / update_time，
 * 因此本类对外 API 增加：
 *   - select_by_phone：手机号登录入口
 *   - touch_login：登录成功后写入 last_login_time / last_login_ip
 *   - set_status：账号状态变更（封禁 / 注销）
 *   - select_multi_users：保留批量查询，改用 in_range 替代字符串拼 SQL（防注入 + 类型安全）
 * 所有 update/insert 都自动维护 update_time。
 * ------------------------------------------------------------------
 */
class UserTable
{
public:
    using ptr = std::shared_ptr<UserTable>;

    UserTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 新增用户；自动填 register_time 与 update_time */
    bool insert(std::shared_ptr<User> &user) {
        try {
            auto now = boost::posix_time::microsec_clock::universal_time();
            user->register_time(now);
            user->update_time(now);

            odb::transaction trans(_db->begin());
            _db->persist(*user);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("新增用户失败: {}-{}", user->nickname(), e.what());
            return false;
        }
        return true;
    }

    /* brief: 通用更新；自动刷新 update_time，便于客户端"我的资料"增量同步 */
    bool update(const std::shared_ptr<User> &user) {
        try {
            user->update_time(boost::posix_time::microsec_clock::universal_time());

            odb::transaction trans(_db->begin());
            _db->update(*user);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新用户失败: {}-{}", user->nickname(), e.what());
            return false;
        }
        return true;
    }

    /* brief: 按昵称查询（昵称不再 unique，可能多匹配，调用方自行处理重名） */
    std::shared_ptr<User> select_by_nickname(const std::string &nickname) {
        std::shared_ptr<User> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<User>;
            res.reset(_db->query_one<User>(query::nickname == nickname));
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过昵称查询用户失败: {}-{}", nickname, e.what());
        }
        return res;
    }

    /* brief: 按邮箱查询（邮箱唯一） */
    std::shared_ptr<User> select_by_mail(const std::string &mail) {
        std::shared_ptr<User> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<User>;
            res.reset(_db->query_one<User>(query::mail == mail));
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过邮箱查询用户失败: {}-{}", mail, e.what());
        }
        return res;
    }

    /* brief: 按手机号查询（手机号唯一）— 新版 schema 新增登录入口 */
    std::shared_ptr<User> select_by_phone(const std::string &phone) {
        std::shared_ptr<User> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<User>;
            res.reset(_db->query_one<User>(query::phone == phone));
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过手机号查询用户失败: {}-{}", phone, e.what());
        }
        return res;
    }

    /* brief: 按业务用户 ID 查询 */
    std::shared_ptr<User> select_by_id(const std::string &user_id) {
        std::shared_ptr<User> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<User>;
            res.reset(_db->query_one<User>(query::user_id == user_id));
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过用户ID查询用户失败: {}-{}", user_id, e.what());
        }
        return res;
    }

    /* brief: 批量查询；in_range 替代旧的字符串拼接，防 SQL 注入 */
    std::vector<User> select_multi_users(const std::vector<std::string> &id_list) {
        std::vector<User> res;
        if(id_list.empty()) return res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<User>;
            using result = odb::result<User>;
            result r(_db->query<User>(query::user_id.in_range(id_list.begin(), id_list.end())));
            for(auto &u : r) res.push_back(u);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过多个用户ID查询多个用户失败: {}", e.what());
        }
        return res;
    }

    /* brief: 登录成功回写：last_login_time / last_login_ip / update_time */
    bool touch_login(const std::string &user_id, const std::string &login_ip) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<User>;
            std::shared_ptr<User> u(_db->query_one<User>(query::user_id == user_id));
            if(!u) {
                trans.commit();
                return false;
            }
            auto now = boost::posix_time::microsec_clock::universal_time();
            u->last_login_time(now);
            if(!login_ip.empty()) u->last_login_ip(login_ip);
            u->update_time(now);
            _db->update(*u);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新登录信息失败 {}: {}", user_id, e.what());
            return false;
        }
        return true;
    }

    /* brief: 修改账号状态（封禁 / 注销）— 风控用 */
    bool set_status(const std::string &user_id, UserStatus status) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<User>;
            std::shared_ptr<User> u(_db->query_one<User>(query::user_id == user_id));
            if(!u) {
                trans.commit();
                return false;
            }
            u->status(status);
            u->update_time(boost::posix_time::microsec_clock::universal_time());
            _db->update(*u);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("修改账号状态失败 {}: {}", user_id, e.what());
            return false;
        }
        return true;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
