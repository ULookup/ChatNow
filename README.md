<p align="center">
  <br>
  <b>即时通讯系统服务端</b>
  <br>
  <sub>用 C++17 编写的分布式 IM 后端 — 支持单聊、群聊、媒体、全文检索与多端推送</sub>
  <br><br>
  <img src="https://img.shields.io/badge/language-C%2B%2B17-blue" alt="Language">
  <img src="https://img.shields.io/badge/build-CMake-brightgreen" alt="Build">
  <img src="https://img.shields.io/badge/license-source%20available-lightgrey" alt="License">
  <br><br>
</p>

---

## 特性

- **单聊 & 群聊** — 写扩散 + 双游标增量同步，支持 2000 人大群自动切读扩散
- **多端登录 & 推送** — JWT HS256 鉴权 / multi-kid 密钥轮换 / WebSocket 长连接 / 跨实例 fanout
- **媒体对象存储** — MinIO presigned URL 直传 / 大文件分片 / content-hash 去重 / 配额管理
- **全文检索** — Elasticsearch 实时索引，仅文本消息入 ES
- **离线消息补齐** — `GetOfflineMsg(last_message_id)` 增量拉取，好友申请 / 群通知不漏
- **端到端链路追踪** — `x-trace-id` 贯穿 HTTP → brpc → MQ → log，单次请求可追溯
- **高可用基础设施** — Redis Cluster (3M3S) / etcd 服务发现 / LeaderElection 统一选举 / L1 多级缓存防击穿
- **结构化日志** — JSON 行输出，spdlog + bthread-local context，按 trace_id 检索

## 快速开始

### 前置条件

- Ubuntu 22.04（推荐）
- Docker & Docker Compose
- CMake ≥ 3.13
- C++17 工具链 (GCC 9+ 或 Clang 10+)

### 1. 安装系统依赖

```bash
# 基础库 (brpc, protobuf, ODB, redis++, etcd-cpp-api, spdlog, ...)
sudo apt install -y build-essential cmake libprotobuf-dev libbrpc-dev \
  libboost-all-dev libhiredis-dev libcurl4-openssl-dev libssl-dev \
  libspdlog-dev libfmt-dev libgtest-dev libev-dev libcpprest-dev \
  libleveldb-dev libjsoncpp-dev

# AWS SDK (MinIO S3 兼容)
sudo bash scripts/install_aws_sdk_linux.sh
```

### 2. Start the Compose runtime

The root Compose profile contains the infrastructure and all nine ChatNow processes. Prepare synthetic local secret inputs as described in [Runtime Secret Management](docs/operations/runtime-secrets.md); never use production values in this profile.

```bash
docker compose config
docker compose up -d --build
./scripts/wait_for_services.sh
```

Compose expects the nine service binaries and their dependency directories to be prepared first, as described in [Compose Runtime Operations](docs/operations/compose-runtime.md). CI builds and restores those artifacts into each disposable test job. Consult PR #89 for current gate results; this local profile is not a production deployment.

### 3. Optional native build

The CI builder compiles the services before Compose packages their runtime images. For host-side development, build the native targets first:

```bash
mkdir build && cd build
cmake ..
cmake --build . -j$(nproc)
```

### 4. Optional native service startup

```bash
# 例: 以 flagfile 启动某个服务
./build/identity/identity_server -flagfile=conf/identity_server.conf
./build/conversation/conversation_server -flagfile=conf/conversation_server.conf
# ... 共 9 个服务，全部配置文件见 conf/
```

### 5. 跑测试

```bash
# C++ 单元测试
./build/common/test/common_tests

# Go 功能测试 (需要 Go 1.21+)
cd tests && go test -tags=func -v ./func/
```

## 架构

```
Client (HTTP/WS)
   │
   │  HTTP 9000              WS 9001
   ▼                         ▲
Gateway ──brpc──▶ 8 个业务服务 ──▶ Push
   │                    │           │
   │                    ▼           │
   │              RabbitMQ ─────────┘
   │                    │
   └── etcd ────────────┴── MySQL ── Redis Cluster(6n) ── ES ── MinIO
```

| 服务 | 端口 | 职责 |
|---|---|---|
| Gateway | 9000 | HTTP 入口 / JWT 鉴权 / 路由分发 |
| Identity | 10003 | 注册 / 登录 / JWT 签发 / 用户资料 / 搜索 |
| Relationship | 10006 | 好友申请 / 关系管理 / 黑名单 |
| Conversation | 10007 | 会话生命周期 / 成员管理 / 未读 / 置顶 |
| Transmite | 10004 | 消息转发入口 / 幂等去重 / MQ 投递 |
| Message | 10005 | 消息落库 / ES 索引 / 历史查询 / 离线补齐 |
| Media | 10002 | MinIO 对象存储 / 分片上传 / 去重 / 配额 |
| Presence | — | 在线状态维护 |
| Push | 9001, 10008 | WebSocket 长连接 / 跨实例推送 / ACK 收敛 |

### 消息链路

```
发送端 → Gateway → Transmite → RabbitMQ → Message(落库+写扩散)
                                            ├→ ES 索引 (异步)
                                            └→ Push → 接收端 WS 下发
```

- **幂等**: `client_msg_id` 唯一索引
- **可靠**: `publish_confirm` 异步 ACK / Outbox 兜底 / DLX 死信
- **有序**: 会话级 `seq_id` + 用户级 `user_seq` 双游标
- **可观测**: `x-trace-id` 全链路 + JSON 结构化日志

## 技术栈

| 类别 | 选型 |
|---|---|
| 语言 | C++17 |
| RPC | brpc + Protobuf 3 |
| 数据库 | MySQL 8.0 (ODB ORM) |
| 缓存 | Redis 7 Cluster (3M3S) / L1 LocalCache |
| 全文检索 | Elasticsearch 7 |
| 消息队列 | RabbitMQ (AMQP-CPP + libev) |
| 对象存储 | MinIO (aws-sdk-cpp, S3 兼容) |
| 服务发现 | etcd |
| 鉴权 | jwt-cpp (HS256, multi-kid) |
| ID 生成 | Snowflake (etcd LeaderElection worker_id) |
| 日志 | spdlog (JSON 行输出) |

## 项目结构

```
ChatNow/
├── proto/               # Protobuf 契约 (按域分目录)
├── common/               # 公共组件 (header-only 优先)
│   ├── auth/             #   JWT 编解码 / 鉴权上下文
│   ├── error/            #   错误码 / HANDLE_RPC 宏
│   ├── infra/            #   日志 / etcd / S3 / Snowflake / LeaderElection
│   ├── dao/              #   MySQL / Redis / ES 数据访问
│   ├── mq/               #   RabbitMQ 信道 / 发布 / 消费
│   ├── utils/            #   LocalCache / InflightRegistry / RedisMutex / trace_id / ...
│   └── test/             #   公共组件单元测试
├── gateway/              # HTTP 入口 & JWT 鉴权
├── identity/             # 注册 / 登录 / 资料 / 用户搜索
├── relationship/         # 好友 / 申请 / 黑名单
├── conversation/         # 会话 & 成员管理
├── transmite/            # 消息转发 & MQ 投递
├── message/              # 消息存储 / ES 索引 / 查询
├── media/                # MinIO 对象存储
├── presence/             # 在线状态
├── push/                 # WebSocket & 推送
├── odb/                  # ODB schema 定义 (*.hxx)
├── conf/                 # 服务配置 & JWT/Media JSON
├── tests/                # Go 集成 & 性能测试
├── scripts/              # 安装脚本 / Prometheus 告警规则
├── docs/                 # 架构 / Spec / Plan / 运维手册
└── docker-compose.yml    # 中间件 & 服务编排
```

## 文档

| 文档 | 说明 |
|---|---|
| [架构现状与路线图](docs/ARCHITECTURE_v2.0_and_roadmap.md) | 整体架构、消息链路、演进方向 |
| [消息管线](docs/MESSAGE_PIPELINE.md) | 消息从发送到接收的完整链路 |
| [设计 Spec](docs/superpowers/specs/) | 20+ 份设计文档，覆盖缓存 / MQ / Proto / 可靠性 |
| [实施 Plan](docs/superpowers/plans/) | 15+ 份实施计划，按分支独立 |
| [运维 Runbook](docs/operations/) | JWT 轮换 / 日志规范 / 监控 / 烟雾测试 |
| [Compose runtime operations](docs/operations/compose-runtime.md) | Root topology, bootstrap, semantic readiness, persistence, and evidence status |
| [Client retry and error handling](docs/client-sdk/error-retry.md) | Client retry boundaries and error categories |

### 运维

```bash
# JWT 密钥轮换
# 详见 docs/operations/jwt-key-rotation.md

# Prometheus 告警 (Redis Cluster / LeaderElection / L1 Cache)
# 详见 scripts/prometheus/redis_alerts.yml
```

## 状态

本项目当前处于 **3.0 开发线**，已完成以下关键里程碑：

- [x] P1 错误处理 & 鉴权元数据 & 结构化日志
- [x] P2 JWT 多端鉴权 (HS256, multi-kid)
- [x] P4 MinIO 对象存储
- [x] P8 trace_id 全链路追踪
- [x] Redis Cluster 化 (6 节点 3M3S)
- [x] L1 多级缓存体系 (LocalCache / InflightRegistry / RedisMutex)
- [x] etcd LeaderElection 统一选举
- [x] 全部 9 个服务迁移到新 proto 域命名空间
- [x] Go 功能 & 性能测试套件

进行中 / 规划中见 [架构路线图](docs/ARCHITECTURE_v2.0_and_roadmap.md)。

## 贡献

本项目目前为个人学习与演示用途，暂不接受外部贡献。如有问题或建议，欢迎提交 Issue。

## 安全

- [Runtime secret management](docs/operations/runtime-secrets.md)
- [JWT key rotation](docs/operations/jwt-key-rotation.md)

## License

Source available — 仅作学习与项目演示用途。
