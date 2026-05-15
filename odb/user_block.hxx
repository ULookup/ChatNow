#pragma once
#include <string>
#include <cstddef>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

/**
 * ===========================================================================
 * 用户拉黑表 (user_block)
 * ---------------------------------------------------------------------------
 * 设计定位：
 *   - 单向：A 拉黑 B 时只写 (A,B) 一行；与 relation 双向冗余明确分离
 *   - 仅用于"拒新好友申请 + 搜索过滤"两个场景；不动 relation / 会话 / 消息
 *   - 唯一索引保证一对 (blocker, blocked) 至多一行；重复 BlockUser 视作幂等
 *
 * 字段速览：
 *   _id           物理主键
 *   _blocker_id   "我"（执行拉黑动作的用户）
 *   _blocked_id   被我拉黑的对端
 *   _create_time  拉黑时间
 *
 * 索引策略：
 *   uk_blocker_blocked  (blocker_id, blocked_id) 唯一
 *   idx_blocked         (blocked_id)              反查"谁拉黑了我"
 * ===========================================================================
 */

namespace chatnow
{

#pragma db object table("user_block")
class UserBlock
{
public:
    UserBlock() = default;
    UserBlock(const std::string &blocker, const std::string &blocked)
        : _blocker_id(blocker), _blocked_id(blocked) {}

    std::string blocker_id() const { return _blocker_id; }
    void blocker_id(const std::string &v) { _blocker_id = v; }

    std::string blocked_id() const { return _blocked_id; }
    void blocked_id(const std::string &v) { _blocked_id = v; }

    boost::posix_time::ptime create_time() const { return _create_time; }
    void create_time(const boost::posix_time::ptime &v) { _create_time = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("varchar(32)")
    std::string _blocker_id;

    #pragma db type("varchar(32)")
    std::string _blocked_id;

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _create_time;

    #pragma db index("uk_blocker_blocked") unique members(_blocker_id, _blocked_id)
    #pragma db index("idx_blocked") members(_blocked_id)
};

} // namespace chatnow
