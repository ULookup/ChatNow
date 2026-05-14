#pragma once
#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

/**
 * ===========================================================================
 * 好友关系表 (relation) — 双向冗余存储
 * ---------------------------------------------------------------------------
 * 设计定位：
 *   - 一对好友写两行：(A→B) (B→A)，各自存"自己看对方"的备注、分组、星标
 *   - 主流 IM（QQ/微信）通用做法：以读放大极低换写双倍
 *   - 关系撤销不物理删除，置 status=DELETED 保留申请审计链路
 *
 * 字段速览：
 *   _id           物理主键
 *   _user_id      关系归属者（"我"）
 *   _peer_id      对端用户 ID（"我"的好友）
 *   _remark       我对对方的备注（不影响对方资料）
 *   _group_name   我把对方放在的分组名（自由文本）
 *   _status       关系状态：NORMAL/BLOCKED/DELETED（单方面拉黑/删除）
 *   _starred      星标好友（特别关心）
 *   _create_time  关系建立时间（同意申请的时间）
 *   _update_time  关系最近变更时间（备注/分组/状态修改），客户端增量同步
 *
 * 索引策略：
 *   uk_user_peer  (user_id, peer_id) 唯一 — 双重作用：
 *                 ① 保证一对关系一行
 *                 ② 充当"我的好友列表"最高效查询入口（按 user_id 前缀扫描）
 *   idx_peer_user (peer_id, user_id)   反向索引 — "谁加了我" 运营/排查用
 *   说明：双行存储后正常业务都按 user_id 前缀查，反向索引使用频次极低
 * ===========================================================================
 */

namespace chatnow
{

enum class RelationStatus : unsigned char {
    NORMAL  = 0,    // 正常好友
    BLOCKED = 1,    // 我已拉黑对方（对端 row 各自维护）
    DELETED = 2     // 我已单方删除（保留行做历史 / 便于恢复）
};

#pragma db object table("relation")
class Relation
{
public:
    Relation() = default;
    Relation(const std::string &uid, const std::string &pid)
        : _user_id(uid), _peer_id(pid) {}

    std::string user_id() const { return _user_id; }
    void user_id(const std::string &v) { _user_id = v; }

    std::string peer_id() const { return _peer_id; }
    void peer_id(const std::string &v) { _peer_id = v; }

    std::string remark() const { return _remark ? *_remark : std::string(); }
    void remark(const std::string &v) { _remark = v; }

    std::string group_name() const { return _group_name ? *_group_name : std::string(); }
    void group_name(const std::string &v) { _group_name = v; }

    RelationStatus status() const { return _status; }
    void status(RelationStatus v) { _status = v; }

    bool starred() const { return _starred; }
    void starred(bool v) { _starred = v; }

    boost::posix_time::ptime create_time() const { return _create_time; }
    void create_time(const boost::posix_time::ptime &v) { _create_time = v; }

    boost::posix_time::ptime update_time() const { return _update_time; }
    void update_time(const boost::posix_time::ptime &v) { _update_time = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("varchar(32)")
    std::string _user_id;

    #pragma db type("varchar(32)")
    std::string _peer_id;

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _remark;

    #pragma db type("varchar(32)")
    odb::nullable<std::string> _group_name;

    #pragma db type("tinyint unsigned")
    RelationStatus _status {RelationStatus::NORMAL};

    #pragma db type("tinyint(1)")
    bool _starred {false};

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _create_time;

    // 关系信息变更时间：好友备注/分组/状态变更同步给客户端用
    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _update_time;

    #pragma db index("uk_user_peer") unique members(_user_id, _peer_id)
    #pragma db index("idx_peer_user") members(_peer_id, _user_id)
};

} // namespace chatnow
