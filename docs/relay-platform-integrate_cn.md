# Relay 平台适配整理（当前实现）

本文总结当前 `tcpquic-proxy` 的 relay 平台适配现状。

## 1. 平台适配入口

Relay 的统一入口在 [src/tunnel/relay.h](/home/jack/src/tcpquic-proxy/src/tunnel/relay.h) 与 [src/tunnel/relay.cpp](/home/jack/src/tcpquic-proxy/src/tunnel/relay.cpp)。

- `TqRelayHandle` 包含后端类型字段，当前 enum 为
  - `None`
  - `LinuxWorker`
  - `WindowsWorker`
  - `DarwinWorker`
- `TqRelayStart()`、`TqRelayStop()`、`TqRelayStartQuicReceiveSink()` 在编译期通过宏分发到对应实现。
- 典型分发逻辑（`relay.cpp`）：
  - `_WIN32` -> `TqWindowsRelayRuntime` + Windows worker
  - `__linux__` -> `TqLinuxRelayRuntime` + Linux worker
  - `__APPLE__` -> `TqDarwinRelayRuntime` + Darwin worker
  - 其他平台走不支持路径（`unsupported`），`TqRelayStart` 会返回 `false`

## 2. Linux 平台

- 代码位置：
  - [src/tunnel/linux_relay_worker.h](/home/jack/src/tcpquic-proxy/src/tunnel/linux_relay_worker.h)
  - [src/tunnel/linux_relay_worker.cpp](/home/jack/src/tcpquic-proxy/src/tunnel/linux_relay_worker.cpp)
  - [src/tunnel/linux_relay_event_queue.h](/home/jack/src/tcpquic-proxy/src/tunnel/linux_relay_event_queue.h)
  - [src/runtime/linux_reactor.h](/home/jack/src/tcpquic-proxy/src/runtime/linux_reactor.h)
  - [src/runtime/linux_reactor.cpp](/home/jack/src/tcpquic-proxy/src/runtime/linux_reactor.cpp)
- 实现特征：基于 `epoll` 的后端实现，`TqLinuxRelayRuntime` 管理多个 worker（`TqLinuxRelayRuntime::Instance()` + `Start/Stop/PickWorker`）。

## 3. Windows 平台

- 代码位置：
  - [src/tunnel/windows_relay_worker.h](/home/jack/src/tcpquic-proxy/src/tunnel/windows_relay_worker.h)
  - [src/tunnel/windows_relay_worker.cpp](/home/jack/src/tcpquic-proxy/src/tunnel/windows_relay_worker.cpp)
  - [src/runtime/windows_reactor.h](/home/jack/src/tcpquic-proxy/src/runtime/windows_reactor.h)
  - [src/runtime/windows_reactor.cpp](/home/jack/src/tcpquic-proxy/src/runtime/windows_reactor.cpp)
- 实现特征：基于 IOCP/事件的后端，`TqWindowsRelayRuntime` 管理 worker。
- 注意：`TqWindowsRelayRuntime::Start` 入参是 worker 数（来自 tuning）并在 `relay.cpp` 中触发。

## 4. macOS 平台

- 代码位置：
  - [src/tunnel/darwin_relay_worker.h](/home/jack/src/tcpquic-proxy/src/tunnel/darwin_relay_worker.h)
  - [src/tunnel/darwin_relay_worker.cpp](/home/jack/src/tcpquic-proxy/src/tunnel/darwin_relay_worker.cpp)
  - [src/tunnel/darwin_relay_event_queue.h](/home/jack/src/tcpquic-proxy/src/tunnel/darwin_relay_event_queue.h)
  - [src/runtime/darwin_reactor.h](/home/jack/src/tcpquic-proxy/src/runtime/darwin_reactor.h)
  - [src/runtime/darwin_reactor.cpp](/home/jack/src/tcpquic-proxy/src/runtime/darwin_reactor.cpp)
- 实现特征：基于 `kqueue` 的后端，`TqDarwinRelayRuntime` 管理 worker。

## 5. 运行时 metrics 与 backend 信息

- backend 输出文案在 [src/config/tuning.cpp](/home/jack/src/tcpquic-proxy/src/config/tuning.cpp) 中按宏打印 Linux/Windows/macOS 名称。
- 指标采集在 [src/runtime/relay_metrics.cpp](/home/jack/src/tcpquic-proxy/src/runtime/relay_metrics.cpp) 中按平台宏调用不同 runtime 的 `Snapshot()`。

## 6. 与 Android / iOS 的关系

- 当前代码里未看到独立 `__ANDROID__` 的 relay 后端分支。
- iOS 走 `__APPLE__` 下的 Darwin 分支。
- Android 大概率会走 Linux 分支（依赖宏是否提供 `__linux__`），但没有单独移动端 relay 适配层。
- 如目标是客户端 APP 形态（尤其 iOS 正式上架流量转发场景），需要额外评估系统权限与网络模型能力，不仅是 C++ 后端编译层面的事情。
