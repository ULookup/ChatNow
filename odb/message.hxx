#pragma once

#include <string>
#include <cstddef>
#include <odb/nullable.hxx>
#include <odb/core.hxx>
#include <boost/date_time/posix_time/posix_time.hpp>

/**
 * ===========================================================================
 * 消息表 (message) — 写入 QPS 最高的核心表
 * ---------------------------------------------------------------------------
 * 设计要点：
 *   1. 引入 (session_id, seq_id) 作为业务唯一键
 *      - seq_id 由 Redis INCR seq:{session_id} 生成，会话内严格连续
 *      - 客户端"会话级增量同步"基石：拉 seq > last_seq 的消息即可
 *      - message_id（雪花）保留为全局唯一标识，跨会话引用用
 *
 *   2. 分库分表友好：sharding_key = session_id
 *      - 单会话所有消息在同一分片，按 seq 范围扫天然有序
 *
 *   3. 客户端去重：client_msg_id（UUID）
 *      - 网络重发幂等：(user_id, client_msg_id) 唯一
 *
 *   4. 修正 file_size 类型 bug：旧表 unsigned char(255B 上限) → bigint unsigned
 *
 *   5. 引用 / 提及：
 *      - reply_to_msg_id 直接列存（单值）
 *      - @ 提及拆独立子表 message_mention（不在本文件，用 SQL DDL 单独建）
 *        理由：列存逗号分隔字符串无法索引，"@我"通知必须 O(n) 扫表
 *
 *   6. 附件信息独立到 message_attachment 子表
 *      - 主表保留 file_id 等字段仅用于"过渡期单文件场景"
 *      - 新功能（多图/视频/缩略图）一律写 message_attachment
 *
 *   7. 冷热分离（DDL 之外的运维策略）：
 *      - 主表按 create_time 月份分区
 *      - 3 个月以上归档冷库
 *
 * 字段速览：
 *   _id              物理主键
 *   _message_id      全局消息 ID（雪花），跨会话引用用，唯一索引
 *   _seq_id          会话内单调递增序号；增量同步主键
 *   _session_id      所属会话 ID
 *   _user_id         发送者用户 ID（不单建索引：IM 几乎无"按发送者查全消息"路径）
 *   _message_type    消息类型枚举（TEXT/IMAGE/FILE/SPEECH/VIDEO/...）
 *   _create_time     消息产生时间（服务端落库时间，权威时间戳）
 *   _content         文本消息内容（TEXT）
 *   _client_msg_id   客户端 UUID，断网重发幂等
 *   _file_id         单附件文件 ID（多附件请走 message_attachment）
 *   _file_name       单附件文件名
 *   _file_size       单附件大小（字节，bigint unsigned）
 *   _reply_to_msg_id 引用消息 ID（"回复"功能）
 *   _status          NORMAL/REVOKED/DELETED/AUDITING
 *   _revoke_time     撤回时间
 *   _revoke_by       撤回操作人 ID（管理员撤回他人消息时与 user_id 不同）
 *
 * 索引策略：
 *   _message_id          unique 全局索引
 *   uk_session_seq       (session_id, seq_id) UNIQUE — 双重作用：
 *                        ① 主查询路径（按会话拉历史，按 seq 倒序）
 *                        ② 防 MQ 重投/双写产生重复消息
 *   uk_client_msg        (user_id, client_msg_id) unique — 客户端去重
 *   说明：
 *     - 原 idx_session_time 删除：与 uk_session_seq 功能重叠（seq 与 time 单调一致）
 *     - 原 _user_id 单列索引删除：实际查询路径都从 session 切入
 * ===========================================================================
 */

namespace chatnow
{

enum class MessageType : unsigned char {
    UNKNOWN  = 0,
    TEXT     = 1,
    IMAGE    = 2,
    FILE     = 3,
    SPEECH   = 4,
    VIDEO    = 5,    // 预留
    LOCATION = 6,    // 预留
    CARD     = 7,    // 预留
    SYSTEM   = 99    // 系统注入消息（"X 加入了群"）
};

enum class MessageStatus : unsigned char {
    NORMAL   = 0,
    REVOKED  = 1,    // 已撤回
    DELETED  = 2,    // 已删除
    AUDITING = 3     // 审核中（敏感词命中暂不下发）
};

#pragma db object table("message")
class Message
{
public:
    Message() = default;
    Message(unsigned long mid,
            const std::string &ssid,
            const std::string &uid,
            MessageType mtype,
            const boost::posix_time::ptime &create_time,
            MessageStatus status)
        : _message_id(mid), _session_id(ssid), _user_id(uid),
          _message_type(mtype), _create_time(create_time), _status(status) {}

    unsigned long message_id() const { return _message_id; }
    void message_id(unsigned long v) { _message_id = v; }

    unsigned long seq_id() const { return _seq_id; }
    void seq_id(unsigned long v) { _seq_id = v; }

    std::string session_id() const { return _session_id; }
    void session_id(const std::string &v) { _session_id = v; }

    std::string user_id() const { return _user_id; }
    void user_id(const std::string &v) { _user_id = v; }

    MessageType message_type() const { return _message_type; }
    void message_type(MessageType v) { _message_type = v; }

    boost::posix_time::ptime create_time() const { return _create_time; }
    void create_time(const boost::posix_time::ptime &v) { _create_time = v; }

    std::string content() const { return _content ? *_content : std::string(); }
    void content(const std::string &v) { _content = v; }

    std::string client_msg_id() const { return _client_msg_id ? *_client_msg_id : std::string(); }
    void client_msg_id(const std::string &v) { _client_msg_id = v; }

    std::string file_id() const { return _file_id ? *_file_id : std::string(); }
    void file_id(const std::string &v) { _file_id = v; }

    std::string file_name() const { return _file_name ? *_file_name : std::string(); }
    void file_name(const std::string &v) { _file_name = v; }

    unsigned long long file_size() const { return _file_size ? *_file_size : 0; }
    void file_size(unsigned long long v) { _file_size = v; }

    unsigned long reply_to_msg_id() const { return _reply_to_msg_id ? *_reply_to_msg_id : 0; }
    void reply_to_msg_id(unsigned long v) { _reply_to_msg_id = v; }

    MessageStatus status() const { return _status; }
    void status(MessageStatus v) { _status = v; }

    boost::posix_time::ptime revoke_time() const {
        return _revoke_time ? *_revoke_time : boost::posix_time::ptime();
    }
    void revoke_time(const boost::posix_time::ptime &v) { _revoke_time = v; }

    std::string revoke_by() const { return _revoke_by ? *_revoke_by : std::string(); }
    void revoke_by(const std::string &v) { _revoke_by = v; }

private:
    friend class odb::access;

    #pragma db id auto
    unsigned long _id;

    #pragma db type("bigint unsigned") index unique
    unsigned long _message_id;

    // 会话内单调递增 seq，由 Redis INCR 生成；与 session_id 组合 unique（见下方索引）
    #pragma db type("bigint unsigned")
    unsigned long _seq_id {0};

    #pragma db type("varchar(32)")
    std::string _session_id;

    // 发送者：不单建索引（IM 几乎无"按发送者列消息"路径）
    #pragma db type("varchar(32)")
    std::string _user_id;

    #pragma db type("tinyint unsigned")
    MessageType _message_type {MessageType::TEXT};

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _create_time;

    #pragma db type("text")
    odb::nullable<std::string> _content;

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _client_msg_id;

    #pragma db type("varchar(64)")
    odb::nullable<std::string> _file_id;

    #pragma db type("varchar(255)")
    odb::nullable<std::string> _file_name;

    #pragma db type("bigint unsigned")
    odb::nullable<unsigned long long> _file_size;

    #pragma db type("bigint unsigned")
    odb::nullable<unsigned long> _reply_to_msg_id;

    #pragma db type("tinyint unsigned")
    MessageStatus _status {MessageStatus::NORMAL};

    #pragma db type("DATETIME(3)")
    odb::nullable<boost::posix_time::ptime> _revoke_time;

    #pragma db type("varchar(32)")
    odb::nullable<std::string> _revoke_by;

    // 主路径 + 防重：seq 由 Redis 分发本就唯一，UNIQUE 兜底防 MQ 重投/双写
    #pragma db index("uk_session_seq") unique members(_session_id, _seq_id)
    // 客户端断网重发幂等：MySQL unique 允许多 NULL，client_msg_id 可空不影响约束
    #pragma db index("uk_client_msg") unique members(_user_id, _client_msg_id)
};

} // namespace chatnow
