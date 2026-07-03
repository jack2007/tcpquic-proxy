# 多网口与热更新高优先级回归 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将同端口多 listen bind plan 和 admin PATCH 修改 `paths` 纳入多路径高优先级回归，避免核心多网口能力回退。

**Architecture:** 回归分三层：本地单元测试锁定配置和运行时语义，脚本 smoke 提供可重复入口，DGX 双机集成脚本保存真实多网口证据。既有 `docs/test/dgx-multi-interface-quic-binding-test-design_cn.md` 保持系统测试设计职责，本文件只描述执行分解。

**Tech Stack:** C++ 单元测试、CMake 构建目标、Bash 集成脚本、Admin HTTP API、DGX 双机 `10.201.1.0/30` 和 `10.201.2.0/30` 数据链路。

---

## 放置决策

- 新增本文档，而不是把执行分解追加到 `docs/test/dgx-multi-interface-quic-binding-execution-20260628_cn.md`：后者是历史执行记录，不适合承载未来计划。
- 不把完整计划直接写入 `docs/test/dgx-multi-interface-quic-binding-test-design_cn.md`：该文档已经负责测试范围、矩阵和门禁；本文作为专项执行计划，由测试设计文档引用。

## 文件边界

- 修改：`docs/test/dgx-multi-interface-quic-binding-test-design_cn.md`
  - 增加本文档索引。
- 修改：`src/unittest/config_router_test.cpp`
  - 强化同端口多 listen bind plan 断言，防止回退到每地址一个 MsQuic listener。
- 修改：`src/unittest/router_runtime_test.cpp`
  - 强化 admin `PATCH /peers/<id>` 修改 `paths` 的运行时重启、drain、配置快照和非法输入回滚断言。
- 创建：`scripts/test-tcpquic-multi-listen-bind-plan.sh`
  - 本地 smoke 入口，执行 bind plan 相关单元测试目标。
- 创建或修改：`scripts/test-tcpquic-multi-interface-paths.sh`
  - 将 admin PATCH paths 单元目标纳入多路径 smoke。
- 创建：`docs/test/dgx-multi-interface-hot-update-runbook_cn.md`
  - 记录 DGX 双机执行步骤、证据目录结构和失败归档要求。

## Task 1: 固化同端口多 listen bind plan 单元回归

**Files:**
- Modify: `src/unittest/config_router_test.cpp`

- [ ] **Step 1: 写失败优先的 bind plan 断言**

在现有 `TqResolveServerListenList` / `TqBuildServerListenerBindList` 测试块附近补齐以下语义，若当前代码已覆盖相同断言，保留并补充注释说明该块是 P1 回归门禁：

```cpp
{
    std::vector<TqResolvedListen> listens;
    std::string err;
    if (!TqResolveServerListenList("10.201.1.2:4433,10.201.2.2:4433", listens, err)) return 1300;
    if (listens.size() != 2) return 1301;

    const std::vector<TqResolvedListen> binds = TqBuildServerListenerBindList(listens);
    if (binds.size() != 1) return 1302;
    if (QuicAddrGetFamily(&binds[0].Address) != QUIC_ADDRESS_FAMILY_INET) return 1303;
    if (QuicAddrGetPort(&binds[0].Address) != 4433) return 1304;

    QUIC_ADDR wildcard{};
    if (!TqMakeQuicAddr(TqEndpoint{"0.0.0.0", 4433}, wildcard)) return 1305;
    if (!QuicAddrCompare(&binds[0].Address, &wildcard)) return 1306;
}
```

- [ ] **Step 2: 验证测试失败或确认已被现有断言覆盖**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j
rtk build/bin/Release/tcpquic_config_router_test
```

Expected:

```text
exit code 0
```

如果修改实现前测试已经通过，需要在提交说明中标明“现有实现已满足，本任务为回归命名和门禁固化”。

- [ ] **Step 3: 若测试失败，修正 bind plan 实现**

只允许触碰以下函数的最小范围：

```text
src/protocol/quic_address.cpp::TqBuildServerListenerBindList
src/protocol/quic_address.cpp::TqServerListenAllowsLocalAddress
```

实现必须保持：

- 同地址族、同端口、多个具体地址合并为一个 wildcard bind。
- 不同端口不合并。
- IPv4 和 IPv6 不合并。
- 原始 resolved listen 仍用于 `TqServerListenAllowsLocalAddress()` 过滤。

- [ ] **Step 4: 重新运行单元目标**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test -j
rtk build/bin/Release/tcpquic_config_router_test
```

Expected:

```text
exit code 0
```

## Task 2: 固化 admin PATCH 修改 paths 的运行时回归

**Files:**
- Modify: `src/unittest/router_runtime_test.cpp`

- [ ] **Step 1: 增强成功路径断言**

在 `PATCH /peers/agent-path-admin` 测试块中确认或补齐以下断言：

```cpp
if (patchResp.find("HTTP/1.1 200 OK") == std::string::npos) return 971;
if (adapter.Stopped.size() != 1 || adapter.Stopped.back() != "agent-path-admin") return 972;
if (adapter.Drained.size() != 1 || adapter.Drained.back() != "agent-path-admin") return 973;
if (adapter.Started.size() != 2 || adapter.LastStartedPeer.QuicPaths.size() != 1) return 974;
if (adapter.LastStartedPeer.QuicPaths[0].Name != "cmcc") return 981;
if (adapter.LastStartedPeer.QuicPaths[0].LocalAddress != "10.0.0.3") return 975;
if (adapter.LastStartedPeer.QuicPaths[0].Peer != "59.1.1.10:443") return 976;
if (adapter.LastStartedPeer.QuicPaths[0].Connections != 3) return 977;
```

- [ ] **Step 2: 增加非法 paths PATCH 回滚断言**

在成功 PATCH 后追加一个非法更新 case，确认配置和 adapter 调用次数不变：

```cpp
const size_t stoppedBeforeBadPatch = adapter.Stopped.size();
const size_t startedBeforeBadPatch = adapter.Started.size();
const TqRouterConfig beforeBadPatch = adminRuntime.SnapshotConfig();
TqHttpRequest badPatch = Request(
    "PATCH",
    "/peers/agent-path-admin",
    "{\"paths\":[{\"name\":\"broken\",\"local\":\"10.0.0.4\",\"peer\":\"59.1.1.10:443\",\"connections\":0}]}");
std::string badPatchResp = adminRuntime.HandleAdmin(badPatch);
if (badPatchResp.find("HTTP/1.1 400 Bad Request") == std::string::npos) return 982;
if (adapter.Stopped.size() != stoppedBeforeBadPatch) return 983;
if (adapter.Started.size() != startedBeforeBadPatch) return 984;
const TqRouterConfig afterBadPatch = adminRuntime.SnapshotConfig();
if (afterBadPatch.Peers.size() != beforeBadPatch.Peers.size()) return 985;
if (afterBadPatch.Peers[0].QuicPaths.size() != beforeBadPatch.Peers[0].QuicPaths.size()) return 986;
if (afterBadPatch.Peers[0].QuicPaths[0].Connections != beforeBadPatch.Peers[0].QuicPaths[0].Connections) return 987;
```

- [ ] **Step 3: 运行 router runtime 单元目标**

Run:

```bash
rtk cmake --build build --target tcpquic_router_runtime_test -j
rtk build/bin/Release/tcpquic_router_runtime_test
```

Expected:

```text
exit code 0
```

## Task 3: 增加本地 smoke 入口

**Files:**
- Create: `scripts/test-tcpquic-multi-listen-bind-plan.sh`
- Modify: `scripts/test-tcpquic-multi-interface-paths.sh`

- [ ] **Step 1: 新增 bind plan smoke 脚本**

创建 `scripts/test-tcpquic-multi-listen-bind-plan.sh`：

```bash
#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${TCPQUIC_CONFIG_ROUTER_TEST_BIN:-}" ]]; then
  CONFIG_ROUTER_TEST="$TCPQUIC_CONFIG_ROUTER_TEST_BIN"
else
  BIN="${TCPQUIC_PROXY_BIN:-$ROOT/build/bin/Release/tcpquic-proxy}"
  BIN_DIR="$(cd "$(dirname "$BIN")" && pwd)"
  CONFIG_ROUTER_TEST="$BIN_DIR/tcpquic_config_router_test"
fi

if [[ ! -x "$CONFIG_ROUTER_TEST" ]]; then
  echo "missing tcpquic_config_router_test binary: $CONFIG_ROUTER_TEST" >&2
  echo "build it with: cmake --build build --target tcpquic_config_router_test -j" >&2
  exit 2
fi

"$CONFIG_ROUTER_TEST"
echo "multi-listen bind plan smoke passed: $CONFIG_ROUTER_TEST"
```

- [ ] **Step 2: 扩展 multi-interface paths smoke**

在 `scripts/test-tcpquic-multi-interface-paths.sh` 中增加 router runtime test 发现和执行逻辑：

```bash
if [[ -n "${TCPQUIC_ROUTER_RUNTIME_TEST_BIN:-}" ]]; then
  ROUTER_RUNTIME_TEST="$TCPQUIC_ROUTER_RUNTIME_TEST_BIN"
else
  ROUTER_RUNTIME_TEST="$BIN_DIR/tcpquic_router_runtime_test"
fi

if [[ ! -x "$ROUTER_RUNTIME_TEST" ]]; then
  echo "missing tcpquic_router_runtime_test binary: $ROUTER_RUNTIME_TEST" >&2
  echo "build it with: cmake --build build --target tcpquic_router_runtime_test -j" >&2
  exit 2
fi

"$ROUTER_RUNTIME_TEST"
```

最终输出改为：

```bash
echo "multi-interface paths smoke passed: generated $tmp/client-paths.json and ran tcpquic_config_router_test + tcpquic_router_runtime_test"
```

- [ ] **Step 3: 运行 smoke**

Run:

```bash
rtk chmod +x scripts/test-tcpquic-multi-listen-bind-plan.sh
rtk cmake --build build --target tcpquic_config_router_test tcpquic_router_runtime_test -j
rtk scripts/test-tcpquic-multi-listen-bind-plan.sh
rtk scripts/test-tcpquic-multi-interface-paths.sh
```

Expected:

```text
multi-listen bind plan smoke passed
multi-interface paths smoke passed
```

## Task 4: 增加 DGX F10 热更新 runbook

**Files:**
- Create: `docs/test/dgx-multi-interface-hot-update-runbook_cn.md`

- [ ] **Step 1: 写入固定前置条件**

runbook 必须声明：

```text
REMOTE_SSH=jack@172.16.10.81
LOCAL_IP_A=10.201.1.1
LOCAL_IP_B=10.201.2.1
REMOTE_IP_A=10.201.1.2
REMOTE_IP_B=10.201.2.2
QUIC_PORT=4433
CLIENT_ADMIN_LISTEN=127.0.0.1:18081
SERVER_ADMIN_LISTEN=127.0.0.1:18081
```

- [ ] **Step 2: 定义 F10 paths PATCH 场景**

F10 必须按顺序执行：

1. 启动 server 显式监听 `10.201.1.2:4433,10.201.2.2:4433`。
2. 启动 client，初始只配置 path-a，等待 `connected=4`。
3. 通过 admin PATCH 把 peer `paths` 改为 path-a + path-b，各 4 条连接。
4. 在 10 秒、20 秒、30 秒采集 client `/api/v1/peers` 和 `/api/v1/connections`。
5. 跑一次 8 并发以上 tunnel 的 iperf3 probe。
6. 通过 admin PATCH 清空 `paths`，回退到 peer list 或默认 peer 语义。
7. 再次采集 admin JSON，确认进程未卡死、admin 未超时。

- [ ] **Step 3: 定义通过标准**

runbook 的通过标准必须包含：

- PATCH 后 30 秒内 path-a/path-b 共 8 条 connection 进入 `connected`。
- admin 请求单次超时不得超过 5 秒。
- PATCH 过程中 client/server 进程不退出。
- iperf3 probe 返回码为 0。
- case 目录保存 PATCH 前、PATCH 后和回退后的 admin JSON。

## Task 5: 将多路径高优先级回归接入人工门禁

**Files:**
- Modify: `docs/test/dgx-multi-interface-quic-binding-test-design_cn.md`

- [ ] **Step 1: 在测试设计中引用专项计划**

在文档开头加入：

```markdown
配套执行计划：

- `docs/test/dgx-multi-interface-hot-update-regression-plan-20260702_cn.md`
```

- [ ] **Step 2: 更新退出条件**

在 `9.2 退出条件` 中增加：

```markdown
- 同端口多 listen bind plan 本地 smoke 已通过。
- admin PATCH 修改 `paths` 的 F10 热更新场景已执行并保存证据。
```

- [ ] **Step 3: 更新发布门禁**

在 `9.3 发布门禁` 中增加阻断项：

```markdown
- 同端口多具体 listen 回退为每地址一个 MsQuic listener。
- admin PATCH 修改 `paths` 后出现连接恢复卡死、admin 超时或配置回滚失败。
```

## Task 6: 最终验证

**Files:**
- Test only

- [ ] **Step 1: 本地构建验证**

Run:

```bash
rtk cmake --build build --target tcpquic_config_router_test tcpquic_router_runtime_test -j
```

Expected:

```text
exit code 0
```

- [ ] **Step 2: 本地回归验证**

Run:

```bash
rtk build/bin/Release/tcpquic_config_router_test
rtk build/bin/Release/tcpquic_router_runtime_test
rtk scripts/test-tcpquic-multi-listen-bind-plan.sh
rtk scripts/test-tcpquic-multi-interface-paths.sh
```

Expected:

```text
exit code 0
```

- [ ] **Step 3: DGX 实机验证**

Run:

```bash
rtk bash docs/dgx-multi-interface-quic-binding-20260628-10net/run-core-10net.sh
rtk bash docs/dgx-multi-interface-quic-binding-20260628-10net/run-dr03-10net.sh
```

Expected:

```text
F01/F04/F05/F08 关键连通 case 通过
DR03 10 轮恢复通过
F10 PATCH paths case 按 runbook 通过
```

如果 DGX 环境不可用，必须在结果摘要中明确写明未执行 DGX 实机验证，并保留本地单元和 smoke 输出。

## 自检清单

- 覆盖 `docs/todo/todo_20260702.md` 第 6 项的两个缺口：同端口多 listen bind plan 集成测试、admin PATCH 修改 `paths` case。
- 计划文档位于 `docs/test/`。
- 历史执行记录不被改写。
- 每个任务都有明确文件、命令和通过标准。
