#pragma once

#include <cstddef>
#include <string>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace chatnow
{

#pragma db object table("user_timeline")
class UserTimeline
{
public:
    UserTimeline() = default;
    UserTimeline(const std::string &uid,
                const std::string &sid,
                const unsigned long msgid,
                const boost::posix_time::ptime &msgtime)
        : _user_id(uid), _session_id(sid), _message_id(msgid), _message_time(msgtime) {}

    std::string user_id() const { return _user_id; }
    void user_id(const std::string &user_id) { _user_id = user_id; }

    std::string session_id() const { return _session_id; }
    void session_id(const std::string &session_id) { _session_id = session_id; }

    unsigned long message_id() const { return _message_id; }
    void message_id(const unsigned long message_id) { _message_id = message_id; }

    boost::posix_time::ptime message_time() const { return _message_time; }
    void message_time(const boost::posix_time::ptime &message_time) { _message_time = message_time; }
private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    // ------ 关键 ------
    // 谁？哪个会话？发了什么？
    #pragma db type("varchar(64)")
    std::string _user_id;
    #pragma db type("varchar(64)")
    std::string _session_id;
    #pragma db type("bigint")
    unsigned long _message_id;

    #pragma db type("TIMESTAMP")
    boost::posix_time::ptime _message_time;

    #pragma db index("idx_user_msg") members(_user_id, _message_id)
    #pragma db index("idx_user_session_msg") members(_user_id, _session_id, _message_id)
}; // classs UserTimeline

#pragma db view object(UserTimeline)
struct LatestIdView {
    #pragma db column("max(" + UserTimeline::_message_id + ")")
    odb::nullable<unsigned long> max_id;
};

// 2. 用于统计数量 (COUNT)
#pragma db view object(UserTimeline)
struct CountView {
    #pragma db column("count(" + UserTimeline::_id + ")")
    unsigned long count;
};

} // namespace chatnow