# ChatNow 双模式部署方案设计

## 背景

当前部署仅支持 docker-compose 单实例，存在三个问题阻止扩展到两种部署模式：

1. **所有 IP 硬编码**：`conf/*.conf` 中 `10.0.4.10` 写死（MySQL / Redis / ES / MQ / etcd）
2. **access_host 写死**：多实例会注册相同的 `access_host`，无法水平扩展
3. **无镜像仓库**：镜像仅本地构建，K8s 无法拉取

**目标**：同一份代码、同一个镜像，通过环境变量切换两种部署模式：

- **模式一：本地单机**（docker compose --scale 水平扩展）
- **模式二：K8s 集群**

---

## 总体架构

```
  同一镜像: registry.example.com/chatnow/message:v3.0

  本地单机                              K8s 集群
  ┌──────────────────────┐          ┌──────────────────────┐
  │ docker-compose.yml    │          │ Deployment + Service │
  │ .env → 变量注入        │          │ ConfigMap/Secret → env│
  │ etcd 发现 + compose DNS│          │ etcd 发现 + k8s DNS  │
  │ access_host=$HOST_IP  │          │ access_host=$POD_IP  │
  └──────────────────────┘          └──────────────────────┘
```

两种模式共享：
- **brpc 服务框架**（不变）
- **etcd 服务发现与注册**（不变，access_host 动态生成）
- **HANDLE_RPC / LogContext / trace_id**（不变）

---

## 配置层改造

### 环境变量命名规范

前缀统一 `CN_`（ChatNow），避免与系统环境变量污染。

| 变量 | 说明 | 本地默认 | K8s 来源 |
|---|---|---|---|
| `CN_MYSQL_HOST` | MySQL 地址 | `mysql` | ConfigMap |
| `CN_MYSQL_PORT` | MySQL 端口 | `3306` | ConfigMap |
| `CN_MYSQL_USER` | 用户 | `root` | Secret |
| `CN_MYSQL_PSWD` | 密码 | `changeme` | Secret |
| `CN_MYSQL_DB` | 库名 | `chatnow` | ConfigMap |
| `CN_MYSQL_CSET` | 字符集 | `utf8mb4` | ConfigMap |
| `CN_MYSQL_POOL` | 连接池 | `16` | ConfigMap |
| `CN_REDIS_HOST` | Redis 地址 | `redis` | ConfigMap |
| `CN_REDIS_PORT` | Redis 端口 | `6379` | ConfigMap |
| `CN_REDIS_SEEDS` | 集群种子 | 空 | ConfigMap |
| `CN_ETCD_ENDPOINT` | etcd 端点 | `http://etcd:2379` | ConfigMap |
| `CN_ES_HOST` | ES 地址 | `http://es:9200` | ConfigMap |
| `CN_MQ_HOST` | MQ 地址 | `rabbitmq:5672` | ConfigMap |
| `CN_MQ_USER` | MQ 用户 | `root` | Secret |
| `CN_MQ_PSWD` | MQ 密码 | `changeme` | Secret |
| `CN_ACCESS_HOST` | 本实例地址 | 自动生成 | Downward API |
| `CN_LISTEN_PORT` | 监听端口 | 按服务固定 | ConfigMap |
| `CN_SERVER_NAME` | 服务名 | 按服务固定 | ConfigMap |
| `CN_SERVER_BIN` | 二进制名 | 按服务固定 | ConfigMap |
| `CN_SKIP_WAIT` | 跳过端口探测 | `false`(本地) | `true`(K8s) |
| `CN_WAIT_PORTS` | 等待端口列表 | 中间件列表 | 空 |

### conf 文件改造

所有 `*.conf` 中的硬编码值替换为 `${CN_XXX}` 引用。gflags 原生支持 `${ENV_VAR}` 语法。

```
# 改前
-mysql_host=10.0.4.10

# 改后
-mysql_host=${CN_MYSQL_HOST}
```

### access_host 动态生成

在 entrypoint.sh 中统一处理：

```bash
if [ -z "$CN_ACCESS_HOST" ]; then
    MY_IP=$(hostname -I | awk '{print $1}')
    export CN_ACCESS_HOST="${MY_IP}:${CN_LISTEN_PORT}"
fi
```

- 本地模式：`hostname -I` 拿到宿主机/容器 IP
- K8s 模式：Downward API 注入 `CN_ACCESS_HOST`（Pod IP 已含），脚本跳过

---

## 本地部署（Docker Compose）

### Dockerfile（统一模板）

```dockerfile
FROM ubuntu:24.04
WORKDIR /im
RUN mkdir -p /im/logs /im/data /im/conf /im/bin
COPY ./build/<name>_server /im/bin
COPY ./depends/* /lib/x86_64-linux-gnu/
COPY ./entrypoint.sh /im/bin/
ENTRYPOINT ["/im/bin/entrypoint.sh"]
```

CMD 不再硬编码，由 entrypoint 根据 `CN_SERVER_BIN` 组装启动命令。

### entrypoint.sh

```bash
#!/bin/bash
set -e

# K8s 模式跳过端口探测
if [ "${CN_SKIP_WAIT}" != "true" ] && [ -n "${CN_WAIT_PORTS}" ]; then
    for hp in ${CN_WAIT_PORTS//,/ }; do
        IFS=':' read -r h p <<< "$hp"
        while ! nc -z $h $p; do sleep 1; done
        echo "port $h:$p ready"
    done
fi

# 动态设置 access_host
if [ -z "$CN_ACCESS_HOST" ]; then
    MY_IP=$(hostname -I | awk '{print $1}')
    export CN_ACCESS_HOST="${MY_IP}:${CN_LISTEN_PORT}"
fi

echo "Starting ${CN_SERVER_NAME} on ${CN_ACCESS_HOST}"

exec /im/bin/${CN_SERVER_BIN} -flagfile=/im/conf/${CN_SERVER_NAME}_server.conf
```

### docker-compose.yml 结构

所有中间件 + 业务服务集中在 `deploy/local/docker-compose.yml`：

```yaml
services:
  # 中间件
  etcd:     { image: quay.io/coreos/etcd:v3.4.30, ports: ["2379:2379"] }
  mysql:    { image: mysql:8.0.44, ports: ["3306:3306"] }
  redis:    { image: redis:7.2.5, ports: ["6379:6379"] }
  es:       { image: elasticsearch:7.17.21, ports: ["9200:9200"] }
  rabbitmq: { image: rabbitmq:3.12.1, ports: ["5672:5672"] }
  minio:    { image: minio/minio, ports: ["9002:9000", "9003:9001"] }

  # 业务服务
  gateway:
    image: chatnow/gateway:latest
    ports: ["9000:9000"]
    env_file: .env
    environment:
      CN_SERVER_NAME: gateway
      CN_LISTEN_PORT: "9000"

  message:
    image: chatnow/message:latest
    deploy: { replicas: 2 }
    env_file: .env
    environment:
      CN_SERVER_NAME: message
      CN_LISTEN_PORT: "10005"
```

### .env 文件

```
CN_MYSQL_HOST=mysql
CN_MYSQL_PORT=3306
CN_MYSQL_USER=root
CN_MYSQL_PSWD=changeme
# ... 其余变量

CN_SKIP_WAIT=false
CN_WAIT_PORTS=mysql:3306,redis:6379,es:9200,rabbitmq:5672,etcd:2379
```

### 水平扩展

```bash
# 启动
docker compose up -d

# 水平扩展 message 服务到 4 个实例
docker compose up -d --scale message=4

# 本地模式端口段规划
# message: 10005-10008, push: 10008-10011, identity: 10003-10006, ...
```

---

## 集群部署（K8s）

### 单服务资源清单（以 message 为例）

**ConfigMap**：非敏感配置

```yaml
apiVersion: v1
kind: ConfigMap
metadata: { name: chatnow-message-config }
data:
  CN_SERVER_NAME: "message"
  CN_LISTEN_PORT: "10005"
  CN_MYSQL_HOST: "mysql.chatnow.svc.cluster.local"
  CN_MYSQL_PORT: "3306"
  CN_MYSQL_DB: "chatnow"
  CN_MYSQL_CSET: "utf8mb4"
  CN_REDIS_HOST: "redis.chatnow.svc.cluster.local"
  CN_ETCD_ENDPOINT: "http://etcd.chatnow.svc.cluster.local:2379"
  CN_ES_HOST: "http://elasticsearch.chatnow.svc.cluster.local:9200"
  CN_MQ_HOST: "rabbitmq.chatnow.svc.cluster.local:5672"
  CN_SKIP_WAIT: "true"
```

**Secret**：密码等敏感信息

```yaml
apiVersion: v1
kind: Secret
metadata: { name: chatnow-message-secret }
stringData:
  CN_MYSQL_USER: "root"
  CN_MYSQL_PSWD: "<from-vault>"
  CN_MQ_USER: "root"
  CN_MQ_PSWD: "<from-vault>"
```

**Deployment**：运行实例

```yaml
apiVersion: apps/v1
kind: Deployment
metadata: { name: chatnow-message, labels: { app: chatnow-message } }
spec:
  replicas: 2
  selector: { matchLabels: { app: chatnow-message } }
  template:
    metadata: { labels: { app: chatnow-message } }
    spec:
      containers:
      - name: message
        image: registry.example.com/chatnow/message:latest
        ports: [{ containerPort: 10005, name: brpc }]
        envFrom:
        - configMapRef:  { name: chatnow-message-config }
        - secretRef:     { name: chatnow-message-secret }
        env:
        - name: CN_ACCESS_HOST
          valueFrom:
            fieldRef: { fieldPath: status.podIP }
```

**Service**：Headless（etcd 直接注册 Pod IP）

```yaml
apiVersion: v1
kind: Service
metadata: { name: chatnow-message }
spec:
  selector: { app: chatnow-message }
  ports: [{ port: 10005, targetPort: brpc, name: brpc }]
  clusterIP: None
```

### 中间件部署

| 中间件 | K8s 内嵌 | 外挂云服务 |
|---|---|---|
| MySQL | StatefulSet + PVC | RDS |
| Redis | StatefulSet × 6（cluster） | 云 Redis |
| ES | StatefulSet × 1 | 云 ES |
| RabbitMQ | StatefulSet × 1 | 云 AMQP |
| etcd | StatefulSet × 3 | — |
| MinIO | StatefulSet × 4 + PVC | OSS / S3 |

ConfigMap 中的 host 指向 Service DNS（内嵌）或云服务 endpoint（外挂），切换仅需改一行。

### kustomize 环境分层

```
deploy/k8s/
├── base/                       # 所有环境通用
│   ├── namespace.yaml
│   ├── gateway/                # Deployment + Service + ConfigMap + Secret
│   ├── message/
│   ├── push/
│   ├── identity/
│   ├── transmite/
│   ├── media/
│   ├── relationship/
│   ├── conversation/
│   ├── middleware/              # etcd/mysql/redis/es/rabbitmq/minio
│   └── kustomization.yaml
└── overlays/
    ├── staging/                 # replicas: 1, resources: low
    └── prod/                    # replicas: 4, resources: high, 云服务 endpoint
```

---

## 目录结构

```
ChatNow/
├── deploy/
│   ├── local/
│   │   ├── docker-compose.yml
│   │   ├── .env
│   │   └── .env.example
│   └── k8s/
│       ├── base/
│       │   ├── namespace.yaml
│       │   ├── gateway/
│       │   │   ├── deployment.yaml
│       │   │   ├── service.yaml
│       │   │   ├── configmap.yaml
│       │   │   └── secret.yaml
│       │   ├── message/
│       │   ├── push/
│       │   ├── identity/
│       │   ├── transmite/
│       │   ├── media/
│       │   ├── relationship/
│       │   ├── conversation/
│       │   ├── middleware/
│       │   └── kustomization.yaml
│       └── overlays/
│           ├── staging/
│           └── prod/
├── entrypoint.sh                # 统一入口脚本
├── conf/                        # gflags flagfile（值改为 ${CN_XXX}）
└── ...（其余目录不变）
```

---

## 从现状的迁移路径

### Step 1：conf 文件全部改为环境变量引用

每个 `conf/*.conf` 中的硬编码值替换为 `${CN_XXX}`。一次做完，确认编译通过。

### Step 2：entrypoint.sh 改造

替换现有的仅做端口探测的 entrypoint.sh，加入 access_host 动态设置 + K8s 兼容逻辑。

### Step 3：Dockerfile 统一

8 个 Dockerfile 统一为 ENTRYPOINT 模式，CMD 移除。

### Step 4：docker-compose.yml 重写

加入 .env、deploy.replicas、环境变量注入，测试 `--scale` 水平扩展。

### Step 5：K8s manifests

先写 base，确认本地 minikube/kind 跑通，再加 overlays。

---

## License

仅作学习与项目演示用途。
