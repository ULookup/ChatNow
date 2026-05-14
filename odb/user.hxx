#pragma once

#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

/**
 * ===========================================================================
 * 用户主表 (user)
 * ---------------------------------------------------------------------------
 * 设计定位：
 *   - 仅承载"用户身份 + 公开资料 + 登录凭证"三类强一致信息
 *   - 多端设备 / token / 在线状态 → user_device
 *   - 高频变更的偏好开关（消息提示、字体、皮肤）→ 后续 user_setting
 *
 * 字段速览：
 *   _id              物理主键，BIGINT 自增，紧贴 InnoDB 聚簇索引顺序写
 *   _user_id         业务用户 ID，对外字符串（雪花/UUID），唯一索引
 *   _nickname        昵称，可重复，普通索引；模糊搜索由 ES 承担
 *   _description     个人签名，varchar(255)，无索引
 *   _password        密码哈希（bcrypt/argon2id），256 位余量；不索引
 *   _password_salt   盐值；bcrypt 自带盐时为空
 *   _mail            邮箱，唯一索引；邮箱注册/找回入口
 *   _phone           手机号，唯一索引；E.164 最长 15 字符，留 20 位余量
 *   _avatar_id       头像文件 ID，引用 file 服务
 *   _gender          性别枚举（0~3），节省空间
 *   _region          地区文本，无索引
 *   _status          账号状态：正常/冻结/封禁/注销
 *   _register_time   注册时间，DATETIME(3) 毫秒精度
 *   _last_login_time 最近登录时间，活跃度统计
 *   _last_login_ip   最近登录 IP，varchar(45) 兼容 IPv6
 *   _update_time     资料最近更新时间，客户端"我的资料"增量同步用
 *
 * 索引策略：
 *   - 仅对高基数字段（user_id / mail / phone / nickname）建索引
 *   - status 不再单列索引：低基数（4 种值）单列索引优化器不会用，
 *     运营批量查询通过分析型库处理
 * ===========================================================================
 */

namespace chatnow
{

enum class Gender : unsigned char {
    UNKNOWN = 0, MALE = 1, FEMALE = 2, OTHER = 3
};

enum class UserStatus : unsigned char {
    NORMAL   = 0,   // 正常
    FROZEN   = 1,   // 临时冻结（风控）
    BANNED   = 2,   // 永久封禁
    DEACTIVE = 3    // 已注销（保留行，user_id 不复用）
};

#pragma db object table("user")
class User
{
public:
    User() = default;
    /* brief: 昵称密码注册 */
    User(const std::string &uid, const std::string &nickname, const std::string &password)
        : _user_id(uid), _nickname(nickname), _password(password) {}

    void user_id(const std::string &v) { _user_id = v; }
    std::string user_id() const { return _user_id; }

    std::string nickname() const { return _nickname ? *_nickname : std::string(); }
    void nickname(const std::string &v) { _nickname = v; }

    std::string description() const { return _description ? *_description : std::string(); }
    void description(const std::string &v) { _description = v; }

    std::string password() const { return _password ? *_password : std::string(); }
    void password(const std::string &v) { _password = v; }

    std::string password_salt() const { return _password_salt ? *_password_salt : std::string(); }
    void password_salt(const std::string &v) { _password_salt = v; }

    std::string mail() const { return _mail ? *_mail : std::string(); }
    void mail(const std::string &v) { _mail = v; }

    std::string phone() const { return _phone ? *_phone : std::string(); }
    void phone(const std::string &v) { _phone = v; }

    std::string avatar_id() const { return _avatar_id ? *_avatar_id : std::string(); }
    void avatar_id(const std::string &v) { _avatar_id = v; }

    Gender gender() const { return _gender; }
    void gender(Gender v) { _gender = v; }

    std::string region() const { return _region ? *_region : std::string(); }
    void region(const std::string &v) { _region = v; }

    UserStatus status() const { return _status; }
    void status(UserStatus v) { _status = v; }

    boost::posix_time::ptime register_time() const { return _register_time; }
    void register_time(const boost::posix_time::ptime &v) { _register_time = v; }

    boost::posix_time::ptime last_login_time() const {
        return _last_login_time ? *_last_login_time : boost::posix_time::ptime();
    }
    void last_login_time(const boost::posix_time::ptime &v) { _last_login_time = v; }

    std::string last_login_ip() const { return _last_login_ip ? *_last_login_ip : std::string(); }
    void last_login_ip(const std::string &v) { _last_login_ip = v; }

    boost::posix_time::ptime update_time() const { return _update_time; }
    void update_time(const boost::posix_time::ptime &v) { _update_time = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("varchar(32)") index unique
    std::string _user_id;

    #pragma db type("varchar(64)") index
    odb::nullable<std::string> _nickname;

    #pragma db type("varchar(255)")
    odb::nullable<std::string> _description;

    #pragma db type("varchar(256)")
    odb::nullable<std::string> _password;

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _password_salt;

    #pragma db type("varchar(128)") index unique
    odb::nullable<std::string> _mail;

    #pragma db type("varchar(20)") index unique
    odb::nullable<std::string> _phone;

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _avatar_id;

    #pragma db type("tinyint unsigned")
    Gender _gender {Gender::UNKNOWN};

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _region;

    // 状态字段不单列加 index：低基数枚举单列索引选择性差，优化器不会走；
    // 运营批量查询通过 OLAP 库或离线分析；故仅保留列，不加索引
    #pragma db type("tinyint unsigned")
    UserStatus _status {UserStatus::NORMAL};

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _register_time;

    #pragma db type("DATETIME(3)")
    odb::nullable<boost::posix_time::ptime> _last_login_time;

    #pragma db type("varchar(45)")
    odb::nullable<std::string> _last_login_ip;

    // 资料更新时间：客户端"我"的资料变更同步用，应用层每次 UPDATE 时刷新
    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _update_time;
};

} // namespace chatnow
