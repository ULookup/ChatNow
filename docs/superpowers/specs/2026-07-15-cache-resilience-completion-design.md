# ChatNow 缓存韧性补全设计

> **日期**: 2026-07-15
> **基线**: `origin/3.0-dev@1cad99a`
> **关联 Issue**: #35、#37、#38、#45、#48
> **测试架构**: PR #49 / #54 定义的纯 Go 五层测试体系

## 1. 目标与范围

本设计补全 ChatNow 3.0 缓存链路在 Redis 故障、高并发缓存回源和集中
过期场景下的韧性。目标规模沿用现有架构约束：峰值 5000 msg/s、单服务
2–8 个实例、最大 2000 人群。

验收目标：

- Redis 熔断打开后，调用在 10–50ms 内快速短路，不再逐请求等待 2 秒。
- Redis 故障期间限流仍生效，不允许无界 fail-open。
- 缓存型数据可跳过 Redis 回源 RPC/DB；Redis 真相源失败时明确返回不可用。
- UserInfo 热路径对 Identity RPC 的回源次数降低至少 95%。
- 同一冷 key 的高并发请求只触发一次进程内回源，不形成 thundering herd。
- 固定 TTL key 全面应用抖动，避免服务重启后的集中失效。
- RedisMutex 竞争采用带 jitter 的有界指数退避，降低热点轮询压力。

范围包含：

- `RedisClient` 进程内熔断与快速失败。
- `RateLimiter` 本地分片令牌桶降级。
- UserInfo L1/L2/RPC cache-aside 读写路径。
- Session、Status、Codes、DeviceSet、UnackedPush 等 TTL 抖动补全。
- RedisMutex 指数退避与 jitter。
- 与新 Go 测试框架一致的功能、可靠性和性能验证。
- 与鉴权模块共享 Redis 熔断状态的接口边界。

不包含：

- Redis Cluster 拓扑、代理、服务网格或外部熔断服务改造。
- 用 DB 或本地状态伪造 SeqGen、UnackedPush 等 Redis 真相源。
- 完整 JWT 吊销策略重构；#48 的鉴权安全策略单独处理。
- 跨进程同步熔断状态或本地限流状态。
- 与缓存问题无关的 DAO、RPC 和部署重构。

## 2. 设计原则

1. **故障域本地化**：每个进程独立熔断，Redis 故障不占满请求线程和连接池。
2. **按数据语义降级**：缓存可回源，限流可本地化，真相源必须失败。
3. **快速路径无锁**：熔断关闭时只做原子状态读取；本地限流按 key 分片。
4. **标准 cache-aside**：缓存 miss 回源，成功后回填；资料变更后失效。
5. **有界资源**：本地 bucket、缓存和等待时间均有硬上限。
6. **不增加常驻后台组件**：恢复探测、bucket 清理都由请求惰性驱动。
7. **低基数观测**：只记录状态和路径计数，不按 uid、ssid 或 key 打标签。

## 3. 总体架构

### 3.1 RedisClient 轻量熔断器

每个 `RedisClient` 持有一个共享 `RedisCircuitBreaker`。由同一客户端构造的
Session、Members、RateLimiter、JwtStore 等 DAO 观察同一个状态。

状态只有三种：

- `Closed`：允许请求。连续成功会清空失败计数。
- `Open`：直接抛出 `RedisCircuitOpen`，不借连接、不访问网络。
- `HalfOpen`：冷却时间到后，只允许一个请求探测，其余继续快速失败。

转换规则：

1. `Closed` 下连续 3 次连接、I/O、超时或连接池等待异常后进入 `Open`。
2. `Open` 固定保持 1 秒。
3. 1 秒后用 CAS 选出一个 `HalfOpen` 探测请求。
4. 探测成功立即回到 `Closed`；失败重新进入 `Open` 1 秒。
5. Redis 命令参数、序列化或业务脚本错误不计入熔断失败。

固定 1 秒冷却避免引入自适应窗口和复杂配置，同时每个进程最多每秒产生一个
恢复探测，对 Redis 故障节点负载可忽略。

Redis 运行时超时调整为：

- `connect_timeout = 50ms`
- `socket_timeout = 50ms`
- `pool.wait_timeout = 20ms`

Redis 部署在同机房；该上限远高于正常亚毫秒至数毫秒延迟，又能在故障时快速
触发熔断。启动时 Redis Cluster 仍依次尝试所有 seed，不改变现有拓扑发现逻辑。

### 3.2 Redis 命令接入

`RedisClient` 的公开命令统一通过两个内部模板执行：

- 有返回值的 `execute(command)`。
- 无返回值的 `execute_void(command)`。

模板负责：

1. 在借连接前调用 `breaker.before_call()`。
2. 执行 standalone 或 cluster 命令。
3. 成功时调用 `breaker.on_success()`。
4. 仅对连接类异常调用 `breaker.on_failure()`，然后原样抛出。

调用方现有的 try/catch 继续负责业务降级。熔断器不返回虚假默认值，防止把
“Redis 不可用”误解释成“key 不存在”。

Pipeline 由 RAII 包装器在 `exec()` 时报告成功或失败；创建 pipeline 本身不视为
一次成功请求。SCAN 和 Lua 也经过相同入口。

### 3.3 按数据语义降级

| 数据类型 | Redis 不可用时行为 |
|---|---|
| Members/UserInfo/LastMessage 等缓存 | 跳过 L2，回源 RPC/DB |
| 缓存写入、失效 | 记录指标后忽略，由 TTL 和后续回填收敛 |
| RateLimiter | 使用进程内令牌桶 |
| 幂等 SETNX | 保留现有 DB 唯一约束兜底 |
| SeqGen、UnackedPush 等真相源 | 返回明确的服务不可用 |
| JWT 相关 DAO | 获得统一快速失败信号；具体 fail-open/fail-close 不在本次改变 |

## 4. 本地限流降级

### 4.1 数据结构

`LocalRateLimiter` 由 `RateLimiter` 持有，使用 64 个固定 shard。每个 shard 包含：

- 一个 mutex。
- `unordered_map<string, Bucket>`。
- bucket 数量计数。

`Bucket` 只保存 `tokens`、`last_refill` 和 `last_seen`。一次请求只哈希并锁定一个
shard，不存在全局热锁。

### 4.2 算法和边界

- 容量、窗口和补充速率与 Redis Lua 令牌桶一致。
- Redis 正常时只执行 Redis Lua，不双写本地 bucket。
- Redis 熔断或调用失败时执行本地 bucket。
- Redis 恢复后下一次请求自动回到 Redis，不迁移本地临时状态。
- 全进程最多 65,536 个活跃 bucket。
- bucket 超过两个窗口未访问时，在后续请求中惰性清理。
- 达到硬上限且无法清理时，不为陌生 key 创建 bucket，直接拒绝请求。

故障期间无法维持严格跨实例额度，最坏上限为“实例数 × 单实例额度”，但流量
始终有界。引入服务发现来动态切分额度会增加一致性和可用性耦合，因此不采用。

## 5. UserInfo 三级缓存

### 5.1 Key 与 TTL

- L2 key：`im:user:{bucket}:uid`，`bucket = fnv1a(uid) % 64`。花括号中的
  bucket 是 Redis Cluster hash tag，使批量读取可按 64 个虚拟分片分组，同时避免
  全部 UserInfo 聚集到一个 slot。
- L2 value：序列化的 UserInfo protobuf。
- L2 正值 TTL：1 小时，±20% 抖动。
- L1 正值 TTL：45 秒，±20% 抖动。
- 空值 sentinel TTL：5 秒，使用相同抖动函数。

### 5.2 单用户读路径

1. 查询 Transmite 现有 `LocalCache<string>`。
2. L1 命中时解析 protobuf 并返回。
3. L1 miss 后进入现有 `InflightRegistry` 的 uid 维度 singleflight。
4. 获得 leader 后 double-check L1。
5. 查询 `UserInfoCache` L2。
6. L2 命中则回填 L1；损坏值删除后按 miss 处理。
7. L2 miss 或熔断时调用 Identity `GetProfile`。
8. RPC 成功后写 L2 和 L1；确认用户不存在时写 5 秒 sentinel。
9. RPC/DB 失败不写 sentinel，避免把依赖故障缓存成“不存在”。

Transmite 当前热路径只读取发送者一个 uid，不额外引入没有消费者的批量业务
流程。

### 5.3 批量接口

`UserInfoCache` 提供 `batch_get` 和 `batch_set`，满足 Message、Conversation 后续
批量读取场景：

- 读取使用 MGET；写入逐 uid 读取 generation 后以同槽 Lua CAS 回填，避免公开
  `set`/`batch_set` 绕过失效 fence。这里不额外引入复杂的批量 Lua 协议。
- Redis Cluster 按 64 个虚拟 bucket 分组；每组 key 共享 hash tag，可安全使用
  MGET/pipeline，避免 CROSSSLOT，也不依赖 redis-plus-plus 的内部连接池。
- 返回命中 map 和 miss uid 列表，调用方可用现有批量 RPC 一次回源。
- 单次批量大小限制为 2000，与最大群规模一致。

本次只把单用户 Transmite 路径接入该 DAO；批量消费者迁移不在范围内。

### 5.4 失效

Identity 在 DB 提交前 best-effort 推进 generation 并删除 L2，DB 成功后再次执行
同一原子失效。夹在 pre-invalidate 与 commit 之间读取旧 DB 的回填会被 post
generation 删除或拒绝；任一 Redis 失效都只记录指标，不拒绝或回滚已经请求的资料
更新。若 Redis 在整个更新期间都不可用，只能依赖 L1 最多约 54 秒、L2 一小时 TTL、
告警和恢复后的后续失效收敛，这是 cache-aside 可用性优先的明确边界，不引入 DB
outbox。Transmite 无法读取 generation 时仍把成功 RPC 结果放入短 TTL L1，供本机
singleflight followers 共享，但不写 L2；not-found 仍必须拿到 generation 才写 sentinel。

## 6. TTL 抖动补全

统一使用现有 `randomized_ttl(base)`，范围为基准值 ±20%，且永远返回正值。

需要补全的路径：

- Session：append、touch。
- Status：append、touch。
- Codes：append。
- DeviceSet：add 时设置与 Session 相同的 7 天 TTL；设备活动路径刷新 TTL。
- UnackedPush：push、bump_score 涉及的 ZSET/HASH 使用同一个随机 TTL 样本，避免
  两个索引因不同抖动提前分离。
- 其他仍使用固定常量的缓存写入和续期点。

DeviceSet 不使用短在线 TTL。它的生命周期与登录设备接近；7 天 TTL 防止永久
常驻，同时避免正常在线设备被几分钟级 TTL 误删。

滚动升级期间，带 hash tag 的 `im:dev:{uid}` 是新权威 key。读取会额外读取旧版
`im:dev:uid`，合并成员并懒迁移到新 key；删除同时清理两边。旧 key 不再接收新写，
每次被观察时只续一个最长 24 小时的迁移 grace TTL，因此不会形成无限双写或永久
兼容负担。跨 slot 的迁移分步执行是有意选择：迁移可重试，在线路由正确性仍由同
slot 的新 key 与 `im:online:{uid}` 原子脚本保证。

## 7. RedisMutex 退避

`try_lock` 保持现有 `SET key token NX PX ttl` 和 Lua CAS 解锁协议，仅调整竞争等待：

- 首次失败等待基准 5ms。
- 后续基准依次为 10ms、20ms，之后保持 20ms 上限。
- 每次加入 `[0, base/2]` 的 thread-local 随机 jitter。
- sleep 不超过剩余 timeout。
- 每次 `try_lock` 调用重置退避状态。

100ms 默认超时下最多产生少量 Redis 轮询，且竞争者不再同频唤醒。

## 8. 错误处理与恢复

- 熔断快速拒绝使用独立异常类型，便于 RateLimiter 精确进入本地降级。
- DAO 不记录每次快速拒绝，只在熔断状态转换时写日志，避免 Redis 故障造成日志
  风暴。
- cache-aside 回源失败时返回原业务依赖错误，不把失败响应写入缓存。
- half-open 探测由真实业务命令完成，不另起 ping 线程。
- Redis 恢复不要求服务重启；第一个成功探测关闭熔断器。
- Redis Cluster 的节点级 failover 仍由 redis-plus-plus 处理；应用熔断只覆盖客户端
  已无法在时限内完成命令的场景。

## 9. 可观测性

在现有 bvar 指标中增加低基数计数器：

- `redis_circuit_open_total`
- `redis_circuit_rejected_total`
- `redis_circuit_recovered_total`
- `redis_call_failure_total`
- `rate_limit_local_fallback_total`
- `rate_limit_local_rejected_total`
- `user_info_l1_hit_total`
- `user_info_l2_hit_total`
- `user_info_rpc_total`
- `redis_mutex_retry_total`

状态转换日志包含 Closed/Open/HalfOpen 和异常分类，不包含 uid、ssid 或完整 Redis
key。现有 Prometheus Redis 告警继续使用；新增应用指标用于区分 Redis 故障和业务
错误。

## 10. 测试设计

测试遵循 PR #49/#54：纯 Go、真实全栈、黑盒行为验证，测试代码是权威，不新增
C++ gtest 或 Python 合约测试。

### 10.1 L2 功能测试

文件：`tests/func/cache_test.go`，build tag：`func`。

- `FN-CA-01`：首次 UserInfo 回源后存在 L2，重复请求不重复调用 Identity。
- `FN-CA-02`：并发请求同一 uid，只发生一次回源。
- `FN-CA-03`：资料修改后 L2 失效，下一次读取获得新值。
- `FN-CA-04`：Session、Status、Codes、DeviceSet、UnackedPush TTL 位于抖动范围，
  多个样本不过期于同一秒。
- `FN-CA-05`：Redis 正常时大量消息请求触发分布式限流。

Session 与 Status DAO 在当前 3.0-dev 没有生产调用点，不为测试增加无业务意义的
endpoint。它们的 TTL 源码契约由独立 contract test 覆盖；Codes、DeviceSet、
UnackedPush 等可达路径继续由新框架做黑盒验证。待业务重新接入前两者时，再把对应
断言提升为黑盒测试。

### 10.2 可靠性测试

文件：`tests/reliability/redis_failover_test.go`，build tag：`reliability`。

`RL-05` 使用 `tests/pkg/chaos` 停止和恢复 Redis：

1. 故障前创建唯一用户和业务数据并预热缓存。
2. 停止 Redis，触发各目标服务熔断。
3. 验证缓存型接口可回源，稳定故障期请求延迟不超过 50ms。
4. 验证突发请求出现本地限流拒绝。
5. 验证依赖 SeqGen 的路径明确返回不可用。
6. 恢复 Redis，验证 half-open 成功并自动恢复正常路径。
7. Push 专项通过真实 WS、Rabbit 和容器 pause 编排验证：Redis 故障时 Unacked 写入
   失败必须 NackRequeue 且不得先向 WS 投递；恢复后重投、持久化并按 at-least-once
   语义最终送达。

### 10.3 L4 性能测试

文件：`tests/perf/cache_test.go`，build tag：`perf`。

- `PF-09` 分别记录冷缓存、L2 命中和 L1 命中的吞吐与 p95。
- 目标负载为 5000 msg/s，报告分配量和每请求耗时。
- 同一 uid 的热路径 Identity RPC 降幅至少 95%。
- 200 个并发请求访问同一冷 key 时，只允许一次进程内 RPC 回源。
- nightly 基线吞吐下降超过 10% 时失败。
- 性能门禁仅在 `PF09_RUN_FULLSTACK=1` 的 Linux 完整服务环境启用；普通编译发现
  明确 Skip，不把缺少业务栈伪装成性能通过。cold、L2 各使用 20 个 fresh sender，
  每个 sender 在计时区间只请求一次；L2 在计时前向真实 Redis Cluster 原子写入从
  Identity 读取的 UserInfo。L1 使用另外 20 个 sender，通过真实发送路径预热后固定
  压测 10 秒；预热按声明的 Transmite 实例数连续发送，利用 Gateway 的 round-robin
  为每个实例填充 L1，保证三个计时区间不会因 key 复用或跨实例路由相互转化。
- 多 Transmite 实例压测通过 `PF09_TRANSMITE_VARS_URLS` 提供所有 bvar 地址并求和，
  并用 `PF09_EXPECTED_TRANSMITE_INSTANCES` 校验实例数。前后快照同时记录 pid、uptime、
  启动时刻估值和 L1/L2/RPC counter，拒绝实例重启、counter 回绕、遗漏和求和溢出；
  每阶段 counter 必须精确符合对应缓存路径。门禁固定 `-benchtime=1x`，内部 10 秒时长
  不可由环境变量修改；使用代码库内 5000 msg/s 基线，基线可向上调整但不可降低。

### 10.4 测试辅助设施

- `tests/pkg/verify/redis.go`：读取 key、TTL 和 bvar 指标，不包含业务断言。
- `tests/pkg/chaos/redis.go`：Redis stop/start/wait 封装。
- 所有用例使用唯一 ID 隔离，可在 PR #49 合并后直接接入 CleanupAll 和 CI。
- macOS 执行 C++ 构建、Go build/vet/gofmt；故障注入和性能测试在 Linux Docker/CI
  执行。

## 11. 文件边界

计划新增：

- `common/utils/redis_circuit_breaker.hpp`：熔断状态机。
- `common/utils/local_rate_limiter.hpp`：分片本地令牌桶。
- `tests/func/cache_test.go`。
- `tests/reliability/redis_failover_test.go`。
- `tests/perf/cache_test.go`。
- `tests/pkg/verify/redis.go`。
- `tests/pkg/chaos/redis.go`。

计划修改：

- `common/dao/data_redis.hpp`：RedisClient 接入、UserInfoCache、TTL、RateLimiter。
- `common/utils/redis_mutex.hpp`：指数退避和 jitter。
- `common/infra/metrics.hpp`：低基数指标。
- `common/utils/redis_keys.hpp`：UserInfo L2 key。
- `transmite/source/transmite_server.h`：UserInfo 三级缓存读路径。
- `identity/source/identity_server.h`：资料变更后失效 UserInfo L2。
- 必要的服务 builder/config 文件：构造并注入共享 DAO，不新增外部依赖。

每个新增组件只有一个职责；不将熔断、限流和业务缓存合并成大型基础设施类。

## 12. 发布与回滚

1. 先合入无行为变化的指标和组件。
2. 接入 RedisClient 熔断与 RateLimiter 降级。
3. 接入 UserInfo cache-aside 和资料失效。
4. 补全 TTL 与锁退避。
5. Linux CI 运行 func、reliability 和 perf；观察熔断误触发与 RPC 降幅。

回滚可以按组件提交逐步进行。熔断器和本地限流没有外部持久状态；关闭相关接入
即可恢复原行为。UserInfo L2 key 使用独立前缀，回滚后由 TTL 自动清理。

## 13. Issue 覆盖

| Issue | 解决点 |
|---|---|
| #35 | RedisClient 熔断、50ms 超时、缓存回源语义 |
| #37 | UserInfo L1/L2/RPC、singleflight、批量 DAO |
| #38 | 固定 TTL 全面抖动、DeviceSet 生命周期 |
| #45 | RedisMutex 指数退避与 jitter |
| #48 | 统一熔断接口、本地限流降级；JWT 策略明确排除 |
