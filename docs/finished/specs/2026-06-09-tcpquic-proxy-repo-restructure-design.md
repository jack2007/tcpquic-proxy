# tcpquic-proxy 仓库重组设计

> 日期：2026-06-09  
> 状态：已批准

## 背景

`tcpquic-proxy` 当前作为 [msquic](https://github.com/microsoft/msquic) 仓库内的一个 tool 存在（`src/tools/tcpquic-proxy/`）。为便于独立演进、版本管理与对外发布，将其拆分为顶层仓库，msquic 以 git submodule 方式引入。

## 决策摘要

| 项 | 决策 |
|----|------|
| 顶层仓库范围 | 仅 `tcpquic-proxy` 本体 + scripts + docs |
| wdtq | 留在 `jack2007/msquic` fork，通过 `TCPQUIC_PROXY_BIN` 引用二进制 |
| msquic submodule | `https://github.com/microsoft/msquic`（上游官方） |
| 本地路径 | `/home/jack/src/tcpquic-proxy` |
| 构建方式 | Superbuild：`add_subdirectory(third_party/msquic)` + 独立 `src/` 目标 |

## 目标目录结构

```
/home/jack/src/tcpquic-proxy/
├── .gitignore
├── .gitmodules
├── CMakeLists.txt
├── README.md
├── third_party/
│   └── msquic/              # submodule → microsoft/msquic
├── src/
│   ├── CMakeLists.txt
│   ├── *.cpp / *.h
│   └── unittest/
├── scripts/
│   ├── test-tcpquic-proxy.sh
│   ├── test-tcpquic-concurrent.sh
│   ├── test-tcpquic-proxy-dgx.sh
│   ├── bench-tcpquic-proxy.sh
│   ├── bench-tcpquic-proxy-dgx.sh
│   └── bench-tcpquic-multi-curl.sh
├── docs/
│   ├── specs/
│   ├── plans/
│   └── tcpquic_next_steps.md
└── build/                   # gitignored
```

## 构建架构

### Superbuild 流程

1. 顶层 `CMakeLists.txt` 声明 `project(tcpquic-proxy)`。
2. `add_subdirectory(third_party/msquic)` 构建 msquic 核心库与内部目标（`msquic`、`inc`、`warnings`、`logging` 等）。
3. 设置 `QUIC_BUILD_TOOLS=OFF`，不编译 msquic 自带 tools。
4. `add_subdirectory(src)` 编译 `tcpquic-proxy` 及单元测试目标。

### `add_quic_tool` 替代

原 msquic 内 `add_quic_tool()` 链接：

- `inc`、`warnings`、`msquic`
- 条件：`msquic_platform`（`BUILD_SHARED_LIBS`）
- `logging`、`base_link`

新仓库在 `src/CMakeLists.txt` 用显式 `add_executable` + `target_link_libraries` 复刻上述链接关系。

### Include 路径

所有 `${CMAKE_SOURCE_DIR}/src/inc` 改为 `${MSQUIC_SOURCE_DIR}/src/inc`，其中：

```cmake
set(MSQUIC_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/msquic)
```

### 产物路径

```text
build/bin/Release/tcpquic-proxy
build/bin/Release/tcpquic_*_test
```

## 迁移范围

### 迁入新仓库

| 来源（msquic 仓库） | 目标 |
|---------------------|------|
| `src/tools/tcpquic-proxy/*` | `src/` |
| `scripts/*tcpquic*` | `scripts/` |
| `docs/superpowers/specs/2026-06-06-tcpquic-proxy-design.md` | `docs/specs/` |
| `docs/superpowers/plans/2026-06-06-tcpquic-proxy.md` | `docs/plans/` |
| `docs/superpowers/plans/2026-06-06-tcpquic-adaptive-tuning.md` | `docs/plans/` |
| `docs/superpowers/plans/2026-06-06-tcpquic-thread-model.md` | `docs/plans/` |
| `docs/superpowers/plans/2026-06-06-tcpquic-thread-model_review.md` | `docs/plans/` |
| `docs/superpowers/plans/2026-06-07-tcpquic-remaining-work.md` | `docs/plans/` |
| `docs/superpowers/tcpquic_next_steps.md` | `docs/` |

### 不迁移

| 路径 | 原因 |
|------|------|
| `src/tools/wdtq/` | wdtq 留在 msquic fork |
| `docs/superpowers/specs/2026-06-07-wdt-tcpquic-agent-design.md` | wdtq 相关 |
| msquic fork 中其他 wdtq / wdt 改动 | 独立产品线 |

## 脚本适配

脚本 `ROOT_DIR` 指向仓库根目录。二进制路径统一为：

```bash
BIN="$ROOT_DIR/build/bin/Release/tcpquic-proxy"
```

不再引用 `build-iouring/`。

## wdtq 集成

wdtq 留在 `jack2007/msquic`，通过环境变量指向新仓库产物：

```bash
export TCPQUIC_PROXY_BIN=/home/jack/src/tcpquic-proxy/build/bin/Release/tcpquic-proxy
```

在 README 中记录此约定。

## msquic Submodule 版本策略

- 初始 pin 到上游稳定 tag `v2.6.0`（与 msquic `QUIC_FULL_VERSION` 对齐）。
- 若 tag 不可用，pin 到验证过的 commit 并在 README 注明。
- 升级流程：`cd third_party/msquic && git fetch --tags && git checkout vX.Y.Z && cd ../.. && git add third_party/msquic`

## 原 msquic fork 处理（本次不执行）

迁移验证通过后，可选地从 `jack2007/msquic` 删除 `src/tools/tcpquic-proxy/` 及 `src/tools/CMakeLists.txt` 中对应 `add_subdirectory`。本次重组**不自动修改** msquic fork，避免影响 wdtq 开发。

## 验证标准

1. `git submodule update --init --recursive` 成功。
2. `cmake .. && cmake --build . --target tcpquic-proxy` 成功。
3. 全部 `tcpquic_*_test` 目标编译并通过。
4. `./scripts/test-tcpquic-proxy.sh` 通过。
5. wdtq 可通过 `TCPQUIC_PROXY_BIN` 找到新二进制（文档验证即可）。

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| msquic 作为 subdirectory 的 CMake 兼容性 | 使用独立 `build/` 目录；首次全量验证 |
| 上游 msquic API 变更 | submodule pin tag；升级时跑全量测试 |
| fork 中 msquic 补丁未在上游 | 记录差异；必要时以 patch 或 fork submodule 回退 |
