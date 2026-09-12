#!/bin/sh
# minio-init/entrypoint.sh
# ---------------------------------------------------------------------------
# MinIO bucket 初始化（幂等）：
#   chatnow-media-public  ：公共可读（avatar/sticker），anonymous download
#   chatnow-media-private ：会话媒体，关闭匿名（默认）
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------

set -eu

MAX_ATTEMPTS=${MINIO_INIT_MAX_ATTEMPTS:-60}
required="MINIO_ROOT_USER MINIO_ROOT_PASSWORD MINIO_APP_ACCESS_KEY MINIO_APP_SECRET_KEY"
for name in $required; do
  value=$(printenv "$name" 2>/dev/null || true)
  if [ -z "$value" ]; then
    echo "minio-init: required credential is missing: ${name}" >&2
    exit 1
  fi
done

MC_CONFIG_DIR=$(mktemp -d)
export MC_CONFIG_DIR
policy_file=""
cleanup() {
  if [ -n "$policy_file" ]; then
    rm -f "$policy_file"
  fi
  rm -rf "$MC_CONFIG_DIR"
}
trap cleanup EXIT

printf '%s\n%s\n' "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" |
  mc alias set local http://minio:9000 --api S3v4 --path on >/dev/null

attempt=1
while [ "$attempt" -le "$MAX_ATTEMPTS" ]; do
  if mc admin info local >/dev/null 2>&1; then
    break
  fi
  attempt=$((attempt + 1))
  sleep 1
done

if [ "$attempt" -gt "$MAX_ATTEMPTS" ]; then
  echo "minio-init: server did not become ready" >&2
  exit 1
fi

mc mb --ignore-existing local/chatnow-media-public
mc mb --ignore-existing local/chatnow-media-private
mc anonymous set download local/chatnow-media-public
mc anonymous set none local/chatnow-media-private

policy_file=$(mktemp)
cat >"$policy_file" <<'JSON'
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": ["s3:GetBucketLocation", "s3:ListBucketMultipartUploads"],
      "Resource": ["arn:aws:s3:::chatnow-media-public", "arn:aws:s3:::chatnow-media-private"]
    },
    {
      "Effect": "Allow",
      "Action": ["s3:GetObject", "s3:PutObject", "s3:DeleteObject", "s3:AbortMultipartUpload"],
      "Resource": ["arn:aws:s3:::chatnow-media-public/*", "arn:aws:s3:::chatnow-media-private/*"]
    }
  ]
}
JSON

# `user add` and `policy create` converge existing entries to the supplied state.
printf '%s\n%s\n' "$MINIO_APP_ACCESS_KEY" "$MINIO_APP_SECRET_KEY" |
  mc admin user add local
mc admin policy create local chatnow-media-app "$policy_file"
mc admin policy attach local chatnow-media-app --user "$MINIO_APP_ACCESS_KEY"

mc stat local/chatnow-media-public >/dev/null
mc stat local/chatnow-media-private >/dev/null
mc admin user info local "$MINIO_APP_ACCESS_KEY" >/dev/null
echo "minio-init: buckets and application identity are ready"
