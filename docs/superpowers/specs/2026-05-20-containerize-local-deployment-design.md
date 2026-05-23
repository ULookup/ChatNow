# Containerize Local Deployment — Design Spec

**Date:** 2026-05-20
**Branch:** 3.0-dev
**Goal:** 将 ChatNow 9 个 C++ 微服务全部容器化，实现本地 `docker-compose up` 一键启动。

## Context

当前所有服务以本地二进制运行（`build/` 目录），基础设施（MySQL、Redis Cluster、etcd、ES、RabbitMQ）已容器化。目标是将服务也容器化，统一通过 docker-compose 编排。

核心挑战：服务发现基于 etcd — 每个服务向 etcd 注册自己的 `access_host:port`，其他服务通过 etcd 发现并创建 brpc Channel 直连。本地跑时所有 host 都是 `127.0.0.1`，容器内每个容器有自己的 localhost，必须改用 Docker 内置 DNS。

## Design

### 1. 配置拆分

```
conf/
├── local/                     # 本地直接跑 (host=127.0.0.1)
│   ├── gateway_server.conf
│   ├── identity_server.conf
│   ├── media_server.conf
│   ├── presence_server.conf
│   ├── message_server.conf
│   ├── conversation_server.conf
│   ├── relationship_server.conf
│   ├── push_server.conf
│   └── transmite_server.conf
├── docker/                    # 容器内 (host=compose 服务名)
│   └── (同上 9 个 conf)
├── auth.json                  # JWT 公钥，和环境无关
└── media.json                 # MinIO/COS 凭证，和环境无关
```

两套配置差异仅限于 host/IP：

| 依赖 | local | docker |
|------|-------|--------|
| etcd | 127.0.0.1:2379 | etcd:2379 |
| MySQL | 127.0.0.1 | mysql |
| Redis (单节点) | 127.0.0.1:6379 | redis-node1:6379 |
| Redis seeds | 127.0.0.1:6379,6380,... | redis-node1:6379,redis-node2:6380,... |
| ES | 127.0.0.1:9200 | elasticsearch:9200 |
| RabbitMQ | 127.0.0.1:5672 | rabbitmq:5672 |
| 服务的 access_host | 127.0.0.1:<port> | <service_name>:<port> |

### 2. 缺失的 presence Dockerfile

`presence/` 目录没有 Dockerfile，新建一个，和其他服务完全一致的结构（ubuntu:24.04 基础镜像，COPY 二进制和依赖）。

### 3. entrypoint.sh 改造

当前接口：`-h <single_IP> -p <ports_comma_separated>`

问题：容器内各依赖分布在不同的 hostname（etcd、mysql、redis-node1...），不再是一个统一 IP。

改为：`-d host1:port1,host2:port2,...`

```bash
wait_for() {
    local host=$1
    local port=$2
    while ! nc -z $host $port; do
        echo "$host:$port 端口连接失败，休眠等待";
        sleep 1;
    done
    echo "$host:$port 检测成功";
}

while getopts "d:c:" arg; do
    case $arg in
        d) deps=$OPTARG;;
        c) command=$OPTARG;;
    esac
done

for dep in ${deps//,/ }; do
    host=${dep%:*}
    port=${dep#*:}
    wait_for $host $port
done

eval $command
```

### 4. docker-compose.yml 更新

- 新增 `presence_server` 服务（目前缺失）
- 所有服务 entrypoint 从 `-h 10.0.4.10 -p ...` 改为 `-d <服务名>:<端口>,...`
- 配置文件挂载路径从 `./conf/xxx.conf` 改为 `./conf/docker/xxx.conf`
- 移除硬编码的 `10.0.4.10`

### 5. 消除硬编码密码

创建 `.env` 文件存储敏感信息：

```ini
MYSQL_ROOT_PASSWORD=YHY060403
RABBITMQ_DEFAULT_PASS=YHY060403
```

docker-compose.yml 引用变量：

```yaml
environment:
  MYSQL_ROOT_PASSWORD: ${MYSQL_ROOT_PASSWORD}
  RABBITMQ_DEFAULT_PASS: ${RABBITMQ_DEFAULT_PASS}
```

`.env` 加入 `.gitignore`。配置文件中的 `-mysql_pswd=YHY060403` 等暂时保留（配置文件本身不提交到公开仓库）。

### 6. 验证步骤

1. `docker compose build` — 逐个构建 9 个服务镜像
2. `docker compose up -d` — 启动所有容器
3. `docker compose logs gateway | grep ERROR` — 检查日志
4. `curl localhost:9000/` — 确认 gateway 可达
5. 跑集成测试：`cd tests && go test -tags func ./func/ -v`

## Scope

- 仅覆盖本地容器化部署，不涉及生产环境 TLS/健康检查/CI 等
- 不修改 C++ 源码，仅修改配置、脚本和 Docker 编排文件
- 不改变服务发现机制（仍基于 etcd + brpc Channel）
