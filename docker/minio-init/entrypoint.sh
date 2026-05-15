#!/bin/sh
# minio-init/entrypoint.sh
# ---------------------------------------------------------------------------
# MinIO bucket 初始化（幂等）：
#   chatnow-media-public  ：公共可读（avatar/sticker），anonymous download
#   chatnow-media-private ：会话媒体，关闭匿名（默认）
# ---------------------------------------------------------------------------
# alias "local" 已通过 MC_HOST_local 环境变量预置（见 docker-compose.yml）。
# ---------------------------------------------------------------------------

set -e

# 1) 创建 bucket（幂等：-p 让父级路径存在；存在则不报错）
mc mb -p local/chatnow-media-public  || true
mc mb -p local/chatnow-media-private || true

# 2) 公共 bucket：任意 GET 都允许（download policy）
mc anonymous set download local/chatnow-media-public

# 3) 私密 bucket：显式声明无匿名（默认即如此，但显式声明便于自检 / 防误改）
mc anonymous set none local/chatnow-media-private

echo "[minio-init] buckets ready:"
mc ls local
