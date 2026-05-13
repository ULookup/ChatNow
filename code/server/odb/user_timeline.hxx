#pragma once

#include <cstddef>
#include <string>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

/**
 * ===========================================================================
 * 用户消息收件箱 (user_timeline) — 写扩散 Timeline
 * ---------------------------------------------------------------------------
 * 设计要点：
 *   1. user_seq：用户级单调递增序号
 *      - 由 Redis INCR user_seq:{user_id} 生成
 *      - 客户端只需保存一个游标即可拉「跨所有会话」的全部新消息
 *      - 这是主流推拉结合方案的标杆字段，旧设计缺失
 *
 *   2. session_seq：消息在会话内的 seq（与 message.seq_id 一致）
 *      - 已读未读计算改用 (session_id, session_seq)
 *      - message_id 同时保留，跨会话引用与回查 message 表用
 *
 *   3. deliver_status：消息投递状态机
 *      - PENDING（已落 timeline，未送达）
 *      - DELIVERED（至少一端送达）
 *      - READ（已读）— 单聊/被 @ 等场景置 READ
 *      - 旧版本"is_read 布尔字段"被合并到此枚举（已读即 status=READ）
 *
 *   4. 分库分表：sharding_key = user_id
 *      - 用户的所有 timeline 在同一分片，按 user_seq 顺序扫高效
 *      - 海量群消息扩散写入压力分散到不同用户分片
 *
 * 字段速览：
 *   _id              物理主键
 *   _user_id         收件人用户 ID（写扩散：会话每成员各一行）
 *   _user_seq        用户级单调递增 seq；增量同步唯一游标
 *   _session_id      消息所属会话 ID
 *   _session_seq     消息在会话内的 seq（与 message.seq_id 一致）
 *   _message_id      消息全局 ID（雪花），跨会话引用与回查 message 表用
 *   _message_time    消息产生时间（与 message.create_time 同源，冗余以避免 join）
 *   _deliver_status  投递状态：PENDING/DELIVERED/READ
 *
 * 索引策略：
 *   idx_user_seq          (user_id, user_seq) 增量同步主索引
 *   idx_user_session_seq  (user_id, session_id, session_seq) 会话内范围查询
 *   说明：原 idx_user_msg 删除 — 与 idx_user_seq 功能重叠，
 *         按 message_id 删除/标已读的批处理走 message_id 全局索引即可
 * ===========================================================================
 */

namespace chatnow
{

enum class TimelineDeliverStatus : unsigned char {
    PENDING   = 0,  // 已落 Timeline，未确认送达
    DELIVERED = 1,  // 已送达至少一端
    READ      = 2   // 已读
};

#pragma db object table("user_timeline")
class UserTimeline
{
public:
    UserTimeline() = default;
    UserTimeline(const std::string &uid,
                 const std::string &sid,
                 unsigned long msgid,
                 const boost::posix_time::ptime &msgtime)
        : _user_id(uid), _session_id(sid),
          _message_id(msgid), _message_time(msgtime) {}

    std::string user_id() const { return _user_id; }
    void user_id(const std::string &v) { _user_id = v; }

    std::string session_id() const { return _session_id; }
    void session_id(const std::string &v) { _session_id = v; }

    unsigned long message_id() const { return _message_id; }
    void message_id(unsigned long v) { _message_id = v; }

    unsigned long user_seq() const { return _user_seq; }
    void user_seq(unsigned long v) { _user_seq = v; }

    unsigned long session_seq() const { return _session_seq; }
    void session_seq(unsigned long v) { _session_seq = v; }

    boost::posix_time::ptime message_time() const { return _message_time; }
    void message_time(const boost::posix_time::ptime &v) { _message_time = v; }

    TimelineDeliverStatus deliver_status() const { return _deliver_status; }
    void deliver_status(TimelineDeliverStatus v) { _deliver_status = v; }

    /* 语义辅助：已读判断 */
    bool is_read() const { return _deliver_status == TimelineDeliverStatus::READ; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("varchar(32)")
    std::string _user_id;

    #pragma db type("bigint unsigned")
    unsigned long _user_seq {0};

    #pragma db type("varchar(32)")
    std::string _session_id;

    #pragma db type("bigint unsigned")
    unsigned long _session_seq {0};

    #pragma db type("bigint unsigned")
    unsigned long _message_id;

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _message_time;

    // 投递状态枚举（PENDING/DELIVERED/READ）；旧 _is_read 已合并到此处
    #pragma db type("tinyint unsigned")
    TimelineDeliverStatus _deliver_status {TimelineDeliverStatus::PENDING};

    // 增量同步主索引：按 user_seq 顺序拉「我」的全部新消息
    #pragma db index("idx_user_seq") members(_user_id, _user_seq)
    // 会话维度范围查询索引：(user_id, session_id) 取该会话的 timeline
    #pragma db index("idx_user_session_seq") members(_user_id, _session_id, _session_seq)
};

#pragma db view object(UserTimeline)
struct LatestIdView {
    #pragma db column("max(" + UserTimeline::_user_seq + ")")
    odb::nullable<unsigned long> max_id;
};

#pragma db view object(UserTimeline)
struct CountView {
    #pragma db column("count(" + UserTimeline::_id + ")")
    unsigned long count;
};

} // namespace chatnow
