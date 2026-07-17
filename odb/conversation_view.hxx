#pragma once

#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>
#include "conversation.hxx"
#include "conversation_member.hxx"

/**
 * 会话列表视图 OrderedConversationView — 替代 OrderedChatSessionView
 *  字段集合不变；类名/列别名 rename。
 */

namespace chatnow
{

#pragma db view                                                              \
    object(Conversation = c)                                                 \
    object(ConversationMember = m : c::_conversation_id == m::_conversation_id)
struct OrderedConversationView
{
    #pragma db column(c::_conversation_id)
    std::string conversation_id;

    #pragma db column(c::_conversation_name)
    odb::nullable<std::string> conversation_name;

    #pragma db column(c::_conversation_type)
    ConversationType conversation_type;

    #pragma db column(c::_create_time)
    boost::posix_time::ptime create_time;

    #pragma db column(c::_member_count)
    int member_count;

    #pragma db column(c::_status)
    ConversationStatus status;

    #pragma db column(c::_avatar_id)
    odb::nullable<std::string> avatar_id;

    #pragma db column(c::_owner_id)
    odb::nullable<std::string> owner_id;

    #pragma db column(c::_peer_user_id)
    odb::nullable<std::string> peer_user_id;

    #pragma db column(c::_max_seq)
    unsigned long max_seq;

    #pragma db column(c::_muted_all)
    bool muted_all;

    #pragma db column(c::_update_time)
    boost::posix_time::ptime update_time;

    #pragma db column(m::_user_id)
    std::string user_id;

    #pragma db column(m::_pin_time)
    odb::nullable<boost::posix_time::ptime> pin_time;

    #pragma db column(m::_muted)
    bool muted;

    #pragma db column(m::_visible)
    bool visible;

    #pragma db column(m::_role)
    MemberRole role;

    #pragma db column(m::_alias)
    odb::nullable<std::string> alias;

    #pragma db column(m::_last_read_seq)
    unsigned long last_read_seq;

    #pragma db column(m::_last_ack_seq)
    unsigned long last_ack_seq;

    #pragma db column(m::_mute_until)
    odb::nullable<boost::posix_time::ptime> mute_until;

    #pragma db column(m::_join_time)
    boost::posix_time::ptime join_time;

    #pragma db column(m::_is_quit)
    bool is_quit;

    #pragma db column(m::_draft)
    odb::nullable<std::string> draft;
};

} // namespace chatnow
