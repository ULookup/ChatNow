#!/usr/bin/env bash

set -euo pipefail

DEPENDENCY_WAIT_TIMEOUT_SEC=${DEPENDENCY_WAIT_TIMEOUT_SEC:-120}
deps=""
command=""

while getopts "d:c:" arg; do
  case "$arg" in
    d) deps=$OPTARG ;;
    c) command=$OPTARG ;;
    *) exit 2 ;;
  esac
done

if [[ -z "$command" ]]; then
  echo "entrypoint: service command is required" >&2
  exit 2
fi

deadline=$((SECONDS + DEPENDENCY_WAIT_TIMEOUT_SEC))
for dependency in ${deps//,/ }; do
  host=${dependency%:*}
  port=${dependency##*:}
  if [[ -z "$host" || -z "$port" || "$host" == "$dependency" ]]; then
    echo "entrypoint: invalid dependency locator" >&2
    exit 2
  fi

  until nc -z -w 1 "$host" "$port"; do
    if (( SECONDS >= deadline )); then
      echo "entrypoint: dependency did not become reachable: ${host}:${port}" >&2
      exit 1
    fi
    sleep 1
  done
done

echo "entrypoint: dependencies are reachable"
exec /bin/bash -c "$command"
