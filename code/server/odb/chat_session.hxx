#pragma once
#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace chatnow
{

enum class ChatSessionType {
    SINGLE = 1,
    GROUP = 2
};

#pragma db object table("chat_session")
class ChatSession
{
public:
    ChatSession() = default;
    ChatSession(const std::string &ssid, 
                const std::string &ssname, 
                const ChatSessionType sstype,
                const boost::posix_time::ptime &create_time,
                int member_count,
                int status)
        : _chat_session_id(ssid), 
        _chat_session_name(ssname), 
        _chat_session_type(sstype),
        _create_time(create_time),
        _member_count(member_count),
        _status(status) {}

    std::string chat_session_id() const { return _chat_session_id; }
    void chat_session_id(const std::string &ssid) { _chat_session_id = ssid; }

    std::string chat_session_name() const { return _chat_session_name; }
    void chat_session_name(const std::string &ssname) { _chat_session_name = ssname; }    

    ChatSessionType chat_session_type() const { return _chat_session_type; }
    void chat_session_type(const ChatSessionType sstype) { _chat_session_type = sstype; }  

    boost::posix_time::ptime create_time() const { return _create_time; }
    void create_time(const boost::posix_time::ptime &create_time) { _create_time = create_time; }

    unsigned long last_message_id() const { 
        if(!_last_message_id) {
            return 0;
        }
        return *_last_message_id; 
    }
    void last_message_id(const unsigned long last_message_id) { _last_message_id = last_message_id; }

    boost::posix_time::ptime last_message_time() const { 
        if(!_last_message_time) {
            return boost::posix_time::ptime();
        }
        return *_last_message_time; 
    }
    void last_message_time(const boost::posix_time::ptime &last_message_time) { _last_message_time = last_message_time; }

    int member_count() const { return _member_count; }
    void member_count(int member_count) { _member_count = member_count; }

    int status() const { return _status; }
    void status(int status) { _status = status; }

    std::string avatar_id() const { 
        if(!_avatar_id) {
            return std::string();
        }
        return *_avatar_id;
     }
     void avatar_id(const std::string &avatar_id) { _avatar_id = avatar_id; }
private:
    friend class odb::access;
    #pragma db id auto
    unsigned long _id;  //主键ID
    #pragma db type("varchar(64)") index unique
    std::string _chat_session_id;   //会话ID
    #pragma db type("varchar(64)")
    std::string _chat_session_name; //会话名称
    #pragma db type("tinyint")
    ChatSessionType _chat_session_type; // 1-SINGLE-单聊; 2-GROUP-群聊
    #pragma db type("TIMESTAMP")
    boost::posix_time::ptime _create_time; // 聊天会话创建时间
    #pragma db type("bigint")
    odb::nullable<unsigned long> _last_message_id;                   //最近一次消息id
    #pragma db type("TIMESTAMP")
    odb::nullable<boost::posix_time::ptime> _last_message_time;    //最近一次消息时间
    #pragma db type("int")
    int _member_count;  //会话人员数量
    #pragma db type("tinyint")
    int _status;    // 0 NORMAL(正常会话) / 1 ARCHIVED(只读会话) / 2 DISMISSED(解散了的会话)
    #pragma db type("varchar(64)") 
    odb::nullable<std::string> _avatar_id; //用户头像文件ID，不一定存在   
};

} // namespace chatnow