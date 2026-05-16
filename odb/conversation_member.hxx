#pragma once
#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

/**
 * 会话成员表 (conversation_member) — 替代旧 chat_session_member.hxx
 * 字段集合不变，仅类名/枚举名/表名 rename：
 *   ChatSessionMember  → ConversationMember
 *   ChatSessionRole    → MemberRole          (NORMAL/ADMIN/OWNER)
 *   table chat_session_member → table conversation_member
 *   _session_id        → _conversation_id
 */

namespace chatnow
{

enum class MemberRole : unsigned char {
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

#pragma db object table("conversation_member")
class ConversationMember
{
public:
    ConversationMember() = default;
    ConversationMember(const std::string &cid,
                       const std::string &uid,
                       bool muted,
                       bool visible,
                       MemberRole role,
                       const boost::posix_time::ptime &join_time)
        : _conversation_id(cid), _user_id(uid),
          _muted(muted), _visible(visible),
          _role(role), _join_time(join_time) {}

    std::string conversation_id() const { return _conversation_id; }
    void conversation_id(const std::string &v) { _conversation_id = v; }

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
    bool is_pinned() const { return static_cast<bool>(_pin_time); }

    MemberRole role() const { return _role; }
    void role(MemberRole v) { _role = v; }

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

    std::string draft() const { return _draft ? *_draft : std::string(); }
    void draft(const std::string &v) { _draft = v; }
    void clear_draft() { _draft = odb::nullable<std::string>(); }
    bool has_draft() const { return static_cast<bool>(_draft); }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("varchar(32)")
    std::string _conversation_id;

    #pragma db type("varchar(32)")
    std::string _user_id;

    #pragma db type("bigint unsigned")
    unsigned long _last_read_seq {0};

    #pragma db type("bigint unsigned")
    unsigned long _last_ack_seq {0};

    #pragma db type("tinyint(1)")
    bool _muted {false};

    #pragma db type("tinyint(1)")
    bool _visible {true};

    #pragma db type("DATETIME(3)")
    odb::nullable<boost::posix_time::ptime> _pin_time;

    #pragma db type("tinyint unsigned")
    MemberRole _role {MemberRole::NORMAL};

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

    #pragma db type("tinyint(1)")
    bool _is_quit {false};

    #pragma db type("DATETIME(3)")
    odb::nullable<boost::posix_time::ptime> _quit_time;

    #pragma db type("text")
    odb::nullable<std::string> _draft;

    #pragma db index("uk_conv_user") unique members(_conversation_id, _user_id)
    #pragma db index("idx_user_conv") members(_user_id, _is_quit, _conversation_id)
};

} // namespace chatnow
