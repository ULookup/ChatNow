#pragma once

#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>

namespace chatnow
{

#pragma db object table("user")
class User
{
public:
    User() {}
    /* brief: 用户名--新增用户：用户ID、昵称、密码  */
    User(const std::string &uid, const std::string &nickname, const std::string &password) 
        : _user_id(uid), _nickname(nickname), _password(password) {}
    /* brief: 手机号--新增用户：用户ID、邮箱、随机昵称 */
    User(const std::string &uid, const std::string &mail) 
        : _user_id(uid), _mail(mail), _nickname(uid) {}

    void user_id(const std::string &uid) { _user_id = uid; }
    std::string user_id() const { return _user_id; }

    odb::nullable<std::string> nickname() { return _nickname; }
    void nickname(const std::string &val) { _nickname = val; }

    odb::nullable<std::string> description() { return _description; }
    void description(const std::string &val) { _description = val; }

    odb::nullable<std::string> password() { return _password; }
    void password(const std::string &val) { _password = val; }

    odb::nullable<std::string> mail() { return _mail; }
    void mail(const std::string &val) { _mail = val; }

    odb::nullable<std::string> avatar_id() { return _avatar_id; }
    void avatar_id(const std::string &val) { _avatar_id = val; }
private:
    friend class odb::access;

    #pragma db id auto //定义主键ID
    unsigned long _id;
    #pragma db type("varchar(64)") index unique  // 给用户ID建立索引
    std::string _user_id;   //用户ID
    #pragma db type("varchar(64)") index unique  // 给用户名建立索引
    odb::nullable<std::string> _nickname;  //用户昵称，不一定存在(邮箱注册)
    odb::nullable<std::string> _description; //用户签名，不一定存在
    #pragma db type("varchar(64)") 
    odb::nullable<std::string> _password;  //登录密码, 不一定存在
    #pragma db type("varchar(64)")  index unique // 给用户邮箱建立索引
    odb::nullable<std::string> _mail;     //用户邮箱，不一定存在
    #pragma db type("varchar(64)") 
    odb::nullable<std::string> _avatar_id; //用户头像文件ID，不一定存在    
};

}