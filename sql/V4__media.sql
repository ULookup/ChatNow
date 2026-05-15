-- ===========================================================================
-- V4__media.sql —— 媒体子系统三张表 DDL
-- ---------------------------------------------------------------------------
-- 注意：本仓的权威 schema 由 ODB 通过 odb/media_*.hxx 的
--       `--generate-schema` 自动生成（见 file/CMakeLists.txt）。
-- 本文件仅作部署参考与 code review 友好性；与 ODB 输出冲突时以 ODB 为准。
-- ---------------------------------------------------------------------------
-- 表关系：
--   media_file       元数据 + 状态机（pending / committed / deleted / quarantined）
--   media_blob_ref   按 content_hash 去重的物理对象引用计数
--   media_user_quota 用户级配额（默认 5 GB）
-- ===========================================================================

-- 媒体文件元数据
CREATE TABLE IF NOT EXISTS media_file (
    id              BIGINT NOT NULL AUTO_INCREMENT,
    file_id         VARCHAR(20)  NOT NULL,                  -- 业务唯一 ID（snowflake Next() → 16 hex）
    content_hash    VARCHAR(72)  NOT NULL,                  -- "sha256:<64hex>"
    bucket          VARCHAR(64)  NOT NULL,
    object_key      VARCHAR(255) NOT NULL,
    file_name       VARCHAR(255) NOT NULL DEFAULT '',
    file_size       BIGINT       NOT NULL,
    mime_type       VARCHAR(128) NOT NULL,
    purpose         TINYINT UNSIGNED NOT NULL,              -- MediaPurpose enum
    owner_id        VARCHAR(32)  NOT NULL,                  -- user_id
    uploaded_at     DATETIME(3)  NOT NULL,
    status          TINYINT UNSIGNED NOT NULL,              -- 0 pending / 1 committed / 2 deleted / 3 quarantined
    upload_id       VARCHAR(128) NULL,                      -- multipart upload_id (S3)
    PRIMARY KEY (id),
    UNIQUE KEY uq_file_id (file_id),
    KEY idx_hash               (content_hash),
    KEY idx_owner_uploaded     (owner_id, uploaded_at),
    KEY idx_status_uploaded    (status, uploaded_at),
    KEY idx_upload_id          (upload_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 物理 blob 的引用计数（按 content_hash 去重）
CREATE TABLE IF NOT EXISTS media_blob_ref (
    content_hash            VARCHAR(72)  NOT NULL,
    bucket                  VARCHAR(64)  NOT NULL,
    object_key              VARCHAR(255) NOT NULL,
    ref_count               INT          NOT NULL DEFAULT 0,
    total_size              BIGINT       NOT NULL,
    last_decremented_at     DATETIME(3)  NOT NULL,
    PRIMARY KEY (content_hash),
    KEY idx_refcount_decremented (ref_count, last_decremented_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 用户级配额
CREATE TABLE IF NOT EXISTS media_user_quota (
    user_id        VARCHAR(32) NOT NULL,
    used_bytes     BIGINT      NOT NULL DEFAULT 0,
    quota_bytes    BIGINT      NOT NULL DEFAULT 5368709120,   -- 5 GB
    updated_at     DATETIME(3) NOT NULL,
    PRIMARY KEY (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
