#pragma once

#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

/**
 * ===========================================================================
 * 用户设备表 (user_device) — 多端登录 / 推送 / 风控核心
 * ---------------------------------------------------------------------------
 * 设计定位：
 *   - 一个用户可同时持有多个设备 token（手机 + 电脑 + 网页）
 *   - 网关推消息按 user_id 查所有 push_token 全部下发，配合客户端去重
 *   - 提供"踢掉旧设备"/"互斥登录"/"异地登录提醒"等基础能力
 *
 * 字段速览：
 *   _id                 物理主键
 *   _user_id            用户 ID
 *   _device_id          设备唯一指纹（客户端生成）
 *   _device_type        平台：ANDROID/IOS/WEB/MACOS/...
 *   _device_name        设备型号名称（"iPhone 14 Pro"）
 *   _app_version        客户端版本号
 *   _os_version         操作系统版本
 *   _login_session_id   登录会话 ID（与 Redis Session 同步），与 WS 长连接绑定
 *   _push_token         推送 Token（FCM/APNs/厂商通道）
 *   _login_ip           登录 IP（IPv6 兼容长度 45）
 *   _online_status      在线状态：离线/在线/离开/忙碌/隐身
 *                       注意：高频字段，实时态在 Redis 维护，本字段为 last-known 落库快照
 *   _last_active_time   最近活跃时间（心跳更新；不索引避免 index 页频繁抖动）
 *   _login_time         本次登录时间
 *   _expire_time        session 过期时间（用于强制下线）
 *   _update_time        记录最近更新时间
 *
 * 索引策略：
 *   _login_session_id   unique — 网关 session_id → 设备/用户的高频查询入口
 *   uk_user_device      (user_id, device_id) UNIQUE
 *                       同一用户同一设备只一行；重新登录是 UPDATE 不堆积
 *   uk_push_token       push_token UNIQUE（允许 NULL）
 *                       一个推送 token 只能挂在一个用户上；账号切换时必须迁移
 *   说明：
 *     - 原 _online_status 单列索引删除：枚举 5 种值，单列选择性差
 *     - 原 _last_active_time 单列索引删除：心跳每 30s 写一次，索引页持续抖动；
 *       "找掉线设备"是低频批处理，扫表足够
 * ===========================================================================
 */

namespace chatnow
{

enum class DeviceType : unsigned char {
    UNKNOWN = 0,
    ANDROID = 1,
    IOS     = 2,
    WINDOWS = 3,
    MACOS   = 4,
    LINUX   = 5,
    WEB     = 6,
    MINI    = 7   // 小程序 / 第三方接入
};

enum class OnlineStatus : unsigned char {
    OFFLINE = 0,
    ONLINE  = 1,
    AWAY    = 2,
    BUSY    = 3,
    HIDDEN  = 4
};

#pragma db object table("user_device")
class UserDevice
{
public:
    UserDevice() = default;

    std::string user_id() const { return _user_id; }
    void user_id(const std::string &v) { _user_id = v; }

    std::string device_id() const { return _device_id; }
    void device_id(const std::string &v) { _device_id = v; }

    DeviceType device_type() const { return _device_type; }
    void device_type(DeviceType v) { _device_type = v; }

    std::string device_name() const { return _device_name ? *_device_name : std::string(); }
    void device_name(const std::string &v) { _device_name = v; }

    std::string app_version() const { return _app_version ? *_app_version : std::string(); }
    void app_version(const std::string &v) { _app_version = v; }

    std::string os_version() const { return _os_version ? *_os_version : std::string(); }
    void os_version(const std::string &v) { _os_version = v; }

    std::string login_session_id() const { return _login_session_id; }
    void login_session_id(const std::string &v) { _login_session_id = v; }

    std::string push_token() const { return _push_token ? *_push_token : std::string(); }
    void push_token(const std::string &v) { _push_token = v; }

    std::string login_ip() const { return _login_ip ? *_login_ip : std::string(); }
    void login_ip(const std::string &v) { _login_ip = v; }

    OnlineStatus online_status() const { return _online_status; }
    void online_status(OnlineStatus v) { _online_status = v; }

    boost::posix_time::ptime last_active_time() const { return _last_active_time; }
    void last_active_time(const boost::posix_time::ptime &v) { _last_active_time = v; }

    boost::posix_time::ptime login_time() const { return _login_time; }
    void login_time(const boost::posix_time::ptime &v) { _login_time = v; }

    boost::posix_time::ptime expire_time() const {
        return _expire_time ? *_expire_time : boost::posix_time::ptime();
    }
    void expire_time(const boost::posix_time::ptime &v) { _expire_time = v; }

    boost::posix_time::ptime update_time() const { return _update_time; }
    void update_time(const boost::posix_time::ptime &v) { _update_time = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("varchar(32)")
    std::string _user_id;

    #pragma db type("varchar(128)")
    std::string _device_id;

    #pragma db type("tinyint unsigned")
    DeviceType _device_type {DeviceType::UNKNOWN};

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _device_name;

    #pragma db type("varchar(32)")
    odb::nullable<std::string> _app_version;

    #pragma db type("varchar(32)")
    odb::nullable<std::string> _os_version;

    #pragma db type("varchar(64)") index unique
    std::string _login_session_id;

    // push_token 唯一：同一推送通道 token 只属于一个用户/设备；账号切换需 UPDATE 迁移
    #pragma db type("varchar(255)")
    odb::nullable<std::string> _push_token;

    #pragma db type("varchar(45)")
    odb::nullable<std::string> _login_ip;

    // 在线状态：高频字段，实时态在 Redis；本字段不单列加索引（低基数 + 高频写）
    #pragma db type("tinyint unsigned")
    OnlineStatus _online_status {OnlineStatus::OFFLINE};

    // 心跳每 30s 更新一次：不加 index 避免 index 页持续抖动；扫表找掉线设备
    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _last_active_time;

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _login_time;

    #pragma db type("DATETIME(3)")
    odb::nullable<boost::posix_time::ptime> _expire_time;

    // 记录最近更新时间：审计/排查用
    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _update_time;

    // 同一用户同一设备只一行：重新登录走 UPDATE 路径
    #pragma db index("uk_user_device") unique members(_user_id, _device_id)
    // 推送 token 全局唯一（允许 NULL）：账号切换时迁移而非堆积
    #pragma db index("uk_push_token") unique members(_push_token)
};

} // namespace chatnow
