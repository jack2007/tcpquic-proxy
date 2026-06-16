# 代理多用户鉴权 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement task-by-task.

**Goal:** 为 client 侧 SOCKS5 / HTTP CONNECT 增加 `--client-config` 顶层 `proxy_auth` 多用户鉴权。

**Architecture:** `TqRouterConfig.ProxyAuth` → `TqProxyAuthTable` → ingress server 构造注入；SOCKS5 RFC 1929 + HTTP Basic 407。

**Tech Stack:** C++17, 现有 JSON client config 解析器

---

## 已完成任务

- [x] `ingress/proxy_auth.{h,cpp}` — 多用户校验、Basic 解析、base64
- [x] `config` — 解析/校验 `proxy_auth`
- [x] `socks5_server` / `http_connect_server` — 鉴权接入
- [x] `main.cpp` — 全局 auth 表注入 listener；speed test 保留 ProxyAuth
- [x] 单元测试 + README + design spec

**验证：**

```bash
cd build-glibc/bin/Release
./tcpquic_proxy_auth_test
./tcpquic_config_router_test
./tcpquic_http_connect_test
./tcpquic_socks5_test
```
