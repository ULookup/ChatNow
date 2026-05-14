#pragma once

#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

/**
 * ===========================================================================
 * 好友申请事件表 (friend_apply)
 * ---------------------------------------------------------------------------
 * 设计定位：
 *   - 一次"加好友"流程产生一行；同意后状态置 ACCEPTED 不级联删除，便于审计
 *   - 同一对 (user_id, peer_id) 历史可能产生多条申请，靠 event_id 唯一标识
 *
 * 字段速览：
 *   _id           物理主键
 *   _event_id     业务事件 ID（雪花/UUID），通知/处理回调通过它关联
 *   _user_id      申请人 ID
 *   _peer_id      被申请人 ID
 *   _status       申请状态：PENDING/ACCEPTED/REJECTED/CANCELED/EXPIRED
 *   _source       申请来源：搜索/扫码/群成员/名片/手机号 — 风控分析用
 *   _greeting     招呼语（"我是 XXX"）
 *   _remark       申请人为对方预设的备注（同意后写入 relation.remark）
 *   _create_time  申请创建时间
 *   _handle_time  申请处理时间（同意/拒绝/撤回时填充）
 *
 * 索引策略：
 *   idx_peer_status_time  (peer_id, status, create_time)
 *                         覆盖「我的待处理申请列表」高频查询路径
 *   idx_user_peer_time    (user_id, peer_id, create_time DESC)
 *                         应用层判重 + 防骚扰用：是否最近已申请过
 *   说明：原 idx_user_time 被合并到上面那个复合索引，前缀已覆盖
 * ===========================================================================
 */

namespace chatnow
{

enum class FriendApplyStatus : unsigned char {
    PENDING  = 0,
    ACCEPTED = 1,
    REJECTED = 2,
    CANCELED = 3,
    EXPIRED  = 4
};

enum class FriendApplySource : unsigned char {
    UNKNOWN  = 0,
    SEARCH   = 1,   // ID/昵称搜索
    PHONE    = 2,   // 通过手机号
    QR_CODE  = 3,   // 扫一扫
    GROUP    = 4,   // 群成员
    BUSINESS = 5    // 名片分享
};

#pragma db object table("friend_apply")
class FriendApply
{
public:
    FriendApply() = default;
    FriendApply(const std::string &eid,
                const std::string &uid,
                const std::string &pid,
                FriendApplyStatus status,
                const boost::posix_time::ptime &create_time)
        : _event_id(eid), _user_id(uid), _peer_id(pid),
          _status(status), _create_time(create_time) {}

    std::string event_id() const { return _event_id; }
    void event_id(const std::string &v) { _event_id = v; }

    std::string user_id() const { return _user_id; }
    void user_id(const std::string &v) { _user_id = v; }

    std::string peer_id() const { return _peer_id; }
    void peer_id(const std::string &v) { _peer_id = v; }

    FriendApplyStatus status() const { return _status; }
    void status(FriendApplyStatus v) { _status = v; }

    FriendApplySource source() const { return _source; }
    void source(FriendApplySource v) { _source = v; }

    std::string greeting() const { return _greeting ? *_greeting : std::string(); }
    void greeting(const std::string &v) { _greeting = v; }

    std::string remark() const { return _remark ? *_remark : std::string(); }
    void remark(const std::string &v) { _remark = v; }

    boost::posix_time::ptime create_time() const { return _create_time; }
    void create_time(const boost::posix_time::ptime &v) { _create_time = v; }

    boost::posix_time::ptime handle_time() const {
        return _handle_time ? *_handle_time : boost::posix_time::ptime();
    }
    void handle_time(const boost::posix_time::ptime &v) { _handle_time = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("varchar(32)") index unique
    std::string _event_id;

    #pragma db type("varchar(32)")
    std::string _user_id;

    #pragma db type("varchar(32)")
    std::string _peer_id;

    #pragma db type("tinyint unsigned")
    FriendApplyStatus _status {FriendApplyStatus::PENDING};

    #pragma db type("tinyint unsigned")
    FriendApplySource _source {FriendApplySource::UNKNOWN};

    #pragma db type("varchar(255)")
    odb::nullable<std::string> _greeting;

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _remark;

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _create_time;

    #pragma db type("DATETIME(3)")
    odb::nullable<boost::posix_time::ptime> _handle_time;

    // 主路径：被申请人翻"待处理申请"列表（按时间倒序）
    #pragma db index("idx_peer_status_time") members(_peer_id, _status, _create_time)
    // 防骚扰判重：申请人查与某用户的历史申请；前缀已含 user_id，原 idx_user_time 合并掉
    #pragma db index("idx_user_peer_time") members(_user_id, _peer_id, _create_time)
};

} // namespace chatnow
