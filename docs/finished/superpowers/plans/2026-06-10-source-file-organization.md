# Source File Organization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reorganize `src/` source files into responsibility-based subdirectories while preserving Linux and Windows behavior.

**Architecture:** Keep `main.cpp`, `CMakeLists.txt`, `README.md`, and `unittest/` at their current top-level locations under `src/`. Move production source files into `acl/`, `config/`, `protocol/`, `platform/`, `ingress/`, `tunnel/`, `runtime/`, and move source-adjacent markdown files into `src/docs/`. Update `src/CMakeLists.txt` to use subdirectory paths and centralized include directories so existing short include names continue to work.

**Tech Stack:** C++17, CMake 3.16+, msquic, lz4, zstd, POSIX/Linux sockets, Windows Winsock/Schannel or QuicTLS.

**Design Spec:** `docs/superpowers/specs/2026-06-10-source-file-organization-design.md`

---

## File Structure

Production source files after the change:

```text
src/
├── main.cpp
├── acl/acl.cpp
├── acl/acl.h
├── config/config.cpp
├── config/config.h
├── config/tuning.cpp
├── config/tuning.h
├── protocol/compress.cpp
├── protocol/compress.h
├── protocol/control_protocol.cpp
├── protocol/control_protocol.h
├── protocol/quic_session.cpp
├── protocol/quic_session.h
├── platform/platform_socket.h
├── platform/platform_socket_posix.cpp
├── platform/platform_socket_win.cpp
├── platform/quic_credentials_win.cpp
├── platform/quic_credentials_win.h
├── ingress/http_connect_server.cpp
├── ingress/http_connect_server.h
├── ingress/socks5_server.cpp
├── ingress/socks5_server.h
├── tunnel/linux_relay_buffer_pool.cpp
├── tunnel/linux_relay_buffer_pool.h
├── tunnel/linux_relay_worker.cpp
├── tunnel/linux_relay_worker.h
├── tunnel/relay.cpp
├── tunnel/relay.h
├── tunnel/relay_blocking_demo.cpp
├── tunnel/relay_blocking_demo.h
├── tunnel/tcp_dialer.cpp
├── tunnel/tcp_dialer.h
├── tunnel/tcp_tunnel.cpp
├── tunnel/tcp_tunnel.h
├── tunnel/tcp_write_queue.cpp
├── tunnel/tcp_write_queue.h
├── tunnel/tunnel_reaper.cpp
├── tunnel/tunnel_reaper.h
├── tunnel/windows_relay_worker.cpp
├── tunnel/windows_relay_worker.h
├── runtime/admin_http.cpp
├── runtime/admin_http.h
├── runtime/router_runtime.cpp
├── runtime/router_runtime.h
├── runtime/server_metrics.cpp
├── runtime/server_metrics.h
├── runtime/thread_pool.cpp
├── runtime/thread_pool.h
├── runtime/warmup.cpp
├── runtime/warmup.h
├── docs/key_parameter.md
├── docs/thread_model_cn.md
└── unittest/*.cpp
```

Files modified in place:

- Modify: `src/CMakeLists.txt`
- Modify: `src/README.md`
- Modify: `README.md`
- Modify: `src/unittest/acl_test.cpp`
- Modify: `src/unittest/blocking_relay_demo_test.cpp`
- Modify: `src/unittest/compress_test.cpp`
- Modify: `src/unittest/control_protocol_test.cpp`
- Modify: `src/unittest/http_connect_server_test.cpp`
- Modify: `src/unittest/linux_relay_buffer_pool_test.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`
- Modify: `src/unittest/linux_relay_worker_queue_test.cpp`
- Modify: `src/unittest/relay_backend_selection_test.cpp`
- Modify: `src/unittest/socks5_server_test.cpp`
- Modify: `src/unittest/tcp_tunnel_test.cpp`
- Modify: `src/unittest/tcp_write_queue_test.cpp`
- Modify: `src/unittest/thread_pool_test.cpp`
- Modify: `src/unittest/tuning_test.cpp`
- Modify: `src/unittest/tunnel_reaper_test.cpp`

---

### Task 1: Preflight Inventory

**Files:**
- Read: `src/CMakeLists.txt`
- Read: `src/README.md`
- Read: `README.md`
- Read: `src/unittest/*.cpp`

- [ ] **Step 1: Confirm the worktree is clean**

Run:

```bash
rtk git status --short
```

Expected: no output. If there is output, inspect it and do not overwrite unrelated user changes.

- [ ] **Step 2: Capture the current top-level source file list**

Run:

```bash
rtk find src -maxdepth 1 -type f | sort
```

Expected: the output includes `src/main.cpp`, `src/CMakeLists.txt`, `src/README.md`, the current production `.cpp/.h` files, `src/key_parameter.md`, and `src/thread_model_cn.md`.

- [ ] **Step 3: Capture current test include patterns**

Run:

```bash
rtk rg -n '#include \"\\.\\./' src/unittest
```

Expected: output lists the tests that still include production headers with `../`.

- [ ] **Step 4: Stop if the tree contains unrelated changes**

If Step 1 produced output, inspect those files before continuing:

```bash
rtk git diff --stat
```

Expected: either no unrelated changes are present, or execution pauses for the user to decide how to handle them. Do not stage or commit unrelated files as part of this plan.

---

### Task 2: Move Source Files Into Target Directories

**Files:**
- Create: `src/acl/`
- Create: `src/config/`
- Create: `src/protocol/`
- Create: `src/platform/`
- Create: `src/ingress/`
- Create: `src/tunnel/`
- Create: `src/runtime/`
- Create: `src/docs/`
- Move: production source files listed in the file structure section

- [ ] **Step 1: Create destination directories**

Run:

```bash
rtk mkdir -p src/acl src/config src/protocol src/platform src/ingress src/tunnel src/runtime src/docs
```

Expected: command succeeds with no output.

- [ ] **Step 2: Move ACL files**

Run:

```bash
rtk git mv src/acl.cpp src/acl.h src/acl/
```

Expected: `src/acl/acl.cpp` and `src/acl/acl.h` exist.

- [ ] **Step 3: Move config files**

Run:

```bash
rtk git mv src/config.cpp src/config.h src/tuning.cpp src/tuning.h src/config/
```

Expected: `src/config/config.cpp`, `src/config/config.h`, `src/config/tuning.cpp`, and `src/config/tuning.h` exist.

- [ ] **Step 4: Move protocol files**

Run:

```bash
rtk git mv src/compress.cpp src/compress.h src/control_protocol.cpp src/control_protocol.h src/quic_session.cpp src/quic_session.h src/protocol/
```

Expected: `src/protocol/compress.*`, `src/protocol/control_protocol.*`, and `src/protocol/quic_session.*` exist.

- [ ] **Step 5: Move platform files**

Run:

```bash
rtk git mv src/platform_socket.h src/platform_socket_posix.cpp src/platform_socket_win.cpp src/quic_credentials_win.cpp src/quic_credentials_win.h src/platform/
```

Expected: `src/platform/platform_socket*` and `src/platform/quic_credentials_win.*` exist.

- [ ] **Step 6: Move ingress files**

Run:

```bash
rtk git mv src/http_connect_server.cpp src/http_connect_server.h src/socks5_server.cpp src/socks5_server.h src/ingress/
```

Expected: `src/ingress/http_connect_server.*` and `src/ingress/socks5_server.*` exist.

- [ ] **Step 7: Move tunnel files**

Run:

```bash
rtk git mv src/linux_relay_buffer_pool.cpp src/linux_relay_buffer_pool.h src/linux_relay_worker.cpp src/linux_relay_worker.h src/relay.cpp src/relay.h src/relay_blocking_demo.cpp src/relay_blocking_demo.h src/tcp_dialer.cpp src/tcp_dialer.h src/tcp_tunnel.cpp src/tcp_tunnel.h src/tcp_write_queue.cpp src/tcp_write_queue.h src/tunnel_reaper.cpp src/tunnel_reaper.h src/windows_relay_worker.cpp src/windows_relay_worker.h src/tunnel/
```

Expected: all listed relay, tunnel, dialer, queue, reaper, Linux worker, and Windows worker files exist under `src/tunnel/`.

- [ ] **Step 8: Move runtime files**

Run:

```bash
rtk git mv src/admin_http.cpp src/admin_http.h src/router_runtime.cpp src/router_runtime.h src/server_metrics.cpp src/server_metrics.h src/thread_pool.cpp src/thread_pool.h src/warmup.cpp src/warmup.h src/runtime/
```

Expected: all listed runtime files exist under `src/runtime/`.

- [ ] **Step 9: Move source-adjacent docs**

Run:

```bash
rtk git mv src/key_parameter.md src/thread_model_cn.md src/docs/
```

Expected: `src/docs/key_parameter.md` and `src/docs/thread_model_cn.md` exist.

- [ ] **Step 10: Confirm only intended top-level files remain in `src/`**

Run:

```bash
rtk find src -maxdepth 1 -type f | sort
```

Expected output:

```text
src/CMakeLists.txt
src/README.md
src/main.cpp
```

- [ ] **Step 11: Commit the file moves**

Run:

```bash
rtk git add src
rtk git commit -m "refactor: group source files by responsibility"
```

Expected: git creates a commit containing only file moves and no content edits.

---

### Task 3: Centralize Source Include Directories In CMake

**Files:**
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Add a shared include directory list near the top of `src/CMakeLists.txt` after `find_package(Threads REQUIRED)`**

Add this exact block:

```cmake
set(TCPQUIC_SOURCE_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/acl
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/ingress
    ${CMAKE_CURRENT_SOURCE_DIR}/tunnel
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${MSQUIC_SOURCE_DIR}/src/inc
)

function(tcpquic_target_include_dirs target)
    target_include_directories(${target} PRIVATE ${TCPQUIC_SOURCE_INCLUDE_DIRS})
endfunction()
```

- [ ] **Step 2: Replace direct source include directory calls with the helper**

For every target in `src/CMakeLists.txt` that currently has direct source include directories, replace the direct call with `tcpquic_target_include_dirs`. For example, replace this existing pattern on `tcpquic_tuning_test`:

```cmake
target_include_directories(tcpquic_tuning_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
```

with:

```cmake
tcpquic_target_include_dirs(tcpquic_tuning_test)
```

For targets that only need no source headers, such as `tcpquic_production_linkage_guard_test`, do not add the helper unless compilation requires it.

- [ ] **Step 3: Preserve target-specific link libraries and compile definitions**

Do not change any existing `target_link_libraries`, `target_compile_definitions`, `target_compile_options`, `set_property`, `tcpquic_copy_msquic_runtime`, or `target_compile_features` calls.

- [ ] **Step 4: Configure to expose CMake syntax errors**

Run:

```bash
rtk cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

Expected: CMake may fail because source paths still reference old locations. It must not fail with a parse error or unknown command for `tcpquic_target_include_dirs`.

- [ ] **Step 5: Commit CMake include helper**

Run:

```bash
rtk git add src/CMakeLists.txt
rtk git commit -m "build: centralize tcpquic include directories"
```

Expected: git creates a commit containing only the include helper and include call changes.

---

### Task 4: Update CMake Source Paths

**Files:**
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: Update platform source variable**

Replace the existing `TCPQUIC_PLATFORM_SOURCES` block with:

```cmake
set(TCPQUIC_PLATFORM_SOURCES)
if(WIN32)
    list(APPEND TCPQUIC_PLATFORM_SOURCES platform/platform_socket_win.cpp)
else()
    list(APPEND TCPQUIC_PLATFORM_SOURCES platform/platform_socket_posix.cpp)
endif()
```

- [ ] **Step 2: Update `TCPQUIC_PROXY_SOURCES`**

Ensure the source list uses these exact paths:

```cmake
set(TCPQUIC_PROXY_SOURCES
    main.cpp
    config/config.cpp
    config/tuning.cpp
    protocol/control_protocol.cpp
    acl/acl.cpp
    protocol/compress.cpp
    protocol/quic_session.cpp
    tunnel/tcp_dialer.cpp
    tunnel/tcp_tunnel.cpp
    ingress/http_connect_server.cpp
    ingress/socks5_server.cpp
    tunnel/relay.cpp
    runtime/thread_pool.cpp
    tunnel/tunnel_reaper.cpp
    runtime/warmup.cpp
    runtime/admin_http.cpp
    runtime/router_runtime.cpp
    runtime/server_metrics.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
```

- [ ] **Step 3: Update Linux production append block**

Ensure the Linux-only production append block is:

```cmake
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND TCPQUIC_PROXY_SOURCES
        tunnel/linux_relay_buffer_pool.cpp
        tunnel/linux_relay_worker.cpp
    )
endif()
```

- [ ] **Step 4: Update Windows production append block**

Ensure the Windows-only production append block is:

```cmake
if(WIN32)
    list(APPEND TCPQUIC_PROXY_SOURCES
        tunnel/windows_relay_worker.cpp
        platform/quic_credentials_win.cpp
    )
endif()
```

- [ ] **Step 5: Update shared test source variable**

Ensure `TCPQUIC_SERVER_TEST_SOURCES` is:

```cmake
set(TCPQUIC_SERVER_TEST_SOURCES
    runtime/thread_pool.cpp
    tunnel/tcp_dialer.cpp
    config/tuning.cpp
    ${TCPQUIC_PLATFORM_SOURCES}
)
```

- [ ] **Step 6: Update each test target source list**

Replace old production source paths in each `add_executable` and test source variable:

```text
acl.cpp -> acl/acl.cpp
admin_http.cpp -> runtime/admin_http.cpp
compress.cpp -> protocol/compress.cpp
config.cpp -> config/config.cpp
control_protocol.cpp -> protocol/control_protocol.cpp
http_connect_server.cpp -> ingress/http_connect_server.cpp
linux_relay_buffer_pool.cpp -> tunnel/linux_relay_buffer_pool.cpp
linux_relay_worker.cpp -> tunnel/linux_relay_worker.cpp
platform_socket_win.cpp -> platform/platform_socket_win.cpp
relay.cpp -> tunnel/relay.cpp
relay_blocking_demo.cpp -> tunnel/relay_blocking_demo.cpp
router_runtime.cpp -> runtime/router_runtime.cpp
server_metrics.cpp -> runtime/server_metrics.cpp
socks5_server.cpp -> ingress/socks5_server.cpp
tcp_dialer.cpp -> tunnel/tcp_dialer.cpp
tcp_tunnel.cpp -> tunnel/tcp_tunnel.cpp
tcp_write_queue.cpp -> tunnel/tcp_write_queue.cpp
thread_pool.cpp -> runtime/thread_pool.cpp
tuning.cpp -> config/tuning.cpp
tunnel_reaper.cpp -> tunnel/tunnel_reaper.cpp
windows_relay_worker.cpp -> tunnel/windows_relay_worker.cpp
```

- [ ] **Step 7: Configure to verify all CMake source paths exist**

Run:

```bash
rtk cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

Expected: configure succeeds. If it fails with `Cannot find source file`, update the missing path in `src/CMakeLists.txt` and rerun the command.

- [ ] **Step 8: Build the main executable**

Run:

```bash
rtk cmake --build build --target tcpquic-proxy -j$(nproc)
```

Expected: `tcpquic-proxy` builds successfully and `build/bin/Release/tcpquic-proxy` exists.

- [ ] **Step 9: Commit CMake source path changes**

Run:

```bash
rtk git add src/CMakeLists.txt
rtk git commit -m "build: update source paths after reorganization"
```

Expected: git creates a commit containing CMake source path updates.

---

### Task 5: Normalize Unit Test Includes

**Files:**
- Modify: `src/unittest/acl_test.cpp`
- Modify: `src/unittest/blocking_relay_demo_test.cpp`
- Modify: `src/unittest/compress_test.cpp`
- Modify: `src/unittest/control_protocol_test.cpp`
- Modify: `src/unittest/http_connect_server_test.cpp`
- Modify: `src/unittest/linux_relay_buffer_pool_test.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`
- Modify: `src/unittest/linux_relay_worker_queue_test.cpp`
- Modify: `src/unittest/relay_backend_selection_test.cpp`
- Modify: `src/unittest/socks5_server_test.cpp`
- Modify: `src/unittest/tcp_tunnel_test.cpp`
- Modify: `src/unittest/tcp_write_queue_test.cpp`
- Modify: `src/unittest/thread_pool_test.cpp`
- Modify: `src/unittest/tuning_test.cpp`
- Modify: `src/unittest/tunnel_reaper_test.cpp`

- [ ] **Step 1: Replace parent-relative includes with short includes**

Apply these exact include replacements:

```text
#include "../acl.h" -> #include "acl.h"
#include "../compress.h" -> #include "compress.h"
#include "../config.h" -> #include "config.h"
#include "../control_protocol.h" -> #include "control_protocol.h"
#include "../http_connect_server.h" -> #include "http_connect_server.h"
#include "../linux_relay_buffer_pool.h" -> #include "linux_relay_buffer_pool.h"
#include "../linux_relay_worker.h" -> #include "linux_relay_worker.h"
#include "../platform_socket.h" -> #include "platform_socket.h"
#include "../relay.h" -> #include "relay.h"
#include "../relay_blocking_demo.h" -> #include "relay_blocking_demo.h"
#include "../socks5_server.h" -> #include "socks5_server.h"
#include "../tcp_dialer.h" -> #include "tcp_dialer.h"
#include "../tcp_tunnel.h" -> #include "tcp_tunnel.h"
#include "../tcp_write_queue.h" -> #include "tcp_write_queue.h"
#include "../thread_pool.h" -> #include "thread_pool.h"
#include "../tuning.h" -> #include "tuning.h"
#include "../tunnel_reaper.h" -> #include "tunnel_reaper.h"
```

- [ ] **Step 2: Verify no parent-relative production includes remain in tests**

Run:

```bash
rtk rg -n '#include \"\\.\\./' src/unittest
```

Expected: no output.

- [ ] **Step 3: Build all Linux-available unit test targets**

Run:

```bash
rtk cmake --build build --target \
  tcpquic_acl_test \
  tcpquic_admin_http_test \
  tcpquic_blocking_relay_demo_test \
  tcpquic_compress_test \
  tcpquic_config_router_test \
  tcpquic_control_test \
  tcpquic_http_connect_test \
  tcpquic_linux_relay_buffer_pool_test \
  tcpquic_linux_relay_worker_io_test \
  tcpquic_linux_relay_worker_queue_test \
  tcpquic_platform_socket_test \
  tcpquic_production_linkage_guard_test \
  tcpquic_relay_backend_selection_test \
  tcpquic_router_runtime_test \
  tcpquic_socks5_test \
  tcpquic_tcp_write_queue_test \
  tcpquic_thread_pool_test \
  tcpquic_tuning_test \
  tcpquic_tunnel_reaper_test \
  tcpquic_tunnel_test \
  -j$(nproc)
```

Expected: all listed targets build successfully on Linux.

- [ ] **Step 4: Run all built Linux unit tests**

Run:

```bash
for t in build/bin/Release/tcpquic_*_test; do "$t"; done
```

Expected: each test exits with status 0.

- [ ] **Step 5: Commit test include updates**

Run:

```bash
rtk git add src/unittest
rtk git commit -m "test: normalize source includes after reorganization"
```

Expected: git creates a commit containing only test include updates.

---

### Task 6: Update Source Structure Documentation

**Files:**
- Modify: `README.md`
- Modify: `src/README.md`

- [ ] **Step 1: Update root README source structure section**

Replace the old source structure block that starts with `src/tools/tcpquic-proxy/` with this block:

````markdown
## 源码结构

```text
src/
├── main.cpp                  # client / server 入口
├── acl/                      # CIDR ACL + DNS 候选过滤
├── config/                   # CLI 解析与调优参数
├── protocol/                 # OPEN 帧、压缩、QUIC 会话封装
├── platform/                 # POSIX/Windows socket 抽象与 Windows QUIC 凭据
├── ingress/                  # SOCKS5 / HTTP CONNECT 本地入口
├── tunnel/                   # TCP ↔ QUIC Stream 隧道与平台 relay worker
├── runtime/                  # admin、metrics、router runtime、线程池、warmup
├── docs/                     # 与源码实现紧密相关的说明
└── unittest/                 # 单元测试
```
````

- [ ] **Step 2: Update `src/README.md` design link**

Replace:

```markdown
[`docs/superpowers/specs/2026-06-06-tcpquic-proxy-design.md`](../../../docs/superpowers/specs/2026-06-06-tcpquic-proxy-design.md)
```

with:

```markdown
[`docs/specs/2026-06-06-tcpquic-proxy-design.md`](../docs/specs/2026-06-06-tcpquic-proxy-design.md)
```

- [ ] **Step 3: Update `src/README.md` source structure section**

Replace the old `src/tools/tcpquic-proxy/` source tree with the same `src/` source structure block from Step 1.

- [ ] **Step 4: Verify docs no longer describe the old source root**

Run:

```bash
rtk rg -n 'src/tools/tcpquic-proxy' README.md src/README.md
```

Expected: no output.

- [ ] **Step 5: Commit documentation updates**

Run:

```bash
rtk git add README.md src/README.md
rtk git commit -m "docs: update source structure documentation"
```

Expected: git creates a commit containing only documentation edits.

---

### Task 7: Final Verification

**Files:**
- Read: `src/CMakeLists.txt`
- Read: `README.md`
- Read: `src/README.md`

- [ ] **Step 1: Confirm no production `.cpp`, `.h`, or source-adjacent `.md` files remain flat under `src/` except approved files**

Run:

```bash
rtk find src -maxdepth 1 -type f | sort
```

Expected output:

```text
src/CMakeLists.txt
src/README.md
src/main.cpp
```

- [ ] **Step 2: Confirm every CMake source path points to an existing file**

Run:

```bash
rtk cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

Expected: configure succeeds.

- [ ] **Step 3: Rebuild main executable**

Run:

```bash
rtk cmake --build build --target tcpquic-proxy -j$(nproc)
```

Expected: build succeeds.

- [ ] **Step 4: Rebuild all Linux-available unit tests**

Run:

```bash
rtk cmake --build build --target \
  tcpquic_acl_test \
  tcpquic_admin_http_test \
  tcpquic_blocking_relay_demo_test \
  tcpquic_compress_test \
  tcpquic_config_router_test \
  tcpquic_control_test \
  tcpquic_http_connect_test \
  tcpquic_linux_relay_buffer_pool_test \
  tcpquic_linux_relay_worker_io_test \
  tcpquic_linux_relay_worker_queue_test \
  tcpquic_platform_socket_test \
  tcpquic_production_linkage_guard_test \
  tcpquic_relay_backend_selection_test \
  tcpquic_router_runtime_test \
  tcpquic_socks5_test \
  tcpquic_tcp_write_queue_test \
  tcpquic_thread_pool_test \
  tcpquic_tuning_test \
  tcpquic_tunnel_reaper_test \
  tcpquic_tunnel_test \
  -j$(nproc)
```

Expected: all listed targets build successfully.

- [ ] **Step 5: Run unit tests**

Run:

```bash
for t in build/bin/Release/tcpquic_*_test; do "$t"; done
```

Expected: each test exits with status 0.

- [ ] **Step 6: Record Windows verification requirement**

If a Windows machine is available, run:

```powershell
cmake -S . -B build-x64 -A x64 -DLZ4_SOURCE_DIR="$PWD/third_party/lz4"
cmake --build build-x64 --config Release --target tcpquic-proxy tcpquic_platform_socket_test tcpquic_windows_relay_worker_test
.\scripts\test-tcpquic-proxy-windows.ps1 -Compress off
```

Expected: Windows build succeeds and the loopback script passes. If Windows is not available in this session, record that Windows verification remains pending in the final handoff.

- [ ] **Step 7: Confirm git status**

Run:

```bash
rtk git status --short
```

Expected: no output.

---

### Task 8: Post-Implementation Review

**Files:**
- Read: `src/CMakeLists.txt`
- Read: `README.md`
- Read: `src/README.md`

- [ ] **Step 1: Inspect the final diff against the design scope**

Run:

```bash
rtk git show --stat --oneline HEAD~5..HEAD
```

Expected: recent commits contain file moves, CMake path/include updates, test include updates, and docs updates only.

- [ ] **Step 2: Search for old flat source path assumptions**

Run:

```bash
rtk rg -n '(^|[^[:alnum:]_/])(acl|compress|config|control_protocol|http_connect_server|linux_relay|platform_socket|quic_session|relay|router_runtime|server_metrics|socks5_server|tcp_dialer|tcp_tunnel|tcp_write_queue|thread_pool|tuning|tunnel_reaper|warmup|windows_relay_worker)\\.(cpp|h)' CMakeLists.txt src README.md docs scripts
```

Expected: occurrences in `src/CMakeLists.txt` use subdirectory paths, occurrences in `#include` lines use short header names, and docs describe the new directory structure.

- [ ] **Step 3: Ensure no unintended behavior changes were made**

Run:

```bash
rtk git diff HEAD~5..HEAD -- '*.cpp' '*.h'
```

Expected: diffs for production `.cpp/.h` files show file renames only, except test include lines changing from `../header.h` to `header.h`.

- [ ] **Step 4: Leave final implementation summary**

Summarize with actual results from Task 7:

```text
Implemented source organization per docs/superpowers/specs/2026-06-10-source-file-organization-design.md.
Linux verification: cmake configure, tcpquic-proxy build, Linux unit test build, and Linux unit test run all passed.
Windows verification: Windows host was not available in this session, so Windows build and loopback verification remain pending.
Behavior changes: none intended.
```

If Windows verification is run during implementation, replace the Windows line with the exact Windows commands that passed.
