# Unified Client Peer Runtime 设计方案

> 日期：2026-06-20
> 状态：设计完成 / 待实现
> 适用范围：Linux / Windows / macOS 公共 client runtime 编排层
> 相关代码：`src/main.cpp`、`src/protocol/quic_session.*`、`src/ingress/client_ingress_reactor.*`、`src/runtime/router_runtime.*`

## 1. 背景

当前 client 有两套 runtime 编排代码：

- single-peer：`TqSinglePeerClientRuntime` + `RunSinglePeerClient()`。
- multi-peer：`TqMultiPeerRuntimeAdapter` + 内部 `PeerRuntime`。

两套代码底层使用相同组件：

- `QuicClientSession`
- `TqClientIngressReactor`
- `TqStartClientTunnelAsync`
- connection-state callback 打开 / 关闭 ingress listen
- delayed reconnect scheduler

重复集中在 runtime 编排层：listener open/close、connection state gating、StartTunnel lambda、reconnect scheduler 注入、metrics snapshot、stop/drain/abort。

这会导致后续改 reconnect、ingress gating、speedtest 或 metrics 时必须同时维护 single-peer 和 multi-peer 两份逻辑。2026-06-19 的 16x4 DGX 问题修复已经体现了这个风险：同一个 connection-state 处理规则需要在两处同步修改。

## 2. 目标

1. single-peer 和 multi-peer 共用同一套 peer runtime 编排代码。
2. single-peer 退化为一个固定 `peer_id=primary` 的 peer。
3. 保留现有 CLI、single-peer admin `/health` / `/metrics` 输出语义。
4. 保留现有 router config、multi-peer admin API、drain/abort 行为。
5. 保持每个 client runtime manager 一个共享 `TqClientIngressReactor`，不引入 per-peer ingress thread。
6. 不改变 `QuicClientSession`、relay worker、server dial reactor 的核心职责。

## 3. 非目标

- 不重写 router config parser。
- 不改变 SOCKS5 / HTTP CONNECT 协议。
- 不改变 speedtest 控制协议。
- 不改变 QUIC reconnect 策略。
- 不把 single-peer admin JSON 完全替换成 router metrics JSON；只在内部数据来源统一。

## 4. 总体设计

新增两个 runtime 单元：

```text
TqClientRuntimeManager
  owns one TqClientIngressReactor
  owns map<peer_id, shared_ptr<TqClientPeerRuntime>>

TqClientPeerRuntime
  owns one QuicClientSession
  owns one peer config
  opens/closes that peer in shared ingress reactor
```

### 4.1 Single-peer 路径

当没有 `--client-config` peers 时，`RunClient()` 生成一个固定 peer：

```text
peer_id = primary
quic_peer = cfg.QuicPeer
socks_listen = cfg.SocksListen
http_listen = cfg.HttpListen
quic_connections = cfg.QuicConnections
compress = cfg.Compress
enabled = true
```

然后调用 `TqClientRuntimeManager::StartPeer(primaryPeer, err)`。

single-peer speedtest 继续只允许一个 peer，并复用 `primary` peer 的 ingress gating 和 QUIC control connection。

### 4.2 Multi-peer 路径

`TqMultiPeerRuntimeAdapter` 不再内联维护 `PeerRuntime` 和 ingress reactor。它变成 `TqRouterRuntime` 到 `TqClientRuntimeManager` 的薄适配层：

```text
StartPeer(peer)          -> manager.StartPeer(peer)
StopAccepting(peer_id)   -> manager.StopAccepting(peer_id)
AbortPeerTunnels(peer_id)-> manager.AbortPeerTunnels(peer_id)
DrainPeer(peer_id, sec)  -> manager.DrainPeer(peer_id, sec)
SnapshotPeerMetrics(id)  -> manager.SnapshotPeerMetrics(id)
```

router runtime 的配置状态机和 admin API 不迁移。

### 4.3 TqClientPeerRuntime 职责

每个 peer runtime 负责：

- 构造 peer 专属 `TqConfig`。
- 创建并启动 `QuicClientSession`。
- 在 `QuicClientSession` 上注入 delayed scheduler，调度到共享 ingress reactor。
- 在 `QuicClientSession` 上注入 connection state handler。
- connected count 从 0 变为大于 0 时，在共享 ingress reactor 中 `AddPeer()`。
- connected count 变为 0 或 stop 时 `RemovePeer()`。
- tunnel accept 后选择可用 QUIC connection 并调用 `TqStartClientTunnelAsync()`。
- `AbortAllTunnels()`、`StopAccepting()`、`StopAll()`。
- 生成 peer metrics snapshot。

### 4.4 TqClientRuntimeManager 职责

manager 负责：

- 启动 / 停止共享 `TqClientIngressReactor`。
- 持有 `std::unordered_map<std::string, std::shared_ptr<TqClientPeerRuntime>>`。
- 根据 base config 和 `TqPeerConfig` 合成 peer config。
- 管理 peer 生命周期。
- 为 single-peer admin 提供 `TqClientMetrics` snapshot。
- 为 router adapter 提供 `TqPeerMetrics` snapshot。

### 4.5 生命周期

启动 peer：

1. manager 确保 ingress reactor 已启动。
2. manager 合成 peer config。
3. manager 创建 `shared_ptr<TqClientPeerRuntime>`。
4. peer runtime 设置 StartTunnel、scheduler、connection-state handler。
5. peer runtime 调用 `QuicClientSession::Start()`.
6. peer runtime 调用 `EnsureAnyConnected()` 做启动期探测；失败只打印 warning，不开放 ingress。
7. peer runtime 设置 `AcceptingEnabled=true` 并应用当前 connected state。
8. manager 将 peer runtime 放入 map。

停止 peer：

1. `StopAccepting()` 先关闭 ingress listen。
2. `DrainPeer()` 从 map 移除 runtime，并按 grace seconds 延迟 `StopAll()`。
3. `StopAll()` 清理 handler、停止 QUIC session、关闭 ingress listen。

### 4.6 Admin 兼容

single-peer admin 仍返回现有 `TqClientHealthJson()` / `TqClientMetricsJson()` 格式。数据从 manager 的 `SnapshotClientMetrics("primary")` 得到。

multi-peer admin 仍由 `TqRouterRuntime` 生成 router config / metrics / health JSON。adapter 通过 manager 提供每个 peer 的 runtime metrics。

### 4.7 错误处理

- duplicate peer id：`StartPeer()` 返回 false，错误为 `peer already running: <peer_id>`。
- ingress reactor 启动失败：返回 false，错误为 `failed to start client ingress reactor`。
- `QuicClientSession::Start()` 失败：关闭该 peer runtime，返回 false。
- listener `AddPeer()` 失败：保持 peer 不接受新 ingress，返回 false。
- connection count 为 0：不是启动失败；listener 保持关闭，等待 reconnect callback 后重新开放。

### 4.8 测试策略

单元测试：

- peer config overlay：single primary peer 与 router peer 使用相同合成规则。
- ingress gating：connected count 0 关闭，>0 打开，回到 0 关闭。
- manager lifecycle：start/stop/drain/abort 调用不会访问已移除 peer。
- router adapter：仍能向 `TqRouterRuntime` 暴露 peer metrics。

回归测试：

- `tcpquic_client_ingress_reactor_test`
- `tcpquic_quic_session_reconnect_test`
- `tcpquic_tunnel_test`
- `tcpquic_speed_test_test`
- `tcpquic_router_runtime_test`
- `tcpquic_config_router_test`

DGX 验证：

- single-peer 1x1 smoke。
- multi-peer 至少两个 peer 的 listen gating smoke。
- speedtest ingress flow smoke。

## 5. 风险与缓解

| 风险 | 缓解 |
|------|------|
| single-peer admin JSON 变化 | 保留 `TqClientMetrics` 输出函数，只改变数据来源 |
| 日志格式变化影响脚本 grep | 保留 single-peer `HTTP CONNECT listening on` 文案；multi-peer 保留 `peer <id>` 文案 |
| 抽象过大导致一次性改动不可审查 | 分 task 提交：先抽 peer runtime，再接 multi，再接 single |
| manager 与 router runtime 职责重叠 | router runtime 只管配置状态机和 admin；manager 只管真实 runtime |
| speedtest 被误改为 multi-peer | 本阶段仍要求 speedtest router config 只有一个 enabled peer |

## 6. 验收标准

1. `src/main.cpp` 不再包含 `TqSinglePeerClientRuntime` 与 `TqMultiPeerRuntimeAdapter::PeerRuntime` 两份重复 runtime。
2. single-peer 和 multi-peer 都通过 `TqClientPeerRuntime` 启动 QUIC、开放 ingress、启动 tunnel。
3. single-peer admin `/health` / `/metrics` 兼容原字段。
4. multi-peer router admin API 行为不变。
5. 回归测试全部通过。
6. DGX single-peer smoke 和 multi-peer smoke 通过。
