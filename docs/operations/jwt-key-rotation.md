# JWT 密钥轮换 runbook

> 目标：按 spec §2.2 规则在线轮换 HS256 签发密钥，不影响线上活跃 token。

## 触发场景

- 定期轮换（建议 90 天一次）
- 怀疑密钥泄漏
- 配置文件历史泄漏审计

## 前置确认

- access_ttl_sec  默认 7200（2h）
- refresh_ttl_sec 默认 2592000（30d）
- `current_kid` 当前指向的 key id 是 v\<N\>

## 步骤

### 1. 生成新密钥（≥32 字节）

```bash
openssl rand -base64 48
```

### 2. 编辑 `conf/auth.json`，加 v\<N+1\>，**保留 v\<N\>，current_kid 仍 v\<N\>**

```json
{
  "auth": {
    "jwt": {
      "current_kid": "v1",
      "keys": {
        "v1": "OLD_KEY_AT_LEAST_32_BYTES",
        "v2": "NEW_KEY_FROM_OPENSSL_RAND"
      },
      "access_ttl_sec":  7200,
      "refresh_ttl_sec": 2592000
    }
  }
}
```

### 3. 滚动重启 user_server 和 gateway_server，让两者都加载新 keys map

### 4. 等待 30 天 + 1 天（>refresh TTL），让所有 v1 签的 token 失效

### 5. 编辑 `conf/auth.json`，把 current_kid 改为 v2

### 6. 滚动重启，新 token 用 v2 签

### 7. 等待 30 天 + 1 天

### 8. 编辑 `conf/auth.json`，删除 v1，滚动重启，归档老配置

## 故障排查

- 启动报 `auth.jwt.current_kid not in keys`：current_kid 写错或 keys 没改
- 启动报 `auth.jwt.keys[xx] must be >=32 bytes`：补长密钥
- 客户端大面积 401 (AUTH_TOKEN_INVALID=1003)：可能误删了仍有 token 的旧 kid，回滚 conf 即可
