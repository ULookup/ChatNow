#pragma once

#include <string>
#include <cstddef>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

namespace chatnow
{

#pragma db object table("message_pin")
class MessagePin
{
public:
    MessagePin() = default;
    MessagePin(const std::string &ssid, unsigned long mid, const std::string &by)
        : _session_id(ssid), _message_id(mid), _pinned_by(by) {}

    std::string session_id() const { return _session_id; }
    void session_id(const std::string &v) { _session_id = v; }

    unsigned long message_id() const { return _message_id; }
    void message_id(unsigned long v) { _message_id = v; }

    std::string pinned_by() const { return _pinned_by; }
    void pinned_by(const std::string &v) { _pinned_by = v; }

    boost::posix_time::ptime pinned_at() const { return _pinned_at; }
    void pinned_at(const boost::posix_time::ptime &v) { _pinned_at = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("varchar(32)")
    std::string _session_id;

    #pragma db type("bigint unsigned")
    unsigned long _message_id;

    #pragma db type("varchar(32)")
    std::string _pinned_by;

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _pinned_at;

    #pragma db index("uk_conv_msg") unique members(_session_id, _message_id)
    #pragma db index("idx_conv") members(_session_id)
};

} // namespace chatnow
