#pragma once

#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>

namespace chatnow
{

enum class FriendApplyStatus {
    PENDING  = 0,  // 未处理
    ACCEPTED = 1,  // 已同意
    REJECTED = 2,  // 已拒绝
    CANCELED = 3   // 发起方撤回
};

#pragma db object table("friend_apply")
class FriendApply
{
public:
    FriendApply() = default;
    FriendApply(const std::string &eid, 
                const std::string &uid, 
                const std::string &pid, 
                const FriendApplyStatus status,
                const boost::posix_time::ptime &create_time)
        : _event_id(eid), 
          _user_id(uid), 
          _peer_id(pid),
          _status(status),
          _create_time(create_time) {}

    std::string event_id() const { return _event_id; }
    void event_id(const std::string &eid) { _event_id = eid; }

    std::string user_id() const { return _user_id; }
    void user_id(const std::string &uid) { _user_id = uid; }

    std::string peer_id() const { return _peer_id; }
    void peer_id(const std::string &pid) { _peer_id = pid; }

    FriendApplyStatus status() const { return _status; }
    void status(const FriendApplyStatus status) { _status = status; }

    boost::posix_time::ptime create_time() const { return _create_time; }
    void create_time(const boost::posix_time::ptime &create_time) { _create_time = create_time; }

    boost::posix_time::ptime handle_time() const {
        if(!_handle_time) {
            return boost::posix_time::ptime();
        }
        return *_handle_time;
    }
    void handle_time(const boost::posix_time::ptime &handle_time) { _handle_time = handle_time; }
private:
    friend class odb::access;
    #pragma db id auto
    unsigned long _id;
    #pragma db type("varchar(64)") index unique
    std::string _event_id;
    #pragma db type("varchar(64)") index
    std::string _user_id;
    #pragma db type("varchar(64)") index
    std::string _peer_id;
    //================= V2.0 ==================
    // 申请状态
    #pragma db type("tinyint")
    FriendApplyStatus _status {FriendApplyStatus::PENDING};

    // 创建时间
    #pragma db type("TIMESTAMP")
    boost::posix_time::ptime _create_time;

    // 处理时间
    #pragma db type("TIMESTAMP")
    odb::nullable<boost::posix_time::ptime> _handle_time;
};

}