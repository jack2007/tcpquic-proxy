# Crashpad 本地崩溃转储设计

## 目标

为 `raypx2` 集成 Chromium Crashpad。程序发生访问违规、非法指令、abort 等原生崩溃后，自动在本机保存 minidump，用于后续用符号文件分析堆栈。实现必须覆盖 Linux、Windows、macOS 的统一入口，不再保留 Windows 旧 `MiniDumpWriteDump` 实现。

## 架构

`main.cpp` 继续只调用 `TqInstallCrashDumpHandler()`。该函数从头文件声明迁移到 `platform/crash_dump.cpp` 实现，内部在编译期检测 `TCPQUIC_HAS_CRASHPAD`：

- 启用 Crashpad 时，创建 `<raypx2 同目录>/crashpad` 数据库目录和同级 metrics 目录，启动同目录下的 `crashpad_handler`。
- 未启用 Crashpad 时，不注册任何平台私有 fallback，只在 stderr 输出一次诊断。
- `crashpad_handler` 由 CMake 在 Crashpad 可用时复制到 `raypx2` 输出目录。

Crashpad 数据库按官方 handler 的 `--database` 语义管理。dmp 文件不手写命名，使用 Crashpad 数据库默认结构，主要落在 `crashpad/completed/`。

## CMake 集成

构建层增加可选 Crashpad 探测：

- `TCPQUIC_ENABLE_CRASHPAD=AUTO|ON|OFF`，默认 `AUTO`。
- `TCPQUIC_CRASHPAD_SOURCE_DIR` 默认指向 `third_party/crashpad/crashpad`。
- `TCPQUIC_CRASHPAD_BUILD_DIR` 默认指向 `${TCPQUIC_CRASHPAD_SOURCE_DIR}/out/Default`，兼容 GN/Ninja 构建产物。
- 检测到 `client/crashpad_client.h`、mini_chromium 头、`crashpad_handler` 和静态库时，创建 imported target 并给 `tcpquic-proxy` 链接。
- `ON` 但缺失依赖时报 FATAL_ERROR；`AUTO` 缺失时仅禁用 Crashpad。

项目默认单配置构建类型改为 `RelWithDebInfo`，保持 release 优化并产出调试信息。多配置生成器保留现有 `$<CONFIG>` 行为。

## 运行路径

运行时优先通过 `TqGetExecutablePath()` 定位 `raypx2` 所在目录。Crashpad handler 路径固定为同目录：

- Windows: `crashpad_handler.exe`
- 其他平台: `crashpad_handler`

数据库路径为 `<exe_dir>/crashpad`，metrics 路径为 `<exe_dir>/crashpad/metrics`。启动时会创建这些目录。不上报远端，因此 `url` 为空，uploads 保持默认禁用。

## 测试

增加/扩展构建约束测试：

- 生产目标必须包含 `platform/crash_dump.cpp`。
- `crash_dump.h` 不允许再包含 `MiniDumpWriteDump` 或 `dbghelp`。
- CMake 默认构建类型必须包含 `RelWithDebInfo`。

验证命令：

- `cmake --build build --config Release --target tcpquic_production_linkage_guard_test`
- `build/bin/Release/tcpquic_production_linkage_guard_test`
- `cmake --build build --config Release --target tcpquic-proxy`

