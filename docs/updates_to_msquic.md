# 当前 MsQuic fork 相对上游的更新

本文记录 `third_party/msquic` 当前子模块相对 `microsoft/msquic` 上游 `main`
的项目定制更新。信息基于 2026-07-02 对 `origin` 和 `upstream` 执行 fetch 后的本地
Git 状态。

## 基准状态

| 项 | 值 |
| --- | --- |
| 当前 fork remote | `origin = git@github.com:jack2007/msquic.git` |
| 上游 remote | `upstream = https://github.com/microsoft/msquic.git` |
| 当前子模块 HEAD | `9f8380759` (`Tune BBR ProbeRTT behavior`) |
| 当前 upstream/main | `b63246910` (`[2/3] Integrate XSK maps into the core XDP datapath (#6003)`) |
| 最近共同祖先 | `40f08c6a3` (`Fix data race in connection pool by running QuicConnStart on the connection's worker thread (#6114)`) |
| 分叉关系 | upstream 侧多 2 个提交，fork 侧多 9 个提交；fork 侧包含 8 个非 merge 补丁和 1 个 merge 提交 |

上游当前比 fork 多出的提交：

| commit | 说明 |
| --- | --- |
| `07bbcb72e` | `Fix ServerAcceptContext smart pointer handling and lifetime (#6125)` |
| `b63246910` | `[2/3] Integrate XSK maps into the core XDP datapath (#6003)` |

fork 侧额外提交：

| commit | 说明 |
| --- | --- |
| `beb1bf76f` | `perf: raise ideal send buffer cap` |
| `159510a22` | `perf: raise send flush batch and expose diagnostics` |
| `67a7b2723` | `diag: expose loss detection network stats` |
| `b75c1f8dc` | `feat: add QUIC_DISABLE_XDP to skip Windows XDP datapath` |
| `f42ae7b74` | `platform: route allocator through mimalloc` |
| `1575610d2` | `core: avoid FIN-only receive without final size` |
| `2f245179d` | `Merge upstream main into fork` |
| `822524de6` | `Update DocFx after documentation changes.` |
| `9f8380759` | `Tune BBR ProbeRTT behavior` |

## 定制更新分类

### 高吞吐发送参数

相关提交：`beb1bf76f`、`159510a22`

- `src/core/quicdef.h`
  - `QUIC_MAX_IDEAL_SEND_BUFFER_SIZE` 从上游默认提高到 `0x80000000`
    （2 GiB）。
  - `QUIC_MAX_DATAGRAMS_PER_SEND` 从 `40` 提高到 `240`。
- 目的：降低高 BDP / 高吞吐场景下 MsQuic send path 的调度和缓冲上限约束，
  让 tcpquic-proxy 的大带宽隧道压测更容易填满链路。

### 发送路径诊断扩展

相关提交：`159510a22`

- `src/core/connection.h`
  - 在 `QUIC_CONN_STATS` 中新增 `SendDiag` 统计结构。
- `src/core/send.c`
  - 统计 send flush 进入 packet builder 的次数。
  - 统计 pacing delayed、congestion-control blocked、scheduling limited、
    anti-amplification blocked、no-work 等 send flush 结果。
  - 记录最后一次 flush 的 send allowance、path allowance、result、datagram 数和
    `OutFlowBlockedReasons`。
- `src/core/bbr.c` / `src/inc/msquic.h`
  - 将 BBR 状态、recovery 状态、pacing/cwnd gain、MinRTT、send quantum、
    app-limited 状态，以及 send flush 诊断字段暴露到
    `QUIC_CONNECTION_EVENT_NETWORK_STATISTICS` 的 `QUIC_NETWORK_STATISTICS`。
- 目的：让上层 proxy 可以通过 MsQuic network statistics 事件定位吞吐瓶颈，
  区分 pacing、拥塞控制、调度批次、放大限制或无可发送数据等原因。

### 丢包检测诊断扩展

相关提交：`67a7b2723`

- `src/core/loss_detection.c`
  - 统计 FACK packet-threshold loss、RACK time-threshold loss。
  - 累计包含 retransmittable bytes 的 loss detection batch 数。
  - 累计和记录最近一次 lost retransmittable bytes。
- `src/core/connection.h` / `src/core/bbr.c` / `src/inc/msquic.h`
  - 将上述 loss detection 统计纳入 `SendDiag` 并暴露到
    `QUIC_NETWORK_STATISTICS`。
- 目的：在高延迟、丢包、netem 矩阵和 DGX 双机测试中，能把吞吐下降与实际 loss
  detection 行为关联起来。

### Windows XDP 构建开关

相关提交：`b75c1f8dc`

- 顶层 `CMakeLists.txt`
  - 新增 `QUIC_DISABLE_XDP` 选项。
- `src/platform/CMakeLists.txt`
  - Windows 平台构建时，如果 `QUIC_DISABLE_XDP=ON`，跳过 XDP raw datapath 相关
    源文件和 include 路径。
- 目的：tcpquic-proxy 在 Windows 上使用 socket raw path，避免引入 XDP-for-Windows
  构建和运行依赖。

### MsQuic 平台 allocator 接入 mimalloc

相关提交：`f42ae7b74`

- `src/platform/CMakeLists.txt`
  - 当 `TCPQUIC_MSQUIC_MIMALLOC_ENABLED` 为真时，为 `msquic_platform` 添加
    `TCPQUIC_MSQUIC_USE_MIMALLOC=1`、mimalloc include 路径和 `mimalloc-static`
    链接。
- `src/platform/platform_posix.c`
  - `CxPlatAlloc` / `CxPlatAllocUninitialized` / `CxPlatFree` 分别改为可走
    `mi_zalloc` / `mi_malloc` / `mi_free`。
- `src/platform/platform_winuser.c`
  - Windows 用户态平台 allocator 同样可走 mimalloc；debug tag 偏移释放逻辑保留。
- `src/bin/CMakeLists.txt`
  - 静态归档时排除部分系统库名，避免 MsQuic archive flatten 误处理。
- 目的：让 tcpquic-proxy 在默认静态 mimalloc 模式下，项目自身 allocator、zstd、
  c-ares 和 vendored MsQuic 平台层 allocator 可以走同一显式 allocator 策略。

### FIN-only receive 修复

相关提交：`1575610d2`

- `src/core/stream_recv.c`
  - 在 `QuicStreamRecvFlush()` 的 FIN-only receive 分支中，如果
    `Stream->RecvMaxLength == UINT64_MAX`，不再向应用层上报 FIN。
- 背景：multi-receive 模式下，连续数据可能已经 pending 给应用，但 final size 仍未知；
  此时“没有未读数据”不足以证明 stream 已经收到 FIN。
- 目的：避免在 final size 未知时过早生成 FIN-only receive 事件。

### BBR ProbeRTT 调整

相关提交：`9f8380759`

- `src/core/bbr.c`
  - 新增 `kProbeRttCwndInMss = 20`。
  - BBR 在 `BBR_STATE_PROBE_RTT` 中的 cwnd 从原先 `4 * MSS` 调整为
    `20 * MSS`。
  - MinRTT 过期时间从 `10s` 调整为 `20s`。
- `src/core/unittest/BbrTest.cpp`
  - 更新 ProbeRTT 相关单元测试，增加 “11 秒不应过期” 的覆盖，并按 20 秒过期窗口调整
    进入/退出 ProbeRTT 的测试。
- 目的：减少高吞吐长肥管链路中 ProbeRTT 对发送窗口的冲击，降低周期性吞吐塌陷风险。

### DocFx 生成文档更新

相关提交：`822524de6`

- 新增大量 `msquicdocs/` 生成产物。
- 该提交主要是文档生成结果，不是 tcpquic-proxy 数据面或构建路径的核心定制逻辑。

## 与 tcpquic-proxy 的关系

这些 MsQuic fork 更新主要服务于当前项目的几个目标：

- 高 BDP / 高吞吐 TCP-over-QUIC 隧道：提高 send batch、send buffer cap 和 ProbeRTT
  保守程度。
- 可观测性：把 BBR、send flush 和 loss detection 细节暴露给上层 metrics/diagnostics，
  支撑 `docs/io-throughput-diag_cn.md` 和 `docs/test/` 下的 DGX/netem 分析。
- 部署一致性：通过 `QUIC_DISABLE_XDP` 避免 Windows XDP 依赖；通过 mimalloc patch
  与项目默认静态 allocator 策略保持一致。
- 正确性：修复 final size 未知时 FIN-only receive 过早上报的问题。

## 维护注意事项

1. 当前 fork 已落后 `upstream/main` 两个提交；下次更新 MsQuic 子模块时需要重新合并
   `07bbcb72e` 和 `b63246910` 之后的上游变化。
2. `QUIC_NETWORK_STATISTICS` 增加了非上游字段，属于 ABI/API surface 修改。更新上游时
   要重点检查 `src/inc/msquic.h`、`src/core/bbr.c` 和调用方对结构布局的假设。
3. `QUIC_MAX_DATAGRAMS_PER_SEND`、`QUIC_MAX_IDEAL_SEND_BUFFER_SIZE`、ProbeRTT cwnd/min-rtt
   过期时间属于吞吐优先的策略修改。低延迟、小带宽或公网复杂链路场景需要保留对照测试。
4. mimalloc patch 修改了 MsQuic 平台层分配/释放路径。任何上游 platform allocator 重构
   都需要重新确认 POSIX 和 Windows 两侧没有 allocator family 混用。
5. `QUIC_DISABLE_XDP` 会和上游 Windows/XDP datapath 演进产生冲突风险，尤其是当前 upstream
   已有 XSK/XDP 相关提交进入 `main`。
