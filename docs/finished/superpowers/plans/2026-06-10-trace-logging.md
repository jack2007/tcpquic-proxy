# Trace 日志 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 完成 trace 模块剩余接入（main 初始化、tunnel 返回值补全）并通过 Release 编译。

**Architecture:** `main.cpp` 在解析配置后调用 `TqTraceInit`；`TqTraceGuard` RAII 负责 shutdown。`TqStartClientTunnel` 失败路径返回 `TraceTunnelId` 供 ingress 日志关联。

**Tech Stack:** C++17, spdlog, MsQuic, CMake

---

### Task 1: main.cpp 接入 TqTraceInit

**Files:** Modify `src/main.cpp`

- [ ] 在 `TqFinalizeConfig` 之后、`RunClient`/`RunServer` 之前调用 `TqTraceInit(cfg.Mode, cfg.TraceIntervalSec)`
- [ ] 使用栈上 `TqTraceGuard` 确保进程退出时 flush/shutdown

### Task 2: tcp_tunnel 返回值补全

**Files:** Modify `src/tunnel/tcp_tunnel.cpp`

- [ ] `TqStartClientTunnel` 在已分配 `tunnelId` 后的失败路径带上 `tunnelId`

### Task 3: 编译验证

- [ ] `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target tcpquic-proxy -j$(nproc)`
