# DGX Spark 双机 200Gbps 互联测试方案

## 1. 测试环境

| 项目 | 本机 (spark-1619) | 对端 (spark-1b6f) |
|------|-------------------|-------------------|
| 主机名 | spark-1619 | spark-1b6f |
| 管理网 IP | 172.16.10.80 | 172.16.10.81 |
| 200G 直连 IP | 169.254.250.230 | 169.254.59.196 |
| 200G 网卡 | enp1s0f0np0 | enp1s0f0np0 |
| 链路速率 | 200000 Mb/s (200 Gbps) | 200000 Mb/s |
| MTU | 9000 (Jumbo Frame) | 9000 |
| SSH | jack@169.254.59.196 免密 | jack@169.254.250.230 免密 |
| sudo | 免密 | 免密 |

物理拓扑：两台 DGX Spark 通过 200Gbps 线缆直连，使用 169.254.0.0/16 链路本地地址通信。

## 2. 测试方案概览

| 方案 | 名称 | 目的 | 脚本 |
|------|------|------|------|
| 方案一 | 链路连通性与健康检查 | 验证物理链路、L2/L3/L4 连通、SSH/sudo、接口错误计数 | `scripts/dgx-interconnect-scheme1-connectivity.sh` |
| 方案二 | 带宽与延迟性能基准 | 裸链路下测量 TCP/UDP 吞吐、双向带宽、RTT | `scripts/dgx-interconnect-scheme2-performance.sh` |
| 方案三 | 对端 netem 时延/丢包模拟 | 在对端 200G 网卡施加 netem，验证 RTT/丢包/降速 | `scripts/dgx-interconnect-scheme3-netem.sh` |

统一入口：`scripts/run-dgx-interconnect-tests.sh`

### netem 操作原则

- **本机** `enp1s0f0np0`：**禁止** netem，测试前自动清除
- **对端** `enp1s0f0np0`：时延/丢包模拟**仅在此网卡**操作（`sudo tc qdisc replace dev enp1s0f0np0 root netem ...`）
- 方案二测裸链路前会清除两端 netem；方案三在对端施加 netem，测毕自动清除

### netem 队列深度（`limit`）与 UDP 假性丢包

`tc netem` 默认队列长度（`limit`）为 **1000**。在 200Gbps 这类高速链路上，一旦叠加**时延**（`delay`），报文会在 netem 队列中积压；队列仅 1000 个包时很快溢出，后续包被直接丢弃，表现为 **UDP 丢包率异常偏高**——这往往并非链路或网卡真实丢包，而是 **netem 队列长度不足** 造成的假性丢包。

**处理方式**：施加 netem 时必须显式增大队列深度，例如：

```bash
sudo tc qdisc replace dev enp1s0f0np0 root netem delay 100ms loss 5% limit 1000000
```

本仓库测试脚本（`dgx-interconnect-netem-common.sh`、`bench-tcpquic-proxy-dgx.sh`）默认使用 **`limit 1000000`**（环境变量 `NETEM_LIMIT`）。若在手工配置 netem 时省略 `limit` 或沿用默认 1000，200G 线速 + 延迟场景下 UDP/iperf 测试结果不可信。

## 3. 方案一：链路连通性与健康检查

### 3.1 测试目标

确认 200G 直连链路在链路层、网络层、传输层均可用，远程运维通道（SSH/sudo）正常，接口无异常错误计数。

### 3.2 测试用例

| 用例 ID | 名称 | 步骤 | 通过标准 |
|---------|------|------|----------|
| TC-1.1 | 链路层状态 | 本端与对端 `ethtool` 检查 Speed/Duplex/Link | Speed=200000Mb/s，Duplex=Full，Link detected=yes |
| TC-1.2 | 路由与邻居 | `ip route get` + `ip neigh` | 流量走 enp1s0f0np0，邻居 MAC 可达 |
| TC-1.3 | ICMP 连通 | `ping -c 20 -i 0.2` | 0% 丢包（允许 netem 影响时延，但不允许 100% 不可达） |
| TC-1.4 | SSH 免密 | `ssh BatchMode` 执行 hostname | 退出码 0，返回对端主机名 |
| TC-1.5 | 对端 sudo 免密 | 远程 `sudo -n true` | 退出码 0 |
| TC-1.6 | TCP 端口探测 | `nc -z -w 3` 对 iperf3 端口 | 连接成功 |
| TC-1.7 | Jumbo MTU | `ping -M do -s 8972`（9000-20-8） | 无 fragmentation needed 错误 |
| TC-1.8 | 接口错误计数 | `ip -s link` + `ethtool -S` 关键计数器 | errors=0，无持续增长 |

### 3.3 执行命令

```bash
./scripts/dgx-interconnect-scheme1-connectivity.sh
```

## 4. 方案二：带宽与延迟性能基准

### 4.1 测试目标

在**无 netem 干扰**条件下，测量 200G 直连链路的实际吞吐与 RTT，建立性能基线。脚本会清除本机与对端 200G 网卡上的 netem。

### 4.2 测试用例

| 用例 ID | 名称 | 步骤 | 通过标准 |
|---------|------|------|----------|
| TC-2.1 | RTT 基线 | `fping -C 50` 或 `ping` 统计 | 平均 RTT < 1ms（无 netem 时） |
| TC-2.2 | TCP 单流吞吐 | iperf3 单流 10s，对端 server | ≥ 10 Gbps（保守下限；理论上限接近 200G） |
| TC-2.3 | TCP 多流吞吐 | iperf3 `-P 8` 10s | ≥ 50 Gbps |
| TC-2.4 | 反向吞吐 | 对端作 client，本机 server | 与 TC-2.3 同量级 |
| TC-2.5 | UDP 吞吐 | iperf3 `-u -b 0` 10s | ≥ 10 Gbps（裸链路；若对端存在 `limit 1000` 的 netem，UDP 假性丢包可忽略） |
| TC-2.6 | 双向吞吐 | iperf3 `--bidir -P 4` 10s | 合计 ≥ 50 Gbps |

### 4.3 执行命令

```bash
./scripts/dgx-interconnect-scheme2-performance.sh
```

环境变量：

| 变量 | 默认 | 说明 |
|------|------|------|
| LOCAL_IP | 169.254.250.230 | 本机 200G IP |
| PEER_IP | 169.254.59.196 | 对端 200G IP |
| PEER | jack@169.254.59.196 | SSH 目标 |
| IFACE | enp1s0f0np0 | 200G 网卡 |
| IPERF_PORT | 15201 | iperf3 端口 |
| DURATION | 10 | 每项测试秒数 |
| MIN_TCP_Gbps | 10 | TCP 单流最低通过线 |
| MIN_TCP_PAR_Gbps | 50 | TCP 多流最低通过线 |
| MIN_BIDIR_Gbps | 50 | 双向最低通过线 |

## 5. 方案三：对端 netem 时延/丢包模拟

### 5.1 测试目标

在对端 `enp1s0f0np0`（200G 网卡，IP 169.254.59.196）施加 `tc netem`，验证时延与丢包模拟生效，且本机网卡保持无 netem。

### 5.2 测试用例

| 用例 ID | 名称 | 步骤 | 通过标准 |
|---------|------|------|----------|
| TC-3.1 | 本机无 netem | 检查本机 `tc qdisc` | 本机 enp1s0f0np0 无 netem |
| TC-3.2 | 对端施加 netem | 对端 `tc qdisc replace ... netem delay 100ms loss 5% limit 1000000` | qdisc 显示 netem 规则且 `limit 1000000` |
| TC-3.3 | 时延验证 | ping 30 次 | 平均 RTT ≥ 80ms |
| TC-3.4 | 丢包验证 | ping 100 次 | 丢包率 ≥ 2% |
| TC-3.5 | 降速验证 | 对端 iperf client → 本机 server | 吞吐 < 30 Gbps |
| TC-3.6 | 清除 netem | 对端 `tc qdisc del` | 对端无 netem 残留 |

### 5.3 执行命令

```bash
# 脚本默认 NETEM_LIMIT=1000000，勿使用 netem 默认 limit 1000
DELAY=100ms LOSS=5% NETEM_LIMIT=1000000 ./scripts/dgx-interconnect-scheme3-netem.sh
```

| 变量 | 默认 | 说明 |
|------|------|------|
| NETEM_LIMIT | 1000000 | netem 队列深度；默认 1000 在高速+延迟下会导致 UDP 假性大量丢包 |
| DELAY | 100ms | 模拟时延 |
| LOSS | 5% | 模拟丢包率 |

## 6. 结果输出

- 控制台：每项 PASS/FAIL 与实测值
- 报告文件：`/tmp/dgx-interconnect-report-<timestamp>.txt`

## 7. 全部执行

```bash
./scripts/run-dgx-interconnect-tests.sh
```
