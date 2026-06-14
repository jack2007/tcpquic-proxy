# tcpquic-proxy 仓库重组实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 `tcpquic-proxy` 从 msquic 内嵌 tool 拆分为独立顶层仓库 `/home/jack/src/tcpquic-proxy`，以 `microsoft/msquic` 为 git submodule。

**Architecture:** Superbuild — 顶层 CMake `add_subdirectory(third_party/msquic)` 构建 msquic 库，`src/CMakeLists.txt` 显式链接 `msquic`/`inc`/`logging` 等目标，脚本与文档随源码一并迁移。

**Tech Stack:** C++17、CMake 3.16+、msquic、libzstd、liblz4（可选）、bash 测试脚本

**设计规格:** `docs/specs/2026-06-09-tcpquic-proxy-repo-restructure-design.md`

---

### Task 1: 初始化仓库与 submodule

**Files:**
- Create: `/home/jack/src/tcpquic-proxy/.gitignore`
- Create: `/home/jack/src/tcpquic-proxy/.gitmodules`

- [ ] **Step 1: 创建目录并 git init**

```bash
mkdir -p /home/jack/src/tcpquic-proxy
cd /home/jack/src/tcpquic-proxy
git init
```

- [ ] **Step 2: 添加 .gitignore**

```gitignore
build/
.cache/
*.o
*.a
*.so
.DS_Store
```

- [ ] **Step 3: 添加 msquic submodule**

```bash
cd /home/jack/src/tcpquic-proxy
git submodule add https://github.com/microsoft/msquic.git third_party/msquic
cd third_party/msquic
git checkout v2.6.0
cd /home/jack/src/tcpquic-proxy
```

若 `v2.6.0` tag 不存在，改用：

```bash
git fetch --tags
git tag -l 'v2.*' | tail -5   # 选最新稳定 tag
git checkout <tag>
```

- [ ] **Step 4: 记录 submodule pin**

```bash
git add .gitmodules third_party/msquic
```

---

### Task 2: 迁移源码

**Files:**
- Create: `/home/jack/src/tcpquic-proxy/src/**`（从 msquic 复制）

- [ ] **Step 1: 复制源码与单元测试**

```bash
SRC_MSQUIC=/home/jack/src/msquic/src/tools/tcpquic-proxy
DST=/home/jack/src/tcpquic-proxy/src
mkdir -p "$DST"
rsync -a --exclude='CMakeLists.txt' "$SRC_MSQUIC/" "$DST/"
```

- [ ] **Step 2: 确认文件清单**

应包含：`main.cpp`、`config.*`、`quic_session.*`、`unittest/` 等约 50 个文件；`thread_model_cn.md`、`key_parameter.md` 保留在 `src/` 或移至 `docs/`（保持 `src/` 亦可）。

---

### Task 3: 顶层 CMakeLists.txt

**Files:**
- Create: `/home/jack/src/tcpquic-proxy/CMakeLists.txt`

- [ ] **Step 1: 写入顶层 CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.16)
project(tcpquic-proxy C CXX)

set(MSQUIC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/msquic" CACHE PATH "msquic source tree")

if(NOT EXISTS "${MSQUIC_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "msquic submodule missing. Run: git submodule update --init --recursive")
endif()

set(QUIC_BUILD_TOOLS OFF CACHE BOOL "Build msquic tools" FORCE)
set(QUIC_ENABLE_LOGGING ON CACHE BOOL "" FORCE)

add_subdirectory("${MSQUIC_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/msquic")

add_subdirectory(src)
```

- [ ] **Step 2: 验证 submodule 可被 CMake 识别**

```bash
cd /home/jack/src/tcpquic-proxy
mkdir -p build && cd build
cmake .. 2>&1 | head -30
```

预期：CMake 配置成功或仅因 `src/CMakeLists.txt` 缺失而失败（Task 4 解决）。

---

### Task 4: 改编 src/CMakeLists.txt

**Files:**
- Create: `/home/jack/src/tcpquic-proxy/src/CMakeLists.txt`

- [ ] **Step 1: 定义链接辅助函数（替代 add_quic_tool）**

在 `src/CMakeLists.txt` 顶部添加：

```cmake
function(add_tcpquic_executable target)
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE inc warnings msquic logging base_link)
    if(BUILD_SHARED_LIBS)
        target_link_libraries(${target} PRIVATE msquic_platform)
    endif()
    set_property(TARGET ${target} PROPERTY FOLDER "tools")
    set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
endfunction()
```

- [ ] **Step 2: 从原 CMakeLists.txt 迁移，替换以下内容**

| 原写法 | 新写法 |
|--------|--------|
| `add_quic_tool(tcpquic-proxy ...)` | `add_tcpquic_executable(tcpquic-proxy ...)` |
| `${CMAKE_SOURCE_DIR}/src/inc` | `${MSQUIC_SOURCE_DIR}/src/inc` |
| `${QUIC_FOLDER_PREFIX}tools` | `"tools"`（或保留变量若 msquic 已定义） |

- [ ] **Step 3: 保留 pkg-config 依赖与全部测试目标**

原 `src/tools/tcpquic-proxy/CMakeLists.txt` 中 `find_package(PkgConfig)`、`libzstd`、`liblz4` 及全部 `tcpquic_*_test` 目标原样迁移，仅做上述替换。

---

### Task 5: 迁移脚本并修正路径

**Files:**
- Create: `/home/jack/src/tcpquic-proxy/scripts/*.sh`

- [ ] **Step 1: 复制脚本**

```bash
cp /home/jack/src/msquic/scripts/*tcpquic* /home/jack/src/tcpquic-proxy/scripts/
chmod +x /home/jack/src/tcpquic-proxy/scripts/*.sh
```

- [ ] **Step 2: 批量替换二进制路径**

在每个脚本中将：

```bash
BIN="$ROOT_DIR/build-iouring/bin/Release/tcpquic-proxy"
```

改为：

```bash
BIN="$ROOT_DIR/build/bin/Release/tcpquic-proxy"
```

并检查脚本内其他 `build-iouring` 引用一并替换为 `build`。

- [ ] **Step 3: 确认 ROOT_DIR 仍正确**

`ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"` 应指向 `/home/jack/src/tcpquic-proxy`。

---

### Task 6: 迁移文档

**Files:**
- Create: `/home/jack/src/tcpquic-proxy/docs/**`

- [ ] **Step 1: 复制 tcpquic 相关文档**

```bash
MSQUIC=/home/jack/src/msquic
DST=/home/jack/src/tcpquic-proxy/docs
mkdir -p "$DST/specs" "$DST/plans"

cp "$MSQUIC/docs/superpowers/specs/2026-06-06-tcpquic-proxy-design.md" "$DST/specs/"
cp "$MSQUIC/docs/superpowers/plans/2026-06-06-tcpquic-proxy.md" "$DST/plans/"
cp "$MSQUIC/docs/superpowers/plans/2026-06-06-tcpquic-adaptive-tuning.md" "$DST/plans/"
cp "$MSQUIC/docs/superpowers/plans/2026-06-06-tcpquic-thread-model.md" "$DST/plans/"
cp "$MSQUIC/docs/superpowers/plans/2026-06-06-tcpquic-thread-model_review.md" "$DST/plans/"
cp "$MSQUIC/docs/superpowers/plans/2026-06-07-tcpquic-remaining-work.md" "$DST/plans/"
cp "$MSQUIC/docs/superpowers/tcpquic_next_steps.md" "$DST/"
cp "$MSQUIC/docs/specs/2026-06-09-tcpquic-proxy-repo-restructure-design.md" "$DST/specs/" 2>/dev/null || true
```

- [ ] **Step 2: 更新 README 内文档链接**

将 `../../../docs/superpowers/...` 改为 `docs/specs/...` 或 `docs/plans/...`。

---

### Task 7: 编写 README.md

**Files:**
- Create: `/home/jack/src/tcpquic-proxy/README.md`

- [ ] **Step 1: 基于原 README 改写**

要点：
- 说明本仓库独立存在，msquic 为 `third_party/msquic` submodule
- 构建命令改为本仓库 `build/` 目录
- 增加 submodule 初始化步骤
- 增加 wdtq 集成说明（`TCPQUIC_PROXY_BIN`）
- 更新文档链接路径

```bash
git clone --recursive git@github.com:jack2007/tcpquic-proxy.git   # 远端待创建
cd tcpquic-proxy
git submodule update --init --recursive
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target tcpquic-proxy -j$(nproc)
```

---

### Task 8: 构建验证

- [ ] **Step 1: 全量配置与编译**

```bash
cd /home/jack/src/tcpquic-proxy/build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target tcpquic-proxy -j$(nproc)
```

预期：`build/bin/Release/tcpquic-proxy` 存在且可执行。

- [ ] **Step 2: 编译单元测试**

```bash
cmake --build . --target tcpquic_control_test tcpquic_acl_test \
  tcpquic_compress_test tcpquic_tunnel_test \
  tcpquic_socks5_test tcpquic_http_connect_test \
  tcpquic_tuning_test tcpquic_config_router_test \
  tcpquic_admin_http_test tcpquic_router_runtime_test -j$(nproc)
```

- [ ] **Step 3: 运行单元测试**

```bash
cd /home/jack/src/tcpquic-proxy/build
for t in bin/Release/tcpquic_*_test; do echo "==> $t"; "$t" || exit 1; done
```

预期：全部 PASS。

---

### Task 9: 集成测试

- [ ] **Step 1: 运行主集成测试脚本**

```bash
cd /home/jack/src/tcpquic-proxy
./scripts/test-tcpquic-proxy.sh
```

预期：`[tcpquic-test]` 日志无失败退出。

- [ ] **Step 2: 记录 wdtq 对接说明**

在 README 添加：

```bash
export TCPQUIC_PROXY_BIN=/home/jack/src/tcpquic-proxy/build/bin/Release/tcpquic-proxy
```

---

### Task 10: 初始提交

- [ ] **Step 1: 暂存并提交**

```bash
cd /home/jack/src/tcpquic-proxy
git add .
git status
git commit -m "$(cat <<'EOF'
chore: bootstrap standalone tcpquic-proxy repository

Extract tcpquic-proxy from msquic tree with microsoft/msquic as submodule,
superbuild CMake, migrated scripts/docs, and integration test path updates.
EOF
)"
```

- [ ] **Step 2: （可选）创建 GitHub 远端并推送**

```bash
gh repo create jack2007/tcpquic-proxy --private --source=. --remote=origin
git push -u origin main
```

---

## 不在本次范围

- 修改 `jack2007/msquic` fork（删除旧 `src/tools/tcpquic-proxy/`）
- 迁移 wdtq 代码
- 向上游 microsoft/msquic 提交 tcpquic-proxy 回归
