#pragma once

#include <brpc/server.h>
#include "dao/data_es.hpp"
#include "dao/data_redis.hpp"
#include "dao/mysql_message.hpp"
#include "dao/mysql_user_timeline.hpp"
#include "dao/mysql_chat_session_member.hpp"
#include "infra/etcd.hpp"
#include "infra/logger.hpp"
#include "utils/utils.hpp"
#include "mq/channel.hpp"
#include "mq/rabbitmq.hpp"

#include "message.hxx"
#include "user_timeline.hxx"

#include "base.pb.h"
#include "message.pb.h"
#include "file.pb.h"
#include "user.pb.h"
#include "chatsession.pb.h"
#include <atomic>
#include <chrono>
#include <thread>

namespace chatnow
{

class MessageServiceImpl : public chatnow::MsgStorageService
{
public:
    MessageServiceImpl(const std::string &file_service_name,
                        const std::string &user_service_name,
                        const std::string &chatsession_service_name,
                        const ServiceManager::ptr &channels,
                        const std::shared_ptr<elasticlient::Client> &es_client,
                        const std::shared_ptr<odb::core::database> &mysql_client,
                        const std::shared_ptr<Subscriber> &mq_subscriber,
                        const std::shared_ptr<Subscriber> &es_subscriber,
                        const declare_settings &db_settings,
                        const declare_settings &es_settings,
                        const SeqGen::ptr &seq_gen,
                        const Publisher::ptr &push_publisher = nullptr,
                        const PushOutbox::ptr &push_outbox = nullptr)
                        : _file_service_name(file_service_name),
                        _user_service_name(user_service_name),
                        _chatsession_service_name(chatsession_service_name),
                        _mm_channels(channels),
                        _es_client(std::make_shared<ESMessage>(es_client)),
                        _db(mysql_client),
                        _mysql_usertimeline_table(std::make_shared<UserTimeLineTable>(mysql_client)),
                        _mysql_message_table(std::make_shared<MessageTable>(mysql_client)),
                        _mysql_member_table(std::make_shared<ChatSessionMemberTable>(mysql_client)),
                        _mq_subscriber(mq_subscriber),
                        _es_subscriber(es_subscriber),
                        _db_settings(db_settings),
                        _es_settings(es_settings),
                        _seq_gen(seq_gen),
                        _push_publisher(push_publisher),
                        _push_outbox(push_outbox)
    {
        _es_client->createIndex();
    }
    ~MessageServiceImpl() {
        stop_outbox_reaper();
    }
    virtual void GetHistoryMsg(google::protobuf::RpcController* controller,
                       const ::chatnow::GetHistoryMsgReq* request,
                       ::chatnow::GetHistoryMsgRsp* response,
                       ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &errmsg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(errmsg);
            return;
        };
        //1. 提取关键要素： 会话ID，起始时间，结束时间
        std::string rid = request->request_id();
        std::string uid = request->user_id();
        std::string chat_ssid = request->chat_session_id();
        boost::posix_time::ptime stime = boost::posix_time::from_time_t(request->start_time());
        boost::posix_time::ptime etime = boost::posix_time::from_time_t(request->over_time());
        //2. 从Timeline数据库中获取该时间段内属于用户的消息ID
        auto timeline_list = _mysql_usertimeline_table->range(uid, chat_ssid, stime, etime);

        if(timeline_list.empty()) {
            response->set_request_id(rid);
            response->set_success(true);
            return;
        }
        //3. 提取message_id列表
        std::vector<unsigned long> msg_id_list;
        for(const auto &timeline : timeline_list) {
            msg_id_list.push_back(timeline.message_id());
        }
        //4. 通过ID列表去Message表查消息
        auto msg_list = _mysql_message_table->select_by_ids(msg_id_list);
        if(msg_list.empty()) {
            response->set_request_id(rid);
            response->set_success(true);
            return;
        }
        //5. 统计所有文件类型消息的文件ID，并从文件子服务进行批量文件下载
        std::unordered_set<std::string> file_id_list;
        for(const auto &msg : msg_list) {
            if(msg.file_id().empty()) continue;
            LOG_DEBUG("需要下载的文件ID: {}", msg.file_id());
            file_id_list.insert(msg.file_id());
        }
        std::unordered_map<std::string, std::string> file_data_list;
        bool ret = _GetFile(rid, file_id_list, file_data_list);
        if(ret == false) {
            LOG_ERROR("请求ID {} - 批量文件数据下载失败", rid);
            return err_response(rid, "批量文件数据下载失败");
        }
        //4. 统计所有消息的发送者用户ID，从用户子服务进行批量用户信息获取
        std::unordered_set<std::string> user_id_list;
        for(const auto &msg : msg_list) {
            user_id_list.insert(msg.user_id());
        }
        std::unordered_map<std::string, UserInfo> user_list;
        ret = _GetUser(rid, user_id_list, user_list);
        if(ret == false) {
            LOG_ERROR("请求ID {} - 批量用户信息获取失败", rid);
            return err_response(rid, "批量用户信息获取失败");
        }
        //5. 组织响应
        response->set_request_id(rid);
        response->set_success(true);
        for(const auto &msg : msg_list) {
            auto message_info = response->add_msg_list();
            message_info->set_message_id(msg.message_id());
            message_info->set_chat_session_id(msg.session_id());
            message_info->set_timestamp(boost::posix_time::to_time_t(msg.create_time()));
            message_info->mutable_sender()->CopyFrom(user_list[msg.user_id()]);
            switch(msg.message_type()) {
                case MessageType::STRING:
                    message_info->mutable_message()->set_message_type(MessageType::STRING);
                    message_info->mutable_message()->mutable_string_message()->set_content(msg.content());
                    break;
                case MessageType::IMAGE:
                    message_info->mutable_message()->set_message_type(MessageType::IMAGE);
                    message_info->mutable_message()->mutable_image_message()->set_file_id(msg.file_id());
                    message_info->mutable_message()->mutable_image_message()->set_image_content(file_data_list[msg.file_id()]);
                    break;
                case MessageType::FILE:
                    message_info->mutable_message()->set_message_type(MessageType::FILE);
                    message_info->mutable_message()->mutable_file_message()->set_file_id(msg.file_id());
                    message_info->mutable_message()->mutable_file_message()->set_file_size(msg.file_size());
                    message_info->mutable_message()->mutable_file_message()->set_file_name(msg.file_name());
                    message_info->mutable_message()->mutable_file_message()->set_file_contents(file_data_list[msg.file_id()]);
                    break;
                case MessageType::SPEECH:
                    message_info->mutable_message()->set_message_type(MessageType::FILE);
                    message_info->mutable_message()->mutable_file_message()->set_file_id(msg.file_id());
                    message_info->mutable_message()->mutable_file_message()->set_file_contents(file_data_list[msg.file_id()]);
                    break;
                default:
                    LOG_ERROR("消息类型错误");
                    return;
            }
        }
        return;
    }
    virtual void GetRecentMsg(google::protobuf::RpcController* controller,
                       const ::chatnow::GetRecentMsgReq* request,
                       ::chatnow::GetRecentMsgRsp* response,
                       ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &errmsg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(errmsg);
            return;
        };
        //1. 提取请求中的关键要素：请求ID，会话ID，要获取的消息数量
        std::string rid = request->request_id();
        std::string uid = request->user_id();
        std::string chat_ssid = request->chat_session_id();
        int msg_count = request->msg_count();
        //2. 从 Timeline 获取最近的消息 ID（小群写扩散路径）
        LOG_DEBUG("从用户 {} 的 Timeline 获取会话 {} 的最近 {} 条", uid, chat_ssid, msg_count);
        std::vector<UserTimeline> timeline_list =
            _mysql_usertimeline_table->list_session_latest(uid, chat_ssid, static_cast<size_t>(msg_count));

        std::vector<chatnow::Message> msg_list;
        if(!timeline_list.empty()) {
            std::vector<unsigned long> msg_id_list;
            msg_id_list.reserve(timeline_list.size());
            for(const auto &t : timeline_list) msg_id_list.push_back(t.message_id());
            msg_list = _mysql_message_table->select_by_ids(msg_id_list);
        } else {
            // 大群读扩散兜底：直接按 (session_id, seq_id) 取最近 N 条
            msg_list = _mysql_message_table->recent_by_seq(chat_ssid, static_cast<int>(msg_count));
        }
        if(msg_list.empty()) {
            response->set_request_id(rid);
            response->set_success(true);
            return;
        }

        std::unordered_set<std::string> file_id_list;
        for(const auto &msg : msg_list) {
            if(msg.file_id().empty()) continue;
            LOG_DEBUG("需要下载的文件ID: {}", msg.file_id());
            file_id_list.insert(msg.file_id());
        }
        std::unordered_map<std::string, std::string> file_data_list;
        bool ret = _GetFile(rid, file_id_list, file_data_list);
        if(ret == false) {
            LOG_ERROR("请求ID {} - 批量文件数据下载失败", rid);
            return err_response(rid, "批量文件数据下载失败");
        }
        //4. 组织消息中所有消息的用户ID，并从用户子服务进行用户信息查询
        std::unordered_set<std::string> user_id_set;
        for(const auto &msg : msg_list) {
            user_id_set.insert(msg.user_id());
        }
        std::unordered_map<std::string, UserInfo> user_map;
        ret = _GetUser(rid, user_id_set, user_map);
        if(ret == false) {
            LOG_ERROR("请求ID {} - 批量用户信息获取失败", rid);
            return err_response(rid, "批量用户信息获取失败");
        }
        //5. 组织响应
        response->set_request_id(rid);
        response->set_success(true);
        for(const auto &msg : msg_list) {
            auto message_info = response->add_msg_list();
            message_info->set_message_id(msg.message_id());
            message_info->set_chat_session_id(msg.session_id());
            message_info->set_seq_id(msg.seq_id());
            message_info->set_client_msg_id(msg.client_msg_id());
            message_info->set_timestamp(boost::posix_time::to_time_t(msg.create_time()));
            message_info->mutable_sender()->CopyFrom(user_map[msg.user_id()]);
            switch(msg.message_type()) {
                case MessageType::STRING:
                    LOG_DEBUG("消息是字符消息, 组织响应, 内容大小: {}", file_data_list[msg.file_id()].size());
                    message_info->mutable_message()->set_message_type(MessageType::STRING);
                    message_info->mutable_message()->mutable_string_message()->set_content(msg.content());
                    break;
                case MessageType::IMAGE:
                    LOG_DEBUG("消息是图像消息, 组织响应, 内容大小: {}", file_data_list[msg.file_id()].size());
                    message_info->mutable_message()->set_message_type(MessageType::IMAGE);
                    message_info->mutable_message()->mutable_image_message()->set_file_id(msg.file_id());
                    message_info->mutable_message()->mutable_image_message()->set_image_content(file_data_list[msg.file_id()]);
                    break;
                case MessageType::FILE:
                    LOG_DEBUG("消息是文件消息, 组织响应, 内容大小: {}", file_data_list[msg.file_id()].size());
                    message_info->mutable_message()->set_message_type(MessageType::FILE);
                    message_info->mutable_message()->mutable_file_message()->set_file_id(msg.file_id());
                    message_info->mutable_message()->mutable_file_message()->set_file_size(msg.file_size());
                    message_info->mutable_message()->mutable_file_message()->set_file_name(msg.file_name());
                    message_info->mutable_message()->mutable_file_message()->set_file_contents(file_data_list[msg.file_id()]);
                    break;
                case MessageType::SPEECH:
                    LOG_DEBUG("消息是语音消息, 组织响应, 内容大小: {}", file_data_list[msg.file_id()].size());
                    message_info->mutable_message()->set_message_type(MessageType::SPEECH);
                    message_info->mutable_message()->mutable_speech_message()->set_file_id(msg.file_id());
                    message_info->mutable_message()->mutable_speech_message()->set_file_contents(file_data_list[msg.file_id()]);
                    break;
                default:
                    LOG_ERROR("消息类型错误");
                    return;                
            }
        }
        return;
    }                   
    virtual void MsgSearch(google::protobuf::RpcController* controller,
                       const ::chatnow::MsgSearchReq* request,
                       ::chatnow::MsgSearchRsp* response,
                       ::google::protobuf::Closure* done) 
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &errmsg) -> void {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(errmsg);
            return;
        };
        //关键字消息搜索--只针对文本消息
        //1. 从请求中提取关键要素：请求ID，会话ID，关键字
        std::string rid = request->request_id();
        std::string chat_ssid = request->chat_session_id();
        std::string skey = request->search_key();
        //2. 从ES搜索引擎进行关键字消息搜索，得到消息列表
        auto msg_list = _es_client->search(skey, chat_ssid);
        if(msg_list.empty()) {
            response->set_request_id(rid);
            response->set_success(true);
            return;
        }
        //3. 组织所有消息的用户ID，从用户子服务获取用户信息
        std::unordered_set<std::string> user_id_list;
        for(const auto &msg : msg_list) {
            user_id_list.insert(msg.user_id());
        }
        std::unordered_map<std::string, UserInfo> user_list;
        bool ret = _GetUser(rid, user_id_list, user_list);
        if(ret == false) {
            LOG_ERROR("请求ID {} - 批量用户信息获取失败", rid);
            return err_response(rid, "批量用户信息获取失败");
        }
        //4. 组织响应
        response->set_request_id(rid);
        response->set_success(true);
        for(const auto &msg : msg_list) {
            auto message_info = response->add_msg_list();
            message_info->set_message_id(msg.message_id());
            message_info->set_chat_session_id(msg.session_id());
            message_info->set_timestamp(boost::posix_time::to_time_t(msg.create_time()));
            message_info->mutable_sender()->CopyFrom(user_list[msg.user_id()]);
            message_info->mutable_message()->set_message_type(MessageType::STRING);
            message_info->mutable_message()->mutable_string_message()->set_content(msg.content());
        }
        return;
    }
    /* brief: 获取离线消息 */
    virtual void GetOfflineMsg(google::protobuf::RpcController* controller,
                            const ::chatnow::GetOfflineMsgReq* request,
                            ::chatnow::GetOfflineMsgRsp* response,
                            ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        auto err_response = [this, response](const std::string &rid, const std::string &errmsg) {
            response->set_request_id(rid);
            response->set_success(false);
            response->set_errmsg(errmsg);
        };

        // 1. 提取参数（last_message_id 字段语义：客户端本地最大 user_seq）
        std::string rid = request->request_id();
        std::string user_id = request->user_id();
        unsigned long last_user_seq = static_cast<unsigned long>(request->last_message_id());
        int msg_count = request->msg_count();

        // 容错处理
        if(msg_count <= 0) msg_count = 50;
        if(msg_count > 1000) msg_count = 1000;

        // 2. 全局增量：按 user_seq > last_user_seq 拉取（多取一条用于 has_more 判断）
        std::vector<UserTimeline> timeline_list = _mysql_usertimeline_table->list_global_after(
            user_id, last_user_seq, msg_count + 1);

        // 3. 判断是否还有更多数据
        bool has_more = false;
        if (timeline_list.size() > msg_count) {
            has_more = true;
            timeline_list.pop_back(); // 移除多取的那一条，只返回客户端请求的数量
        }
        
        if(timeline_list.empty()) {
            response->set_request_id(rid);
            response->set_success(true);
            response->set_has_more(false);
            return;
        }

        // 4. 提取 Message ID 列表，并建立 message_id → user_seq 映射，
        //    保证响应中每条 MessageInfo.user_seq 与 timeline 的 user_seq 严格对齐
        //    （select_by_ids 内部按 seq_id ASC 排序，与 timeline 顺序不同，
        //     直接 by-index 配对会跨会话错配）
        std::vector<unsigned long> msg_id_list;
        msg_id_list.reserve(timeline_list.size());
        std::unordered_map<unsigned long, unsigned long> mid_to_user_seq;
        mid_to_user_seq.reserve(timeline_list.size());
        for(const auto &tl : timeline_list) {
            msg_id_list.push_back(tl.message_id());
            mid_to_user_seq[tl.message_id()] = tl.user_seq();
        }

        // 5. 批量查询消息正文
        auto msg_list = _mysql_message_table->select_by_ids(msg_id_list);
        if(msg_list.empty()) {
            // Timeline 有 ID 但 Message 表没数据（极少见的数据不一致）
            LOG_WARN("增量同步时 Timeline 存在数据但 Message 表缺失, User: {}, last_user_seq: {}", user_id, last_user_seq);
            response->set_request_id(rid);
            response->set_success(true);
            response->set_has_more(has_more); // 依然返回 has_more 状态，客户端可能需要跳过这些 ID
            return;
        }

        // 6. 批量下载文件数据 (标准流程)
        std::unordered_set<std::string> file_id_list;
        for(const auto &msg : msg_list) {
            if(!msg.file_id().empty()) file_id_list.insert(msg.file_id());
        }
        std::unordered_map<std::string, std::string> file_data_list;
        if(!file_id_list.empty()) {
            if(!_GetFile(rid, file_id_list, file_data_list)) {
                return err_response(rid, "增量同步: 文件数据获取失败");
            }
        }

        // 7. 批量获取用户信息 (标准流程)
        std::unordered_set<std::string> sender_id_list;
        for(const auto &msg : msg_list) {
            sender_id_list.insert(msg.user_id());
        }
        std::unordered_map<std::string, UserInfo> user_map;
        if(!_GetUser(rid, sender_id_list, user_map)) {
            return err_response(rid, "增量同步: 用户信息获取失败");
        }

        // 8. 组装响应
        response->set_request_id(rid);
        response->set_success(true);
        response->set_has_more(has_more);

        for(const auto &msg : msg_list) {
            auto info = response->add_msg_list();

            // 填充基础信息（含会话内 seq、客户端幂等 ID、收件人 user_seq）
            info->set_message_id(msg.message_id());
            info->set_chat_session_id(msg.session_id());
            info->set_seq_id(msg.seq_id());
            info->set_client_msg_id(msg.client_msg_id());
            info->set_timestamp(boost::posix_time::to_time_t(msg.create_time()));
            auto seq_it = mid_to_user_seq.find(msg.message_id());
            if(seq_it != mid_to_user_seq.end()) info->set_user_seq(seq_it->second);

            // 填充发送者信息
            if (user_map.find(msg.user_id()) != user_map.end()) {
                info->mutable_sender()->CopyFrom(user_map[msg.user_id()]);
            }

            // 填充消息内容 (复用之前的 switch-case 逻辑，建议封装成函数 fill_content)
            switch(msg.message_type()) {
                case MessageType::STRING:
                    info->mutable_message()->set_message_type(MessageType::STRING);
                    info->mutable_message()->mutable_string_message()->set_content(msg.content());
                    break;
                case MessageType::IMAGE:
                    info->mutable_message()->set_message_type(MessageType::IMAGE);
                    info->mutable_message()->mutable_image_message()->set_file_id(msg.file_id());
                    if(file_data_list.count(msg.file_id()))
                         info->mutable_message()->mutable_image_message()->set_image_content(file_data_list[msg.file_id()]);
                    break;
                case MessageType::FILE:
                    info->mutable_message()->set_message_type(MessageType::FILE);
                    info->mutable_message()->mutable_file_message()->set_file_id(msg.file_id());
                    info->mutable_message()->mutable_file_message()->set_file_size(msg.file_size());
                    info->mutable_message()->mutable_file_message()->set_file_name(msg.file_name());
                    if(file_data_list.count(msg.file_id()))
                        info->mutable_message()->mutable_file_message()->set_file_contents(file_data_list[msg.file_id()]);
                    break;
                case MessageType::SPEECH:
                    info->mutable_message()->set_message_type(MessageType::SPEECH);
                    info->mutable_message()->mutable_speech_message()->set_file_id(msg.file_id());
                    if(file_data_list.count(msg.file_id()))
                        info->mutable_message()->mutable_speech_message()->set_file_contents(file_data_list[msg.file_id()]);
                    break;
            }
        }
    }
    /* brief: 获取未读消息数量
     *  - last_read_msg_id 字段语义：客户端传过来的是会话内 last_read_seq（兼容旧字段名）
     *  - 走 (user_id, session_id, session_seq > last_read_seq) 的 count；O(log n)
     */
    virtual void GetUnreadCount(google::protobuf::RpcController* controller,
                                const ::chatnow::GetUnreadCountReq* request,
                                ::chatnow::GetUnreadCountRsp* response,
                                ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);

        std::string rid = request->request_id();
        std::string user_id = request->user_id();
        std::string chat_ssid = request->chat_session_id();
        unsigned long last_read_seq = static_cast<unsigned long>(request->last_read_msg_id());

        unsigned long latest_seq = _mysql_usertimeline_table->latest_session_seq(user_id, chat_ssid);
        int unread_count = 0;
        if(latest_seq > last_read_seq) {
            unread_count = _mysql_usertimeline_table->unread_count_by_seq(user_id, chat_ssid, last_read_seq);
        }

        response->set_request_id(rid);
        response->set_success(true);
        response->set_unread_count(unread_count);
        response->set_latest_msg_id(static_cast<int64_t>(latest_seq));

        LOG_DEBUG("请求ID {} - 未读: uid={} ssid={} last_read_seq={} unread={} latest_seq={}",
            rid, user_id, chat_ssid, last_read_seq, unread_count, latest_seq);
    }

    /* brief: 客户端幂等查询（Transmite 调用） */
    virtual void SelectByClientMsg(google::protobuf::RpcController* controller,
                                   const ::chatnow::SelectByClientMsgReq* request,
                                   ::chatnow::SelectByClientMsgRsp* response,
                                   ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        const std::string &rid = request->request_id();
        response->set_request_id(rid);

        if(request->client_msg_id().empty() || request->user_id().empty()) {
            response->set_success(true);
            response->set_exists(false);
            return;
        }
        auto msg = _mysql_message_table->select_by_client_msg(request->user_id(), request->client_msg_id());
        if(!msg) {
            response->set_success(true);
            response->set_exists(false);
            return;
        }
        // 命中：组装最小 MessageInfo 返回（用户看不到内容差异即可）
        auto *info = response->mutable_message();
        info->set_message_id(msg->message_id());
        info->set_chat_session_id(msg->session_id());
        info->set_seq_id(msg->seq_id());
        info->set_client_msg_id(msg->client_msg_id());
        info->set_timestamp(boost::posix_time::to_time_t(msg->create_time()));
        // sender 信息要查 user 服务才能填全；幂等返回时由 Transmite 决定是否再补
        info->mutable_sender()->set_user_id(msg->user_id());
        // 内容根据消息类型回填（仅文本/file_id；文件二进制由客户端自己拉）
        info->mutable_message()->set_message_type(msg->message_type());
        switch(msg->message_type()) {
            case MessageType::STRING:
                info->mutable_message()->mutable_string_message()->set_content(msg->content());
                break;
            case MessageType::IMAGE:
                info->mutable_message()->mutable_image_message()->set_file_id(msg->file_id());
                break;
            case MessageType::FILE:
                info->mutable_message()->mutable_file_message()->set_file_id(msg->file_id());
                info->mutable_message()->mutable_file_message()->set_file_name(msg->file_name());
                info->mutable_message()->mutable_file_message()->set_file_size(msg->file_size());
                break;
            case MessageType::SPEECH:
                info->mutable_message()->mutable_speech_message()->set_file_id(msg->file_id());
                break;
            default: break;
        }
        response->set_success(true);
        response->set_exists(true);
        LOG_DEBUG("SelectByClientMsg 命中 uid={} cid={} mid={}",
                  request->user_id(), request->client_msg_id(), msg->message_id());
    }

    /* brief: ACK 收敛（Push 服务收到客户端 ACK 后调用）
     *  - 仅向前推进 last_ack_seq；不会回退（DAO 走 SELECT FOR UPDATE 单调推进）
     *  - 入参非法 → 直接拒绝；DAO 失败 → 返回 false 让调用方重试
     */
    virtual void UpdateAckSeq(google::protobuf::RpcController* controller,
                              const ::chatnow::UpdateAckSeqReq* request,
                              ::chatnow::UpdateAckSeqRsp* response,
                              ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        response->set_request_id(request->request_id());
        if(request->user_id().empty() || request->chat_session_id().empty() ||
           request->user_seq() == 0) {
            response->set_success(false);
            response->set_errmsg("invalid args: user_id/chat_session_id/user_seq required");
            return;
        }
        bool ok = _mysql_member_table->update_last_ack_seq(
            request->chat_session_id(), request->user_id(), request->user_seq());
        response->set_success(ok);
        if(!ok) response->set_errmsg("update last_ack_seq failed");
    }
    //================================================================================================//
    //=========================================== 消费回调 ============================================//
    //================================================================================================//
    /* brief: DB 消费：写 message 主表 + (写扩散群) user_timeline
     *  - 文件上传由客户端前置完成，本路径仅落 file_id
     *  - 大群（is_large_group=true）启用读扩散，仅写 message 主表
     */
    ConsumeAction onDBMessage(const char *body, size_t sz, bool redelivered) {
        LOG_DEBUG("收到新消息，进行存储处理！redelivered={}", redelivered);
        //1. 反序列化
        chatnow::InternalMessage internal_msg;
        if(!internal_msg.ParseFromArray(body, sz)) {
            LOG_ERROR("DB-Consumer: 反序列化失败");
            return ConsumeAction::NackDiscard;
        }

        const auto &msg_info = internal_msg.message_info();
        const unsigned long mid = msg_info.message_id();
        const unsigned long session_seq = msg_info.seq_id();
        const std::string &client_msg_id = msg_info.client_msg_id();

        //2. 提取消息体（文件已由客户端前置上传，仅取 file_id / 文本内容）
        std::string file_id, file_name, content;
        int64_t file_size = 0;
        auto msg_type = msg_info.message().message_type();
        switch(msg_type) {
            case MessageType::STRING:
                content = msg_info.message().string_message().content();
                break;
            case MessageType::IMAGE:
                file_id = msg_info.message().image_message().file_id();
                break;
            case MessageType::FILE:
                file_id = msg_info.message().file_message().file_id();
                file_name = msg_info.message().file_message().file_name();
                file_size = msg_info.message().file_message().file_size();
                break;
            case MessageType::SPEECH:
                file_id = msg_info.message().speech_message().file_id();
                break;
            default:
                LOG_ERROR("DB-Consumer: 未知消息类型 mid={}", mid);
                return ConsumeAction::NackDiscard;
        }
        // 文件类型必须带 file_id（客户端前置上传契约）
        if(msg_type != MessageType::STRING && file_id.empty()) {
            LOG_ERROR("DB-Consumer: 非文本消息缺少 file_id mid={}", mid);
            return ConsumeAction::NackDiscard;
        }

        //3. 组装 Message 对象
        chatnow::Message msg(mid,
                            msg_info.chat_session_id(),
                            msg_info.sender().user_id(),
                            msg_info.message().message_type(),
                            boost::posix_time::from_time_t(msg_info.timestamp()),
                            MessageStatus::NORMAL);
        msg.seq_id(session_seq);
        msg.content(content);
        msg.file_id(file_id);
        msg.file_name(file_name);
        msg.file_size(file_size);
        if(!client_msg_id.empty()) msg.client_msg_id(client_msg_id);

        //4. 写扩散：组装 user_timeline 列表（仅小群；大群跳过实现读扩散）
        std::vector<UserTimeline> timeline_list;
        if(!internal_msg.is_large_group()) {
            // 把 user_seqs 整理为 map 便于 O(1) 查找
            std::unordered_map<std::string, unsigned long> uid2seq;
            for(const auto &p : internal_msg.user_seqs()) {
                uid2seq[p.user_id()] = p.user_seq();
            }
            timeline_list.reserve(internal_msg.member_id_list_size());
            auto msg_pt = boost::posix_time::from_time_t(msg_info.timestamp());
            for(const auto &member : internal_msg.member_id_list()) {
                UserTimeline tl;
                tl.user_id(member);
                tl.session_id(msg_info.chat_session_id());
                tl.message_id(mid);
                tl.message_time(msg_pt);
                tl.session_seq(session_seq);
                auto it = uid2seq.find(member);
                if(it != uid2seq.end()) tl.user_seq(it->second);
                timeline_list.push_back(std::move(tl));
            }
        }

        //5. 事务落库
        try {
            odb::transaction trans(_db->begin());
            _mysql_message_table->insert(msg);
            if(!timeline_list.empty()) {
                _mysql_usertimeline_table->insert(timeline_list);
            }
            trans.commit();

            LOG_INFO("DB-Consumer: 消息落库 mid={} seq={} large={} timeline_count={}",
                     mid, session_seq, internal_msg.is_large_group(), timeline_list.size());

            // 6. 落库成功后投递到 push_queue（fire-and-forget；推送服务消费）
            //    投递失败 → 落 PushOutbox（Redis ZSET），由独立 reaper 定期重投，避免推送丢失
            if(_push_publisher) {
                std::string payload = internal_msg.SerializeAsString();
                auto outbox = _push_outbox;  // 拷一份引用进 lambda
                long long now_ts = static_cast<long long>(time(nullptr));
                try {
                    _push_publisher->publish_confirm(payload,
                        [mid, payload, outbox, now_ts](PublishStatus status, const std::string &err) {
                            if(status != PublishStatus::Acked) {
                                LOG_WARN("Push-Publisher: 投递 push_queue 失败 mid={} err={}, 入 outbox", mid, err);
                                if(outbox) outbox->enqueue(payload, now_ts);
                            }
                        });
                } catch(std::exception &e) {
                    LOG_WARN("Push-Publisher: 同步异常 mid={} err={}, 入 outbox", mid, e.what());
                    if(_push_outbox) _push_outbox->enqueue(payload, now_ts);
                }
            }
            return ConsumeAction::Ack;
        } catch(const odb::object_already_persistent &e) {
            // 唯一索引冲突（uk_session_seq / uk_client_msg）→ MQ 重投导致的重复消费，幂等丢弃
            LOG_WARN("DB-Consumer: 重复消息（唯一索引命中），幂等丢弃 mid={} client_msg_id={} err={}",
                     mid, client_msg_id, e.what());
            return ConsumeAction::Ack;
        } catch(std::exception &e) {
            // redelivered=true 表示这条消息已经被 broker 重新投递过 → 上一次明确失败
            // 再次失败则视为永久错误，进 DLX（避免 hot spin）
            if(redelivered) {
                LOG_ERROR("DB-Consumer: 二次失败转 DLX mid={} err={}", mid, e.what());
                return ConsumeAction::NackDiscard;
            }
            LOG_ERROR("DB-Consumer: 事务失败（首次），重投 mid={} err={}", mid, e.what());
            return ConsumeAction::NackRequeue;
        }
    }

    /* M3: PushOutbox reaper —
     *   - 单独线程，每 kReapIntervalSec 唤醒；
     *   - Redis SET NX EX 单实例租约（kLeaseTtlSec）保证多副本只一个实例真正消费；
     *   - 从 outbox ZSET 取 kBatchLimit 条失败 payload，重投到 push_publisher；
     *   - peek-then-zrem 防同批反复重投；publish_confirm 失败回调里重新 enqueue + bump 时间戳。
     */
    void start_outbox_reaper(const std::string &owner) {
        if(!_push_outbox || !_push_publisher) {
            LOG_WARN("PushOutbox reaper 未启动：outbox / publisher 未注入");
            return;
        }
        constexpr int kReapIntervalSec = 5;
        constexpr int kLeaseTtlSec     = 30;
        constexpr int kBatchLimit      = 50;
        _reaper_running.store(true);
        _reaper_owner = owner;
        _reaper_thread = std::thread([this]() {
            while(_reaper_running.load()) {
                try {
                    if(!_push_outbox->try_acquire_reaper_lease(_reaper_owner, kLeaseTtlSec)) {
                        std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
                        continue;
                    }
                    auto batch = _push_outbox->peek(kBatchLimit);
                    if(batch.empty()) {
                        std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
                        continue;
                    }
                    LOG_INFO("PushOutbox reaper: 取出 {} 条失败投递准备重投", batch.size());
                    for(const auto &payload : batch) _push_outbox->remove(payload);
                    auto outbox = _push_outbox;
                    for(const auto &payload : batch) {
                        std::string p = payload;
                        _push_publisher->publish_confirm(p,
                            [outbox, p](PublishStatus status, const std::string &err) {
                                if(status == PublishStatus::Acked) return;
                                LOG_WARN("PushOutbox reaper 重投仍失败，回排入 outbox：{}", err);
                                if(outbox) outbox->enqueue(
                                    p, static_cast<long long>(time(nullptr)));
                            });
                    }
                } catch(std::exception &e) {
                    LOG_ERROR("PushOutbox reaper 异常: {}", e.what());
                } catch(...) {
                    LOG_ERROR("PushOutbox reaper 未知异常");
                }
                std::this_thread::sleep_for(std::chrono::seconds(kReapIntervalSec));
            }
            if(_push_outbox) _push_outbox->release_reaper_lease(_reaper_owner);
            LOG_INFO("PushOutbox reaper 已停止");
        });
    }

    void stop_outbox_reaper() {
        _reaper_running.store(false);
        if(_reaper_thread.joinable()) _reaper_thread.join();
    }

    ConsumeAction onESMessage(const char *body, size_t sz, bool redelivered) {
        // 1. 反序列化 Protobuf
        chatnow::InternalMessage internal_msg;
        if (!internal_msg.ParseFromArray(body, sz)) {
            LOG_ERROR("ES-Consumer: 反序列化失败，无法写入索引");
            return ConsumeAction::NackDiscard; // 反序列化失败，丢弃消息（可能是格式问题，不适合重试）
        }

        const auto &msg_info = internal_msg.message_info();
        
        // 2. 过滤：非文本消息不进 ES
        if (msg_info.message().message_type() != chatnow::MessageType::STRING) {
            return ConsumeAction::Ack; // 非文本消息，直接确认消费，不写入 ES
        }

        // 3. 提取字段
        // 注意：确保 Proto 定义中字段类型与 appendData 参数类型兼容
        std::string user_id = msg_info.sender().user_id();
        unsigned long message_id = msg_info.message_id();
        long create_time = msg_info.timestamp();
        std::string session_id = msg_info.chat_session_id();
        std::string content = msg_info.message().string_message().content();

        // 4. 调用你的封装接口写入数据
        bool ret = _es_client->appendData(
            user_id, 
            message_id, 
            create_time, 
            session_id, 
            content
        );

        if (!ret) {
            // 二次失败 → 进 DLX 由 fail-store 处理，避免长时间循环重投
            if(redelivered) {
                LOG_ERROR("[ES-Consumer] 二次失败转 DLX mid={}", message_id);
                return ConsumeAction::NackDiscard;
            }
            LOG_ERROR("[ES-Consumer] 写入 ES 失败（首次），重投: MsgID={}", message_id);
            return ConsumeAction::NackRequeue;
        } else {
            LOG_DEBUG("[ES-Consumer] 索引成功: MsgID={}", message_id);
            return ConsumeAction::Ack;
        }
    }
private:
    bool _GetUser(const std::string &rid,
                const std::unordered_set<std::string> &user_id_list,
                std::unordered_map<std::string, UserInfo> &user_list)
    {
        auto channel = _mm_channels->choose(_user_service_name);
        if(!channel) {
            LOG_ERROR("{} 没有可供访问的用户子服务节点", _user_service_name);
            return false;
        }
        UserService_Stub stub(channel.get());
        GetMultiUserInfoReq req;
        GetMultiUserInfoRsp rsp;
        req.set_request_id(rid);
        for(const auto &id : user_id_list) {
            req.add_users_id(id);
        }
        brpc::Controller cntl;
        stub.GetMultiUserInfo(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed() == true || rsp.success() == false) {
            LOG_ERROR("用户子服务调用失败: {}", cntl.ErrorText());
            return false;
        }
        const auto &umap = rsp.users_info();
        for(auto it = umap.begin(); it != umap.end(); ++it) {
            user_list.insert(std::make_pair(it->first, it->second));
        }
        return true;
    }

    bool _GetFile(const std::string &rid, 
                std::unordered_set<std::string> &file_id_list, 
                std::unordered_map<std::string, std::string> &file_data_list) 
    {
        auto channel = _mm_channels->choose(_file_service_name);
        if(!channel) {
            LOG_ERROR("{} 没有可供访问的文件子服务节点", _file_service_name);
            return false;
        }
        FileService_Stub stub(channel.get());
        GetMultiFileReq req;
        GetMultiFileRsp rsp;
        req.set_request_id(rid);
        for(const auto &id : file_id_list) {
            req.add_file_id_list(id);
        }
        brpc::Controller cntl;
        stub.GetMultiFile(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed() == true || rsp.success() == false) {
            LOG_ERROR("文件子服务调用失败: {}", req.request_id());
            return false;
        }
        const auto &fmap = rsp.file_data();
        for(auto it = fmap.begin(); it != fmap.end(); ++it) {
            file_data_list.insert(std::make_pair(it->first, it->second.file_content()));
        }
        return true;
    }

    bool _PutFile(const std::string &filename, const std::string &body, const int64_t fsize, std::string &file_id) {
        //实现文件数据的上传
        auto channel = _mm_channels->choose(_file_service_name);
        if(!channel) {
            LOG_ERROR("{} 没有可供访问的文件子服务节点", _file_service_name);
            return false;
        }
        FileService_Stub stub(channel.get());
        PutSingleFileReq req;
        PutSingleFileRsp rsp;
        req.mutable_file_data()->set_file_name(filename);
        req.mutable_file_data()->set_file_size(fsize);
        req.mutable_file_data()->set_file_content(body);
        brpc::Controller cntl;
        stub.PutSingleFile(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed() == true || rsp.success() == false) {
            LOG_ERROR("文件子服务调用失败: {}", cntl.ErrorText());
            return false;
        }
        file_id = rsp.file_info().file_id();
        return true;
    }
private:
    std::string _file_service_name;
    std::string _user_service_name;
    std::string _chatsession_service_name;
    std::shared_ptr<odb::core::database> _db;
    MessageTable::ptr _mysql_message_table;
    UserTimeLineTable::ptr _mysql_usertimeline_table;
    ChatSessionMemberTable::ptr _mysql_member_table;
    ESMessage::ptr _es_client;
    Subscriber::ptr _mq_subscriber;
    Subscriber::ptr _es_subscriber;
    declare_settings _db_settings;
    declare_settings _es_settings;
    ServiceManager::ptr _mm_channels;
    SeqGen::ptr _seq_gen;            // 启动时回填 / 推送链路重传
    Publisher::ptr _push_publisher;  // 写完 timeline 后向 push_queue 投递
    PushOutbox::ptr _push_outbox;    // push_queue 投递失败兜底

    // M3: outbox reaper 状态
    std::atomic<bool> _reaper_running {false};
    std::thread _reaper_thread;
    std::string _reaper_owner;
};

class MessageServer
{
public:
    using ptr = std::shared_ptr<MessageServer>;

    MessageServer(const Discovery::ptr &service_discover,
                const Registry::ptr &reg_client,
                const MQClient::ptr &mq_client,
                const std::shared_ptr<elasticlient::Client> &es_client,
                const std::shared_ptr<odb::core::database> &mysql_client,
                const std::shared_ptr<brpc::Server> &server,
                MessageServiceImpl *service_impl)
        : _service_discover(service_discover),
        _reg_client(reg_client),
        _mq_client(mq_client),
        _es_client(es_client),
        _mysql_client(mysql_client),
        _rpc_server(server),
        _service_impl(service_impl) {}
    ~MessageServer() = default;
    /* M3: 按顺序退出，避免 ev 线程访问已 delete 的 MessageServiceImpl
     *  1) RunUntilAskedToQuit 返回（brpc 已开始 Stop）
     *  2) 先停 reaper 主线程（不再发起 publish_confirm）
     *  3) 释放 mq_client → MQClient 析构关闭 channel + join ev 线程，未决回调丢弃
     *  4) brpc Join 等 in-flight RPC，brpc::Server 析构再 delete impl
     */
    void start() {
        _rpc_server->RunUntilAskedToQuit();
        // 关停顺序：reaper → MQ ev 线程 → brpc Join → brpc::Server 析构 delete impl
        if(_service_impl) _service_impl->stop_outbox_reaper();
        _mq_client.reset();
        _rpc_server->Join();
        LOG_INFO("Message 关停完成");
    }
private:
    Discovery::ptr _service_discover;
    Registry::ptr _reg_client;
    MQClient::ptr _mq_client;
    std::shared_ptr<elasticlient::Client> _es_client;
    std::shared_ptr<odb::core::database> _mysql_client;
    std::shared_ptr<brpc::Server> _rpc_server;
    MessageServiceImpl *_service_impl {nullptr};  // 仅观察指针（owned by brpc）
};

/* 建造者模式: 将对象真正的构造过程封装，便于后期扩展和调整 */
class MessageServerBuilder
{
public:
    /* brief: 构造es客户端对象 */
    void make_es_object(const std::vector<std::string> host_list) { _es_client = ESClientFactory::create(host_list); }
    /* brief: 构造 Redis 客户端 + SeqGen + PushOutbox */
    void make_redis_object(const std::string &host, uint16_t port, int db,
                           bool keep_alive, int pool_size)
    {
        _redis = RedisClientFactory::create(host, port, db, keep_alive, pool_size);
        _seq_gen = std::make_shared<SeqGen>(_redis);
        _push_outbox = std::make_shared<PushOutbox>(_redis);
    }
    /* brief: 构造推送队列 Publisher（写 timeline 后向 push_queue 投递） */
    void make_push_publisher(const std::string &exchange,
                             const std::string &queue,
                             const std::string &binding_key)
    {
        if(!_mq_client) {
            LOG_WARN("MQ 未初始化，跳过 push publisher");
            return;
        }
        _push_settings = {
            .exchange = exchange,
            .exchange_type = chatnow::DIRECT,
            .queue = queue,
            .binding_key = binding_key
        };
        _push_publisher = std::make_shared<Publisher>(_mq_client, _push_settings);
    }
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
    /* brief: 用于构造服务发现&信道管理客户端对象 */
    void make_discovery_object(const std::string &reg_host, 
                            const std::string &base_service_name,
                            const std::string &file_service_name,
                            const std::string &user_service_name,
                            const std::string &chatsession_service_name)
    {
        _file_service_name = file_service_name;
        _user_service_name = user_service_name;
        _chatsession_service_name = chatsession_service_name;
        _mm_channels = std::make_shared<ServiceManager>();
        _mm_channels->declared(_file_service_name);
        _mm_channels->declared(_user_service_name);
        auto put_cb = std::bind(&ServiceManager::onServiceOnline, _mm_channels.get(), std::placeholders::_1, std::placeholders::_2);
        auto del_cb = std::bind(&ServiceManager::onServiceOffline, _mm_channels.get(), std::placeholders::_1, std::placeholders::_2);

        _service_discover = std::make_shared<Discovery>(reg_host, base_service_name, put_cb, del_cb);
    }
    /* brief: 用于构造服务注册客户端对象 */
    void make_reg_object(const std::string &reg_host,
                        const std::string &service_name,
                        const std::string &access_host) {
        _reg_client = std::make_shared<Registry>(reg_host);
        _reg_client->registry(service_name, access_host);
    }
    /* brief: 构造 RabbitMQ 客户端对象
     * 契约：exchange_name 必须与 transmite 服务的 mq_msg_exchange 完全一致（FANOUT 路由依赖 exchange 名）
     */
    void make_mq_object(const std::string &user,
                    const std::string &password,
                    const std::string &host,
                    const std::string &exchange_name,
                    const std::string &queue_name_db,
                    const std::string &queue_name_es,
                    const std::string &binding_key_db,
                    const std::string &binding_key_es)
    {
        if(exchange_name.empty()) {
            LOG_ERROR("Message MQ exchange 不能为空，必须与 transmite 服务的 mq_msg_exchange 对齐");
            abort();
        }
        if(queue_name_db.empty() || queue_name_es.empty()) {
            LOG_ERROR("Message MQ queue 不能为空: db={} es={}", queue_name_db, queue_name_es);
            abort();
        }
        LOG_INFO("Message MQ 已就绪: exchange={} (FANOUT) db_queue={} es_queue={}",
                 exchange_name, queue_name_db, queue_name_es);
        std::string amqp_url = "amqp://" + user + ":" + password + "@" + host + ":5672/";
        _mq_client = std::make_shared<MQClient>(amqp_url);
        //声明 DB 队列
        _db_queue_settings = {
            .exchange = exchange_name,
            .exchange_type = chatnow::FANOUT,
            .queue = queue_name_db,
            .binding_key = binding_key_db
        };
        _es_queue_settings = {
            .exchange = exchange_name,
            .exchange_type = chatnow::FANOUT,
            .queue = queue_name_es,
            .binding_key = binding_key_es
        };

        auto dummy_cb = [](const char*, size_t, bool) -> chatnow::ConsumeAction { 
            return chatnow::ConsumeAction::Ack; 
        };

        _subscriber_db = chatnow::MQFactory::create<chatnow::Subscriber>(_mq_client, _db_queue_settings, dummy_cb);
        _subscriber_es = chatnow::MQFactory::create<chatnow::Subscriber>(_mq_client, _es_queue_settings, dummy_cb);
    }
    /* brief: 构造RPC服务器对象，并添加服务 */
    void make_rpc_object(uint16_t port, uint32_t timeout, uint8_t num_threads) {
        if(!_mm_channels) {
            LOG_ERROR("还未初始化信道管理模块");
            abort();
        }
        if(!_es_client) {
            LOG_ERROR("还未初始化ES搜索引擎模块");
            abort();
        }
        if(!_mysql_client) {
            LOG_ERROR("还未初始化MySQL数据库模块");
            abort();
        }
        if(!_subscriber_db || !_subscriber_es || !_mq_client) {
            LOG_ERROR("还未初始化消息队列客户端模块");
            abort();
        }
        if(!_seq_gen) {
            LOG_ERROR("还未初始化 SeqGen / Redis");
            abort();
        }
        _rpc_server = std::make_shared<brpc::Server>();
        MessageServiceImpl *message_service = new MessageServiceImpl(
            _file_service_name, _user_service_name, _chatsession_service_name,
            _mm_channels, _es_client, _mysql_client,
            _subscriber_db, _subscriber_es,
            _db_queue_settings, _es_queue_settings,
            _seq_gen, _push_publisher, _push_outbox);
        _service_impl = message_service;  // 观察指针，build() 时透传给 MessageServer
        int ret = _rpc_server->AddService(message_service, brpc::ServiceOwnership::SERVER_OWNS_SERVICE);
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
        auto callback_db = std::bind(&MessageServiceImpl::onDBMessage, message_service, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
        auto callback_es = std::bind(&MessageServiceImpl::onESMessage, message_service, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);

        LOG_INFO("开始向 Broker 订阅消费者队列...");
        _subscriber_db->consume(std::move(callback_db));
        _subscriber_es->consume(std::move(callback_es));
        LOG_INFO("队列订阅完成，服务全面启动！");

        // M3: 启动 PushOutbox reaper（多副本只一个实例真正消费，靠 Redis SET NX EX 选主）
        std::string owner = _reaper_owner.empty()
            ? std::to_string(::getpid()) : _reaper_owner;
        message_service->start_outbox_reaper(owner);
    }
    /* brief: 设置 reaper owner 标识（access_host:pid 等），用于多实例租约辨识 */
    void set_reaper_owner(const std::string &owner) { _reaper_owner = owner; }
    MessageServer::ptr build() {
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

        // M3 析构序：与 push 服务同样把 mq_client 等强引用 move 给 MessageServer，
        // 让 main 销毁 builder 时不再拖着 ev 线程；MessageServer::start() 末尾的
        // _mq_client.reset() 才能真正触发 ev 线程退出，再 brpc Join 安全析构 impl。
        MessageServer::ptr server = std::make_shared<MessageServer>(
            std::move(_service_discover),
            std::move(_reg_client),
            std::move(_mq_client),
            std::move(_es_client),
            std::move(_mysql_client),
            std::move(_rpc_server),
            _service_impl);
        return server;
    }
private:
    std::string _file_service_name;
    std::string _user_service_name;
    std::string _chatsession_service_name;
    ServiceManager::ptr _mm_channels;

    declare_settings _db_queue_settings;
    declare_settings _es_queue_settings;
    declare_settings _push_settings;
    MQClient::ptr _mq_client;
    Subscriber::ptr _subscriber_db;
    Subscriber::ptr _subscriber_es;
    Publisher::ptr _push_publisher;
    std::shared_ptr<sw::redis::Redis> _redis;
    SeqGen::ptr _seq_gen;
    PushOutbox::ptr _push_outbox;
    std::string _reaper_owner;
    MessageServiceImpl *_service_impl {nullptr};  // brpc 拥有，仅观察指针用

    Discovery::ptr _service_discover;   // 服务发现客户端
    Registry::ptr _reg_client;          // 服务注册客户端
    std::shared_ptr<elasticlient::Client> _es_client;
    std::shared_ptr<odb::core::database> _mysql_client; // mysql 数据库客户端
    std::shared_ptr<brpc::Server> _rpc_server;
};

}
