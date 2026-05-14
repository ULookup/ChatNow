#pragma once

#include <string>
#include <cstddef>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace chatnow
{

#pragma db object table("message_reaction")
class MessageReaction
{
public:
    MessageReaction() = default;
    MessageReaction(unsigned long mid, const std::string &uid, const std::string &e)
        : _message_id(mid), _user_id(uid), _emoji(e) {}

    unsigned long message_id() const { return _message_id; }
    void message_id(unsigned long v) { _message_id = v; }

    std::string user_id() const { return _user_id; }
    void user_id(const std::string &v) { _user_id = v; }

    std::string emoji() const { return _emoji; }
    void emoji(const std::string &v) { _emoji = v; }

    boost::posix_time::ptime create_time() const { return _create_time; }
    void create_time(const boost::posix_time::ptime &v) { _create_time = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("bigint unsigned")
    unsigned long _message_id;

    #pragma db type("varchar(32)")
    std::string _user_id;

    #pragma db type("varchar(16)")
    std::string _emoji;

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _create_time;

    #pragma db index("uk_msg_user_emoji") unique members(_message_id, _user_id, _emoji)
    #pragma db index("idx_msg") members(_message_id)
};

} // namespace chatnow
