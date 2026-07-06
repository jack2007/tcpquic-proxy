# Linux Relay epoll Tagged Data Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 Linux relay worker 中 wake fd 与 relay id 数值碰撞导致的 epoll busy loop。

**Architecture:** 将 `epoll_event::data.u64` 统一编码为带 tag 的 64 位值，最高位标识 internal event，低 63 位承载 relay id。事件循环只解析 `data.u64`，不再混用 `data.fd` 与 `data.u64`。

**Tech Stack:** C++17、Linux epoll/eventfd、现有 `TqLinuxRelayWorker`、现有 CMake 单元测试。

---

## 文件结构

- 修改：`src/tunnel/linux_relay_worker.cpp`
  - 增加 epoll data 编码/解码 helper。
  - wake fd 注册改用 tagged wake data。
  - relay fd 注册和修改改用 tagged relay data。
  - Run 事件循环改用 tagged decode 分发。
  - 增加测试专用 encoded dispatch helper 的实现。
- 修改：`src/tunnel/linux_relay_worker.h`
  - 在 `TQ_UNIT_TESTING` 下声明测试专用 helper。
- 修改：`src/tunnel/linux_relay_event_queue.h`
  - 增加 encoded epoll dispatch 测试控制事件类型。
- 修改：`src/unittest/linux_relay_worker_io_test.cpp`
  - 增加事故模式回归测试。
- 修改：`src/unittest/linux_relay_worker_queue_test.cpp`
  - 增加编码 helper 的纯逻辑测试。
- 修改：`src/CMakeLists.txt`
  - 给 `tcpquic_linux_relay_worker_queue_test` 增加 `TQ_UNIT_TESTING=1`，确保测试声明和 cpp 实现的条件编译一致。

## Task 1: 增加 epoll data 编码 helper

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`

- [ ] **Step 1: 写入编码 helper**

在匿名 namespace 中加入：

```cpp
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
```

- [ ] **Step 2: 运行格式/编译检查**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test
```

Expected: 编译通过，行为还未改变。

## Task 2: 暴露测试专用编码接口

**Files:**
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/CMakeLists.txt`

- [ ] **Step 1: 在 header 中添加测试声明**

在 `TqLinuxRelayWorker` 的 `#if defined(TQ_UNIT_TESTING)` 区域添加：

```cpp
    uint64_t EncodeEpollWakeForTest() const;
    uint64_t EncodeEpollRelayForTest(uint64_t relayId) const;
    bool IsEpollWakeForTest(uint64_t value) const;
    bool DecodeEpollRelayForTest(uint64_t value, uint64_t& relayId) const;
```

- [ ] **Step 2: 在 cpp 中添加实现**

```cpp
#if defined(TQ_UNIT_TESTING)
uint64_t TqLinuxRelayWorker::EncodeEpollWakeForTest() const {
    return EncodeLinuxRelayEpollWake();
}

uint64_t TqLinuxRelayWorker::EncodeEpollRelayForTest(uint64_t relayId) const {
    return EncodeLinuxRelayEpollRelay(relayId);
}

bool TqLinuxRelayWorker::IsEpollWakeForTest(uint64_t value) const {
    return IsLinuxRelayEpollWake(value);
}

bool TqLinuxRelayWorker::DecodeEpollRelayForTest(uint64_t value, uint64_t& relayId) const {
    return DecodeLinuxRelayEpollRelay(value, relayId);
}
#endif
```

- [ ] **Step 3: 给 queue 测试目标开启 TQ_UNIT_TESTING**

在 `src/CMakeLists.txt` 的 `tcpquic_linux_relay_worker_queue_test` 配置中，`target_link_libraries(...)` 后添加：

```cmake
    target_compile_definitions(tcpquic_linux_relay_worker_queue_test PRIVATE TQ_UNIT_TESTING=1)
```

- [ ] **Step 4: 编译测试目标**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test
```

Expected: 编译通过。

## Task 3: 添加编码逻辑单元测试

**Files:**
- Modify: `src/unittest/linux_relay_worker_queue_test.cpp`

- [ ] **Step 1: 添加编码逻辑测试**

在 `main()` 的第一个测试块前增加：

```cpp
    {
        TqLinuxRelayWorker worker(TqLinuxRelayWorkerConfig{});
        const uint64_t wake = worker.EncodeEpollWakeForTest();
        const uint64_t relayThirty = worker.EncodeEpollRelayForTest(30);
        if (!worker.IsEpollWakeForTest(wake)) return 131;
        if (worker.IsEpollWakeForTest(relayThirty)) return 132;

        uint64_t decoded = 0;
        if (!worker.DecodeEpollRelayForTest(relayThirty, decoded)) return 133;
        if (decoded != 30) return 134;
        if (worker.DecodeEpollRelayForTest(wake, decoded)) return 135;
        if (worker.DecodeEpollRelayForTest(0, decoded)) return 136;
    }
```

- [ ] **Step 2: 运行测试确认通过**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test && rtk build/src/tcpquic_linux_relay_worker_queue_test
```

Expected: 编译通过，测试进程 exit code 0。

- [ ] **Step 3: 提交编码 helper 和测试**

```bash
rtk git add src/CMakeLists.txt src/tunnel/linux_relay_worker.cpp src/tunnel/linux_relay_worker.h src/unittest/linux_relay_worker_queue_test.cpp
rtk git commit -m "test: cover linux relay epoll data tagging"
```

## Task 4: 将 wake 和 relay 注册改为 tagged data

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/unittest/linux_relay_worker_queue_test.cpp`

- [ ] **Step 1: 修改 Start wake 注册**

把 `Start()` 中：

```cpp
event.data.fd = WakeFd;
```

改为：

```cpp
event.data.u64 = EncodeLinuxRelayEpollWake();
```

- [ ] **Step 2: 修改 StartForTest wake 注册**

把 `StartForTest()` 中：

```cpp
event.data.fd = WakeFd;
```

改为：

```cpp
event.data.u64 = EncodeLinuxRelayEpollWake();
```

- [ ] **Step 3: 修改 relay fd 注册**

把 `RegisterRelayWithIdLocal()` 中：

```cpp
relay->Id = NextRelayId++;
event.data.u64 = relay->Id;
```

改为：

```cpp
relay->Id = AllocateRelayId();
event.data.u64 = EncodeLinuxRelayEpollRelay(relay->Id);
```

- [ ] **Step 4: 修改 relay fd epoll modify**

查找：

```cpp
event.data.u64 = relay->Id;
```

保留语义但改为：

```cpp
event.data.u64 = EncodeLinuxRelayEpollRelay(relay->Id);
```

- [ ] **Step 5: 新增 AllocateRelayId**

在 `TqLinuxRelayWorker` 私有方法中声明并实现：

```cpp
uint64_t TqLinuxRelayWorker::AllocateRelayId() {
    if (NextRelayId == 0 || (NextRelayId & kLinuxRelayEpollTagMask) != 0) {
        NextRelayId = 1;
    }
    return NextRelayId++;
}
```

Header 中 private 区域添加：

```cpp
    uint64_t AllocateRelayId();
```

- [ ] **Step 6: 增加 allocator 测试入口**

在 `TqLinuxRelayWorker` 的 `#if defined(TQ_UNIT_TESTING)` public 区域添加：

```cpp
    void SetNextRelayIdForTest(uint64_t nextRelayId);
    uint64_t AllocateRelayIdForTest();
```

在 cpp 中添加：

```cpp
#if defined(TQ_UNIT_TESTING)
void TqLinuxRelayWorker::SetNextRelayIdForTest(uint64_t nextRelayId) {
    NextRelayId = nextRelayId;
}

uint64_t TqLinuxRelayWorker::AllocateRelayIdForTest() {
    return AllocateRelayId();
}
#endif
```

- [ ] **Step 7: 增加 allocator 边界测试**

在 `src/unittest/linux_relay_worker_queue_test.cpp` 的编码逻辑测试块后添加：

```cpp
    {
        TqLinuxRelayWorker worker(TqLinuxRelayWorkerConfig{});

        worker.SetNextRelayIdForTest(0);
        if (worker.AllocateRelayIdForTest() != 1) return 137;

        worker.SetNextRelayIdForTest(1ull << 63);
        if (worker.AllocateRelayIdForTest() != 1) return 138;

        worker.SetNextRelayIdForTest((1ull << 63) - 1);
        if (worker.AllocateRelayIdForTest() != ((1ull << 63) - 1)) return 139;
        if (worker.AllocateRelayIdForTest() != 1) return 140;
    }
```

- [ ] **Step 8: 编译并运行 queue 测试**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test && rtk build/src/tcpquic_linux_relay_worker_queue_test
```

Expected: 编译通过，测试进程 exit code 0。

- [ ] **Step 9: 编译 IO 测试**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test
```

Expected: 编译通过。

## Task 5: 修改 Run 分发逻辑

**Files:**
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/tunnel/linux_relay_worker.h`

- [ ] **Step 1: 抽取 wake drain helper**

新增私有方法：

```cpp
void TqLinuxRelayWorker::DrainWakeFd() {
    uint64_t value = 0;
    while (::read(WakeFd, &value, sizeof(value)) > 0) {
    }
}
```

Header private 区域添加：

```cpp
    void DrainWakeFd();
```

- [ ] **Step 2: 抽取 encoded epoll dispatch helper**

新增私有方法：

```cpp
bool TqLinuxRelayWorker::DispatchEncodedEpollEvent(uint64_t epollData, uint32_t events) {
    if (IsLinuxRelayEpollWake(epollData)) {
        DrainWakeFd();
        DrainEvents(Config.EventBudget);
        return true;
    }

    uint64_t relayId = 0;
    if (DecodeLinuxRelayEpollRelay(epollData, relayId)) {
        ProcessTcpEvents(relayId, events);
        return true;
    }
    return false;
}
```

Header private 区域添加：

```cpp
    bool DispatchEncodedEpollEvent(uint64_t epollData, uint32_t events);
```

- [ ] **Step 3: 修改 Run**

把：

```cpp
if (events[i].data.fd == WakeFd) {
    uint64_t value = 0;
    while (::read(WakeFd, &value, sizeof(value)) > 0) {
    }
    DrainEvents(Config.EventBudget);
} else {
    ProcessTcpEvents(events[i].data.u64, events[i].events);
}
```

改为：

```cpp
DispatchEncodedEpollEvent(events[i].data.u64, events[i].events);
```

- [ ] **Step 4: 确认不再混用 data.fd**

Run:

```bash
rtk rg -n "data\\.fd" src/tunnel/linux_relay_worker.cpp
```

Expected: 没有输出。

## Task 6: 增加事故模式回归测试

**Files:**
- Modify: `src/tunnel/linux_relay_event_queue.h`
- Modify: `src/tunnel/linux_relay_worker.h`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Modify: `src/unittest/linux_relay_worker_io_test.cpp`

- [ ] **Step 1: 添加 encoded dispatch 控制事件类型**

在 `TqLinuxRelayEventType` 末尾添加：

```cpp
    DispatchEncodedEpollEventForTest,
```

- [ ] **Step 2: 添加测试 command 结构和声明**

在 public 方法区域、`DispatchTcpEventsForTest()` 附近添加：

```cpp
    bool DispatchEncodedEpollEventForTest(uint64_t epollData, uint32_t events);
```

在 private command 区域添加：

```cpp
    struct DispatchEncodedEpollEventForTestCommand {
        uint64_t EpollData{0};
        uint32_t Events{0};
        bool Result{false};
        std::mutex Mutex;
        std::condition_variable Cv;
        bool Done{false};
    };
```

在 private 方法声明区域添加：

```cpp
    bool DispatchEncodedEpollEventForTestLocal(uint64_t epollData, uint32_t events);
    void CompleteDispatchEncodedEpollEventForTestCommand(
        DispatchEncodedEpollEventForTestCommand* command,
        bool result);
```

- [ ] **Step 3: 在 DrainEvents 中处理测试控制事件**

在 `DrainEvents()` 的 switch 中添加：

```cpp
        case TqLinuxRelayEventType::DispatchEncodedEpollEventForTest: {
            auto* command =
                static_cast<DispatchEncodedEpollEventForTestCommand*>(event.Control);
            const bool result =
                command != nullptr &&
                DispatchEncodedEpollEventForTestLocal(command->EpollData, command->Events);
            CompleteDispatchEncodedEpollEventForTestCommand(command, result);
            break;
        }
```

- [ ] **Step 4: 添加 command 完成 helper**

```cpp
void TqLinuxRelayWorker::CompleteDispatchEncodedEpollEventForTestCommand(
    DispatchEncodedEpollEventForTestCommand* command,
    bool result) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(command->Mutex);
        command->Result = result;
        command->Done = true;
    }
    command->Cv.notify_one();
}
```

- [ ] **Step 5: 添加线程安全测试分发入口实现**

```cpp
bool TqLinuxRelayWorker::DispatchEncodedEpollEventForTest(uint64_t epollData, uint32_t events) {
    if (IsWorkerThread()) {
        return DispatchEncodedEpollEventForTestLocal(epollData, events);
    }

    std::unique_lock<std::mutex> controlGuard(ControlLock);
    for (;;) {
        if (!Running.load() || std::this_thread::get_id() == WorkerThreadId) {
            return DispatchEncodedEpollEventForTestLocal(epollData, events);
        }

        DispatchEncodedEpollEventForTestCommand command{};
        command.EpollData = epollData;
        command.Events = events;

        TqLinuxRelayEvent event{};
        event.Type = TqLinuxRelayEventType::DispatchEncodedEpollEventForTest;
        event.Control = &command;
        if (!Enqueue(std::move(event))) {
            controlGuard.unlock();
            Wake();
            std::this_thread::yield();
            controlGuard.lock();
            continue;
        }

        std::unique_lock<std::mutex> lock(command.Mutex);
        command.Cv.wait(lock, [&command]() {
            return command.Done;
        });
        return command.Result;
    }
}

bool TqLinuxRelayWorker::DispatchEncodedEpollEventForTestLocal(
    uint64_t epollData,
    uint32_t events) {
    return DispatchEncodedEpollEvent(epollData, events);
}
```

- [ ] **Step 6: 写回归测试**

在 `linux_relay_worker_io_test.cpp` 的早期基础测试之后添加：

```cpp
    {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = 128;
        TqLinuxRelayWorker worker(config);
        if (!worker.StartForTest()) return 3101;

        int relayFds[2]{-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, relayFds) != 0) {
            worker.Stop();
            return 3102;
        }

        TqLinuxRelayRegistration registration{};
        registration.TcpFd = relayFds[0];
        registration.Stream = nullptr;
        registration.Handle = nullptr;
        registration.EnableQuicSends = false;
        const TqLinuxRelayRegistrationResult registered = worker.RegisterRelayWithId(registration);
        if (!registered.Ok) {
            worker.Stop();
            ::close(relayFds[0]);
            ::close(relayFds[1]);
            return 3103;
        }

        const uint64_t encodedRelay = worker.EncodeEpollRelayForTest(registered.RelayId);
        if (worker.IsEpollWakeForTest(encodedRelay)) {
            worker.Stop();
            ::close(relayFds[1]);
            return 3104;
        }

        ::shutdown(relayFds[1], SHUT_RDWR);
        ::close(relayFds[1]);
        relayFds[1] = -1;

        if (!worker.DispatchEncodedEpollEventForTest(
                encodedRelay,
                EPOLLIN | EPOLLRDHUP | EPOLLHUP)) {
            worker.Stop();
            return 3105;
        }

        TqLinuxRelayWorkerSnapshot snapshot = worker.Snapshot();
        bool sawTcpReadClosed = false;
        bool sawTcpReadDisarmed = false;
        for (const auto& relay : snapshot.ActiveRelayStates) {
            if (relay.RelayId == registered.RelayId) {
                sawTcpReadClosed = relay.TcpReadClosed;
                sawTcpReadDisarmed = !relay.TcpReadArmed;
            }
        }
        if (!sawTcpReadClosed || !sawTcpReadDisarmed) {
            std::fprintf(stderr, "expected encoded relay event to close TCP read, relay=%llu\n",
                static_cast<unsigned long long>(registered.RelayId));
            worker.Stop();
            return 3106;
        }

        worker.Stop();
    }
```

- [ ] **Step 7: 运行回归测试**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test && rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected: PASS。

## Task 7: 运行完整相关测试

**Files:**
- No code changes.

- [ ] **Step 1: 构建两个 Linux relay worker 测试**

Run:

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_queue_test tcpquic_linux_relay_worker_io_test
```

Expected: build succeeds。

- [ ] **Step 2: 运行 queue 测试**

Run:

```bash
rtk build/src/tcpquic_linux_relay_worker_queue_test
```

Expected: exit code 0。

- [ ] **Step 3: 运行 IO 测试**

Run:

```bash
rtk build/src/tcpquic_linux_relay_worker_io_test
```

Expected: exit code 0。

- [ ] **Step 4: 检查源码中不再混用 fd/u64**

Run:

```bash
rtk rg -n "data\\.fd|events\\[i\\]\\.data\\.fd" src/tunnel/linux_relay_worker.cpp
```

Expected: 没有输出。

- [ ] **Step 5: 提交实现**

```bash
rtk git add src/CMakeLists.txt src/tunnel/linux_relay_event_queue.h src/tunnel/linux_relay_worker.cpp src/tunnel/linux_relay_worker.h src/unittest/linux_relay_worker_io_test.cpp src/unittest/linux_relay_worker_queue_test.cpp
rtk git commit -m "fix: tag linux relay epoll event data"
```

## 自检

- 设计覆盖根因：wake fd 与 relay id 数值碰撞。
- 实现覆盖所有 epoll data 写入点：wake add、relay add、relay mod。
- Run 只消费 `data.u64`，不再读取 `data.fd`。
- 测试既覆盖编码 helper、relay id 分配边界，也覆盖事故模式分发。
- encoded dispatch 测试入口通过 worker 控制队列执行，保持与生产 worker 线程模型一致。
- 范围限定在 Linux relay worker，不影响 Darwin/Windows。

## 评审意见确认

1. 已接受：`tcpquic_linux_relay_worker_queue_test` 目标没有给 `tunnel/linux_relay_worker.cpp` 设置 `TQ_UNIT_TESTING=1`，Task 2/3 会产生链接失败风险。已在 Task 2 增加 `src/CMakeLists.txt` 修改步骤，给 queue target 添加 `target_compile_definitions(... TQ_UNIT_TESTING=1)`。

2. 已接受：Task 6 的回归测试期望 relay 从 `ActiveRelayStates` 消失，与现有 relay 生命周期不匹配。已把断言改为验证该 relay 的 `TcpReadClosed == true` 且 `TcpReadArmed == false`，直接证明 encoded relay event 进入了 TCP 分支。

3. 已接受：Task 6 的 `DispatchEncodedEpollEventForTest()` 不应直接从测试线程调用 `DispatchEncodedEpollEvent()`。已改为新增 `DispatchEncodedEpollEventForTestCommand` 和 `DispatchEncodedEpollEventForTest` 控制事件，外部调用时入队，在 worker 线程内执行 local helper。

4. 已接受：Task 3 标为“写失败测试”与当前任务顺序不一致。已改为“添加编码逻辑测试”和“运行测试确认通过”。

5. 已接受：计划缺少 `AllocateRelayId()` 边界行为测试。已在 Task 4 增加 `SetNextRelayIdForTest()`、`AllocateRelayIdForTest()` 和 queue 测试，覆盖 `NextRelayId == 0`、最高位被置位、以及低 63 bit 最大值后回绕到 `1`。

6. 已接受：Task 4 的 `Files:` 漏列 `src/tunnel/linux_relay_worker.h`。已补充 header 和 queue test 文件范围。
