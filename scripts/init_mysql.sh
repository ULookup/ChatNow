#!/usr/bin/env bash

set -euo pipefail

if [[ -z "${MYSQL_ROOT_PASSWORD:-}" ]]; then
  echo "mysql-init: MYSQL_ROOT_PASSWORD is required" >&2
  exit 1
fi

mysql_root() {
  MYSQL_PWD="${MYSQL_ROOT_PASSWORD}" mysql \
    --host=mysql --protocol=tcp --user=root --batch --skip-column-names "$@"
}

mysql_root <<'SQL'
CREATE DATABASE IF NOT EXISTS `chatnow` CHARACTER SET utf8mb4;
CREATE TABLE IF NOT EXISTS `chatnow`.`schema_migrations` (
  `version` varchar(128) NOT NULL PRIMARY KEY,
  `checksum` char(64) NOT NULL,
  `applied_at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;
SQL

shopt -s nullglob
migrations=(/migrations/V*.sql)
if (( ${#migrations[@]} == 0 )); then
  echo "mysql-init: no versioned SQL migrations found" >&2
  exit 1
fi
mapfile -t migrations < <(printf '%s\n' "${migrations[@]}" | sort -V)

for migration in "${migrations[@]}"; do
  version=$(basename "$migration" .sql)
  checksum=$(sha256sum "$migration" | awk '{print $1}')
  applied_checksum=$(mysql_root --execute="SELECT checksum FROM chatnow.schema_migrations WHERE version='${version}'")

  if [[ -n "$applied_checksum" ]]; then
    if [[ "$applied_checksum" != "$checksum" ]]; then
      echo "mysql-init: checksum mismatch for applied migration ${version}; migration changed" >&2
      exit 1
    fi
    continue
  fi

  echo "mysql-init: applying ${version}"
  mysql_root chatnow <"$migration"
  mysql_root chatnow --execute="INSERT INTO schema_migrations(version, checksum) VALUES ('${version}', '${checksum}')"
done

/init/converge_mysql_users.sh
echo "mysql-init: schema and application users are ready"
