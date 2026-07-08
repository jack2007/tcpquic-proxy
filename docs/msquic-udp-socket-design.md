# MsQuic UDP Socket 与 Datapath Partition 设计说明

本文记录截至 2026-07-08 对本仓库 vendored MsQuic 与上游 MsQuic `main` 的确认结果。当前本仓库
`third_party/msquic` 固定在 `13c87cdb2c2008bde01f90356f4fe37578f99e41`。

## 结论摘要

- Linux 上 MsQuic 支持并自动使用 `SO_REUSEPORT`。server/listener UDP binding 在 datapath partition 数大于 1 时会创建多个 UDP socket，并在 `bind()` 前设置 `SO_REUSEPORT`。
- Linux 的 `SO_ATTACH_REUSEPORT_CBPF` 是 best-effort RSS 辅助；失败不会阻止服务启动，只会退化为非 RSS 对齐的 socket 分发。
- Windows 上 MsQuic 不使用 `SO_REUSEPORT`，而是使用 Winsock `SIO_CPU_AFFINITY` 创建 per-processor UDP sockets，实现同地址同端口 UDP socket 的 RSS 分发。
- macOS/Darwin 当前没有等价的多接收者 datapath。MsQuic kqueue datapath 明确把 `PartitionCount` 固定为 1。
- Datapath partition 数不是编译时常量。Linux/Windows 默认在运行时按 worker pool/CPU 数确定，初始化后固定；也可通过全局 execution config 或持久化 `MaxPartitionCount` 影响。

## Linux：`SO_REUSEPORT` 行为

Linux `epoll` datapath 在 `CxPlatSocketContextInitialize()` 中设置 UDP socket 选项。对于没有固定 remote address 的 server socket，且 datapath partition 数大于 1 时，代码会设置：

```c
setsockopt(SocketContext->SocketFd, SOL_SOCKET, SO_REUSEPORT, ...);
```

本地代码位置：

- `third_party/msquic/src/platform/datapath_epoll.c:721`
- `third_party/msquic/src/platform/datapath_iouring.c:900`

触发条件可以概括为：

```c
(Config->Flags & CXPLAT_SOCKET_FLAG_SHARE || Config->RemoteAddress == NULL) &&
SocketContext->Binding->Datapath->PartitionCount > 1
```

因此，对普通 server/listener 场景，不需要应用层额外配置 `SO_REUSEPORT`。这是 MsQuic datapath 内部的自动行为。如果 `setsockopt(SO_REUSEPORT)` 失败，当前 socket 初始化会失败并返回错误。

Linux 还会尝试附加 `SO_ATTACH_REUSEPORT_CBPF`：

- `third_party/msquic/src/platform/datapath_linux.c:194`
- `third_party/msquic/src/platform/datapath_epoll.c:1153`

这段 BPF 逻辑按 CPU number 对 socket count 取模，用于更贴近 CPU/RSS 分发。调用方忽略返回值；如果内核或构建环境不支持，MsQuic 仍继续运行，socket 分发会退化为内核默认的 reuseport 行为。

## Windows：`SIO_CPU_AFFINITY`

Windows user-mode datapath 的 server UDP socket 创建路径会在 partition 数大于 1 时创建 per-processor UDP sockets：

- `third_party/msquic/src/platform/datapath_winuser.c:1216`
- `third_party/msquic/src/platform/datapath_winuser.c:1224`

每个 socket 在 `bind()` 前调用 `WSAIoctl(..., SIO_CPU_AFFINITY, ...)`：

- `third_party/msquic/src/platform/datapath_winuser.c:1377`

Microsoft Winsock 文档对 `SIO_CPU_AFFINITY` 的语义是：启用端口共享和接收并行化；同一地址绑定多个 socket 后，接收会按 RSS hash 分发到不同 socket；同一个 flow 仍固定到同一个 socket。该 IOCTL 只支持 UDP，且必须在 `bind()` 前调用。

这意味着 Windows 上不需要、也不能按 Linux 方式依赖 `SO_REUSEPORT`。如果目标是提升 server 端多连接吞吐，应优先确认：

- RSS/UDP RSS 在网卡和系统层面可用；
- MsQuic worker/partition 数没有被 execution config 或配置上限压低；
- 应用 workload 不是单一 UDP 4-tuple/单一 QUIC connection 的瓶颈。

如果瓶颈来自单个 QUIC connection 或单个 UDP flow，多 socket/RSS 不会把同一个 flow 拆散。需要从应用层使用多条 QUIC connection、多个 client source port、多个 listen IP/port，或前置支持 QUIC CID 路由的负载均衡。

## macOS/Darwin：当前固定单接收者

Darwin kqueue datapath 中，代码直接设置：

```c
Datapath->PartitionCount = 1; // Darwin only supports a single receiver
```

本地代码位置：

- `third_party/msquic/src/platform/datapath_kqueue.c:475`

因此在当前 MsQuic 实现中，macOS/Darwin 不会像 Linux/Windows 那样创建多个同端口 UDP receiver。可行绕法主要在部署和应用层：

- 使用多端口或多 IP 分片；
- 使用多条 QUIC connection 分散流量；
- 使用外部负载均衡或 QUIC CID 路由；
- 对高吞吐单端口服务端，优先选择 Linux，Windows 次之。

## Datapath Partition 数如何确定

MsQuic 有两个相关概念：

- core library 的 `MsQuicLib.PartitionCount`；
- platform datapath 的 `Datapath->PartitionCount`。

默认情况下，它们都来自运行时 CPU/worker 数，而不是编译期常量。

### Worker pool 默认按运行时 CPU 数创建

`CxPlatWorkerPoolCreate()` 的默认逻辑是：

```c
if (Config && Config->ProcessorCount) {
    ProcessorCount = Config->ProcessorCount;
    ProcessorList = Config->ProcessorList;
} else {
    ProcessorCount = CxPlatProcCount();
}
```

本地代码位置：

- `third_party/msquic/src/platform/platform_worker.c:267`

`CxPlatProcCount()` 是平台运行时探测值：

- Linux/POSIX 非 Darwin：`sysconf(_SC_NPROCESSORS_ONLN)`，见 `third_party/msquic/src/platform/platform_posix.c:99`；
- Windows user-mode：按 active processor group 信息累计 active processor count，见 `third_party/msquic/src/platform/platform_winuser.c:90`；
- Darwin：当前强制为 1，见 `third_party/msquic/src/platform/platform_posix.c:104`。

### Core partition count 可被运行时配置影响

`QuicLibraryInitializePartitions()` 初始使用 `CxPlatProcCount()`，随后可能被 worker pool、`QUIC_GLOBAL_EXECUTION_CONFIG` 或持久化配置裁剪：

- 默认：`MsQuicLib.PartitionCount = (uint16_t)CxPlatProcCount()`；
- 如果已有 custom worker pool，则使用 `CxPlatWorkerPoolGetCount(MsQuicLib.WorkerPool)`；
- 如果设置了 `QUIC_GLOBAL_EXECUTION_CONFIG.ProcessorCount`，使用该 processor count 和 processor list；
- 否则受持久化 `MaxPartitionCount` 限制，默认最大值为 `QUIC_MAX_PARTITION_COUNT`，当前是 512。

本地代码位置：

- `third_party/msquic/src/core/library.c:140`
- `third_party/msquic/src/inc/msquic.h:287`
- `third_party/msquic/src/core/quicdef.h:151`
- `third_party/msquic/src/core/quicdef.h:667`

### Platform datapath partition count

Linux/Windows datapath 使用 worker pool count：

- Linux epoll：`Datapath->PartitionCount = (uint16_t)CxPlatWorkerPoolGetCount(WorkerPool)`，见 `third_party/msquic/src/platform/datapath_epoll.c:247`；
- Linux io_uring：同上，见 `third_party/msquic/src/platform/datapath_iouring.c:449`；
- Windows user-mode：同上，见 `third_party/msquic/src/platform/datapath_winuser.c:670`；
- Darwin kqueue：固定为 1，见 `third_party/msquic/src/platform/datapath_kqueue.c:475`。

注意：这些值是在 MsQuic library/datapath 初始化时确定的。运行过程中 CPU hotplug 或 affinity 变化不会自动重建 partition/socket 拓扑。

## 对 tcpquic-proxy 的设计含义

1. Linux server 端监听单个 UDP 端口时，MsQuic 已经自动利用 `SO_REUSEPORT` 和 per-processor sockets；应用层不需要自己打开多个 UDP socket。
2. Windows server 端依赖 MsQuic 的 `SIO_CPU_AFFINITY` 路径和系统 RSS。性能排查应关注 RSS、网卡队列、CPU 亲和性、worker 数、单 flow 热点，而不是寻找 `SO_REUSEPORT`。
3. macOS/Darwin 不适合作为高吞吐单端口 QUIC 服务端的主要部署目标。若必须支持，应通过多连接、多端口、多 IP 或外部负载均衡分摊。
4. 如果压测只使用单条 QUIC connection 或单个 client UDP 4-tuple，无法验证多 socket/RSS 的收益。压测需要覆盖多连接、多 source port、多客户端或负载均衡后的真实分布。
