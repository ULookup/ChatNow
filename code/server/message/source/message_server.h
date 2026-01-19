#pragma once

#include <brpc/server.h>
#include "data_es.hpp"
#include "mysql_message.hpp"
#include "mysql_user_timeline.hpp"
#include "etcd.hpp"
#include "logger.hpp"
#include "utils.hpp"
#include "channel.hpp"
#include "rabbitmq.hpp"

#include "message.hxx"
#include "user_timeline.hxx"

#include "base.pb.h"
#include "message.pb.h"
#include "file.pb.h"
#include "user.pb.h"
#include "chatsession.pb.h"

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
                        const std::shared_ptr<odb::core::database> &mysql_client) 
                        : _file_service_name(file_service_name),
                        _user_service_name(user_service_name),
                        _chatsession_service_name(chatsession_service_name),
                        _mm_channels(channels),
                        _es_client(std::make_shared<ESMessage>(es_client)),
                        _db(mysql_client),
                        _mysql_usertimeline_table(std::make_shared<UserTimeLineTable>(mysql_client)),
                        _mysql_message_table(std::make_shared<MessageTable>(mysql_client)) 
    {
        _es_client->createIndex();
    }
    ~MessageServiceImpl() = default;
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
        //2. 从Timeline获取最近的消息ID列表
        LOG_DEBUG("从用户 {} 的 Timeline 获取会话 {} 的最近消息ID", uid, chat_ssid);
        std::vector<UserTimeline> timeline_list = _mysql_usertimeline_table->list_latest(uid, chat_ssid, msg_count);
        if(timeline_list.empty()) {
            response->set_request_id(rid);
            response->set_success(true);
            return;
        }
        //2.1 提取所有的message_id
        std::vector<unsigned long> msg_id_list;
        for(const auto &timeline : timeline_list) {
            msg_id_list.push_back(timeline.message_id());
        }
        //3. 用message_id去Message表拉取真正的元信息
        auto msg_list = _mysql_message_table->select_by_ids(msg_id_list);

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

        // 1. 提取参数
        std::string rid = request->request_id();
        std::string user_id = request->user_id();
        unsigned long last_msg_id = request->last_message_id();
        int msg_count = request->msg_count();
        
        // 容错处理
        if(msg_count <= 0) msg_count = 50;
        if(msg_count > 1000) msg_count = 1000;

        // 2. 从 Timeline 全局获取消息 ID (多取一条用于判断 has_more)
        // 注意：这里调用的是上面新增的 list_global_after
        std::vector<UserTimeline> timeline_list = _mysql_usertimeline_table->list_global_after(
            user_id, last_msg_id, msg_count + 1);

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

        // 4. 提取 Message ID 列表
        std::vector<unsigned long> msg_id_list;
        msg_id_list.reserve(timeline_list.size());
        for(const auto &tl : timeline_list) {
            msg_id_list.push_back(tl.message_id());
        }

        // 5. 批量查询消息正文 (使用之前实现的 select_by_ids)
        auto msg_list = _mysql_message_table->select_by_ids(msg_id_list);
        if(msg_list.empty()) {
            // Timeline 有 ID 但 Message 表没数据（极少见的数据不一致）
            LOG_WARN("增量同步时 Timeline 存在数据但 Message 表缺失, User: {}, StartID: {}", user_id, last_msg_id);
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
            auto *info = response->add_msg_list();
            
            // 填充基础信息
            info->set_message_id(msg.message_id());
            info->set_chat_session_id(msg.session_id());
            info->set_timestamp(boost::posix_time::to_time_t(msg.create_time()));
            
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
    virtual void GetUnreadCount(google::protobuf::RpcController* controller,
                                const ::chatnow::GetUnreadCountReq* request,
                                ::chatnow::GetUnreadCountRsp* response,
                                ::google::protobuf::Closure* done)
    {
        brpc::ClosureGuard rpc_guard(done);
        
        // 1. 提取参数
        std::string rid = request->request_id();
        std::string user_id = request->user_id();
        std::string chat_ssid = request->chat_session_id();
        unsigned long last_read_id = request->last_read_msg_id();

        // 2. 获取该会话最新的消息 ID
        unsigned long latest_msg_id = _mysql_usertimeline_table->get_latest_msg_id(user_id, chat_ssid);

        int unread_count = 0;

        // 3. 优化逻辑：如果库里的最新消息ID <= 用户已读ID，说明没有未读消息，无需 count
        if (latest_msg_id > 0 && latest_msg_id > last_read_id) {
            unread_count = _mysql_usertimeline_table->get_unread_count(user_id, chat_ssid, last_read_id);
        }

        // 4. 组织响应
        response->set_request_id(rid);
        response->set_success(true);
        response->set_unread_count(unread_count);
        response->set_latest_msg_id(latest_msg_id);

        LOG_DEBUG("请求ID {} - 未读数计算完成: uid={}, session={}, last_read={}, unread={}, latest={}", 
            rid, user_id, chat_ssid, last_read_id, unread_count, latest_msg_id);
    }
    //================================================================================================//
    //=========================================== 消费回调 ============================================//
    //================================================================================================//
    void onMessage(const char *body, size_t sz) {
        LOG_DEBUG("收到新消息，进行存储处理！");
        //1. 取出序列化的消息内容进行反序列化
        chatnow::MessageInfo  message;
        bool ret = message.ParseFromArray(body, sz);
        if(ret == false) {
            LOG_ERROR("对消费到的消息进行反序列化失败");
            return;
        }
        std::string file_id, file_name, content;
        int64_t file_size = 0;
        //2. 根据不同的消息类型进行不同的处理
        auto msg_type = message.message().message_type();
        if (msg_type == MessageType::STRING) {
            content = message.message().string_message().content();
        } else {
            // 如果是文件/图片/语音，先上传到文件子服务拿到 file_id
            bool file_ret = false;
            if (msg_type == MessageType::IMAGE) {
                const auto &m = message.message().image_message();
                file_ret = _PutFile("", m.image_content(), m.image_content().size(), file_id);
            } else if (msg_type == MessageType::FILE) {
                const auto &m = message.message().file_message();
                file_name = m.file_name();
                file_size = m.file_size();
                file_ret = _PutFile(file_name, m.file_contents(), file_size, file_id);
            } else if (msg_type == MessageType::SPEECH) {
                const auto &m = message.message().speech_message();
                file_ret = _PutFile("", m.file_contents(), m.file_contents().size(), file_id);
            }

            if (!file_ret) {
                LOG_ERROR("文件上传失败，放弃后续存储");
                return; 
            }
        }
        //3. 提取消息的元消息
        chatnow::Message msg(message.message_id(), 
                            message.chat_session_id(), 
                            message.sender().user_id(), 
                            message.message().message_type(),
                            boost::posix_time::from_time_t(message.timestamp()),
                            MessageStatus::NORMAL);
        msg.content(content);
        msg.file_id(file_id);
        msg.file_name(file_name);
        msg.file_size(file_size);
        //4. 取出会话所有成员
        std::vector<UserTimeline> timeline_list;
        std::vector<std::string> member_id_list;
        ret = _GetMembers(message.chat_session_id(), member_id_list);
        if(ret == false) {
            LOG_ERROR("调用会话管理子服务获取会话成员失败");
            return;
        }
        timeline_list.reserve(member_id_list.size());
        for(const auto &member : member_id_list) {
            UserTimeline timeline;
            timeline.message_id(message.message_id());
            timeline.message_time(boost::posix_time::from_time_t(message.timestamp()));
            timeline.session_id(message.chat_session_id());
            timeline.user_id(member);
            timeline_list.push_back(timeline);
        }
        //5. 开启事务，向消息表和Timeline表持久化数据
        try {
            odb::transaction trans(_db->begin());

            if(!_mysql_message_table->insert(msg)) {
                throw std::runtime_error("插入消息到Message表失败");
            }

            if(!_mysql_usertimeline_table->insert(timeline_list)) {
                throw std::runtime_error("将消息扩散到会话成员失败");
            }

            trans.commit();
            LOG_INFO("消息及扩散写成功: {}", message.message_id());
            if(msg_type == MessageType::STRING) {
                bool es_ret = _es_client->appendData(message.sender().user_id(), 
                                                    message.message_id(), 
                                                    message.timestamp(),
                                                    message.chat_session_id(),
                                                    content);
                if(!es_ret) {
                    // ES 失败不回滚 MySQL，只需记录日志，后续靠补偿机制或重新索引
                    LOG_ERROR("ES 索引失败，需后续补偿，MsgID: {}", message.message_id());
                }
            }
        } catch(std::exception &e) {
            LOG_ERROR("数据库事务失败: {}", e.what());
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
    bool _GetMembers(const std::string &ssid, std::vector<std::string> &member_id_list) {
        auto channel = _mm_channels->choose(_chatsession_service_name);
        if(!channel) {
            LOG_ERROR("{} 没有可供访问的会话管理子服务节点", _chatsession_service_name);
            return false;
        }
        ChatSessionService_Stub stub(channel.get());
        GetMemberIdListReq req;
        GetMemberIdListRsp rsp;
        req.set_chat_session_id(ssid);
        brpc::Controller cntl;
        stub.GetMemberIdList(&cntl, &req, &rsp, nullptr);
        if(cntl.Failed() == true || rsp.success() == false) {
            LOG_ERROR("会话子服务调用失败: {}", cntl.ErrorText());
            return false;
        }
        for(int i = 0; i < rsp.member_id_list_size(); ++i) {
            member_id_list.push_back(rsp.member_id_list(i));
        }
        return true;
    }
private:
    std::string _file_service_name;
    std::string _user_service_name;
    std::string _chatsession_service_name;
    std::shared_ptr<odb::core::database> _db;
    MessageTable::ptr _mysql_message_table;
    UserTimeLineTable::ptr _mysql_usertimeline_table;
    ESMessage::ptr _es_client;
    ServiceManager::ptr _mm_channels;
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
                const std::shared_ptr<brpc::Server> &server) 
        : _service_discover(service_discover),
        _reg_client(reg_client), 
        _mq_client(mq_client),
        _es_client(es_client),
        _mysql_client(mysql_client), 
        _rpc_server(server) {}
    ~MessageServer() = default;
    /* brief: 搭建RPC服务器，并启动服务器 */
    void start() {
        _rpc_server->RunUntilAskedToQuit();
    }
private:
    Discovery::ptr _service_discover;   // 服务发现客户端
    Registry::ptr _reg_client;          // 服务注册客户端
    MQClient::ptr _mq_client;
    std::shared_ptr<elasticlient::Client> _es_client;
    std::shared_ptr<odb::core::database> _mysql_client; // mysql 数据库客户端
    std::shared_ptr<brpc::Server> _rpc_server;
};

/* 建造者模式: 将对象真正的构造过程封装，便于后期扩展和调整 */
class MessageServerBuilder
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
    /* brief: 构造 RabbitMQ 客户端对象 */
    void make_mq_object(const std::string &user, 
                    const std::string &password, 
                    const std::string &host,
                    const std::string &exchange_name,
                    const std::string &queue_name,
                    const std::string &binding_key)
    {
        _exchange_name = exchange_name;
        _queue_name = queue_name;
        _mq_client = std::make_shared<MQClient>(user, password, host); 
        _mq_client->declareComponents(exchange_name, queue_name, binding_key);
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
        _rpc_server = std::make_shared<brpc::Server>();
        MessageServiceImpl *message_service = new MessageServiceImpl(_file_service_name, _user_service_name, _chatsession_service_name, _mm_channels, _es_client, _mysql_client);
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
        auto callback = std::bind(&MessageServiceImpl::onMessage, message_service, std::placeholders::_1, std::placeholders::_2);
        _mq_client->consume(_queue_name, callback);
    }
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

        MessageServer::ptr server = std::make_shared<MessageServer>(_service_discover, _reg_client, _mq_client, _es_client, _mysql_client, _rpc_server);

        return server;
    }
private:
    std::string _file_service_name;
    std::string _user_service_name;
    std::string _chatsession_service_name;
    ServiceManager::ptr _mm_channels;

    std::string _exchange_name;
    std::string _queue_name;
    MQClient::ptr _mq_client;
    

    Discovery::ptr _service_discover;   // 服务发现客户端
    Registry::ptr _reg_client;          // 服务注册客户端
    std::shared_ptr<elasticlient::Client> _es_client;
    std::shared_ptr<odb::core::database> _mysql_client; // mysql 数据库客户端
    std::shared_ptr<brpc::Server> _rpc_server;

};

}
