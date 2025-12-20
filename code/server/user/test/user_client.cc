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

TEST(用户子服务测试, 用户注册测试) {
    auto channel = user_channels->choose(FLAGS_user_service); //获取通信信道
    ASSERT_TRUE(channel);
    user_info.set_nickname("猪妈妈");

    chatnow::UserRegisterReq req;
    req.set_request_id(chatnow::uuid());
    req.set_nickname(user_info.nickname());
    req.set_password("123456");
    chatnow::UserRegisterRsp rsp;
    brpc::Controller cntl;
    chatnow::UserService_Stub stub(channel.get());
    stub.UserRegister(&cntl, &req, &rsp, nullptr);
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


    return RUN_ALL_TESTS();
}