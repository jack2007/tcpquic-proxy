# tcpquic-proxy 剩余工作 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 关闭 `docs/superpowers/to_be_fixed.md` 中未完成的验证项与性能边界探索，统一压测方法论，并更新交付文档。

**Architecture:** 分四阶段推进——先固化指标口径与文档（Phase 0），再补全关键 WAN 验证（Phase 1），然后做边界场景实验（Phase 2），最后处理低优先级工具/产品 backlog（Phase 3）。每阶段独立可验收。

**Tech Stack:** bash bench 脚本、tc netem、curl、secnetperf、tcpquic-proxy、research_progress.md 数据记录

---

## 执行状态（2026-06-07）

| 阶段 | 状态 | 提交 |
|------|------|------|
| Phase 0 Task 0 | ✅ 完成 | `9f2812743`, `94de326e4` |
| Phase 1 Task 1–3 | ✅ 完成 | `b39dbfb3a`, `0f2c83c3b`, `1e3b9445a`, `2ba0a43d1` |
| Phase 2 Task 4 | ✅ 完成 | `fd89d1d73` |
| Phase 3 Task A–C | ✅ 完成 | 本 session（未提交） |
| Phase 3 Task D（压缩 profiling / NETEM 矩阵） | ✅ 完成 | 2026-06-07 session |
| Phase 3 Task 5–6 | 暂缓 | — |

**Phase 3 已完成（2026-06-07）：**
- Task A：ASAN smoke PASS（`build-iouring/bin/Debug/tcpquic-proxy`，3 次连续）
- Task B：`bench-tcpquic-proxy-dgx.sh` 默认 `RATE=1gbit`（`NETEM=1` 且未设置时；`RATE=` 可禁用）
- Task C：新增 `scripts/bench-tcpquic-multi-curl.sh`；本地冒烟 `LOCAL=1 PARALLEL_CURL=2` 聚合约 3903 Mbps
- Task D：`bench-tcpquic-multi-curl.sh` 扩展 NETEM/RATE/PAYLOAD_KIND/CPU；`run-compression-profiling.sh`；`run-multi-curl-netem-matrix.sh`；双机 QUICK 矩阵数据写入 `research_progress.md`

**遗留：** secnetperf CLI 对齐（Task 5）；v1 产品功能（Task 6）；高并发 500/1000 隧道（见 `tcpquic_next_steps.md` Phase B）。

---

## Phase 0：基线固化与方法论统一

### Task 0: 指标口径 + bench 脚本 + 文档同步

**Files:**
- Modify: `scripts/bench-tcpquic-proxy-dgx.sh`
- Modify: `docs/superpowers/to_be_fixed.md`
- Modify: `research_progress.md`（顶部增加指标 glossary）
- Modify: `docs/superpowers/delivery.md` §6.3

**背景：** 32 MB @ 100ms 测出 ~494 Mbps 与 128 MB 稳态 ~1352 Mbps 口径不同，需在脚本与文档中显式区分。

- [x] **Step 1: bench 脚本增加 METRIC_MODE**

在 `scripts/bench-tcpquic-proxy-dgx.sh` 环境变量区增加：

```bash
#   METRIC_MODE   steady（DURATION_SEC>0，稳态吞吐）| average（固定 SIZE_MB 整段平均，默认）
#   RATE          可选 netem 带宽上限，如 1gbit（空=不限带宽）
METRIC_MODE="${METRIC_MODE:-average}"
RATE="${RATE:-}"
```

修改 `netem_tc_args()`：当 `RATE` 非空时在 delay/loss 后追加 `rate ${RATE}`。

修改 `append_log()`：在结果表增加行 `| 指标口径 | ${METRIC_MODE} |`；若 `RATE` 非空增加 `| netem rate | ${RATE} |`。

在 `run_mode()` 开头：若 `METRIC_MODE=steady` 且 `DURATION_SEC=0`，自动设 `DURATION_SEC=10` 并 log 提示。

在脚本头部注释增加 sudoers 示例：

```text
# 对端免密 tc（可选）：
#   jack ALL=(ALL) NOPASSWD: /sbin/tc
```

- [x] **Step 2: to_be_fixed.md 增加指标定义表**

在 §2.2 后插入 §2.2.1「压测指标口径」：

| 口径 | 环境变量 | 含义 | 适用场景 |
|------|----------|------|----------|
| `average` | `SIZE_MB=128, DURATION_SEC=0` | curl 整段平均速率 | 对比历史 32MB 数据时注意 BBR 慢启动 |
| `steady` | `DURATION_SEC=10` | 持续下载稳态 | 与 secnetperf `-down:10s` 对齐 |

- [x] **Step 3: research_progress.md 顶部增加指标 glossary**

在文件开头（第一个 `##` 之前）插入简短「指标口径说明」段落，引用上述两种模式及 `METRIC_MODE` 环境变量。

- [x] **Step 4: 更新 delivery.md §6.3**

| 验收项 | 新状态 |
|--------|--------|
| 100ms+5% 丢包稳定工作 | ⏳ Phase 1 背靠背复测待完成 |
| 单连接 ≥100 并发隧道 | ✅ `scripts/test-tcpquic-concurrent.sh` PASS |

- [x] **Step 5: 验证脚本语法**

```bash
bash -n scripts/bench-tcpquic-proxy-dgx.sh
```

Expected: exit 0

- [x] **Step 6: Commit**

```bash
git add scripts/bench-tcpquic-proxy-dgx.sh docs/superpowers/to_be_fixed.md research_progress.md docs/superpowers/delivery.md docs/superpowers/plans/2026-06-07-tcpquic-remaining-work.md
git commit -m "docs: unify tcpquic-proxy benchmark metric modes and delivery status"
```

---

## Phase 1：关键验证补全

### Task 1: LOSS=5% tunnel_off vs secnetperf 背靠背

**Files:**
- Modify: `research_progress.md`（追加实验数据）
- Modify: `docs/superpowers/to_be_fixed.md` §3.4（勾选 A2）

**前置：** 双机 DGX 可达（`ssh jack@169.254.59.196`）；发送端出站网卡由脚本 `resolve_netem_iface` 自动解析。

- [x] **Step 1: 清理端口占用**

```bash
ssh jack@169.254.59.196 'killall -9 secnetperf tcpquic-proxy 2>/dev/null; true'
killall -9 secnetperf tcpquic-proxy 2>/dev/null || true
```

- [x] **Step 2: tunnel_off 丢包压测（3 次）**

```bash
NETEM=1 DELAY=100ms LOSS=5% NETEM_LIMIT=1000000 \
  METRIC_MODE=steady DURATION_SEC=10 SIZE_MB=32 MODES="tunnel_off" ITERATIONS=3 \
  ./scripts/bench-tcpquic-proxy-dgx.sh
```

- [x] **Step 3: secnetperf 同条件基线（3 次）**

对端 netem（脚本已施加）；本机：

```bash
secnetperf -target:169.254.59.196 -bind:169.254.250.230 \
  -exec:maxtput -down:10s -ptput:1 -iosize:1048576 -threads:4 -cc:bbr
```

记录 3 次 kbps，换算 Mbps，写入 `research_progress.md` 表格「LOSS=5% tunnel_off vs secnetperf」。

- [x] **Step 4: 更新 to_be_fixed.md**

勾选 §3.4 A2；在 §2.2 进展表增加丢包行。

- [x] **Step 5: Commit**

```bash
git add research_progress.md docs/superpowers/to_be_fixed.md
git commit -m "bench: LOSS=5% tunnel_off vs secnetperf back-to-back results"
```

### Task 2: netem rate 1gbit 对比 + ASAN 回归

**Files:**
- Modify: `research_progress.md`
- Modify: `docs/superpowers/to_be_fixed.md` §3.4（勾选 A1）

- [x] **Step 1: rate 对比实验**

```bash
# delay-only（已有基线可引用）
NETEM=1 DELAY=100ms LOSS=0% METRIC_MODE=steady DURATION_SEC=10 \
  MODES="tunnel_off" ITERATIONS=2 ./scripts/bench-tcpquic-proxy-dgx.sh

# delay + rate 1gbit
NETEM=1 DELAY=100ms LOSS=0% RATE=1gbit METRIC_MODE=steady DURATION_SEC=10 \
  MODES="tunnel_off" ITERATIONS=2 ./scripts/bench-tcpquic-proxy-dgx.sh
```

对比两次 Mbps，写入 research_progress；勾选 A1。

- [x] **Step 2: ASAN 构建**

```bash
cd build-iouring && cmake .. -DQUIC_BUILD_TOOLS=ON -DQUIC_ENABLE_ASAN=ON && cmake --build . --target tcpquic-proxy
```

- [x] **Step 3: ASAN smoke**（2026-06-07 Phase 3 复核：3 次连续 PASS）

```bash
./scripts/test-tcpquic-proxy.sh
```

Expected: PASS，无 ASAN 报错

- [x] **Step 4: ASAN 100 并发（若可行）**

```bash
./scripts/test-tcpquic-concurrent.sh
```

- [x] **Step 5: Commit**

```bash
git add research_progress.md docs/superpowers/to_be_fixed.md
git commit -m "bench: netem rate comparison; ASAN regression pass"
```

### Task 3: 线程模型后双机 WAN 回归

- [x] **Step 1: 双机稳态基准 3 次**

```bash
NETEM=1 DELAY=100ms LOSS=0% NETEM_LIMIT=1000000 \
  METRIC_MODE=steady DURATION_SEC=10 SIZE_MB=32 MODES="tunnel_off" ITERATIONS=3 \
  ./scripts/bench-tcpquic-proxy-dgx.sh
```

- [x] **Step 2: 对比历史 1352 Mbps 基线**

在 research_progress 记录均值；若低于 1285 Mbps（-5%）则标记回归并调查。

- [x] **Step 3: Commit 数据**

```bash
git add research_progress.md
git commit -m "bench: post-thread-model WAN regression baseline"
```

---

## Phase 2：性能边界探索

### Task 4: 短载荷 / 多连接 / 压缩 WAN 实验矩阵

**Files:**
- Modify: `research_progress.md`
- Modify: `docs/superpowers/to_be_fixed.md` §2.3（更新子目标状态）

- [x] **Step 1: 短载荷矩阵**

`SIZE_MB=8/16/32`，`METRIC_MODE=average`，`NETEM=1 DELAY=100ms LOSS=0%`，`MODES=tunnel_off`，各 2 次。

- [x] **Step 2: 多连接评估**

`QUIC_CONNECTIONS=1/2/4`，128 MB 或 DURATION_SEC=10，4 路并发 curl（可脚本化或手动记录）。

- [x] **Step 3: 压缩 WAN A/B**

`MODES="tunnel_off tunnel_lz4 tunnel_zstd"` × `LOSS=0%/5%`，记录 CPU（`top` 或 `/usr/bin/time`）。

- [x] **Step 4: 更新 to_be_fixed.md §2.3 与 §3.2**

- [x] **Step 5: Commit**

```bash
git add research_progress.md docs/superpowers/to_be_fixed.md
git commit -m "bench: short-payload, multi-conn, compression WAN matrix"
```

---

## Phase 3：低优先级 backlog（按需）

### Task A: ASAN 回归 ✅（2026-06-07）

- [x] `QUIC_ENABLE_ASAN=ON` 构建 `build-iouring/bin/Debug/tcpquic-proxy`
- [x] `test-tcpquic-proxy.sh` 3 次连续 PASS

### Task B: bench 默认 RATE=1gbit ✅（2026-06-07）

- [x] `NETEM=1` 且 `RATE` 未设置时默认 `RATE=1gbit`
- [x] `RATE=` 显式空值不限速；`RATE=<value>` 可覆盖

### Task C: 多 curl 并行压测脚本 ✅（2026-06-07）

- [x] 新增 `scripts/bench-tcpquic-multi-curl.sh`
- [x] 本地冒烟 `LOCAL=1 PARALLEL_CURL=2`：聚合约 3903 Mbps
- [x] 双机直网 multi-curl：`PARALLEL_CURL=1` 4123 vs `PARALLEL_CURL=4` 6837 Mbps
- [x] 双机 `NETEM=1` 多 curl 限速矩阵（`run-multi-curl-netem-matrix.sh` QUICK，2026-06-07）

### Task 5: secnetperf CLI 对齐（可选）

暴露 `-fcw/-srw/-iw/-initrtt` 与 proxy tuning 对齐——仅当 Phase 1–2 完成后启动。

### Task 6: v1 产品功能（可选）

透明迁移、SOCKS/HTTP 认证——见 delivery.md，独立计划。
