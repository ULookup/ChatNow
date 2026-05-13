#pragma once

#include "logger.hpp"
#include "mysql.hpp"
#include "user_device.hxx"
#include "user_device-odb.hxx"
#include <odb/database.hxx>
#include <odb/mysql/database.hxx>

#include <memory>
#include <string>
#include <vector>

namespace chatnow
{

/**
 * UserDeviceTable
 * ------------------------------------------------------------------
 * user_device 表的 DAO 封装（多端登录 / 推送 / 风控）。
 *
 * 典型使用场景：
 *   - 登录：upsert_login() — 同一用户同一设备只一行（uk_user_device 唯一）
 *   - 心跳：touch_active() — 仅写 last_active_time 与 online_status
 *   - 推送：list_active_for_push() — 取该用户所有在线设备的 push_token
 *   - 风控：set_offline_by_session() — 强制下线指定 session
 *
 * 设计要点：
 *   - last_active_time 不索引（30s 心跳频率写抖动太大），找掉线设备走批处理扫表
 *   - online_status 实时态在 Redis 维护；DB 落库为 last-known 快照
 *   - push_token 唯一约束防账号切换残留：旧 row 的 token 必须先清空再写新 row
 * ------------------------------------------------------------------
 */
class UserDeviceTable
{
public:
    using ptr = std::shared_ptr<UserDeviceTable>;
    UserDeviceTable(const std::shared_ptr<odb::core::database> &db) : _db(db) {}

    /* brief: 登录写入（首次登录 = persist；已有 = update） */
    bool upsert_login(UserDevice &dev) {
        try {
            auto now = boost::posix_time::microsec_clock::universal_time();
            dev.login_time(now);
            dev.last_active_time(now);
            dev.update_time(now);
            dev.online_status(OnlineStatus::ONLINE);

            odb::transaction trans(_db->begin());
            using query = odb::query<UserDevice>;
            std::shared_ptr<UserDevice> existing(_db->query_one<UserDevice>(
                query::user_id == dev.user_id() && query::device_id == dev.device_id()));

            if(existing) {
                existing->device_type(dev.device_type());
                existing->device_name(dev.device_name());
                existing->app_version(dev.app_version());
                existing->os_version(dev.os_version());
                existing->login_session_id(dev.login_session_id());
                existing->push_token(dev.push_token());
                existing->login_ip(dev.login_ip());
                existing->online_status(OnlineStatus::ONLINE);
                existing->last_active_time(now);
                existing->login_time(now);
                existing->expire_time(dev.expire_time());
                existing->update_time(now);
                _db->update(*existing);
            } else {
                _db->persist(dev);
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("登录写入设备失败 {}-{}: {}", dev.user_id(), dev.device_id(), e.what());
            return false;
        }
        return true;
    }

    /* brief: 心跳 — 仅刷活跃时间，不动其他字段（轻量、避免 update_time 干扰） */
    bool touch_active(const std::string &session_id) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserDevice>;
            std::shared_ptr<UserDevice> dev(_db->query_one<UserDevice>(
                query::login_session_id == session_id));
            if(!dev) {
                trans.commit();
                return false;
            }
            dev->last_active_time(boost::posix_time::microsec_clock::universal_time());
            dev->online_status(OnlineStatus::ONLINE);
            _db->update(*dev);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("更新设备心跳失败 sid={}: {}", session_id, e.what());
            return false;
        }
        return true;
    }

    /* brief: 通过 session_id 查设备（与 Redis Session 配合） */
    std::shared_ptr<UserDevice> select_by_session(const std::string &session_id) {
        std::shared_ptr<UserDevice> res;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserDevice>;
            res.reset(_db->query_one<UserDevice>(query::login_session_id == session_id));
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("通过 session_id 查设备失败 {}: {}", session_id, e.what());
        }
        return res;
    }

    /* brief: 取用户所有"在线"设备（含 ONLINE/AWAY/BUSY 等非离线状态）
     *  - 推送下发的核心入口；调用方再各自拿 push_token 投递
     */
    std::vector<UserDevice> list_active(const std::string &user_id) {
        std::vector<UserDevice> res;
        try {
            odb::transaction trans(_db->begin());
            using query  = odb::query<UserDevice>;
            using result = odb::result<UserDevice>;
            result r(_db->query<UserDevice>(
                query::user_id == user_id && query::online_status != OnlineStatus::OFFLINE));
            for(auto &dev : r) res.push_back(dev);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("取用户在线设备失败 {}: {}", user_id, e.what());
        }
        return res;
    }

    /* brief: 强制下线某个 session（管理员踢人 / 互斥登录） */
    bool set_offline_by_session(const std::string &session_id) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserDevice>;
            std::shared_ptr<UserDevice> dev(_db->query_one<UserDevice>(
                query::login_session_id == session_id));
            if(!dev) {
                trans.commit();
                return false;
            }
            dev->online_status(OnlineStatus::OFFLINE);
            dev->update_time(boost::posix_time::microsec_clock::universal_time());
            _db->update(*dev);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("强制下线设备失败 {}: {}", session_id, e.what());
            return false;
        }
        return true;
    }

    /* brief: 一个用户的所有设备全部下线（账号被封禁 / 用户主动注销）*/
    bool set_offline_all(const std::string &user_id) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserDevice>;
            using result = odb::result<UserDevice>;
            result r(_db->query<UserDevice>(query::user_id == user_id));
            auto now = boost::posix_time::microsec_clock::universal_time();
            for(auto &dev : r) {
                if(dev.online_status() != OnlineStatus::OFFLINE) {
                    UserDevice copy = dev;
                    copy.online_status(OnlineStatus::OFFLINE);
                    copy.update_time(now);
                    _db->update(copy);
                }
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("批量下线设备失败 {}: {}", user_id, e.what());
            return false;
        }
        return true;
    }

    /* brief: 移除设备（用户解绑 / 卸载 App） */
    bool remove(const std::string &user_id, const std::string &device_id) {
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserDevice>;
            _db->erase_query<UserDevice>(
                query::user_id == user_id && query::device_id == device_id);
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("移除设备失败 {}-{}: {}", user_id, device_id, e.what());
            return false;
        }
        return true;
    }

    /* brief: push_token 全局迁移 — 旧用户 row 清空 token，便于 uk_push_token 让出 */
    bool clear_push_token_globally(const std::string &push_token) {
        if(push_token.empty()) return true;
        try {
            odb::transaction trans(_db->begin());
            using query = odb::query<UserDevice>;
            using result = odb::result<UserDevice>;
            result r(_db->query<UserDevice>(query::push_token == push_token));
            for(auto &dev : r) {
                UserDevice copy = dev;
                copy.push_token(""); // 让出唯一约束
                copy.update_time(boost::posix_time::microsec_clock::universal_time());
                _db->update(copy);
            }
            trans.commit();
        } catch(std::exception &e) {
            LOG_ERROR("清理 push_token 失败: {}", e.what());
            return false;
        }
        return true;
    }

private:
    std::shared_ptr<odb::core::database> _db;
};

} // namespace chatnow
