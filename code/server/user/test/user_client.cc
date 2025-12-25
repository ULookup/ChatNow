#include "etcd.hpp"
#include "channel.hpp"
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <thread>
#include "utils.hpp"
#include "user.pb.h"
#include "base.pb.h"

DEFINE_bool(run_mode, false, "程序的运行模式 false-调试 ; true-发布");
DEFINE_string(log_file, "", "发布模式下，用于指定日志的输出文件");
DEFINE_int32(log_level, 0, "发布模式下，用于指定日志的输出等级");

DEFINE_string(etcd_host, "http://127.0.0.1:2379", "服务注册中心地址");
DEFINE_string(base_service, "/service", "服务监控根目录");
DEFINE_string(user_service, "/service/user_service", "服务监控根目录");

chatnow::ServiceManager::ptr user_channels;

chatnow::UserInfo user_info;

std::string login_ssid;
std::string new_nickname = "亲爱的猪妈妈";

//TEST(用户子服务测试, 用户注册测试) {
//    auto channel = user_channels->choose(FLAGS_user_service); //获取通信信道
//    ASSERT_TRUE(channel);
//
//    chatnow::UserRegisterReq req;
//    req.set_request_id(chatnow::uuid());
//    req.set_nickname(user_info.nickname());
//    req.set_password("123456");
//    chatnow::UserRegisterRsp rsp;
//    brpc::Controller cntl;
//    chatnow::UserService_Stub stub(channel.get());
//    stub.UserRegister(&cntl, &req, &rsp, nullptr);
//    ASSERT_FALSE(cntl.Failed());
//    ASSERT_TRUE(rsp.success());
//}

TEST(用户子服务测试, 用户登录测试) {
    auto channel = user_channels->choose(FLAGS_user_service); //获取通信信道
    ASSERT_TRUE(channel);

    chatnow::UserLoginReq req;
    req.set_request_id(chatnow::uuid());
    req.set_nickname(new_nickname);
    req.set_password("123456");
    chatnow::UserLoginRsp rsp;
    brpc::Controller cntl;
    chatnow::UserService_Stub stub(channel.get());
    stub.UserLogin(&cntl, &req, &rsp, nullptr);
    ASSERT_FALSE(cntl.Failed());
    ASSERT_TRUE(rsp.success());
    login_ssid = rsp.login_session_id();
}

TEST(用户子服务测试, 用户头像测试) {
    auto channel = user_channels->choose(FLAGS_user_service); //获取通信信道
    ASSERT_TRUE(channel);

    chatnow::SetUserAvatarReq req;
    req.set_request_id(chatnow::uuid());
    req.set_user_id(user_info.user_id());
    req.set_session_id(login_ssid);
    req.set_avatar(user_info.avatar());
    chatnow::SetUserAvatarRsp rsp;
    brpc::Controller cntl;
    chatnow::UserService_Stub stub(channel.get());
    stub.SetUserAvatar(&cntl, &req, &rsp, nullptr);
    ASSERT_FALSE(cntl.Failed());
    ASSERT_TRUE(rsp.success());
}

TEST(用户子服务测试, 用户签名测试) {
    auto channel = user_channels->choose(FLAGS_user_service); //获取通信信道
    ASSERT_TRUE(channel);

    chatnow::SetUserDescriptionReq req;
    req.set_request_id(chatnow::uuid());
    req.set_user_id(user_info.user_id());
    req.set_session_id(login_ssid);
    req.set_description(user_info.description());
    chatnow::SetUserDescriptionRsp rsp;
    brpc::Controller cntl;
    chatnow::UserService_Stub stub(channel.get());
    stub.SetUserDescription(&cntl, &req, &rsp, nullptr);
    ASSERT_FALSE(cntl.Failed());
    ASSERT_TRUE(rsp.success());
}

TEST(用户子服务测试, 用户昵称测试) {
    auto channel = user_channels->choose(FLAGS_user_service); //获取通信信道
    ASSERT_TRUE(channel);

    chatnow::SetUserNicknameReq req;
    req.set_request_id(chatnow::uuid());
    req.set_user_id(user_info.user_id());
    req.set_session_id(login_ssid);
    req.set_nickname(new_nickname);
    chatnow::SetUserNicknameRsp rsp;
    brpc::Controller cntl;
    chatnow::UserService_Stub stub(channel.get());
    stub.SetUserNickname(&cntl, &req, &rsp, nullptr);
    ASSERT_FALSE(cntl.Failed());
    ASSERT_TRUE(rsp.success());
}

void set_user_avatar(const std::string &uid, const std::string &avatar) {
    auto channel = user_channels->choose(FLAGS_user_service); //获取通信信道
    ASSERT_TRUE(channel);

    chatnow::SetUserAvatarReq req;
    req.set_request_id(chatnow::uuid());
    req.set_user_id(uid);
    req.set_session_id(login_ssid);
    req.set_avatar(avatar);
    chatnow::SetUserAvatarRsp rsp;
    brpc::Controller cntl;
    chatnow::UserService_Stub stub(channel.get());
    stub.SetUserAvatar(&cntl, &req, &rsp, nullptr);
}

std::string code_id;

void get_code() {
    auto channel = user_channels->choose(FLAGS_user_service); //获取通信信道
    ASSERT_TRUE(channel);

    chatnow::MailVerifyCodeReq req;
    req.set_request_id(chatnow::uuid());
    req.set_mail_number(user_info.mail());
    chatnow::MailVerifyCodeRsp rsp;
    brpc::Controller cntl;
    chatnow::UserService_Stub stub(channel.get());
    stub.GetMailVerifyCode(&cntl, &req, &rsp, nullptr);
    ASSERT_FALSE(cntl.Failed());
    ASSERT_TRUE(rsp.success());

    code_id = rsp.verify_code_id();
}

TEST(用户子服务测试, 用户信息获取测试) {
    auto channel = user_channels->choose(FLAGS_user_service); //获取通信信道
    ASSERT_TRUE(channel);

    chatnow::GetUserInfoReq req;
    req.set_request_id(chatnow::uuid());
    req.set_user_id(user_info.user_id());
    req.set_session_id(login_ssid);
    chatnow::GetUserInfoRsp rsp;
    brpc::Controller cntl;
    chatnow::UserService_Stub stub(channel.get());
    stub.GetUserInfo(&cntl, &req, &rsp, nullptr);
    ASSERT_FALSE(cntl.Failed());
    ASSERT_TRUE(rsp.success());
    ASSERT_EQ(user_info.user_id(), rsp.user_info().user_id());
    ASSERT_EQ(new_nickname, rsp.user_info().nickname());
    ASSERT_EQ(user_info.description(), rsp.user_info().description());
    ASSERT_EQ("", rsp.user_info().mail());
    ASSERT_EQ(user_info.avatar(), rsp.user_info().avatar());
}

TEST(用户子服务测试, 批量用户信息获取测试) {
    set_user_avatar("用户ID1", "小猪佩奇的头像数据");
    set_user_avatar("用户ID2", "小猪乔治的头像数据");
    auto channel = user_channels->choose(FLAGS_user_service); //获取通信信道
    ASSERT_TRUE(channel);

    chatnow::GetMultiUserInfoReq req;
    req.set_request_id(chatnow::uuid());
    req.add_users_id("c1f9-10706913-0000");
    req.add_users_id("用户ID1");
    req.add_users_id("用户ID2");
    chatnow::GetMultiUserInfoRsp rsp;
    brpc::Controller cntl;
    chatnow::UserService_Stub stub(channel.get());
    stub.GetMultiUserInfo(&cntl, &req, &rsp, nullptr);
    ASSERT_FALSE(cntl.Failed());
    ASSERT_TRUE(rsp.success());
    auto users_map = rsp.mutable_users_info();
    chatnow::UserInfo father_user = (*users_map)["c1f9-10706913-0000"];
    ASSERT_EQ(father_user.user_id(), "c1f9-10706913-0000");
    ASSERT_EQ(father_user.nickname(), "猪爸爸");
    ASSERT_EQ(father_user.description(), "");
    ASSERT_EQ(father_user.mail(), "");
    ASSERT_EQ(father_user.avatar(), "");
    chatnow::UserInfo p_user = (*users_map)["用户ID1"];
    ASSERT_EQ(p_user.user_id(), "用户ID1");
    ASSERT_EQ(p_user.nickname(), "小猪佩奇");
    ASSERT_EQ(p_user.description(), "这是一只小猪");
    ASSERT_EQ(p_user.mail(), "1234@qq.com");
    ASSERT_EQ(p_user.avatar(), "小猪佩奇的头像数据");
    chatnow::UserInfo q_user = (*users_map)["用户ID2"];
    ASSERT_EQ(q_user.user_id(), "用户ID2");
    ASSERT_EQ(q_user.nickname(), "小猪乔治");
    ASSERT_EQ(q_user.description(), "这是一只小小猪");
    ASSERT_EQ(q_user.mail(), "2345@qq.com");
    ASSERT_EQ(q_user.avatar(), "小猪乔治的头像数据");
}

TEST(用户子服务测试, 邮箱注册测试) {
    get_code();
    auto channel = user_channels->choose(FLAGS_user_service); //获取通信信道
    ASSERT_TRUE(channel);

    chatnow::MailRegisterReq req;
    req.set_request_id(chatnow::uuid());
    req.set_mail_number(user_info.mail());
    req.set_verify_code_id(code_id);
    std::string code;
    std::cin >> code;
    req.set_verify_code(code);
    chatnow::MailRegisterRsp rsp;
    brpc::Controller cntl;
    chatnow::UserService_Stub stub(channel.get());
    stub.MailRegister(&cntl, &req, &rsp, nullptr);
    ASSERT_FALSE(cntl.Failed());
    ASSERT_TRUE(rsp.success());
}

TEST(用户子服务测试, 邮箱登录测试) {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    get_code();
    auto channel = user_channels->choose(FLAGS_user_service); //获取通信信道
    ASSERT_TRUE(channel);

    chatnow::MailLoginReq req;
    req.set_request_id(chatnow::uuid());
    req.set_mail_number(user_info.mail());
    req.set_verify_code_id(code_id);
    std::cout << "邮箱登录时, 输入验证码: " << std::endl;
    std::string code;
    std::cin >> code;
    req.set_verify_code(code);
    chatnow::MailLoginRsp rsp;
    brpc::Controller cntl;
    chatnow::UserService_Stub stub(channel.get());
    stub.MailLogin(&cntl, &req, &rsp, nullptr);
    ASSERT_FALSE(cntl.Failed());
    ASSERT_TRUE(rsp.success());
    std::cout << "邮箱登录会话ID" << rsp.login_session_id() << std::endl;
}

TEST(用户子服务测试, 邮箱设置测试) {
    get_code();
    auto channel = user_channels->choose(FLAGS_user_service); //获取通信信道
    ASSERT_TRUE(channel);

    chatnow::SetUserMailNumberReq req;
    req.set_request_id(chatnow::uuid());
    std::cout << "邮箱设置时, 输入用户ID: " << std::endl;
    std::string user_id;
    std::cin >> user_id;
    req.set_user_id(user_id);
    req.set_mail_number("chbulookup@outlook.com");
    req.set_mail_verify_code_id(code_id);
    std::cout << "邮箱设置时, 输入验证码: " << std::endl;
    std::string code;
    std::cin >> code;
    req.set_mail_verify_code(code);
    chatnow::SetUserMailNumberRsp rsp;
    brpc::Controller cntl;
    chatnow::UserService_Stub stub(channel.get());
    stub.SetUserMailNumber(&cntl, &req, &rsp, nullptr);
    ASSERT_FALSE(cntl.Failed());
    ASSERT_TRUE(rsp.success());
}

int main(int argc, char *argv[])
{
    testing::InitGoogleTest(&argc, argv);
    google::ParseCommandLineFlags(&argc, &argv, true);
    chatnow::init_logger(FLAGS_run_mode, FLAGS_log_file, FLAGS_log_level);

    //1. 先构造 Rpc 信道管理对象
    user_channels = std::make_shared<chatnow::ServiceManager>();
    user_channels->declared(FLAGS_user_service);
    auto put_cb = std::bind(&chatnow::ServiceManager::onServiceOnline, user_channels.get(), std::placeholders::_1, std::placeholders::_2);
    auto del_cb = std::bind(&chatnow::ServiceManager::onServiceOffline, user_channels.get(), std::placeholders::_1, std::placeholders::_2);
    //2. 构造服务发现对象
    chatnow::Discovery::ptr dclient = std::make_shared<chatnow::Discovery>(FLAGS_etcd_host, FLAGS_base_service, put_cb, del_cb);

    user_info.set_nickname("猪妈妈");
    user_info.set_user_id("a62c-7f60fa42-0000");
    user_info.set_description("这是一个美丽的猪妈妈");
    user_info.set_mail("459096189@qq.com");
    user_info.set_avatar("猪妈妈头像数据");

    return RUN_ALL_TESTS();
}