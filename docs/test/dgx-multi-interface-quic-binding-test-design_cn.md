# DGX 多网口 QUIC 绑定系统测试设计

日期：2026-06-28

## 1. 范围和目标

本文档用于验证 `raypx2` / `tcpquic-proxy` 的多网口 QUIC 绑定能力，重点覆盖 server 多地址监听、client peer 多地址连接、client `paths` 显式本地源地址绑定，以及同一 peer 多条 QUIC connection 被 TCP tunnel 复用后的吞吐和稳定性。

本测试沿用 `docs/test/dgx-netem-delay-loss-matrix_cn.md` 中的 DGX 双机环境信息：

- 本机和对端 DGX 均使用 `jack` 用户。
- 对端控制面通过 `jack@172.16.10.81` 访问。
- `sudo -n` 已免交互，测试命令不得触发密码提示。
- `172.16.*` 是 SSH 管理网，禁止在管理网口上配置 `tc netem` 或承载 QUIC 数据面。

本次新增验证两条 200Gbps 数据链路。早期测试使用 `169.254.0.0/16` 链路本地地址；由于两条数据链路同属一个 `/16`，路由选择容易受 metric、scope、源地址和 FIB 缓存影响。正式验证改用两个独立 `/30` 直连网段，旧 `169.254.*` 地址仅作为历史证据保留。

| 逻辑链路 | 本机网口 | 本机 IP | 对端网口 | 对端 IP | 用途 |
|---|---|---:|---|---:|---|
| path-a | `enp1s0f0np0` | `10.201.1.1/30` | `enp1s0f0np0` | `10.201.1.2/30` | 对照既有 DGX 单链路 |
| path-b | `enP2p1s0f0np0` | `10.201.2.1/30` | `enP2p1s0f0np0` | `10.201.2.2/30` | 新增第二数据链路 |

核心目标：

- server 使用 `0.0.0.0:4433` 时能展开并监听两条数据链路上的本机地址；如果实现同时枚举到 `172.16.*` 管理地址，测试需要记录该行为，但数据面验证不使用管理地址。
- server 使用显式 listen 列表时只监听指定地址，这是 DGX 双数据网口验证和生产部署的推荐方式，可避免管理网地址被纳入 QUIC listener。
- client 未配置 `paths` 时，peer 地址列表能建立多条连接，但本地出口仍由系统路由决定。
- client 配置 `paths` 时，`path-a` 固定为 `10.201.1.1 -> 10.201.1.2:4433`，`path-b` 固定为 `10.201.2.1 -> 10.201.2.2:4433`。
- 多个并发 TCP tunnel 能在同一 peer 的已连接 QUIC slot 间轮询分配，长期吞吐能同时利用两条网口链路。
- 单个 TCP tunnel 不要求跨 path 聚合带宽；这是当前多连接方案的非目标。

## 2. 功能和非功能目标

### 2.1 功能目标

| 目标 | 验收标准 |
|---|---|
| server 通配监听展开 | `--listen 0.0.0.0:4433` 后 admin / 日志显示 resolved listens 至少包含两个数据 IP；如包含 `172.16.*`，记录为通配枚举行为，不作为本轮数据面入口 |
| server 显式监听列表 | `--listen 10.201.1.2:4433,10.201.2.2:4433` 只启动这两个 listener，不包含管理网 listener |
| client peer 列表模式 | `--peer 10.201.1.2:4433,10.201.2.2:4433 --connections 8` 下 slot 轮询分配到两个 peer 地址 |
| client paths 模式 | router 配置中 `paths` 生效，peer 级 `quic_peer` / `proto_peer` 和 `connections` 不参与 slot 分配 |
| TCP tunnel 调度 | 并发 8 条以上 TCP tunnel 时，admin connection snapshot 中 `total_tunnels` 在两个 path 上分布接近连接数权重 |
| 故障隔离 | 断开或 netem 限制单个 path 时，另一个 path 连接仍保持可用；新 tunnel 不应被分配到不可用 slot |

### 2.2 非功能目标

| 类别 | 暂定目标 |
|---|---|
| 吞吐 | 双 path 并发吞吐应高于单 path 基线；若未达到 `1.6x`，必须保留诊断证据并说明瓶颈 |
| 稳定性 | 每个场景至少连续运行 10 分钟或 3 轮 60 秒 iperf3，无 `raypx2` 进程退出、崩溃或 admin 不响应 |
| 连接恢复 | 单 path 故障恢复后 30 秒内对应 slot 重新 connected；另一 path 在故障期间不重启、不断流 |
| 观测性 | 每轮保存进程日志、admin JSON、`ss`、`ip route get`、`ip -s link`、`tc -s qdisc` 和 iperf3 JSON |
| 安全边界 | 控制面只走 `172.16.10.81` SSH；admin 只监听 loopback；数据面和 netem 不触碰 `172.16.*` |

## 3. 系统级端到端功能链路图

### 3.1 固定变量

```bash
ROOT=/home/jack/src/tcpquic-proxy
BIN="${ROOT}/build/bin/Release/raypx2"

REMOTE_SSH=jack@172.16.10.81
REMOTE_DIR=/home/jack/tcpquic-dgx-bin
REMOTE_BIN="${REMOTE_DIR}/raypx2"

LOCAL_IFACE_A=enp1s0f0np0
LOCAL_IFACE_B=enP2p1s0f0np0
REMOTE_IFACE_A=enp1s0f0np0
REMOTE_IFACE_B=enP2p1s0f0np0

LOCAL_IP_A=10.201.1.1
LOCAL_IP_B=10.201.2.1
REMOTE_IP_A=10.201.1.2
REMOTE_IP_B=10.201.2.2

QUIC_PORT=4433
HTTP_PORT=18080
SOCKS_PORT=19080
IPERF_PORT=16001
CLIENT_ADMIN_LISTEN=127.0.0.1:18081
SERVER_ADMIN_LISTEN=127.0.0.1:18081
CERT_DIR="${ROOT}/cert"
```

两条数据链路使用独立 `/30` 直连网段，执行前必须同时确认普通路由和源地址约束路由：

```bash
rtk ip route get "$REMOTE_IP_A"
rtk ip route get "$REMOTE_IP_B"
rtk ip route get "$REMOTE_IP_A" from "$LOCAL_IP_A"
rtk ip route get "$REMOTE_IP_B" from "$LOCAL_IP_B"

rtk ssh -o BatchMode=yes "$REMOTE_SSH" "ip route get '$LOCAL_IP_A'"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "ip route get '$LOCAL_IP_B'"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "ip route get '$LOCAL_IP_A' from '$REMOTE_IP_A'"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "ip route get '$LOCAL_IP_B' from '$REMOTE_IP_B'"
```

期望输出分别包含：

```text
dev enp1s0f0np0
dev enP2p1s0f0np0
```

### 3.2 端到端链路和断言

| 链路环节 | 正向路径 | 关键断言 | 观测证据 |
|---|---|---|---|
| 管理面 | 本机 shell -> `jack@172.16.10.81` | SSH 不经过数据网口；断开数据网口 netem 不影响 SSH | `ip route get 172.16.10.81`、SSH 日志 |
| server 启动 | 对端 `raypx2 server` -> MsQuic listener | `0.0.0.0` 至少展开到 `REMOTE_IP_A`、`REMOTE_IP_B`；显式列表只绑定指定数据地址 | server 日志、admin metrics `resolved_listens`、`ss -lunp` |
| client 启动 | 本机 router client -> QUIC slots | paths 模式下 slot 绑定指定 `local -> peer`；peer 列表模式下 local 为空或默认 | client 日志、admin `/api/v1/peers` connection JSON |
| QUIC 握手 | 本机 path-a/path-b -> 对端 path-a/path-b | 每条 path 的连接数达到配置值；源/目的地址符合预期 | `ss -uapn`、tcpdump 五元组、admin connected count |
| TCP 接入 | iperf3/curl -> 本机 HTTP CONNECT/SOCKS | 新 TCP tunnel 能打开 QUIC stream；失败时有明确错误 | iperf3 rc、client 日志、tunnel counters |
| tunnel 调度 | 多 TCP tunnel -> QUIC connected slots | tunnel 只分配到 connected slot，按 slot 轮询，单 tunnel 不迁移 | admin connection `path/local/peer/active_tunnels/total_tunnels` |
| 数据回程 | 对端 server -> 对端数据网口 -> 本机数据网口 | path-a 回 path-a，path-b 回 path-b，不走默认单网口 | `ip -s link` delta、tcpdump、`nstat` |
| 异常恢复 | 单 path netem/断链/进程重启 | 故障 path retry，健康 path 继续工作，恢复后重新 connected | reconnect counters、日志、admin health |

## 4. 测试策略和覆盖矩阵

### 4.1 前置检查

执行测试前必须保存以下证据：

```bash
cd "$ROOT"

rtk git rev-parse HEAD
rtk git submodule status third_party/msquic
rtk test -x "$BIN"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "test -x '$REMOTE_BIN'"

rtk ip -o -4 addr show dev "$LOCAL_IFACE_A"
rtk ip -o -4 addr show dev "$LOCAL_IFACE_B"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "ip -o -4 addr show dev '$REMOTE_IFACE_A'; ip -o -4 addr show dev '$REMOTE_IFACE_B'"

rtk ip route get "$REMOTE_IP_A"
rtk ip route get "$REMOTE_IP_B"
rtk ip route get "$REMOTE_IP_A" from "$LOCAL_IP_A"
rtk ip route get "$REMOTE_IP_B" from "$LOCAL_IP_B"
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "ip route get '$LOCAL_IP_A'; ip route get '$LOCAL_IP_B'; ip route get '$LOCAL_IP_A' from '$REMOTE_IP_A'; ip route get '$LOCAL_IP_B' from '$REMOTE_IP_B'"
```

防呆规则：

- 任何数据网口地址输出中出现 `172.16.` 时立即停止。
- 任一 `ip route get <peer> from <local>` 未命中预期网口时立即停止。
- 任一普通 `ip route get <peer>` 未命中预期网口时立即停止；这用于确认 peer-list 无 paths 场景不会依赖源地址绑定才能选对链路。
- 所有 `tc qdisc` 操作只允许作用于 `enp1s0f0np0` 和 `enP2p1s0f0np0`。
- admin 只监听 `127.0.0.1`；对端 admin 通过 SSH 在对端本机 curl。

### 4.2 配置样例

server 通配监听样例：

```bash
rtk ssh -o BatchMode=yes "$REMOTE_SSH" "
  LD_LIBRARY_PATH='$REMOTE_DIR' nohup '$REMOTE_BIN' server \
    --listen 0.0.0.0:$QUIC_PORT \
    --allow-targets '$REMOTE_IP_A/32,$REMOTE_IP_B/32,127.0.0.0/8' \
    --cert ~/tcpquic-dgx-certs/server/server.crt \
    --key ~/tcpquic-dgx-certs/server/server.key \
    --ca ~/tcpquic-dgx-certs/ca.crt \
    --compress off \
    --tuning wan \
    --admin-listen '$SERVER_ADMIN_LISTEN' \
    --diag-stats \
    --diag-stats-interval 1 \
    </dev/null >/home/jack/dgx-multi-if-server.log 2>&1 &
"
```

server 显式监听样例：

```bash
--listen "$REMOTE_IP_A:$QUIC_PORT,$REMOTE_IP_B:$QUIC_PORT"
```

client peer 列表模式样例：

```bash
"$BIN" client \
  --peer "$REMOTE_IP_A:$QUIC_PORT,$REMOTE_IP_B:$QUIC_PORT" \
  --connections 8 \
  --http-listen "127.0.0.1:$HTTP_PORT" \
  --socks-listen "127.0.0.1:$SOCKS_PORT" \
  --cert "$CERT_DIR/client/client.crt" \
  --key "$CERT_DIR/client/client.key" \
  --ca "$CERT_DIR/ca.crt" \
  --compress off \
  --tuning wan \
  --admin-listen "$CLIENT_ADMIN_LISTEN" \
  --diag-stats \
  --diag-stats-interval 1
```

client paths 模式样例：

```json
{
  "version": 1,
  "tls": {
    "cert": "cert/client/client.crt",
    "key": "cert/client/client.key",
    "ca": "cert/ca.crt"
  },
  "peers": [
    {
      "peer_id": "dgx-remote",
      "http_listen": "127.0.0.1:18080",
      "socks_listen": "127.0.0.1:19080",
      "compress": "off",
      "paths": [
        {
          "name": "path-a",
          "local": "10.201.1.1",
          "peer": "10.201.1.2:4433",
          "connections": 4
        },
        {
          "name": "path-b",
          "local": "10.201.2.1",
          "peer": "10.201.2.2:4433",
          "connections": 4
        }
      ]
    }
  ]
}
```

### 4.3 功能覆盖矩阵

| 编号 | 场景 | server 配置 | client 配置 | 负载 | 期望 |
|---|---|---|---|---|---|
| F01 | 单链路兼容 | `REMOTE_IP_A:4433` | `REMOTE_IP_A:4433`, `connections=4` | 4 条 iperf3/curl tunnel | 行为与既有单网口一致 |
| F02 | server 通配展开 | `0.0.0.0:4433` | `REMOTE_IP_A:4433,REMOTE_IP_B:4433` | 8 条 tunnel | server resolved listens 至少包含 A/B 两个数据 IP；如包含管理网地址需记录 |
| F03 | server 显式列表 | `REMOTE_IP_A:4433,REMOTE_IP_B:4433` | peer 列表 | 8 条 tunnel | 只监听 A/B，不监听管理网 |
| F04 | client peer 列表，无 paths | 显式列表 | `--peer A,B --connections 8` | 8 条 tunnel | slot 连接到两个 peer；本地出口由路由决定 |
| F05 | client paths，双链路对双链路 | 显式列表或通配 | `path-a=4`、`path-b=4` | 8 条以上 tunnel | slot `local/peer/path` 精确匹配配置 |
| F06 | path 权重 | 显式列表 | `path-a=6`、`path-b=2` | 32 条短 tunnel | `total_tunnels` 长期接近 3:1 |
| F07 | 单 tunnel 非聚合 | 显式列表 | `path-a=4`、`path-b=4` | 单条长 iperf3 | 单 tunnel 只落到一个 slot，不要求双网口同时增量 |
| F08 | 多 tunnel 聚合 | 显式列表 | `path-a=4`、`path-b=4` | 8 或 16 条并发 iperf3 | 两个数据网口都有明显 tx/rx 增量，总吞吐高于单 path |
| F09 | runtime admin 可观测 | 任意双链路 | paths | curl admin | connection JSON 包含 `path`、`local`、`peer` |
| F10 | 配置热更新 | 任意双链路 | admin PATCH 修改 paths | 短 tunnel | path 变化触发 peer 重启，旧 path 不残留 |

### 4.4 负载模型

基础吞吐使用 `iperf3`：

- 单 path 基线：`path-a` 跑 3 轮，每轮 60 秒。
- 双 path 聚合：`path-a + path-b` 跑 3 轮，每轮 60 秒。
- tunnel 并发：推荐 `8`、`16`、`32` 三档。
- 每个场景分别跑 download 和 upload；若只跑单方向，结果摘要必须说明。

短连接调度使用 `curl` 或 `k6`：

- 目标是制造大量短 TCP tunnel，验证 slot/path 分配，不把吞吐作为主要判断。
- 每轮至少 1000 个请求，统计 `total_tunnels` 和错误率。

netem 故障注入只放在数据发送端 egress：

- download：对端发送为主，对端对应数据网口加 netem。
- upload：本机发送为主，本机对应数据网口加 netem。
- 每轮开始前清理两端两个数据网口的 qdisc。

## 5. k6 性能基线

`k6` 用于验证大量短 HTTP CONNECT tunnel 的调度、错误率和建立延迟；大流量带宽基线仍以 `iperf3` 为准。

### 5.1 环境要求

- 本机运行 k6。
- 本机 `raypx2 client` 开启 HTTP CONNECT：`127.0.0.1:18080`。
- 对端运行轻量 HTTP 服务或 nginx，监听在数据面可达地址，例如 `10.201.1.2:18000` 和 `10.201.2.2:18000`。
- k6 通过 `HTTP_PROXY=http://127.0.0.1:18080` 访问对端 HTTP URL。

### 5.2 场景

| 场景 | 流量形态 | 目标 |
|---|---|---|
| baseline-short | 30 秒 ramp-up，3 分钟 steady，30 秒 ramp-down，50 VUs | 建立短 tunnel 基线 |
| peak-short | 30 秒 ramp-up，5 分钟 steady，200 VUs | 验证高频短连接 path 分配 |
| spike-short | 10 秒从 20 VUs 拉到 400 VUs，保持 60 秒 | 验证突增时无大量 dropped iterations |
| soak-short | 100 VUs 持续 30 分钟 | 验证长时间 reconnect 和 admin 可用性 |

### 5.3 指标和门禁

| 指标 | 门禁 |
|---|---|
| `http_req_failed` | `< 0.1%`，故障注入场景单独记录 |
| `http_req_duration p95` | 基线场景 `< 500ms`；netem 场景按注入延迟调整 |
| `checks` | `> 99.9%` |
| `dropped_iterations` | 基线和 peak 场景为 0；spike 场景必须记录 |
| path 分布 | `path-a:path-b` 接近配置连接数权重，允许 `±20%` 波动 |
| CPU | 任一 `raypx2` 进程不长期满核导致 admin 超时 |

示例执行：

```bash
HTTP_PROXY=http://127.0.0.1:18080 \
rtk k6 run docs/test/k6/multi-interface-short-tunnel.js
```

如果仓库未提供 k6 脚本，本测试设计仍要求保存等价的短连接压测脚本、配置和原始输出。

## 6. 容量和可扩展性验证

容量测试关注两个维度：QUIC connection slot 数量和 TCP tunnel 并发数量。

| 维度 | 测试档位 | 验收 |
|---|---|---|
| path 数 | 1、2 | 2 path 场景不低于 1 path 稳定性 |
| 每 path connection 数 | `1+1`、`4+4`、`8+8` | connected slot 数等于配置总和 |
| tunnel 并发 | 1、8、16、32 | 多 tunnel 场景能跨 path 分摊 |
| 长稳 | 30 分钟 soak | 无进程退出，无 admin 连续失败，无异常内存增长 |

吞吐结果按以下方式汇总：

```text
scenario,direction,paths,connections,total_tunnels,duration_sec,receiver_mbps,path_a_tx_mbps,path_b_tx_mbps,path_a_rx_mbps,path_b_rx_mbps,client_cpu_pct,server_cpu_pct,error_rate,notes
```

判定原则：

- 单 tunnel 场景只作为兼容和稳定性验证，不作为双网口聚合失败依据。
- 多 tunnel 场景如果双 path 总吞吐没有显著高于单 path，需要检查 CPU、单进程调度、MsQuic connection 数、iperf3 tunnel 数和 NIC 计数。
- 如果两个网口中只有一个有流量，优先检查 `paths.local` 是否生效、`ip route get from` 是否正确、server listener 是否覆盖对应 peer IP。

## 7. 异常条件和灾难恢复

| 编号 | 触发 | 注入故障 | 预期用户影响 | 检测信号 | 缓解和恢复 | 验收 |
|---|---|---|---|---|---|---|
| DR01 | path-a 运行中 | 对本机或对端 `enp1s0f0np0` 加 `loss 100%` | path-a 上新 tunnel 失败或延迟，path-b 继续服务 | path-a reconnect 增加，path-b connected | 清理 qdisc | 30 秒内 path-a 恢复 connected |
| DR02 | path-b 运行中 | 对 `enP2p1s0f0np0` 加 `delay 200ms loss 10%` | path-b 吞吐下降，path-a 基本不受影响 | qdisc、path-b RTT/吞吐变化 | 清理 qdisc | path-a tunnel 成功率保持 `> 99%` |
| DR03 | server 单 listener 不可达 | 停止 server 后仅用 path-a listen 重启 | path-b slot retry，path-a 可用 | server resolved listens 缺 path-b，client path-b last_error | 恢复双 listen | path-b slot 恢复 |
| DR04 | client local 配错 | 将 path-b local 改为不存在 IP | path-b slot `SetLocalAddr` 失败，path-a 可用 | client log/admin last_error 包含 path-b local | 修正配置并重启或 PATCH | 错误可定位到 path 名称和 local IP |
| DR05 | server 显式 listen 配错 | listen 包含本机不存在 IP | server 启动失败，不进入部分监听 | server 非 0 退出或启动失败日志 | 修正 listen | 不允许“只启动一部分 listener”误判成功 |
| DR06 | 进程重启 | 重启 client 或 server | 正在进行的 tunnel 中断 | 进程日志、iperf3 rc | supervisor 或脚本重启 | 新 tunnel 在恢复后可成功 |
| DR07 | 管理网保护 | 对数据网口加 netem | SSH 仍可用 | SSH 命令成功 | 清理 qdisc | 管理网未受影响 |

每个异常场景必须保存：

- 注入前后的 admin JSON。
- 注入前后的 `ip -s link show dev <iface>`。
- 注入前后的 `tc -s qdisc show dev <iface>`。
- client/server 日志。
- iperf3 或 k6 原始结果。
- 恢复动作和恢复耗时。

## 8. 可观测性和测试证据

每次完整执行创建独立结果目录：

```bash
RESULT_ROOT="${ROOT}/docs/dgx-multi-interface-quic-binding-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RESULT_ROOT"/{env,proxy,cases,admin,net,summary}
```

顶层证据：

```text
env/git-version.txt
env/msquic-version.txt
env/local-ip-addr.txt
env/remote-ip-addr.txt
env/local-route-from.txt
env/remote-route-from.txt
env/local-link-before.txt
env/remote-link-before.txt
proxy/client.log
proxy/server.log
summary/summary.csv
summary/summary.md
```

每个 case 保存：

```text
cases/<case-id>/
  run.env
  run.log
  iperf.stdout.json
  iperf.stderr.txt
  iperf.rc
  k6-summary.json
  client-admin-before.json
  client-admin-after.json
  server-admin-before.json
  server-admin-after.json
  local-ss.txt
  remote-ss.txt
  local-link-before.txt
  local-link-after.txt
  remote-link-before.txt
  remote-link-after.txt
  local-qdisc-before.txt
  local-qdisc-after.txt
  remote-qdisc-before.txt
  remote-qdisc-after.txt
```

admin 断言重点字段：

```json
{
  "path": "path-a",
  "local": "10.201.1.1",
  "peer": "10.201.1.2:4433",
  "state": "connected",
  "active_tunnels": 0,
  "total_tunnels": 128
}
```

## 9. 进入、退出和发布门禁

### 9.1 进入条件

- 本机和对端 `raypx2` 二进制可执行，版本一致或差异已记录。
- 本机和对端两条数据网口均 `UP`，IP 与本文档固定变量一致。
- `ip route get <peer> from <local>` 命中预期网口。
- `iperf3`、`curl`、`jq`、`tc`、`ss`、`ip` 可用。
- 两端无残留 `raypx2`、`iperf3` 和数据网口 qdisc。

### 9.2 退出条件

- 功能矩阵 F01-F10 全部执行并保存证据。
- 至少完成一轮单 path 和双 path 的 download/upload 吞吐对比。
- 至少完成 DR01、DR03、DR04、DR07 四个故障场景。
- 所有失败都有 case 目录、日志、admin JSON 和结论。
- 测试结束后两端 `raypx2`、`iperf3` 进程已清理，两个数据网口 qdisc 已清理。

### 9.3 发布门禁

阻断发布的问题：

- paths 模式下 slot 的 `local` 或 `peer` 与配置不一致。
- server `0.0.0.0` 没有展开到两个数据网口，或显式 listen 列表仍绑定了管理网地址。
- 任一 listener 启动失败后 server 仍进入部分监听成功状态。
- 单 path 故障导致健康 path 也无法新建 tunnel。
- admin 无法定位 path 名称、本地地址和远端地址。

非阻断但必须记录的问题：

- 双 path 吞吐未达到单 path 的 `1.6x`。
- k6 短连接 p95 超过暂定门禁，但 tunnel 成功率正常。
- 某个故障恢复时间超过 30 秒但最终恢复。

## 10. 风险、假设和开放问题

### 10.1 风险

- 历史 `169.254.0.0/16` 双链路配置存在 route metric、scope 和源地址选择风险；正式测试改用独立 `/30` 网段，并同时依赖普通 `ip route get`、`ip route get from` 和实际五元组证据确认。
- `paths.local` 只绑定源 IP，不自动创建策略路由；如果系统路由不支持该源地址到 peer 的出口，连接失败是预期行为。
- 单 TCP tunnel 不跨 QUIC connection 聚合，单流吞吐无法代表多网口聚合能力。
- k6 更适合短连接调度和错误率验证，不适合作为 200Gbps 带宽上限测试工具。
- 如果测试脚本使用固定端口，残留进程可能造成假失败；每轮前必须清理或记录端口占用。

### 10.2 假设

- `enp1s0f0np0` 和 `enP2p1s0f0np0` 是本轮唯一允许承载 QUIC 数据面的网口。
- 对端 `jack@172.16.10.81` 始终可作为控制面入口。
- 证书目录和远端二进制部署方式沿用既有 DGX 测试文档。
- 默认压缩关闭，避免压缩比影响吞吐判断。

### 10.3 开放问题

- 双 path 吞吐发布门禁是否应固定为 `1.6x`，还是后续用实测历史基线替换。
- 是否需要增加第三条运营商路径的等价验证；当前 DGX 环境只定义两条数据链路。
- 是否需要为本设计补充自动化执行脚本，还是先以人工执行和证据归档为准。
