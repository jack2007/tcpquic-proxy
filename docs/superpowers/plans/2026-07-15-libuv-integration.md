# libuv 静态集成实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 libuv `v1.52.1` 作为 third_party 子模块，用官方 CMake 构建静态库并链接到主程序 `raypx2`。

**Architecture:** 主仓库用 gitlink 固定 libuv 发布提交，并通过 `add_subdirectory(... EXCLUDE_FROM_ALL)` 使用上游跨平台 CMake。根构建强制关闭 libuv 共享库、测试和 benchmark，只向 `tcpquic-proxy` 私有传递静态目标 `uv_a`。

**Tech Stack:** Git submodule、CMake 3.20+、libuv v1.52.1、C/C++17、Linux ELF 工具。

## Global Constraints

- 子模块路径为 `third_party/libuv`，URL 为 `https://github.com/libuv/libuv`。
- 固定到 `v1.52.1` 的实际提交 `1cfa32ff59c076ffb6ed735bbc8c18361558661f`。
- 静态链接到 `raypx2`，不能产生 `libuv.so` 运行时依赖。
- 沿用 libuv 官方 CMake 覆盖 Linux、macOS 和 Windows，不自行枚举平台源文件或系统库。
- 本次只声明 Linux 实机验证；macOS 和 Windows 留待对应机器验证。
- 不迁移现有 I/O 实现，不新增 libuv 业务调用。
- 文档使用中文；只暂存本计划列出的文件，保留用户已有修改。

## 文件结构

- `.gitmodules`：声明子模块。
- `third_party/libuv`：固定版本 gitlink。
- `CMakeLists.txt`：检查源码、设置上游选项并创建 `uv_a`。
- `src/CMakeLists.txt`：把 `uv_a` 私有链接到主程序。
- `README.md`：更新用户依赖与部署说明。
- `build.md`：更新详细构建流程、拓扑和验证范围。

---

### Task 1: 添加子模块并接入构建

**Files:**
- Modify: `.gitmodules`
- Create: `third_party/libuv`（gitlink）
- Modify: `CMakeLists.txt:16-30,360-376`
- Modify: `src/CMakeLists.txt:213`

**Interfaces:**
- Consumes: libuv 官方目标 `uv_a`。
- Produces: `TCPQUIC_LIBUV_SOURCE_DIR`、根构建目标 `uv_a`、主程序私有链接依赖。

- [ ] **Step 1: 运行集成前断言并确认失败**

```bash
test -f third_party/libuv/CMakeLists.txt && test "$(git -C third_party/libuv rev-parse HEAD)" = "1cfa32ff59c076ffb6ed735bbc8c18361558661f"
```

预期：失败，因为子模块尚未加入。

- [ ] **Step 2: 添加并固定子模块**

```bash
git submodule add https://github.com/libuv/libuv third_party/libuv
git -C third_party/libuv checkout v1.52.1
test "$(git -C third_party/libuv rev-parse HEAD)" = "1cfa32ff59c076ffb6ed735bbc8c18361558661f"
git -C third_party/libuv describe --tags --exact-match
```

预期：检查通过，最后输出 `v1.52.1`。

- [ ] **Step 3: 加入源码缺失检查**

在根 `CMakeLists.txt` 的 third_party 检查区加入：

```cmake
set(TCPQUIC_LIBUV_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libuv" CACHE PATH "libuv source tree")
if(NOT EXISTS "${TCPQUIC_LIBUV_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "libuv submodule missing. Run: git submodule update --init --recursive third_party/libuv")
endif()
```

- [ ] **Step 4: 只创建 libuv 静态目标**

在其他 vendored 子工程附近、`add_subdirectory(src)` 之前加入：

```cmake
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "Build libuv shared library" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "Build libuv tests" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "Build libuv benchmarks" FORCE)
add_subdirectory("${TCPQUIC_LIBUV_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/libuv" EXCLUDE_FROM_ALL)
if(NOT TARGET uv_a)
    message(FATAL_ERROR "Vendored libuv did not define the required static target uv_a")
endif()
```

- [ ] **Step 5: 私有链接到主程序**

把 `src/CMakeLists.txt` 的主程序依赖改为：

```cmake
target_link_libraries(tcpquic-proxy PRIVATE
    libzstd spdlog::spdlog ${TCPQUIC_CARES_TARGET}
    cpp_httplib nlohmann_json::nlohmann_json uv_a)
```

- [ ] **Step 6: 验证静态目标能配置和构建**

```bash
cmake -S . -B /tmp/tcpquic-proxy-libuv-build -DCMAKE_BUILD_TYPE=Release -DTCPQUIC_ENABLE_CRASHPAD=OFF
cmake --build /tmp/tcpquic-proxy-libuv-build --target uv_a -j2
find /tmp/tcpquic-proxy-libuv-build/libuv -type f -name 'libuv_a.a' -print
```

预期：命令均退出 0，并输出静态归档路径。

- [ ] **Step 7: 检查并提交**

```bash
git diff --check -- .gitmodules CMakeLists.txt src/CMakeLists.txt
git add .gitmodules third_party/libuv CMakeLists.txt src/CMakeLists.txt
git commit -m "build: statically link vendored libuv"
```

预期：提交只包含列出的四个路径。

### Task 2: 更新中文文档

**Files:**
- Modify: `README.md:5,38-96`
- Modify: `build.md:38-82,108-163,210-250`

**Interfaces:**
- Consumes: Task 1 的 `third_party/libuv`、`uv_a` 和 `v1.52.1`。
- Produces: 可执行的初始化、构建与部署说明。

- [ ] **Step 1: 运行文档前断言并确认失败**

```bash
rg -n 'third_party/libuv.*v1\.52\.1|uv_a|libuv\.so' README.md build.md
```

预期：失败或未覆盖三项信息。

- [ ] **Step 2: 更新 README**

加入依赖表行：

```markdown
| [libuv](https://github.com/libuv/libuv) | `third_party/libuv` | 跨平台异步 I/O 库（静态链接） |
```

同时把 `libuv 1cfa32ff（v1.52.1）` 加入当前 pin；在无需安装的系统包中加入
`libuv-dev`；说明 `raypx2` 无 `libuv.so` 依赖；说明共享库、测试和 benchmark 被强制关闭。

- [ ] **Step 3: 更新 build.md**

加入依赖表行：

```markdown
| [libuv](https://github.com/libuv/libuv) | `third_party/libuv` | 跨平台异步 I/O 库（静态链接进 `raypx2`） | `1cfa32ff`（v1.52.1） |
```

将 libuv 加入子模块初始化与状态清单、CMake 检查步骤、`uv_a` 构建步骤和构建拓扑；
说明无需系统开发包和共享运行库；注明本次仅完成 Linux 实机验证，macOS/Windows 待验。

- [ ] **Step 4: 验证并提交文档**

```bash
rg -n 'libuv|third_party/libuv|1cfa32ff|uv_a|libuv\.so' README.md build.md
git diff --check -- README.md build.md
git add README.md build.md
git commit -m "docs: document vendored libuv dependency"
```

预期：两份文档覆盖路径、版本、静态链接和平台验证范围；提交只包含两份文档。

### Task 3: Linux 主程序验收

**Files:**
- Verify only: `.gitmodules`, `third_party/libuv`, `CMakeLists.txt`, `src/CMakeLists.txt`, `README.md`, `build.md`

**Interfaces:**
- Consumes: Task 1 的构建集成和 Task 2 的文档。
- Produces: Linux 构建、启动、静态链接和动态依赖证据。

- [ ] **Step 1: 从独立目录重新配置并构建**

```bash
rm -rf /tmp/tcpquic-proxy-libuv-build
cmake -S . -B /tmp/tcpquic-proxy-libuv-build -DCMAKE_BUILD_TYPE=Release -DTCPQUIC_ENABLE_CRASHPAD=OFF
cmake --build /tmp/tcpquic-proxy-libuv-build --target tcpquic-proxy -j2
```

预期：均退出 0，生成 `/tmp/tcpquic-proxy-libuv-build/bin/Release/raypx2`。

- [ ] **Step 2: 验证静态归档参与主程序链接**

```bash
find /tmp/tcpquic-proxy-libuv-build/libuv -type f -name 'libuv_a.a' -print
rg -n 'libuv_a\.a' /tmp/tcpquic-proxy-libuv-build/src/CMakeFiles/tcpquic-proxy.dir/link.txt
```

预期：找到静态归档，并在主程序链接命令中找到它。

- [ ] **Step 3: 验证启动和无共享依赖**

```bash
/tmp/tcpquic-proxy-libuv-build/bin/Release/raypx2 --help
if ldd /tmp/tcpquic-proxy-libuv-build/bin/Release/raypx2 | grep -E 'libuv\.so'; then exit 1; fi
```

预期：`--help` 退出 0；动态依赖检查不输出 `libuv.so`。

- [ ] **Step 4: 最终状态检查**

```bash
git diff --check
git submodule status third_party/libuv
git -C third_party/libuv describe --tags --exact-match
git status --short
```

预期：libuv 为 `1cfa32ff59c076ffb6ed735bbc8c18361558661f` / `v1.52.1`；用户原有无关修改保持不变，本任务文件无未提交修改。
