#pragma once

#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>
#include "chat_session.hxx"
#include "chat_session_member.hxx"

/**
 * ===========================================================================
 * 会话列表视图 (OrderedChatSessionView) — 唯一保留的 ODB view
 * ---------------------------------------------------------------------------
 * 为什么保留：
 *   - 会话列表是 IM 最高频读路径（开 App / 切到会话页都要拉）
 *   - 真实多表 join + 投影：chat_session ⋈ chat_session_member 扁平化结果
 *     直接映射 protobuf，比加载两个 ODB 对象 + 应用层拼装明显省 CPU
 *   - 一次 SQL 拿全字段，避免 N+1
 *
 * 删掉的旧视图与原因：
 *   - SingleChatSession           → chat_session.peer_user_id 直接取，零 join
 *   - GroupChatSession            → 单纯列投影，普通 query<ChatSession> 即可
 *   - ChatSessionMemberRoleView   → 等价于 query<ChatSessionMember> 选列，view 是噪音
 *
 * 字段速览：
 *   会话维度（来自 chat_session）：
 *     session_id           会话 ID
 *     session_name         会话名称（单聊允许为空，由对方昵称兜底）
 *     session_type         单聊/群聊
 *     create_time          创建时间
 *     member_count         成员数缓存
 *     status               会话状态
 *     avatar_id            群头像
 *     owner_id             群主
 *     peer_user_id         单聊对端 user_id（取代旧 self-join 视图的核心字段）
 *     max_seq              会话最新 seq 快照
 *     muted_all            全员禁言
 *     update_time          会话元信息变更时间
 *
 *   成员维度（来自 chat_session_member，"我"在该会话的个性化设置）：
 *     pin_time             置顶时间
 *     muted                免打扰
 *     visible              显隐
 *     role                 群角色
 *     alias                群昵称
 *     last_read_seq        已读到的最大 seq（计算未读用）
 *     last_ack_seq         已确认送达的最大 seq
 *     mute_until           禁言到期时间
 * ===========================================================================
 */

namespace chatnow
{

#pragma db view                                                                  \
        object(ChatSession = cs)                                                 \
        object(ChatSessionMember = cm : cs::_chat_session_id == cm::_session_id) \
        query((?))
struct OrderedChatSessionView
{
    #pragma db column(cs::_chat_session_id)
    std::string session_id;

    #pragma db column(cs::_chat_session_name)
    odb::nullable<std::string> session_name;

    #pragma db column(cs::_chat_session_type)
    ChatSessionType session_type;

    #pragma db column(cs::_create_time)
    boost::posix_time::ptime create_time;

    #pragma db column(cs::_member_count)
    int member_count;

    #pragma db column(cs::_status)
    ChatSessionStatus status;

    #pragma db column(cs::_avatar_id)
    odb::nullable<std::string> avatar_id;

    #pragma db column(cs::_owner_id)
    odb::nullable<std::string> owner_id;

    // 单聊对端 ID：直接由主表带出，无需 self-join
    #pragma db column(cs::_peer_user_id)
    odb::nullable<std::string> peer_user_id;

    #pragma db column(cs::_max_seq)
    unsigned long max_seq;

    #pragma db column(cs::_muted_all)
    bool muted_all;

    #pragma db column(cs::_update_time)
    boost::posix_time::ptime update_time;

    // —— 成员维度（"我"在该会话的个性化设置） ——
    #pragma db column(cm::_pin_time)
    odb::nullable<boost::posix_time::ptime> pin_time;

    #pragma db column(cm::_muted)
    bool muted;

    #pragma db column(cm::_visible)
    bool visible;

    #pragma db column(cm::_role)
    ChatSessionRole role;

    #pragma db column(cm::_alias)
    odb::nullable<std::string> alias;

    #pragma db column(cm::_last_read_seq)
    unsigned long last_read_seq;

    #pragma db column(cm::_last_ack_seq)
    unsigned long last_ack_seq;

    #pragma db column(cm::_mute_until)
    odb::nullable<boost::posix_time::ptime> mute_until;
};

} // namespace chatnow
