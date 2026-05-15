# P4 Media Smoke Test

## Prereq

```bash
# 1. 启 MinIO + 初始化 bucket
cd docker && docker compose up -d minio minio-init && cd ..

# 2. 应用 schema（任选一）
#    A. ODB schema 已在 build 时由 --generate-schema 输出 SQL 文件
mysql -uroot -p chatnow < build/file/media_file.sql
mysql -uroot -p chatnow < build/file/media_blob_ref.sql
mysql -uroot -p chatnow < build/file/media_user_quota.sql
#    B. 或直接走参考 SQL（与 ODB 等价）
mysql -uroot -p chatnow < sql/V4__media.sql

# 3. 启 media_server
cd build/file
./media_server --media_conf=../../conf/media.json \
               --mysql_host=127.0.0.1 --mysql_user=root --mysql_pswd=YOUR_PASS \
               --mysql_db=chatnow --mysql_cset=utf8mb4 \
               --redis_host=127.0.0.1 --redis_port=6379 &
cd -
```

## Run

```bash
TEST_USER_ID=u_smoke_001 \
MYSQL_PASSWORD=YOUR_PASS \
bash file/test/smoke/run_smoke.sh
```

## Expected

- ApplyUpload 返回 `file_id` + presigned `upload_url`
- `curl PUT 'hello'` 返回 200
- CompleteUpload 返回 FileInfo（含 file_size=5）
- ApplyDownload 返回 download_url
- `curl $download_url` 返回 `hello`
- mysql 查到 `media_user_quota.used_bytes == 5`

## Failure modes

| 现象                                | 排查                                                 |
|-------------------------------------|------------------------------------------------------|
| ApplyUpload 5xx                     | media_server stderr；mime/quota/content_hash 校验失败 |
| ApplyUpload 5004                    | 配额超限（5GB 默认）                                  |
| curl PUT 403                        | presigned URL 过期 或 Content-Length 头与实际 body 不符 |
| CompleteUpload 5006 UPLOAD_INCOMPLETE | 客户端未先 PUT bytes 就调 Complete                   |
| CompleteUpload 5005 HASH_MISMATCH   | PUT body size 与 ApplyUpload 声明 file_size 不一致   |
| Download GET 404                    | bucket policy 未生效，重跑 minio-init                |
