# Crashpad Local Dumps Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 集成 Crashpad，让 `raypx2` 崩溃时将 dmp 保存在本地 Crashpad 数据库中。

**Architecture:** `main.cpp` 保持调用 `TqInstallCrashDumpHandler()`；平台层改为 `crash_dump.h` 声明加 `crash_dump.cpp` 实现。CMake 负责可选探测 GN 构建的 Crashpad 产物、链接客户端库、复制 `crashpad_handler`，并把默认构建类型改为 `RelWithDebInfo`。

**Tech Stack:** C++17、CMake、Chromium Crashpad、GN/Ninja 生成的 Crashpad 静态库和 handler。

---

### Task 1: 约束测试

**Files:**
- Modify: `src/unittest/production_linkage_guard_test.cpp`

- [x] **Step 1: Write the failing test**

检查 `src/CMakeLists.txt` 必须包含 `platform/crash_dump.cpp`，并检查 `src/platform/crash_dump.h` 不包含 `MiniDumpWriteDump` 或 `dbghelp`。

- [x] **Step 2: Run test to verify it fails**

Run: `cmake --build build --config Release --target tcpquic_production_linkage_guard_test -j$(nproc) && ./build/bin/Release/tcpquic_production_linkage_guard_test`

Expected: FAIL with exit code 14 before production source is added.

### Task 2: Crashpad CMake 探测

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add build type default**

在根 `CMakeLists.txt` 项目初始化后添加单配置默认 `RelWithDebInfo`。

- [ ] **Step 2: Add Crashpad cache variables**

添加 `TCPQUIC_ENABLE_CRASHPAD`、`TCPQUIC_CRASHPAD_SOURCE_DIR`、`TCPQUIC_CRASHPAD_BUILD_DIR`。

- [ ] **Step 3: Add imported Crashpad target**

检测 `client/crashpad_client.h`、`third_party/mini_chromium/mini_chromium/base/files/file_path.h`、`crashpad_handler`、`obj/client/libclient.a`、`obj/util/libutil.a`、`obj/third_party/mini_chromium/mini_chromium/base/libbase.a`。存在时创建 `tcpquic_crashpad_client` imported interface target。

- [ ] **Step 4: Link and copy handler**

`tcpquic-proxy` 在 `TCPQUIC_HAS_CRASHPAD` 为真时链接 `tcpquic_crashpad_client`，并 POST_BUILD 复制 `crashpad_handler` 到输出目录。

### Task 3: 平台层实现

**Files:**
- Modify: `src/platform/crash_dump.h`
- Create: `src/platform/crash_dump.cpp`

- [ ] **Step 1: Replace header-only Windows implementation**

`crash_dump.h` 只保留 `void TqInstallCrashDumpHandler();` 声明。

- [ ] **Step 2: Implement no-Crashpad fallback**

未定义 `TCPQUIC_HAS_CRASHPAD` 时，`TqInstallCrashDumpHandler()` 只打印 `crashpad disabled` 诊断，不注册旧 handler。

- [ ] **Step 3: Implement Crashpad startup**

启用 Crashpad 时，创建 `<exe_dir>/crashpad` 和 `<exe_dir>/crashpad/metrics`，调用 `crashpad::CrashpadClient::StartHandler()`，annotations 使用 `prod=raypx2`。

### Task 4: 验证

**Files:**
- No additional source files.

- [ ] **Step 1: Run guard test**

Run: `cmake --build build --config Release --target tcpquic_production_linkage_guard_test -j$(nproc) && ./build/bin/Release/tcpquic_production_linkage_guard_test`

Expected: PASS.

- [ ] **Step 2: Build raypx2**

Run: `cmake --build build --config Release --target tcpquic-proxy -j$(nproc)`

Expected: PASS in the current workspace. If Crashpad is absent and mode is AUTO, build should continue with `TCPQUIC_HAS_CRASHPAD=0`.

