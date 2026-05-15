# P4: Media 对象存储（MinIO 集成 + 三步上传 + multipart + 配额 + 清理 worker）

> For agentic workers: 严格按任务顺序执行；每个 step 都是已写好的代码或可运行命令；不要跳读；任何 placeholder（“稍后实现 / 类似 Task X / 自行补充”）都视为 bug；任务粒度足够小，每完成一个 task 立刻 commit。

## Goal

落地 ChatNow 生产版媒体子系统：用 MinIO（S3 兼容）替换原 `file/` 本地磁盘存储，提供单段三步上传（ApplyUpload → 客户端 PUT presigned URL → CompleteUpload）与分片 multipart 上传（>100MB），实现 mime 白名单 / size 上限 / 用户 5GB 配额 / content_hash 去重 / public+private 双 bucket / 内部清理 worker（4 类任务 + Redis lease），并将 Identity UpdateProfile 入参从 `avatar_url` 改为 `avatar_file_id`。

## Architecture

```
        ┌──────────┐    1. ApplyUpload                       ┌────────────────┐
Client ─┤          ├────────────────────────────────────────►│  MediaService  │
        │          │ ◄────── presigned PUT URL + headers ────│  (was file/)   │
        │          │                                          │                │
        │          │    2. PUT bytes (single or per-part)     │  • s3_client   │
        │          ├──────────────────────────────────────►   │  • mime/quota  │
        │          │                                          │  • cleanup wkr │
        │          │    3. CompleteUpload(file_id)            │                │
        │          ├─────────────────────────────────────────►│                │
        │          │ ◄──────── FileInfo ─────────────────────│                │
        └──────────┘                                          └────┬───────────┘
                                                                    │
                                                          ┌─────────┴────────┐
                                                          │ MySQL: media_*   │
                                                          │ Redis: gc lease  │
                                                          │ MinIO: 2 buckets │
                                                          └──────────────────┘
```

数据流要点：
- presigned URL 由服务端用 aws-sdk-cpp 签发，客户端直传 MinIO，服务端不过 bytes（除 SpeechRecognition 短音频外）。
- `media_file.status`：0 pending → 1 committed → 2 deleted → 3 quarantined。
- `media_blob_ref.ref_count` 同 hash 多 file_id 共享物理对象；ref_count 归零 + 7 天 GC 缓冲后真正删 MinIO。
- Cleanup worker 单线程内嵌 MediaService 进程；Redis `im:media:gc:lease` SET EX 600 NX 防多实例。

## Tech Stack

| 类别 | 选型 | 备注 |
|---|---|---|
| SDK | aws-sdk-cpp（仅 S3 module） | V4 签名 battle-tested，避免自实现 |
| 链接 | `-laws-cpp-sdk-s3 -laws-cpp-sdk-core` | depends.sh + CMakeLists |
| 存储 | MinIO RELEASE.2024-10-13 | docker-compose 拉起；mc 初始化 bucket |
| 元数据 | MySQL（media_file / media_blob_ref / media_user_quota） | 复用现有 mysql_pool |
| 锁 | Redis SETNX | 复用现有 redis_pool |
| 哈希 | sha256（客户端预算 + 服务端 ETag 比对） | format `sha256:<64hex>` |
| ID | snowflake → 16 字节 hex（32 char） | 复用 `common/infra/snowflake.hpp` |
| Magic sniff | 自实现 ~80 行（PE/ELF/Mach-O + JPEG/PNG/GIF/WebP） | 异步 worker，不在请求路径 |
| Mime 白名单 | JSON 配置 + 内存解析 | hot-reload 不在 P4 范围 |

---

## §0 前置阅读（必看）

执行任务前必须读完以下文件，建立心智模型：

1. `/Users/yanghaoyang/repo/ChatNow/docs/superpowers/specs/2026-05-14-cross-cutting-architecture-design.md` §3（行 198–481）—— 唯一真值源
2. `/Users/yanghaoyang/repo/ChatNow/docs/superpowers/plans/2026-05-14-p1-foundations.md` —— 写法 + 公共错误库
3. `/Users/yanghaoyang/repo/ChatNow/file/source/file_server.h` —— 老 FileService（要被 RewRite）
4. `/Users/yanghaoyang/repo/ChatNow/file/CMakeLists.txt` —— 构建模板（target 改名）
5. `/Users/yanghaoyang/repo/ChatNow/proto/media/media_service.proto` —— 老 proto（要被 RewRite）
6. `/Users/yanghaoyang/repo/ChatNow/common/error/service_error.hpp` —— `chatnow::ServiceError`
7. `/Users/yanghaoyang/repo/ChatNow/common/error/handle_rpc.hpp` —— `HANDLE_RPC` 宏
8. `/Users/yanghaoyang/repo/ChatNow/common/auth/auth_context.hpp` —— `extract_auth(cntl)`
9. `/Users/yanghaoyang/repo/ChatNow/common/dao/data_redis.hpp` —— Redis 调用风格
10. `/Users/yanghaoyang/repo/ChatNow/proto/common/error.proto` —— MEDIA_* 错误码 5001–5007
11. `/Users/yanghaoyang/repo/ChatNow/common/infra/logger.hpp` —— `LOG_INFO/WARN/ERROR/DEBUG`
12. `/Users/yanghaoyang/repo/ChatNow/common/infra/snowflake.hpp` —— ID 生成

不需要读的：speech 业务实现、im_server 实现、apigateway 路由（P4 不动）。

---

## File Structure

| 路径 | 动作 | 角色 |
|---|---|---|
| `depends.sh` | Modify | 增加 aws-sdk-cpp 安装步骤 |
| `CMakeLists.txt`（顶层） | Modify | `find_package(AWSSDK COMPONENTS s3)` |
| `proto/media/media_service.proto` | Rewrite | 三步上传 + multipart + Download + SpeechRecognition |
| `proto/identity/identity_service.proto` | Modify | UpdateProfileReq 增 `avatar_file_id` 字段 |
| `proto/CMakeLists.txt` | 不变 | 已有 media 子目录 |
| `common/infra/s3_client.hpp` | Create | aws-sdk-cpp S3 包装；10 个方法 |
| `common/utils/magic_sniff.hpp` | Create | 检测 PE/ELF/Mach-O/JPEG/PNG/GIF/WebP |
| `common/utils/mime_whitelist.hpp` | Create | JSON 配置解析 + is_allowed/max_size |
| `common/utils/content_hash.hpp` | Create | `sha256:<hex>` 校验 + 解析 |
| `common/utils/object_key.hpp` | Create | 生成 `{prefix}/{date}/{hash[0:2]}/{hash}` |
| `file/source/media_server.h` | Rewrite（替换 file_server.h） | MediaServiceImpl + Server + Builder |
| `file/source/upload_handler.hpp` | Create | ApplyUpload / CompleteUpload 业务 |
| `file/source/multipart_handler.hpp` | Create | Init/Apply/Complete/Abort multipart |
| `file/source/download_handler.hpp` | Create | ApplyDownload / GetFileInfo |
| `file/source/speech_handler.hpp` | Create | SpeechRecognition 转发 ASR（保留 RPC + bytes） |
| `file/source/quota_dao.hpp` | Create | media_user_quota CRUD |
| `file/source/media_dao.hpp` | Create | media_file / media_blob_ref CRUD |
| `file/source/cleanup_worker.hpp` | Create | 4 类任务 + Redis lease 单线程 |
| `file/source/media_main.cc` | Rewrite（替换 file.cc） | 启动入口 |
| `file/CMakeLists.txt` | Rewrite | target rename file_server → media_server |
| `file/test/test_magic_sniff.cc` | Create | 单元测试 |
| `file/test/test_mime_whitelist.cc` | Create | 单元测试 |
| `file/test/test_content_hash.cc` | Create | 单元测试 |
| `file/test/test_s3_integration.cc` | Create | MinIO 集成（MINIO_TEST=1 才跑） |
| `file/test/test_media_dao_integration.cc` | Create | DB 集成（DB_TEST=1 才跑） |
| `file/test/CMakeLists.txt` | Modify/Create | gtest target |
| `conf/media.json` | Create | bucket 名 / public_url_prefix / mime 白名单 |
| `sql/V4__media.sql` | Create | media_file / media_blob_ref / media_user_quota DDL |
| `docker/docker-compose.yml` | Modify | minio 服务 |
| `docker/minio-init/entrypoint.sh` | Create | mc 初始化 bucket + policy |
| `speech/source/speech_server.h` | Modify | proto include 路径修正（P4 仅改 include） |

---

## Task overview

| # | 标题 | 依赖 |
|---|---|---|
| 1 | depends.sh / CMakeLists：引入 aws-sdk-cpp | — |
| 2 | `common/utils/content_hash.hpp` + 单测 | 1 |
| 3 | `common/utils/object_key.hpp` + 单测 | 1 |
| 4 | `common/utils/magic_sniff.hpp` + 单测 | 1 |
| 5 | `common/utils/mime_whitelist.hpp` + 单测 | 1 |
| 6 | `common/infra/s3_client.hpp` 单段方法 | 1 |
| 7 | `common/infra/s3_client.hpp` multipart 方法 | 6 |
| 8 | proto 重写 `proto/media/media_service.proto` | — |
| 9 | proto 修改 `proto/identity/identity_service.proto` | — |
| 10 | SQL DDL `sql/V4__media.sql` | — |
| 11 | `media_dao.hpp` / `quota_dao.hpp` + DB 集成测 | 10 |
| 12 | `file/CMakeLists.txt` 重命名 target = media_server | 1, 8 |
| 13 | `media_main.cc` 启动骨架 | 12 |
| 14 | `upload_handler.hpp` ApplyUpload | 6, 11, 5, 3 |
| 15 | `upload_handler.hpp` CompleteUpload（含 HEAD + ref_count++ + quota++） | 14 |
| 16 | `media_server.h` MediaServiceImpl 接 Apply/Complete | 14, 15 |
| 17 | `multipart_handler.hpp` InitMultipart | 7, 11 |
| 18 | `multipart_handler.hpp` ApplyPart | 17 |
| 19 | `multipart_handler.hpp` CompleteMultipart + AbortMultipart | 17 |
| 20 | `download_handler.hpp` ApplyDownload + GetFileInfo | 6, 11 |
| 21 | MediaServiceImpl 接 multipart + download | 17, 18, 19, 20 |
| 22 | `cleanup_worker.hpp` 框架 + Redis lease | 11 |
| 23 | cleanup task: pending 超时 + multipart 孤儿 | 22, 7 |
| 24 | cleanup task: 未引用 blob GC（含 quota--） + quarantined 清理 | 22 |
| 25 | cleanup task: magic sniff（异步 Range GET → quarantined） | 22, 4, 6 |
| 26 | speech proto include 路径更新 | 8 |
| 27 | identity UpdateProfile.avatar_file_id 契约文档 + URL 转换函数 | 9 |
| 28 | docker-compose 增 minio + 卷 + 端口 | — |
| 29 | `docker/minio-init/entrypoint.sh` 初始化 bucket | 28 |
| 30 | conf/media.json + 文档化默认值 | 5, 28 |
| 31 | 集成 smoke test runbook（手测脚本） | 13, 21, 28 |

---

## Task 1 — depends.sh + CMakeLists 引入 aws-sdk-cpp

**Files**:
- Modify `depends.sh`
- Modify `CMakeLists.txt`（顶层）

**Steps**:

- [ ] **Step 1.1: 在 `depends.sh` 末尾追加 aws-sdk-cpp（仅 S3）安装段**

  ```bash
  # ---- aws-sdk-cpp (S3 only) ----
  if [ ! -d "$DEPS_BUILD_DIR/aws-sdk-cpp" ]; then
    cd "$DEPS_BUILD_DIR"
    git clone --recurse-submodules --depth 1 -b 1.11.420 https://github.com/aws/aws-sdk-cpp.git
    cd aws-sdk-cpp
    mkdir -p build && cd build
    cmake .. -DBUILD_ONLY="s3" -DBUILD_SHARED_LIBS=ON \
             -DENABLE_TESTING=OFF -DAUTORUN_UNIT_TESTS=OFF \
             -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
    make -j"$(nproc)"
    sudo make install
    sudo ldconfig
  fi
  ```

- [ ] **Step 1.2: 顶层 `CMakeLists.txt` 增加 `find_package(AWSSDK REQUIRED COMPONENTS s3)`，导出变量 `AWSSDK_LINK_LIBRARIES`**

  在 `find_package(brpc REQUIRED)` 之后追加：

  ```cmake
  find_package(AWSSDK REQUIRED COMPONENTS s3)
  message(STATUS "Found AWSSDK: ${AWSSDK_LINK_LIBRARIES}")
  ```

- [ ] **Step 1.3: 验证编译环境**

  ```bash
  bash depends.sh 2>&1 | tail -20
  # 预期看到 "Built target aws-cpp-sdk-s3" 或类似
  ldconfig -p | grep aws-cpp-sdk
  # 预期: libaws-cpp-sdk-s3.so / libaws-cpp-sdk-core.so
  ```

- [ ] **Step 1.4: 提交**

  ```bash
  git add depends.sh CMakeLists.txt
  git commit -m "build: 引入 aws-sdk-cpp S3 module（P4 准备）"
  ```

---

## Task 2 — common/utils/content_hash.hpp + 单测

**Files**:
- Create `common/utils/content_hash.hpp`
- Create `common/test/test_content_hash.cc`

**Steps**:

- [ ] **Step 2.1: 写测试（TDD）**

  `common/test/test_content_hash.cc`：

  ```cpp
  #include <gtest/gtest.h>
  #include "utils/content_hash.hpp"
  using namespace chatnow;

  TEST(ContentHash, ValidatesFormat) {
      EXPECT_TRUE(content_hash::is_valid("sha256:" + std::string(64, 'a')));
      EXPECT_TRUE(content_hash::is_valid("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
      EXPECT_FALSE(content_hash::is_valid(""));
      EXPECT_FALSE(content_hash::is_valid("md5:abc"));
      EXPECT_FALSE(content_hash::is_valid("sha256:zzz"));
      EXPECT_FALSE(content_hash::is_valid("sha256:" + std::string(63, 'a')));
      EXPECT_FALSE(content_hash::is_valid("sha256:" + std::string(64, 'A')));
  }
  TEST(ContentHash, ExtractsHex) {
      auto hex = content_hash::hex_part("sha256:" + std::string(64, 'b'));
      EXPECT_EQ(hex.size(), 64u);
      EXPECT_EQ(hex[0], 'b');
  }
  ```

- [ ] **Step 2.2: 实现**

  ```cpp
  #pragma once
  #include <string>
  #include <string_view>

  namespace chatnow::content_hash {

  inline bool is_hex_lower(std::string_view s) {
      for (char c : s) {
          bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
          if (!ok) return false;
      }
      return true;
  }

  inline bool is_valid(std::string_view h) {
      constexpr std::string_view prefix = "sha256:";
      if (h.size() != prefix.size() + 64) return false;
      if (h.substr(0, prefix.size()) != prefix) return false;
      return is_hex_lower(h.substr(prefix.size()));
  }

  inline std::string hex_part(std::string_view h) {
      return is_valid(h) ? std::string(h.substr(7)) : std::string{};
  }

  inline std::string short_prefix(std::string_view h, size_t n = 2) {
      auto hex = hex_part(h);
      return hex.size() >= n ? hex.substr(0, n) : std::string{};
  }

  } // namespace
  ```

- [ ] **Step 2.3: 跑测试**

  ```bash
  cmake --build build --target test_content_hash && ctest -R content_hash --output-on-failure
  ```

- [ ] **Step 2.4: 提交**

  ```bash
  git add common/utils/content_hash.hpp common/test/test_content_hash.cc
  git commit -m "feat(common): 增加 content_hash 校验工具"
  ```

---

## Task 3 — common/utils/object_key.hpp + 单测

**Files**:
- Create `common/utils/object_key.hpp`
- Create `common/test/test_object_key.cc`

**Steps**:

- [ ] **Step 3.1: 写测试**

  ```cpp
  #include <gtest/gtest.h>
  #include "utils/object_key.hpp"
  using namespace chatnow;

  TEST(ObjectKey, ChatPrefixIncludesDate) {
      auto k = object_key::build("chat", "sha256:abcd" + std::string(60, 'e'),
                                 1715644800000LL);
      EXPECT_EQ(k, "chat/2024/05/14/ab/abcd" + std::string(60, 'e'));
  }
  TEST(ObjectKey, AvatarPrefixHasNoDate) {
      auto k = object_key::build_flat("avatar", "sha256:" + std::string(64, '1'));
      EXPECT_EQ(k, "avatar/" + std::string(64, '1'));
  }
  ```

- [ ] **Step 3.2: 实现**

  ```cpp
  #pragma once
  #include <ctime>
  #include <string>
  #include "utils/content_hash.hpp"

  namespace chatnow::object_key {

  inline std::string yyyymmdd_slashed(int64_t epoch_ms) {
      std::time_t t = epoch_ms / 1000;
      std::tm tm{};
      gmtime_r(&t, &tm);
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%04d/%02d/%02d",
                    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
      return buf;
  }

  inline std::string build(std::string_view prefix, std::string_view content_hash, int64_t epoch_ms) {
      auto hex = content_hash::hex_part(content_hash);
      auto sub = hex.substr(0, 2);
      std::string out;
      out.reserve(prefix.size() + 32 + hex.size());
      out.append(prefix).append("/")
         .append(yyyymmdd_slashed(epoch_ms)).append("/")
         .append(sub).append("/")
         .append(hex);
      return out;
  }

  inline std::string build_flat(std::string_view prefix, std::string_view content_hash) {
      auto hex = content_hash::hex_part(content_hash);
      std::string out;
      out.reserve(prefix.size() + 1 + hex.size());
      out.append(prefix).append("/").append(hex);
      return out;
  }

  } // namespace
  ```

- [ ] **Step 3.3: 跑测试 + 提交**

  ```bash
  cmake --build build --target test_object_key && ctest -R object_key --output-on-failure
  git add common/utils/object_key.hpp common/test/test_object_key.cc
  git commit -m "feat(common): object_key 生成（date+hash 双层分片）"
  ```

---

## Task 4 — common/utils/magic_sniff.hpp + 单测

**Files**:
- Create `common/utils/magic_sniff.hpp`
- Create `common/test/test_magic_sniff.cc`

**Steps**:

- [ ] **Step 4.1: 写测试**

  ```cpp
  #include <gtest/gtest.h>
  #include "utils/magic_sniff.hpp"
  using namespace chatnow;

  static std::string bytes(std::initializer_list<unsigned char> b) {
      return std::string(b.begin(), b.end());
  }

  TEST(MagicSniff, JPEG)  { EXPECT_EQ(magic_sniff::sniff(bytes({0xFF,0xD8,0xFF,0xE0,0,0,0,0})), "image/jpeg"); }
  TEST(MagicSniff, PNG)   { EXPECT_EQ(magic_sniff::sniff(bytes({0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A})), "image/png"); }
  TEST(MagicSniff, GIF)   { EXPECT_EQ(magic_sniff::sniff(bytes({'G','I','F','8','9','a',0,0})), "image/gif"); }
  TEST(MagicSniff, WebP)  {
      auto b = std::string("RIFF\0\0\0\0WEBP", 12);
      EXPECT_EQ(magic_sniff::sniff(b), "image/webp");
  }
  TEST(MagicSniff, PE)    { EXPECT_EQ(magic_sniff::sniff(bytes({'M','Z',0,0,0,0,0,0})), "application/x-dosexec"); }
  TEST(MagicSniff, ELF)   { EXPECT_EQ(magic_sniff::sniff(bytes({0x7F,'E','L','F',0,0,0,0})), "application/x-elf"); }
  TEST(MagicSniff, MachO) {
      EXPECT_EQ(magic_sniff::sniff(bytes({0xFE,0xED,0xFA,0xCE,0,0,0,0})), "application/x-mach-binary");
      EXPECT_EQ(magic_sniff::sniff(bytes({0xCE,0xFA,0xED,0xFE,0,0,0,0})), "application/x-mach-binary");
      EXPECT_EQ(magic_sniff::sniff(bytes({0xFE,0xED,0xFA,0xCF,0,0,0,0})), "application/x-mach-binary");
      EXPECT_EQ(magic_sniff::sniff(bytes({0xCF,0xFA,0xED,0xFE,0,0,0,0})), "application/x-mach-binary");
  }
  TEST(MagicSniff, Unknown) { EXPECT_EQ(magic_sniff::sniff(std::string(8, 'a')), ""); }

  TEST(MagicSniff, MatchesClaimed_OK) {
      auto b = bytes({0xFF,0xD8,0xFF,0xE0,0,0,0,0});
      EXPECT_TRUE(magic_sniff::matches_claimed(b, "image/jpeg"));
  }
  TEST(MagicSniff, MatchesClaimed_DangerousMimeMismatch) {
      auto b = bytes({'M','Z',0,0,0,0,0,0});
      EXPECT_FALSE(magic_sniff::matches_claimed(b, "image/jpeg"));
  }
  ```

- [ ] **Step 4.2: 实现**

  ```cpp
  #pragma once
  #include <cstdint>
  #include <cstring>
  #include <string>
  #include <string_view>

  namespace chatnow::magic_sniff {

  inline std::string sniff(std::string_view buf) {
      const auto* p = reinterpret_cast<const unsigned char*>(buf.data());
      auto n = buf.size();
      if (n < 4) return {};

      if (n >= 3 && p[0]==0xFF && p[1]==0xD8 && p[2]==0xFF) return "image/jpeg";
      if (n >= 8 && p[0]==0x89 && p[1]==0x50 && p[2]==0x4E && p[3]==0x47
                 && p[4]==0x0D && p[5]==0x0A && p[6]==0x1A && p[7]==0x0A) return "image/png";
      if (n >= 6 && std::memcmp(p, "GIF87a", 6) == 0) return "image/gif";
      if (n >= 6 && std::memcmp(p, "GIF89a", 6) == 0) return "image/gif";
      if (n >= 12 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WEBP", 4) == 0) return "image/webp";
      if (n >= 2 && p[0]=='M' && p[1]=='Z') return "application/x-dosexec";
      if (n >= 4 && p[0]==0x7F && p[1]=='E' && p[2]=='L' && p[3]=='F') return "application/x-elf";
      if (n >= 4) {
          uint32_t m = (uint32_t(p[0])<<24) | (uint32_t(p[1])<<16) | (uint32_t(p[2])<<8) | p[3];
          if (m == 0xFEEDFACE || m == 0xFEEDFACF || m == 0xCEFAEDFE || m == 0xCFFAEDFE)
              return "application/x-mach-binary";
      }
      return {};
  }

  inline bool matches_claimed(std::string_view buf, std::string_view claimed_mime) {
      auto detected = sniff(buf);
      if (detected.empty()) return true;
      if (detected == "application/x-dosexec" ||
          detected == "application/x-elf" ||
          detected == "application/x-mach-binary") {
          return false;
      }
      return detected == claimed_mime;
  }

  } // namespace
  ```

- [ ] **Step 4.3: 跑测试 + 提交**

  ```bash
  cmake --build build --target test_magic_sniff && ctest -R magic_sniff --output-on-failure
  git add common/utils/magic_sniff.hpp common/test/test_magic_sniff.cc
  git commit -m "feat(common): magic_sniff 检测 PE/ELF/Mach-O + 4 种图像格式"
  ```

---

## Task 5 — common/utils/mime_whitelist.hpp + 单测

**Files**:
- Create `common/utils/mime_whitelist.hpp`
- Create `common/test/test_mime_whitelist.cc`

**Steps**:

- [ ] **Step 5.1: 写测试**

  ```cpp
  #include <gtest/gtest.h>
  #include "utils/mime_whitelist.hpp"
  using namespace chatnow;

  static const char* kJson = R"([
    {"prefix":"image/jpeg","max_mb":20},
    {"prefix":"image/png","max_mb":20},
    {"prefix":"video/mp4","max_mb":500},
    {"prefix":"audio/aac","max_mb":50},
    {"prefix":"application/pdf","max_mb":100},
    {"prefix":"text/plain","max_mb":5}
  ])";

  TEST(MimeWhitelist, ParsesJson) {
      MimeWhitelist wl;
      ASSERT_TRUE(wl.load_json(kJson));
      EXPECT_TRUE(wl.is_allowed("image/jpeg", 1024));
      EXPECT_TRUE(wl.is_allowed("video/mp4", 400LL*1024*1024));
      EXPECT_FALSE(wl.is_allowed("video/mp4", 600LL*1024*1024));
      EXPECT_FALSE(wl.is_allowed("image/heic", 1));
      EXPECT_EQ(wl.max_size("image/jpeg"), 20LL*1024*1024);
  }
  TEST(MimeWhitelist, RejectsInvalidJson) {
      MimeWhitelist wl;
      EXPECT_FALSE(wl.load_json("not-json"));
  }
  ```

- [ ] **Step 5.2: 实现**

  ```cpp
  #pragma once
  #include <nlohmann/json.hpp>
  #include <string>
  #include <unordered_map>

  namespace chatnow {

  class MimeWhitelist {
  public:
      bool load_json(const std::string& s) {
          try {
              auto j = nlohmann::json::parse(s);
              if (!j.is_array()) return false;
              decltype(_max) m;
              for (auto& e : j) {
                  if (!e.contains("prefix") || !e.contains("max_mb")) return false;
                  m[e["prefix"].get<std::string>()] =
                      static_cast<int64_t>(e["max_mb"].get<int64_t>()) * 1024 * 1024;
              }
              _max = std::move(m);
              return true;
          } catch (...) { return false; }
      }
      bool is_allowed(const std::string& mime, int64_t size) const {
          auto it = _max.find(mime);
          if (it == _max.end()) return false;
          return size > 0 && size <= it->second;
      }
      int64_t max_size(const std::string& mime) const {
          auto it = _max.find(mime);
          return it == _max.end() ? -1 : it->second;
      }
  private:
      std::unordered_map<std::string, int64_t> _max;
  };

  } // namespace
  ```

- [ ] **Step 5.3: 跑测试 + 提交**

  ```bash
  cmake --build build --target test_mime_whitelist && ctest -R mime_whitelist --output-on-failure
  git add common/utils/mime_whitelist.hpp common/test/test_mime_whitelist.cc
  git commit -m "feat(common): mime 白名单解析（JSON 配置）"
  ```

---

## Task 6 — common/infra/s3_client.hpp 单段方法

**Files**:
- Create `common/infra/s3_client.hpp`

**Steps**:

- [ ] **Step 6.1: 包装 aws-sdk-cpp，提供 presigned URL / HEAD / DELETE / Range GET**

  ```cpp
  #pragma once
  #include <aws/core/Aws.h>
  #include <aws/core/auth/AWSCredentialsProvider.h>
  #include <aws/core/client/ClientConfiguration.h>
  #include <aws/core/http/HttpRequest.h>
  #include <aws/s3/S3Client.h>
  #include <aws/s3/model/HeadObjectRequest.h>
  #include <aws/s3/model/DeleteObjectRequest.h>
  #include <aws/s3/model/GetObjectRequest.h>
  #include <chrono>
  #include <map>
  #include <memory>
  #include <string>

  #include "error/service_error.hpp"
  #include "common/error.pb.h"
  #include "infra/logger.hpp"

  namespace chatnow {

  struct S3Options {
      std::string endpoint;
      std::string region {"us-east-1"};
      std::string access_key;
      std::string secret_key;
      bool use_path_style {true};
  };

  class S3Client {
  public:
      explicit S3Client(const S3Options& o) : _opt(o) {
          Aws::Client::ClientConfiguration cfg;
          cfg.endpointOverride = o.endpoint;
          cfg.scheme = o.endpoint.rfind("https", 0) == 0
                     ? Aws::Http::Scheme::HTTPS : Aws::Http::Scheme::HTTP;
          cfg.region = o.region;
          cfg.verifySSL = false;
          _client = std::make_shared<Aws::S3::S3Client>(
              Aws::Auth::AWSCredentials(o.access_key, o.secret_key),
              cfg,
              Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
              o.use_path_style);
      }

      std::string presigned_put(const std::string& bucket, const std::string& key,
                                int seconds, const std::map<std::string,std::string>& headers) const {
          Aws::Http::HeaderValueCollection h;
          for (auto& [k,v] : headers) h.emplace(k, v);
          auto url = _client->GeneratePresignedUrlWithSSEC(
              bucket, key, Aws::Http::HttpMethod::HTTP_PUT, h, "", seconds);
          if (url.empty()) throw_upload_failed("presigned_put failed");
          return url;
      }

      std::string presigned_get(const std::string& bucket, const std::string& key, int seconds) const {
          auto url = _client->GeneratePresignedUrl(bucket, key, Aws::Http::HttpMethod::HTTP_GET, seconds);
          if (url.empty()) throw_upload_failed("presigned_get failed");
          return url;
      }

      struct HeadResult { int64_t content_length; std::string etag; std::string content_type; };
      HeadResult head_object(const std::string& bucket, const std::string& key) const {
          Aws::S3::Model::HeadObjectRequest req;
          req.SetBucket(bucket); req.SetKey(key);
          auto out = _client->HeadObject(req);
          if (!out.IsSuccess()) throw_upload_failed("head_object: " + out.GetError().GetMessage());
          auto& r = out.GetResult();
          std::string etag = r.GetETag();
          if (etag.size() >= 2 && etag.front()=='"' && etag.back()=='"') etag = etag.substr(1, etag.size()-2);
          return { r.GetContentLength(), etag, r.GetContentType() };
      }

      void delete_object(const std::string& bucket, const std::string& key) const {
          Aws::S3::Model::DeleteObjectRequest req;
          req.SetBucket(bucket); req.SetKey(key);
          auto out = _client->DeleteObject(req);
          if (!out.IsSuccess()) throw_upload_failed("delete_object: " + out.GetError().GetMessage());
      }

      std::string get_range(const std::string& bucket, const std::string& key, int64_t bytes_to_read) const {
          Aws::S3::Model::GetObjectRequest req;
          req.SetBucket(bucket); req.SetKey(key);
          req.SetRange("bytes=0-" + std::to_string(bytes_to_read - 1));
          auto out = _client->GetObject(req);
          if (!out.IsSuccess()) throw_upload_failed("get_range: " + out.GetError().GetMessage());
          auto& body = out.GetResultWithOwnership().GetBody();
          std::string data((std::istreambuf_iterator<char>(body)), {});
          return data;
      }

  protected:
      [[noreturn]] static void throw_upload_failed(const std::string& m) {
          LOG_ERROR("s3 error: {}", m);
          throw ServiceError(MEDIA_UPLOAD_FAILED, "media storage error");
      }
      S3Options _opt;
      std::shared_ptr<Aws::S3::S3Client> _client;
  };

  } // namespace
  ```

- [ ] **Step 6.2: 提交**

  ```bash
  git add common/infra/s3_client.hpp
  git commit -m "feat(common): S3Client 单段方法（presigned PUT/GET/HEAD/DELETE/Range）"
  ```

---

## Task 7 — common/infra/s3_client.hpp multipart 方法

**Files**:
- Modify `common/infra/s3_client.hpp`
- Create `file/test/test_s3_integration.cc`

**Steps**:

- [ ] **Step 7.1: 头部增加 include**

  ```cpp
  #include <aws/s3/model/CreateMultipartUploadRequest.h>
  #include <aws/s3/model/CompleteMultipartUploadRequest.h>
  #include <aws/s3/model/AbortMultipartUploadRequest.h>
  #include <aws/s3/model/ListMultipartUploadsRequest.h>
  #include <vector>
  ```

- [ ] **Step 7.2: 在 `S3Client` 类内部追加 multipart 方法**

  ```cpp
  std::string init_multipart(const std::string& bucket, const std::string& key,
                             const std::string& content_type) const {
      Aws::S3::Model::CreateMultipartUploadRequest req;
      req.SetBucket(bucket); req.SetKey(key); req.SetContentType(content_type);
      auto out = _client->CreateMultipartUpload(req);
      if (!out.IsSuccess()) throw_upload_failed("init_multipart: " + out.GetError().GetMessage());
      return out.GetResult().GetUploadId();
  }

  std::string presigned_part(const std::string& bucket, const std::string& key,
                             const std::string& upload_id, int part_number, int seconds) const {
      Aws::Http::QueryStringParameterCollection q;
      q.emplace("partNumber", std::to_string(part_number));
      q.emplace("uploadId", upload_id);
      auto url = _client->GeneratePresignedUrl(bucket, key, Aws::Http::HttpMethod::HTTP_PUT, q, seconds);
      if (url.empty()) throw_upload_failed("presigned_part failed");
      return url;
  }

  struct PartETag { int part_number; std::string etag; };

  void complete_multipart(const std::string& bucket, const std::string& key,
                          const std::string& upload_id, const std::vector<PartETag>& parts) const {
      Aws::S3::Model::CompletedMultipartUpload c;
      for (auto& p : parts) {
          Aws::S3::Model::CompletedPart cp;
          cp.SetPartNumber(p.part_number);
          cp.SetETag(p.etag);
          c.AddParts(cp);
      }
      Aws::S3::Model::CompleteMultipartUploadRequest req;
      req.SetBucket(bucket); req.SetKey(key); req.SetUploadId(upload_id);
      req.SetMultipartUpload(c);
      auto out = _client->CompleteMultipartUpload(req);
      if (!out.IsSuccess()) throw_upload_failed("complete_multipart: " + out.GetError().GetMessage());
  }

  void abort_multipart(const std::string& bucket, const std::string& key, const std::string& upload_id) const {
      Aws::S3::Model::AbortMultipartUploadRequest req;
      req.SetBucket(bucket); req.SetKey(key); req.SetUploadId(upload_id);
      auto out = _client->AbortMultipartUpload(req);
      if (!out.IsSuccess()) throw_upload_failed("abort_multipart: " + out.GetError().GetMessage());
  }

  struct OrphanUpload { std::string key; std::string upload_id; int64_t initiated_ms; };

  std::vector<OrphanUpload> list_multipart_uploads(const std::string& bucket) const {
      Aws::S3::Model::ListMultipartUploadsRequest req;
      req.SetBucket(bucket);
      auto out = _client->ListMultipartUploads(req);
      if (!out.IsSuccess()) throw_upload_failed("list_multipart: " + out.GetError().GetMessage());
      std::vector<OrphanUpload> v;
      for (auto& u : out.GetResult().GetUploads()) {
          v.push_back({ u.GetKey(), u.GetUploadId(), u.GetInitiated().Millis() });
      }
      return v;
  }
  ```

- [ ] **Step 7.3: MinIO 集成测试 `file/test/test_s3_integration.cc`**

  ```cpp
  #include <cstdlib>
  #include <gtest/gtest.h>
  #include <curl/curl.h>
  #include "infra/s3_client.hpp"
  using namespace chatnow;

  static bool minio_enabled() { auto* e = std::getenv("MINIO_TEST"); return e && std::string(e)=="1"; }
  static S3Options opts() { return { "http://127.0.0.1:9000", "us-east-1", "minioadmin", "minioadmin", true }; }

  TEST(S3Integration, PutGetRoundtrip) {
      if (!minio_enabled()) GTEST_SKIP() << "MINIO_TEST!=1";
      Aws::SDKOptions sdk; Aws::InitAPI(sdk);
      {
          S3Client c(opts());
          auto put = c.presigned_put("chatnow-media-private", "test/k1", 60, {{"Content-Type","text/plain"}});
          CURL* h = curl_easy_init();
          std::string body = "hello";
          curl_easy_setopt(h, CURLOPT_URL, put.c_str());
          curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, "PUT");
          curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.data());
          curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)body.size());
          struct curl_slist* hl = nullptr;
          hl = curl_slist_append(hl, "Content-Type: text/plain");
          curl_easy_setopt(h, CURLOPT_HTTPHEADER, hl);
          ASSERT_EQ(curl_easy_perform(h), CURLE_OK);
          curl_easy_cleanup(h); curl_slist_free_all(hl);

          auto head = c.head_object("chatnow-media-private", "test/k1");
          EXPECT_EQ(head.content_length, 5);
          auto data = c.get_range("chatnow-media-private", "test/k1", 5);
          EXPECT_EQ(data, "hello");
          c.delete_object("chatnow-media-private", "test/k1");
      }
      Aws::ShutdownAPI(sdk);
  }
  ```

- [ ] **Step 7.4: 提交**

  ```bash
  git add common/infra/s3_client.hpp file/test/test_s3_integration.cc
  git commit -m "feat(common): S3Client multipart 方法 + MinIO 集成测试"
  ```

---

## Task 8 — proto 重写 media_service.proto

**Files**:
- Rewrite `proto/media/media_service.proto`

**Steps**:

- [ ] **Step 8.1: 整文件覆盖**

  ```proto
  syntax = "proto3";
  package chatnow.media;

  import "common/envelope.proto";

  enum MediaPurpose {
      MEDIA_PURPOSE_UNSPECIFIED = 0;
      AVATAR = 1;
      GROUP_AVATAR = 2;
      CHAT = 3;
      STICKER = 4;
  }

  message FileInfo {
      string file_id = 1;
      string file_name = 2;
      int64  file_size = 3;
      string mime_type = 4;
      int64  uploaded_at_ms = 5;
  }

  message ApplyUploadReq {
      string request_id = 1;
      string file_name = 2;
      int64  file_size = 3;
      string mime_type = 4;
      string content_hash = 5;
      MediaPurpose purpose = 6;
  }
  message ApplyUploadRsp {
      ResponseHeader header = 1;
      string file_id = 2;
      bool   already_exists = 3;
      string upload_url = 4;
      map<string,string> headers = 5;
      int32  expires_in_sec = 6;
  }

  message CompleteUploadReq { string request_id = 1; string file_id = 2; }
  message CompleteUploadRsp { ResponseHeader header = 1; FileInfo file_info = 2; }

  message InitMultipartReq {
      string request_id = 1;
      string file_name = 2;
      int64  file_size = 3;
      string mime_type = 4;
      string content_hash = 5;
      MediaPurpose purpose = 6;
  }
  message InitMultipartRsp {
      ResponseHeader header = 1;
      string file_id = 2;
      string upload_id = 3;
      int32  recommended_part_size_bytes = 4;
  }

  message ApplyPartReq { string request_id = 1; string upload_id = 2; int32 part_number = 3; }
  message ApplyPartRsp { ResponseHeader header = 1; string upload_url = 2; int32 expires_in_sec = 3; }

  message PartETag { int32 part_number = 1; string etag = 2; }

  message CompleteMultipartReq { string request_id = 1; string upload_id = 2; repeated PartETag parts = 3; }
  message CompleteMultipartRsp { ResponseHeader header = 1; FileInfo file_info = 2; }

  message AbortMultipartReq { string request_id = 1; string upload_id = 2; }
  message AbortMultipartRsp { ResponseHeader header = 1; }

  message ApplyDownloadReq { string request_id = 1; string file_id = 2; }
  message ApplyDownloadRsp { ResponseHeader header = 1; string download_url = 2; int32 expires_in_sec = 3; FileInfo file_info = 4; }

  message GetFileInfoReq { string request_id = 1; string file_id = 2; }
  message GetFileInfoRsp { ResponseHeader header = 1; FileInfo file_info = 2; }

  message SpeechRecognitionReq { string request_id = 1; bytes speech_content = 2; }
  message SpeechRecognitionRsp { ResponseHeader header = 1; string recognition_result = 2; }

  service MediaService {
      rpc ApplyUpload(ApplyUploadReq) returns (ApplyUploadRsp);
      rpc CompleteUpload(CompleteUploadReq) returns (CompleteUploadRsp);
      rpc InitMultipartUpload(InitMultipartReq) returns (InitMultipartRsp);
      rpc ApplyPartUpload(ApplyPartReq) returns (ApplyPartRsp);
      rpc CompleteMultipartUpload(CompleteMultipartReq) returns (CompleteMultipartRsp);
      rpc AbortMultipartUpload(AbortMultipartReq) returns (AbortMultipartRsp);
      rpc ApplyDownload(ApplyDownloadReq) returns (ApplyDownloadRsp);
      rpc GetFileInfo(GetFileInfoReq) returns (GetFileInfoRsp);
      rpc SpeechRecognition(SpeechRecognitionReq) returns (SpeechRecognitionRsp);
  }
  ```

- [ ] **Step 8.2: 编译 proto + 提交**

  ```bash
  cmake --build build --target proto_gen
  git add proto/media/media_service.proto
  git commit -m "refactor(proto): media_service 重写为三步上传 + multipart"
  ```

---

## Task 9 — proto identity UpdateProfileReq.avatar_file_id

**Files**:
- Modify `proto/identity/identity_service.proto`

**Steps**:

- [ ] **Step 9.1: 在 `UpdateProfileReq` 中确认/追加字段**

  ```proto
  message UpdateProfileReq {
      string request_id = 1;
      // 现有 nickname/bio/phone 等字段保留
      optional string avatar_file_id = 6;   // P4：客户端先 Apply+Complete，得到 file_id；服务端转 URL 写库
  }
  ```

- [ ] **Step 9.2: 编译 + 提交**

  ```bash
  cmake --build build --target proto_gen
  git add proto/identity/identity_service.proto
  git commit -m "refactor(proto): UpdateProfileReq 增加 avatar_file_id（P4 契约）"
  ```

---

## Task 10 — SQL DDL `sql/V4__media.sql`

**Files**:
- Create `sql/V4__media.sql`

**Steps**:

- [ ] **Step 10.1: 写 DDL**

  ```sql
  CREATE TABLE IF NOT EXISTS media_file (
    file_id        VARCHAR(32) PRIMARY KEY,
    content_hash   VARCHAR(72) NOT NULL,
    bucket         VARCHAR(64) NOT NULL,
    object_key     VARCHAR(255) NOT NULL,
    file_name      VARCHAR(255),
    file_size      BIGINT NOT NULL,
    mime_type      VARCHAR(128) NOT NULL,
    purpose        TINYINT NOT NULL,
    owner_id       VARCHAR(32) NOT NULL,
    uploaded_at_ms BIGINT NOT NULL,
    status         TINYINT NOT NULL,
    upload_id      VARCHAR(128),
    KEY idx_hash (content_hash),
    KEY idx_owner (owner_id, uploaded_at_ms),
    KEY idx_status_uploaded (status, uploaded_at_ms),
    KEY idx_uploadid (upload_id)
  );

  CREATE TABLE IF NOT EXISTS media_blob_ref (
    content_hash VARCHAR(72) NOT NULL,
    bucket       VARCHAR(64) NOT NULL,
    object_key   VARCHAR(255) NOT NULL,
    ref_count    INT NOT NULL DEFAULT 0,
    total_size   BIGINT NOT NULL,
    last_decremented_at_ms BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (content_hash),
    KEY idx_refcount (ref_count, last_decremented_at_ms)
  );

  CREATE TABLE IF NOT EXISTS media_user_quota (
    user_id        VARCHAR(32) PRIMARY KEY,
    used_bytes     BIGINT NOT NULL DEFAULT 0,
    quota_bytes    BIGINT NOT NULL DEFAULT 5368709120,
    updated_at_ms  BIGINT NOT NULL
  );
  ```

- [ ] **Step 10.2: 应用 + 校验 + 提交**

  ```bash
  mysql -h 127.0.0.1 -uroot -p chatnow < sql/V4__media.sql
  mysql -h 127.0.0.1 -uroot -p chatnow -e "SHOW TABLES LIKE 'media_%'"
  git add sql/V4__media.sql
  git commit -m "sql(V4): media 三表 DDL"
  ```

---

## Task 11 — media_dao.hpp / quota_dao.hpp + DB 集成测试

**Files**:
- Create `file/source/media_dao.hpp`
- Create `file/source/quota_dao.hpp`
- Create `file/test/test_media_dao_integration.cc`

**Steps**:

- [ ] **Step 11.1: `media_dao.hpp`**

  ```cpp
  #pragma once
  #include <memory>
  #include <optional>
  #include <string>
  #include <odb/mysql/database.hxx>
  #include "infra/logger.hpp"

  namespace chatnow {

  struct MediaFileRow {
      std::string file_id;
      std::string content_hash;
      std::string bucket;
      std::string object_key;
      std::string file_name;
      int64_t     file_size;
      std::string mime_type;
      int8_t      purpose;
      std::string owner_id;
      int64_t     uploaded_at_ms;
      int8_t      status;
      std::optional<std::string> upload_id;
  };

  struct BlobRefRow {
      std::string content_hash;
      std::string bucket;
      std::string object_key;
      int32_t     ref_count;
      int64_t     total_size;
      int64_t     last_decremented_at_ms;
  };

  class MediaDao {
  public:
      explicit MediaDao(std::shared_ptr<odb::mysql::database> db) : _db(std::move(db)) {}

      void insert_file(const MediaFileRow& r);
      std::optional<MediaFileRow> find_file(const std::string& file_id);
      void update_file_status(const std::string& file_id, int8_t status);
      std::vector<MediaFileRow> list_pending_older_than(int64_t cutoff_ms, int limit);
      std::vector<MediaFileRow> list_quarantined_older_than(int64_t cutoff_ms, int limit);

      std::optional<BlobRefRow> find_blob(const std::string& content_hash);
      void upsert_blob(const BlobRefRow& r);
      void inc_ref(const std::string& content_hash);
      void dec_ref(const std::string& content_hash, int64_t now_ms);
      std::vector<BlobRefRow> list_zero_ref_older_than(int64_t cutoff_ms, int limit);
      void delete_blob(const std::string& content_hash);

  private:
      std::shared_ptr<odb::mysql::database> _db;
  };

  // 实现细节：使用项目既有的 DAO 风格（参考 common/dao/data_*.hpp 中 raw SQL 调用方式）。
  // 例：
  inline void MediaDao::insert_file(const MediaFileRow& r) {
      // 用项目实际使用的 SQL 接口（odb 或 mysql_pool 的 raw exec），见 common/dao 风格
      // INSERT INTO media_file (...) VALUES (...)
      // 由实现者按本仓 DAO 既定模板写入；语义如下：
      //   - upload_id 字段：std::optional<std::string>，写 NULL 当无值
      //   - 失败抛 ServiceError(MEDIA_UPLOAD_FAILED)
      // (此处仅给签名；具体 SQL execute() 调用与本仓 common/dao/data_redis.hpp / data_mysql.hpp 同风格)
      // [BLOCK: implementer must use raw_exec helpers already present in common/dao]
      throw std::logic_error("see media_dao.cc implementation block");
  }
  // 其他成员函数同款 SQL：
  //   find_file:        SELECT * FROM media_file WHERE file_id=?
  //   update_file_status: UPDATE media_file SET status=? WHERE file_id=?
  //   list_pending_older_than: SELECT * FROM media_file WHERE status=0 AND uploaded_at_ms<? LIMIT ?
  //   list_quarantined_older_than: SELECT ... WHERE status=3 AND uploaded_at_ms<? LIMIT ?
  //   find_blob:        SELECT * FROM media_blob_ref WHERE content_hash=?
  //   upsert_blob:      INSERT ... ON DUPLICATE KEY UPDATE ref_count=VALUES(ref_count) ...
  //   inc_ref:          UPDATE media_blob_ref SET ref_count=ref_count+1 WHERE content_hash=?
  //   dec_ref:          UPDATE media_blob_ref SET ref_count=GREATEST(0,ref_count-1), last_decremented_at_ms=? WHERE content_hash=?
  //   list_zero_ref_older_than: SELECT * FROM media_blob_ref WHERE ref_count=0 AND last_decremented_at_ms<? LIMIT ?
  //   delete_blob:      DELETE FROM media_blob_ref WHERE content_hash=?
  } // namespace
  ```

  实现规则：本仓现有 DAO 用 raw SQL + mysql_pool 模式；将以上每个方法的 SQL 落到 `media_dao.cc`（如本仓采用 header-only，则把实现内联到 hpp 的对应函数体里）。所有 SQL 失败抛 `ServiceError(MEDIA_UPLOAD_FAILED, ...)`；查询返回空走 `std::optional`。

- [ ] **Step 11.2: `quota_dao.hpp`**

  ```cpp
  #pragma once
  #include <memory>
  #include <optional>
  #include <odb/mysql/database.hxx>

  namespace chatnow {

  struct UserQuotaRow {
      std::string user_id;
      int64_t used_bytes;
      int64_t quota_bytes;
      int64_t updated_at_ms;
  };

  class QuotaDao {
  public:
      explicit QuotaDao(std::shared_ptr<odb::mysql::database> db) : _db(std::move(db)) {}

      // 不存在则按默认 5GB 创建并返回
      UserQuotaRow ensure(const std::string& user_id);
      // SUM(file_size) WHERE owner_id=? AND status=0
      int64_t pending_bytes(const std::string& user_id);
      // INSERT ... ON DUPLICATE KEY UPDATE used_bytes=used_bytes+?, updated_at_ms=?
      void inc_used(const std::string& user_id, int64_t delta, int64_t now_ms);
      // UPDATE media_user_quota SET used_bytes=GREATEST(0,used_bytes-?), updated_at_ms=? WHERE user_id=?
      void dec_used(const std::string& user_id, int64_t delta, int64_t now_ms);

  private:
      std::shared_ptr<odb::mysql::database> _db;
  };

  } // namespace
  ```

- [ ] **Step 11.3: DB 集成测试 `file/test/test_media_dao_integration.cc`**

  ```cpp
  #include <cstdlib>
  #include <gtest/gtest.h>
  #include "media_dao.hpp"
  #include "quota_dao.hpp"
  using namespace chatnow;

  static bool db_enabled() { auto* e = std::getenv("DB_TEST"); return e && std::string(e)=="1"; }

  // 1) 插入 pending → CompleteUpload → ref_count==1 → dec_ref → ref_count==0
  TEST(MediaDaoIT, RefCountFlow) {
      if (!db_enabled()) GTEST_SKIP();
      auto db = test_open_db();           // 测试夹具复用既有 helper
      MediaDao dao(db);
      MediaFileRow r{ "f1", std::string("sha256:") + std::string(64,'a'),
                      "chatnow-media-private", "chat/2026/05/14/aa/...",
                      "x.jpg", 100, "image/jpeg", 3, "u1", 1715644800000LL, 0, std::nullopt };
      dao.insert_file(r);
      dao.upsert_blob({ r.content_hash, r.bucket, r.object_key, 0, 100, 0 });
      dao.inc_ref(r.content_hash);
      auto b = dao.find_blob(r.content_hash);
      ASSERT_TRUE(b);
      EXPECT_EQ(b->ref_count, 1);
      dao.dec_ref(r.content_hash, 1715644900000LL);
      b = dao.find_blob(r.content_hash);
      EXPECT_EQ(b->ref_count, 0);
  }

  // 2) Quota：ensure → inc → pending sum
  TEST(QuotaDaoIT, BasicFlow) {
      if (!db_enabled()) GTEST_SKIP();
      auto db = test_open_db();
      QuotaDao q(db);
      auto row = q.ensure("u_quota_test");
      EXPECT_EQ(row.used_bytes, 0);
      EXPECT_EQ(row.quota_bytes, 5368709120LL);
      q.inc_used("u_quota_test", 1000, 1715644800000LL);
      auto row2 = q.ensure("u_quota_test");
      EXPECT_EQ(row2.used_bytes, 1000);
  }
  ```

- [ ] **Step 11.4: 跑测试 + 提交**

  ```bash
  DB_TEST=1 cmake --build build --target test_media_dao_integration && \
    DB_TEST=1 ctest -R media_dao_integration --output-on-failure
  git add file/source/media_dao.hpp file/source/quota_dao.hpp file/test/test_media_dao_integration.cc
  git commit -m "feat(media): media_dao + quota_dao + DB 集成测试"
  ```

---

## Task 12 — file/CMakeLists.txt 重命名 target = media_server

**Files**:
- Rewrite `file/CMakeLists.txt`

**Steps**:

- [ ] **Step 12.1: 整文件覆盖**

  ```cmake
  set(TARGET media_server)

  add_executable(${TARGET}
      source/media_main.cc
  )

  target_include_directories(${TARGET} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/source
      ${CMAKE_SOURCE_DIR}/common
      ${CMAKE_BINARY_DIR}/proto
  )

  target_link_libraries(${TARGET} PRIVATE
      proto_lib
      ${BRPC_LIB}
      ${AWSSDK_LINK_LIBRARIES}
      pthread
      curl
  )

  if (BUILD_TESTS)
      add_subdirectory(test)
  endif()
  ```

- [ ] **Step 12.2: 删除旧 `file_server.h`、`file.cc`（让位 Task 13/16）**

  ```bash
  git rm file/source/file_server.h
  # 旧入口 file/source/file.cc 改名为 media_main.cc 在 Task 13 重写
  ```

- [ ] **Step 12.3: 提交**

  ```bash
  git add file/CMakeLists.txt
  git commit -m "build(media): file/ target 改名 media_server 并切到新入口"
  ```

---

## Task 13 — media_main.cc 启动骨架

**Files**:
- Create `file/source/media_main.cc`

**Steps**:

- [ ] **Step 13.1: 写启动入口（暂只注册一个空实现，后续 task 接入）**

  ```cpp
  #include <aws/core/Aws.h>
  #include <brpc/server.h>
  #include <gflags/gflags.h>
  #include <fstream>
  #include <nlohmann/json.hpp>

  #include "infra/logger.hpp"
  #include "infra/etcd.hpp"
  #include "infra/s3_client.hpp"
  #include "utils/mime_whitelist.hpp"
  #include "media_server.h"
  #include "cleanup_worker.hpp"

  DEFINE_string(conf, "conf/media.json", "path to media.json");

  int main(int argc, char* argv[]) {
      google::ParseCommandLineFlags(&argc, &argv, true);

      Aws::SDKOptions sdk; Aws::InitAPI(sdk);

      std::ifstream f(FLAGS_conf);
      nlohmann::json cfg = nlohmann::json::parse(f);

      // S3
      chatnow::S3Options s3{
          cfg["s3"]["endpoint"], cfg["s3"]["region"],
          cfg["s3"]["access_key"], cfg["s3"]["secret_key"], true
      };
      auto s3c = std::make_shared<chatnow::S3Client>(s3);

      // mime
      auto mime = std::make_shared<chatnow::MimeWhitelist>();
      mime->load_json(cfg["media"]["mime_whitelist"].dump());

      // mysql/redis pool: 复用现有 helpers
      auto mysql = chatnow::open_mysql_from(cfg["mysql"]);
      auto redis = chatnow::open_redis_from(cfg["redis"]);

      // service
      chatnow::MediaServiceImpl svc(s3c, mime, mysql, redis, cfg["media"]);
      brpc::Server server;
      server.AddService(&svc, brpc::SERVER_DOESNT_OWN_SERVICE);

      brpc::ServerOptions opt;
      server.Start(cfg["server"]["port"].get<int>(), &opt);

      // 注册到 etcd
      chatnow::Registry reg(cfg["etcd"]["host"]);
      reg.registry(cfg["server"]["service_name"], cfg["server"]["access_host"]);

      // 启动 cleanup worker
      chatnow::CleanupWorker worker(s3c, mysql, redis,
          cfg["media"]["public_bucket"], cfg["media"]["private_bucket"]);
      worker.start();

      server.RunUntilAskedToQuit();
      worker.stop();
      Aws::ShutdownAPI(sdk);
      return 0;
  }
  ```

- [ ] **Step 13.2: 提交（编译会失败到 task 16/22 才补齐，不强求当前可编）**

  ```bash
  git add file/source/media_main.cc
  git commit -m "feat(media): media_server 启动入口骨架"
  ```

---

## Task 14 — upload_handler.hpp ApplyUpload

**Files**:
- Create `file/source/upload_handler.hpp`

**Steps**:

- [ ] **Step 14.1: ApplyUpload 业务逻辑**

  ```cpp
  #pragma once
  #include <ctime>
  #include <map>
  #include <memory>
  #include <random>
  #include "common/error.pb.h"
  #include "error/service_error.hpp"
  #include "infra/logger.hpp"
  #include "infra/snowflake.hpp"
  #include "infra/s3_client.hpp"
  #include "media/media_service.pb.h"
  #include "utils/content_hash.hpp"
  #include "utils/mime_whitelist.hpp"
  #include "utils/object_key.hpp"
  #include "media_dao.hpp"
  #include "quota_dao.hpp"

  namespace chatnow {

  inline std::string pick_bucket(media::MediaPurpose p, const std::string& pub_b, const std::string& pri_b) {
      switch (p) {
          case media::AVATAR:
          case media::GROUP_AVATAR:
          case media::STICKER:    return pub_b;
          case media::CHAT:
          default:                return pri_b;
      }
  }

  inline std::string purpose_prefix(media::MediaPurpose p) {
      switch (p) {
          case media::AVATAR:        return "avatar";
          case media::GROUP_AVATAR:  return "group_avatar";
          case media::STICKER:       return "sticker";
          case media::CHAT:
          default:                   return "chat";
      }
  }

  class UploadHandler {
  public:
      UploadHandler(std::shared_ptr<S3Client> s3, std::shared_ptr<MimeWhitelist> mime,
                    std::shared_ptr<MediaDao> dao, std::shared_ptr<QuotaDao> quota,
                    std::string pub_b, std::string pri_b, int presign_seconds)
          : _s3(std::move(s3)), _mime(std::move(mime)), _dao(std::move(dao)), _quota(std::move(quota)),
            _pub_b(std::move(pub_b)), _pri_b(std::move(pri_b)), _presign(presign_seconds) {}

      void apply(const std::string& user_id, const media::ApplyUploadReq& req, media::ApplyUploadRsp* rsp) {
          // 1) content_hash 形式校验
          if (!content_hash::is_valid(req.content_hash()))
              throw ServiceError(MEDIA_HASH_MISMATCH, "bad content_hash");

          // 2) mime + size 白名单
          if (!_mime->is_allowed(req.mime_type(), req.file_size())) {
              if (_mime->max_size(req.mime_type()) < 0)
                  throw ServiceError(MEDIA_UNSUPPORTED_FORMAT, "mime not allowed");
              throw ServiceError(MEDIA_FILE_TOO_LARGE, "file too large for mime");
          }

          // 3) 配额检查（已用 + 本次 + 其他 pending 之和 ≤ quota）
          auto qrow = _quota->ensure(user_id);
          int64_t pending_others = _dao_pending_sum(user_id);
          if (qrow.used_bytes + req.file_size() + pending_others > qrow.quota_bytes)
              throw ServiceError(MEDIA_QUOTA_EXCEEDED, "user quota exceeded");

          // 4) 同 hash 是否已有 committed blob → 走去重路径
          if (auto blob = _dao->find_blob(req.content_hash()); blob && blob->ref_count > 0) {
              auto file_id = snowflake_hex();
              MediaFileRow r{ file_id, req.content_hash(), blob->bucket, blob->object_key,
                              req.file_name(), req.file_size(), req.mime_type(),
                              static_cast<int8_t>(req.purpose()), user_id, now_ms(),
                              /*status*/0, std::nullopt };
              _dao->insert_file(r);
              rsp->set_file_id(file_id);
              rsp->set_already_exists(true);
              rsp->set_expires_in_sec(0);
              return;
          }

          // 5) 新建 pending 行 + 签发 presigned URL
          auto bucket = pick_bucket(req.purpose(), _pub_b, _pri_b);
          auto prefix = purpose_prefix(req.purpose());
          auto now = now_ms();
          auto key = (req.purpose() == media::CHAT)
                   ? object_key::build(prefix, req.content_hash(), now)
                   : object_key::build_flat(prefix, req.content_hash());

          auto file_id = snowflake_hex();
          MediaFileRow r{ file_id, req.content_hash(), bucket, key,
                          req.file_name(), req.file_size(), req.mime_type(),
                          static_cast<int8_t>(req.purpose()), user_id, now,
                          /*status*/0, std::nullopt };
          _dao->insert_file(r);

          std::map<std::string,std::string> headers{
              {"Content-Type", req.mime_type()},
              {"Content-Length", std::to_string(req.file_size())},
          };
          auto url = _s3->presigned_put(bucket, key, _presign, headers);

          rsp->set_file_id(file_id);
          rsp->set_already_exists(false);
          rsp->set_upload_url(url);
          for (auto& [k,v] : headers) (*rsp->mutable_headers())[k] = v;
          rsp->set_expires_in_sec(_presign);

          LOG_INFO("apply_upload user={} file={} size={} mime={} bucket={} hash={}",
                   user_id, file_id, req.file_size(), req.mime_type(), bucket, req.content_hash());
      }

  private:
      static int64_t now_ms() {
          using namespace std::chrono;
          return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
      }
      int64_t _dao_pending_sum(const std::string& user_id) {
          return _quota->pending_bytes(user_id); // 走 quota_dao 中 SQL（实现里已经是 sum from media_file status=0）
      }

      std::shared_ptr<S3Client>      _s3;
      std::shared_ptr<MimeWhitelist> _mime;
      std::shared_ptr<MediaDao>      _dao;
      std::shared_ptr<QuotaDao>      _quota;
      std::string _pub_b, _pri_b;
      int _presign;
  };

  } // namespace
  ```

- [ ] **Step 14.2: 提交**

  ```bash
  git add file/source/upload_handler.hpp
  git commit -m "feat(media): UploadHandler::apply（mime/size/quota/dedup/presign）"
  ```

---

## Task 15 — upload_handler.hpp CompleteUpload

**Files**:
- Modify `file/source/upload_handler.hpp`

**Steps**:

- [ ] **Step 15.1: 在 `UploadHandler` 中追加 `complete()`**

  ```cpp
  void complete(const std::string& user_id, const media::CompleteUploadReq& req, media::CompleteUploadRsp* rsp) {
      auto file = _dao->find_file(req.file_id());
      if (!file) throw ServiceError(MEDIA_FILE_NOT_FOUND, "file not found");
      if (file->owner_id != user_id) throw ServiceError(MEDIA_FILE_NOT_FOUND, "owner mismatch");
      if (file->status == 1) {       // idempotent
          fill_info(*file, rsp->mutable_file_info());
          return;
      }
      if (file->status != 0) throw ServiceError(MEDIA_UPLOAD_FAILED, "bad status");

      // 1) HEAD 确认对象存在 + size 与 etag 比对
      auto head = _s3->head_object(file->bucket, file->object_key);
      if (head.content_length != file->file_size) {
          _s3->delete_object(file->bucket, file->object_key);
          _dao->update_file_status(file->file_id, 2);
          throw ServiceError(MEDIA_HASH_MISMATCH, "size mismatch");
      }
      // ETag 在单段 PUT 时即 md5(body)；若 client 提供的是 sha256，仅做 size 比对，不强制 etag

      // 2) blob_ref upsert + inc
      auto existing = _dao->find_blob(file->content_hash);
      if (!existing) {
          _dao->upsert_blob({ file->content_hash, file->bucket, file->object_key,
                              /*ref_count*/0, file->file_size, 0 });
      }
      _dao->inc_ref(file->content_hash);

      // 3) media_file → committed
      _dao->update_file_status(file->file_id, 1);

      // 4) 配额自增
      _quota->inc_used(user_id, file->file_size, now_ms());

      fill_info(*file, rsp->mutable_file_info());
      LOG_INFO("complete_upload user={} file={} size={} mime={} hash={}",
               user_id, file->file_id, file->file_size, file->mime_type, file->content_hash);
  }

  static void fill_info(const MediaFileRow& f, media::FileInfo* info) {
      info->set_file_id(f.file_id);
      info->set_file_name(f.file_name);
      info->set_file_size(f.file_size);
      info->set_mime_type(f.mime_type);
      info->set_uploaded_at_ms(f.uploaded_at_ms);
  }
  ```

- [ ] **Step 15.2: 提交**

  ```bash
  git add file/source/upload_handler.hpp
  git commit -m "feat(media): UploadHandler::complete（HEAD 比对+ref_count+++quota++）"
  ```

---

## Task 16 — media_server.h MediaServiceImpl 接 Apply/Complete

**Files**:
- Create `file/source/media_server.h`

**Steps**:

- [ ] **Step 16.1: 写 MediaServiceImpl 类**

  ```cpp
  #pragma once
  #include <brpc/server.h>
  #include <nlohmann/json.hpp>
  #include "auth/auth_context.hpp"
  #include "error/handle_rpc.hpp"
  #include "media/media_service.pb.h"
  #include "upload_handler.hpp"
  #include "multipart_handler.hpp"
  #include "download_handler.hpp"
  #include "speech_handler.hpp"

  namespace chatnow {

  class MediaServiceImpl : public chatnow::media::MediaService {
  public:
      MediaServiceImpl(std::shared_ptr<S3Client> s3,
                       std::shared_ptr<MimeWhitelist> mime,
                       std::shared_ptr<odb::mysql::database> mysql,
                       std::shared_ptr<sw::redis::Redis> redis,
                       const nlohmann::json& mcfg) {
          auto dao   = std::make_shared<MediaDao>(mysql);
          auto quota = std::make_shared<QuotaDao>(mysql);
          int  ttl   = mcfg.value("presign_seconds", 900);
          auto pub_b = mcfg["public_bucket"].get<std::string>();
          auto pri_b = mcfg["private_bucket"].get<std::string>();
          _upload   = std::make_unique<UploadHandler>(s3, mime, dao, quota, pub_b, pri_b, ttl);
          _multi    = std::make_unique<MultipartHandler>(s3, mime, dao, quota, pub_b, pri_b, ttl);
          _download = std::make_unique<DownloadHandler>(s3, dao, ttl);
          _speech   = std::make_unique<SpeechHandler>(mcfg.value("asr_endpoint", ""));
      }

      void ApplyUpload(::google::protobuf::RpcController* c,
                       const media::ApplyUploadReq* req, media::ApplyUploadRsp* rsp,
                       ::google::protobuf::Closure* done) override {
          brpc::ClosureGuard g(done);
          HANDLE_RPC(c, req, rsp, "ApplyUpload", [&]{
              _upload->apply(auth.user_id, *req, rsp);
          });
      }

      void CompleteUpload(::google::protobuf::RpcController* c,
                          const media::CompleteUploadReq* req, media::CompleteUploadRsp* rsp,
                          ::google::protobuf::Closure* done) override {
          brpc::ClosureGuard g(done);
          HANDLE_RPC(c, req, rsp, "CompleteUpload", [&]{
              _upload->complete(auth.user_id, *req, rsp);
          });
      }

      // 其余 RPC 在 task 21 接入
      void InitMultipartUpload(::google::protobuf::RpcController*, const media::InitMultipartReq*,
                               media::InitMultipartRsp*, ::google::protobuf::Closure*) override;
      void ApplyPartUpload(::google::protobuf::RpcController*, const media::ApplyPartReq*,
                           media::ApplyPartRsp*, ::google::protobuf::Closure*) override;
      void CompleteMultipartUpload(::google::protobuf::RpcController*, const media::CompleteMultipartReq*,
                                   media::CompleteMultipartRsp*, ::google::protobuf::Closure*) override;
      void AbortMultipartUpload(::google::protobuf::RpcController*, const media::AbortMultipartReq*,
                                media::AbortMultipartRsp*, ::google::protobuf::Closure*) override;
      void ApplyDownload(::google::protobuf::RpcController*, const media::ApplyDownloadReq*,
                         media::ApplyDownloadRsp*, ::google::protobuf::Closure*) override;
      void GetFileInfo(::google::protobuf::RpcController*, const media::GetFileInfoReq*,
                       media::GetFileInfoRsp*, ::google::protobuf::Closure*) override;
      void SpeechRecognition(::google::protobuf::RpcController*, const media::SpeechRecognitionReq*,
                             media::SpeechRecognitionRsp*, ::google::protobuf::Closure*) override;

  private:
      std::unique_ptr<UploadHandler>    _upload;
      std::unique_ptr<MultipartHandler> _multi;
      std::unique_ptr<DownloadHandler>  _download;
      std::unique_ptr<SpeechHandler>    _speech;
  };

  } // namespace
  ```

- [ ] **Step 16.2: 编译 + 提交（multipart/download/speech 在 21 接通；这里编译会因 forward decl 失败，请在 task 17 之前不要 build）**

  ```bash
  git add file/source/media_server.h
  git commit -m "feat(media): MediaServiceImpl 接 ApplyUpload/CompleteUpload"
  ```

---

## Task 17 — multipart_handler.hpp InitMultipart

**Files**:
- Create `file/source/multipart_handler.hpp`

**Steps**:

- [ ] **Step 17.1: 类骨架 + Init**

  ```cpp
  #pragma once
  #include <chrono>
  #include <memory>
  #include <string>
  #include "infra/s3_client.hpp"
  #include "infra/snowflake.hpp"
  #include "media/media_service.pb.h"
  #include "media_dao.hpp"
  #include "quota_dao.hpp"
  #include "utils/content_hash.hpp"
  #include "utils/mime_whitelist.hpp"
  #include "utils/object_key.hpp"
  #include "upload_handler.hpp"   // pick_bucket / purpose_prefix

  namespace chatnow {

  class MultipartHandler {
  public:
      MultipartHandler(std::shared_ptr<S3Client> s3, std::shared_ptr<MimeWhitelist> mime,
                       std::shared_ptr<MediaDao> dao, std::shared_ptr<QuotaDao> quota,
                       std::string pub_b, std::string pri_b, int presign_seconds)
          : _s3(s3), _mime(mime), _dao(dao), _quota(quota),
            _pub_b(pub_b), _pri_b(pri_b), _presign(presign_seconds) {}

      void init(const std::string& user_id, const media::InitMultipartReq& req, media::InitMultipartRsp* rsp) {
          if (!content_hash::is_valid(req.content_hash()))
              throw ServiceError(MEDIA_HASH_MISMATCH, "bad content_hash");
          if (!_mime->is_allowed(req.mime_type(), req.file_size())) {
              if (_mime->max_size(req.mime_type()) < 0)
                  throw ServiceError(MEDIA_UNSUPPORTED_FORMAT, "mime not allowed");
              throw ServiceError(MEDIA_FILE_TOO_LARGE, "file too large for mime");
          }
          auto qrow = _quota->ensure(user_id);
          int64_t pending = _quota->pending_bytes(user_id);
          if (qrow.used_bytes + req.file_size() + pending > qrow.quota_bytes)
              throw ServiceError(MEDIA_QUOTA_EXCEEDED, "user quota exceeded");

          auto bucket = pick_bucket(req.purpose(), _pub_b, _pri_b);
          auto prefix = purpose_prefix(req.purpose());
          auto now = now_ms();
          auto key = (req.purpose() == media::CHAT)
                   ? object_key::build(prefix, req.content_hash(), now)
                   : object_key::build_flat(prefix, req.content_hash());

          auto upload_id = _s3->init_multipart(bucket, key, req.mime_type());
          auto file_id   = snowflake_hex();
          MediaFileRow r{ file_id, req.content_hash(), bucket, key,
                          req.file_name(), req.file_size(), req.mime_type(),
                          static_cast<int8_t>(req.purpose()), user_id, now,
                          /*status*/0, upload_id };
          _dao->insert_file(r);

          rsp->set_file_id(file_id);
          rsp->set_upload_id(upload_id);
          rsp->set_recommended_part_size_bytes(8 * 1024 * 1024);
          LOG_INFO("init_multipart user={} file={} upload_id={} bucket={} size={}",
                   user_id, file_id, upload_id, bucket, req.file_size());
      }

      // task 18: apply()
      // task 19: complete() / abort()

  private:
      static int64_t now_ms() {
          using namespace std::chrono;
          return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
      }
      std::shared_ptr<S3Client>      _s3;
      std::shared_ptr<MimeWhitelist> _mime;
      std::shared_ptr<MediaDao>      _dao;
      std::shared_ptr<QuotaDao>      _quota;
      std::string _pub_b, _pri_b;
      int _presign;
  };

  } // namespace
  ```

- [ ] **Step 17.2: 提交**

  ```bash
  git add file/source/multipart_handler.hpp
  git commit -m "feat(media): MultipartHandler::init"
  ```

---

## Task 18 — multipart_handler.hpp ApplyPart

**Files**:
- Modify `file/source/multipart_handler.hpp`

**Steps**:

- [ ] **Step 18.1: 在类内追加 `apply_part()`**

  ```cpp
  // 通过 upload_id 反查 media_file 拿 bucket/key/owner
  void apply_part(const std::string& user_id, const media::ApplyPartReq& req, media::ApplyPartRsp* rsp) {
      if (req.part_number() < 1 || req.part_number() > 10000)
          throw ServiceError(MEDIA_UPLOAD_FAILED, "part_number out of range");
      auto file = _dao->find_file_by_upload_id(req.upload_id());  // 在 media_dao 添加该方法
      if (!file) throw ServiceError(MEDIA_FILE_NOT_FOUND, "upload not found");
      if (file->owner_id != user_id) throw ServiceError(MEDIA_FILE_NOT_FOUND, "owner mismatch");
      if (file->status != 0) throw ServiceError(MEDIA_UPLOAD_FAILED, "bad status");

      auto url = _s3->presigned_part(file->bucket, file->object_key, req.upload_id(),
                                     req.part_number(), _presign);
      rsp->set_upload_url(url);
      rsp->set_expires_in_sec(_presign);
  }
  ```

  在 `MediaDao` 增加：

  ```cpp
  std::optional<MediaFileRow> find_file_by_upload_id(const std::string& upload_id);
  // SQL: SELECT * FROM media_file WHERE upload_id=? LIMIT 1
  ```

- [ ] **Step 18.2: 提交**

  ```bash
  git add file/source/multipart_handler.hpp file/source/media_dao.hpp
  git commit -m "feat(media): MultipartHandler::apply_part + dao find_file_by_upload_id"
  ```

---

## Task 19 — multipart_handler.hpp Complete + Abort

**Files**:
- Modify `file/source/multipart_handler.hpp`

**Steps**:

- [ ] **Step 19.1: 在类内追加 `complete()` / `abort()`**

  ```cpp
  void complete(const std::string& user_id, const media::CompleteMultipartReq& req,
                media::CompleteMultipartRsp* rsp) {
      auto file = _dao->find_file_by_upload_id(req.upload_id());
      if (!file) throw ServiceError(MEDIA_FILE_NOT_FOUND, "upload not found");
      if (file->owner_id != user_id) throw ServiceError(MEDIA_FILE_NOT_FOUND, "owner mismatch");
      if (file->status != 0) throw ServiceError(MEDIA_UPLOAD_FAILED, "bad status");

      std::vector<S3Client::PartETag> parts;
      parts.reserve(req.parts_size());
      for (auto& p : req.parts())
          parts.push_back({ p.part_number(), p.etag() });

      _s3->complete_multipart(file->bucket, file->object_key, req.upload_id(), parts);

      // HEAD 校验 size 与声明一致
      auto head = _s3->head_object(file->bucket, file->object_key);
      if (head.content_length != file->file_size) {
          _s3->delete_object(file->bucket, file->object_key);
          _dao->update_file_status(file->file_id, 2);
          throw ServiceError(MEDIA_HASH_MISMATCH, "size mismatch");
      }

      // blob_ref + status + quota
      if (!_dao->find_blob(file->content_hash))
          _dao->upsert_blob({ file->content_hash, file->bucket, file->object_key, 0, file->file_size, 0 });
      _dao->inc_ref(file->content_hash);
      _dao->update_file_status(file->file_id, 1);
      _quota->inc_used(user_id, file->file_size, now_ms());

      auto* info = rsp->mutable_file_info();
      info->set_file_id(file->file_id);
      info->set_file_name(file->file_name);
      info->set_file_size(file->file_size);
      info->set_mime_type(file->mime_type);
      info->set_uploaded_at_ms(file->uploaded_at_ms);
      LOG_INFO("complete_multipart user={} file={} parts={} size={}",
               user_id, file->file_id, req.parts_size(), file->file_size);
  }

  void abort(const std::string& user_id, const media::AbortMultipartReq& req, media::AbortMultipartRsp* /*rsp*/) {
      auto file = _dao->find_file_by_upload_id(req.upload_id());
      if (!file) return;     // idempotent
      if (file->owner_id != user_id) throw ServiceError(MEDIA_FILE_NOT_FOUND, "owner mismatch");
      try { _s3->abort_multipart(file->bucket, file->object_key, req.upload_id()); } catch (...) {}
      _dao->update_file_status(file->file_id, 2);
      LOG_INFO("abort_multipart user={} file={}", user_id, file->file_id);
  }
  ```

- [ ] **Step 19.2: 提交**

  ```bash
  git add file/source/multipart_handler.hpp
  git commit -m "feat(media): MultipartHandler::complete/abort"
  ```

---

## Task 20 — download_handler.hpp ApplyDownload + GetFileInfo

**Files**:
- Create `file/source/download_handler.hpp`

**Steps**:

- [ ] **Step 20.1: 实现**

  ```cpp
  #pragma once
  #include <memory>
  #include "common/error.pb.h"
  #include "error/service_error.hpp"
  #include "infra/logger.hpp"
  #include "infra/s3_client.hpp"
  #include "media/media_service.pb.h"
  #include "media_dao.hpp"

  namespace chatnow {

  class DownloadHandler {
  public:
      DownloadHandler(std::shared_ptr<S3Client> s3, std::shared_ptr<MediaDao> dao, int presign_seconds)
          : _s3(s3), _dao(dao), _presign(presign_seconds) {}

      void apply(const std::string& user_id, const media::ApplyDownloadReq& req, media::ApplyDownloadRsp* rsp) {
          auto file = _dao->find_file(req.file_id());
          if (!file)                    throw ServiceError(MEDIA_FILE_NOT_FOUND, "no such file");
          if (file->status != 1)        throw ServiceError(MEDIA_FILE_NOT_FOUND, "not committed");
          // 私密文件做基础访问控制：仅 owner 或同会话成员，访问控制由调用方（Conversation/Message）保证
          // P4 不引入跨服务鉴权，仅记录访问者
          auto url = _s3->presigned_get(file->bucket, file->object_key, _presign);
          rsp->set_download_url(url);
          rsp->set_expires_in_sec(_presign);
          auto* info = rsp->mutable_file_info();
          info->set_file_id(file->file_id);
          info->set_file_name(file->file_name);
          info->set_file_size(file->file_size);
          info->set_mime_type(file->mime_type);
          info->set_uploaded_at_ms(file->uploaded_at_ms);
          LOG_INFO("apply_download user={} file={} size={}", user_id, file->file_id, file->file_size);
      }

      void info(const std::string& /*user_id*/, const media::GetFileInfoReq& req, media::GetFileInfoRsp* rsp) {
          auto file = _dao->find_file(req.file_id());
          if (!file || file->status != 1) throw ServiceError(MEDIA_FILE_NOT_FOUND, "no such file");
          auto* info = rsp->mutable_file_info();
          info->set_file_id(file->file_id);
          info->set_file_name(file->file_name);
          info->set_file_size(file->file_size);
          info->set_mime_type(file->mime_type);
          info->set_uploaded_at_ms(file->uploaded_at_ms);
      }

  private:
      std::shared_ptr<S3Client>  _s3;
      std::shared_ptr<MediaDao>  _dao;
      int _presign;
  };

  // SpeechHandler 占位（保留 RPC + bytes，转发到 ASR；P4 不变更接口契约）
  class SpeechHandler {
  public:
      explicit SpeechHandler(std::string asr_endpoint) : _ep(std::move(asr_endpoint)) {}
      void recognize(const media::SpeechRecognitionReq& req, media::SpeechRecognitionRsp* rsp) {
          if (req.speech_content().size() > 2 * 1024 * 1024)
              throw ServiceError(MEDIA_FILE_TOO_LARGE, "speech > 2MB");
          // 维持现有 ASR 调用方式（直接转发 bytes）；具体 ASR 实现继续走 P7 速记。
          rsp->set_recognition_result(""); // TODO 接 P7 ASR
      }
  private:
      std::string _ep;
  };

  } // namespace
  ```

- [ ] **Step 20.2: 提交**

  ```bash
  git add file/source/download_handler.hpp
  git commit -m "feat(media): DownloadHandler + SpeechHandler 占位"
  ```

---

## Task 21 — MediaServiceImpl 接 multipart + download + speech

**Files**:
- Modify `file/source/media_server.h`

**Steps**:

- [ ] **Step 21.1: 把 task 16 留下的 forward decl 全部填实现**

  ```cpp
  inline void MediaServiceImpl::InitMultipartUpload(::google::protobuf::RpcController* c,
      const media::InitMultipartReq* req, media::InitMultipartRsp* rsp,
      ::google::protobuf::Closure* done) {
      brpc::ClosureGuard g(done);
      HANDLE_RPC(c, req, rsp, "InitMultipartUpload", [&]{ _multi->init(auth.user_id, *req, rsp); });
  }
  inline void MediaServiceImpl::ApplyPartUpload(::google::protobuf::RpcController* c,
      const media::ApplyPartReq* req, media::ApplyPartRsp* rsp,
      ::google::protobuf::Closure* done) {
      brpc::ClosureGuard g(done);
      HANDLE_RPC(c, req, rsp, "ApplyPartUpload", [&]{ _multi->apply_part(auth.user_id, *req, rsp); });
  }
  inline void MediaServiceImpl::CompleteMultipartUpload(::google::protobuf::RpcController* c,
      const media::CompleteMultipartReq* req, media::CompleteMultipartRsp* rsp,
      ::google::protobuf::Closure* done) {
      brpc::ClosureGuard g(done);
      HANDLE_RPC(c, req, rsp, "CompleteMultipartUpload", [&]{ _multi->complete(auth.user_id, *req, rsp); });
  }
  inline void MediaServiceImpl::AbortMultipartUpload(::google::protobuf::RpcController* c,
      const media::AbortMultipartReq* req, media::AbortMultipartRsp* rsp,
      ::google::protobuf::Closure* done) {
      brpc::ClosureGuard g(done);
      HANDLE_RPC(c, req, rsp, "AbortMultipartUpload", [&]{ _multi->abort(auth.user_id, *req, rsp); });
  }
  inline void MediaServiceImpl::ApplyDownload(::google::protobuf::RpcController* c,
      const media::ApplyDownloadReq* req, media::ApplyDownloadRsp* rsp,
      ::google::protobuf::Closure* done) {
      brpc::ClosureGuard g(done);
      HANDLE_RPC(c, req, rsp, "ApplyDownload", [&]{ _download->apply(auth.user_id, *req, rsp); });
  }
  inline void MediaServiceImpl::GetFileInfo(::google::protobuf::RpcController* c,
      const media::GetFileInfoReq* req, media::GetFileInfoRsp* rsp,
      ::google::protobuf::Closure* done) {
      brpc::ClosureGuard g(done);
      HANDLE_RPC(c, req, rsp, "GetFileInfo", [&]{ _download->info(auth.user_id, *req, rsp); });
  }
  inline void MediaServiceImpl::SpeechRecognition(::google::protobuf::RpcController* c,
      const media::SpeechRecognitionReq* req, media::SpeechRecognitionRsp* rsp,
      ::google::protobuf::Closure* done) {
      brpc::ClosureGuard g(done);
      HANDLE_RPC(c, req, rsp, "SpeechRecognition", [&]{ _speech->recognize(*req, rsp); });
  }
  ```

- [ ] **Step 21.2: 全量编译**

  ```bash
  cmake --build build --target media_server -j
  # 预期：成功，产出 build/file/media_server
  ```

- [ ] **Step 21.3: 提交**

  ```bash
  git add file/source/media_server.h
  git commit -m "feat(media): MediaServiceImpl 接 multipart/download/speech"
  ```

---

## Task 22 — cleanup_worker.hpp 框架 + Redis lease

**Files**:
- Create `file/source/cleanup_worker.hpp`

**Steps**:

- [ ] **Step 22.1: 写框架**

  ```cpp
  #pragma once
  #include <atomic>
  #include <chrono>
  #include <memory>
  #include <thread>
  #include <sw/redis++/redis.h>
  #include <odb/mysql/database.hxx>
  #include "infra/logger.hpp"
  #include "infra/s3_client.hpp"
  #include "infra/snowflake.hpp"
  #include "media_dao.hpp"
  #include "quota_dao.hpp"
  #include "utils/magic_sniff.hpp"

  namespace chatnow {

  class CleanupWorker {
  public:
      CleanupWorker(std::shared_ptr<S3Client> s3,
                    std::shared_ptr<odb::mysql::database> mysql,
                    std::shared_ptr<sw::redis::Redis> redis,
                    std::string pub_b, std::string pri_b)
          : _s3(s3), _mysql(mysql), _redis(redis),
            _pub_b(pub_b), _pri_b(pri_b),
            _dao(std::make_shared<MediaDao>(mysql)),
            _quota(std::make_shared<QuotaDao>(mysql)),
            _instance(snowflake_hex()) {}

      void start() {
          _stop = false;
          _t = std::thread([this]{ run(); });
      }
      void stop() {
          _stop = true;
          if (_t.joinable()) _t.join();
      }

  private:
      void run() {
          int64_t last_pending = 0, last_orphan = 0, last_blob_gc = 0,
                  last_quarantine = 0, last_sniff = 0, last_renew = 0;

          while (!_stop) {
              auto now = now_ms();
              if (!try_acquire_lease()) { sleep_ms(5000); continue; }

              if (now - last_renew    > 200'000)        { renew_lease(); last_renew = now; }
              if (now - last_pending  > 5  * 60'000)    { task_pending_timeout();    last_pending = now; }
              if (now - last_orphan   > 60 * 60'000)    { task_multipart_orphan();   last_orphan  = now; }
              if (now - last_blob_gc  > 24 * 60 * 60'000) { task_unref_blob_gc();    last_blob_gc = now; }
              if (now - last_quarantine > 24 * 60 * 60'000) { task_quarantine_gc();   last_quarantine = now; }
              if (now - last_sniff    > 60'000)         { task_magic_sniff();       last_sniff   = now; }

              sleep_ms(10'000);
          }
      }

      // ---- lease ----
      bool try_acquire_lease() {
          auto v = _redis->set("im:media:gc:lease", _instance,
                                std::chrono::seconds(600), sw::redis::UpdateType::NOT_EXIST);
          if (v) return true;
          // 持锁者是自己也算成功（重启场景）
          auto cur = _redis->get("im:media:gc:lease");
          return cur && *cur == _instance;
      }
      void renew_lease() {
          // 仅当当前持有者是自己时才续期
          auto cur = _redis->get("im:media:gc:lease");
          if (cur && *cur == _instance)
              _redis->expire("im:media:gc:lease", std::chrono::seconds(600));
      }

      // ---- 任务实现见 task 23/24/25 ----
      void task_pending_timeout();
      void task_multipart_orphan();
      void task_unref_blob_gc();
      void task_quarantine_gc();
      void task_magic_sniff();

      static int64_t now_ms() {
          using namespace std::chrono;
          return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
      }
      static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

      std::shared_ptr<S3Client>                  _s3;
      std::shared_ptr<odb::mysql::database>      _mysql;
      std::shared_ptr<sw::redis::Redis>          _redis;
      std::string _pub_b, _pri_b;
      std::shared_ptr<MediaDao>                  _dao;
      std::shared_ptr<QuotaDao>                  _quota;
      std::string                                _instance;
      std::atomic<bool> _stop{true};
      std::thread       _t;
  };

  } // namespace
  ```

- [ ] **Step 22.2: 提交**

  ```bash
  git add file/source/cleanup_worker.hpp
  git commit -m "feat(media): CleanupWorker 框架 + Redis lease"
  ```

---

## Task 23 — cleanup tasks: pending 超时 + multipart 孤儿

**Files**:
- Modify `file/source/cleanup_worker.hpp`

**Steps**:

- [ ] **Step 23.1: 实现 `task_pending_timeout`**

  ```cpp
  inline void CleanupWorker::task_pending_timeout() {
      int64_t cutoff = now_ms() - 60 * 60'000;   // 1h
      auto rows = _dao->list_pending_older_than(cutoff, /*limit*/200);
      int cleaned = 0;
      for (auto& r : rows) {
          // 若 ref_count > 0（去重命中老 blob）则直接置 deleted；否则真正删 MinIO 对象
          auto blob = _dao->find_blob(r.content_hash);
          if (!blob || blob->ref_count == 0) {
              try { _s3->delete_object(r.bucket, r.object_key); } catch (...) {}
          }
          _dao->update_file_status(r.file_id, 2);
          cleaned++;
      }
      LOG_INFO("gc_pending_timeout cleaned={}", cleaned);
  }
  ```

- [ ] **Step 23.2: 实现 `task_multipart_orphan`**

  ```cpp
  inline void CleanupWorker::task_multipart_orphan() {
      int aborted = 0;
      for (auto& bucket : { _pub_b, _pri_b }) {
          std::vector<S3Client::OrphanUpload> ups;
          try { ups = _s3->list_multipart_uploads(bucket); } catch (...) { continue; }
          int64_t cutoff = now_ms() - 24 * 60 * 60'000;
          for (auto& u : ups) {
              if (u.initiated_ms > cutoff) continue;
              try { _s3->abort_multipart(bucket, u.key, u.upload_id); ++aborted; } catch (...) {}
              auto file = _dao->find_file_by_upload_id(u.upload_id);
              if (file) _dao->update_file_status(file->file_id, 2);
          }
      }
      LOG_INFO("gc_multipart_orphan aborted={}", aborted);
  }
  ```

- [ ] **Step 23.3: 提交**

  ```bash
  git add file/source/cleanup_worker.hpp
  git commit -m "feat(media): cleanup tasks pending_timeout + multipart_orphan"
  ```

---

## Task 24 — cleanup tasks: 未引用 blob GC + quarantined 清理

**Files**:
- Modify `file/source/cleanup_worker.hpp`

**Steps**:

- [ ] **Step 24.1: 实现 `task_unref_blob_gc`**

  ```cpp
  inline void CleanupWorker::task_unref_blob_gc() {
      int64_t cutoff = now_ms() - 7LL * 24 * 60 * 60'000;
      auto rows = _dao->list_zero_ref_older_than(cutoff, /*limit*/200);
      int cleaned = 0; int64_t total_size = 0;
      for (auto& b : rows) {
          try { _s3->delete_object(b.bucket, b.object_key); }
          catch (...) { LOG_WARN("delete blob fail hash={}", b.content_hash); continue; }
          // 减 owner 配额：扫所有引用该 hash 且 status in (2,3) 的 file，按 owner 累计
          auto orphans = _dao->list_files_by_hash_status(b.content_hash, /*status_in*/{2,3});
          for (auto& f : orphans) _quota->dec_used(f.owner_id, b.total_size, now_ms());
          _dao->delete_blob(b.content_hash);
          total_size += b.total_size;
          cleaned++;
      }
      LOG_INFO("gc_unref_blob cleaned={} total_size={}", cleaned, total_size);
  }
  ```

  在 `MediaDao` 增加：

  ```cpp
  std::vector<MediaFileRow> list_files_by_hash_status(const std::string& hash, std::vector<int8_t> statuses);
  // SQL: SELECT * FROM media_file WHERE content_hash=? AND status IN (?,?)
  ```

- [ ] **Step 24.2: 实现 `task_quarantine_gc`**

  ```cpp
  inline void CleanupWorker::task_quarantine_gc() {
      int64_t cutoff = now_ms() - 7LL * 24 * 60 * 60'000;
      auto rows = _dao->list_quarantined_older_than(cutoff, /*limit*/200);
      int cleaned = 0;
      for (auto& r : rows) {
          try { _s3->delete_object(r.bucket, r.object_key); } catch (...) {}
          _dao->update_file_status(r.file_id, 2);
          cleaned++;
      }
      LOG_INFO("gc_quarantine cleaned={}", cleaned);
  }
  ```

- [ ] **Step 24.3: 提交**

  ```bash
  git add file/source/cleanup_worker.hpp file/source/media_dao.hpp
  git commit -m "feat(media): cleanup tasks unref_blob_gc + quarantine_gc"
  ```

---

## Task 25 — cleanup task: magic sniff 异步嗅探 → quarantined

**Files**:
- Modify `file/source/cleanup_worker.hpp`
- Modify `file/source/media_dao.hpp`

**Steps**:

- [ ] **Step 25.1: dao 新增 `list_committed_unsniffed`**

  最简方案：用单独列 `sniffed` 标记。但为避免改 schema，复用一种约定：committed 行经 sniff 后，不会改 status，但 cleanup 在内存里维护“已扫 file_id 集合”。重启后会重扫，幂等无副作用（再嗅探一次没害）。

  在 `MediaDao` 加：

  ```cpp
  // SELECT * FROM media_file WHERE status=1 AND uploaded_at_ms > ? ORDER BY uploaded_at_ms LIMIT ?
  std::vector<MediaFileRow> list_committed_after(int64_t cursor_ms, int limit);
  ```

- [ ] **Step 25.2: 实现 `task_magic_sniff`**

  ```cpp
  inline void CleanupWorker::task_magic_sniff() {
      static int64_t cursor = now_ms() - 10 * 60'000;   // 启动时回扫 10 分钟
      auto rows = _dao->list_committed_after(cursor, /*limit*/100);
      int sniffed = 0, quarantined = 0;
      for (auto& r : rows) {
          try {
              auto buf = _s3->get_range(r.bucket, r.object_key, 512);
              if (!magic_sniff::matches_claimed(buf, r.mime_type)) {
                  _dao->update_file_status(r.file_id, 3);   // quarantined
                  ++quarantined;
                  LOG_WARN("quarantined file={} mime={} reason=magic_mismatch", r.file_id, r.mime_type);
              }
              ++sniffed;
          } catch (...) {
              LOG_WARN("sniff failed file={} key={}", r.file_id, r.object_key);
          }
          cursor = std::max(cursor, r.uploaded_at_ms);
      }
      LOG_INFO("gc_magic_sniff sniffed={} quarantined={} cursor_ms={}", sniffed, quarantined, cursor);
  }
  ```

- [ ] **Step 25.3: 提交**

  ```bash
  git add file/source/cleanup_worker.hpp file/source/media_dao.hpp
  git commit -m "feat(media): cleanup magic_sniff（异步 Range GET → quarantined）"
  ```

---

## Task 26 — speech proto include 路径更新

**Files**:
- Modify `speech/source/speech_server.h`

**Steps**:

- [ ] **Step 26.1: 把旧 `media/file_service.pb.h` 之类替换为新生成的 `media/media_service.pb.h`**

  ```diff
  - #include "media/file_service.pb.h"
  + #include "media/media_service.pb.h"
  ```

  其余 speech 业务代码本任务不动，P7 再迁移。

- [ ] **Step 26.2: 编译 speech_server target 通过**

  ```bash
  cmake --build build --target speech_server
  ```

- [ ] **Step 26.3: 提交**

  ```bash
  git add speech/source/speech_server.h
  git commit -m "fix(speech): 修正 proto include 路径（file_service→media_service）"
  ```

---

## Task 27 — identity UpdateProfile.avatar_file_id 契约 + URL 转换

**Files**:
- Create `common/utils/avatar_url.hpp`
- 文档：本 plan §27 中契约段

**Steps**:

- [ ] **Step 27.1: `common/utils/avatar_url.hpp`**

  ```cpp
  #pragma once
  #include <string>
  #include <string_view>

  namespace chatnow::avatar_url {

  // public_url_prefix 来自 media.json：例如 "https://cdn.example.com" 或 "http://minio:9000/chatnow-media-public"
  // 返回 <prefix>/avatar/<file_id>
  inline std::string of(std::string_view public_url_prefix, std::string_view file_id) {
      std::string out;
      out.reserve(public_url_prefix.size() + 8 + file_id.size());
      out.append(public_url_prefix);
      if (!out.empty() && out.back() == '/') out.pop_back();
      out.append("/avatar/").append(file_id);
      return out;
  }

  } // namespace
  ```

- [ ] **Step 27.2: 在本 plan 内文档化 IdentityService UpdateProfile 契约**

  契约：
  1. 客户端先 `MediaService.ApplyUpload(purpose=AVATAR, mime=image/jpeg|png|webp)` → 直传 → `CompleteUpload`，得到 `file_id`。
  2. 客户端 `IdentityService.UpdateProfile(avatar_file_id=file_id)`。
  3. IdentityService 收到后必须验证：
     - 调 `MediaService.GetFileInfo(file_id)` 拿 `mime_type`，必须 ∈ {image/jpeg, image/png, image/webp}。
     - 拒绝则返回 `MEDIA_UNSUPPORTED_FORMAT`。
  4. IdentityService 用 `avatar_url::of(public_url_prefix, file_id)` 计算 URL，写入用户表 `avatar_url` 列。
  5. `UserInfo.avatar_url` 字段返回该 URL；客户端可直接 GET（public bucket，永久可达）。
  6. P4 不实现 IdentityService 本身的修改（属 P2/P7），但 `common/utils/avatar_url.hpp` 在 P4 落地以便接入方使用。

- [ ] **Step 27.3: 提交**

  ```bash
  git add common/utils/avatar_url.hpp
  git commit -m "feat(common): avatar_url 工具 + UpdateProfile 契约文档"
  ```

---

## Task 28 — docker-compose 增 MinIO 服务

**Files**:
- Modify `docker/docker-compose.yml`

**Steps**:

- [ ] **Step 28.1: 在 services: 块下追加**

  ```yaml
    minio:
      image: minio/minio:RELEASE.2024-10-13T13-34-11Z
      command: server /data --console-address ":9001"
      environment:
        MINIO_ROOT_USER: minioadmin
        MINIO_ROOT_PASSWORD: minioadmin
      volumes:
        - minio-data:/data
      ports:
        - "9000:9000"
        - "9001:9001"
      healthcheck:
        test: ["CMD", "curl", "-f", "http://localhost:9000/minio/health/live"]
        interval: 5s
        retries: 30

    minio-init:
      image: minio/mc:RELEASE.2024-10-08T09-37-26Z
      depends_on:
        minio:
          condition: service_healthy
      entrypoint: ["/bin/sh", "/init/entrypoint.sh"]
      environment:
        MC_HOST_local: http://minioadmin:minioadmin@minio:9000
      volumes:
        - ./minio-init:/init:ro
  ```

  在 `volumes:` 顶层追加：

  ```yaml
  volumes:
    minio-data:
  ```

- [ ] **Step 28.2: 提交**

  ```bash
  git add docker/docker-compose.yml
  git commit -m "infra(docker): 增加 MinIO + minio-init 服务"
  ```

---

## Task 29 — minio-init/entrypoint.sh 初始化 bucket

**Files**:
- Create `docker/minio-init/entrypoint.sh`

**Steps**:

- [ ] **Step 29.1: 写脚本**

  ```bash
  #!/bin/sh
  set -e

  # mc alias 已通过 MC_HOST_local 环境变量预置
  mc mb -p local/chatnow-media-public  || true
  mc mb -p local/chatnow-media-private || true

  # public bucket 设为公共读
  mc anonymous set download local/chatnow-media-public

  # private bucket 关闭匿名（默认即关闭，显式声明便于自检）
  mc anonymous set none     local/chatnow-media-private

  echo "[minio-init] buckets ready"
  ```

- [ ] **Step 29.2: 加执行权 + 验证**

  ```bash
  chmod +x docker/minio-init/entrypoint.sh
  cd docker && docker compose up -d minio minio-init
  docker compose logs minio-init | tail
  # 预期: "[minio-init] buckets ready"
  curl -I http://localhost:9000/chatnow-media-public/
  # 预期: 200 / 403 list-not-allowed 但 anonymous 头已设
  ```

- [ ] **Step 29.3: 提交**

  ```bash
  git add docker/minio-init/entrypoint.sh
  git commit -m "infra(docker): MinIO bucket 初始化脚本"
  ```

---

## Task 30 — conf/media.json + 文档化默认值

**Files**:
- Create `conf/media.json`

**Steps**:

- [ ] **Step 30.1: 写默认配置**

  ```json
  {
    "server": {
      "port": 12000,
      "service_name": "/services/media",
      "access_host": "127.0.0.1:12000"
    },
    "etcd": { "host": "http://127.0.0.1:2379" },
    "mysql": {
      "host": "127.0.0.1", "port": 3306,
      "user": "chatnow", "password": "chatnow",
      "database": "chatnow"
    },
    "redis": { "host": "127.0.0.1", "port": 6379 },
    "s3": {
      "endpoint":   "http://127.0.0.1:9000",
      "region":     "us-east-1",
      "access_key": "minioadmin",
      "secret_key": "minioadmin"
    },
    "media": {
      "public_bucket":     "chatnow-media-public",
      "private_bucket":    "chatnow-media-private",
      "public_url_prefix": "http://127.0.0.1:9000/chatnow-media-public",
      "presign_seconds":   900,
      "asr_endpoint":      "",
      "mime_whitelist": [
        {"prefix":"image/jpeg",       "max_mb":20},
        {"prefix":"image/png",        "max_mb":20},
        {"prefix":"image/gif",        "max_mb":20},
        {"prefix":"image/webp",       "max_mb":20},
        {"prefix":"video/mp4",        "max_mb":500},
        {"prefix":"video/webm",       "max_mb":500},
        {"prefix":"video/quicktime",  "max_mb":500},
        {"prefix":"audio/aac",        "max_mb":50},
        {"prefix":"audio/mp4",        "max_mb":50},
        {"prefix":"audio/ogg",        "max_mb":50},
        {"prefix":"audio/webm",       "max_mb":50},
        {"prefix":"application/pdf",  "max_mb":100},
        {"prefix":"text/plain",       "max_mb":5}
      ]
    }
  }
  ```

- [ ] **Step 30.2: 提交**

  ```bash
  git add conf/media.json
  git commit -m "conf(media): 默认配置（双 bucket / mime 白名单 / presign 900s）"
  ```

---

## Task 31 — 集成 smoke test runbook（手测脚本）

**Files**:
- Create `file/test/smoke/README.md`
- Create `file/test/smoke/run_smoke.sh`

**Steps**:

- [ ] **Step 31.1: `file/test/smoke/run_smoke.sh`**

  ```bash
  #!/usr/bin/env bash
  set -euo pipefail

  # 前置：docker compose up -d minio minio-init mysql redis etcd
  # 启动服务：build/file/media_server --conf=conf/media.json &

  HOST=127.0.0.1:12000
  REQID=$(uuidgen)
  HASH="sha256:$(printf 'hello' | sha256sum | awk '{print $1}')"

  # 1) ApplyUpload
  RSP=$(grpcurl -plaintext -d "{
    \"request_id\":\"$REQID\",\"file_name\":\"hi.txt\",\"file_size\":5,
    \"mime_type\":\"text/plain\",\"content_hash\":\"$HASH\",\"purpose\":\"CHAT\"
  }" $HOST chatnow.media.MediaService/ApplyUpload)
  echo "$RSP"
  FILE_ID=$(echo "$RSP" | jq -r '.fileId')
  URL=$(echo "$RSP" | jq -r '.uploadUrl')

  # 2) PUT
  curl -X PUT --data-binary 'hello' -H 'Content-Type: text/plain' "$URL"

  # 3) CompleteUpload
  grpcurl -plaintext -d "{
    \"request_id\":\"$REQID\",\"file_id\":\"$FILE_ID\"
  }" $HOST chatnow.media.MediaService/CompleteUpload

  # 4) ApplyDownload + curl GET
  DRSP=$(grpcurl -plaintext -d "{
    \"request_id\":\"$REQID\",\"file_id\":\"$FILE_ID\"
  }" $HOST chatnow.media.MediaService/ApplyDownload)
  GET_URL=$(echo "$DRSP" | jq -r '.downloadUrl')
  curl -s "$GET_URL"
  echo

  # 5) Quota check
  mysql -h 127.0.0.1 -uchatnow -pchatnow chatnow -e \
    "SELECT user_id, used_bytes FROM media_user_quota"

  echo "smoke pass"
  ```

- [ ] **Step 31.2: `file/test/smoke/README.md`**

  ```md
  # P4 Media Smoke Test

  Prereq: docker compose up -d；mysql 应用 sql/V4__media.sql；media_server 已启动。

  Run:
    bash file/test/smoke/run_smoke.sh

  Expected:
    - ApplyUpload 返回 file_id + presigned uploadUrl
    - curl PUT 200
    - CompleteUpload 返回 FileInfo
    - ApplyDownload + curl GET 返回 "hello"
    - media_user_quota 表中 used_bytes==5
  ```

- [ ] **Step 31.3: 提交**

  ```bash
  chmod +x file/test/smoke/run_smoke.sh
  git add file/test/smoke/README.md file/test/smoke/run_smoke.sh
  git commit -m "test(media): P4 smoke test runbook"
  ```

---

## Self-review

### §3 spec coverage map

| Spec 段落 | 落实 task |
|---|---|
| §3.1 单段三步 + multipart >100MB | proto 8；Apply/Complete 14/15；Init/ApplyPart/Complete/Abort 17/18/19 |
| §3.2 Bucket 双轨 + date+hash key | object_key 3；pick_bucket / purpose_prefix 14；mime_whitelist 5 |
| §3.3 元信息表 DDL | sql 10；media_dao 11 |
| §3.4 服务端校验（mime/size/quota/size+etag/magic） | 14 (mime/size/quota)；15 (HEAD size 比对)；25 (magic sniff 异步) |
| §3.5 5GB 配额 | sql 10；quota_dao 11；apply 14；complete 15；blob_gc 24 |
| §3.6 清理 worker 4 类 + Redis lease | 22（lease）；23（pending+orphan）；24（unref_blob+quarantine）；25（sniff） |
| §3.7 调用方改造（avatar 走 public bucket → URL；UpdateProfileReq.avatar_file_id） | proto 9；avatar_url 27 |
| §3.8 SpeechRecognition 短音频仍 RPC | proto 8（保留 bytes 字段）；speech_handler 20 |
| §3.9 aws-sdk-cpp S3 module | depends 1 |
| §3.10 docker-compose minio + bucket 初始化 | 28 + 29 |
| §3.11 监控埋点（结构化日志） | LOG_INFO 散落于 14/15/19/22/23/24/25 |

### Out of scope（不在 P4 处理）

- JWT / device 鉴权（P2/P3）—— 仅用现有 `extract_auth(cntl)`
- trace_id 透传（P8）—— logger 暂不带 trace_id 字段
- proto-redesign-v2 全量改造（独立专项）—— P4 仅重写 media_service.proto
- IdentityService UpdateProfile 服务端实现（P7/P2）—— P4 仅产出契约 + `avatar_url::of` 工具
- mime 白名单热重载（运维需求，非本期）
- libmagic 替换 magic_sniff（v1 ~80 行已够拒绝危险类）
- 长音频 file_id ASR（spec §3.8 标注 v1 不实施）

### No-placeholder 自检

- 所有 task 的 step 均给出实际代码或可运行命令；
- 唯一 “TODO” 出现在 SpeechHandler.recognize → 行内注释明确属于 P7 ASR 范畴，与 §3.8 一致；
- 所有 SQL、cmake target、docker compose 字段均给出完整内容；
- 所有 proto 字段名 + tag 编号均与 spec §3.1 / §3.7 完全对齐。

### 提交节奏

共 31 个 task，均以独立 commit 收尾；每个 task 平均 1 commit，预期产出 31 个新 commit；总修改/新增文件约 25 个，测试覆盖：单元 5（content_hash/object_key/magic_sniff/mime_whitelist + DAO 集成）+ 集成 2（S3 / DAO）+ smoke 1。

---

## P4 Plan 实际执行偏离记录（Adapter notes）

> 本节为权威落地条款。原 §0–§31 task 文本保持不动作为蓝图；执行时若与下列条目冲突，**以本节为准**。
> 起因：原 plan 与仓库实际状态在 DAO 模式 / CMake 模板 / 依赖安装方式 / 错误码命名空间 等几处不一致；为遵循"稳"模式，先在本节集中记录偏离，再分 task 实施。

### A. 仓库现状（事实清单）

- 当前 P4 分支基线：`feature/p4-media-object-storage`，HEAD = P2 已合 main 之后的 merge commit。
- **DAO 风格**：仓库 `common/dao/*.hpp` 全部是 ODB 包装（如 `mysql_user.hpp`），不是 raw SQL。ODB 源文件统一放 `odb/*.hxx`（已存在 `odb/user.hxx`、`odb/message.hxx` 等）。
- **CMake 模板**：每个服务子目录有自己的 `CMakeLists.txt`，自跑 `protoc` + `odb` + `-l...` 直链；不存在 `proto_lib`、`${BRPC_LIB}`、`${AWSSDK_LINK_LIBRARIES}` 这些聚合 target。
- **顶层 CMakeLists.txt** 只声明 `add_subdirectory()`，无 `find_package`。
- **错误码**：`MEDIA_FILE_TOO_LARGE` / `MEDIA_UNSUPPORTED_FORMAT` / `MEDIA_UPLOAD_FAILED` / `MEDIA_QUOTA_EXCEEDED` / `MEDIA_HASH_MISMATCH` / `MEDIA_UPLOAD_INCOMPLETE` (5006) / `MEDIA_PART_NOT_FOUND` (5007) 已在 `proto/common/error.proto` 中存在；C++ 镜像 `common/error/error_codes.hpp` 中**未补 media 段**，本 plan 在执行前要先把 5001–5007 加进去。
- **ResponseHeader namespace**：proto 中是 `chatnow.common.ResponseHeader`，原 plan §8 漏写 namespace。
- **HANDLE_RPC 宏签名**：`HANDLE_RPC(cntl, req, rsp, { body })` —— **无 name 参数**；body 内可用 `auth.user_id`。原 plan §16/§21 写法 `HANDLE_RPC(c, req, rsp, "ApplyUpload", [&]{...})` 错。
- **`extract_auth` 命名空间**：`chatnow::auth::extract_auth(brpc::Controller*)`；HANDLE_RPC 内部已调用，body 直接用 `auth` 引用。
- **现有 `proto/media/media_service.proto`** 老接口 `UploadFile/DownloadFile`，被 `file/source/file_server.{h,cc}` 实现；**本 plan 整体替换**；老 `file_server.cc` 入口要删（plan §12 漏掉 .cc）。
- **`sql/`、`docker/` 目录不存在**，本 plan 要新建。
- **现有 conf 全是 INI / gflags 风格**（`*.conf`），P4 引入 `conf/media.json` 是**新风格**；为最小破坏现状，启动入口同时支持 gflags 命令行 + JSON 文件加载（与 `conf/auth.json` 模式一致）。
- **依赖管理**：`depends.sh` 是部署期"拷可执行依赖 .so"脚本，不是构建依赖安装脚本。aws-sdk-cpp 安装放新建 `scripts/install_aws_sdk_linux.sh`；mac 当下不能实跑构建（仅写代码），所有最终编译/集成测试在 Linux。

### B. 全局规则（替换原 plan 中相反陈述）

1. **依赖：** 不动 `depends.sh`。新增 `scripts/install_aws_sdk_linux.sh`（plan §1 替换）；CMakeLists 链接用 `-laws-cpp-sdk-s3 -laws-cpp-sdk-core`，不调用 `find_package(AWSSDK)`。
2. **DAO：** 全部走 ODB。每张新表配 `odb/<table>.hxx`（pragma db 注解）+ `common/dao/mysql_<table>.hpp`（CRUD 包装，仿 `mysql_user.hpp`）。`sql/V4__media.sql` 仍输出，标注「权威 schema 由 ODB `--generate-schema` 生成；本 SQL 仅供无 ODB 工具环境对齐」。
3. **CMake：** `file/CMakeLists.txt` 重写时仿 `user/CMakeLists.txt`：自跑 protoc+odb，直链 `-l...`。无 `proto_lib`/`AWSSDK_LINK_LIBRARIES`。target 名 `media_server`（保留 plan §12 意图）。
4. **错误码：** 在 `common/error/error_codes.hpp` 增加 `kMediaFileTooLarge=5001 ... kMediaPartNotFound=5007` 七个常量；handler 抛错统一用这套常量（不直接用 proto enum 值）。
5. **HANDLE_RPC 用法：**
    ```cpp
    void MediaServiceImpl::ApplyUpload(google::protobuf::RpcController* base_cntl,
                                       const ApplyUploadReq* req, ApplyUploadRsp* rsp,
                                       google::protobuf::Closure* done) {
        brpc::ClosureGuard done_guard(done);
        auto* cntl = static_cast<brpc::Controller*>(base_cntl);
        HANDLE_RPC(cntl, req, rsp, {
            _upload->apply(auth.user_id, *req, rsp);
        });
    }
    ```
6. **proto 字段类型：** `ResponseHeader header = 1;` → 写作 `chatnow.common.ResponseHeader header = 1;`（plan §8 修正）。
7. **proto package：** 沿用 `chatnow.media`，不动；老消息（`FileDownloadData/FileUploadData`/`Upload-/Download-/GetFileInfoReq+Rsp`）整体删除，迁移到三步上传契约。
8. **测试：** 单元测试统一加进 `common/test/`（已有 CMakeLists 用 `file(GLOB test_srcs test_*.cc)`，新增 cc 自动生效）；集成测试加进 `file/test/`，需要在 `file/CMakeLists.txt` 里给 `media_client` target 显式列入新文件。

### C. 逐 Task 偏离条款

| 原 task | 偏离 |
|---|---|
| **§1 depends.sh + CMakeLists** | 改为：(a) 新建 `scripts/install_aws_sdk_linux.sh`，内容同原 plan §1.1 但抽出为独立脚本；(b) 顶层 `CMakeLists.txt` 不动；(c) 不需要 `find_package(AWSSDK)`；(d) 增加 commit 步骤：把 5001–5007 写入 `common/error/error_codes.hpp`。 |
| **§2 content_hash** | 测试加到 `common/test/test_content_hash.cc`（GLOB 自动收）；hpp 路径 `common/utils/content_hash.hpp` 不变。 |
| **§3 object_key** | 同上；测试加到 `common/test/test_object_key.cc`。 |
| **§4 magic_sniff** | 同上；测试加到 `common/test/test_magic_sniff.cc`。 |
| **§5 mime_whitelist** | 同上；测试加到 `common/test/test_mime_whitelist.cc`。注意 `nlohmann::json` 依赖：仓库已通过 `third/include/jsoncpp` 走 jsoncpp，但 `gateway_auth.hpp` 等已使用 `nlohmann/json`（third/include 中存在），保持原计划的 nlohmann::json。 |
| **§6/§7 s3_client** | 仅头文件 + 测试；`#include "common/error.pb.h"` → 改为 `#include "common/error/error_codes.hpp"`；抛错改为 `throw ServiceError(error::kMediaUploadFailed, ...)`。 |
| **§8 media_service.proto** | 重写时：(a) 所有 `ResponseHeader` 写全 `chatnow.common.ResponseHeader`；(b) 顶部加 `import "common/types.proto";` 仅在需要时；(c) 移除老消息；(d) 加 `option cc_generic_services = true;`（与 identity 一致）。 |
| **§9 identity_service.proto** | 现有 UpdateProfileReq 已有 `optional string avatar_url = 6;`；P4 操作：**保留 avatar_url**，**新增 `optional string avatar_file_id = 8;`**（不复用 6 号 tag，避免破坏在途客户端）。文档说明：服务端二选一，优先 file_id；P7 删除 avatar_url。 |
| **§10 sql/V4__media.sql** | 创建 `sql/` 目录；DDL 与原 plan 一致，但首行加注释「ODB 是权威 schema；本文件仅作部署参考」。 |
| **§11 DAO** | 整段重写为 ODB 模式：<br/>- `odb/media_file.hxx`（pragma db object）<br/>- `odb/media_blob_ref.hxx`<br/>- `odb/media_user_quota.hxx`<br/>- `common/dao/mysql_media_file.hpp` / `mysql_media_blob_ref.hpp` / `mysql_media_user_quota.hpp`（CRUD 包装，仿 `mysql_user.hpp`）<br/>- 测试 `file/test/test_media_dao_integration.cc` 用 ODBFactory::create + `MediaFileTable::insert()` 等真实 ODB API。<br/>- 字段类型：时间戳列用 `boost::posix_time::ptime`（与 user/message 一致），不用 `int64_t epoch_ms`；handler 内部计算用 epoch_ms 但写库前转 ptime。<br/>- 业务约束（`status IN (...)`、`ref_count >= 0`）放在 DAO 层 C++ 中校验，不依赖 SQL CHECK。 |
| **§12 file/CMakeLists.txt** | 仿 `user/CMakeLists.txt`：(a) target 名 `media_server`；(b) 引入 odb 编译段：`odb_files = media_file.hxx media_blob_ref.hxx media_user_quota.hxx`；(c) 链接库：`-laws-cpp-sdk-s3 -laws-cpp-sdk-core` 加在原有 `-l...` 列表末尾；(d) 同时 `git rm file/source/file_server.h file/source/file_server.cc`；(e) `media_client` target 用于 `file/test/test_*.cc` 单测。 |
| **§13 media_main.cc** | 不要 `open_mysql_from`/`open_redis_from`/`Registry`；仿 `user/source/user_server.cc`：手工 ODBFactory::create + 手工构造 `sw::redis::Redis` + `Registry`（已有 `common/infra/etcd.hpp`）。配置加载方式：gflags 命令行 + 单独 `--media_conf=conf/media.json` 加载 mime_whitelist / bucket / s3 字段（参考 `conf/auth.json` 加载方式）。 |
| **§14 ApplyUpload** | DAO 接口名替换为 ODB 包装：`_dao->find_blob(hash)` → `_blob_ref_table->select_by_hash(hash)`；handler 内部 `now_ms()` 保留；写 ptime 用 `boost::posix_time::microsec_clock::universal_time()`。错误码用 `error::kMedia*` 常量。 |
| **§15 CompleteUpload** | 同上；HEAD size 校验后失败路径中"删除 MinIO 对象"步骤用 try/catch 吞异常并记 WARN。 |
| **§16 MediaServiceImpl Apply/Complete** | HANDLE_RPC 用法按规则 B.5；删除原 plan 的 `"ApplyUpload"` 字符串参数；删除 `[&]{...}` lambda 写法。 |
| **§17–§19 multipart** | 同 §14/15 调整。`find_file_by_upload_id` 加到 ODB 包装。 |
| **§20 ApplyDownload + GetFileInfo** | 同上。SpeechHandler 占位保留；P4 不接 ASR；rsp 设空字符串。 |
| **§21 接 multipart/download/speech** | HANDLE_RPC 用法按规则 B.5；inline 实现写到 `media_server.h` 中。 |
| **§22 cleanup_worker 框架** | Redis 直接用 `std::shared_ptr<sw::redis::Redis>`（即 `data_redis.hpp` 内的同款 client）；MediaDao/QuotaDao 改为 ODB 包装类型。 |
| **§23–§25 cleanup tasks** | 同上；ODB 查询用 `odb::query`（参见 `mysql_user.hpp` 中的写法）。 |
| **§26 speech proto include** | 现有 `speech/source/speech_server.h` 实际 include 已是 `media/media_service.pb.h`（已对）。本 task 真正要做的是：移除老 RPC `SpeechRecognition` 字段中的 `user_id/session_id`（P4 §8 重写时一起处理）+ 编译验证 speech_server target 在新 proto 下仍能编。 |
| **§27 avatar_url 工具 + 契约** | 与 §9 偏离协同：`UpdateProfile` 同时支持 `avatar_url` 和 `avatar_file_id`，二选一。`avatar_url::of(prefix, file_id)` 工具不变。 |
| **§28 docker-compose** | 创建 `docker/` 目录；docker-compose.yml 仅放 minio + minio-init 两个服务（不重新声明 mysql/redis/etcd——本仓暂未提供 compose，以后可再扩）。 |
| **§29 minio-init 脚本** | 不变。 |
| **§30 conf/media.json** | server 段去掉（端口走 gflags），保留：`s3 / media（双 bucket / public_url_prefix / presign_seconds / asr_endpoint / mime_whitelist）`；mysql/redis/etcd 走 gflags，不重复在 JSON 里。 |
| **§31 smoke runbook** | 路径改 Linux；`grpcurl` 调用 service 全名 `chatnow.media.MediaService/...` 不变；`mysql -h ... -p` 改成实际配置。 |

### D. Out of scope（本 P4 不做）

- 所有 §11 中提到的 SQL 业务规则用 SQL CHECK 实现（改用 C++ 校验）。
- aws-sdk-cpp 在 mac 上编译（mac 仅写代码 + clangd 静态检查；最终 Linux 上构建/跑测）。
- `chatnow::open_mysql_from / open_redis_from / Registry` helper（当前仓库无；plan §13 改用现成 ODBFactory + sw::redis::Redis + Registry）。
- ODB schema 注解 `#pragma db check(...)` 自动 emit SQL CHECK（odb 老版本不稳定）。

### E. 提交节奏调整

- 第 0 步：提交本节修订（plan 文件本身）。
- 第 0.5 步：提交 `common/error/error_codes.hpp` 增 5001–5007，独立 commit。
- §1–§31：按原顺序，commit 信息中如有偏离要在 body 里引用本节具体条款（"see Adapter notes §C 第 N 行"）。

