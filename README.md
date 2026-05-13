# ChatNow 即时通讯系统

一个基于 C++ 实现的分布式即时通讯（IM）系统，采用微服务架构，支持单聊 / 群聊、文件传输、语音消息（含语音转文字）、好友关系管理、离线消息推拉结合等功能。客户端基于 Qt6 实现，通过 HTTP + WebSocket 与服务端通信。

---

## 目录

- [一、整体架构](#一整体架构)
- [二、技术栈](#二技术栈)
- [三、目录结构](#三目录结构)
- [四、各服务实现说明](#四各服务实现说明)
- [五、数据存储设计](#五数据存储设计)
- [六、消息流转链路](#六消息流转链路)
- [七、客户端实现](#七客户端实现)
- [八、构建与部署](#八构建与部署)
- [九、配置说明](#九配置说明)

---

## 一、整体架构

ChatNow 整体采用 **「客户端 ↔ 网关 ↔ 后端微服务集群」** 三层结构，服务之间通过 **brpc + Protobuf** 进行 RPC 通信，通过 **etcd** 完成服务注册与发现，依赖 **MySQL / Redis / Elasticsearch / RabbitMQ** 作为持久化、缓存、检索与异步消息中间件。

```
                        ┌──────────────────────────┐
                        │       Qt6 客户端          │
                        │  (HTTP 业务请求 + WS 长连接) │
                        └────────────┬─────────────┘
                                     │
                                     ▼
                        ┌──────────────────────────┐
                        │  Gateway (网关 / 9000:HTTP, │
                        │            9001:WebSocket) │
                        │  · 鉴权 · 路由 · 长连接管理   │
                        └────────────┬─────────────┘
                                     │ brpc
        ┌────────────┬──────────────┼──────────────┬────────────┬────────────┐
        ▼            ▼              ▼              ▼            ▼            ▼
   User 服务      Friend 服务   ChatSession 服务  Transmite 服务   Message 服务   File / Speech
  (账号/资料)    (好友/申请)     (会话/成员)       (转发入口)      (消息存储/检索)  (文件/语音识别)
        │            │              │              │            │            │
        └─── MySQL ──┴──── ES ──────┴── Redis ─────┴── RabbitMQ ─┘            │
                                                                              │
                                                              本地磁盘 / 百度ASR
```

服务之间通过 **etcd** 实现：
- 各微服务启动后注册自身实例到 `/service/<服务名>/instance/<id>`；
- 网关 / 上游服务通过 `Discovery` 监听根路径，自动维护可用 brpc 信道（`ServiceManager`），实现 **动态服务发现 + 负载均衡（轮询）**。

---

## 二、技术栈

### 服务端

| 类别 | 选型 |
|---|---|
| 语言 / 标准 | C++17 |
| RPC 框架 | brpc + Protobuf 3 |
| HTTP 服务 | cpp-httplib（用于网关对外） |
| WebSocket | websocketpp（网关长连接） |
| 服务注册/发现 | etcd（etcd-cpp-api） |
| 关系数据库 | MySQL 8.0（ODB ORM） |
| 搜索引擎 | Elasticsearch 7（elasticlient） |
| 缓存 | Redis 7（redis++） |
| 消息队列 | RabbitMQ（AMQP-CPP + libev） |
| 日志 | spdlog |
| 邮件 | libcurl + SMTP |
| 语音识别 | 百度 AIP SDK |
| ID 生成 | Snowflake（雪花算法） |
| 容器化 | Docker + docker-compose |

### 客户端

| 类别 | 选型 |
|---|---|
| 框架 | Qt6（Widgets / Network / WebSockets / Protobuf / Multimedia） |
| 通信 | HTTP（QNetworkAccessManager）+ WebSocket（QWebSocket） |
| 序列化 | Qt Protobuf（`*.qpb.h`） |

---

## 三、目录结构

```
ChatNow/
└── code/
    ├── client/                # Qt6 客户端
    │   ├── proto/             # 客户端使用的 .proto 文件
    │   ├── model/             # 客户端数据模型与 DataCenter
    │   ├── network/           # NetClient（HTTP + WebSocket 封装）
    │   ├── *widget.{h,cpp}    # 各 UI 模块（登录、主界面、会话、消息、好友、设置...）
    │   └── CMakeLists.txt
    │
    └── server/
        ├── proto/             # 全部服务端 .proto 接口定义
        ├── common/            # 公共组件（etcd / channel / mysql / redis / es / mq / asr / mail / snowflake ...）
        ├── odb/               # ODB 生成的实体头文件（.hxx）
        ├── sql/               # 数据库建表 SQL（自动挂载到 MySQL 容器）
        ├── conf/              # 各服务运行配置（gflags flagfile）
        │
        ├── gateway/           # 网关服务（HTTP + WebSocket 入口）
        ├── user/              # 用户服务（注册/登录/资料）
        ├── friend/            # 好友服务（关系/申请/搜索）
        ├── chatsession/       # 会话服务（单聊/群聊/成员/会话状态）
        ├── transmite/         # 消息转发服务（消息预处理 + 投递 MQ）
        ├── message/           # 消息存储服务（DB/ES 落库 + 历史/未读/Timeline）
        ├── file/              # 文件服务（文件上传/下载/本地存储）
        ├── speech/            # 语音识别服务（百度 ASR）
        │
        ├── docker-compose.yml # 一键起：etcd/mysql/redis/es/mq + 7 个微服务
        ├── depends.sh         # 抽取每个服务可执行文件的 .so 依赖
        ├── entrypoint.sh      # 容器启动脚本（端口探测 + 启动）
        └── CMakeLists.txt
```

---

## 四、各服务实现说明

### 1. Gateway（网关服务）

源码：`code/server/gateway/`

- 同时启动 **HTTP 服务（默认 9000）** 与 **WebSocket 服务（默认 9001）**：
  - HTTP：客户端所有请求/响应类业务接口（注册、登录、好友/会话/消息查询、文件上传等），路径形如 `/service/user/...`、`/service/friend/...`、`/service/chatsession/...`、`/service/message_storage/...`、`/service/file/...`、`/service/speech/...`、`/service/message_transmit/new_message`。
  - WebSocket：客户端登录后建立长连接，用于服务端推送（新消息、好友申请、会话创建等 `NotifyMessage`）。
- **登录态管理**：使用 Redis 存储 `session_id → user_id`（`Session`）和 `user_id → 在线状态`（`Status`）。
- **鉴权与路由**：每个 HTTP handler 从请求 body 中反序列化 protobuf，根据 `session_id` 取到 `user_id` 后填充进上游 RPC 请求，再通过 `ServiceManager::choose()` 选择对应后端服务的 brpc Channel 转发。
- **长连接表**：`Connection`（`source/connection.hpp`）维护 `user_id ↔ websocket_hdl` 映射，断开时清理 Redis 登录态。
- **下行通知**：当后端服务（friend / chatsession / transmite）需要推送给在线用户时，由网关通过 WebSocket 发送 `NotifyMessage`。

入口：`gateway_server.cc`，使用 `GatewayServerBuilder` 构造（建造者模式）。

### 2. User 服务（用户服务，端口 10003）

源码：`code/server/user/`

提供账户与个人资料相关 RPC（见 `proto/user.proto`）：

- 用户名注册/登录、邮箱验证码注册/登录；
- 个人信息查询 / 批量用户信息查询（内部接口供其它服务调用）；
- 修改昵称 / 签名 / 头像（头像上传到 File 服务，仅保存 file_id）/ 绑定邮箱。

实现要点：
- 用户主数据写入 MySQL（`user` 表，ODB ORM）；
- 同步索引到 ES（`ESUser`），用于昵称/邮箱模糊搜索；
- 邮箱验证码通过 SMTP（163）发送，验证码保存至 Redis（`Codes`）；
- 登录成功后生成 `login_session_id` 写入 Redis（`Session`），并标记在线状态（`Status`）。

### 3. Friend 服务（好友服务，端口 10006）

源码：`code/server/friend/`

提供好友关系与申请相关 RPC（见 `proto/friend.proto`）：

- 好友列表 `GetFriendList`、删除好友、好友申请、申请处理（同意时自动创建单聊会话）、待处理申请列表、好友模糊搜索（走 ES `ESUser`）。
- 同意申请会写 `relation` 表 + 创建 `chat_session`、`chat_session_member` 记录；并通过网关向相关用户推送 `FRIEND_ADD_PROCESS_NOTIFY`、`CHAT_SESSION_CREATE_NOTIFY` 等通知。

### 4. ChatSession 服务（会话管理，端口暂用内部）

源码：`code/server/chatsession/`

实现 `proto/chatsession.proto` 中所有会话相关接口，是接口数最丰富的服务（约 18 个 RPC），覆盖：

- 会话生命周期：创建、获取列表、获取详情、修改名称/头像、修改状态（NORMAL / ARCHIVED / DISMISSED）、搜索；
- 成员管理：成员列表、添加 / 移除成员、转让群主、修改成员权限（NORMAL / ADMIN / OWNER）、退出会话；
- 用户在该会话中的个人状态：免打扰、置顶、显示/隐藏、未读 ACK；
- 内部接口：根据会话 ID 获取成员 ID 列表（供 transmite 服务做消息扩散使用）。

依赖：
- MySQL（`chat_session` / `chat_session_member` / 视图 `chat_session_view`）；
- ES（`ESChatSession`）做会话名称模糊搜索；
- 调用 User、File、Message 子服务做信息聚合（如带最近一条消息预览的会话列表）。

### 5. Transmite 服务（消息转发，端口 10004）

源码：`code/server/transmite/`

接口：`MsgTransmitService::GetTransmitTarget`（即客户端发送消息 `/service/message_transmit/new_message` 的后端实现）。

核心流程（`transmite_server.h`）：

1. 用 `brpc::DoNothing()` **并行** 调用：
   - User 服务：拿到发送者完整 `UserInfo`；
   - ChatSession 服务：拿到该会话所有成员 ID 列表。
2. 用 Snowflake 算法生成全局 `message_id`，组装 `InternalMessage`（包含 `MessageInfo` 与 `member_id_list`，"胖消息"），下游消费者无需再回查。
3. 通过 RabbitMQ Publisher（`publish_confirm` 异步带 ACK）将消息投递到交换机；
4. **MQ Broker ACK 后再调用 `done->Run()`** 把响应返回给网关——保证消息不丢；同时把组装好的完整 `MessageInfo` 与 `target_id_list` 一并返回，网关据此向所有在线接收者通过 WebSocket 推送 `CHAT_MESSAGE_NOTIFY`。

> 注：MQ 投递成功才视为发送成功；若失败则清空响应中的消息内容，由客户端重试。

### 6. Message 服务（消息存储与检索，端口 10005）

源码：`code/server/message/`

承担两大角色：

#### A. 异步消费 + 双写
订阅 RabbitMQ 上的两个队列（DB / ES），分别由 `onDBMessage`、`onESMessage` 回调处理：

- **DB Consumer**：
  - 反序列化 `InternalMessage`；
  - 文件/图片/语音消息：先调用 File 服务上传文件得到 `file_id`；
  - 在一个 ODB 事务里同时写 `message` 表 与 **`user_timeline` 表（写扩散）**：会话每个成员都插入一条 timeline 记录，方便后续按用户拉取。
  - 失败时返回 `NackRequeue`，由 MQ 重投，避免丢消息。
- **ES Consumer**：
  - 仅文本消息写入 ES，用于全文检索；其它类型直接 ACK。

#### B. RPC 查询接口（`MsgStorageService`）
- `GetHistoryMsg`：按时间段查 timeline，再按 ID 批量查 `message`，并批量回查 File / User 服务做内容/发送者补全；
- `GetRecentMsg`：取最近 N 条消息；
- `MsgSearch`：基于 ES 做关键字检索；
- `GetOfflineMsg`：基于 `last_message_id` 游标的增量拉取（替代轮询，配合上线同步）；
- `GetMsgByIds`：按 ID 批量查询消息（供其它服务做"最后一条消息预览"等场景）；
- `DeleteTimelineMsg`：用户删除自己的聊天记录（仅删 timeline，原始 `message` 保留）；
- `GetUnreadCount`：根据 `last_read_msg_id` 计算指定会话的未读数量。

### 7. File 服务（文件存储，端口 10002）

源码：`code/server/file/`

接口：`FileService`（单/多文件上传下载）。

- 启动时根据 `--storage_path` 创建本地存储目录；
- 上传时使用 `uuid()` 生成文件名作为 `file_id`，原内容直接写入磁盘文件；
- 下载根据 `file_id` 读取文件返回二进制；
- 不依赖 DB，纯本地磁盘 + brpc。

### 8. Speech 服务（语音识别，端口 10001）

源码：`code/server/speech/`

接口：`SpeechService::SpeechRecognition`。

- 封装百度 AIP SDK（`asr.hpp`）；
- 接收客户端上传的 PCM 16k 语音二进制，调用 `aip::Speech::recognize` 得到识别文本返回；
- 配置文件中通过 `app_id` / `api_key` / `secret_key` 注入鉴权信息。

---

## 五、数据存储设计

### MySQL（库名：`chatnow`）

建表 SQL 位于 `code/server/sql/`，docker-compose 启动时挂载到 MySQL 容器的 `/docker-entrypoint-initdb.d/` 自动执行。

| 表名 | 说明 |
|---|---|
| `user` | 用户基本信息：`user_id` / `nickname` / `description` / `password` / `mail` / `avatar_id`；nickname、mail、user_id 全部唯一索引 |
| `relation` | 好友关系（`user_id`, `peer_id`），按用户索引 |
| `friend_apply` | 好友申请事件（`event_id` 唯一，含状态、创建/处理时间） |
| `chat_session` | 会话主表：会话 ID / 名称 / 类型（单聊/群聊）/ 创建时间 / 最近一条消息 ID 与时间 / 成员数 / 状态 / 头像 |
| `chat_session_member` | 用户在会话中的状态：`session_id` + `user_id` 唯一；含 `last_read_msg`、`muted`、`visible`、`pin_time`、`role`、`join_time`，是会话成员个性化配置的核心表 |
| `message` | 消息原始内容：`message_id` 唯一（Snowflake）、消息类型、内容/file 元信息、状态、撤回时间 |
| `user_timeline` | **写扩散** Timeline：每条消息为每个会话成员各插一行（`user_id` + `session_id` + `message_id`），是离线/历史/增量拉取的查询入口 |

### Redis

- `Session`：登录会话 `session_id → user_id`；
- `Status`：在线状态 `user_id → bool`；
- `Codes`：邮箱验证码（带 TTL）。

### Elasticsearch

- `user` 索引：用户昵称/邮箱/签名，支持好友/用户搜索；
- `chat_session` 索引：会话名称模糊搜索；
- `message` 索引：仅文本消息，支持历史消息全文检索。

### RabbitMQ

- 交换机：`msg_exchange`（FANOUT）
- 队列：DB 队列 与 ES 队列各订阅一份
- 投递路径：`Transmite 服务（生产者）→ Exchange →（DB Queue / ES Queue）→ Message 服务（消费者，双写）`。

---

## 六、消息流转链路

以一次 **群消息发送** 为例，完整链路如下：

```
客户端
  │ 1. HTTP POST /service/message_transmit/new_message
  ▼
Gateway
  │ 2. 校验登录态 → 注入 user_id → brpc 调 transmite_service
  ▼
Transmite
  │ 3. 并行 RPC: User.GetUserInfo + ChatSession.GetMemberIdList
  │ 4. Snowflake 生成 message_id, 组装 InternalMessage（含成员列表）
  │ 5. publish_confirm 异步投递到 RabbitMQ
  │ 6. Broker ACK 后 done->Run() 返回成功 + 完整消息体 + 收件人列表
  ▼
Gateway
  │ 7. 遍历 target_id_list，对在线用户通过 WebSocket 推送 CHAT_MESSAGE_NOTIFY
  ▼ （并行）
Message (DB Consumer)         Message (ES Consumer)
  │ 写 message + user_timeline   │ 仅文本消息写 ES 索引
```

接收端：客户端在 WS 长连接上收到 `NotifyNewMessage`，触发 UI 增量刷新。下次上线如有遗漏，可走 `GetOfflineMsg` 按 `last_message_id` 增量补齐。

---

## 七、客户端实现

源码：`code/client/`，Qt6 项目。

- **入口**：`main.cpp` → `LoginWidget`，登录成功后切换到 `MainWidget`。
- **UI 模块**：
  - `loginwidget` / `mailloginwidget` / `verifycodewidget`：用户名 + 密码、邮箱验证码两套登录方式；
  - `mainwidget`：主界面骨架，左侧会话/好友列表、中部消息区、右侧详情；
  - `sessionfriendarea` / `messageshowarea` / `messageeditarea`：会话列表、消息渲染、输入区（含图片/文件/语音录制）；
  - `selfinfowidget` / `userinfowidget` / `sessiondetailwidget` / `groupsessiondetailwidget`：资料/会话详情；
  - `addfrienddialog` / `choosefrienddialog`：好友查找/邀请；
  - `historymessagewidget`：历史消息检索/查看；
  - `soundrecorder`：语音消息录制（PCM 16k）；
  - `toast`：轻量消息提示。
- **模型层** `model/`：
  - `data.h`：用户、好友、会话、消息等领域模型，提供 protobuf ↔ Qt 模型互转；
  - `datacenter.{h,cpp}`：进程级单例，集中管理本地状态、缓存、当前登录会话，并对外提供"业务方法"（内部调用 `NetClient`）。
- **网络层** `network/NetClient.{h,cpp}`：
  - 封装 HTTP（`QNetworkAccessManager`）与 WebSocket（`QWebSocket`）；
  - 提供模板化的 `handleHttpResponse<T>()`，统一反序列化 + 业务错误判定；
  - 收到 WebSocket `NotifyMessage` 后按类型分派到对应处理器（新消息、好友申请、申请处理、会话创建等），并通知 `DataCenter` 更新 UI。
- **proto** 与服务端 `code/server/proto` 内容一致，仅在 CMake 中通过 `qt_add_protobuf` 生成 `*.qpb.h` 给 Qt 使用。

服务器地址默认指向 `http://127.0.0.1:8000` / `ws://127.0.0.1:8001`，部署时需根据网关实际地址修改 `network/NetClient.h` 中常量。

---

## 八、构建与部署

### 1. 服务端（推荐 docker-compose）

`code/server/docker-compose.yml` 一键拉起 **基础设施 + 全部 7 个微服务**：

```bash
cd code/server
# 1) 先在宿主机本地（或 CI 中）按服务依次构建可执行文件，输出到各自 build/ 目录
#    每个服务独立 CMake：cd <svc> && mkdir build && cd build && cmake .. && make
# 2) 抽取动态库依赖到各服务 depends/，复制 nc 工具
bash depends.sh
# 3) 启动整套环境
docker-compose up -d
```

启动后开放端口：

| 服务 | 端口 |
|---|---|
| etcd | 2379 |
| MySQL | 3306 |
| Redis | 6379 |
| Elasticsearch | 9200 / 9300 |
| RabbitMQ | 5672 |
| Gateway | 9000(HTTP) / 9001(WS) |
| Speech / File / User / Transmite / Message / Friend | 10001 ~ 10006 |

容器启动脚本 `entrypoint.sh` 会先用 `nc` 探测依赖端口（etcd/MySQL/Redis/ES/MQ）就绪，再启动业务进程，避免冷启动竞态。

### 2. 客户端

```bash
cd code/client
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=<Qt6 安装路径>
cmake --build . -j
./ChatClient
```

依赖：Qt 6（Widgets / Network / WebSockets / Protobuf / Multimedia）。

---

## 九、配置说明

每个服务配置文件位于 `code/server/conf/<svc>_server.conf`，通过 gflags 的 `-flagfile` 形式加载。常见字段：

- `run_mode` / `log_file` / `log_level`：日志输出模式与等级；
- `registry_host`：etcd 地址；
- `base_service` + `instance_name`：本实例在 etcd 中的注册路径；
- `access_host`：注册到 etcd 的对外可访问地址（容器化部署时填宿主机/内网 IP + 端口）；
- 各服务对其依赖（MySQL / Redis / ES / MQ / 邮件 / ASR）的连接参数。

> ⚠️ 仓库中的配置文件示例包含明文密码、邮箱授权码与 ASR `secret_key`，**生产环境部署前请替换为环境变量或安全的配置中心**。

---

## License

仅作学习与项目演示用途。
