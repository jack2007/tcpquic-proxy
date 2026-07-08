# Crashpad 构建依赖说明

本文说明 tcpquic-proxy 集成 Crashpad 时依赖哪些第三方代码和本机工具，以及 Linux、Windows、macOS 三个平台应如何保持同一套提交和构建策略。

## 1. 当前集成结论

Crashpad 作为主仓库子模块放在 `third_party/crashpad`。主仓库 `.gitmodules` 指向项目维护的 fork：

```text
git@github.com:jack2007/crashpad.git
```

主仓库当前记录的 Crashpad 子模块提交为：

```text
df2c143e5cd9a6c0910814806a54a257696bb2ab
```

这个 fork 的目标不是改写 Crashpad 架构，而是把 tcpquic-proxy 构建 Crashpad 所需的 `DEPS` 代码转为普通 Git 源码提交。这样从 GitHub clone 主仓库后，只需要执行子模块初始化即可获得 Crashpad 构建所需代码，不需要在 CMake 配置阶段再执行 `gclient sync`、下载 `depot_tools` 或从 Chromium 源站拉依赖。

## 2. 提交边界

Crashpad 依赖采用两层提交边界。

第一层是 Crashpad fork：

- 仓库：`git@github.com:jack2007/crashpad.git`
- 分支：`main`
- 关键提交：
  - `eb0d842e vendor crashpad deps for offline builds`
  - `df2c143e fix linux embedded zlib build`

这一层提交 Crashpad 自身和它构建需要的第三方源码。新增或修复平台依赖时，优先在这个 fork 里提交。

第二层是 tcpquic-proxy 主仓库：

- 仓库：`git@github.com:jack2007/tcpquic-proxy.git`
- 分支：`master`
- 关键提交：
  - `ab85060 build: vendor crashpad build dependencies`
  - `8c0d1f2 build: use vendored crashpad dependencies`

这一层只记录子模块指针、CMake 集成逻辑和文档。主仓库不直接复制 Crashpad 依赖源码，也不保留会联网拉 Chromium 依赖的 bootstrap 逻辑。

## 3. 已 vendored 的 Crashpad 代码依赖

Crashpad fork 中已经包含下列构建相关第三方目录。它们应作为普通文件存在于 `jack2007/crashpad` fork 中，而不是嵌套 `.git` 仓库或 gitlink。

| 目录 | 作用 | 平台说明 |
|------|------|----------|
| `third_party/mini_chromium/mini_chromium` | Crashpad 基础库实现，提供 `base`、路径、日志、同步、字符串等基础设施 | 三个平台都需要 |
| `third_party/zlib/zlib` | Crashpad gzip/http body、minidump 工具等使用的 zlib | 三个平台都需要；Linux 当前使用 embedded zlib |
| `third_party/lss/lss` | Linux syscall support | Linux 需要 |
| `third_party/googletest/googletest` | Crashpad 测试和部分 GN 目标依赖 | 构建/测试相关，跨平台保留 |
| `third_party/getopt` | Windows/POSIX 参数解析兼容代码 | Windows 构建可能需要 |
| `third_party/xnu` | macOS Mach/XNU 相关头文件 | macOS 构建需要 |
| `third_party/cpp-httplib/cpp-httplib` | Crashpad HTTP 传输相关第三方代码 | 启用相关传输目标时使用 |
| `third_party/ninja/ninja` | Crashpad DEPS 中记录的 Ninja 工具副本 | 可作为参考或备用；主仓库当前仍优先查找本机 `ninja` |
| `third_party/edo`、`third_party/fuchsia`、`third_party/linux` | Crashpad 原有第三方占位/平台描述目录 | 保持与 Crashpad GN 文件兼容 |

维护原则：

- 如果 Windows 或 macOS 后续发现缺少 Crashpad `DEPS` 目录，应先补到 `jack2007/crashpad` fork。
- 补依赖时不要提交依赖目录下的 `.git`，也不要把它作为嵌套子模块。
- 主仓库只更新 `third_party/crashpad` 的 gitlink 到新的 fork commit。

## 4. 本机工具依赖

vendored 源码只解决“代码从哪里来”的问题。本机仍需要平台工具链和构建工具。

| 工具 | 用途 | Linux | Windows | macOS |
|------|------|-------|---------|-------|
| Git | 拉取主仓库和子模块 | 需要 | 需要 | 需要 |
| CMake | 配置 tcpquic-proxy 和 vendored 库 | 需要 | 需要 | 需要 |
| C/C++ 编译器 | 编译 tcpquic-proxy、msquic、Crashpad | GCC/Clang | MSVC / Visual Studio 2022 | Apple Clang / Xcode Command Line Tools |
| Perl、make | 构建 vendored quictls/OpenSSL | 需要 | Strawberry Perl；make 由相关环境提供 | 需要 |
| GN | 生成 Crashpad Ninja build files | 启用 Crashpad 时需要 | 启用 Crashpad 时需要 | 启用 Crashpad 时需要 |
| Ninja | 构建 Crashpad 静态库和 handler | 启用 Crashpad 时需要 | 启用 Crashpad 时需要 | 启用 Crashpad 时需要 |
| Python | 部分 GN/平台脚本可能使用 | 建议安装 | 建议安装 | Xcode/系统通常提供，仍建议确认 |

不属于本方案的依赖：

- 不要求安装 `depot_tools`。
- 不在 CMake 中执行 `gclient sync`。
- 不在 CMake 中 `git clone` Chromium 或 Crashpad DEPS。
- 不要求安装系统 `libzstd-dev`、`libssl-dev`、`libmsquic-dev` 等库来替代 vendored 代码。

## 5. CMake 集成逻辑

主仓库通过以下选项控制 Crashpad：

| 选项 | 默认值 | 含义 |
|------|--------|------|
| `TCPQUIC_ENABLE_CRASHPAD` | `AUTO` | `AUTO` 自动检测并启用；`ON` 要求必须可构建/可检测；`OFF` 禁用 |
| `TCPQUIC_CRASHPAD_AUTO_BUILD` | `ON` | 缺少 Crashpad 产物时，用本机 `gn`/`ninja` 从 vendored 源码构建 |
| `TCPQUIC_CRASHPAD_SOURCE_DIR` | `third_party/crashpad` | Crashpad 子模块源码目录 |
| `TCPQUIC_CRASHPAD_BUILD_DIR` | `third_party/crashpad/out/Default` | Crashpad GN/Ninja 输出目录 |

CMake 检查以下产物：

- `client/crashpad_client.h`
- `third_party/mini_chromium/mini_chromium/base/files/file_path.h`
- `crashpad_handler`，Windows 为 `crashpad_handler.exe`
- `obj/client/libclient.a` 或同等 library 名称
- `obj/util/libutil.a`
- `obj/third_party/mini_chromium/mini_chromium/base/libbase.a`
- macOS 额外检查 `obj/util/libmig_output.a`

缺少产物且 `TCPQUIC_CRASHPAD_AUTO_BUILD=ON` 时，CMake 调用：

```text
gn gen <TCPQUIC_CRASHPAD_BUILD_DIR> --root=<TCPQUIC_CRASHPAD_SOURCE_DIR> --args=is_debug=false crashpad_http_transport_impl="socket"
ninja -C <TCPQUIC_CRASHPAD_BUILD_DIR> crashpad_handler client:client util:util third_party/mini_chromium:base
```

macOS 还会追加：

```text
util:mig_output
```

构建成功后，主目标会链接 Crashpad client/util/base，并把 `crashpad_handler` 复制到 `raypx2` 输出目录。

## 6. Linux 构建依赖

Linux 使用 fork 中 vendored 的 `lss`、`mini_chromium`、`zlib`、`googletest`。当前 Crashpad fork 对 Linux 做了两个关键处理：

1. Linux standalone Crashpad 使用 embedded zlib，而不是系统 `libz`。
2. embedded zlib 的 GN 规则补齐 `cpu_features.c`、x86 SIMD/ARM Linux 宏等必要构建项。

因此 Linux 启用 Crashpad 后，`crashpad_handler` 不需要依赖系统 `libz.so`。验证方式：

```bash
ldd <crashpad_build_dir>/crashpad_handler
```

预期只看到标准 C/C++ 运行库等系统基础库，不应出现 `libz.so`。

Linux 示例：

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTCPQUIC_ENABLE_CRASHPAD=ON
cmake --build build --target tcpquic-proxy -j$(nproc)
```

## 7. Windows 构建依赖

Windows 使用同一套 Crashpad fork 和子模块指针。平台差异只体现在本机工具链、handler 文件名和系统库链接。

Windows 需要：

- Visual Studio 2022 / MSVC C++ 工作负载
- CMake
- Git
- Strawberry Perl
- GN
- Ninja
- Python、curl、OpenSSL 等项目 Windows 构建文档中列出的辅助工具

CMake 行为：

- handler 名称为 `crashpad_handler.exe`。
- Crashpad target 仍由本机 `gn`/`ninja` 从 `third_party/crashpad` 构建。
- tcpquic-proxy 链接 Crashpad 时额外链接 Windows 系统库：`user32`、`version`、`winhttp`。
- 主项目仍强制使用 msquic `quictls` 后端，不使用 Schannel。

Windows 示例：

```powershell
git submodule update --init --recursive
cmake -S . -B build-x64 -A x64 -DTCPQUIC_ENABLE_CRASHPAD=ON
cmake --build build-x64 --config Release --target tcpquic-proxy
```

部署时需要把 `crashpad_handler.exe` 与 `raypx2.exe` 放在同一目录。

## 8. macOS 构建依赖

macOS 同样使用 `jack2007/crashpad` fork 中 vendored 的 DEPS。平台差异主要是 Mach/MIG 相关产物和系统 framework 链接。

macOS 需要：

- Xcode 或 Xcode Command Line Tools
- CMake
- Git
- Perl、make
- GN
- Ninja
- Python

CMake 行为：

- handler 名称为 `crashpad_handler`。
- 自动构建 target 在通用 Crashpad 目标之外追加 `util:mig_output`。
- 检测 Crashpad 可用时，除 client/util/base 外，还要求 `mig_output` library 存在。
- tcpquic-proxy 链接 Crashpad 时额外链接：
  - `ApplicationServices`
  - `CoreFoundation`
  - `CoreGraphics`
  - `CoreText`
  - `Foundation`
  - `IOKit`
  - `bsm`

macOS 示例：

```bash
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DTCPQUIC_ENABLE_CRASHPAD=ON
cmake --build build --target tcpquic-proxy -j$(sysctl -n hw.ncpu)
```

部署时需要把 `crashpad_handler` 与 `raypx2` 放在同一目录。

## 9. clone 后的推荐流程

首次获取完整源码：

```bash
git clone git@github.com:jack2007/tcpquic-proxy.git
cd tcpquic-proxy
git submodule update --init --recursive
```

更新已有工作区：

```bash
git pull --rebase --autostash origin master
git submodule sync --recursive
git submodule update --init --recursive
```

如果 Crashpad fork 后续有新平台依赖提交，主仓库需要更新子模块指针：

```bash
git -C third_party/crashpad fetch origin main
git -C third_party/crashpad checkout <new-crashpad-commit>
git add third_party/crashpad
git commit -m "build: update vendored crashpad dependencies"
git push origin master
```

## 10. 维护规则

1. 不把 Crashpad DEPS 下载步骤放回主仓库 CMake。
2. 不在 CMake configure 阶段联网。
3. 不把平台补丁直接散落到主仓库；Crashpad 依赖源码和 Crashpad GN 修复应提交到 `jack2007/crashpad` fork。
4. 主仓库只更新子模块指针、检测逻辑和文档。
5. Linux、Windows、macOS 应共享同一个 fork 策略：缺什么源码，就先 vendor 到 Crashpad fork，再更新主仓库 gitlink。
6. 每个平台至少验证：
   - CMake configure 能找到或自动构建 Crashpad。
   - `tcpquic-proxy` 目标能链接成功。
   - 输出目录中存在 `raypx2` 和对应平台的 `crashpad_handler`。
   - 运行 `raypx2 --help` 时能看到 Crashpad 启用诊断或至少不因 handler 缺失崩溃。

## 11. 常见问题

### 为什么还需要 GN/Ninja？

Crashpad 自身使用 GN/Ninja 作为构建系统。fork 已经提交源码依赖，但不会把每个平台、每种配置的二进制产物提交进仓库。因此启用 Crashpad 时仍需要本机有 GN/Ninja 来生成和编译 Crashpad。

### 为什么不提交 depot_tools？

`depot_tools` 是 Chromium 生态的下载和同步工具链，体积和行为都不适合作为主仓库构建路径的一部分。本项目的目标是 clone 后通过 Git 子模块拿到源码，而不是在配置阶段再由 `gclient` 拉取额外代码。

### 如果 `TCPQUIC_ENABLE_CRASHPAD=AUTO` 没启用怎么办？

`AUTO` 模式下 Crashpad 不可用时会降级为禁用。需要强制检查依赖时使用：

```bash
cmake -S . -B build -DTCPQUIC_ENABLE_CRASHPAD=ON
```

这样缺少 GN、Ninja、vendored DEPS 或构建失败都会直接报错。
