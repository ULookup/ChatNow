#pragma once

#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

/**
 * ===========================================================================
 * 群消息已读回执表 (message_read)
 * ---------------------------------------------------------------------------
 * 设计要点：
 *   1. 仅为「重要群消息」记录已读名单（产品开关 / 默认关闭），
 *      千人群每条消息都记录会爆表，必须按需开启。
 *   2. 单聊已读靠 chat_session_member.last_read_seq，无需此表。
 *   3. (message_id, user_id) 是逻辑唯一键，同一用户对同一消息一行。
 *   4. 高 QPS 写入路径：
 *      - Redis SET 暂存当日已读集合（key: read:{message_id}）
 *      - 后台批量异步刷库
 *      - 大群消息发出 24h 后停止收集 ack（产品策略）
 *   5. 读取路径：发送者点开"已读列表"按 message_id 分页查
 *
 * 字段速览：
 *   _id          物理主键
 *   _message_id  被已读的消息 ID（全局 message_id）
 *   _user_id     已读人用户 ID
 *   _read_time   已读时间
 *
 * 索引策略：
 *   uk_msg_user  (message_id, user_id) UNIQUE — 同一用户对同一消息只一行
 *   idx_msg_time (message_id, read_time) — "已读列表" 按时间倒序分页
 *   说明：原冗余字段 _session_id 已删除 — 没有索引使用者，
 *         需要"按会话归档"时直接 join message 表的 session_id 列即可
 * ===========================================================================
 */

namespace chatnow
{

#pragma db object table("message_read")
class MessageRead
{
public:
    MessageRead() = default;
    MessageRead(unsigned long mid,
                const std::string &uid,
                const boost::posix_time::ptime &rtime)
        : _message_id(mid), _user_id(uid), _read_time(rtime) {}

    unsigned long message_id() const { return _message_id; }
    void message_id(unsigned long v) { _message_id = v; }

    std::string user_id() const { return _user_id; }
    void user_id(const std::string &v) { _user_id = v; }

    boost::posix_time::ptime read_time() const { return _read_time; }
    void read_time(const boost::posix_time::ptime &v) { _read_time = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("bigint unsigned")
    unsigned long _message_id;

    #pragma db type("varchar(32)")
    std::string _user_id;

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _read_time;

    #pragma db index("uk_msg_user") unique members(_message_id, _user_id)
    #pragma db index("idx_msg_time") members(_message_id, _read_time)
};

} // namespace chatnow
