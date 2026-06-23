# tcpquic-proxy 本地端口转发设计

> 日期：2026-06-23  
> 状态：待评审

## 背景

`tcpquic-proxy` 当前提供 SOCKS5 和 HTTP CONNECT 两种 client 侧本地入口。两者最终都会生成 `TunnelRequest`，通过现有 QUIC stream OPEN 控制握手交给 server，server 再按目标地址和端口发起 TCP dial。

本设计增加类似 `ssh -L` 的本地端口转发能力：

```text
client 本地应用
  -> 127.0.0.1:LOCAL
  -> tcpquic-proxy client
  -> QUIC stream
  -> tcpquic-proxy server
  -> TARGET_HOST:TARGET_PORT
```

该能力适合把固定服务暴露到本机端口，避免应用显式配置 SOCKS5 或 HTTP CONNECT 代理。例如：

```bash
tcpquic-proxy client \
  --peer proxy-b.example.com:443 \
  --ca cert/ca.crt \
  --forward 127.0.0.1:15432=db.internal.example.com:5432
```

## 目标

1. 支持 client 侧本地端口转发，语义等价于 `ssh -L LOCAL_HOST:LOCAL_PORT:TARGET_HOST:TARGET_PORT`。
2. 本地 listener 只在对应 peer 至少有一条已连接 QUIC connection 时打开；所有 QUIC connection 断开时关闭，重连后重新打开。
3. 转发连接复用现有 `TunnelRequest`、OPEN/OPEN_OK/OPEN_FAIL、压缩协商、server ACL、TCP dial 和 relay worker。
4. 单 peer CLI 和多 peer router JSON 都支持配置本地端口转发。
5. 支持一个 peer 配置多个 forward，且能在 admin `PUT /config` 时动态增删改。
6. 保持跨平台：Linux、macOS、Windows 共享主要实现，监听和 fd 事件继续走现有 socket/reactor 抽象。

## 非目标

- 不实现 `ssh -R` 远端端口转发。
- 不实现 `ssh -D` 动态 SOCKS 语义；现有 SOCKS5 已覆盖动态目标。
- 不新增 QUIC wire protocol 字段或 server 侧新命令。
- 不绕过 server ACL；forward 的目标仍由 server 侧 ACL 和 DNS 解析规则决定。
- 不为本地 port forward 增加鉴权协议。本地访问控制首版依赖显式 bind 地址，默认建议 loopback。
- 不支持 UDP 转发。

## 方案对比

| 方案 | 描述 | 优点 | 缺点 | 结论 |
|------|------|------|------|------|
| A. 扩展现有 `TqClientIngressReactor` | 把 fixed-target listener 作为第三种 ingress proto，与 SOCKS5/HTTP CONNECT 共用 reactor、open timeout、completion/cancel 逻辑 | 复用最多；连接状态切换和 peer remove 路径一致；跨平台成本低 | 需要整理现有 “两个 listener fd” 的内部结构 | 采用 |
| B. 新增独立 `TqPortForwardReactor` | 为 port forward 单独实现 listener、accept、async open completion | 对现有 ingress 侵入小 | 重复超时、completion、remove peer、reactor 生命周期逻辑；后续动态配置更容易分叉 | 不采用 |
| C. 让本地 forward 伪装成 SOCKS5 请求 | accept 后在内部构造 SOCKS5 handshake 再走现有解析器 | 初看改动小 | 无意义的协议自绕；错误处理和 relay 接管时机更别扭 | 不采用 |

## 用户体验

### 单 peer CLI

新增可重复参数：

```text
--forward <local>=<target>
```

其中 `<local>` 和 `<target>` 都使用现有 `host:port` 表示：

```bash
--forward 127.0.0.1:15432=db.internal.example.com:5432
--forward [::1]:18080=10.0.0.15:8080
```

规则：

- `--forward` 只在 client 模式有效。
- 可配置多次。
- `local` 必须是合法 `host:port`。
- `target` 必须是合法 `host:port`，target host 可以是域名、IPv4 或 bracketed IPv6。
- `local` 与同一进程内的 SOCKS5、HTTP CONNECT、其他 forward listener 不能重复。
- `local` 允许非 loopback，但文档和示例默认使用 `127.0.0.1` 或 `::1`。

### Router JSON

`TqPeerConfig` 增加 `PortForwards`：

```json
{
  "version": 1,
  "peers": [
    {
      "peer_id": "agent-b",
      "quic_peer": "proxy-b.example.com:443",
      "socks_listen": "127.0.0.1:1080",
      "http_listen": "127.0.0.1:8080",
      "port_forwards": [
        {
          "listen": "127.0.0.1:15432",
          "target": "db.internal.example.com:5432"
        },
        {
          "listen": "127.0.0.1:18080",
          "target": "10.0.0.15:8080"
        }
      ],
      "enabled": true
    }
  ]
}
```

`GET /config` 和 `PUT /config` 使用同一字段。动态变更时，如果 peer 的 `port_forwards` 发生变化，视为 data-plane 配置变化：停止旧 listener，主动中断该 peer 的 tunnel，并按现有 drain 规则启动新 peer runtime。

### Metrics

client metrics 的 peer 字段增加：

```json
"port_forwards": [
  {
    "listen": "127.0.0.1:15432",
    "target": "db.internal.example.com:5432"
  }
]
```

首版不增加 per-forward 连接计数。连接数仍通过现有 `active_streams`、`total_streams` 观察。

## 配置模型

新增结构：

```cpp
struct TqPortForwardConfig {
    std::string Listen;
    std::string TargetHost;
    uint16_t TargetPort{0};
};
```

`TqPeerConfig` 增加：

```cpp
std::vector<TqPortForwardConfig> PortForwards;
```

`TqConfig` 保留单 peer 全局配置入口：

```cpp
std::vector<TqPortForwardConfig> PortForwards;
```

`TqMakePrimaryPeerConfig()` 和 `TqMakePeerRuntimeConfig()` 将 forward 配置复制到 peer runtime。`PeerDataPlaneChanged()` 和 `SameBridgeActivePeer()` 必须比较 `PortForwards`，保证 admin 动态变更能触发 listener 重建。

## 地址解析和编码

CLI/JSON 解析阶段只校验格式，不做目标 DNS 解析。这样保持与 SOCKS5 hostname 行为一致：target 域名由 server 侧解析并接受 ACL 校验。

构造 `TunnelRequest` 时：

- IPv4 target 设为 `AddrType = TQ_ADDR_IPV4`，`Host` 为文本 IPv4。
- bracketed IPv6 target 设为 `AddrType = TQ_ADDR_IPV6`，`Host` 为不带方括号的 IPv6 文本。
- 其他 target host 设为 `AddrType = TQ_ADDR_DOMAIN`。
- `Port` 为 target port。
- `CompressFlags` 初始为 0，实际压缩选择仍由 `TqFlagsFromConfig()` 根据 peer `Compress` 决定。
- `IngressTraceProto` 增加新值 `3=port-forward`，仅用于 trace 区分入口来源。

OPEN frame、OPEN response 和 server dial path 不变。

## Ingress 设计

`TqClientIngressReactor` 扩展 `ListenProto`：

```cpp
enum class ListenProto {
    Socks5,
    HttpConnect,
    PortForward,
};
```

现有 `PeerEntry` 从固定 `SocksFd/HttpFd` 调整为 listener 列表：

```cpp
struct PortForwardEntry {
    TqPortForwardConfig Config;
    TqSocketHandle Fd{TqInvalidSocket};
    std::string BoundAddress;
};
```

或更通用的内部 `PeerListenEntry`：

```cpp
struct PeerListenEntry {
    ListenProto Proto;
    TqSocketHandle Fd{TqInvalidSocket};
    std::string Address;
    TqPortForwardConfig Forward;
};
```

`AddPeer()` 启动顺序：

1. 如果 `SocksListen` 非空，创建 SOCKS listener。
2. 如果 `HttpListen` 非空，创建 HTTP CONNECT listener。
3. 为每个 `PortForwardConfig` 创建 fixed-target listener。
4. 任一 listener 创建或注册失败，则回滚已经创建的 fd，`AddPeer()` 返回 false。

为保持向后兼容，单 peer 默认仍有 `SocksListen = 127.0.0.1:1080`。router peer 仍要求至少有一个可用入口：SOCKS、HTTP 或 port forward 之一。这样允许专用 forward peer 不开启 SOCKS：

```json
{
  "peer_id": "db",
  "quic_peer": "proxy-b.example.com:443",
  "socks_listen": "",
  "http_listen": "",
  "port_forwards": [
    {"listen": "127.0.0.1:15432", "target": "db.internal.example.com:5432"}
  ]
}
```

## 连接打开流程

SOCKS5/HTTP CONNECT 保持现有 handshake 状态机。Port Forward accept 后不需要读本地应用的首字节，也不向本地应用写代理响应。

实现上复用现有 `Opening` phase，并用 `ListenProto::PortForward` 判断是否需要写 response。这样避免新增只服务一个入口类型的状态分支：

1. `AcceptLoop()` 接受 port-forward client fd。
2. 创建 `ClientEntry`，直接填入 fixed `TunnelRequest`，跳过 `TqClientIngressState`。
3. 调用 `StartClientOpen(clientFd)`。
4. `CompleteClientOpen()` 收到 OPEN_OK：
   - 对 port-forward：不生成 `PendingWrite`，直接调用 `AcceptTunnel()`，把 fd 交给 relay。
   - 对 SOCKS/HTTP：保持现有写响应后再接管 fd。
5. `CompleteClientOpen()` 收到 OPEN_FAIL：
   - 对 port-forward：直接关闭本地 fd，并调用 `RejectTunnel()` 释放 handle。
   - 对 SOCKS/HTTP：保持现有协议错误响应。
6. open timeout：
   - 对 port-forward：取消 handle 并关闭 fd。
   - 对 SOCKS/HTTP：保持现有 timeout response。

这个流程保证本地应用看到的行为接近普通 TCP connect：server 侧目标不可达时，本地连接会被关闭；目标可达时连接进入透明双向转发。

## Runtime 行为

`TqClientPeerRuntime::OpenListenersLocked()` 将 peer 的 `PortForwards` 传给 ingress。日志增加：

```text
tcpquic-proxy: port forward listening on 127.0.0.1:15432 -> db.internal.example.com:5432
```

多 peer 模式日志带 peer id：

```text
tcpquic-proxy: peer db port forward listening on 127.0.0.1:15432 -> db.internal.example.com:5432
```

connection state 行为沿用现有策略：

- connected count 为 0：关闭所有该 peer 的 SOCKS/HTTP/forward listener。
- connected count 大于 0：打开所有该 peer listener。
- listener 打开失败：保持 peer runtime 运行，但该次 `ApplyConnectionState` 返回错误并记录日志；后续重连状态变化会再次尝试打开。

## 安全与 ACL

本地 port forward 的安全边界分两层：

1. client 本地 bind 地址决定谁能连接本地端口。默认示例和推荐配置使用 loopback。
2. server 侧 ACL 决定最终是否允许连接 `TARGET_HOST:TARGET_PORT`。

不在 client 侧提前执行 ACL。原因是现有 ACL 语义属于 server：域名由 server 侧 DNS 解析，deny 优先于 allow，并对所有 A/AAAA 候选过滤。Port Forward 必须复用该语义，避免 client/server 策略分叉。

## 错误处理

配置错误在启动或 `PUT /config` 时拒绝：

- `--forward` 缺少 `=`。
- local 或 target 不是合法 `host:port`。
- port 为 0 或大于 65535。
- 同一 router config 中 listener 地址重复。
- peer 没有 SOCKS、HTTP 或 port forward 任一入口。

运行时错误：

- local bind/listen 失败：该 peer `StartPeer()` 失败，admin metrics `last_error` 记录原因。
- QUIC 当前不可用：listener 会关闭；已经 accept 但尚未 OPEN 的连接被关闭。
- server ACL/DNS/TCP dial 失败：port-forward 本地 fd 直接关闭。
- OPEN timeout：取消 OPEN handle，关闭 fd。

## 测试计划

单元测试：

1. `config_router_test`
   - CLI `--forward 127.0.0.1:15432=db.example.com:5432` 解析成功。
   - 多个 `--forward` 解析成功并保序。
   - 缺少 `=`、非法 port、非法 IPv6 bracket、重复 listen 拒绝。
   - router JSON `port_forwards` 解析、序列化、metrics 输出。
   - only-forward peer 允许 `socks_listen=""`、`http_listen=""`。
2. `router_runtime_test`
   - `PortForwards` 变化触发 data-plane changed。
   - `GET /config` 和 `PUT /config` 保留 port forward 配置。
3. `client_ingress_reactor_test`
   - AddPeer 创建 forward listener，并能连接到实际 bound 地址。
   - forward accept 后直接调用 `StartTunnel()`，生成正确 `TunnelRequest`。
   - OPEN_OK 后调用 `AcceptTunnel()` 且不向本地 fd 写代理响应。
   - OPEN_FAIL/timeout 关闭本地 fd 并释放 open handle。

端到端测试：

1. 启动 server，`--allow-targets` 覆盖目标 loopback echo server。
2. client 使用脚本选择的空闲本地端口，例如 `--forward 127.0.0.1:<forward_port>=127.0.0.1:<echo_port>`。
3. 本地连接 forward bound port，发送数据并验证 echo。
4. 覆盖 `--compress off` 和 `--compress zstd`。
5. 覆盖目标 ACL deny：本地连接应被关闭，server metrics `acl_denied` 增加。

回归验证：

- Linux 构建 `tcpquic-proxy` 和相关 unit tests。
- 现有 SOCKS5、HTTP CONNECT、proxy auth、router runtime 测试继续通过。
- Windows/macOS 至少验证新增 unit tests 编译通过；socket 行为使用现有跨平台 reactor 抽象，不引入平台专用分支。

## 文档更新

实现时同步更新：

- `README.md` CLI 参数表，增加 `--forward`。
- `README.md` client 使用示例，增加本地端口转发。
- `README.md` Admin config 示例，增加 `port_forwards`。
- `docs/config_guide_cn.md` 如仍维护完整配置说明，也增加对应字段。

## 风险与缓解

| 风险 | 缓解 |
|------|------|
| 现有 ingress reactor 假设每个 peer 只有 SOCKS/HTTP 两个 fd | 先把内部 listener 存储改为统一列表，再接入 PortForward；测试覆盖 add/remove/stop |
| port-forward OPEN 失败时本地应用没有协议错误码 | 行为与普通 TCP connect 失败接近：关闭连接；详细原因在 client/server 日志和 metrics |
| forward listener 与 SOCKS/HTTP 地址重复 | 配置校验阶段全局去重，启动前拒绝 |
| target IPv6 与 `host:port` 解析歧义 | 要求 IPv6 使用 bracketed 格式 `[::1]:443`；内部存储去掉方括号 |
| admin 动态更新遗漏 forward 差异 | `PeerDataPlaneChanged()`、`SameBridgeActivePeer()`、config JSON 和 metrics JSON 都显式包含 `PortForwards` |
| 非 loopback local bind 暴露内网服务 | 默认示例使用 loopback，文档明确非 loopback 需要用户自行承担访问面 |

## 推荐实施顺序

1. 增加 `TqPortForwardConfig`，扩展 CLI、runtime JSON、router JSON 解析和校验。
2. 更新 config/router/admin metrics 序列化，补充配置和 runtime 单元测试。
3. 将 `TqClientIngressReactor` 的 peer listener 表示从固定 SOCKS/HTTP fd 改为统一 listener 列表。
4. 在 ingress 中加入 `ListenProto::PortForward` 和 fixed-target `TunnelRequest` 打开流程。
5. 更新 `TqClientPeerRuntime` 日志、metrics snapshot、data-plane change 比较。
6. 增加 ingress 单元测试和端到端脚本覆盖。
7. 更新 README 和配置文档。
