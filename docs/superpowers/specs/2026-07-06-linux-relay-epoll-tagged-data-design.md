# Linux Relay epoll 标记数据设计

## 背景

`TqLinuxRelayWorker` 同一个 epoll 实例同时监听两类事件：

- worker wake eventfd，用于唤醒 worker drain 控制队列。
- TCP relay fd，用于处理 TCP read/write/HUP/error。

当前实现把两类数据放进同一个 `epoll_event::data` 联合体：

- wake event 使用 `event.data.fd = WakeFd`。
- relay event 使用 `event.data.u64 = relay->Id`。
- 事件循环使用 `events[i].data.fd == WakeFd` 判断是否为 wake event。

这会让 `WakeFd` 和 `relay->Id` 落入同一个整数空间。现场问题中 `WakeFd == 30` 且存在 `relay_id == 30`，relay 的 `EPOLLHUP|EPOLLRDHUP` 被误判成 wake event。worker 只 drain eventfd，`read(WakeFd)` 得到 `EAGAIN`，没有执行 `ProcessTcpEvents()` 注销关闭的 relay，epoll 立即再次返回同一个事件，造成 CPU 100% busy loop。

## 目标

把 Linux relay worker 的 epoll data 编码成带 tag 的 `uint64_t`，从数据结构层面隔离 wake event 和 relay event，避免 fd 与 relay id 数值碰撞。

## 非目标

- 不改变 Darwin、Windows relay worker。
- 不改 relay 调度策略、buffer 策略、backpressure 策略。
- 不新增运行时配置项。
- 不修改 admin API 响应格式。

## 推荐方案

使用 `uint64_t` 的最高位作为 event type tag：

- `0xxx...`：relay event，低 63 bit 存 relay id。
- `1xxx...`：worker internal event，当前只定义 wake event。

具体编码：

```cpp
namespace {
constexpr uint64_t kLinuxRelayEpollTagMask = 1ull << 63;
constexpr uint64_t kLinuxRelayEpollWake = kLinuxRelayEpollTagMask;
constexpr uint64_t kLinuxRelayEpollRelayMask = ~kLinuxRelayEpollTagMask;

uint64_t EncodeLinuxRelayEpollWake() {
    return kLinuxRelayEpollWake;
}

uint64_t EncodeLinuxRelayEpollRelay(uint64_t relayId) {
    return relayId & kLinuxRelayEpollRelayMask;
}

bool IsLinuxRelayEpollWake(uint64_t value) {
    return value == kLinuxRelayEpollWake;
}

bool DecodeLinuxRelayEpollRelay(uint64_t value, uint64_t& relayId) {
    if ((value & kLinuxRelayEpollTagMask) != 0) {
        return false;
    }
    relayId = value;
    return relayId != 0;
}
}
```

relay id 分配保持从 `1` 开始递增，但必须保证不会设置最高位：

```cpp
uint64_t TqLinuxRelayWorker::AllocateRelayId() {
    if (NextRelayId == 0 || (NextRelayId & kLinuxRelayEpollTagMask) != 0) {
        NextRelayId = 1;
    }
    return NextRelayId++;
}
```

生产环境实际不可能自然跑到 `2^63` 个 relay，但这个保护让编码不变量明确，也便于单测覆盖。

## 数据流

### Start / StartForTest

创建 wake eventfd 后，注册 epoll event：

```cpp
event.events = EPOLLIN;
event.data.u64 = EncodeLinuxRelayEpollWake();
epoll_ctl(EpollFd, EPOLL_CTL_ADD, WakeFd, &event);
```

### RegisterRelayWithIdLocal

分配 relay id 后，注册 TCP fd：

```cpp
relay->Id = AllocateRelayId();
event.data.u64 = EncodeLinuxRelayEpollRelay(relay->Id);
epoll_ctl(EpollFd, EPOLL_CTL_ADD, registration.TcpFd, &event);
```

### Run

事件循环只读取 `data.u64`，不再读取 `data.fd`：

```cpp
const uint64_t epollData = events[i].data.u64;
if (IsLinuxRelayEpollWake(epollData)) {
    DrainWakeFd();
    DrainEvents(Config.EventBudget);
    continue;
}

uint64_t relayId = 0;
if (DecodeLinuxRelayEpollRelay(epollData, relayId)) {
    ProcessTcpEvents(relayId, events[i].events);
}
```

如果将来新增 internal event，可以继续使用最高位为 `1` 的编码空间，例如 `kLinuxRelayEpollTimer = kLinuxRelayEpollTagMask | 2`。当前只实现 wake event，避免过度设计。

## 错误处理

- `DecodeLinuxRelayEpollRelay()` 遇到未知 internal tag 时返回 false，事件循环忽略该事件并继续处理其他事件。
- relay id 为 `0` 视为非法，避免误处理未初始化数据。
- `WakeFd` drain 逻辑保持现状：读取到 `EAGAIN/EWOULDBLOCK` 即停止。修复点不是改变 eventfd drain，而是确保只有真正的 wake event 才进入该分支。

## 测试策略

### 单元测试

新增 Linux relay worker epoll data 编码单元测试，覆盖：

- wake 编码可被识别为 wake。
- relay id `30` 编码后不被识别为 wake。
- relay id `30` 解码后仍是 `30`。
- relay id 最高位不会被用于 relay 编码。

### 回归测试

在 `linux_relay_worker_io_test.cpp` 增加一个针对事故模式的测试：

1. 启动 `TqLinuxRelayWorker`。
2. 通过测试入口获得或构造 wake fd 与 relay id 数值碰撞场景。
3. 注册 relay id 等于 wake fd 数值的 TCP fd。
4. 关闭 peer，触发 `EPOLLHUP/EPOLLRDHUP`。
5. 验证 worker 处理 TCP event，而不是误走 wake drain 分支。

为了让这个测试稳定，不依赖系统分配出特定 fd，增加测试专用 helper：

```cpp
#if defined(TQ_UNIT_TESTING)
uint64_t EncodeEpollRelayForTest(uint64_t relayId);
uint64_t EncodeEpollWakeForTest();
bool IsEpollWakeForTest(uint64_t value);
bool DecodeEpollRelayForTest(uint64_t value, uint64_t& relayId);
#endif
```

同时增加 `DispatchEncodedEpollEventForTest(uint64_t epollData, uint32_t events)`，直接测试 Run 中的分发逻辑。这样不需要依赖真实 fd 数值碰撞，测试更确定。

## 验收标准

- `TqLinuxRelayWorker::Run()` 不再读取 `events[i].data.fd`。
- wake event 和 relay event 的 epoll data 编码空间互斥。
- relay id 与 wake fd 数值相等时，relay event 仍进入 `ProcessTcpEvents()`。
- `tcpquic_linux_relay_worker_queue_test` 通过。
- `tcpquic_linux_relay_worker_io_test` 通过。
- 修复版进程在同类现场状态下不再出现 `epoll_wait -> read(WakeFd)=EAGAIN` 的高频 busy loop。

