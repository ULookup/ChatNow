/* brief: 聊天会话成员映射对象 */
#pragma once
#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>

namespace chatnow
{

enum class ChatSessionRole {
    NORMAL = 0, //普通群友
    ADMIN  = 1, //狗管理
    OWNER  = 2  //群主老婆
};
    
#pragma db object table("chat_session_member")
#pragma db index("idx_session_user") unique members(_session_id, _user_id)
#pragma db index("idx_user_session") members(_user_id, _session_id)
class ChatSessionMember
{
public:
    ChatSessionMember() = default;
    ChatSessionMember(const std::string &ssid, 
                    const std::string &uid,
                    const std::string &last_read_msg,
                    unsigned int unread_count,
                    bool muted,
                    bool visible,
                    const ChatSessionRole role,
                    const boost::posix_time::ptime &join_time) 
        : _session_id(ssid), 
        _user_id(uid), 
        _last_read_msg(last_read_msg),
        _unread_count(unread_count),
        _muted(muted),
        _visible(visible),
        _role(role),
        _join_time(join_time) {}

    std::string session_id() { return _session_id; }
    void session_id(const std::string &ssid) { _session_id = ssid; }

    std::string user_id() { return _user_id; }
    void user_id(const std::string &uid) { _user_id = uid; }

    std::string last_read_msg() const { return _last_read_msg; }
    void last_read_msg(const std::string &last_read_msg) { _last_read_msg = last_read_msg; }

    unsigned int unread_count() const { return _unread_count; }
    void unread_count(int unread_count) { _unread_count = unread_count; }

    bool muted() const { return _muted; }
    void muted(bool muted) { _muted = muted;}

    bool visible() const { return _visible; }
    void visible(bool visible) { _visible = visible;}

    boost::posix_time::ptime pin_time() const {
        if(!_pin_time) {
            return boost::posix_time::ptime();
        }
        return *_pin_time;
    }
    void pin_time(const boost::posix_time::ptime &pin_time) { _pin_time = pin_time; }

    ChatSessionRole role() const { return _role; }
    void role(const ChatSessionRole role) { _role = role; }

    boost::posix_time::ptime join_time() const { return _join_time; }
    void join_time(const boost::posix_time::ptime &join_time) { _join_time = join_time; }
private:
    friend class odb::access;
    #pragma db id auto
    unsigned long _id;
    #pragma db type("varchar(64)") index
    std::string _session_id;
    #pragma db type("varchar(64)")
    std::string _user_id;

    //=================================
    //===========状态字段===============
    //=================================
    #pragma db type("varchat(64)")
    std::string _last_read_msg {0};      // 最后一次读到的消息id，用作这个会话的游标

    #pragma db type("int")
    unsigned int _unread_count {0};      // 未读消息数

    #pragma db type("tinyint")
    bool _muted {false};                 // 免打扰开关

    #pragma db type("tinyint")
    bool _visible {true};                // 是否展示会话

    #pragma db type("TIMESTAMP")
    odb::nullable<boost::posix_time::ptime> _pin_time;  // 置顶时间（null表示不置顶），用时间主要是为了置顶排序

    #pragma db type("tinyint")
    ChatSessionRole _role {ChatSessionRole::NORMAL}; // 该用户在该群的身份

    #pragma db type("TIMESTAMP")
    boost::posix_time::ptime _join_time; // 该成员加入该会话时间
};

} // namespace chatnow