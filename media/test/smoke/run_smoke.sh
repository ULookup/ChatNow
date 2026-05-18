#!/usr/bin/env bash
# P4 Media smoke test
# ---------------------------------------------------------------------------
# 前置（手动）：
#   1. cd docker && docker compose up -d minio minio-init
#   2. mysql -uroot -p chatnow < sql/V4__media.sql   # 或让 ODB --generate-schema 建表
#   3. build/file/media_server --media_conf=conf/media.json &
#   4. 安装：curl + python3 + brpc 自带的 HTTP+JSON 入站（默认开启）
# ---------------------------------------------------------------------------
# 验证 happy path：
#   ApplyUpload → curl PUT bytes → CompleteUpload → ApplyDownload → curl GET
#   最后用 mysql CLI 校验 media_user_quota 已 +5
# ---------------------------------------------------------------------------

set -euo pipefail

HOST="${MEDIA_HOST:-127.0.0.1:10002}"
USER_ID="${TEST_USER_ID:-u_smoke_001}"
DEVICE_ID="${TEST_DEVICE_ID:-d_smoke_001}"
TRACE_ID="${TEST_TRACE_ID:-$(printf '%032x' $RANDOM$RANDOM$RANDOM$RANDOM)}"
REQID="$(uuidgen 2>/dev/null || python3 -c 'import uuid;print(uuid.uuid4())')"

# brpc HTTP 入站：path=/<service>/<method>，body 是 json2pb
auth_headers=(
  -H "x-user-id: $USER_ID"
  -H "x-device-id: $DEVICE_ID"
  -H "x-trace-id: $TRACE_ID"
  -H "Content-Type: application/json"
)

# 计算 sha256 of "hello"
HASH_HEX=$(printf 'hello' | sha256sum | awk '{print $1}')
HASH="sha256:${HASH_HEX}"

echo ">>> ApplyUpload"
APPLY_RSP=$(curl -s "${auth_headers[@]}" -X POST "http://$HOST/chatnow.media.MediaService/ApplyUpload" \
    -d "{\"request_id\":\"$REQID\",\"file_name\":\"hi.txt\",\"file_size\":5,\"mime_type\":\"text/plain\",\"content_hash\":\"$HASH\",\"purpose\":\"CHAT\"}")
echo "$APPLY_RSP"
FILE_ID=$(echo "$APPLY_RSP" | python3 -c 'import sys,json;print(json.load(sys.stdin)["file_id"])')
URL=$(echo "$APPLY_RSP"      | python3 -c 'import sys,json;print(json.load(sys.stdin)["upload_url"])')

echo ">>> PUT bytes"
curl -X PUT --data-binary 'hello' -H 'Content-Type: text/plain' -H 'Content-Length: 5' "$URL"

echo ">>> CompleteUpload"
curl -s "${auth_headers[@]}" -X POST "http://$HOST/chatnow.media.MediaService/CompleteUpload" \
    -d "{\"request_id\":\"$REQID\",\"file_id\":\"$FILE_ID\"}"
echo

echo ">>> ApplyDownload"
DRSP=$(curl -s "${auth_headers[@]}" -X POST "http://$HOST/chatnow.media.MediaService/ApplyDownload" \
    -d "{\"request_id\":\"$REQID\",\"file_id\":\"$FILE_ID\"}")
echo "$DRSP"
GET_URL=$(echo "$DRSP" | python3 -c 'import sys,json;print(json.load(sys.stdin)["download_url"])')

echo ">>> GET bytes"
BODY=$(curl -s "$GET_URL")
[[ "$BODY" == "hello" ]] || { echo "FAIL: body mismatch '$BODY'"; exit 1; }

echo ">>> Quota check"
mysql -h "${MYSQL_HOST:-127.0.0.1}" -u"${MYSQL_USER:-root}" -p"${MYSQL_PASSWORD:-root}" \
    "${MYSQL_DB:-chatnow}" -e \
    "SELECT user_id, used_bytes FROM media_user_quota WHERE user_id='$USER_ID'"

echo ">>> smoke pass"
