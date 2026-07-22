#!/usr/bin/env bash

set -euo pipefail

READY_TIMEOUT_SEC=${CHATNOW_READY_TIMEOUT_SEC:-180}
POLL_INTERVAL_SEC=${CHATNOW_READY_POLL_INTERVAL_SEC:-2}
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_root"

deadline=$((SECONDS + READY_TIMEOUT_SEC))

compose() {
  docker compose "$@"
}

wait_until() {
  local label=$1
  local probe=$2
  while (( SECONDS < deadline )); do
    if "$probe"; then
      echo "readiness: ${label} ready"
      return 0
    fi
    sleep "$POLL_INTERVAL_SEC"
  done
  echo "readiness: timed out waiting for ${label}" >&2
  return 1
}

probe_redis_cluster() {
  local info
  info=$(compose exec -T redis-node1 redis-cli -p 6379 cluster info 2>/dev/null) || return 1
  grep -q '^cluster_state:ok' <<<"$info" &&
    grep -q '^cluster_slots_assigned:16384' <<<"$info" &&
    grep -q '^cluster_slots_ok:16384' <<<"$info" &&
    grep -q '^cluster_known_nodes:6' <<<"$info"
}

probe_mysql_schema() {
  local count users
  count=$(compose exec -T mysql sh -ec '
    MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql --user=root --batch --skip-column-names \
      --execute="SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = '\''chatnow'\'' AND table_name IN ('\''conversation'\'', '\''conversation_member'\'', '\''friend_apply'\'', '\''media_blob_ref'\'', '\''media_file'\'', '\''media_user_quota'\'', '\''message'\'', '\''message_attachment'\'', '\''message_mention'\'', '\''message_pin'\'', '\''message_reaction'\'', '\''message_read'\'', '\''relation'\'', '\''user'\'', '\''user_block'\'', '\''user_device'\'', '\''user_timeline'\'');" chatnow
  ' 2>/dev/null) || return 1
  [[ "$count" == "17" ]] || return 1

  users=$(compose exec -T mysql sh -ec '
    MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql --user=root --batch --skip-column-names \
      --execute="SELECT COUNT(*) FROM mysql.user WHERE user IN ('\''chatnow_identity'\'', '\''chatnow_conversation'\'', '\''chatnow_relationship'\'', '\''chatnow_message'\'', '\''chatnow_media'\'');"
  ' 2>/dev/null) || return 1
  [[ "$users" == "5" ]]
}

probe_rabbitmq() {
  compose exec -T rabbitmq rabbitmq-diagnostics -q check_running >/dev/null 2>&1 &&
    compose exec -T rabbitmq rabbitmq-diagnostics -q check_local_alarms >/dev/null 2>&1 || return 1

  local users user
  users=$(compose exec -T rabbitmq rabbitmqctl -q list_users 2>/dev/null) || return 1
  for user in chatnow_transmite chatnow_message chatnow_push; do
    grep -Eq "^${user}[[:space:]]" <<<"$users" || return 1
    compose exec -T rabbitmq rabbitmqctl -q list_user_permissions "$user" 2>/dev/null |
      grep -Eq '^/[[:space:]]' || return 1
  done
}

probe_elasticsearch() {
  local health
  health=$(curl --fail --silent --show-error --max-time 2 \
    'http://127.0.0.1:9200/_cluster/health?wait_for_status=yellow&timeout=1s') || return 1
  grep -Eq '"status":"(yellow|green)"' <<<"$health"
}

probe_minio() {
  curl --fail --silent --show-error --max-time 2 \
    'http://127.0.0.1:19000/minio/health/ready' >/dev/null || return 1
  compose run --rm --no-deps --entrypoint /bin/sh minio-init -ec '
    mc alias set ready http://minio:9000 "$MINIO_ROOT_USER" "$MINIO_ROOT_PASSWORD" >/dev/null
    mc stat ready/chatnow-media-public >/dev/null
    mc stat ready/chatnow-media-private >/dev/null
  ' >/dev/null 2>&1
}

probe_etcd_registrations() {
  local keys service count
  keys=$(compose exec -T etcd sh -ec \
    'ETCDCTL_API=3 etcdctl --endpoints=http://127.0.0.1:2379 get /service --prefix --keys-only' \
    2>/dev/null) || return 1
  for service in identity media transmite message relationship conversation presence push; do
    count=$(grep -c "^/service/${service}_service/instance$" <<<"$keys" || true)
    [[ "$count" == "1" ]] || return 1
  done
  count=$(sed -n 's#^/service/\([^/]*_service\)/instance$#\1#p' <<<"$keys" | sort -u | wc -l)
  [[ "${count//[[:space:]]/}" == "8" ]]
}

probe_gateway() {
  curl --fail --silent --show-error --max-time 2 \
    'http://127.0.0.1:9000/health' >/dev/null
}

probe_push() {
  nc -z -w 2 127.0.0.1 9001
}

wait_until "Redis Cluster" probe_redis_cluster
wait_until "MySQL schema and application users" probe_mysql_schema
wait_until "RabbitMQ" probe_rabbitmq
wait_until "Elasticsearch" probe_elasticsearch
wait_until "MinIO buckets" probe_minio
wait_until "eight exact etcd service registrations" probe_etcd_registrations
wait_until "Gateway HTTP health" probe_gateway
wait_until "Push WebSocket port 9001" probe_push

echo "readiness: full stack is ready"
