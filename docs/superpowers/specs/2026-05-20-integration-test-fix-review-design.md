# Integration Test Fix Review — 补充优化设计

> 基于 code review 发现，对 `a4bc80d` 集成测试修复的 4 项补充优化

## 背景

`a4bc80d` ("test: fix integration test failures for Redis Cluster and local dev") 修复了 Redis Cluster 兼容性和本地开发环境配置。Code review 发现 3 个额外问题需要在本分支补充修复。

## 1. scan() 集群模式防御性守卫

### 问题

集群模式 `scan()` 中 `cursor != 0` 时静默返回 0，不扫描不报错。当前 4 个调用方（push_server.h:370/659, presence_server.h:51/139）都以 cursor=0 起扫，不受影响。但 API 语义不匹配：单节点模式支持迭代续扫，集群模式不支持，调用方无法从接口层面感知差异。

### 方案

```cpp
template <typename Out>
long long scan(long long cursor, const std::string &pattern, long long count, Out out) {
    if (_rc) {
        if (cursor != 0) {
            LOG_ERROR("RedisCluster scan 不支持迭代续扫，cursor 必须为 0，当前={}", cursor);
            abort();
        }
        _rc->for_each([&](sw::redis::Redis &r) {
            long long cur = 0;
            while (true) {
                cur = r.scan(cur, pattern, count, out);
                if (cur == 0) break;
            }
        });
        return 0;
    }
    return _r->scan(cursor, pattern, count, out);
}
```

### 决策理由

- **高并发**：无状态，每次调用独立完成全节点遍历
- **高可用**：无"扫了一半 cursor 丢失"的中间态
- **高性能**：当前 count=100，key 量级下内存可控；若未来量级增长应换数据结构（online 索引 SET）而非续扫
- **防御深度**：`abort()` 而非静默吞错——API 误用在单测阶段直接暴露（参照 Envoy `PANIC_ON_ASSERT` 惯例）

## 2. eval() Out 重载签名对齐

### 问题

当前分支 `eval()` 第二重载（带 `Out out` 参数）仍声明 `Ret` 模板参数和返回值，但底层 `_rc->eval` 在有 `Out` 参数时返回 `void`，签名不对齐。

### 方案

去掉 `Ret` 模板参数，返回 `void`：

```cpp
template <typename KeyIt, typename ArgIt, typename Out>
void eval(const std::string &script, KeyIt key_first, KeyIt key_last,
          ArgIt arg_first, ArgIt arg_last, Out out) {
    _rc ? _rc->eval(script, key_first, key_last, arg_first, arg_last, out)
        : _r->eval(script, key_first, key_last, arg_first, arg_last, out);
}
```

### 影响面

唯一使用此重载的调用方 `ReadAck::drain()` 不捕获返回值，安全。第一重载（无 `Out`）不变。

## 3. pipeline() hash_tag 参数显式化

### 问题

`next_user_seq_batch` 中 pipeline 调用不指定 hash_tag。虽然 `kSeqUser`/`kSeqSession` 已包含 `{seq}` 前缀（Cluster 自动路由到同 slot），但显式传入 hash_tag 自文档化意图，且不依赖底层 `sw::redis::RedisCluster::pipeline(StringView)` 对传入值的提取/路由语义。

### 方案

新增 hash tag 常量，显式传给 pipeline：

```cpp
namespace key {
    inline constexpr const char* kSeqHashTag = "{seq}";  // seq key 共享 hash tag，确保 pipeline 路由到同一 slot
}
```

`next_user_seq_batch` 调用处：

```cpp
auto pipe = _c->pipeline(key::kSeqHashTag);
```

`RedisClient::pipeline()` 签名增加默认参数：

```cpp
auto pipeline(const sw::redis::StringView &hash_tag = {}) {
    return _rc ? _rc->pipeline(hash_tag) : _r->pipeline();
}
```

## 4. Conversation ID

### 结论：不变更

`p_<min_uid>_<max_uid>` 是 WhatsApp/Signal 同款确定性派生设计。在 WhatsApp/Signal 中，私聊 ID 由用户标识符的确定性组合生成——这是主流 IM 的标准做法。

- user_id 是标识符，不是凭据。鉴权才是安全边界
- 确定性派生保证 A→B 和 B→A 得到同一个 conversation_id，是幂等创建的基石
- varchar(48) 对 Snowflake 19 位 uid（`p_` + 19 + `_` + 19 = 41 字符）充裕，未来换 UUID 再说

## 变更文件

| 文件 | 变更 |
|------|------|
| `common/dao/data_redis.hpp` | scan() 加 abort 守卫、eval() 去 Ret 模板参数、pipeline() 增加 hash_tag 默认参数、新增 kSeqHashTag 常量 |

无 proto 变更，无数据迁移，无新增文件。
