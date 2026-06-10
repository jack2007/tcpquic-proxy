# tcpquic-proxy 源码文件分类整理设计

> 日期：2026-06-10  
> 状态：待评审

## 背景

`tcpquic-proxy` 已完成 Linux 与 Windows 平台适配，当前主要源码仍平铺在 `src/` 根目录。随着平台实现、协议处理、入口服务、运行时管理和隧道转发逻辑增加，平铺结构降低了阅读效率，也使新文件归属不够明确。

本次整理只调整源码文件所在目录、CMake 源文件路径、include 搜索路径和文档中的源码结构说明，不修改业务逻辑、线程模型、平台行为或命令行接口。

## 目标

- 按职责将源码文件分类到子目录，提升阅读和维护效率。
- 保持 Linux 与 Windows 现有构建逻辑稳定。
- 避免同时引入库化、命名空间调整、接口拆分等更高风险重构。
- 让后续新增功能能自然归类到明确目录。

## 非目标

- 不改变 `tcpquic-proxy` 的 CLI、协议格式、ACL 行为、压缩行为或 relay 行为。
- 不把核心代码拆成独立静态库或共享库。
- 不批量重写 include 风格，除非文件移动后必须修正。
- 不移动 `src/unittest/` 下测试文件到多层测试目录。

## 目标目录结构

```text
src/
├── CMakeLists.txt
├── README.md
├── main.cpp
├── acl/
│   └── acl.*
├── config/
│   ├── config.*
│   └── tuning.*
├── protocol/
│   ├── compress.*
│   ├── control_protocol.*
│   └── quic_session.*
├── platform/
│   ├── platform_socket.h
│   ├── platform_socket_posix.cpp
│   ├── platform_socket_win.cpp
│   └── quic_credentials_win.*
├── ingress/
│   ├── http_connect_server.*
│   └── socks5_server.*
├── tunnel/
│   ├── linux_relay_buffer_pool.*
│   ├── linux_relay_worker.*
│   ├── relay.*
│   ├── relay_blocking_demo.*
│   ├── tcp_dialer.*
│   ├── tcp_tunnel.*
│   ├── tcp_write_queue.*
│   ├── tunnel_reaper.*
│   └── windows_relay_worker.*
├── runtime/
│   ├── admin_http.*
│   ├── router_runtime.*
│   ├── server_metrics.*
│   ├── thread_pool.*
│   └── warmup.*
├── docs/
│   ├── key_parameter.md
│   └── thread_model_cn.md
└── unittest/
    └── *_test.cpp
```

## 文件归类规则

| 目录 | 职责 | 文件 |
|------|------|------|
| `src/` | 程序入口与构建入口 | `main.cpp`、`CMakeLists.txt`、`README.md` |
| `src/acl/` | 目标访问控制、CIDR 与 DNS 候选过滤 | `acl.*` |
| `src/config/` | CLI 配置解析与调优参数 | `config.*`、`tuning.*` |
| `src/protocol/` | 隧道协议、压缩协议处理、QUIC 会话封装 | `control_protocol.*`、`compress.*`、`quic_session.*` |
| `src/platform/` | OS socket 抽象与 Windows QUIC 凭据辅助 | `platform_socket*`、`quic_credentials_win.*` |
| `src/ingress/` | client 侧本地入口协议 | `socks5_server.*`、`http_connect_server.*` |
| `src/tunnel/` | TCP-over-QUIC 隧道、relay、平台 relay worker | `tcp_tunnel.*`、`relay.*`、`linux_relay_*`、`windows_relay_worker.*` 等 |
| `src/runtime/` | 进程运行期协调、metrics、admin、线程池、warmup | `router_runtime.*`、`server_metrics.*`、`admin_http.*`、`thread_pool.*`、`warmup.*` |
| `src/docs/` | 与源码实现紧密相关的说明文档 | `key_parameter.md`、`thread_model_cn.md` |
| `src/unittest/` | 单元测试 | 现有 `*_test.cpp` |

## CMake 调整

`src/CMakeLists.txt` 继续显式列出源文件，但路径改为带子目录的形式。例如：

```cmake
set(TCPQUIC_PLATFORM_SOURCES)
if(WIN32)
    list(APPEND TCPQUIC_PLATFORM_SOURCES platform/platform_socket_win.cpp)
else()
    list(APPEND TCPQUIC_PLATFORM_SOURCES platform/platform_socket_posix.cpp)
endif()
```

`TCPQUIC_PROXY_SOURCES`、测试目标和平台条件分支中的源文件路径同步更新。Linux 专用源文件继续只在 `CMAKE_SYSTEM_NAME STREQUAL "Linux"` 时加入，Windows 专用源文件继续只在 `WIN32` 时加入。

为减少 include 改动，所有生产目标和测试目标统一包含以下源码 include 目录：

```cmake
target_include_directories(<target> PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/acl
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/protocol
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/ingress
    ${CMAKE_CURRENT_SOURCE_DIR}/tunnel
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${MSQUIC_SOURCE_DIR}/src/inc)
```

实际实施时可用一个 helper 函数或变量集中维护这些 include 目录，避免每个测试目标重复声明。

## Include 策略

第一阶段保持现有短 include 形式，例如：

```cpp
#include "config.h"
#include "tcp_tunnel.h"
#include "platform_socket.h"
```

测试中已有的 `#include "../xxx.h"` 需要改成短 include 或新相对路径。推荐统一改为短 include，依赖 CMake include 目录解析。这样移动测试文件以外的源码时，测试 include 不再绑定源文件物理位置。

## 文档更新

- 更新根目录 `README.md` 的“源码结构”章节，替换旧的 `src/tools/tcpquic-proxy/` 路径。
- 更新 `src/README.md` 中仍指向旧仓库内路径或旧 `docs/superpowers` 相对路径的链接。
- 在 `src/README.md` 增加新的目录职责表，便于读者从入口、协议、平台、隧道、运行时几个方向阅读代码。

## 验证标准

整理完成后至少验证：

1. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` 配置成功。
2. `cmake --build build --target tcpquic-proxy -j$(nproc)` 编译成功。
3. Linux 环境下所有 Linux 相关单元测试目标可编译并运行。
4. Windows 环境下 `tcpquic-proxy`、通用测试和 `tcpquic_windows_relay_worker_test` 可编译；Windows loopback 脚本继续可用。
5. `README.md` 与 `src/README.md` 中的源码结构说明不再引用旧路径。

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| CMake 源文件路径漏改导致目标缺源文件 | 按目标逐段更新，并先配置再编译全部测试目标 |
| 测试中 `../xxx.h` include 失效 | 统一改为短 include，并集中维护源码 include 目录 |
| Windows 专用文件在 Linux 上误编译，或反之 | 保留现有 `WIN32` / `CMAKE_SYSTEM_NAME STREQUAL "Linux"` 条件 |
| include 目录过多导致同名头文件冲突 | 当前项目内头文件名唯一；不引入新同名文件 |
| 文件移动掩盖行为变更 | 本次提交限制为 move、CMake 路径、include 路径和文档更新 |

## 推荐实施顺序

1. 创建目标子目录。
2. 移动生产源码和源码说明文档。
3. 更新 `src/CMakeLists.txt` 源文件路径和 include 目录 helper。
4. 将测试中的 `../xxx.h` include 改为短 include。
5. 更新 `README.md` 和 `src/README.md` 源码结构说明。
6. 运行 Linux 配置、构建和单元测试验证。
7. 在 Windows 环境运行对应构建与 loopback 验证。
