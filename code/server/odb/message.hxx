#pragma once

#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace chatnow
{

enum class MessageStatus : unsigned char {
    NORMAL  = 0,
    REVOKED = 1
};

#pragma db object table("message")
class Message
{
public:
    Message() = default;
    Message(unsigned long mid,
            const std::string &ssid,
            const std::string &uid,
            const unsigned char mtype,
            const boost::posix_time::ptime &create_time,
            const MessageStatus status)
            : _message_id(mid), 
            _session_id(ssid), 
            _user_id(uid),
            _message_type(mtype), 
            _create_time(create_time),
            _status(status) {}
    
    unsigned long message_id() const { return _message_id; }
    void message_id(unsigned long message_id) { _message_id = message_id; }

    std::string session_id() const { return _session_id; }
    void session_id(const std::string &session_id) { _session_id = session_id; }

    std::string user_id() const { return _user_id; }
    void user_id(const std::string &user_id) { _user_id = user_id; }

    unsigned char message_type() const { return _message_type; }
    void message_type(unsigned char message_type) { _message_type = message_type; }

    boost::posix_time::ptime create_time() const { return _create_time; }
    void create_time(const boost::posix_time::ptime &create_time) { _create_time = create_time; }

    std::string content() const { 
        if(!_content) {
            return std::string(); 
        }
        return *_content; 
    }
    void content(const std::string &content) {  _content = content; }

    std::string file_id() const { 
        if(!_file_id) {
            return std::string();
        } 
        return *_file_id; 
    }
    void file_id(const std::string &file_id) { _file_id = file_id; }

    std::string file_name() const { 
        if(!_file_name) {
            return std::string(); 
        }
        return *_file_name; 
    }
    void file_name(const std::string &file_name) { _file_name = file_name; }

    unsigned char file_size() const { 
        if(!_file_size) {
            return 0; 
        }
        return *_file_size; 
    }
    void file_size(unsigned char file_size) { _file_size = file_size; }

    // ================== 消息状态 ======================

    MessageStatus status() const { return _status; }
    void status(const MessageStatus status) { _status = status; }

    boost::posix_time::ptime revoke_time() const {  
        if(!_revoke_time) {
            return boost::posix_time::ptime();
        }
        return *_revoke_time;
    }
    void revoke_time(const boost::posix_time::ptime &revoke_time) { _revoke_time = revoke_time; }
private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;
    #pragma db type("bigint") index unique
    unsigned long _message_id;
    #pragma db type("varchar(64)") index
    std::string _session_id;        // 所属会话ID
    #pragma db type("varchar(64)") 
    std::string _user_id;           // 发送者用户ID
    unsigned char _message_type;    // 消息类型: 1-文本、2-图像、3-文件、4-语音
    #pragma db type("TIMESTAMP")
    boost::posix_time::ptime _create_time;         // 消息产生时间

    odb::nullable<std::string> _content;           // 文本消息内容--非文本消息可忽略
    #pragma db type("varchar(64)") 
    odb::nullable<std::string> _file_id;           // 文件消息的文件ID--文本消息可忽略
    #pragma db type("varchar(128)") 
    odb::nullable<std::string> _file_name;         // 文件消息的文件名称--文本消息可忽略
    odb::nullable<unsigned int> _file_size;        // 文件消息的文件大学--文本消息可忽略 

    //=======消息状态=======
    #pragma db type("tinyint")
    MessageStatus _status {MessageStatus::NORMAL};
    #pragma db type("TIMESTAMP")
    odb::nullable<boost::posix_time::ptime> _revoke_time; 
};

}