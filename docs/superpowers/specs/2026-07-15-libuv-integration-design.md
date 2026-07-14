# libuv third_party 静态集成设计

## 目标

将 libuv 作为项目的 Git 子模块放入 `third_party/libuv`，固定到当前最新稳定版本
`v1.52.1`，使用 libuv 官方 CMake 构建静态库，并将其静态链接进主程序
`raypx2`。

集成面向项目现有的 Linux、macOS 和 Windows 平台。本次只在 Linux 上执行构建和
运行验证；macOS 与 Windows 由后续对应平台机器验证。

## 方案选择

采用 libuv 官方 CMake 子工程：根构建通过 `add_subdirectory()` 加入
`third_party/libuv`，并把 libuv 提供的静态目标 `uv_a` 私有链接到
`tcpquic-proxy`。

不采用以下方案：

- 项目自行枚举 libuv 源文件：需要持续跟踪各平台源文件和编译选项，升级成本高。
- 使用 `ExternalProject` 独立构建：会增加多配置生成器、并行构建和跨平台路径处理的
  复杂度，而本项目已有直接加入 vendored CMake 子工程的模式。

## 子模块与版本固定

- 子模块 URL：`https://github.com/libuv/libuv`
- 子模块路径：`third_party/libuv`
- 固定版本：正式发布标签 `v1.52.1` 对应的提交
- `.gitmodules` 使用与现有公开 third_party 依赖一致的 HTTPS URL

主仓库记录确定的 gitlink 提交，因此构建不会自动跟随 libuv 默认分支变化。

## CMake 集成

根 `CMakeLists.txt` 增加缓存路径变量 `TCPQUIC_LIBUV_SOURCE_DIR`，默认指向
`third_party/libuv`。配置时检查该目录是否包含 libuv 的 `CMakeLists.txt`；缺失时
终止配置，并提示执行：

```bash
git submodule update --init --recursive third_party/libuv
```

加入 libuv 子目录前，通过其官方支持的 CMake 选项关闭测试、benchmark、共享库及
本项目不需要的附加构建内容，只保留静态库。选项名称以 `v1.52.1` 的实际 CMake
定义为准，并使用缓存变量，确保 libuv 子工程读取到确定值。

根构建加入 libuv 后应确认静态目标 `uv_a` 存在；若不存在则给出明确配置错误，避免
静默回退到系统 libuv 或共享库。

`src/CMakeLists.txt` 将 `uv_a` 以 `PRIVATE` 方式链接到 `tcpquic-proxy`。这使 libuv
进入最终的 `raypx2`，但不会无条件传播到项目中的全部单元测试目标。当前工作不改写
任何网络或事件循环实现，也不引入新的 libuv 运行时代码调用。

## 跨平台策略

Linux、macOS 和 Windows 均使用 libuv 官方 `v1.52.1` CMake 中的目标和平台判断，
项目不自行维护平台源文件列表或系统库列表。这样 libuv 所需的 pthread、dl、IOCP 等
平台依赖由上游目标传递。

本次验证范围仅为 Linux。macOS 和 Windows 的验收要求保留在文档中，但不将未执行的
平台构建描述为已验证。

## 文档更新

更新 `README.md` 和 `build.md`：

- 将 libuv 加入 vendored 依赖、子模块状态和初始化检查清单。
- 说明 libuv 由 CMake 静态构建并链接进 `raypx2`，无需系统 `libuv` 开发包或运行库。
- 将 libuv 加入构建步骤与 CMake 构建拓扑。
- 记录本次 Linux 验证范围以及 macOS、Windows 待在对应机器验证。

文档默认使用中文，保持项目现有表格和术语风格。

## 验证与验收

Linux 上执行以下验证：

1. `git submodule status third_party/libuv` 显示已初始化，并指向 `v1.52.1` 对应提交。
2. 使用一个不覆盖现有用户构建产物的独立构建目录执行 CMake 配置。
3. 构建目标 `tcpquic-proxy`，确认生成 `raypx2`。
4. 执行 `raypx2 --help`，确认程序可以启动。
5. 使用 `ldd` 检查 `raypx2`，确认没有 `libuv.so` 动态依赖。
6. 检查构建目录中存在 libuv 静态归档产物，并确认链接命令包含该静态库。

若完整构建因本任务以外的已有环境或仓库状态失败，应保留错误证据，区分 libuv 集成
问题与既有问题，不对未通过的检查作完成声明。

## 非目标

- 不在本次工作中把现有 socket、DNS、线程或 reactor 实现迁移到 libuv。
- 不新增基于 libuv 的业务功能或测试程序。
- 不构建或安装 libuv 的共享库、测试、benchmark 或独立安装包。
- 不在本机声称完成 macOS 或 Windows 实机验证。
