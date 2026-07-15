# client tunnel open 测试链接修复实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 Linux `tcpquic_client_tunnel_open_test` 缺少 relay 测试入口定义的链接错误，并将当前 `master` 推送到 `origin/master`。

**Architecture:** 保留现有测试专用入口与生产 relay 路径，只补齐测试目标遗漏的 `tunnel/relay.cpp` 编译单元。先复现链接红灯，再做单行 CMake 修复，最后执行目标测试和完整 Linux 测试验证。

**Tech Stack:** CMake 3.20+、C++17、GNU linker、Git。

## Global Constraints

- 只修改 Linux 的 `TCPQUIC_CLIENT_TUNNEL_OPEN_TEST_SOURCES`。
- 不新增 stub，不重构 relay 实现，不修改 macOS 或 Windows 源文件列表。
- 不暂存或提交工作区已有的无关修改和未跟踪文件。
- 修复提交后将当前 `master` 推送到 `origin/master`，包括此前尚未推送的 libuv 集成提交。

---

### Task 1: 补齐测试目标的 relay 编译单元

**Files:**
- Modify: `src/CMakeLists.txt:984-994`
- Test: `src/unittest/client_tunnel_open_test.cpp`

**Interfaces:**
- Consumes: `tunnel/tcp_tunnel.cpp` 对 `TqRelayStartManagedOnLinuxWorkerForTest(...)` 的调用。
- Produces: 由 `tunnel/relay.cpp` 提供的同签名链接定义。

- [ ] **Step 1: 重新配置并验证链接红灯**

```bash
rtk cmake -S . -B /tmp/tcpquic-proxy-libuv-build -DCMAKE_BUILD_TYPE=Release -DTCPQUIC_ENABLE_CRASHPAD=OFF
rtk cmake --build /tmp/tcpquic-proxy-libuv-build --target tcpquic_client_tunnel_open_test -j2
```

预期：第二条命令失败，错误包含
`undefined reference to TqRelayStartManagedOnLinuxWorkerForTest`。

- [ ] **Step 2: 实施最小修复**

在 `src/CMakeLists.txt` 的 Linux 条件源文件列表中加入：

```cmake
        tunnel/relay.cpp
```

修改后的相关列表必须为：

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TCPQUIC_CLIENT_TUNNEL_OPEN_TEST_SOURCES
        tunnel/server_dial_reactor.cpp
        tunnel/ares_dns_resolver.cpp
        runtime/linux_reactor.cpp
        acl/acl_filter.cpp
        tunnel/relay.cpp
        tunnel/relay_buffer.cpp
        tunnel/relay_alloc.cpp
        tunnel/linux_relay_worker.cpp
    )
endif()
```

- [ ] **Step 3: 验证目标构建转绿**

```bash
rtk cmake -S . -B /tmp/tcpquic-proxy-libuv-build -DCMAKE_BUILD_TYPE=Release -DTCPQUIC_ENABLE_CRASHPAD=OFF
rtk cmake --build /tmp/tcpquic-proxy-libuv-build --target tcpquic_client_tunnel_open_test -j2
```

预期：退出码为 0，输出 `Built target tcpquic_client_tunnel_open_test`。

- [ ] **Step 4: 运行目标测试**

```bash
rtk /tmp/tcpquic-proxy-libuv-build/bin/Release/tcpquic_client_tunnel_open_test
```

预期：退出码为 0，无失败断言。

- [ ] **Step 5: 构建并运行完整 Linux 测试集合**

```bash
rtk cmake --build /tmp/tcpquic-proxy-libuv-build --target all -j2
rtk proxy bash -c 'set -e; count=0; for t in /tmp/tcpquic-proxy-libuv-build/bin/Release/tcpquic_*_test; do "$t"; count=$((count + 1)); done; echo "passed_tests=$count"'
```

预期：完整构建退出码为 0；所有测试退出码为 0，并输出通过的测试数量。

- [ ] **Step 6: 检查并提交修复**

```bash
rtk git diff --check -- src/CMakeLists.txt
rtk git diff -- src/CMakeLists.txt
rtk git add src/CMakeLists.txt
rtk git commit -m "test: link relay implementation into client tunnel test" -- src/CMakeLists.txt
```

预期：提交只包含 `src/CMakeLists.txt` 的 Linux 测试源文件列表变更。

### Task 2: 最终验证并推送

**Files:**
- Verify only: `src/CMakeLists.txt`
- Verify only: `.gitmodules`, `third_party/libuv`, `CMakeLists.txt`, `README.md`, `build.md`

**Interfaces:**
- Consumes: Task 1 的通过状态及当前 `master` 上的 libuv 集成提交。
- Produces: `origin/master` 上可获取的完整提交序列。

- [ ] **Step 1: 运行提交后验证**

```bash
rtk cmake --build /tmp/tcpquic-proxy-libuv-build --target tcpquic_client_tunnel_open_test tcpquic-proxy -j2
rtk /tmp/tcpquic-proxy-libuv-build/bin/Release/tcpquic_client_tunnel_open_test
rtk /tmp/tcpquic-proxy-libuv-build/bin/Release/raypx2 --help
rtk git diff --check
rtk git status -sb
```

预期：两个目标和两个程序均退出 0；本任务文件没有未提交修改；用户原有无关修改保持不变。

- [ ] **Step 2: 推送当前 master**

```bash
rtk git push origin master
```

预期：退出码为 0，远端 `origin/master` 更新到本次修复提交。

- [ ] **Step 3: 验证远端提交**

```bash
rtk git fetch origin master
rtk proxy bash -c 'test "$(git rev-parse HEAD)" = "$(git rev-parse origin/master)"'
rtk git status -sb
```

预期：本地 `HEAD` 与 `origin/master` 相同；用户工作区无关修改仍保持原状。
