# ChatNow 缓存机制重构设计

> **状态**: 设计完成，待评审
> **日期**: 2026-05-13
> **基线**: main @ `55decee`
> **范围**: Members 缓存一致性 / 穿透击穿防护 / UserInfo 缓存层 / LastMessage 缓存接入

---

## 0. 现状全景

### 0.1 Redis 数据分类

当前系统有 10 类 Redis 数据。按性质分为两层：

**状态存储（Redis 是真相源，不是缓存）：**

| Key | 数据 | TTL | 一致性需求 |
|---|---|---|---|
| `im:sess:{ssid}` | 登录态 | 7d | 无（丢了用户重新登录） |
| `im:status:{uid}` | 在线标记 | 5min | 无（心跳续期，不续即过期） |
| `im:code:{id}` | 验证码 | 5min | 无 |
| `im:seq:ssid/uid:*` | 序号 | 永久 | 无（Redis 是唯一真相源） |
| `im:online:{uid}` | 在线路由 | 60s | 无（心跳续期） |
| `im:unack:{uid}` | 未ACK消息 | 7d | 无（Redis 是唯一真相源） |
| `im:push:outbox` | 投递失败队列 | 永久 | 无 |
| `im:es:outbox` | ES 索引失败队列 | 永久 | 无（新增，见 ES 双写设计） |
| `im:worker:*` | worker_id 租约 | 300s | 无（Redis 是唯一真相源） |
| `im:rl:*` | 限流计数 | 60s | 无 |

**真正的缓存（Redis 是 DB 副本，存在一致性问题）：**

| Key | 数据 | TTL | 写入方 | 读取方 | 一致性 |
|---|---|---|---|---|---|
| `im:members:{ssid}` | 群成员列表 | 30min | Transmite(warm), ChatSession(invalidate) | Transmite | **有竞态窗口** |
| （不存在） | 用户信息 | — | — | — | **每条消息都 RPC** |
| （已定义未使用） | 最近消息预览 | — | — | — | **会话列表走 DB 视图** |

### 0.2 核心问题

1. **Members 缓存的 invalidate + warm 竞态**：DEL 和 SADD 之间的空窗期，并发 warm 可能用旧数据覆盖新数据
2. **Members 缓存固定 TTL**：全服务重启后首次消息集中 warm，30min 后集中过期
3. **热点群 Members 击穿**：大群 key 过期瞬间，数十个并发请求同时穿透到 ChatSession
4. **用户信息无缓存**：每条消息发送都 RPC 查 sender 信息，每条消息拉取都批量 RPC 查多用户
5. **最近消息预览无缓存**：会话列表每次查 DB 视图

---

## 1. 改进一：Members 缓存增量维护（消除竞态）

### 1.1 改前 vs 改后

```
改前（全量 invalidate）:
  ChatSession 加人 → DEL im:members:{ssid}
  Transmite 发消息 → list() 未命中 → RPC → SADD {全量列表}
  竞态: DEL 之后、SADD 之前，另一个 list() 可能拿到空 → 再次 RPC → 旧数据覆盖新数据

改后（增量维护）:
  ChatSession 加人 → SADD im:members:{ssid} uid  (+ EXPIRE 续期)
  ChatSession 踢人 → SREM im:members:{ssid} uid
  Transmite warm → SADD im:members:{ssid} uid1 uid2 ... (+ EXPIRE 续期)
  无竞态: SADD 是幂等的，不存在"空窗期"
```

**关键区别**：不再 DEL key。Redis SET 的 SADD/SREM 操作是原子的，多客户端并发操作同一个 key 时，Redis 内部串行执行，结果始终是各操作的累积并/差集。

### 1.2 竞态场景走查

```
场景：加人 U2 + 同时发消息

改前:
  Transmite list() → {U1}        ChatSession DEL
  （中间窗口: key 不存在）         ChatSession DB insert U2
  Transmite warm({U1}) → {U1}    （U2 丢失！）

改后:
  Transmite list() → {U1}        ChatSession SADD U2 → {U1, U2}
  Transmite warm({U1}) → SADD   （幂等: {U1} + {U1, U2} = {U1, U2}, U2 存在！）
```

### 1.3 代码改动

#### `common/dao/data_redis.hpp::Members` — warm 改为 SADD 变体

当前：

```cpp
void warm(const std::string &ssid, const std::vector<std::string> &uids,
          std::chrono::seconds ttl = kMembersTtl) {
    if (uids.empty()) return;
    try {
        std::string k = key::kMembers + ssid;
        _c->sadd(k, uids.begin(), uids.end());
        _c->expire(k, ttl);
    } catch (...) { ... }
}
```

`warm` 本身就是 SADD，不需要改。关键是 **ChatSession 端不再调用 invalidate()，改为调用 add() / remove()**。

#### `chatsession/source/chatsession_server.h` — 4 处 invalidate 改为增量操作

**AddChatSessionMember（加人）：**

```
改前: if(_members_cache) _members_cache->invalidate(ssid);
改后: if(_members_cache) {
          for (const auto &uid : new_member_list)
              _members_cache->add(ssid, uid);
          _members_cache->touch_ttl(ssid);   // 续期，见 1.4
      }
```

**RemoveChatSessionMember（踢人）：**

```
改前: if(_members_cache) _members_cache->invalidate(ssid);
改后: if(_members_cache) {
          for (const auto &uid : removed_member_list)
              _members_cache->remove(ssid, uid);
      }
```

**QuitChatSession（退群）：**

```
改前: if(_members_cache) _members_cache->invalidate(ssid);
改后: if(_members_cache) _members_cache->remove(ssid, uid);
```

**ChatSessionCreate（建群）：**

```
改前: if(_members_cache) _members_cache->invalidate(cssID);
改后: if(_members_cache) _members_cache->warm(ssid, member_list, kMembersTtl);
      // 建群是首次写入，用 warm 初始化整个集合
```

**解散会话 / 所有人退出**：保留 DEL 调用（`invalidate`），因为数据已无意义。

### 1.4 `Members` 新增 `touch_ttl` 方法

加人时需要续期 key 的 TTL，否则 30 分钟后整个 key（含老成员和新成员）一起过期：

```cpp
void touch_ttl(const std::string &ssid,
               std::chrono::seconds ttl = kMembersTtl) {
    try { _c->expire(key::kMembers + ssid, ttl); }
    catch (std::exception &e) {
        LOG_ERROR("Members.touch_ttl 失败 {}: {}", ssid, e.what());
    }
}
```

---

## 2. 改进二：随机 TTL 偏移（防雪崩）

### 2.1 问题

当前所有 Members key 固定 30min TTL。虽然消息发送时间分散降低了集中过期概率，但全服务重启后第一批消息的 warm 时间相近，30min 后可能集中过期。

### 2.2 方案

在 warm 和 touch_ttl 时加随机偏移：

```cpp
static std::chrono::seconds randomized_ttl(std::chrono::seconds base) {
    long base_sec = base.count();
    long jitter = base_sec / 5;  // 20% 偏移
    // 线程安全随机
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<long> dist(0, jitter);
    return std::chrono::seconds(base_sec + dist(rng));
}
```

改动点：`warm()` 和 `touch_ttl()` 中用 `randomized_ttl(kMembersTtl)` 替换硬编码的 `kMembersTtl`。

---

## 3. 改进三：Singleflight 防击穿

### 3.1 问题

热点群（如 500 人大群）Members key 过期瞬间，10 个人同时发消息 → 10 个 Transmite bthread 都 cache miss → 10 个并发 RPC 打到 ChatSession 查同一个群成员列表。

### 3.2 方案：per-key 互斥 + double-check

在 Transmite 侧（而非 Members 类内部）实现。逻辑：

```
Transmite::resolve_members(ssid):
  ① members = _members_cache->list(ssid)
  ② if members not empty → return members   // 快速路径

  ③ // cache miss: 拿 per-key 锁
     auto mu = _inflight.get_or_create(ssid)
     std::lock_guard lk(*mu)

  ④ // double-check: 可能第一个请求刚 warm 完
     members = _members_cache->list(ssid)
     if members not empty → return members

  ⑤ // 真正穿透: 执行 RPC
     members = RPC ChatSession.GetMemberIdList(ssid)
     _members_cache->warm(ssid, members)
     _inflight.remove(ssid)    // 释放锁，让后续请求快速路径通过
     return members
```

### 3.3 代码改动：新增 `InflightRegistry`

放在 `common/utils/` 下，轻量级工具类：

```cpp
// common/utils/inflight.hpp
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace chatnow {

class InflightRegistry {
public:
    using ptr = std::shared_ptr<InflightRegistry>;

    // 获取或创建 key 对应的互斥锁（shared_ptr 保证外部持有期间锁对象不被析构）
    std::shared_ptr<std::mutex> acquire(const std::string &key) {
        std::lock_guard<std::mutex> lk(_mu);
        auto it = _inflight.find(key);
        if (it != _inflight.end()) {
            return it->second;  // 已有在途请求，返回同一个锁
        }
        auto mu = std::make_shared<std::mutex>();
        _inflight[key] = mu;
        return mu;
    }

    void release(const std::string &key) {
        std::lock_guard<std::mutex> lk(_mu);
        _inflight.erase(key);
    }

private:
    std::mutex _mu;
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> _inflight;
};

} // namespace chatnow
```

`InflightRegistry` 实例作为 `TransmiteServiceImpl` 的成员，由 builder 注入。

每个 key 的生命周期：`acquire(ssid)` → lock → double-check → RPC+warm → unlock → `release(ssid)`。锁对象由 `shared_ptr` 管理，即使 `release` 先于 unlock 执行（正常不会），锁对象也不会被析构（调用方仍持有一个引用）。

---

## 4. 改进四：UserInfo 缓存层（新增）

### 4.1 问题

当前用户信息（昵称、头像、签名）**完全没有缓存**：

- 每条消息发送：Transmite 同步 RPC `GetUserInfo` 拿 sender 信息
- 每条消息拉取：Message 服务批量 RPC `GetMultiUserInfo` 拿所有 sender
- 会话成员列表：ChatSession 拉完成员还要每条 RPC 拉昵称/头像

对于日活 100 万、人均每天发 50 条消息的系统：每天 5000 万次 `GetUserInfo` RPC。其中 99% 是查重复的用户信息（一个活跃用户的昵称/头像几个月不变）。

### 4.2 方案：Cache-Aside + 被动失效

```
Key: im:user:{uid}          → 序列化的 UserInfo proto (不含 avatar bytes)
TTL: 1 小时

读路径:
  get_user(uid):
    r = redis.get("im:user:" + uid)
    if r: return UserInfo.Parse(r)
    info = RPC User.GetUserInfo(uid)
    redis.set("im:user:" + uid, info.Serialize(), TTL=1h + random(0, 12min))
    return info

写路径（User 服务，用户修改个人信息时）:
  SetNickname / SetAvatar / SetDescription / SetMail
    → 成功返回后: redis.del("im:user:" + uid)
```

**为什么不做主动更新（cache.set）而是 DEL（被动失效）：**
- 用户改个人信息频率极低（人均每月可能不到 1 次）
- DEL 比 SET 更安全（避免序列化错误、部分字段丢失）
- 下次有人查这个 uid 时自然 cold-RPC 重建，延迟 5-20ms 对用户完全无感

### 4.3 哪些服务需要改

| 服务 | 当前 | 改后 |
|---|---|---|
| Transmite::GetTransmitTarget | 同步 RPC GetUserInfo | `_user_cache->get(uid)` → hit 直接用 / miss RPC+set |
| Message::onDBMessage | 不用查用户信息 | 无改动（InternalMessage 已包含 sender） |
| Message::GetRecentMsg | 批量 RPC GetMultiUserInfo | `_user_cache->get_multi(uids)` → 命中直接用 / miss 列表批量 RPC+set |
| Message::GetHistoryMsg | 批量 RPC GetMultiUserInfo | 同上 |
| ChatSession::GetChatSessionMember | 每条 RPC GetMultiUserInfo | `_user_cache->get_multi(uids)` |
| User::SetNickname/SetAvatar/... | 无 | 成功返回后 DEL `im:user:{uid}` |

### 4.4 UserInfoCache 类设计

放在 `common/dao/data_redis.hpp` 中：

```cpp
class UserInfoCache {
public:
    using ptr = std::shared_ptr<UserInfoCache>;
    UserInfoCache(const std::shared_ptr<sw::redis::Redis> &c) : _c(c) {}

    // 批量获取，返回 uid→UserInfo 映射；cache hit 的直接返回，miss 的返回空 UserInfo（uid 为空）
    // 调用方拿到 miss 列表后批量 RPC，再调 batch_set 回填
    std::unordered_map<std::string, UserInfo> batch_get(
        const std::vector<std::string> &uids)
    {
        std::unordered_map<std::string, UserInfo> res;
        if (uids.empty()) return res;
        try {
            std::vector<std::string> keys;
            keys.reserve(uids.size());
            for (const auto &uid : uids)
                keys.push_back(kUserCachePrefix + uid);
            auto vals = _c->mget(keys.begin(), keys.end());
            for (size_t i = 0; i < uids.size(); ++i) {
                if (vals[i]) {
                    UserInfo info;
                    if (info.ParseFromString(*vals[i]))
                        res[uids[i]] = std::move(info);
                }
            }
        } catch (...) { LOG_ERROR("UserInfoCache.batch_get 失败"); }
        return res;
    }

    // 单个获取
    std::optional<UserInfo> get(const std::string &uid) {
        auto map = batch_get({uid});
        auto it = map.find(uid);
        if (it != map.end()) return it->second;
        return std::nullopt;
    }

    // 回填
    void batch_set(const std::unordered_map<std::string, UserInfo> &infos) {
        if (infos.empty()) return;
        try {
            auto pipe = _c->pipeline();
            for (const auto &kv : infos) {
                std::string key = kUserCachePrefix + kv.first;
                std::string val = kv.second.SerializeAsString();
                auto ttl = randomized_ttl(kUserCacheTtl);
                pipe.set(key, val, ttl);
            }
            pipe.exec();
        } catch (...) { LOG_ERROR("UserInfoCache.batch_set 失败"); }
    }

    void invalidate(const std::string &uid) {
        try { _c->del(kUserCachePrefix + uid); }
        catch (...) { LOG_ERROR("UserInfoCache.invalidate 失败 {}", uid); }
    }

private:
    std::shared_ptr<sw::redis::Redis> _c;
    static constexpr const char *kUserCachePrefix = "im:user:";
    static constexpr std::chrono::seconds kUserCacheTtl{3600};  // 1h
};
```

### 4.5 Transmite 接入示例

```cpp
// GetTransmitTarget 中，替换同步 GetUserInfo RPC:
UserInfo sender_info;
auto cached = _user_cache->get(uid);
if (cached.has_value()) {
    sender_info = *cached;
} else {
    // 现有的 brpc GetUserInfo 调用
    // ... RPC 成功后将 user_rsp.user_info() 写入缓存
    _user_cache->batch_set({{uid, user_rsp.user_info()}});
    sender_info = user_rsp.user_info();
}
```

---

## 5. 改进五：LastMessage 预览缓存接入（锦上添花）

### 5.1 问题

`LastMessage` 类已定义但从未使用。当前会话列表每次查 DB 视图 `list_ordered_by_user` 来展示"最后一条消息预览"。

### 5.2 方案

在 `onDBMessage` 的 DB commit 成功后，更新 `im:last:{ssid}`：

```cpp
// trans.commit() 之后:
if (_last_msg_cache) {
    std::string preview = build_preview_json(msg_info);  // {type, sender_name, content_preview, ts}
    _last_msg_cache->set(msg_info.chat_session_id(), preview);
}
```

ChatSession 的 `GetChatSessionList` 中，先从 Redis 批量 MGET `im:last:{ssid}`，缺失的再从 DB 查。

### 5.3 优先级

这是性能优化（减少 DB 查询），非正确性问题。建议在 Members 和 UserInfo 缓存完成后再做。

---

## 6. 改动清单

| 文件 | 改动 | 行数 |
|---|---|---|
| `common/dao/data_redis.hpp` | `Members` 新增 `touch_ttl`、`randomized_ttl` 辅助函数；新增 `UserInfoCache` 类 | +80 |
| `common/utils/inflight.hpp` | 新增 `InflightRegistry`（per-key 互斥） | +35 |
| `chatsession/source/chatsession_server.h` | 4 处 `invalidate` 改为增量 `add`/`remove`/`warm` | ±20 |
| `transmite/source/transmite_server.h` | `TransmiteServiceImpl` 新增 `_inflight`、接入 singleflight + UserInfoCache | +40 |
| `transmite/source/transmite_server.h` | `TransmiteServerBuilder` 新增 `_user_cache` / `_inflight` 构造和注入 | +15 |
| `message/source/message_server.h` | `GetRecentMsg`/`GetHistoryMsg` 接入 UserInfoCache | +30 |
| `message/source/message_server.h` | `MessageServerBuilder` 新增 `_user_cache` 注入 | +8 |
| `message/source/message_server.h` | `onDBMessage` 接入 LastMessage 缓存 | +8 |
| `user/source/user_server.h` | `SetNickname`/`SetAvatar`/`SetDescription`/`SetMail` 成功后 DEL 缓存 | +12 |
| `conf/transmite_server.conf` | （可选）缓存参数配置化 | +3 |
| **总计** | | **~250 行** |

---

## 7. 上线顺序

按依赖关系分三个阶段：

### 阶段 1：Members 缓存修复（无依赖，独立上线）

- `Members` 新增 `touch_ttl`、`randomized_ttl`
- `ChatSession` 端 4 处 invalidate 改增量操作
- `InflightRegistry` + Transmite 端接入 singleflight
- **期望效果**：消除竞态窗口、消除击穿、消除雪崩

### 阶段 2：UserInfo 缓存层（依赖阶段 1 的 InflightRegistry 模式经验）

- `UserInfoCache` 类
- Transmite 接入
- Message 服务接入
- User 服务接入失效
- **期望效果**：User RPC 调用量下降 95%+

### 阶段 3：LastMessage 缓存（独立，低优先级）

- `onDBMessage` 写入 LastMessage 缓存
- `ChatSession::GetChatSessionList` 读取 LastMessage 缓存
- **期望效果**：会话列表 DB 查询减少

---

## 8. Redis 内存估算

以 100 万日活、10 万个群聊为例：

| Key | 单条大小 | 数量 | 总内存 |
|---|---|---|---|
| `im:members:{ssid}` | ~200B/人 × 平均 50 人 = 10KB | 10 万 | ~1GB |
| `im:user:{uid}` | ~200B (proto) | 100 万 | ~200MB |
| `im:last:{ssid}` | ~150B | 10 万 | ~15MB |
| **新增总计** | | | **~1.2GB** |

现有 Redis 数据（session/status/online/seq/unacked/outbox）预估约 2-3GB。新增后总计 3-4GB，单实例 Redis 完全够用。达千万级 DAU 时需考虑 Redis Cluster 按 uid/ssid hash 分片。

---

## 9. 一致性保证矩阵

| 缓存 | 一致性模型 | 最大不一致窗口 | 不一致后果 |
|---|---|---|---|
| `im:members:{ssid}` | 最终一致（增量 SADD/SREM） | 0（无窗口） | — |
| `im:user:{uid}` | 最终一致（被动失效 DEL） | TTL 剩余时间（最多 1h） | 用户看到旧昵称/头像（可接受） |
| `im:last:{ssid}` | 最终一致（直接 SET） | 0（已有消息必然覆盖） | — |

---

## 10. 测试要点

### Members 缓存
- [ ] 加人同时发消息：新成员收到推送（竞态修复验证）
- [ ] 踢人后发消息：被踢成员不收推送
- [ ] 10 个并发请求同一 session（cache miss）：只有 1 个 RPC（singleflight 验证）
- [ ] 全服务重启后 100 个会话同时发消息：成员列表正常

### UserInfo 缓存
- [ ] 首次查用户信息：cache miss → RPC → 回填 → 第二次命中
- [ ] 用户改昵称：缓存 DEL → 下次查自动重建（新昵称）
- [ ] 批量查 50 个用户：50% 命中 / 50% miss → 只对 miss 的发起批量 RPC

### LastMessage 缓存
- [ ] 发消息后会话列表立刻显示新消息预览
- [ ] 缓存过期后会话列表回退 DB（无异常）
