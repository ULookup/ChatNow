#!/usr/bin/env bash

set -euo pipefail

required=(
  MYSQL_ROOT_PASSWORD
  CHATNOW_IDENTITY_MYSQL_PASSWORD
  CHATNOW_CONVERSATION_MYSQL_PASSWORD
  CHATNOW_RELATIONSHIP_MYSQL_PASSWORD
  CHATNOW_MESSAGE_MYSQL_PASSWORD
  CHATNOW_MEDIA_MYSQL_PASSWORD
)

for name in "${required[@]}"; do
  if [[ -z "${!name:-}" ]]; then
    echo "mysql-init: required credential is missing: ${name}" >&2
    exit 1
  fi
done

mysql_root() {
  MYSQL_PWD="${MYSQL_ROOT_PASSWORD}" mysql \
    --host=mysql --protocol=tcp --user=root --batch --skip-column-names "$@"
}

secret_hex() {
  printf '%s' "$1" | od -An -tx1 | tr -d ' \n'
}

ensure_user() {
  local user=$1
  local password_hex
  password_hex=$(secret_hex "$2")
  mysql_root mysql <<SQL
SET @password = CONVERT(X'${password_hex}' USING utf8mb4);
SET @create_user = CONCAT(
  'CREATE USER IF NOT EXISTS ''${user}''@''%'' IDENTIFIED BY ', QUOTE(@password));
PREPARE create_user_stmt FROM @create_user;
EXECUTE create_user_stmt;
DEALLOCATE PREPARE create_user_stmt;
SET @alter_user = CONCAT(
  'ALTER USER ''${user}''@''%'' IDENTIFIED BY ', QUOTE(@password));
PREPARE alter_user_stmt FROM @alter_user;
EXECUTE alter_user_stmt;
DEALLOCATE PREPARE alter_user_stmt;
SQL
}

ensure_user chatnow_identity "${CHATNOW_IDENTITY_MYSQL_PASSWORD}"
ensure_user chatnow_conversation "${CHATNOW_CONVERSATION_MYSQL_PASSWORD}"
ensure_user chatnow_relationship "${CHATNOW_RELATIONSHIP_MYSQL_PASSWORD}"
ensure_user chatnow_message "${CHATNOW_MESSAGE_MYSQL_PASSWORD}"
ensure_user chatnow_media "${CHATNOW_MEDIA_MYSQL_PASSWORD}"

mysql_root chatnow <<'SQL'
REVOKE ALL PRIVILEGES, GRANT OPTION FROM 'chatnow_identity'@'%';
REVOKE ALL PRIVILEGES, GRANT OPTION FROM 'chatnow_conversation'@'%';
REVOKE ALL PRIVILEGES, GRANT OPTION FROM 'chatnow_relationship'@'%';
REVOKE ALL PRIVILEGES, GRANT OPTION FROM 'chatnow_message'@'%';
REVOKE ALL PRIVILEGES, GRANT OPTION FROM 'chatnow_media'@'%';

GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`user` TO 'chatnow_identity'@'%';
GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`user_device` TO 'chatnow_identity'@'%';

GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`conversation` TO 'chatnow_conversation'@'%';
GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`conversation_member` TO 'chatnow_conversation'@'%';

GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`friend_apply` TO 'chatnow_relationship'@'%';
GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`relation` TO 'chatnow_relationship'@'%';
GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`user_block` TO 'chatnow_relationship'@'%';

GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`message` TO 'chatnow_message'@'%';
GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`message_attachment` TO 'chatnow_message'@'%';
GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`message_mention` TO 'chatnow_message'@'%';
GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`message_pin` TO 'chatnow_message'@'%';
GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`message_reaction` TO 'chatnow_message'@'%';
GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`message_read` TO 'chatnow_message'@'%';
GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`user_timeline` TO 'chatnow_message'@'%';
GRANT SELECT, UPDATE ON chatnow.`conversation_member` TO 'chatnow_message'@'%';
GRANT SELECT, UPDATE ON chatnow.`conversation` TO 'chatnow_message'@'%';

GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`media_file` TO 'chatnow_media'@'%';
GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`media_blob_ref` TO 'chatnow_media'@'%';
GRANT SELECT, INSERT, UPDATE, DELETE ON chatnow.`media_user_quota` TO 'chatnow_media'@'%';
SQL

echo "mysql-init: application users and table grants are ready"
