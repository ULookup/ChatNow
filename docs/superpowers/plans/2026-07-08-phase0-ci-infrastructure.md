# Phase 0: CI 基础设施 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让现有 Go 测试套件（tests/func/ 10 个文件 + tests/perf/ 4 个文件）在 GitHub Actions 上自动执行，PR 触发功能测试，合并前触发场景测试，nightly 跑性能测试。

**Architecture:** 新增 `scripts/wait_for_services.sh`（端口轮询健康检查）+ `.github/workflows/ci.yml`（三 job 串联）。复用现有 `docker-compose.yml`（全套服务）和 `tests/Makefile`（proto/test-func/test-scenario/test-perf 目标）。不修改任何生产代码。

**Tech Stack:** GitHub Actions、Docker Compose、Go 1.23、protoc、testify

## Global Constraints

- 目标环境是 Linux（Ubuntu 22.04），开发在 macOS。
- 测试语言：纯 Go（testify + 标准 testing）。不引入 C++ 测试。
- CI 三 job 串联：func（每 PR）-> scenario（PR to 3.0-dev + nightly）-> perf（nightly）。
- 复用现有 `docker-compose.yml`，不新增 compose 文件。
- 复用现有 `tests/Makefile`，仅必要时微调。
- 本地与 CI 命令一致：`cd tests && make proto && make test-func`。

---

## File Structure

本 plan 新增/修改以下文件：

```
scripts/wait_for_services.sh              # Task 1: 服务健康检查脚本
tests/.gitignore                          # Task 2: 忽略生成的 proto 代码
.github/workflows/ci.yml                  # Task 3: CI workflow
tests/Makefile                            # Task 4: 微调（如需）
```

不修改任何生产代码（`common/`、`identity/`、`media/`、`message/` 等）。

---

### Task 1: 服务健康检查脚本

**Files:**
- Create: `scripts/wait_for_services.sh`

**Interfaces:**
- Produces: `scripts/wait_for_services.sh`（CI 和本地用于等待全栈服务就绪）

- [ ] **Step 1: 创建 wait_for_services.sh**

Create `scripts/wait_for_services.sh`:

```bash
#!/bin/bash
# 轮询业务服务端口（gateway + 8 个微服务），全部就绪后退出 0，超时退出 1
# 用法：./scripts/wait_for_services.sh [timeout_seconds]
set -euo pipefail

TIMEOUT=${1:-120}
START=$(date +%s)

check_port() {
    local host=$1 port=$2
    nc -z "$host" "$port" 2>/dev/null
}

wait_for() {
    local name=$1 host=$2 port=$3
    while ! check_port "$host" "$port"; do
        local now=$(date +%s)
        if [ $((now - START)) -gt $TIMEOUT ]; then
            echo "TIMEOUT: $name ($host:$port) 未就绪" >&2
            return 1
        fi
        echo "等待 $name ($host:$port)..."
        sleep 2
    done
    echo "$name ($host:$port) 就绪"
}

wait_for "Gateway-HTTP" 127.0.0.1 9000
wait_for "Gateway-WS" 127.0.0.1 9001
wait_for "IdentityService" 127.0.0.1 10003
wait_for "MediaService" 127.0.0.1 10002
wait_for "TransmiteService" 127.0.0.1 10004
wait_for "MessageService" 127.0.0.1 10005
wait_for "RelationshipService" 127.0.0.1 10006
wait_for "PresenceService" 127.0.0.1 10007
wait_for "ConversationService" 127.0.0.1 10008

echo "所有业务服务就绪"
```

> 注：端口号需与 `docker-compose.yml` 实际映射一致。实施时先 `grep -A2 "ports:" docker-compose.yml | grep "100"` 确认端口，如不一致则修正脚本。

- [ ] **Step 2: 赋予执行权限**

Run: `chmod +x scripts/wait_for_services.sh`
Expected: `ls -la scripts/wait_for_services.sh` 显示 `rwxr-xr-x`。

- [ ] **Step 3: 验证端口与 docker-compose.yml 一致**

Run: `grep -E "^\s+- \"100" docker-compose.yml | sort -u`
Expected: 输出各服务的端口映射，与脚本中 `wait_for` 的端口逐一核对。如不一致，修正脚本。

- [ ] **Step 4: 本地验证（需 docker compose up）**

Run:
```bash
docker compose up -d
./scripts/wait_for_services.sh
```
Expected: 脚本输出每个服务 "就绪"，最终 "所有业务服务就绪"，退出码 0。

- [ ] **Step 5: 提交**

```bash
git add scripts/wait_for_services.sh
git commit -m "infra(test): 服务健康检查脚本

轮询 gateway + 8 个业务服务端口，全部就绪后退出 0，超时 120s 退出 1。
CI 和本地用同一脚本等待全栈启动。"
```

---

### Task 2: tests/.gitignore 忽略生成 proto

**Files:**
- Modify: `tests/.gitignore`

**Interfaces:**
- 无（仅防止生成的 Go proto 代码误提交）

- [ ] **Step 1: 更新 tests/.gitignore**

Modify `tests/.gitignore`（当前为空行）:

```
# 生成的 Go protobuf 代码（make proto 生成）
proto/chatnow/
```

- [ ] **Step 2: 验证 proto/chatnow/ 已被忽略**

Run:
```bash
cd tests && make proto
git status tests/proto/
```
Expected: `git status` 不显示 `tests/proto/chatnow/` 下的文件（已被忽略）。如显示，检查 .gitignore 路径。

- [ ] **Step 3: 提交**

```bash
git add tests/.gitignore
git commit -m "chore(tests): 忽略 make proto 生成的 Go 代码

proto/chatnow/ 由 make proto 动态生成，不入库。"
```

---

### Task 3: CI workflow

**Files:**
- Create: `.github/workflows/ci.yml`

**Interfaces:**
- Produces: `.github/workflows/ci.yml`（三 job 串联 CI）

- [ ] **Step 1: 确认 docker-compose.yml 服务端口**

Run: `grep -E "ports:|^\s+- \"" docker-compose.yml | grep -E "900[01]|100[0-9]" | head -20`
Expected: 确认 gateway 9000/9001 + 各服务 100xx 端口，供 ci.yml 和 wait_for_services.sh 参考。

- [ ] **Step 2: 创建 ci.yml**

Create `.github/workflows/ci.yml`:

```yaml
name: CI

on:
  push:
    branches: [main, develop, 3.0-dev]
  pull_request:
    branches: [3.0-dev]
  schedule:
    - cron: "0 2 * * *"

jobs:
  func:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with:
          go-version: '1.23'
      - name: Install protoc
        run: |
          sudo apt-get update
          sudo apt-get install -y protobuf-compiler netcat-openbsd
      - name: Start full stack
        run: docker compose up -d --build
      - name: Wait for services
        run: ./scripts/wait_for_services.sh
      - name: Generate Go protobuf
        run: cd tests && make proto
      - name: Download Go deps
        run: cd tests && go mod download
      - name: Run functional tests
        run: cd tests && make test-func
      - name: Tear down
        if: always()
        run: docker compose down -v

  scenario:
    needs: func
    runs-on: ubuntu-22.04
    if: github.event_name == 'schedule' || (github.event_name == 'pull_request' && github.base_ref == '3.0-dev')
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with:
          go-version: '1.23'
      - name: Install protoc
        run: |
          sudo apt-get update
          sudo apt-get install -y protobuf-compiler netcat-openbsd
      - name: Start full stack
        run: docker compose up -d --build
      - name: Wait for services
        run: ./scripts/wait_for_services.sh
      - name: Generate Go protobuf
        run: cd tests && make proto
      - name: Download Go deps
        run: cd tests && go mod download
      - name: Run scenario tests
        run: cd tests && make test-scenario
      - name: Tear down
        if: always()
        run: docker compose down -v

  perf:
    needs: scenario
    runs-on: ubuntu-22.04
    if: github.event_name == 'schedule'
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-go@v5
        with:
          go-version: '1.23'
      - name: Install protoc
        run: |
          sudo apt-get update
          sudo apt-get install -y protobuf-compiler netcat-openbsd
      - name: Start full stack
        run: docker compose up -d --build
      - name: Wait for services
        run: ./scripts/wait_for_services.sh
      - name: Generate Go protobuf
        run: cd tests && make proto
      - name: Download Go deps
        run: cd tests && go mod download
      - name: Run performance tests
        run: cd tests && make test-perf
      - name: Tear down
        if: always()
        run: docker compose down -v
```

- [ ] **Step 3: 验证 YAML 语法**

Run: `python3 -c "import yaml; yaml.safe_load(open('.github/workflows/ci.yml'))"`
Expected: 无输出，退出码 0。

- [ ] **Step 4: 提交**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: Go 测试三 job 串联 workflow

- func: 每 push + PR，跑 tests/func/ 全部功能测试
- scenario: PR to 3.0-dev + nightly，跑 E2E 场景测试
- perf: nightly only，跑性能基准
- job 间 needs 串联，func 失败短路
- 复用现有 docker-compose.yml + tests/Makefile"
```

---

### Task 4: Makefile 验证 + Go 依赖检查

**Files:**
- Modify: `tests/Makefile`（仅必要时微调，如添加 deps 前置依赖）

**Interfaces:**
- 无（Makefile 已有 proto/test-func/test-scenario/test-perf 目标）

- [ ] **Step 1: 验证 make proto 能在干净环境跑**

Run:
```bash
cd tests
make clean
make proto
ls proto/chatnow/
```
Expected: `proto/chatnow/` 下有 common/identity/relationship/conversation/message/transmite/media/presence 子目录，各含 .pb.go 文件。

- [ ] **Step 2: 验证 make test-func 能跑**

Run:
```bash
cd tests
make proto
make test-func 2>&1 | tail -20
```
Expected: go test 编译并运行功能测试。如全栈已起（`docker compose up -d`），测试应通过；如未起，测试应 fail 并报连接错误（证明测试确实在跑）。

- [ ] **Step 3: 检查 go.mod 依赖完整性**

Run: `cd tests && go mod tidy && git diff go.mod go.sum`
Expected: `go mod tidy` 后 go.mod/go.sum 无变化（依赖已完整）。如有变化，提交。

- [ ] **Step 4: 如需微调 Makefile，提交**

检查 Makefile 是否需要调整：
- `test-func` 是否需要先 `proto`？当前分开，CI 显式调两个。如想让 `test-func` 自动依赖 `proto`，可加 `.proto` 依赖。但当前设计 CI 显式分步更清晰，**不改**。

如 go.mod/go.sum 有变化：
```bash
git add tests/go.mod tests/go.sum
git commit -m "chore(tests): go mod tidy 修正依赖"
```

如无变化，跳过提交。

---

### Task 5: 验收 - 本地模拟 CI 流程

**Files:**
- 无新增/修改

- [ ] **Step 1: 本地模拟 CI func job**

Run:
```bash
docker compose up -d --build
./scripts/wait_for_services.sh
cd tests && make proto && go mod download && make test-func
docker compose down -v
```
Expected:
- `wait_for_services.sh` 退出码 0
- `make test-func` 输出各测试通过（PASS），无 FAIL
- 如有 FAIL，记录失败用例，属于 Phase 1 修复范围（本 plan 仅搭 CI，不修测试）

- [ ] **Step 2: 本地模拟 CI scenario job**

Run:
```bash
docker compose up -d --build
./scripts/wait_for_services.sh
cd tests && make test-scenario
docker compose down -v
```
Expected: 3 个场景测试（RegisterToFirstMessage / GroupChatLifecycle / FriendFullLifecycle）通过。

- [ ] **Step 3: 验证 git log 完整**

Run: `git log --oneline origin/3.0-dev..HEAD`
Expected: 看到 Task 1-4 的提交（wait_for_services.sh / .gitignore / ci.yml / 可能的 go.mod），消息清晰。

- [ ] **Step 4: 推送并观察 CI**

Run:
```bash
git push origin docs/test-architecture-design
```
Expected: 推送成功。GitHub 上 `func` job 自动触发。观察首次 CI 运行结果，如 fail 则根据日志修正（可能是端口不匹配、protoc 缺插件、Go 版本等环境问题）。

---

## 验收标准

Phase 0 完成后应满足：

1. **CI workflow 存在** - `.github/workflows/ci.yml` 有 func/scenario/perf 三 job
2. **func job 绿** - push 到 docs/test-architecture-design 分支后，func job 自动跑且通过现有 10 个功能测试
3. **本地可复现** - `docker compose up -d` + `./scripts/wait_for_services.sh` + `cd tests && make test-func` 本地能跑通
4. **不破坏现有测试** - tests/func/ 和 tests/perf/ 的测试文件不修改，仅新增 CI 基础设施
5. **生成的 proto 不入库** - `tests/.gitignore` 正确忽略 `proto/chatnow/`

## 已知风险

| 风险 | 处理 |
|---|---|
| docker-compose.yml 端口与 wait_for_services.sh 不一致 | Task 1 Step 3 核对端口 |
| protoc 版本不兼容 | CI 装 protobuf-compiler，如版本问题则改为固定版本下载 |
| Go 1.23 在 ubuntu-22.04 不默认可用 | 用 actions/setup-go@v5 显式安装 |
| 首次 CI 因环境差异 fail | Task 5 Step 4 推送后观察日志，按实际情况修正 |
| 测试本身有 flaky | 属 Phase 1 修复范围，Phase 0 仅搭 CI |

## 下一步

Phase 0 完成后，进入 **Phase 1: 核心消息链路覆盖补齐**（独立 plan）：
- transmite + message 错误路径测试
- 数据一致性断言 helper（直查 DB/ES）
- 离线消息同步场景
- 消息可靠性场景

之后 **Phase 2: media/presence 覆盖 + C++ 测试移除**（独立 plan）。
