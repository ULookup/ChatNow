#pragma once
#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

/**
 * 会话主表 (conversation) — 替代旧 chat_session.hxx
 * 字段集合不变，仅类名/枚举名/表名 rename：
 *   ChatSession         → Conversation
 *   ChatSessionType     → ConversationType   (PRIVATE=1, GROUP=2, CHANNEL=3)
 *   ChatSessionStatus   → ConversationStatus (NORMAL/ARCHIVED/DISMISSED/BANNED)
 *   table chat_session  → table conversation
 *   _chat_session_id    → _conversation_id
 *   _chat_session_name  → _conversation_name
 *   _chat_session_type  → _conversation_type
 */

namespace chatnow
{

enum class ConversationType : unsigned char {
    PRIVATE = 1,
    GROUP   = 2,
    CHANNEL = 3
};

enum class ConversationStatus : unsigned char {
    NORMAL    = 0,
    ARCHIVED  = 1,
    DISMISSED = 2,
    BANNED    = 3
};

#pragma db object table("conversation")
class Conversation
{
public:
    Conversation() = default;
    Conversation(const std::string &cid,
                 const std::string &cname,
                 ConversationType ctype,
                 const boost::posix_time::ptime &create_time,
                 int member_count,
                 ConversationStatus status)
        : _conversation_id(cid), _conversation_name(cname),
          _conversation_type(ctype), _create_time(create_time),
          _member_count(member_count), _status(status) {}

    std::string conversation_id() const { return _conversation_id; }
    void conversation_id(const std::string &v) { _conversation_id = v; }

    std::string conversation_name() const { return _conversation_name ? *_conversation_name : std::string(); }
    void conversation_name(const std::string &v) { _conversation_name = v; }

    ConversationType conversation_type() const { return _conversation_type; }
    void conversation_type(ConversationType v) { _conversation_type = v; }

    boost::posix_time::ptime create_time() const { return _create_time; }
    void create_time(const boost::posix_time::ptime &v) { _create_time = v; }

    int member_count() const { return _member_count; }
    void member_count(int v) { _member_count = v; }

    ConversationStatus status() const { return _status; }
    void status(ConversationStatus v) { _status = v; }

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
    std::string _conversation_id;

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _conversation_name;

    #pragma db type("tinyint unsigned")
    ConversationType _conversation_type {ConversationType::PRIVATE};

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _create_time;

    #pragma db type("int unsigned")
    int _member_count {0};

    #pragma db type("tinyint unsigned")
    ConversationStatus _status {ConversationStatus::NORMAL};

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _avatar_id;

    #pragma db type("varchar(32)")
    odb::nullable<std::string> _owner_id;

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

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _update_time;
};

} // namespace chatnow
