#pragma once

#include <odb/database.hxx>
#include <odb/mysql/database.hxx>
#include "logger.hpp"

//用户注册/登录，验证码获取，邮箱注册/登录，获取用户信息，用户信息修改
//用信息新增；通过昵称获取用户信息，通过邮箱获取用户信息，通过用户ID获取用户信息，通过多个用户ID获取多个用户信息；信息修改

namespace chatnow
{

class ODBFactory
{
public:
    static std::shared_ptr<odb::core::database> create(const std::string &user,
                                                    const std::string &password,
                                                    const std::string &host,
                                                    const std::string &db,
                                                    const std::string &cset,
                                                    uint16_t port,
                                                    int conn_pool_count) 
    {
       std::unique_ptr<odb::mysql::connection_pool_factory> factory(new odb::mysql::connection_pool_factory(conn_pool_count, 0));
       auto res = std::make_shared<odb::mysql::database>(user, password, db, host, port, "", cset, 0, std::move(factory));
       return res;
    }
};


}