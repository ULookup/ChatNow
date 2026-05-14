# ChatNow Proto 重构 — DB 表 & Redis 缓存变更方案

> **状态**: 设计完成，待评审
> **日期**: 2026-05-14
> **依赖**: `docs/superpowers/specs/2026-05-14-proto-redesign.md`
> **范围**: 与 proto 重构对齐的 ODB 表结构变更和 Redis 缓存 key 变更

---

## 0. 分析方法

以 proto 重构设计（`2026-05-14-proto-redesign.md`）中新增/变更的每个 Message 和 RPC 字段为起点，逐项检查：
1. 该字段是否需要持久化到 DB（是 → 哪张表，否 → 仅 proto 传输层）
2. 是否需要 Redis 缓存加速（是 → 新增 key，否 → 仅 DB 查询）
3. 现有表/Redis key 是否已满足（是 → 不变，否 → 标记变更）

---

## 1. DB 表变更总览

| 操作 | 表 | 说明 |
|------|-----|------|
| 新增 | `message_reaction` | 消息表情回应 |
| 新增 | `message_pin` | 会话 pinned 消息 |
| 加列 | `message` | + edit_time, forward_from_uid, forward_at |
| 枚举扩展 | `message` | MessageType 加 STICKER=7 |
| 加列 | `chat_session_member` | + draft |
| 枚举扩展 | `chat_session` | ChatSessionType 加 CHANNEL=3 |
| 不变 | user, user_timeline, relation, friend_apply, message_attachment, message_read, message_mention, user_device, chat_session_view | 已满足需求 |

---

## 2. 新增表详细设计

### 2.1 message_reaction（消息表情回应）

一条消息可被多人用不同 emoji 回应。独立表而非 message 列的 JSON 字段的原因是：热门消息可能有几十种 emoji × 数百人，单行存储会撑爆；独立表支持按 message_id + emoji 分页查询。

```cpp
#pragma db object table("message_reaction")
class MessageReaction
{
public:
    MessageReaction() = default;
    MessageReaction(unsigned long mid, const std::string &uid, const std::string &e)
        : _message_id(mid), _user_id(uid), _emoji(e) {}

    unsigned long message_id() const;
    std::string user_id() const;
    std::string emoji() const;             // unicode emoji 或自定义表情 ID
    boost::posix_time::ptime create_time() const;

private:
    #pragma db id auto
    unsigned long _id;

    #pragma db type("bigint unsigned")
    unsigned long _message_id;

    #pragma db type("varchar(32)")
    std::string _user_id;

    #pragma db type("varchar(16)")        // emoji 最长约 8 字节 UTF-8，留 16 余量
    std::string _emoji;

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _create_time;

    // 同一用户对同一消息同一 emoji 只一行：toggle 模式（再点取消 = DELETE）
    #pragma db index("uk_msg_user_emoji") unique members(_message_id, _user_id, _emoji)
    // 按消息取所有 reaction 分组
    #pragma db index("idx_msg") members(_message_id)
};
```

写入策略：
- 正常路径：直接 INSERT，冲突则 DELETE（toggle 取消）
- 高 QPS 场景（热门消息）：Redis SET 暂存 `im:react:buf:{message_id}`，后台 500ms 批量刷库

### 2.2 message_pin（会话 Pinned 消息）

独立表动机：pin 是低频操作（每个会话通常 0~3 条 pinned），独立表避免在主表加低基数布尔列；且可记录谁 pin 的、何时 pin 的。

```cpp
#pragma db object table("message_pin")
class MessagePin
{
public:
    MessagePin() = default;
    MessagePin(const std::string &ssid, unsigned long mid, const std::string &by)
        : _session_id(ssid), _message_id(mid), _pinned_by(by) {}

    std::string session_id() const;          // 所属会话 ID（沿用 DB 列名，DAO 映射为 conversation_id）
    unsigned long message_id() const;
    std::string pinned_by() const;
    boost::posix_time::ptime pinned_at() const;

private:
    #pragma db id auto
    unsigned long _id;

    #pragma db type("varchar(32)")
    std::string _session_id;

    #pragma db type("bigint unsigned")
    unsigned long _message_id;

    #pragma db type("varchar(32)")
    std::string _pinned_by;

    #pragma db type("DATETIME(3)")
    boost::posix_time::ptime _pinned_at;

    #pragma db index("uk_conv_msg") unique members(_session_id, _message_id)
    #pragma db index("idx_conv") members(_session_id)
};
```

---

## 3. 修改表详细设计

### 3.1 message 表加列

在现有 `message.hxx` 的 `Message` 类中新增：

```cpp
// —— 编辑时间 ——
#pragma db type("DATETIME(3)")
odb::nullable<boost::posix_time::ptime> _edit_time;

// —— 转发来源 ——
#pragma db type("varchar(32)")
odb::nullable<std::string> _forward_from_uid;

#pragma db type("DATETIME(3)")
odb::nullable<boost::posix_time::ptime> _forward_at;
```

对应 getter/setter：
```cpp
boost::posix_time::ptime edit_time() const;
void edit_time(const boost::posix_time::ptime &v);

std::string forward_from_uid() const;
void forward_from_uid(const std::string &v);

boost::posix_time::ptime forward_at() const;
void forward_at(const boost::posix_time::ptime &v);
```

MessageType 枚举对齐：
```cpp
enum class MessageType : unsigned char {
    UNKNOWN       = 0,
    TEXT          = 1,
    IMAGE         = 2,
    FILE          = 3,
    AUDIO         = 4,   // 原名 SPEECH，值不变
    VIDEO         = 5,   // 已预留，保持不变
    LOCATION      = 6,   // 已预留，保持不变
    STICKER       = 7,   // 新增（原 CARD=7 改为 STICKER=7）
    SYSTEM_NOTICE = 8    // 新增（原 SYSTEM=99 迁移至此）
};
```

说明：原 `SPEECH=4` 不改值，仅注释标记为 AUDIO（兼容已有数据）。原 `CARD=7` / `SYSTEM=99` 未在线上使用，直接调整。

### 3.2 chat_session_member 表加列

```cpp
// —— 草稿 ——
#pragma db type("text")
odb::nullable<std::string> _draft;
```

对应 getter/setter：
```cpp
std::string draft() const;
void draft(const std::string &v);
```

### 3.3 chat_session 表枚举扩展

```cpp
enum class ChatSessionType : unsigned char {
    SINGLE  = 1,
    GROUP   = 2,
    CHANNEL = 3    // 新增：频道/广播（预留，暂不实现）
};
```

---

## 4. 不做的事（设计决策）

| 决定 | 理由 |
|------|------|
| **不重命名 DB 列** | `session_id`、`user_id`、`chat_session_name` 等沿用现有列名。Proto 的 `conversation_id` / `sender_id` / `name` 等重命名仅在 DAO→proto 映射层转换。避免大规模 DB migration 风险和线上数据不一致窗口。 |
| **is_pinned 不走 message 列** | 独立 message_pin 表，避免在主表加低基数布尔列。且独立表可以记录"谁 pin 的、何时"。 |
| **reactions 独立表** | 热门消息可能有几十种 emoji × 数百人，单行 JSON 会撑爆。独立表支持高效分页和索引。 |
| **mentioned_user_ids 不存 message 列** | 已有 message_mention 表，无需变更。 |
| **draft 存 DB 而非 Redis** | 草稿不是实时同步场景（秒级延迟可接受），存 DB 避免 Redis 内存膨胀，且不丢。 |

---

## 5. Redis 缓存变更

### 5.1 新增 Key（Presence 域）

Presence 实时态全部在 Redis，DB 的 `user_device.online_status` 仅作 last-known 落库快照。

```
im:presence:{uid}              HASH
  state          → "online" | "away" | "busy" | "offline" | "invisible"
  last_active    → 毫秒时间戳
  custom_status  → 自定义状态文字（可选）

im:presence:devices:{uid}      SET<device_id>
  TTL: 120s（Push 心跳每 60s 续期）

im:presence:typing:{uid}       SET<conversation_id>
  TTL: 10s（输入中指示超时）

im:presence:sub:{uid}          SET<user_id>
  "我订阅了哪些人的状态变化"（用于推送 PresenceChange 通知）
```

### 5.2 建议改名（可选，向后兼容）

这些 key 前缀改名不是必须的，旧 key 继续工作。建议在新 DAO 中使用新 key，过渡期双写。

```
im:seq:ssid:   →  im:seq:conv:       (session → conversation)
im:last:       →  im:last:msg:       (更明确含义)
im:rl:ssid:    →  im:rl:conv:        (session → conversation)
```

### 5.3 不变 Key

以下 key 无需变更：`im:sess:`、`im:status:`、`im:code:`、`im:seq:uid:`、`im:dev:`、`im:read:`、`im:members:`、`im:rl:user:`、`im:online:`、`im:push:route:`、`im:unack:`、`im:push:outbox`、`im:push:outbox:lock`、`im:push:cross_outbox`、`im:push:cross_outbox:lock`、`im:es:outbox`、`im:es:outbox:lock`

---

## 6. 文件变更清单

| 操作 | 文件 |
|------|------|
| 新增 | `odb/message_reaction.hxx` |
| 新增 | `odb/message_pin.hxx` |
| 修改 | `odb/message.hxx` — 加列 + 枚举对齐 |
| 修改 | `odb/chat_session_member.hxx` — 加 draft 列 |
| 修改 | `odb/chat_session.hxx` — ChatSessionType 加 CHANNEL |
| 修改 | `common/dao/data_redis.hpp` — 新 namespace key + Presence Redis 操作类 |
| 不变 | `odb/user.hxx`, `odb/user_timeline.hxx`, `odb/relation.hxx`, `odb/friend_apply.hxx`, `odb/message_attachment.hxx`, `odb/message_read.hxx`, `odb/message_mention.hxx`, `odb/user_device.hxx`, `odb/chat_session_view.hxx` |

---

## 7. 迁移顺序

与 proto 重构的 Phase 2"逐域迁移"同步进行：

1. 先加列（message edit_time/forward_*, chat_session_member draft）— 新列为 nullable，不影响现有读写
2. 再建新表（message_reaction, message_pin）— 空表，无迁移数据
3. 新增 Redis key（presence 系列）— 全新写入路径
4. 最后改枚举（MessageType, ChatSessionType）— 需同步更新所有 DAO 引用
