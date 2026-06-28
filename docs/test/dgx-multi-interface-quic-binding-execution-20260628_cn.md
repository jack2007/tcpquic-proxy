# DGX 多网口 QUIC 绑定执行记录

日期：2026-06-28

## 1. 执行环境

- 本机：`/home/jack/src/tcpquic-proxy`
- 对端：`jack@172.16.10.81`
- 本机二进制：`build/bin/Release/raypx2`
- 对端二进制：`/home/jack/tcpquic-dgx-bin/raypx2`
- 结果目录：`docs/dgx-multi-interface-quic-binding-20260628-161456/`

数据链路：

| path | 本机网口 | 本机 IP | 对端网口 | 对端 IP |
|---|---|---:|---|---:|
| path-a | `enp1s0f0np0` | `169.254.250.230` | `enp1s0f0np0` | `169.254.59.196` |
| path-b | `enP2p1s0f0np0` | `169.254.212.101` | `enP2p1s0f0np0` | `169.254.75.45` |

路由检查通过：

- `169.254.250.230 -> 169.254.59.196` 使用 `enp1s0f0np0`
- `169.254.212.101 -> 169.254.75.45` 使用 `enP2p1s0f0np0`

## 2. 初始问题

显式多地址监听：

```bash
--listen 169.254.59.196:4433,169.254.75.45:4433
```

初始结果：

- path-a 的 4 个 slot 能连接。
- path-b 的 4 个 slot 持续断连。
- tcpdump 显示 path-b 双向 UDP 包存在，说明物理链路、路由和回程不是根因。
- 单独监听 `169.254.75.45:4433` 可成功。
- 通配监听 `*:4433` 可成功。

结论：问题集中在同进程内对同一 UDP 端口启动多个 MsQuic listener 的运行时模型。

## 3. 修复摘要

修复点：

- 逻辑配置仍保留用户输入的多个具体 listen 地址。
- 当多个具体地址属于同一地址族和同一端口时，实际 MsQuic bind 合并为一个 family wildcard listener。
- 在 `QUIC_LISTENER_EVENT_NEW_CONNECTION` 阶段根据 MsQuic 提供的 `Info->LocalAddress` 做过滤，只允许原始 resolved listen 中的地址进入连接。

这样避免了 Linux/MsQuic 同端口多 listener 的握手问题，同时不把用户未配置的同端口本地地址放开。

涉及文件：

- `src/protocol/quic_address.cpp`
- `src/protocol/quic_address.h`
- `src/protocol/quic_session.cpp`
- `src/protocol/quic_session.h`
- `src/unittest/config_router_test.cpp`

## 4. 复测结果

### 4.1 显式多 listen 握手

case：`docs/dgx-multi-interface-quic-binding-20260628-161456/cases/explicit-multi-listen-after-fix-163339/`

结果：

- client 退出码 `124`，来自 `timeout 25s` 主动结束，不是连接失败。
- 8 个 QUIC slot 全部 connected。
- slot 1-4：`169.254.250.230 -> 169.254.59.196:4433`
- slot 5-8：`169.254.212.101 -> 169.254.75.45:4433`

### 4.2 TCP forward 多连接复用

case：`docs/dgx-multi-interface-quic-binding-20260628-161456/cases/tcp-forward-explicit-after-fix-163429/`

结果：

- `iperf3` 退出码：`0`
- client 退出码：`0`
- 发送口径：约 `51.54 Gbps`
- 接收口径：约 `49.92 Gbps`
- trace 显示 8 个 slot 都打开业务 stream，说明 TCP 接入侧通过 `PickConnection()` 轮询复用了同一 peer 的多条 QUIC connection。

最终回归 case：`docs/dgx-multi-interface-quic-binding-20260628-161456/cases/explicit-multi-listen-final-164129/`

- `READY=1`
- `iperf3` 退出码：`0`
- client 退出码：`0`
- 发送口径：约 `51.48 Gbps`
- 接收口径：约 `49.45 Gbps`

### 4.3 通配监听对照

case：`docs/dgx-multi-interface-quic-binding-20260628-161456/cases/tcp-forward-wildcard-paths-4x4-162942/`

结果：

- `iperf3` 退出码：`0`
- client 退出码：`0`
- 发送口径：约 `45.46 Gbps`
- 接收口径：约 `44.36 Gbps`
- 8 个 slot 全部 connected，TCP forward 正常。

说明：

- 该 case 只能证明通配监听下双 path 业务可用。
- 当前证据不足以完整证明 server `0.0.0.0:4433` 的 resolved listens 枚举结果，也未记录是否枚举到 `172.16.*` 管理网地址。
- `trace-client.log` 存在历史追加内容，后续引用该 case 时应优先使用 admin/summary 或截取后的 trace 证据。

### 4.4 设计矩阵补充执行结果

| 编号 | case | 结论 | 证据摘要 |
|---|---|---|---|
| F01 单链路兼容 | `f01-path-a-baseline-170736` | 通过短跑验证 | `READY=1`、`IPERF_RC=0`、`CLIENT_RC=0`；path-a 4/4 connected；接收约 `30.03 Gbps` |
| F03 server 显式列表 | `explicit-multi-listen-final-164129` | 通过 | 显式 listen A/B，8 个 slot 全部 connected |
| F04 peer 列表无 paths | `f04-peer-list-no-paths-170749` | 通过短跑验证 | `primary` 8/8 connected，peer 地址 A/B 轮询，`local` 为空，接收约 `54.23 Gbps` |
| F05 client paths 双链路 | `explicit-multi-listen-final-164129` | 通过 | slot 1-4 为 path-a，slot 5-8 为 path-b，本地源地址和 peer 地址匹配配置 |
| F06 path 权重 | `f06-weight-6x2-170822` | 部分通过 | 6+2 slot 配置和 8/8 connected 成立，16 stream iperf 成功；未形成 32 条短 tunnel 和 `total_tunnels` 3:1 权重证据 |
| F07 单 tunnel 非聚合 | `f07-single-tunnel-170834` | 通过短跑验证 | `PARALLEL=1`、`IPERF_RC=0`、`CLIENT_RC=0`；单 tunnel 不要求跨 path 聚合 |
| F08 多 tunnel 聚合 | `explicit-multi-listen-final-164129` | 通过短跑验证 | 双 path 接收约 `49.45 Gbps`，相对 F01 单 path 接收约 `1.65x` |
| F09 admin 可观测 | `f01-path-a-baseline-170736`、`f04-peer-list-no-paths-170749`、`f06-weight-6x2-170822`、`f07-single-tunnel-170834` | 通过 | admin connection JSON 已包含 `path`、`local`、`peer`、`connected`、`state` |
| DR04 client local 配错 | `dr04-invalid-client-local-170955` | 部分通过 | invalid local 触发重试/错误日志，path-a 仍可连接；缺少 admin `last_error` 级别证据 |
| DR07 管理网保护 | `dr07-management-plane-protection-171050` | 通过 | `SSH_STILL_OK=1`、`SSH_CLEANUP_OK=1` |

吞吐对比：

- 单 path F01 接收约 `30.03 Gbps`。
- 显式双 path 最终回归接收约 `49.45 Gbps`。
- 双 path / 单 path 接收口径约 `1.65x`，达到测试设计中暂定的 `1.6x` 门槛。
- 以上吞吐均为 5-10 秒短跑结果，不等同于 3 轮 60 秒或 10 分钟稳定性验证。

### 4.5 DR03 server 单 listener 恢复

case：`docs/dgx-multi-interface-quic-binding-20260628-161456/cases/dr03-server-single-listener-recover-rerun2-171438/`

测试编排：

- 先启动 server，仅监听 `169.254.59.196:4433`。
- client 使用 `client-paths-forward.json`，配置 path-a 4 条、path-b 4 条。
- 保存 a-only admin 快照后，不停止 client，仅重启 server 为 `169.254.59.196:4433,169.254.75.45:4433`。
- 继续在 10/20/30/45 秒采集 client admin connection 快照，并尝试通过 `127.0.0.1:15445` 做 iperf probe。
- 本轮测试脚本保存为 `run-dr03.sh`，远端日志同步到 `remote-final/`。

a-only 阶段结果：

- `TOKEN_READY=1`
- `A_ONLY_CONNECTED=4`
- admin 快照显示 path-a 4 条 `connected`，path-b 4 条 `retry_scheduled`。
- 对端 server 日志显示只监听 `169.254.59.196:4433`，并接受来自 `169.254.250.230` 的 4 条连接。

恢复阶段结果：

- 双 listen server 启动成功，`server-dual-start.txt` 记录对端进程和 `*:4433` UDP socket。
- client 日志显示 server 重启后 path-a 连接断开，path-b slot 5 曾出现一次 connected。
- 10/20/30/45 秒 admin 查询均超时，对应 `admin-after-recover-*s-connections.json` 为 0 字节。
- iperf probe 未完成数据传输，最终因人工中断留下 `interrupt - the client has terminated by signal Interrupt(2)`。

结论：

- DR03 的 a-only 阶段符合预期。
- 恢复阶段未达到“30 秒内恢复到 8 条 connected”的设计目标；本轮应记录为失败/异常，不计为通过。
- 需要后续专项定位：server 重启后 client retry 是否被单个 path 的连接状态、admin 处理线程或 MsQuic 回调路径阻塞。

### 4.6 未完成或证据不足项

- F02 server 通配展开：缺少 resolved listens/admin/`ss` 的完整枚举证据。
- F10 配置热更新：未执行 admin PATCH 修改 paths 的 case。
- DR01/DR02/DR05/DR06：未形成完整 case 证据。
- 稳定性门禁：多数 case 为短跑，尚未覆盖 3 轮 60 秒、10 分钟稳定、download/upload 双方向、短连接 k6 基线等。

## 5. 10.201 独立网段复测

为排除两条数据链路同属 `169.254.0.0/16` 带来的 route metric、scope、源地址选择和 FIB 缓存不确定性，新增两个独立 `/30` 直连网段作为正式测试地址：

| path | 本机网口 | 本机 IP | 对端网口 | 对端 IP |
|---|---|---:|---|---:|
| path-a | `enp1s0f0np0` | `10.201.1.1/30` | `enp1s0f0np0` | `10.201.1.2/30` |
| path-b | `enP2p1s0f0np0` | `10.201.2.1/30` | `enP2p1s0f0np0` | `10.201.2.2/30` |

地址以 secondary address 方式临时添加，未删除历史 `169.254.*` 地址。路由和 ping 证据保存在：

`docs/dgx-multi-interface-quic-binding-20260628-10net/route-and-ping-setup.txt`

关键验证：

- 本机 `ip route get 10.201.1.2` 命中 `enp1s0f0np0`，源地址为 `10.201.1.1`。
- 本机 `ip route get 10.201.2.2` 命中 `enP2p1s0f0np0`，源地址为 `10.201.2.1`。
- 对端反向普通路由和 `from` 路由同样命中预期网口。
- 两条链路 ping 均为 0% 丢包。

### 5.1 证书和配置

新增结果目录：

`docs/dgx-multi-interface-quic-binding-20260628-10net/`

新增 server 证书：

- 本机证据：`docs/dgx-multi-interface-quic-binding-20260628-10net/certs/server-10net.inspect.txt`
- 对端部署：`/home/jack/tcpquic-dgx-certs/10net/server-10net.crt`
- SAN：`127.0.0.1`、`10.201.1.2`、`10.201.2.2`

新增 client 配置：

- `client-path-a-forward.json`
- `client-paths-forward.json`
- `client-paths-weight-6x2-forward.json`

### 5.2 核心回归结果

执行脚本：

`docs/dgx-multi-interface-quic-binding-20260628-10net/run-core-10net.sh`

| 编号 | case | 结论 | 结果摘要 |
|---|---|---|---|
| F01 单链路兼容 | `f01-path-a-baseline-10net` | 通过短跑验证 | `READY=1`、`IPERF_RC=0`、`CONNECTED_AFTER=4`，接收约 `40.95 Gbps` |
| F05 paths 双链路 | `f05-f08-paths-4x4-10net` | 通过短跑验证 | `READY=1`、`IPERF_RC=0`、`CONNECTED_AFTER=8`，admin 显示 path-a/path-b 各 4 条 connected |
| F08 多 tunnel 聚合 | `f05-f08-paths-4x4-10net` | 连通通过，吞吐未达聚合结论 | 接收约 `30.70 Gbps`，低于本轮 F01 单 path；该 8 秒短跑不能作为聚合带宽提升证据 |
| F04 peer-list 无 paths | `f04-peer-list-no-paths-10net` | 通过短跑验证 | `READY=1`、`IPERF_RC=0`、`CONNECTED_AFTER=8`，peer 地址 `10.201.1.2` / `10.201.2.2` 轮询，接收约 `51.14 Gbps` |

结论：

- 独立 `/30` 网段下，paths 模式和 peer-list 无 paths 模式均能建立多条 QUIC connection。
- peer-list 无 paths 模式不再依赖同 `/16` 的源地址选择歧义；普通路由已经能区分两个 peer 地址的出接口。
- 本轮 paths 双链路吞吐短跑波动较大，不能替代 3 轮 60 秒聚合吞吐验证。

### 5.3 DR03 复测

执行脚本：

`docs/dgx-multi-interface-quic-binding-20260628-10net/run-dr03-10net.sh`

case：

`docs/dgx-multi-interface-quic-binding-20260628-10net/cases/dr03-server-single-listener-recover-10net/`

结果：

- a-only 阶段符合预期：`TOKEN_READY=1`、`A_ONLY_CONNECTED=4`；path-a 4 条 connected，path-b 4 条 retry。
- server 重启为 `10.201.1.2:4433,10.201.2.2:4433` 后，10/20/30/45 秒 admin 查询均超时。
- 恢复后的 iperf probe 超时：`IPERF_AFTER_RECOVER_RC=124`。
- 由于 client 在恢复异常后无法正常退出，本轮执行做了强制清理：`FORCED_CLEANUP=1`。

结论：

- 独立 `/30` 网段没有消除 DR03 恢复异常。
- 因此 DR03 的主要根因不应继续归因于 `169.254.0.0/16` 路由歧义；后续应专项定位 client retry、admin 响应、port forward accept 或 MsQuic 回调路径。

### 5.4 DR03 失败定位进展

重复执行目录：

`docs/dgx-multi-interface-quic-binding-20260628-10net/cases/dr03-repeat-181409/`

重复结果：

| 轮次 | a-only | dual recover | iperf | 结论 |
|---|---:|---:|---:|---|
| 1 | 4 | timeout | 124 | 失败 |
| 2 | 4 | 8 | 0 | 通过 |
| 3 | 4 | timeout | 124 | 失败 |

定位结论：

- DR03 不是固定配置错误，而是 server 从单 listener 恢复到双 listener 时的偶发 client 侧恢复卡死。
- 失败时 admin 端口仍在监听，`/connections` 和 `/peers` 请求会卡住；port forward 后续 iperf 也超时。
- 有效线程栈显示 ingress reactor 线程卡在 `QuicClientSession::StartSlot()` 内部的 `TqSetDisable1RttEncryption()` / `MsQuicSetParam()`。
- 同时 MsQuic worker 回调线程正在执行 `QuicClientSession::ConnectionCallback()`，经 `NotifyConnectionStateChanged()` 进入 `TqClientPeerRuntime::ApplyConnectionState()`，再调用 `OpenListenersLocked()` / `TqClientIngressReactor::AddPeer()`，并在 `EnqueueSync()` 等待 ingress reactor。
- admin 线程只是被连带阻塞：一个线程在 `QuicClientSession::SnapshotConnections()` 等 `ConfigLock/State->Lock`，另一个线程在 `TqClientPeerRuntime::SnapshotPeerMetrics()` 等 `TunnelStartMutex`。

初步根因判断：

- 失败核心是 client 侧重连路径中的同步等待环：ingress reactor 正在执行 delayed retry 的 `StartSlot()`，而 MsQuic connection callback 又同步回调到 peer runtime 并等待同一个 ingress reactor 完成 listener 更新。
- admin timeout 和 iperf timeout 是该等待环的外部表现，不是独立的 admin 或 iperf 问题。
- `10.201.1.0/30`、`10.201.2.0/30` 路由已经排除为主因。

后续修复方向：

- 不应在 MsQuic callback 线程内同步等待 ingress reactor；connection state handler 应改为异步投递 listener 状态更新，或者在 peer runtime 中对“listener 已打开”做幂等快速返回，避免 `EnqueueSync()` 反向等待。
- `StartSlot()` 中持有 `ConfigLock` 期间调用 MsQuic API 的范围也需要收窄，避免 admin snapshot 被重连慢路径长时间阻塞。
- 修复后 DR03 至少需要 10 轮重复验证，确认不再出现 recover timeout 和 client stop 强制清理。

### 5.5 DR03 修复后验证

修复提交：

- `e664775 Make ingress EnqueueSync reactor-thread safe`
- `e544ba9 Make client peer listener state apply async`
- `d28beda Shrink client StartSlot lock scope`

验证目录：

`docs/dgx-multi-interface-quic-binding-20260628-10net/cases/dr03-postfix-230012/`

执行命令：

```bash
bash docs/dgx-multi-interface-quic-binding-20260628-10net/run-dr03-10net.sh
```

连续执行 10 轮，结果如下：

| 轮次 | token ready | a-only connected | recover 10s connected | iperf rc | client wait rc | 结论 |
|---:|---:|---:|---:|---:|---:|---|
| 1 | 1 | 4 | 8 | 0 | 0 | 通过 |
| 2 | 1 | 4 | 8 | 0 | 0 | 通过 |
| 3 | 1 | 4 | 8 | 0 | 0 | 通过 |
| 4 | 1 | 4 | 8 | 0 | 0 | 通过 |
| 5 | 1 | 4 | 8 | 0 | 0 | 通过 |
| 6 | 1 | 4 | 8 | 0 | 0 | 通过 |
| 7 | 1 | 4 | 8 | 0 | 0 | 通过 |
| 8 | 1 | 4 | 8 | 0 | 0 | 通过 |
| 9 | 1 | 4 | 8 | 0 | 0 | 通过 |
| 10 | 1 | 4 | 8 | 0 | 0 | 通过 |

结论：

- a-only 阶段稳定保持 path-a 4 条 connected。
- server 恢复为双 listen 后，10 秒内稳定恢复到 8 条 connected。
- 恢复后的 iperf probe 全部成功，client 等待退出全部成功。
- 10 轮均未复现 admin timeout、iperf timeout 或 client stop 强制清理。

## 6. 已清理状态

- 本机无残留 `raypx2` 进程。
- 对端无残留 `raypx2` / `iperf3` 进程。
- 本机和对端 `enp1s0f0np0`、`enP2p1s0f0np0` 未保留 netem qdisc。
- 修复后 10 轮 DR03 验证结束时再次检查：本机/对端无残留 `raypx2`、`iperf3`、`gdb` 进程；本机和对端两张数据网口 qdisc 均为 `mq`。
- `10.201.1.0/30` 和 `10.201.2.0/30` 地址作为本轮后续测试地址保留在数据网口上；它们是运行时添加的 secondary address，重启后可能丢失，尚未写入持久化网络配置。
- 临时生成的 `cert/ca.srl` 已删除。

## 7. 后续建议

- 增加一个集成测试，模拟同端口多 listen 的 bind plan，避免后续回退到每地址一个 MsQuic listener。
- 增加 DR03 专项调试：server 重启后保持 client 存活，分别观察 path retry、admin 响应、port forward accept 和 MsQuic 回调线程状态。
- 补齐 F02/F06/F10 和 DR01/DR02/DR05/DR06 的自动化证据采集脚本。
- 将吞吐验证扩展为 3 轮 60 秒，并分别覆盖 download/upload。
- 如果 10.201 网段作为长期测试基线，需要把 secondary address 固化到两台 DGX 的网络配置中；当前只是运行时配置。
