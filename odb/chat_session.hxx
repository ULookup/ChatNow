#pragma once
#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

/**
 * ===========================================================================
 * 会话主表 (chat_session)
 * ---------------------------------------------------------------------------
 * 设计要点：
 *   1. 移除 last_message_id / last_message_time 字段
 *      —— 每条消息都回写主表是行级写热点；改由 Redis chat:last:{session_id}
 *         缓存最近一条消息预览，未命中再 ORDER BY seq_id DESC LIMIT 1
 *   2. 新增 max_seq：会话内消息单调递增 seq 的快照（异步刷，允许短时间落后）
 *   3. 新增 owner_id：群主提到主表，避免反查 chat_session_member
 *   4. 新增 muted_all：全员禁言开关
 *   5. 新增 peer_user_id：单聊对端 user_id 直接落主表，单聊取对端 0 join
 *      —— 单聊 chat_session_id 建议由 (min_uid, max_uid) 哈希生成，
 *         客户端不查库即可推算
 *
 * 字段速览：
 *   _id                物理主键
 *   _chat_session_id   业务会话 ID（雪花/对端组合哈希），全局唯一
 *   _chat_session_name 会话名称：群聊为群名；单聊允许为空（取对方昵称实时渲染）
 *   _chat_session_type 1-单聊 2-群聊（预留 BROADCAST/ROOM/ASSISTANT）
 *   _create_time       创建时间
 *   _member_count      成员数缓存（增删成员时维护）
 *   _status            会话状态：NORMAL/ARCHIVED/DISMISSED/BANNED
 *   _avatar_id         群头像（单聊为空，由对方头像兜底）
 *   _owner_id          群主用户 ID（单聊为空）
 *   _peer_user_id      单聊对端 user_id（仅 SINGLE 类型有值）
 *   _description       群描述
 *   _announcement      群公告
 *   _muted_all         全员禁言开关
 *   _max_seq           会话内最新 seq 快照（最终一致，非强一致）
 *   _update_time       会话元信息变更时间（改名/换头像/公告）— 客户端列表增量同步用
 *
 * 索引策略：
 *   _chat_session_id   unique 索引；业务主入口
 *   说明：原 _chat_session_type、_status、_update_time 单列索引全部移除：
 *         - type/status 低基数（2~4 种值），单列索引选择性差
 *         - update_time 全局有序意义不大，按用户拉会话列表走 chat_session_member 索引
 * ===========================================================================
 */

namespace chatnow
{

enum class ChatSessionType : unsigned char {
    SINGLE = 1,
    GROUP  = 2
    // 后续可扩：BROADCAST(系统通知) / ROOM(直播间) / ASSISTANT(机器人)
};

enum class ChatSessionStatus : unsigned char {
    NORMAL    = 0,
    ARCHIVED  = 1,
    DISMISSED = 2,
    BANNED    = 3
};

#pragma db object table("chat_session")
class ChatSession
{
public:
    ChatSession() = default;
    ChatSession(const std::string &ssid,
                const std::string &ssname,
                ChatSessionType sstype,
                const boost::posix_time::ptime &create_time,
                int member_count,
                ChatSessionStatus status)
        : _chat_session_id(ssid), _chat_session_name(ssname),
          _chat_session_type(sstype), _create_time(create_time),
          _member_count(member_count), _status(status) {}

    std::string chat_session_id() const { return _chat_session_id; }
    void chat_session_id(const std::string &v) { _chat_session_id = v; }

    std::string chat_session_name() const { return _chat_session_name ? *_chat_session_name : std::string(); }
    void chat_session_name(const std::string &v) { _chat_session_name = v; }

    ChatSessionType chat_session_type() const { return _chat_session_type; }
    void chat_session_type(ChatSessionType v) { _chat_session_type = v; }

    boost::posix_time::ptime create_time() const { return _create_time; }
    void create_time(const boost::posix_time::ptime &v) { _create_time = v; }

    int member_count() const { return _member_count; }
    void member_count(int v) { _member_count = v; }

    ChatSessionStatus status() const { return _status; }
    void status(ChatSessionStatus v) { _status = v; }

    std::string avatar_id() const { return _avatar_id ? *_avatar_id : std::string(); }
    void avatar_id(const std::string &v) { _avatar_id = v; }

    std::string owner_id() const { return _owner_id ? *_owner_id : std::string(); }
    void owner_id(const std::string &v) { _owner_id = v; }

    std::string peer_user_id() const { return _peer_user_id ? *_peer_user_id : std::string(); }
    void peer_user_id(const std::string &v) { _peer_user_id = v; }

    std::string description() const { return _description ? *_description : std::string(); }
    void description(const std::string &v) { _description = v; }

    std::string announcement() const { return _announcement ? *_announcement : std::string(); }
    void announcement(const std::string &v) { _announcement = v; }

    bool muted_all() const { return _muted_all; }
    void muted_all(bool v) { _muted_all = v; }

    unsigned long max_seq() const { return _max_seq; }
    void max_seq(unsigned long v) { _max_seq = v; }

    boost::posix_time::ptime update_time() const { return _update_time; }
    void update_time(const boost::posix_time::ptime &v) { _update_time = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("varchar(32)") index unique
    std::string _chat_session_id;

    // 单聊场景下名称是冗余的（取对方昵称实时渲染），允许为空避免占位脏数据
    #pragma db type("varchar(64)")
    odb::nullable<std::string> _chat_session_name;

    // 类型不单列索引：仅 2 种枚举值，优化器不会走单列索引
    #pragma db type("tinyint unsigned")
    ChatSessionType _chat_session_type {ChatSessionType::SINGLE};

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _create_time;

    #pragma db type("int unsigned")
    int _member_count {0};

    // 状态不单列索引：低基数枚举单列索引选择性差
    #pragma db type("tinyint unsigned")
    ChatSessionStatus _status {ChatSessionStatus::NORMAL};

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _avatar_id;

    #pragma db type("varchar(32)")
    odb::nullable<std::string> _owner_id;

    // 单聊对端 user_id：取代旧 self-join 视图；仅 SINGLE 类型有值
    #pragma db type("varchar(32)")
    odb::nullable<std::string> _peer_user_id;

    #pragma db type("varchar(255)")
    odb::nullable<std::string> _description;

    #pragma db type("varchar(1024)")
    odb::nullable<std::string> _announcement;

    #pragma db type("tinyint(1)")
    bool _muted_all {false};

    #pragma db type("bigint unsigned")
    unsigned long _max_seq {0};

    // 会话元信息变更时间：客户端会话列表增量同步用，不单列索引
    // （会话列表查询从 chat_session_member 入手，按用户取自己的列表）
    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _update_time;
};

} // namespace chatnow
