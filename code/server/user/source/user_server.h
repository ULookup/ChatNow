#include <brpc/server.h>    //
#include <butil/logging.h>  // 实现语音识别子服务
#include "etcd.hpp"         // 服务注册模块封装
#include "channel.hpp"      // 信道管理模块封装
#include "logger.hpp"       // 日志模块封装
#include "utils.hpp"        // 基础工具接口
#include "mail.hpp"         // 邮箱验证
#include "data_es.hpp"      // es数据管理客户端封装
#include "mysql_user.hpp"   // mysql数据管理客户端封装
#include "data_redis.hpp"   // redis数据管理客户端封装
#include "base.pb.h"
#include "user.pb.h"        // protobuf框架代码
#include "file.pb.h"


namespace chatnow
{

class UserServiceImpl : public UserService
{
public:
    UserServiceImpl(const std::shared_ptr<elasticlient::Client> &es_client,
                    const std::shared_ptr<odb::core::database> &mysql_client,
                    const std::shared_ptr<sw::redis::Redis> &redis_client,
                    const std::shared_ptr<MailClient> &mail_client,
                    const ServiceManager::ptr &channel_manager,
                    const std::string &file_service_name)
                    : _es_user(std::make_shared<ESUser>(es_client)),
                    _mysql_user(std::make_shared<UserTable>(mysql_client)),
                    _redis_session(std::make_shared<Session>(redis_client)),
                    _redis_status(std::make_shared<Status>(redis_client)),
                    _redis_codes(std::make_shared<Codes>(redis_client)),
                    _mail_client(std::make_shared<MailClient>(mail_client->settings())),
                    _file_service_name(file_service_name),
                    _mm_channels(channel_manager) 
    {
        _es_user->create_index();
    }
    ~UserServiceImpl() = default;
    bool nickname_check(const std::string &nickname) { return nickname.size() < 22; }
    bool password_check(const std::string &password) {
        if(password.size() < 6 || password.size() > 15) {
            LOG_ERROR("密码长度不合法: {}-{}", password, password.size());
            return false;
        }
        for(int i = 0; i < password.size(); ++i) {
            if(!((password[i] > 'a' && password[i] < 'z') || 
            (password[i] > 'A' && password[i] < 'Z') || 
            (password[i] > '0' && password[i] < '9') ||
            password[i] == '_' || password[i] == '-')) {
                LOG_ERROR("密码字符不合法: {}", password);
                return false;
            }
        }
        return true;
    }
    bool mail_check(const std::string &mail) {
        auto ret1 = mail.find('@');
        auto ret2 = mail.rfind('.');
        auto ret3 = mail.find(' ');
        if(ret1 == std::string::npos || ret2 == std::string::npos || ret3 != std::string::npos) {
            LOG_ERROR("无效邮箱地址");
            return false;
        }
        return true;
    }
    /* brief: 用户注册 */
    virtual void UserRegister(::google::protobuf::RpcController* controller,
                        const ::chatnow::UserRegisterReq* request,
                        ::chatnow::UserRegisterRsp* response,
                        ::google::protobuf::Closure* done)
    {
        LOG_DEBUG("收到用户注册请求");
        brpc::ClosureGuard rpc_guard(done);
        //定义一个错误处理函数，出错时调用
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        //1. 从请求中取出昵称、密码
        std::string nickname = request->nickname();
        std::string password = request->password();
        //2. 检查昵称是否合法（只能包含字母，数字，连字符-，下划线_，长度限制 3~15之间）
        bool ret = nickname_check(nickname);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 用户名长度不合法", request->request_id());
            return err_response(request->request_id(), "用户名长度不合法");
        }
        //3. 检查密码是否合法（只能包含字母，数字，长度限制 6~15之间）
        ret = password_check(password);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 密码格式不合法", request->request_id());
            return err_response(request->request_id(), "密码格式不合法");
        }
        //4. 根据昵称在数据库进行判断昵称是否已存在
        auto user = _mysql_user->select_by_nickname(nickname);
        if(user) {
            LOG_ERROR("请求ID: {} - 用户名已被占用: {}", request->request_id(), nickname);
            return err_response(request->request_id(), "用户名已被占用");
        }
        //5. 向数据库新增数据
        std::string uid = uuid();
        user = std::make_shared<User>(uid, nickname, password);
        ret = _mysql_user->insert(user);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - MySQL数据库新增数据失败", request->request_id());
            return err_response(request->request_id(), "MySQL数据库新增数据失败");
        }
        //6. 向 ES 服务器中新增用户信息
        ret = _es_user->append_data(uid, "", nickname, "", "");
        if(ret == false) {
            LOG_ERROR("请求ID: {} - ES搜索引擎新增数据失败", request->request_id());
            return err_response(request->request_id(), "ES搜索引擎新增数据失败");
        }
        //7. 组织响应，进行成功与否的响应即可
        response->set_request_id(request->request_id());
        response->set_success(true);
    }
    /* brief: 用户登录 */
    virtual void UserLogin(::google::protobuf::RpcController* controller,
                        const ::chatnow::UserLoginReq* request,
                        ::chatnow::UserLoginRsp* response,
                        ::google::protobuf::Closure* done)
    {
        LOG_DEBUG("收到用户登录请求");
        brpc::ClosureGuard rpc_guard(done);
        //定义一个错误处理函数，出错时调用
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        //1. 从请求中取出昵称和密码
        std::string nickname = request->nickname();
        std::string password = request->password();
        //2. 通过昵称获取用户信息，进行密码是否一致的判断
        auto user = _mysql_user->select_by_nickname(nickname);
        if(!user || password != user->password()) {
            LOG_ERROR("请求ID: {} - 用户名或密码错误 - {} - {}", request->request_id(), nickname, password);
            return err_response(request->request_id(), "用户名或密码错误");
        }
        //3. 根据 redis 中的登录标记信息是否存在，判断用户是否已经登录
        bool ret = _redis_status->exists(user->user_id());
        if(ret == true) {
            LOG_ERROR("请求ID: {} - 用户已在它处登录 - {}", request->request_id(), nickname);
            return err_response(request->request_id(), "用户已在它处登录");
        }
        //4. 构造会话ID，生成会话键值对，向 redis 中添加会话信息以及登录标记信息
        std::string ssid = uuid();
        _redis_session->append(ssid, user->user_id());
        // 添加用户登录信息
        _redis_status->append(user->user_id());
        //5. 组织响应，返回生成的会话ID
        response->set_request_id(request->request_id());
        response->set_login_session_id(ssid);
        response->set_success(true);
    }
    /* brief: 获取邮箱验证码 */
    virtual void GetMailVerifyCode(::google::protobuf::RpcController* controller,
                        const ::chatnow::MailVerifyCodeReq* request,
                        ::chatnow::MailVerifyCodeRsp* response,
                        ::google::protobuf::Closure* done)
    {
        LOG_DEBUG("收到邮箱验证码获取请求");
        brpc::ClosureGuard rpc_guard(done);
        //定义一个错误处理函数，出错时调用
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        //1. 从请求中取出邮箱
        std::string mail = request->mail_number();
        //2. 验证邮箱格式是否正确(XX@XX.XXX)
        bool ret = mail_check(mail);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 邮箱格式错误: {}", request->request_id(), mail);
            return err_response(request->request_id(), "邮箱格式错误");
        }
        //3. 生成4位随机验证码
        std::string code_id = uuid();
        std::string code = verifyCode();
        //4. 基于邮箱平台发送验证码
        ret = _mail_client->send(mail, code);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 发送验证码失败: {}", request->request_id(), mail);
            return err_response(request->request_id(), "发送验证码失败");
        }
        //5. 构造验证码ID，添加到redis
        _redis_codes->append(code_id, code);
        //6. 组织响应，返回生成的验证码ID
        response->set_request_id(request->request_id());
        response->set_success(true);  
        response->set_verify_code_id(code_id);     
    }
    /* brief: 邮箱注册 */
    virtual void MailRegister(::google::protobuf::RpcController* controller,
                        const ::chatnow::MailRegisterReq* request,
                        ::chatnow::MailRegisterRsp* response,
                        ::google::protobuf::Closure* done)
    {
        LOG_DEBUG("收到邮箱注册请求");
        brpc::ClosureGuard rpc_guard(done);
        //定义一个错误处理函数，出错时调用
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        //1. 从请求取出邮箱和验证码
        std::string mail = request->mail_number();
        std::string code_id = request->verify_code_id();
        std::string code = request->verify_code();
        //2. 检查注册邮箱是否合法
        bool ret = mail_check(mail);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 邮箱格式非法: {}", request->request_id(), mail);
            return err_response(request->request_id(), "邮箱格式非法");
        }
        //3. 从redis中进行 验证码ID - 验证码 一致性匹配
        auto verify_code = _redis_codes->code(code_id);
        if(verify_code != code) {
            LOG_ERROR("请求ID: {} - 验证码错误: {} - {}", request->request_id(), code_id, code);
            return err_response(request->request_id(), "验证码错误");
        }
        //4. 通过数据库查询判断邮箱是否已经注册过
        auto user = _mysql_user->select_by_mail(mail);
        if(user) {
            LOG_ERROR("请求ID: {} - 该邮箱已注册过用户: {}", request->request_id(), mail);
            return err_response(request->request_id(), "该邮箱已注册过用户");
        }
        //5. 向数据库新增用户信息
        std::string uid = uuid();
        user = std::make_shared<User>(uid, mail);
        ret = _mysql_user->insert(user);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 向数据库添加用户ID失败: {}", request->request_id(), mail);
            return err_response(request->request_id(), "向数据库添加用户ID失败");           
        }
        //6. 向ES服务器中新增用户信息
        ret = _es_user->append_data(uid, mail, uid, "", "");
        if(ret == false) {
            LOG_ERROR("请求ID: {} - ES搜索引擎新增数据失败", request->request_id());
            return err_response(request->request_id(), "ES搜索引擎新增数据失败");
        }        
        //7. 组织响应，返回注册成功与否
        response->set_request_id(request->request_id());
        response->set_success(true);
    }
    /* brief: 邮箱登录 */
    virtual void MailLogin(::google::protobuf::RpcController* controller,
                        const ::chatnow::MailLoginReq* request,
                        ::chatnow::MailLoginRsp* response,
                        ::google::protobuf::Closure* done)
    {
        LOG_DEBUG("收到邮箱登录请求");
        brpc::ClosureGuard rpc_guard(done);
        //定义一个错误处理函数，出错时调用
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        //1. 从请求中取出邮箱和验证码ID，验证码
        std::string mail = request->mail_number();
        std::string code_id = request->verify_code_id();
        std::string code = request->verify_code();
        //2. 检查注册邮箱号码是否合法
        bool ret = mail_check(mail);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 邮箱格式非法: {}", request->request_id(), mail);
            return err_response(request->request_id(), "邮箱格式非法");            
        }
        //3. 根据邮箱从数据库进行用户信息查询，判断用户是否存在
        auto user = _mysql_user->select_by_mail(mail);
        if(!user) {
            LOG_ERROR("请求ID: {} - 用户不存在，该邮箱未注册: {}", request->request_id(), mail);
            return err_response(request->request_id(), "用户不存在，该邮箱未注册");
        }
        //4. 从 redis 中进行验证码ID - 验证码一致性匹配
        auto verify_code = _redis_codes->code(code_id);
        if(verify_code != code) {
            LOG_ERROR("请求ID: {} - 验证码错误: {} - {}", request->request_id(), code_id, code);
            return err_response(request->request_id(), "验证码错误");
        }
        _redis_codes->remove(code_id);
        //5. 根据 redis 中的登录信息判断用户是否已经登录
        ret = _redis_status->exists(user->user_id());
        if(ret == true) {
            LOG_ERROR("请求ID: {} - 用户已在它处登录 - {}", request->request_id(), mail);
            return err_response(request->request_id(), "用户已在它处登录");
        }
        //6. 构造会话 ID，生成会话键值对，向redis添加会话信息以及登录标记信息
        std::string ssid = uuid();
        _redis_session->append(ssid, user->user_id());
        // 添加用户登录信息
        _redis_status->append(user->user_id());
        //7. 组织响应，返回生成的会话ID
        response->set_request_id(request->request_id());
        response->set_login_session_id(ssid);
        response->set_success(true);
    }
    // =================== 从这一步开始，都是用户登录后的操作 =================== //
    /* brief: 获取用户信息 */
    virtual void GetUserInfo(::google::protobuf::RpcController* controller,
                        const ::chatnow::GetUserInfoReq* request,
                        ::chatnow::GetUserInfoRsp* response,
                        ::google::protobuf::Closure* done)
    {
        LOG_DEBUG("收到获取单个用户信息请求");
        brpc::ClosureGuard rpc_guard(done);
        //定义一个错误处理函数，出错时调用
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        //1. 从请求中取出用户ID
        std::string uid = request->user_id();
        //2. 通过用户ID，从数据库中查询用户信息
        auto user = _mysql_user->select_by_id(uid);
        if(!user) {
            LOG_ERROR("请求ID: {} - 用户不存在: {}", request->request_id(), uid);
            return err_response(request->request_id(), "用户不存在");            
        }
        //3. 根据用户信息中的头像ID，从文件服务器获取头像文件数据，组织完整用户信息
        UserInfo *user_info = response->mutable_user_info();
        user_info->set_user_id(user->user_id());
        user_info->set_nickname(user->nickname());
        user_info->set_description(user->description());
        user_info->set_mail(user->mail());
        if(!user->avatar_id().empty()) {
            //1.从信道管理对象中，获取到连接了文件管理子服务的channel
            auto channel = _mm_channels->choose(_file_service_name);
            if(!channel) {
                LOG_ERROR("请求ID: {} - 未找到文件子服务: {}", request->request_id(), _file_service_name);
                return err_response(request->request_id(), "用户不存在");    
            }
            //2. 进行文件子服务rpc请求，进行头像文件下载
            FileService_Stub stub(channel.get());
            GetSingleFileReq req;
            GetSingleFileRsp rsp;
            req.set_request_id(request->request_id());
            req.set_file_id(user->avatar_id());

            brpc::Controller cntl;
            stub.GetSingleFile(&cntl, &req, &rsp, nullptr);
            if(cntl.Failed() == true || rsp.success() == false) {
                LOG_ERROR("请求ID: {} - 文件子服务调用失败: {}", request->request_id(), cntl.ErrorText());
                return err_response(request->request_id(), "文件子服务调用失败");    
            }
            user_info->set_avatar(rsp.file_data().file_content());
        }
        //4. 组织响应，返回用户信息
        response->set_request_id(request->request_id());
        response->set_success(true);
    }
    /* brief: 批量获取用户信息 */
    virtual void GetMultiUserInfo(::google::protobuf::RpcController* controller,
                        const ::chatnow::GetMultiUserInfoReq* request,
                        ::chatnow::GetMultiUserInfoRsp* response,
                        ::google::protobuf::Closure* done)
    {
        LOG_DEBUG("收到批量获取用户信息请求");
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };
        //1. 从请求中取出用户ID列表
        std::vector<std::string> uid_list;
        for(int i = 0; i < request->users_id_size(); ++i) {
            uid_list.push_back(request->users_id(i));
        }
        //2. 从数据库进行批量信息查询
        auto users = _mysql_user->select_multi_users(uid_list);
        if(users.size() != request->users_id_size()) {
            LOG_ERROR("请求ID: {} - 查找到的用户信息数量与请求不一致: {} - {}", request->request_id(), request->users_id_size(), users.size());
            return err_response(request->request_id(), "查找到的用户信息数量与请求不一致");
        }
        //3. 批量从文件管理子服务下载
        //3.1 从信道管理对象中，获取到连接了文件管理子服务的channel
        auto channel = _mm_channels->choose(_file_service_name);
        if(!channel) {
            LOG_ERROR("请求ID: {} - 未找到文件子服务: {}", request->request_id(), _file_service_name);
            return err_response(request->request_id(), "用户不存在");    
        }
        //3.2 进行RPC调用
        FileService_Stub stub(channel.get());
        GetMultiFileReq req;
        GetMultiFileRsp rsp;
        req.set_request_id(request->request_id());
        for(auto &user : users) {
            if(user.avatar_id().empty()) continue;
            req.add_file_id_list(user.avatar_id());
        }
        brpc::Controller cntl;
        stub.GetMultiFile(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed() == true || rsp.success() == false) {
            LOG_ERROR("请求ID: {} - 文件子服务调用失败: {}", request->request_id(), cntl.ErrorText());
            return err_response(request->request_id(), "文件子服务调用失败");    
        }
        //4. 组织响应
        for(auto &user : users) {
            auto user_map = response->mutable_users_info(); //本次请求要响应的用户信息map
            auto file_map = rsp.mutable_file_data();   //批量文件请求的响应中的map
            UserInfo user_info;
            user_info.set_user_id(user.user_id());
            user_info.set_nickname(user.nickname());
            user_info.set_description(user.description());
            user_info.set_mail(user.mail());
            user_info.set_avatar((*file_map)[user.avatar_id()].file_content());
            (*user_map)[user_info.user_id()] = user_info;
        }
        response->set_request_id(request->request_id());
        response->set_success(true);
    }
    /* brief: 设置用户头像 */
    virtual void SetUserAvatar(::google::protobuf::RpcController* controller,
                        const ::chatnow::SetUserAvatarReq* request,
                        ::chatnow::SetUserAvatarRsp* response,
                        ::google::protobuf::Closure* done)
    {
        LOG_DEBUG("收到设置用户头像请求");
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };        
        //1. 从请求中取出用户ID
        std::string uid = request->user_id();
        //2. 将数据库通过用户ID进行用户信息查询，判断用户是否存在
        auto user = _mysql_user->select_by_id(uid);
        if(!user) {
            LOG_ERROR("请求ID: {} - 用户不存在: {}", request->request_id(), uid);
            return err_response(request->request_id(), "用户不存在");            
        }        
        //3. 上传头像文件到文件子服务
        //3.1 先获取文件存储子服务信道
        auto channel = _mm_channels->choose(_file_service_name);
        if(!channel) {
            LOG_ERROR("请求ID: {} - 未找到文件子服务: {}", request->request_id(), _file_service_name);
            return err_response(request->request_id(), "用户不存在");    
        }
        //3.2 进行RPC调用
        FileService_Stub stub(channel.get());
        PutSingleFileReq req;
        PutSingleFileRsp rsp;
        req.set_request_id(request->request_id());
        req.mutable_file_data()->set_file_name("");
        req.mutable_file_data()->set_file_size(request->avatar().size());
        req.mutable_file_data()->set_file_content(request->avatar());

        brpc::Controller cntl;
        stub.PutSingleFile(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed() == true || rsp.success() == false) {
            LOG_ERROR("请求ID: {} - 文件子服务调用失败: {}", request->request_id(), cntl.ErrorText());
            return err_response(request->request_id(), "文件子服务调用失败");    
        }        
        std::string avatar_id = rsp.file_info().file_id();
        //4. 将返回的头像文件ID 更新到数据库中
        user->avatar_id(avatar_id);
        bool ret = _mysql_user->update(user);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 更新数据库用户头像ID失败: {}", request->request_id(), avatar_id);
            return err_response(request->request_id(), "更新数据库用户头像ID失败");              
        }
        //5. 更新 ES 服务器中用户信息
        ret = _es_user->append_data(user->user_id(), user->mail(), user->nickname(), user->description(), user->avatar_id());
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 更新 ES搜索引擎 用户头像ID失败: {}", request->request_id(), avatar_id);
            return err_response(request->request_id(), "更新 ES搜索引擎 用户头像ID失败");                      
        }
        //6. 组织响应，返回更新成功与否
        response->set_request_id(request->request_id());
        response->set_success(true);
    }
    /* brief: 设置昵称 */
    virtual void SetUserNickname(::google::protobuf::RpcController* controller,
                        const ::chatnow::SetUserNicknameReq* request,
                        ::chatnow::SetUserNicknameRsp* response,
                        ::google::protobuf::Closure* done)
    {
        LOG_DEBUG("收到设置用户昵称请求");
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };        
        //1. 从请求中取出用户ID与新昵称
        std::string uid = request->user_id();
        std::string new_nickname = request->nickname();
        //2. 判断昵称是否正确
        bool ret = nickname_check(new_nickname);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 用户名长度不合法", request->request_id());
            return err_response(request->request_id(), "用户名长度不合法");
        }
        //3. 从数据库通过用户 ID 进行用户信息查询，判断用户是否存在
        auto user = _mysql_user->select_by_id(uid);
        if(!user) {
            LOG_ERROR("请求ID: {} - 用户不存在: {}", request->request_id(), uid);
            return err_response(request->request_id(), "用户不存在");            
        }
        //4. 将新的昵称更新到数据库中
        user->nickname(new_nickname);
        ret = _mysql_user->update(user);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 更新数据库用户昵称失败: {}", request->request_id(), new_nickname);
            return err_response(request->request_id(), "更新数据库用户昵称失败");                
        }
        //5. 更新 ES 服务中，用户信息
        ret = _es_user->append_data(user->user_id(), user->mail(), user->nickname(), user->description(), user->avatar_id());
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 更新 ES搜索引擎 用户昵称失败: {}", request->request_id(), user->nickname());
            return err_response(request->request_id(), "更新 ES搜索引擎 用户昵称失败");                      
        }
        //6. 组织响应
        response->set_request_id(request->request_id());
        response->set_success(true);
    }
    /* brief: 设置用户描述 */
    virtual void SetUserDescription(::google::protobuf::RpcController* controller,
                        const ::chatnow::SetUserDescriptionReq* request,
                        ::chatnow::SetUserDescriptionRsp* response,
                        ::google::protobuf::Closure* done)
    {
        LOG_DEBUG("收到设置用户签名请求");
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };        
        //1. 从请求中取出用户ID与新签名
        std::string uid = request->user_id();
        std::string new_description = request->description();
        //2. 从数据库通过用户 ID 进行用户信息查询，判断用户是否存在
        auto user = _mysql_user->select_by_id(uid);
        if(!user) {
            LOG_ERROR("请求ID: {} - 用户不存在: {}", request->request_id(), uid);
            return err_response(request->request_id(), "用户不存在");            
        }
        //3. 将新的签名更新到数据库中
        user->description(new_description);
        bool ret = _mysql_user->update(user);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 更新数据库用户签名失败: {}", request->request_id(), new_description);
            return err_response(request->request_id(), "更新数据库用户签名失败");                
        }
        //4. 更新 ES 服务中，用户信息
        ret = _es_user->append_data(user->user_id(), user->mail(), user->nickname(), user->description(), user->avatar_id());
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 更新 ES搜索引擎 用户签名失败: {}", request->request_id(), user->description());
            return err_response(request->request_id(), "更新 ES搜索引擎 用户签名失败");                      
        }
        //5. 组织响应
        response->set_request_id(request->request_id());
        response->set_success(true);
    }
    /* brief: 设置邮箱 */
    virtual void SetUserMailNumber(::google::protobuf::RpcController* controller,
                        const ::chatnow::SetUserMailNumberReq* request,
                        ::chatnow::SetUserMailNumberRsp* response,
                        ::google::protobuf::Closure* done)
    {
        LOG_DEBUG("收到设置用户邮箱请求");
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &err_msg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(err_msg);
            return;
        };        
        //1. 从请求中取出用户ID与新昵称
        std::string uid = request->user_id();
        std::string new_mail = request->mail_number();
        std::string code_id = request->mail_verify_code_id();
        std::string code = request->mail_verify_code();
        //2. 对验证码进行验证
        auto verifyCode = _redis_codes->code(code_id);
        if(verifyCode != code) {
            LOG_ERROR("请求ID: {} - 验证码错误: {} - {}", request->request_id(), code_id, code);
            return err_response(request->request_id(), "验证码错误");            
        }
        //3. 从数据库通过用户 ID 进行用户信息查询，判断用户是否存在
        auto user = _mysql_user->select_by_id(uid);
        if(!user) {
            LOG_ERROR("请求ID: {} - 用户不存在: {}", request->request_id(), uid);
            return err_response(request->request_id(), "用户不存在");            
        }
        //4. 将新的昵称更新到数据库中
        user->mail(new_mail);
        bool ret = _mysql_user->update(user);
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 更新数据库用户邮箱失败: {}", request->request_id(), new_mail);
            return err_response(request->request_id(), "更新数据库用户邮箱失败");                
        }
        //5. 更新 ES 服务中，用户信息
        ret = _es_user->append_data(user->user_id(), user->mail(), user->nickname(), user->description(), user->avatar_id());
        if(ret == false) {
            LOG_ERROR("请求ID: {} - 更新 ES搜索引擎 用户邮箱失败: {}", request->request_id(), user->mail());
            return err_response(request->request_id(), "更新 ES搜索引擎 用户邮箱失败");                      
        }
        //6. 组织响应
        response->set_request_id(request->request_id());
        response->set_success(true);
    }
private:
    ESUser::ptr _es_user;
    UserTable::ptr _mysql_user;
    Session::ptr _redis_session;
    Status::ptr _redis_status;
    Codes::ptr _redis_codes;
    MailClient::ptr _mail_client;
    /* 以下是 RPC 调用客户端相关对象 */
    std::string _file_service_name;
    ServiceManager::ptr _mm_channels;
};

class UserServer
{
public:
    using ptr = std::shared_ptr<UserServer>;

    UserServer(const Discovery::ptr &service_discover,
            const Registry::ptr &reg_client,
            const std::shared_ptr<elasticlient::Client> &es_client,
            const std::shared_ptr<odb::core::database> &mysql_client,
            const std::shared_ptr<sw::redis::Redis> &redis_client,
            const std::shared_ptr<brpc::Server> &server) 
        : _service_discover(service_discover), 
        _reg_client(reg_client), 
        _es_client(es_client), 
        _mysql_client(mysql_client), 
        _redis_client(redis_client), 
        _rpc_server(server) {}
    ~UserServer() = default;
    /* brief: 搭建RPC服务器，并启动服务器 */
    void start() {
        _rpc_server->RunUntilAskedToQuit();
    }
private:
    Discovery::ptr _service_discover;
    Registry::ptr _reg_client;
    std::shared_ptr<brpc::Server> _rpc_server;
    std::shared_ptr<elasticlient::Client> _es_client;
    std::shared_ptr<odb::core::database> _mysql_client;
    std::shared_ptr<sw::redis::Redis> _redis_client;
};

/* 建造者模式: 将对象真正的构造过程封装，便于后期扩展和调整 */
class UserServerBuilder
{
public:
    /* brief: 构造es客户端对象 */
    void make_es_object(const std::vector<std::string> host_list) { _es_client = ESClientFactory::create(host_list); }
    /* brief: 构造mysql客户端对象 */
    void make_mysql_object(const std::string &user,
                        const std::string &password,
                        const std::string &host,
                        const std::string &db,
                        const std::string &cset,
                        uint16_t port,
                        int conn_pool_count)
    {
        _mysql_client = ODBFactory::create(user, password, host, db, cset, port, conn_pool_count);
    }
    /* brief: 构造redis客户端对象 */
    void make_redis_object(const std::string &host,
                        uint16_t port,
                        int db,
                        bool keep_alive)
    {
        _redis_client = RedisClientFactory::create(host, port, db, keep_alive);
    }
    /* brief: 构造邮箱验证客户端 */
    void make_mail_object(const std::string &mail_username, 
                        const std::string &mail_password,
                        const std::string &mail_url,
                        const std::string &mail_from)
    {
        mail_settings settings = {
            .username = mail_username,
            .password = mail_password,
            .url = mail_url,
            .from = mail_from
        };
        _mail_client = std::make_shared<MailClient>(settings);
    }
    /* brief: 用于构造服务发现&信道管理客户端对象 */
    void make_discovery_object(const std::string &reg_host, 
                            const std::string &base_service_name,
                            const std::string &file_service_name)
    {
        _file_service_name = file_service_name;
        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(_file_service_name);
        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(), std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(), std::placeholders::_1, std::placeholders::_2);

        _service_discover = std::make_shared<Discovery>(reg_host, base_service_name, put_cb, del_cb);
    }
    /* brief: 用于构造服务注册客户端对象 */
    void make_registry_object(const std::string &reg_host,
                        const std::string &service_name,
                        const std::string &access_host) {
        _reg_client = std::make_shared<Registry>(reg_host);
        _reg_client->registry(service_name, access_host);
    }
    /* brief: 构造RPC对象 */
    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        _rpc_server = std::make_shared<brpc::Server>();
        if(!_es_client) {
            LOG_ERROR("还未初始化ES搜索引擎模块");
            abort();
        }
        if(!_mysql_client) {
            LOG_ERROR("还未初始化MySQL数据库模块");
            abort();
        }
        if(!_redis_client) {
            LOG_ERROR("还未初始化Redis数据库模块");
            abort();
        }
        if(!_mail_client) {
            LOG_ERROR("还未初始化邮件验证客户端模块");
            abort();
        }
        if(!_mm_channels) {
            LOG_ERROR("还未初始化信道管理模块");
            abort();
        }

        UserServiceImpl *user_service = new UserServiceImpl(_es_client, _mysql_client, _redis_client, _mail_client, _mm_channels, _file_service_name);
        int ret = _rpc_server->AddService(user_service, brpc::ServiceOwnership::SERVER_OWNS_SERVICE);
        if(ret == -1) {
            LOG_ERROR("添加RPC服务失败!");
            abort();
        }

        brpc::ServerOptions options;
        options.idle_timeout_sec = timeout;
        options.num_threads = num_threads;
        ret = _rpc_server->Start(port, &options);
        if(ret == -1) {
            LOG_ERROR("服务启动失败!");
            abort();
        }
    }
    UserServer::ptr build() {
        if(!_service_discover) {
            LOG_ERROR("还未初始化服务发现模块");
            abort();
        }
        if(!_reg_client) {
            LOG_ERROR("还未初始化服务注册模块");
            abort();
        }
        if(!_rpc_server) {
            LOG_ERROR("还未初始化RPC服务器模块");
            abort();
        }

        UserServer::ptr server = std::make_shared<UserServer>(_service_discover, _reg_client, _es_client, _mysql_client, _redis_client, _rpc_server);
        return server;
    }
private:
    Registry::ptr _reg_client;

    std::shared_ptr<elasticlient::Client> _es_client;
    std::shared_ptr<odb::core::database> _mysql_client;
    std::shared_ptr<sw::redis::Redis> _redis_client;
    std::shared_ptr<MailClient> _mail_client;

    std::string _file_service_name;
    ServiceManager::ptr _mm_channels;
    Discovery::ptr _service_discover;

    std::shared_ptr<brpc::Server> _rpc_server;
};

}