# Integration Test Fix Review — 补充优化 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 对 `a4bc80d` 集成测试修复做 3 项补充优化：eval() 签名对齐、pipeline() hash_tag 显式化、scan() 集群模式防御性守卫。

**Architecture:** 所有变更集中在 `common/dao/data_redis.hpp`。该文件是 Redis 双模（单节点 + Cluster）的统一适配层，封装了 `RedisClient` 类。本次修改不改变任何业务调用方的行为，仅加固适配层。

**Tech Stack:** C++17, sw::redis++ (redis-plus-plus), Redis Cluster

**Spec:** `docs/superpowers/specs/2026-05-20-integration-test-fix-review-design.md`

---

## File Map

| 文件 | 职责 | 变更类型 |
|------|------|---------|
| `common/dao/data_redis.hpp` | Redis 双模适配层（RedisClient 类 + key 常量 + 业务 DAO） | 修改 |

---

### Task 1: eval() Out 重载去 Ret 模板参数

**Files:**
- Modify: `common/dao/data_redis.hpp:149-154`

将带 `Out out` 参数的第二重载从 `Ret eval<Ret>(..., Out out)` 改为 `void eval(..., Out out)`，对齐底层 `sw::redis::RedisCluster::eval` 的 void 返回签名。

- [ ] **Step 1: 替换 eval() Out 重载签名**

将 lines 149-154：
```cpp
    template <typename Ret, typename KeyIt, typename ArgIt, typename Out>
    Ret eval(const std::string &script, KeyIt key_first, KeyIt key_last,
             ArgIt arg_first, ArgIt arg_last, Out out) {
        return _rc ? _rc->eval<Ret>(script, key_first, key_last, arg_first, arg_last, out)
                   : _r->eval<Ret>(script, key_first, key_last, arg_first, arg_last, out);
    }
```

替换为：
```cpp
    template <typename KeyIt, typename ArgIt, typename Out>
    void eval(const std::string &script, KeyIt key_first, KeyIt key_last,
              ArgIt arg_first, ArgIt arg_last, Out out) {
        _rc ? _rc->eval(script, key_first, key_last, arg_first, arg_last, out)
            : _r->eval(script, key_first, key_last, arg_first, arg_last, out);
    }
```

- [ ] **Step 2: 编译验证**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make -j8 2>&1 | tail -20
```

预期：编译通过，无 eval 相关错误。唯一调用方 `ReadAck::drain()` (line 575) 不捕获返回值。

- [ ] **Step 3: Commit**

```bash
git add common/dao/data_redis.hpp
git commit -m "fix(redis): remove Ret template from eval() Out overload, return void"
```

---

### Task 2: pipeline() 增加 hash_tag 参数 + kSeqHashTag 常量

**Files:**
- Modify: `common/dao/data_redis.hpp:157-158` (pipeline 签名)
- Modify: `common/dao/data_redis.hpp:180` (kSeqSession / kSeqUser 加 {seq} 前缀)
- Modify: `common/dao/data_redis.hpp:188` 后 (新增 kSeqHashTag)
- Modify: `common/dao/data_redis.hpp:428` (next_user_seq_batch 调用处)

先给 kSeqSession / kSeqUser 加上 `{seq}` hash tag 前缀（这是 a4bc80d 的前置修复，当前分支未包含），然后新增 kSeqHashTag 常量，最后 pipeline() 签名和调用方传入 hash_tag。

- [ ] **Step 1: kSeqSession / kSeqUser 加 {seq} 前缀**

将 lines 180-181：
```cpp
    inline constexpr const char* kSeqSession = "im:seq:ssid:";      // ssid       -> 会话级 seq
    inline constexpr const char* kSeqUser    = "im:seq:uid:";       // uid        -> 用户级 seq
```

替换为：
```cpp
    inline constexpr const char* kSeqSession = "{seq}:im:seq:ssid:";  // ssid       -> 会话级 seq
    inline constexpr const char* kSeqUser    = "{seq}:im:seq:uid:";   // uid        -> 用户级 seq
```

- [ ] **Step 2: 新增 kSeqHashTag 常量**

在 line 181（kSeqUser 定义）之后插入：
```cpp
    inline constexpr const char* kSeqHashTag  = "{seq}";              // seq key 共享 hash tag 常量
```

- [ ] **Step 3: pipeline() 签名加 hash_tag 默认参数**

将 lines 157-158：
```cpp
    auto pipeline() {
        return _rc ? _rc->pipeline() : _r->pipeline();
    }
```

替换为：
```cpp
    auto pipeline(const sw::redis::StringView &hash_tag = {}) {
        return _rc ? _rc->pipeline(hash_tag) : _r->pipeline();
    }
```

- [ ] **Step 4: next_user_seq_batch 传入 hash_tag**

将 line 428：
```cpp
            auto pipe = _c->pipeline();
```

替换为：
```cpp
            auto pipe = _c->pipeline(key::kSeqHashTag);
```

- [ ] **Step 5: 编译验证**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make -j8 2>&1 | tail -20
```

预期：编译通过。`key::kSeqHashTag` 是 `const char*`，隐式转换为 `sw::redis::StringView`。

- [ ] **Step 6: Commit**

```bash
git add common/dao/data_redis.hpp
git commit -m "fix(redis): add {seq} hash tag to seq keys and pipeline routing"
```

---

### Task 3: scan() 集群模式防御性守卫

**Files:**
- Modify: `common/dao/data_redis.hpp:161-166`

集群模式下 `cursor != 0` 时加 `LOG_ERROR` + `abort()` 守卫，并将单节点 scan 替换为 `for_each` 全节点遍历。

- [ ] **Step 1: 替换 scan() 实现**

将 lines 161-166：
```cpp
    // --- SCAN ---
    template <typename Out>
    long long scan(long long cursor, const std::string &pattern, long long count, Out out) {
        return _rc ? _rc->scan(cursor, pattern, count, out)
                   : _r->scan(cursor, pattern, count, out);
    }
```

替换为：
```cpp
    // --- SCAN ---
    // 集群模式：for_each 一次遍历所有节点。不支持迭代续扫——cursor 非零时 abort。
    template <typename Out>
    long long scan(long long cursor, const std::string &pattern, long long count, Out out) {
        if (_rc) {
            if (cursor != 0) {
                LOG_ERROR("RedisCluster scan does not support iterative scan, cursor must be 0, got {}", cursor);
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

- [ ] **Step 2: 编译验证**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make -j8 2>&1 | tail -20
```

预期：编译通过。`LOG_ERROR` 来自 `<infra/logger.hpp>`（已在文件头通过其他 include 间接引入，`data_redis.hpp` 的 DAO 类大量使用 `LOG_ERROR`/`LOG_INFO` 等宏）。

- [ ] **Step 3: Commit**

```bash
git add common/dao/data_redis.hpp
git commit -m "fix(redis): add abort guard for iterative scan misuse in cluster mode"
```

---

### Task 4: 全量编译 + 功能回归检查

**Files:**
- 无代码变更

变更完成后的全量验证，确保无编译错误且现有 scan/eval/pipeline 调用方不受影响。

- [ ] **Step 1: 全量编译所有服务**

```bash
cd /Users/yanghaoyang/repo/ChatNow/build && make -j8 2>&1 | tail -30
```

预期：8 个服务全部编译通过。

- [ ] **Step 2: 确认 Git 状态**

```bash
git log --oneline -5
```

预期：3 个新 commit 在 `feat/redis-cluster-fix` 分支顶部。
