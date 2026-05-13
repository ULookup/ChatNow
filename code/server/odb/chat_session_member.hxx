/* brief: 聊天会话成员映射对象 */
#pragma once
#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

/**
 * ===========================================================================
 * 会话成员表 (chat_session_member)
 * ---------------------------------------------------------------------------
 * 设计要点：
 *   1. last_read_seq / last_ack_seq 取代 last_read_msg：游标改为会话内 seq 维度，
 *      不再依赖全局 message_id —— 主流 IM 已读未读计算的标准做法
 *   2. 退群保留行（is_quit + quit_time），不物理删除
 *      —— 便于 a) 邀请回群识别旧成员 b) 历史已读统计回溯 c) 风控数据完整
 *      —— 同一对 (session_id, user_id) 仍唯一：再次入群是 UPDATE 现有行
 *         （重置 join_time/role/inviter_id/is_quit=false），不会产生多行
 *
 * 字段速览：
 *   _id              物理主键
 *   _session_id      所属会话 ID
 *   _user_id         成员用户 ID
 *   _last_read_seq   已读到的最大会话 seq（unread = max_seq - last_read_seq）
 *   _last_ack_seq    确认送达的最大 seq（送达 ≠ 已读）
 *   _muted           会话免打扰开关（不触发推送/角标）
 *   _visible         是否在会话列表中可见（隐藏 = false）
 *   _pin_time        置顶时间（null 表示未置顶；按时间倒序便于多置顶并存排序）
 *   _role            群角色：NORMAL/ADMIN/OWNER
 *   _alias           群昵称（不影响 user 表昵称）
 *   _inviter_id      邀请人 ID
 *   _join_source     入群方式：邀请/搜索/扫码/链接/管理员加入/创建者
 *   _join_time       加入时间
 *   _mute_until      禁言到期时间（null 表示未禁言；过期自动失效，无需后台清理）
 *   _is_quit         是否已退群（软删除）
 *   _quit_time       退群时间
 *
 * 索引策略：
 *   uk_session_user  (session_id, user_id) 唯一 — 保证一对一行；二次入群 UPDATE
 *   idx_user_session (user_id, is_quit, session_id)
 *                    "我的活跃会话" 主路径，is_quit 在中间方便 = false 过滤
 *   说明：原 _is_quit 单列索引被合并到上面那个复合索引，删除冗余
 * ===========================================================================
 */

namespace chatnow
{

enum class ChatSessionRole : unsigned char {
    NORMAL = 0, ADMIN = 1, OWNER = 2
};

enum class JoinSource : unsigned char {
    UNKNOWN   = 0,
    INVITE    = 1,
    SEARCH    = 2,
    QR_CODE   = 3,
    LINK      = 4,
    ADMIN_ADD = 5,
    CREATE    = 6
};

#pragma db object table("chat_session_member")
class ChatSessionMember
{
public:
    ChatSessionMember() = default;
    ChatSessionMember(const std::string &ssid,
                      const std::string &uid,
                      bool muted,
                      bool visible,
                      ChatSessionRole role,
                      const boost::posix_time::ptime &join_time)
        : _session_id(ssid), _user_id(uid),
          _muted(muted), _visible(visible),
          _role(role), _join_time(join_time) {}

    std::string session_id() const { return _session_id; }
    void session_id(const std::string &v) { _session_id = v; }

    std::string user_id() const { return _user_id; }
    void user_id(const std::string &v) { _user_id = v; }

    unsigned long last_read_seq() const { return _last_read_seq; }
    void last_read_seq(unsigned long v) { _last_read_seq = v; }

    unsigned long last_ack_seq() const { return _last_ack_seq; }
    void last_ack_seq(unsigned long v) { _last_ack_seq = v; }

    bool muted() const { return _muted; }
    void muted(bool v) { _muted = v; }

    bool visible() const { return _visible; }
    void visible(bool v) { _visible = v; }

    boost::posix_time::ptime pin_time() const {
        return _pin_time ? *_pin_time : boost::posix_time::ptime();
    }
    void pin_time(const boost::posix_time::ptime &v) { _pin_time = v; }
    void unpin() { _pin_time = odb::nullable<boost::posix_time::ptime>(); }

    ChatSessionRole role() const { return _role; }
    void role(ChatSessionRole v) { _role = v; }

    boost::posix_time::ptime join_time() const { return _join_time; }
    void join_time(const boost::posix_time::ptime &v) { _join_time = v; }

    boost::posix_time::ptime mute_until() const {
        return _mute_until ? *_mute_until : boost::posix_time::ptime();
    }
    void mute_until(const boost::posix_time::ptime &v) { _mute_until = v; }

    std::string alias() const { return _alias ? *_alias : std::string(); }
    void alias(const std::string &v) { _alias = v; }

    std::string inviter_id() const { return _inviter_id ? *_inviter_id : std::string(); }
    void inviter_id(const std::string &v) { _inviter_id = v; }

    JoinSource join_source() const { return _join_source; }
    void join_source(JoinSource v) { _join_source = v; }

    bool is_quit() const { return _is_quit; }
    void is_quit(bool v) { _is_quit = v; }

    boost::posix_time::ptime quit_time() const {
        return _quit_time ? *_quit_time : boost::posix_time::ptime();
    }
    void quit_time(const boost::posix_time::ptime &v) { _quit_time = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("varchar(32)")
    std::string _session_id;

    #pragma db type("varchar(32)")
    std::string _user_id;

    // —— 已读 / 已收 游标（按会话 seq 维度） ——
    #pragma db type("bigint unsigned")
    unsigned long _last_read_seq {0};

    #pragma db type("bigint unsigned")
    unsigned long _last_ack_seq {0};

    // —— 个性化设置 ——
    #pragma db type("tinyint(1)")
    bool _muted {false};

    #pragma db type("tinyint(1)")
    bool _visible {true};

    #pragma db type("DATETIME(3)")
    odb::nullable<boost::posix_time::ptime> _pin_time;

    #pragma db type("tinyint unsigned")
    ChatSessionRole _role {ChatSessionRole::NORMAL};

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _alias;

    #pragma db type("varchar(32)")
    odb::nullable<std::string> _inviter_id;

    #pragma db type("tinyint unsigned")
    JoinSource _join_source {JoinSource::UNKNOWN};

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _join_time;

    #pragma db type("DATETIME(3)")
    odb::nullable<boost::posix_time::ptime> _mute_until;

    // 软删除标记，二次入群 UPDATE 现有行（uk_session_user 保证仅一行）
    #pragma db type("tinyint(1)")
    bool _is_quit {false};

    #pragma db type("DATETIME(3)")
    odb::nullable<boost::posix_time::ptime> _quit_time;

    // (session_id, user_id) 唯一：含已退群成员；二次入群语义为 UPDATE
    #pragma db index("uk_session_user") unique members(_session_id, _user_id)
    // "我的活跃会话" 主索引：is_quit 在中间，过滤 = false 即活跃成员
    #pragma db index("idx_user_session") members(_user_id, _is_quit, _session_id)
};

} // namespace chatnow
