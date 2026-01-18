#pragma once

#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>
#include "chat_session.hxx"
#include "chat_session_member.hxx"

namespace chatnow
{

// 这里的条件必须是指定条件: css::chat_session_type == 1 && csm1.user_id=uid && csm2.user_id != csm1.user_id
#pragma db view object(ChatSession = css)\
                object(ChatSessionMember = csm1 : css::_chat_session_id == csm1::_session_id)\
                object(ChatSessionMember = csm2 : css::_chat_session_id == csm2::_session_id)\
                query((?))
struct SingleChatSession 
{
    #pragma db column(css::_chat_session_id)
    std::string chat_session_id;
    #pragma db column(csm2::_user_id)
    std::string friend_id;
};

//这里的条件必须是指定条件: ChatSession::chat_session_type == 2 && csm1.user_id=uid
#pragma db view object(ChatSession = css)\
                object(ChatSessionMember = csm : css::_chat_session_id == csm::_session_id)\
                query((?))
struct GroupChatSession 
{
    #pragma db column(css::_chat_session_id)
    std::string chat_session_id;
    #pragma db column(css::_chat_session_name)
    std::string chat_session_name;
};

// 这个是读取会话列表的视图
#pragma db view \
        object(ChatSession = cs) \
        object(ChatSessionMember = cm : cs::_chat_session_id == cm::_session_id) \
        query((?))
struct OrderedChatSessionView
{
    #pragma db column(cs::_chat_session_id)
    std::string session_id;

    #pragma db column(cs::_chat_session_name)
    std::string session_name;

    #pragma db column(cs::_chat_session_type)
    ChatSessionType session_type;

    #pragma db column(cs::_create_time)
    boost::posix_time::ptime create_time;

    #pragma db column(cs::_last_message_time)
    odb::nullable<boost::posix_time::ptime> last_message_time;

    #pragma db column(cs::_member_count)
    int member_count;

    #pragma db column(cs::_status)
    int status;

    #pragma db column(cs::_avatar_id)
    odb::nullable<std::string> avatar_id;

    #pragma db column(cm::_unread_count)
    unsigned int unread_count;

    #pragma db column(cm::_pin_time)
    odb::nullable<boost::posix_time::ptime> pin_time;

    #pragma db column(cm::_muted)
    bool muted;

    #pragma db column(cm::_visible)
    bool visible;
};

//这个是读取会话成员列表是视图
#pragma db view \
    object(ChatSessionMember = cm) \
    query((?))
struct ChatSessionMemberRoleView
{
    #pragma db column(cm::_session_id)
    std::string session_id;
    
    #pragma db column(cm::_user_id)
    std::string user_id;

    #pragma db column(cm::_role)
    ChatSessionRole role;

    #pragma db column(cm::_join_time)
    boost::posix_time::ptime join_time;
};

} // namespace chatnow