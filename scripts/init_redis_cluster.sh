#!/bin/sh

set -eu

MAX_ATTEMPTS=${REDIS_INIT_MAX_ATTEMPTS:-60}
NODES="redis-node1:6379 redis-node2:6380 redis-node3:6381 redis-node4:6382 redis-node5:6383 redis-node6:6384"

attempt=1
while [ "$attempt" -le "$MAX_ATTEMPTS" ]; do
  all_ready=true
  for node in $NODES; do
    host=${node%:*}
    port=${node#*:}
    if ! redis-cli -h "$host" -p "$port" ping 2>/dev/null | grep -q '^PONG$'; then
      all_ready=false
      break
    fi
  done
  if [ "$all_ready" = true ]; then
    break
  fi
  attempt=$((attempt + 1))
  sleep 1
done

if [ "$attempt" -gt "$MAX_ATTEMPTS" ]; then
  echo "redis-init: nodes did not become ready within ${MAX_ATTEMPTS} attempts" >&2
  exit 1
fi

cluster_info=$(redis-cli -h redis-node1 -p 6379 cluster info 2>/dev/null || true)
if ! printf '%s\n' "$cluster_info" | grep -q '^cluster_state:ok'; then
  known_nodes=$(printf '%s\n' "$cluster_info" | sed -n 's/^cluster_known_nodes:\([0-9][0-9]*\).*/\1/p')
  if [ -n "$known_nodes" ] && [ "$known_nodes" -gt 1 ]; then
    echo "redis-init: existing cluster metadata is degraded; refusing to recreate it" >&2
    exit 1
  fi

  redis-cli --cluster create $NODES --cluster-replicas 1 --cluster-yes
fi

attempt=1
while [ "$attempt" -le "$MAX_ATTEMPTS" ]; do
  cluster_info=$(redis-cli -h redis-node1 -p 6379 cluster info 2>/dev/null || true)
  if printf '%s\n' "$cluster_info" | grep -q '^cluster_state:ok' &&
     printf '%s\n' "$cluster_info" | grep -q '^cluster_slots_assigned:16384'; then
    redis-cli --cluster check redis-node1:6379 >/dev/null
    echo "redis-init: cluster is ready with all 16384 slots assigned"
    exit 0
  fi
  attempt=$((attempt + 1))
  sleep 1
done

echo "redis-init: cluster did not converge within ${MAX_ATTEMPTS} attempts" >&2
exit 1
