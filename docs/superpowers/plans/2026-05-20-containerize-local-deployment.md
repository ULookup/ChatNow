# Containerize Local Deployment — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 ChatNow 9 个 C++ 微服务全部容器化，实现 `docker compose up -d` 一键启动。

**Architecture:** 拆分两套配置（local 用 127.0.0.1 / docker 用 compose 服务名），改造 entrypoint.sh 支持多 host:port 对检测，新建缺失的 presence Dockerfile，消除 docker-compose.yml 中的硬编码 IP 和密码。

**Tech Stack:** Bash shell, Docker Compose v3.8, Ubuntu 24.04 基础镜像

---

### Task 1: 拆分配置文件 — local 目录

**Files:**
- Move: `conf/gateway_server.conf` → `conf/local/gateway_server.conf`
- Move: `conf/identity_server.conf` → `conf/local/identity_server.conf`
- Move: `conf/media_server.conf` → `conf/local/media_server.conf`
- Move: `conf/presence_server.conf` → `conf/local/presence_server.conf`
- Move: `conf/message_server.conf` → `conf/local/message_server.conf`
- Move: `conf/conversation_server.conf` → `conf/local/conversation_server.conf`
- Move: `conf/relationship_server.conf` → `conf/local/relationship_server.conf`
- Move: `conf/push_server.conf` → `conf/local/push_server.conf`
- Move: `conf/transmite_server.conf` → `conf/local/transmite_server.conf`

- [ ] **Step 1: 创建目录并移动文件**

```bash
mkdir -p /home/icepop/ChatNow/conf/local
for f in gateway_server identity_server media_server presence_server message_server conversation_server relationship_server push_server transmite_server; do
    mv /home/icepop/ChatNow/conf/${f}.conf /home/icepop/ChatNow/conf/local/${f}.conf
done
```

- [ ] **Step 2: 验证 local 目录内容完整**

```bash
ls /home/icepop/ChatNow/conf/local/
```

Expected: 9 个 `.conf` 文件。

- [ ] **Step 3: 确认本地服务仍能启动（至少 gateway 能跑）**

```bash
# 先确认旧的进程还在跑（之前 ps 看到过），不做重启
ps aux | grep gateway_server | grep -v grep
```

- [ ] **Step 4: Commit**

```bash
git add /home/icepop/ChatNow/conf/local/
git add /home/icepop/ChatNow/conf/  # track deletions
git commit -m "refactor: split config into conf/local/ for bare-metal runs"
```

---

### Task 2: 拆分配置文件 — docker 目录

**Files:**
- Create: `conf/docker/gateway_server.conf`
- Create: `conf/docker/identity_server.conf`
- Create: `conf/docker/media_server.conf`
- Create: `conf/docker/presence_server.conf`
- Create: `conf/docker/message_server.conf`
- Create: `conf/docker/conversation_server.conf`
- Create: `conf/docker/relationship_server.conf`
- Create: `conf/docker/push_server.conf`
- Create: `conf/docker/transmite_server.conf`

- [ ] **Step 1: 创建 docker 配置目录**

```bash
mkdir -p /home/icepop/ChatNow/conf/docker
```

- [ ] **Step 2: 生成 gateway_server.conf（docker 版）**

容器内日志路径统一用 `/im/logs/`，host 改为 compose 服务名。

```bash
cat > /home/icepop/ChatNow/conf/docker/gateway_server.conf << 'EOF'
-run_mode=true
-log_file=/im/logs/gateway.log
-log_level=0
-http_listen_port=9000
-websocket_listen_port=0
-registry_host=http://etcd:2379
-base_service=/service
-identity_service=/service/identity_service
-media_service=/service/media_service
-presence_service=/service/presence_service
-transmite_service=/service/transmite_service
-message_service=/service/message_service
-relationship_service=/service/relationship_service
-conversation_service=/service/conversation_service
-push_service=/service/push_service
-redis_host=redis-node1
-redis_port=6379
-redis_db=0
-redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384
-redis_keep_alive=true
-auth_config=/im/conf/auth.json
EOF
```

- [ ] **Step 3: 生成 identity_server.conf（docker 版）**

```bash
cat > /home/icepop/ChatNow/conf/docker/identity_server.conf << 'EOF'
-run_mode=true
-log_file=/im/logs/identity.log
-log_level=0
-registry_host=http://etcd:2379
-base_service=/service
-instance_name=/identity_service/instance
-access_host=identity_server:10003
-listen_port=10003
-rpc_timeout=-1
-rpc_threads=1
-media_public_url_prefix=https://cdn.chatnow.com/public
-es_host=http://elasticsearch:9200/
-mysql_host=mysql
-mysql_user=root
-mysql_pswd=<synthetic-mysql-password>
-mysql_db=chatnow
-mysql_cset=utf8
-mysql_port=0
-mysql_pool_count=4
-redis_host=redis-node1
-redis_port=6379
-redis_db=0
-redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384
-redis_keep_alive=true
-mail_user=yhaoyang666@163.com
-mail_paswd=<synthetic-smtp-password>
-mail_host=smtps://smtp.163.com:465
-mail_from=yhaoyang666@163.com
-auth_config=/im/conf/auth.json
EOF
```

- [ ] **Step 4: 生成 media_server.conf（docker 版）**

```bash
cat > /home/icepop/ChatNow/conf/docker/media_server.conf << 'EOF'
-run_mode=true
-log_file=/im/logs/media.log
-log_level=0
-registry_host=http://etcd:2379
-base_service=/service
-instance_name=/media_service/instance
-access_host=media_server:10002
-storage_path=/im/data/
-listen_port=10002
-rpc_timeout=-1
-rpc_threads=1
-redis_host=redis-node1
-redis_port=6379
-redis_db=0
-redis_keep_alive=true
-redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384
EOF
```

- [ ] **Step 5: 生成 presence_server.conf（docker 版）**

```bash
cat > /home/icepop/ChatNow/conf/docker/presence_server.conf << 'EOF'
# Presence 服务配置
-run_mode=false
-log_file=/im/logs/presence.log
-log_level=0

# RPC 端口
-listen_port=9050

# 注册中心
-registry_host=http://etcd:2379
-base_service=/service
-push_service=/service/push_service

# Redis
-redis_host=redis-node1
-redis_port=6379
-redis_db=0
-redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384
-redis_keep_alive=true

# 状态扫描间隔（秒）
-change_scan_interval_sec=5
EOF
```

- [ ] **Step 6: 生成 message_server.conf（docker 版）**

```bash
cat > /home/icepop/ChatNow/conf/docker/message_server.conf << 'EOF'
-run_mode=true
-log_file=/im/logs/message.log
-log_level=0
-registry_host=http://etcd:2379
-base_service=/service
-instance_name=/message_service/instance
-access_host=message_server:10005
-listen_port=10005
-rpc_timeout=-1
-rpc_threads=1
-identity_service=/service/identity_service
-media_service=/service/media_service
-mysql_host=mysql
-mysql_user=root
-mysql_pswd=<synthetic-mysql-password>
-mysql_db=chatnow
-mysql_cset=utf8
-mysql_port=0
-mysql_pool_count=4
-mq_user=root
-mq_pswd=<synthetic-rabbitmq-password>
-mq_host=rabbitmq:5672
-mq_msg_exchange=chat_msg_exchange
-mq_msg_queue_db=msg_queue_db
-mq_msg_queue_es=msg_queue_es
-mq_db_binding_key=msg_db
-mq_es_binding_key=msg_es
-mq_push_exchange=chat_push_exchange
-mq_push_queue=msg_push_queue
-mq_push_binding_key=push
-mq_es_exchange=es_index_exchange
-mq_es_queue=msg_queue_es_index
-mq_es_binding_key=msg_queue_es_index
-es_host=http://elasticsearch:9200/
-redis_host=redis-node1
-redis_port=6379
-redis_db=0
-redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384
-redis_keep_alive=true
-redis_pool_size=8
EOF
```

- [ ] **Step 7: 生成 conversation_server.conf（docker 版）**

```bash
cat > /home/icepop/ChatNow/conf/docker/conversation_server.conf << 'EOF'
-run_mode=true
-log_file=/im/logs/conversation.log
-log_level=0
-registry_host=http://etcd:2379
-base_service=/service
-instance_name=/conversation_service/instance
-access_host=conversation_server:10007
-listen_port=10007
-rpc_timeout=-1
-rpc_threads=1
-identity_service=/service/identity_service
-media_service=/service/media_service
-message_service=/service/message_service
-es_host=http://elasticsearch:9200/
-redis_host=redis-node1
-redis_port=6379
-redis_db=0
-redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384
-redis_keep_alive=true
-redis_pool_size=4
-mysql_host=mysql
-mysql_user=root
-mysql_pswd=<synthetic-mysql-password>
-mysql_db=chatnow
-mysql_cset=utf8mb4
-mysql_port=0
-mysql_pool_count=4
-public_url_prefix=http://gateway_server:9000/chatnow-media-public
EOF
```

- [ ] **Step 8: 生成 relationship_server.conf（docker 版）**

```bash
cat > /home/icepop/ChatNow/conf/docker/relationship_server.conf << 'EOF'
-run_mode=true
-log_file=/im/logs/relationship.log
-log_level=0
-registry_host=http://etcd:2379
-base_service=/service
-instance_name=/relationship_service/instance
-access_host=relationship_server:10006
-listen_port=10006
-rpc_timeout=-1
-rpc_threads=1
-identity_service=/service/identity_service
-conversation_service=/service/conversation_service
-es_host=http://elasticsearch:9200/
-mysql_host=mysql
-mysql_user=root
-mysql_pswd=<synthetic-mysql-password>
-mysql_db=chatnow
-mysql_cset=utf8mb4
-mysql_port=0
-mysql_pool_count=4
EOF
```

- [ ] **Step 9: 生成 push_server.conf（docker 版）**

```bash
cat > /home/icepop/ChatNow/conf/docker/push_server.conf << 'EOF'
-run_mode=true
-log_file=/im/logs/push.log
-log_level=0
-registry_host=http://etcd:2379
-base_service=/service
-instance_name=/push_service/instance
-access_host=push_server:10008
-listen_port=10008
-ws_port=9001
-rpc_timeout=-1
-rpc_threads=4
-message_service=/service/message_service
-redis_host=redis-node1
-redis_port=6379
-redis_db=0
-redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384
-redis_keep_alive=true
-redis_pool_size=16
-mq_user=root
-mq_pswd=<synthetic-rabbitmq-password>
-mq_host=rabbitmq:5672
-mq_push_exchange=chat_push_exchange
-mq_push_queue=msg_push_queue
-mq_push_binding_key=push
-resend_batch=50
-resend_max_age_sec=5
-jwt_current_kid=v1
-jwt_key_v1=<synthetic-jwt-key-at-least-32-bytes>
EOF
```

- [ ] **Step 10: 生成 transmite_server.conf（docker 版）**

```bash
cat > /home/icepop/ChatNow/conf/docker/transmite_server.conf << 'EOF'
-run_mode=true
-log_file=/im/logs/transmite.log
-log_level=0
-registry_host=http://etcd:2379
-base_service=/service
-instance_name=/transmite_service/instance
-access_host=transmite_server:10004
-listen_port=10004
-rpc_timeout=-1
-rpc_threads=1
-identity_service=/service/identity_service
-conversation_service=/service/conversation_service
-message_service=/service/message_service
-redis_host=redis-node1
-redis_port=6379
-redis_db=0
-redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384
-redis_keep_alive=true
-redis_pool_size=8
-mysql_host=mysql
-mysql_user=root
-mysql_pswd=<synthetic-mysql-password>
-mysql_db=chatnow
-mysql_cset=utf8
-mysql_port=0
-mysql_pool_count=4
-mq_user=root
-mq_pswd=<synthetic-rabbitmq-password>
-mq_host=rabbitmq:5672
-mq_msg_exchange=chat_msg_exchange
-mq_msg_queue=
-mq_msg_binding_key=
EOF
```

- [ ] **Step 11: 验证 docker 目录内容完整**

```bash
ls /home/icepop/ChatNow/conf/docker/
```

Expected: 9 个 `.conf` 文件。

- [ ] **Step 12: Commit**

```bash
git add /home/icepop/ChatNow/conf/docker/
git commit -m "feat: add conf/docker/ with compose service names for containerized deployment"
```

---

### Task 3: 新建 presence Dockerfile

**Files:**
- Create: `presence/Dockerfile`

- [ ] **Step 1: 创建 presence/Dockerfile**

```bash
cat > /home/icepop/ChatNow/presence/Dockerfile << 'EOF'
# 声明基础镜像来源
FROM ubuntu:24.04
# 声明工作路径
WORKDIR /im
RUN mkdir -p /im/logs &&\
    mkdir -p /im/data &&\
    mkdir -p /im/conf &&\
    mkdir -p /im/bin
# 将可执行程序文件，拷贝进入镜像
COPY ./build/presence_server /im/bin
# 将可执行程序依赖，拷贝进入镜像
COPY ./depends/* /lib/x86_64-linux-gnu/
COPY ./nc /bin
# 设置容器的启动默认操作 --- 运行程序
CMD /im/bin/presence_server -flagfile=/im/conf/presence_server.conf
EOF
```

- [ ] **Step 2: 验证文件存在**

```bash
cat /home/icepop/ChatNow/presence/Dockerfile | head -3
```

- [ ] **Step 3: Commit**

```bash
git add /home/icepop/ChatNow/presence/Dockerfile
git commit -m "feat: add presence Dockerfile for containerized deployment"
```

---

### Task 4: 改造 entrypoint.sh

**Files:**
- Modify: `entrypoint.sh`

当前接口：`-h <single_IP> -p <ports>` → 改为：`-d host1:port1,host2:port2,...`

- [ ] **Step 1: 备份旧文件然后重写**

```bash
cp /home/icepop/ChatNow/entrypoint.sh /home/icepop/ChatNow/entrypoint.sh.bak
```

- [ ] **Step 2: 写入新 entrypoint.sh**

```bash
cat > /home/icepop/ChatNow/entrypoint.sh << 'ENTRYEOF'
#!/bin/bash
# 端口检测函数：等待指定 host:port 可达
wait_for() {
    local host=$1
    local port=$2
    while ! nc -z $host $port
    do
        echo "$host:$port 端口连接失败，休眠等待";
        sleep 1;
    done
    echo "$host:$port 检测成功";
}

# 解析参数
declare deps
declare command
while getopts "d:c:" arg
do
    case $arg in
        d)
            deps=$OPTARG;;
        c)
            command=$OPTARG;;
    esac
done

# 对每个 host:port 对进行端口检测
for dep in ${deps//,/ }
do
    host=${dep%:*}
    port=${dep#*:}
    wait_for $host $port
done

echo "端口检测完毕"

# 执行命令
eval $command
ENTRYEOF
```

- [ ] **Step 3: 设置可执行权限**

```bash
chmod +x /home/icepop/ChatNow/entrypoint.sh
```

- [ ] **Step 4: 用当前运行的服务验证脚本语法**

```bash
bash -n /home/icepop/ChatNow/entrypoint.sh
```

Expected: 无输出（语法正确）。

- [ ] **Step 5: Commit**

```bash
git add /home/icepop/ChatNow/entrypoint.sh
git commit -m "refactor: change entrypoint.sh from -h/-p to -d host:port pairs for Docker DNS"
```

---

### Task 5: 更新 docker-compose.yml

**Files:**
- Modify: `docker-compose.yml`

所有 entrypoint 从 `-h 10.0.4.10 -p ...` 改为 `-d <服务名>:<端口>,...`，配置文件挂载从 `./conf/xxx.conf` 改为 `./conf/docker/xxx.conf`，新增 `presence_server` 服务。

- [ ] **Step 1: 重写 docker-compose.yml（完整内容）**

```bash
cat > /home/icepop/ChatNow/docker-compose.yml << 'DCEOF'
version: "3.8"

services:
  etcd:
    image: quay.io/coreos/etcd:v3.4.30
    container_name: etcd-service
    environment:
      - ETCD_NAME=etcd-s1
      - ETCD_DATA_DIR=/var/lib/etcd
      - ETCD_LISTEN_CLIENT_URLS=http://0.0.0.0:2379
      - ETCD_ADVERTISE_CLIENT_URLS=http://0.0.0.0:2379
    volumes:
      - ./middle/data/etcd:/var/lib/etcd:rw
    ports:
      - 2379:2379
    restart: always

  mysql:
    image: mysql:8.0.44
    container_name: mysql-service
    environment:
      MYSQL_ROOT_PASSWORD: ${MYSQL_ROOT_PASSWORD}
    volumes:
      - ./sql:/docker-entrypoint-initdb.d/:rw
      - ./middle/data/mysql:/var/lib/mysql:rw
    ports:
      - 3306:3306
    restart: always

  redis-node1:
    image: redis:7.2.5
    container_name: redis-node1
    command: redis-server --port 6379 --cluster-enabled yes --cluster-config-file /data/nodes.conf --cluster-node-timeout 5000 --appendonly yes --appendfilename appendonly-1.aof
    volumes:
      - ./middle/data/redis/node1:/data:rw
    ports:
      - "6379:6379"
    restart: always

  redis-node2:
    image: redis:7.2.5
    container_name: redis-node2
    command: redis-server --port 6380 --cluster-enabled yes --cluster-config-file /data/nodes.conf --cluster-node-timeout 5000 --appendonly yes --appendfilename appendonly-2.aof
    volumes:
      - ./middle/data/redis/node2:/data:rw
    ports:
      - "6380:6380"
    restart: always

  redis-node3:
    image: redis:7.2.5
    container_name: redis-node3
    command: redis-server --port 6381 --cluster-enabled yes --cluster-config-file /data/nodes.conf --cluster-node-timeout 5000 --appendonly yes --appendfilename appendonly-3.aof
    volumes:
      - ./middle/data/redis/node3:/data:rw
    ports:
      - "6381:6381"
    restart: always

  redis-node4:
    image: redis:7.2.5
    container_name: redis-node4
    command: redis-server --port 6382 --cluster-enabled yes --cluster-config-file /data/nodes.conf --cluster-node-timeout 5000 --appendonly yes --appendfilename appendonly-4.aof
    volumes:
      - ./middle/data/redis/node4:/data:rw
    ports:
      - "6382:6382"
    restart: always

  redis-node5:
    image: redis:7.2.5
    container_name: redis-node5
    command: redis-server --port 6383 --cluster-enabled yes --cluster-config-file /data/nodes.conf --cluster-node-timeout 5000 --appendonly yes --appendfilename appendonly-5.aof
    volumes:
      - ./middle/data/redis/node5:/data:rw
    ports:
      - "6383:6383"
    restart: always

  redis-node6:
    image: redis:7.2.5
    container_name: redis-node6
    command: redis-server --port 6384 --cluster-enabled yes --cluster-config-file /data/nodes.conf --cluster-node-timeout 5000 --appendonly yes --appendfilename appendonly-6.aof
    volumes:
      - ./middle/data/redis/node6:/data:rw
    ports:
      - "6384:6384"
    restart: always

  redis-cluster-init:
    image: redis:7.2.5
    container_name: redis-cluster-init
    depends_on:
      - redis-node1
      - redis-node2
      - redis-node3
      - redis-node4
      - redis-node5
      - redis-node6
    entrypoint: |
      /bin/sh -c "
      echo 'Waiting for all Redis nodes...' &&
      sleep 10 &&
      echo 'Creating 3-master 3-slave cluster...' &&
      echo yes | redis-cli --cluster create \
        redis-node1:6379 redis-node2:6380 redis-node3:6381 \
        redis-node4:6382 redis-node5:6383 redis-node6:6384 \
        --cluster-replicas 1 &&
      echo 'Verifying cluster...' &&
      redis-cli --cluster check redis-node1:6379 &&
      echo 'Cluster ready.' &&
      tail -f /dev/null
      "
    restart: "no"

  elasticsearch:
    image: elasticsearch:7.17.21
    container_name: elasticsearch-service
    environment:
      - "discovery.type=single-node"
    volumes:
      - ./middle/data/elasticsearch:/var/lib/elasticsearch:rw
    ports:
      - 9200:9200
      - 9300:9300
    restart: always

  rabbitmq:
    image: rabbitmq:3.12.1
    container_name: rabbitmq-service
    environment:
      RABBITMQ_DEFAULT_USER: root
      RABBITMQ_DEFAULT_PASS: ${RABBITMQ_DEFAULT_PASS}
    volumes:
      - ./middle/data/rabbitmq:/var/lib/rabbitmq:rw
    ports:
      - 5672:5672
    restart: always

  gateway_server:
    build: ./gateway
    container_name: gateway_server-service
    volumes:
      - ./conf/docker/gateway_server.conf:/im/conf/gateway_server.conf
      - ./conf/auth.json:/im/conf/auth.json
      - ./middle/data/logs:/var/lib/logs:rw
      - ./middle/data/data:/var/lib/data:rw
      - ./entrypoint.sh:/im/bin/entrypoint.sh
    ports:
      - 9000:9000
    restart: always
    depends_on:
      - etcd
      - redis-cluster-init
    entrypoint:
      /im/bin/entrypoint.sh -d etcd:2379,redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384 -c "/im/bin/gateway_server -flagfile=/im/conf/gateway_server.conf -redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384"

  identity_server:
    build: ./identity
    container_name: identity_server-service
    volumes:
      - ./conf/docker/identity_server.conf:/im/conf/identity_server.conf
      - ./conf/auth.json:/im/conf/auth.json
      - ./middle/data/logs:/var/lib/logs:rw
      - ./middle/data/data:/var/lib/data:rw
      - ./entrypoint.sh:/im/bin/entrypoint.sh
    ports:
      - 10003:10003
    restart: always
    depends_on:
      - etcd
      - mysql
      - redis-cluster-init
      - elasticsearch
    entrypoint:
      /im/bin/entrypoint.sh -d etcd:2379,mysql:3306,redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384,elasticsearch:9200 -c "/im/bin/identity_server -flagfile=/im/conf/identity_server.conf -redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384"

  media_server:
    build: ./media
    container_name: media_server-service
    volumes:
      - ./conf/docker/media_server.conf:/im/conf/media_server.conf
      - ./conf/media.json:/im/conf/media.json
      - ./middle/data/logs:/var/lib/logs:rw
      - ./middle/data/data:/var/lib/data:rw
      - ./entrypoint.sh:/im/bin/entrypoint.sh
    ports:
      - 10002:10002
    restart: always
    depends_on:
      - etcd
      - redis-cluster-init
    entrypoint:
      /im/bin/entrypoint.sh -d etcd:2379,redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384 -c "/im/bin/media_server -flagfile=/im/conf/media_server.conf -redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384"

  presence_server:
    build: ./presence
    container_name: presence_server-service
    volumes:
      - ./conf/docker/presence_server.conf:/im/conf/presence_server.conf
      - ./middle/data/logs:/var/lib/logs:rw
      - ./middle/data/data:/var/lib/data:rw
      - ./entrypoint.sh:/im/bin/entrypoint.sh
    ports:
      - 9050:9050
    restart: always
    depends_on:
      - etcd
      - redis-cluster-init
    entrypoint:
      /im/bin/entrypoint.sh -d etcd:2379,redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384 -c "/im/bin/presence_server -flagfile=/im/conf/presence_server.conf -redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384"

  relationship_server:
    build: ./relationship
    container_name: relationship_server-service
    volumes:
      - ./conf/docker/relationship_server.conf:/im/conf/relationship_server.conf
      - ./middle/data/logs:/var/lib/logs:rw
      - ./middle/data/data:/var/lib/data:rw
      - ./entrypoint.sh:/im/bin/entrypoint.sh
    ports:
      - 10006:10006
    restart: always
    depends_on:
      - etcd
      - mysql
      - elasticsearch
    entrypoint:
      /im/bin/entrypoint.sh -d etcd:2379,mysql:3306,elasticsearch:9200 -c "/im/bin/relationship_server -flagfile=/im/conf/relationship_server.conf"

  conversation_server:
    build: ./conversation
    container_name: conversation_server-service
    volumes:
      - ./conf/docker/conversation_server.conf:/im/conf/conversation_server.conf
      - ./middle/data/logs:/var/lib/logs:rw
      - ./middle/data/data:/var/lib/data:rw
      - ./entrypoint.sh:/im/bin/entrypoint.sh
    ports:
      - 10007:10007
    restart: always
    depends_on:
      - etcd
      - mysql
      - redis-cluster-init
      - elasticsearch
    entrypoint:
      /im/bin/entrypoint.sh -d etcd:2379,mysql:3306,redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384,elasticsearch:9200 -c "/im/bin/conversation_server -flagfile=/im/conf/conversation_server.conf -redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384"

  push_server:
    build: ./push
    container_name: push_server-service
    volumes:
      - ./conf/docker/push_server.conf:/im/conf/push_server.conf
      - ./middle/data/logs:/var/lib/logs:rw
      - ./middle/data/data:/var/lib/data:rw
      - ./entrypoint.sh:/im/bin/entrypoint.sh
    ports:
      - 10008:10008
      - 9001:9001
    restart: always
    depends_on:
      - etcd
      - redis-cluster-init
      - rabbitmq
    entrypoint:
      /im/bin/entrypoint.sh -d etcd:2379,redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384,rabbitmq:5672 -c "/im/bin/push_server -flagfile=/im/conf/push_server.conf -redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384"

  message_server:
    build: ./message
    container_name: message_server-service
    volumes:
      - ./conf/docker/message_server.conf:/im/conf/message_server.conf
      - ./middle/data/logs:/var/lib/logs:rw
      - ./middle/data/data:/var/lib/data:rw
      - ./entrypoint.sh:/im/bin/entrypoint.sh
    ports:
      - 10005:10005
    restart: always
    depends_on:
      - etcd
      - mysql
      - elasticsearch
      - rabbitmq
      - redis-cluster-init
    entrypoint:
      /im/bin/entrypoint.sh -d etcd:2379,mysql:3306,redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384,elasticsearch:9200,rabbitmq:5672 -c "/im/bin/message_server -flagfile=/im/conf/message_server.conf -redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384"

  transmite_server:
    build: ./transmite
    container_name: transmite_server-service
    volumes:
      - ./conf/docker/transmite_server.conf:/im/conf/transmite_server.conf
      - ./middle/data/logs:/var/lib/logs:rw
      - ./middle/data/data:/var/lib/data:rw
      - ./entrypoint.sh:/im/bin/entrypoint.sh
    ports:
      - 10004:10004
    restart: always
    depends_on:
      - etcd
      - mysql
      - rabbitmq
      - redis-cluster-init
    entrypoint:
      /im/bin/entrypoint.sh -d etcd:2379,mysql:3306,redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384,rabbitmq:5672 -c "/im/bin/transmite_server -flagfile=/im/conf/transmite_server.conf -redis_seeds=redis-node1:6379,redis-node2:6380,redis-node3:6381,redis-node4:6382,redis-node5:6383,redis-node6:6384"
DCEOF
```

- [ ] **Step 2: 检查 YAML 结构完整性**

```bash
grep -c "container_name:" /home/icepop/ChatNow/docker-compose.yml
```

Expected: 17 (基础设施 11 + 服务 9 - redis-cluster-init 无 container_name = 16... 实际数一下)
实际应有 container_name 的行：
- etcd-service, mysql-service, redis-node1-6 (6), elasticsearch-service, rabbitmq-service = 10
- gateway_server-service, identity_server-service, media_server-service, presence_server-service, relationship_server-service, conversation_server-service, push_server-service, message_server-service, transmite_server-service = 9
Total = 19 (redis-cluster-init 没有 container_name)

```bash
grep -c "container_name:" /home/icepop/ChatNow/docker-compose.yml
```

Expected: 19

- [ ] **Step 3: Commit**

```bash
git add /home/icepop/ChatNow/docker-compose.yml
git commit -m "feat: update docker-compose for containerized deployment with service-name-based discovery"
```

---

### Task 6: 创建 .env 和 .gitignore

**Files:**
- Create: `.env`
- Create: `.gitignore`

- [ ] **Step 1: 创建 .env**

```bash
cat > /home/icepop/ChatNow/.env << 'EOF'
MYSQL_ROOT_PASSWORD=<synthetic-mysql-password>
RABBITMQ_DEFAULT_PASS=<synthetic-rabbitmq-password>
EOF
```

- [ ] **Step 2: 创建 .gitignore**

```bash
cat > /home/icepop/ChatNow/.gitignore << 'EOF'
.env
build/
logs/
middle/
third_party/
*.bak
EOF
```

- [ ] **Step 3: 确认 .env 不会被 git 跟踪**

```bash
git -C /home/icepop/ChatNow status .env
```

Expected: `.env` 不出现（被 gitignore 忽略）。

- [ ] **Step 4: Commit**

```bash
git add /home/icepop/ChatNow/.gitignore
git commit -m "feat: add .gitignore and .env for secrets management"
```

---

### Task 7: 构建并验证

**Files:** None (验证步骤)

- [ ] **Step 1: 先停掉本地运行的服务进程**

```bash
# 停掉本地服务进程（它们占用了端口）
kill $(ps aux | grep -E "gateway_server|identity_server|message_server|conversation_server|presence_server|push_server|transmite_server|relationship_server|media_server" | grep -v grep | awk '{print $2}')
sleep 2
# 确认已停止
ps aux | grep -E "_server" | grep -v grep | grep ChatNow
```

Expected: 无输出（服务已停止）。

- [ ] **Step 2: 构建镜像**

```bash
cd /home/icepop/ChatNow && docker compose build
```

Expected: 9 个服务镜像构建成功。

- [ ] **Step 3: 启动所有容器**

```bash
cd /home/icepop/ChatNow && docker compose up -d
```

- [ ] **Step 4: 等待服务启动并检查容器状态**

```bash
sleep 15 && docker compose ps
```

Expected: 所有服务状态为 "Up"。

- [ ] **Step 5: 检查 gateway 日志有无错误**

```bash
docker compose logs gateway_server | grep -iE "error|fatal"
```

Expected: 无错误输出（或者只有 harmless 的 startup warnings）。

- [ ] **Step 6: 检查所有服务日志中的错误**

```bash
docker compose logs 2>&1 | grep -iE "error|fatal"
```

Expected: 无致命错误。

- [ ] **Step 7: 验证 gateway 可访问**

```bash
curl -s -o /dev/null -w "%{http_code}" http://localhost:9000/
```

Expected: 任意非-1 HTTP 状态码（404/405 都说明服务在响应）。

- [ ] **Step 8: 验证 etcd 中有服务注册**

```bash
docker compose exec etcd etcdctl get --prefix /service/ | head -30
```

Expected: 看到 9 个服务的注册信息（带有 access_host）。

- [ ] **Step 9: 运行集成测试**

```bash
cd /home/icepop/ChatNow/tests && go test -tags func ./func/ -v -count=1 -timeout 120s 2>&1 | tail -50
```

- [ ] **Step 10: Commit（如有修复）**

```bash
git add -A
git commit -m "fix: adjustments from containerized deployment verification"
```
