# libuv Relay Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不修改现有 Linux/macOS/Windows native relay 实现的前提下，实现一套构建期互斥、跨平台同源的 libuv relay backend，并首先在 Linux 完成功能、可靠性和 DGX netem 对比验证。

**Architecture:** `TCPQUIC_RELAY_BACKEND=native|libuv` 在 CMake 配置期选择同一组 `TqRelay*` 公共符号的唯一实现。libuv runtime 使用固定 worker 数，每个 worker 拥有一个线程、一个 `uv_loop_t` 和一组 relay；MsQuic callback 仅投递 typed command，所有 libuv handle 操作在所属 loop 线程执行。注册、数据面和 terminal 分成独立源文件，以便在公共 ownership 契约冻结后并行开发。

**Tech Stack:** C++17、CMake、libuv v1.52.1（静态 `uv_a`）、MsQuic、mimalloc、zstd、spdlog、现有 relay buffer/compressor/decompressor/stream lifetime API。

## Global Constraints

- 设计基线：`docs/libuv_relay_backend_integration_design_cn.md` 的 D-01～D-27 全部约束。
- 默认 backend 必须保持 `native`；单个 `raypx2` 只能包含一个 backend，失败不运行时回退。
- 不修改 `src/tunnel/relay.cpp`、`linux_relay_worker.*`、`darwin_relay_worker.*`、`windows_relay_worker.*` 及其 native 专用测试。
- libuv backend 只能调用 libuv、C++ 标准库、仓库现有 third_party 和已有跨平台公共接口；禁止直接调用 OS syscall、平台事件/同步接口或平台专用 fast path。
- 正常 libuv 构建强制 `TCPQUIC_USE_MIMALLOC=ON`；现有 ASan 检测关闭 mimalloc 时允许使用 system allocator。
- `uv_replace_allocator()` 必须先于进程内任何其他 libuv API，进程级 exactly-once，停止后不恢复 allocator。
- 第一轮只在 Linux 构建和运行；不得把 Linux 结果表述为 macOS/Windows 已验证。
- D-19 完整 runtime stop、graceful drain、超时 abort、反复启停和 stop 长稳必须排在正常功能及 D-24 性能验证之后。
- 所有 shell 命令使用 `rtk` 前缀；每个任务遵循 red-green-refactor，并只提交该任务列出的文件。

---

## 0. Worktree 与并行开发拓扑

### 0.1 集成分支和 worktree 规则

`master` 是最终集成分支。开始实现前先按 `using-git-worktrees` 检测当前 checkout；若不是隔离
worktree，则在确认 `.worktrees/` 已被忽略后创建集成 worktree：

```bash
rtk git rev-parse --git-dir
rtk git rev-parse --git-common-dir
rtk git check-ignore -q .worktrees
rtk git worktree add .worktrees/libuv-relay-integration -b feat/libuv-relay-integration master
```

创建前必须运行 `rtk git status --short`。当前主目录若仍有用户修改，不得 stash、清理或覆盖；
先把本计划依赖的已批准设计文档提交到 `master`，并与用户确认任何和本计划重叠的修改已经进入
worktree 基线。尤其是当前已有修改的 `scripts/run-dgx-netem-delay-loss-matrix.sh`，Task 8 必须
基于该修改合入后的 commit 开发，不能从旧版脚本另起分支。

若仓库提供原生 worktree 工具，优先使用原生工具，不执行上述 `git worktree add`。任何并行
分支都从已通过门禁的里程碑 commit 创建，不从带未提交修改的目录复制文件。最多同时运行三个
实现 subagent，加上一个集成负责人，符合四并发槽限制。

### 0.2 依赖图

```text
Task 1 build/source-set/API gate
  -> Task 2 allocator
  -> Task 3 queue/runtime/internal contract
  -> Task 4 facade + registration transaction
       |
       +--> WT-A Task 5 QUIC -> TCP --------+
       +--> WT-B Task 6 TCP -> QUIC --------+--> Task 9 FIN/terminal
       +--> WT-C Task 7 metrics/API lint ---+
       +--> WT-C Task 8 DGX tooling --------+
                                             -> Task 10 Linux E2E
                                             -> Task 11 DGX execution
                                             -> Task 12 D-19 stop (last)
                                             -> Task 13 final verification
```

### 0.3 并行写入所有权

| Lane | 独占文件 | 禁止修改 | 合入条件 |
|---|---|---|---|
| 集成负责人 | 根/`src/CMakeLists.txt`、`relay.h`、`libuv_relay_internal.h`、`libuv_relay_worker.cpp` | native worker/`relay.cpp` | native + libuv 配置和 core tests 通过 |
| WT-A / QUIC→TCP | `libuv_relay_quic_to_tcp.cpp`、对应测试及 CMake fragment | internal header、TCP→QUIC、terminal | receive/write ownership 与解压测试通过 |
| WT-B / TCP→QUIC | `libuv_relay_tcp_to_quic.cpp`、对应测试及 CMake fragment | internal header、QUIC→TCP、terminal | read/send ownership 与压缩测试通过 |
| WT-C / 观测与工具 | `relay_metrics.cpp` 的 libuv 分支、API lint、DGX metadata/汇总脚本及测试 | worker 状态机、native 转换逻辑 | JSON/schema/script tests 通过 |

并行 agent 若发现公共契约缺项，只提交一条包含所需字段、原因和测试的变更请求给集成负责人，
不得自行修改 `libuv_relay_internal.h`。集成负责人修改契约、运行 core tests、产生新里程碑后，
并行分支 rebase 到该里程碑。该规则用来避免多个 worktree 各自发明 ownership 语义。

### 0.4 设计覆盖索引

| 设计决策 | 实施任务 |
|---|---|
| D-01、D-20、D-21、D-25 | Task 1、4、13 |
| D-02、D-05～D-08 | Task 3、4 |
| D-03、D-12～D-16 | Task 5、6 |
| D-04 | Global Constraints；保留现有 c-ares dial reactor，libuv relay 不新增 DNS 路径 |
| D-09～D-11 | Task 4 |
| D-17、D-18 | Task 9 |
| D-19 | Task 12，且严格排在 Task 10/11 后 |
| D-22 | Task 7 |
| D-23 | Task 3～10、12、13 |
| D-24 | Task 8、11 |
| D-26 | Task 2、7、11、13 |
| D-27 | Task 1、3～9、13 |

---

### Task 1: 构建期 backend source set 与平台 API 门禁

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `src/CMakeLists.txt`
- Modify: `src/config/tuning.cpp`
- Modify: `src/tunnel/relay.h`
- Create: `scripts/check-libuv-backend-api.py`
- Create: `tests/scripts/test_check_libuv_backend_api.py`

**Interfaces:**
- Produces: cache string `TCPQUIC_RELAY_BACKEND`，编译宏 `TCPQUIC_RELAY_BACKEND_LIBUV=0|1`，CMake helper `tcpquic_add_libuv_relay_test(target ...)`。
- Produces: `TqRelayBackendType::LibuvWorker` 与条件字段 `std::shared_ptr<const TqUvRelayCommittedState> LibuvCommitted`。
- Produces: `constexpr const char* TqCompiledRelayBackend()`，供启动日志、admin、metrics 和测试 metadata 使用。
- Consumes: 当前 `uv_a`、`tcpquic_link_mimalloc()` 和原有 native source lists。

- [ ] **Step 1: 写 backend 配置失败测试**

在 `tests/scripts/test_check_libuv_backend_api.py` 增加临时文件扫描测试，并用 CMake configure
命令验证非法 backend 被拒绝：

```python
def test_rejects_os_headers(tmp_path):
    source = tmp_path / "libuv_relay_bad.cpp"
    source.write_text("#include <sys/socket.h>\nint f(){ return close(1); }\n")
    result = subprocess.run(
        [sys.executable, "scripts/check-libuv-backend-api.py", str(source)],
        text=True, capture_output=True)
    assert result.returncode != 0
    assert "sys/socket.h" in result.stdout
    assert "close" in result.stdout
```

- [ ] **Step 2: 验证测试先失败**

Run: `rtk python3 -m unittest tests/scripts/test_check_libuv_backend_api.py -v`
Expected: FAIL，原因是 `check-libuv-backend-api.py` 尚不存在。

- [ ] **Step 3: 实现 CMake 互斥 source set**

在根 CMake 中加入严格枚举；mimalloc 强制检查必须放在现有 sanitizer 检测及 ASan 自动关闭
mimalloc 的逻辑之后，不能在变量初始化前判断：

```cmake
set(TCPQUIC_RELAY_BACKEND native CACHE STRING "Compiled relay backend: native or libuv")
set_property(CACHE TCPQUIC_RELAY_BACKEND PROPERTY STRINGS native libuv)
if(NOT TCPQUIC_RELAY_BACKEND MATCHES "^(native|libuv)$")
    message(FATAL_ERROR "TCPQUIC_RELAY_BACKEND must be native or libuv")
endif()
if(TCPQUIC_RELAY_BACKEND STREQUAL "libuv" AND
   NOT TCPQUIC_USE_MIMALLOC AND NOT TCPQUIC_ADDRESS_SANITIZER_ENABLED)
    message(FATAL_ERROR "normal libuv backend builds require TCPQUIC_USE_MIMALLOC=ON")
endif()
```

在 `src/CMakeLists.txt` 把 `tunnel/relay.cpp` 和三个 native worker移入 native 分支；libuv 分支
只加入 libuv 文件并定义 `TCPQUIC_RELAY_BACKEND_LIBUV=1`。为并行 lane 预留互不冲突的 optional
CMake fragment：

```cmake
function(tcpquic_add_libuv_relay_test target)
    add_tcpquic_executable(${target} ${ARGN})
    tcpquic_target_include_dirs(${target})
    target_link_libraries(${target} PRIVATE uv_a libzstd spdlog::spdlog Threads::Threads)
    target_compile_definitions(${target} PRIVATE TQ_UNIT_TESTING=1 TCPQUIC_RELAY_BACKEND_LIBUV=1)
    tcpquic_link_mimalloc(${target})
endfunction()

include(cmake/libuv_relay_quic_to_tcp.cmake OPTIONAL)
include(cmake/libuv_relay_tcp_to_quic.cmake OPTIONAL)
include(cmake/libuv_relay_metrics.cmake OPTIONAL)
```

`src/config/tuning.cpp` 的启动信息通过 `TqCompiledRelayBackend()` 输出 `native` 或 `libuv`；native
分支继续保留现有 epoll/kqueue/IOCP 说明和 worker 数值。

- [ ] **Step 4: 实现 API 静态扫描**

脚本递归扫描传入的 production libuv source，拒绝平台头、平台宏和函数调用 token；允许测试
harness 使用 OS API，但不扫描 `src/unittest`：

```python
FORBIDDEN_LITERAL = (
    "sys/socket.h", "sys/epoll.h", "sys/event.h", "unistd.h", "windows.h",
    "__linux__", "__APPLE__", "_WIN32", "epoll_", "pthread_",
)
FORBIDDEN_CALL = re.compile(
    r"(?<![A-Za-z0-9_])"
    r"(kevent|kqueue|socket|close|shutdown|read|write|readv|writev|"
    r"fcntl|ioctl|setsockopt)\s*\(")
```

负向 lookbehind 保证 `uv_close()`、`uv_shutdown()`、`uv_read_start()` 和 `uv_write()` 不会被裸
系统调用规则误报。扫描失败打印 `path:line:token` 并返回非零；在 libuv target 构建前增加
custom target 调用该脚本。

- [ ] **Step 5: 验证 native 不变与非法配置被拒绝**

Run: `rtk cmake -S . -B build/native-plan -DTCPQUIC_RELAY_BACKEND=native`
Expected: configure PASS，输出 compiled backend 为 native。

Run: `rtk cmake --build build/native-plan --target tcpquic-proxy -j2`
Expected: PASS，产物名仍为 `raypx2`。

Run: `rtk cmake -S . -B build/invalid-plan -DTCPQUIC_RELAY_BACKEND=invalid`
Expected: FAIL，包含 `must be native or libuv`。

Run: `rtk python3 -m unittest tests/scripts/test_check_libuv_backend_api.py -v`
Expected: PASS。

- [ ] **Step 6: 提交构建边界**

```bash
rtk git add CMakeLists.txt src/CMakeLists.txt src/config/tuning.cpp src/tunnel/relay.h scripts/check-libuv-backend-api.py tests/scripts/test_check_libuv_backend_api.py
rtk git commit -m "build: add isolated libuv relay backend selection"
```

---

### Task 2: 进程级 libuv allocator bootstrap

**Files:**
- Create: `src/tunnel/libuv_allocator.h`
- Create: `src/tunnel/libuv_allocator.cpp`
- Create: `src/unittest/libuv_allocator_test.cpp`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Produces: `bool TqUvInstallAllocator() noexcept`。
- Produces: `enum class TqUvAllocatorMode { Mimalloc, System }` 和 `TqUvAllocatorSnapshot TqUvAllocatorStatus() noexcept`，snapshot 字段为 `Mode`、`Attempted`、`Installed`、`Status`、四类调用计数。
- Produces test-only: `using TqUvReplaceAllocatorFn = int (*)(uv_malloc_func, uv_realloc_func, uv_calloc_func, uv_free_func)`、`void TqUvSetReplaceAllocatorForTest(TqUvReplaceAllocatorFn)` 和 `void TqUvResetAllocatorStateForTest()`，只能在任何 libuv allocation 之前调用。

- [ ] **Step 1: 写 allocator 顺序和 exactly-once 测试**

```cpp
static int ReplaceCalls = 0;
static int FakeReplace(uv_malloc_func, uv_realloc_func, uv_calloc_func, uv_free_func) {
    ++ReplaceCalls;
    return 0;
}

int main() {
    TqUvResetAllocatorStateForTest();
    TqUvSetReplaceAllocatorForTest(FakeReplace);
    assert(TqUvInstallAllocator());
    assert(TqUvInstallAllocator());
    assert(ReplaceCalls == 1);
    const auto status = TqUvAllocatorStatus();
    assert(status.Attempted && status.Installed && status.Status == 0);
}
```

另加 replacement 返回 `UV_EINVAL` 时安装失败且第二次调用返回相同失败结果的 case。

- [ ] **Step 2: 验证测试先失败**

Run: `rtk cmake -S . -B build/libuv-plan -DTCPQUIC_RELAY_BACKEND=libuv -DTCPQUIC_USE_MIMALLOC=ON`
Expected: configure PASS；Task 1 已建立 source-set 框架，但 allocator test target 尚不存在。

Run: `rtk cmake --build build/libuv-plan --target tcpquic_libuv_allocator_test -j2`
Expected: FAIL，target/接口尚不存在。

- [ ] **Step 3: 实现 allocator adapter**

正常构建的四个 wrapper 直接调用 mimalloc，ASan/system 模式不调用
`uv_replace_allocator()`：

```cpp
bool TqUvInstallAllocator() noexcept {
#if TCPQUIC_USE_MIMALLOC
    std::call_once(gOnce, [] {
        gAttempted.store(true, std::memory_order_release);
        const int status = gReplace(TqUvMiMalloc, TqUvMiRealloc,
                                    TqUvMiCalloc, TqUvMiFree);
        gStatus.store(status, std::memory_order_release);
        gInstalled.store(status == 0, std::memory_order_release);
    });
    return gInstalled.load(std::memory_order_acquire);
#else
    gStatus.store(0, std::memory_order_release);
    return true;
#endif
}
```

不得复用带私有 header 的 `TqMalloc/TqRealloc`，因为 libuv allocator 需要标准
malloc/realloc/calloc/free 语义；wrapper 直接使用 `mi_*`。

- [ ] **Step 4: 验证 allocator 测试和 ASan 配置**

Run: `rtk cmake -S . -B build/libuv-plan -DTCPQUIC_RELAY_BACKEND=libuv -DTCPQUIC_USE_MIMALLOC=ON`
Expected: configure PASS。

Run: `rtk cmake --build build/libuv-plan --target tcpquic_libuv_allocator_test -j2`
Expected: PASS。

Run: `rtk ./build/libuv-plan/src/tcpquic_libuv_allocator_test`
Expected: exit 0，replacement call count 为 1。

- [ ] **Step 5: 提交 allocator bootstrap**

```bash
rtk git add src/CMakeLists.txt src/tunnel/libuv_allocator.h src/tunnel/libuv_allocator.cpp src/unittest/libuv_allocator_test.cpp
rtk git commit -m "feat: install mimalloc for libuv allocations"
```

---

### Task 3: typed command queue、worker runtime 与冻结的内部契约

**Files:**
- Create: `src/tunnel/libuv_relay_event_queue.h`
- Create: `src/tunnel/libuv_relay_internal.h`
- Create: `src/tunnel/libuv_relay_worker.h`
- Create: `src/tunnel/libuv_relay_worker.cpp`
- Create: `src/unittest/libuv_relay_worker_queue_test.cpp`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Produces: `TqUvRelayRuntime::Instance()/Start/PickWorker/SnapshotWorkers()`；此阶段仅提供 test-only 最小停止清理，完整 D-19 留到 Task 12。
- Produces: `TqUvRelayWorker::RegisterRelayWithId()`、`Post()`、`Snapshot()` 和 loop-local variants。
- Produces frozen internal types: `TqUvRelayState`、`TqUvStreamBinding`、`TqUvTcpWriteOperation`、`TqUvQuicSendOperation`。
- Produces: `TqUvRelayRegistration { TcpSocket, Stream, StreamOwner, StopControl, ControlGeneration, Compressor, Decompressor, CompressAlgo }` 和 `TqUvRelayRegistrationResult { Ok, TcpFdConsumed, RelayId, Committed }`。
- Produces test-only libuv call adapter，用于注入 `uv_tcp_init/open/read_start/write/shutdown/close` 失败；production adapter 必须直接调用相应 libuv 公共 API，不加入 OS fallback。
- Consumes: `TqUvInstallAllocator()` 必须是 runtime 的第一项动作。

- [ ] **Step 1: 写 queue 和 runtime 启动失败测试**

覆盖 async wake coalescing、FIFO command drain、queue-full、worker-thread local dispatch、allocator
失败时 `uv_loop_init` 未调用、第二个 worker 启动失败时已启动 worker 安全清理。

```cpp
assert(runtime.Start(config));
assert(runtime.WorkerCountForTest() == config.WorkerCount);
auto* first = runtime.PickWorker();
auto* second = runtime.PickWorker();
assert(first != second);
assert(runtime.AllocatorInstalledBeforeLoopForTest());
```

- [ ] **Step 2: 运行测试确认失败**

Run: `rtk cmake --build build/libuv-plan --target tcpquic_libuv_relay_worker_queue_test -j2`
Expected: FAIL，runtime 和 queue 尚不存在。

- [ ] **Step 3: 实现 worker thread/loop/async lifecycle**

启动严格顺序：

```cpp
bool TqUvRelayRuntime::Start(const TqTuningConfig& tuning) {
    std::lock_guard<std::mutex> guard(Lock_);
    if (State_ == TqRelayRuntimeState::Running) return true;
    if (State_ != TqRelayRuntimeState::Stopped || !TqUvInstallAllocator()) return false;
    State_ = TqRelayRuntimeState::Starting;
    std::vector<std::unique_ptr<TqUvRelayWorker>> staged;
    for (uint32_t index = 0; index < tuning.RelayWorkerCount; ++index) {
        auto worker = std::make_unique<TqUvRelayWorker>(MakeWorkerConfig(tuning, index));
        if (!worker->StartAndWaitReady()) {
            for (auto& started : staged) started->StopForStartupRollback();
            State_ = TqRelayRuntimeState::Stopped;
            return false;
        }
        staged.push_back(std::move(worker));
    }
    Workers_ = std::move(staged);
    State_ = TqRelayRuntimeState::Running;
    return true;
}
```

跨线程只允许 `uv_async_send()`；queue 由 `uv_mutex_t` 保护并在 async callback 中 swap 到本地。
注册 command 持有 `uv_mutex_t`、`uv_cond_t`、`Queued/Executing/Completed/Cancelled` 状态和结果。

- [ ] **Step 4: 冻结并行 lane 使用的 internal contract**

`libuv_relay_internal.h` 明确定义且由集成负责人独占修改：

```cpp
enum class TqUvActivation : uint8_t { Prepared, Active, Terminal, Failed };
enum class TqUvTerminalTrigger : uint8_t {
    TcpEof, TcpError, QuicFin, QuicAbort, QueueFailure, AllocationFailure, RuntimeStop
};
struct TqUvRelayState;
void TqUvProcessQuicToTcp(TqUvRelayWorker&, TqUvRelayState&);
void TqUvHandleTcpRead(TqUvRelayWorker&, TqUvRelayState&, ssize_t, const uv_buf_t&);
void TqUvHandleSendComplete(TqUvRelayWorker&, TqUvQuicSendOperation&, bool);
void TqUvBeginTerminal(TqUvRelayWorker&, TqUvRelayState&, TqUvTerminalTrigger);
```

所有 `uv_tcp_t/uv_write_t/uv_shutdown_t` ownership 字段、pending byte counters 和 generation
都在该 header 一次定义；方向 lane 不允许增加旁路状态。

- [ ] **Step 5: 验证 core tests 和 API 门禁**

Run: `rtk cmake --build build/libuv-plan --target tcpquic_libuv_relay_worker_queue_test -j2`
Expected: PASS。

Run: `rtk ./build/libuv-plan/src/tcpquic_libuv_relay_worker_queue_test`
Expected: exit 0。

Run: `rtk python3 scripts/check-libuv-backend-api.py src/tunnel/libuv_allocator.cpp src/tunnel/libuv_relay_worker.cpp`
Expected: exit 0，无 forbidden token。

- [ ] **Step 6: 提交并行开发里程碑 M1**

```bash
rtk git add src/CMakeLists.txt src/tunnel/libuv_relay_event_queue.h src/tunnel/libuv_relay_internal.h src/tunnel/libuv_relay_worker.h src/tunnel/libuv_relay_worker.cpp src/unittest/libuv_relay_worker_queue_test.cpp
rtk git commit -m "feat: add libuv relay runtime and command queue"
```

从该 commit 创建 WT-A/WT-B/WT-C；subagent prompt 必须附带对应 lane 的独占文件和禁止修改清单。

---

### Task 4: 公共 facade、注册事务与 precommit RECEIVE

**Files:**
- Create: `src/tunnel/libuv_relay.cpp`
- Create: `src/unittest/libuv_relay_registration_test.cpp`
- Modify: `src/tunnel/libuv_relay_worker.cpp`
- Modify: `src/tunnel/relay.h`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Produces: 与 native 相同的 `TqRelayStartManaged()`、`TqRelayStop()`、`TqRelaySetTraceContext()` 符号。
- Produces: `TqUvRelayRegistrationResult { Ok, TcpFdConsumed, RelayId }`。
- Produces: immutable `TqUvRelayCommittedState { WorkerIdentity, RelayId, WorkerIndex, Control, ControlGeneration }`。

- [ ] **Step 1: 写注册线性化和故障注入测试**

覆盖 prepare failure、PublishTarget failure、`uv_tcp_init` failure、`uv_tcp_open` failure、
`uv_read_start` failure、publish 后 terminal、precommit drain/discard once-only、Queued timeout cancel、
Executing 后等待最终结果。每个 case 断言 `Ok`、`TcpFdConsumed`、fd/handle close count 和 phase。

```cpp
const auto result = worker.RegisterRelayWithId(registration);
assert(!result.Ok);
assert(result.TcpFdConsumed);             // failure after PublishTarget
assert(fakeUv.CloseCallbackCount() == 1); // never raw close in backend
assert(binding.PrecommitSettled());
```

- [ ] **Step 2: 运行测试确认失败**

Run: `rtk cmake --build build/libuv-plan --target tcpquic_libuv_relay_registration_test -j2`
Expected: FAIL，registration transaction 尚未实现。

- [ ] **Step 3: 实现 D-09～D-11 顺序**

```text
prepare identity/state/token
-> PublishTarget
-> ownership = PreparedTokenOwned
-> uv_tcp_init
-> uv_tcp_open
-> ownership = UvHandleOwned
-> uv_read_start
-> activation mutex: Prepared -> Active
-> settle precommit exactly once
```

PublishTarget 前失败返回 `TcpFdConsumed=false`；publish 后任何失败返回 true 并通过 `uv_close()`
回收。callback-visible identity 在 publish 前初始化，publish 后不改写。Queued timeout 只取消
尚未 Executing 的 command。

- [ ] **Step 4: 实现 facade source-set 等价符号**

`libuv_relay.cpp` 只连接公共 API 与 `TqUvRelayRuntime`，不包含或调用 native worker：

```cpp
bool TqRelayStartManaged(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    std::shared_ptr<TqStreamLifetime> streamOwner,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo,
    bool* tcpFdConsumed) {
    if (tcpFdConsumed != nullptr) *tcpFdConsumed = false;
    if (!TqUvRelayRuntime::Instance().Start(tuning)) return false;
    auto* worker = TqUvRelayRuntime::Instance().PickWorker();
    TqUvRelayRegistration registration{
        tcpFd, stream, std::move(streamOwner), handle->Control,
        handle->ControlGeneration, compressor, decompressor, compressAlgo};
    const auto result = worker->RegisterRelayWithId(registration);
    if (tcpFdConsumed != nullptr) *tcpFdConsumed = result.TcpFdConsumed;
    if (!result.Ok) return false;
    std::atomic_store(&handle->LibuvCommitted, result.Committed);
    return true;
}
```

- [ ] **Step 5: 验证注册和 native 双构建**

Run: `rtk cmake --build build/libuv-plan --target tcpquic_libuv_relay_registration_test tcpquic-proxy -j2`
Expected: PASS。

Run: `rtk ./build/libuv-plan/src/tcpquic_libuv_relay_registration_test`
Expected: exit 0。

Run: `rtk cmake --build build/native-plan --target tcpquic-proxy -j2`
Expected: PASS，native source set 无变化。

- [ ] **Step 6: 提交注册事务里程碑 M2**

```bash
rtk git add src/CMakeLists.txt src/tunnel/relay.h src/tunnel/libuv_relay.cpp src/tunnel/libuv_relay_worker.cpp src/unittest/libuv_relay_registration_test.cpp
rtk git commit -m "feat: add transactional libuv relay registration"
```

WT-A/WT-B/WT-C rebase 到 M2 后开始并行实现。

---

### Task 5 [WT-A]: QUIC→TCP 无压缩、解压和背压

**Files:**
- Create: `src/tunnel/libuv_relay_quic_to_tcp.cpp`
- Create: `src/unittest/libuv_relay_quic_to_tcp_test.cpp`
- Create: `src/cmake/libuv_relay_quic_to_tcp.cmake`

**Interfaces:**
- Consumes: M2 的 `TqUvRelayState`、pending receive view、`TqUvProcessQuicToTcp()` contract。
- Produces: `uv_write()`-only QUIC→TCP path、decompress ownership split、receive pause/resume。

- [ ] **Step 1: 写无压缩 ownership 测试**

模拟一个含多个 `QUIC_BUFFER` slice 的 RECEIVE，断言 callback 返回 PENDING、`uv_write()` 前不
ReceiveComplete、write callback 后按准确字节完成一次、terminal error 不重复 completion。

- [ ] **Step 2: 写解压消费边界测试**

断言 decompressor 把输入完整转换到 relay-owned buffer 后立即 ReceiveComplete，早于
`uv_write` callback；partial input 只 completion 已完全消费的字节，输出 buffer 到 write
callback 才释放。

- [ ] **Step 3: 运行测试确认失败**

Run: `rtk cmake --build build/libuv-q2t --target tcpquic_libuv_relay_quic_to_tcp_test -j2`
Expected: FAIL，方向实现不存在。

- [ ] **Step 4: 实现唯一的 uv_write 路径**

```cpp
auto operation = std::make_unique<TqUvTcpWriteOperation>();
operation->ReceiveOwner = pendingReceive;
operation->Buffers = BuildUvBuffers(pendingReceive->Slices);
const int status = uv_write(&operation->Request, relay.TcpHandle,
                            operation->Buffers.data(),
                            static_cast<unsigned>(operation->Buffers.size()),
                            TqUvOnTcpWriteComplete);
```

禁止 `uv_try_write()`。解压路径先取得 relay-owned output，再完成 compressed input；write
operation 只持有 output ownership。高水位调用 `StreamReceiveSetEnabled(false)`，低水位恢复。

- [ ] **Step 5: 验证方向测试和 API 门禁**

Run: `rtk cmake --build build/libuv-q2t --target tcpquic_libuv_relay_quic_to_tcp_test -j2`
Expected: PASS。

Run: `rtk ./build/libuv-q2t/src/tcpquic_libuv_relay_quic_to_tcp_test`
Expected: exit 0。

Run: `rtk python3 scripts/check-libuv-backend-api.py src/tunnel/libuv_relay_quic_to_tcp.cpp`
Expected: exit 0。

- [ ] **Step 6: 提交 WT-A**

```bash
rtk git add src/tunnel/libuv_relay_quic_to_tcp.cpp src/unittest/libuv_relay_quic_to_tcp_test.cpp src/cmake/libuv_relay_quic_to_tcp.cmake
rtk git commit -m "feat: add libuv quic to tcp relay path"
```

---

### Task 6 [WT-B]: TCP→QUIC 无压缩、压缩和背压

**Files:**
- Create: `src/tunnel/libuv_relay_tcp_to_quic.cpp`
- Create: `src/unittest/libuv_relay_tcp_to_quic_test.cpp`
- Create: `src/cmake/libuv_relay_tcp_to_quic.cmake`

**Interfaces:**
- Consumes: M2 的 `TqUvRelayState`、`TqUvHandleTcpRead()`、send completion reservation contract。
- Produces: 一次成功 `uv_read_cb` 对应一次 `StreamSend`，压缩输出保持到 SEND_COMPLETE。

- [ ] **Step 1: 写 read/send ownership 测试**

断言一次 positive `uv_read_cb` 只产生一次 `StreamSend`、buffer 保持到 SEND_COMPLETE、重复或旧
generation completion 只释放一次且不改变新 relay。

- [ ] **Step 2: 写压缩/背压/FIN flush 测试**

断言 compressor 消费输入后可归还 read buffer、压缩输出保持到 SEND_COMPLETE；达到
`MaxBufferedQuicSendBytes` 执行 `uv_read_stop()`，降到 resume 水位后在 loop 线程
`uv_read_start()`；EOF flush compressor 并提交 FIN operation。

- [ ] **Step 3: 运行测试确认失败**

Run: `rtk cmake --build build/libuv-t2q --target tcpquic_libuv_relay_tcp_to_quic_test -j2`
Expected: FAIL，方向实现不存在。

- [ ] **Step 4: 实现一次 read 对应一次 send**

```cpp
auto send = std::make_unique<TqUvQuicSendOperation>();
send->Reservation = relay.StreamOwner->ReserveSendCompletion(send.get());
send->Views = BuildTcpToQuicViews(buffer, compressor, relay.CompressAlgo);
send->QuicBuffers = ToQuicBuffers(send->Views);
const auto status = TqUvQuicApi::StreamSend(
    relay.Stream, send->QuicBuffers.data(),
    static_cast<uint32_t>(send->QuicBuffers.size()), flags, send.get());
```

SEND_COMPLETE callback 只投递 typed command/fallback；释放 buffer、更新 backlog 和恢复 read 都在
loop 线程执行。不得跨相邻 read callback 主动合并。

- [ ] **Step 5: 验证方向测试和 API 门禁**

Run: `rtk cmake --build build/libuv-t2q --target tcpquic_libuv_relay_tcp_to_quic_test -j2`
Expected: PASS。

Run: `rtk ./build/libuv-t2q/src/tcpquic_libuv_relay_tcp_to_quic_test`
Expected: exit 0。

Run: `rtk python3 scripts/check-libuv-backend-api.py src/tunnel/libuv_relay_tcp_to_quic.cpp`
Expected: exit 0。

- [ ] **Step 6: 提交 WT-B**

```bash
rtk git add src/tunnel/libuv_relay_tcp_to_quic.cpp src/unittest/libuv_relay_tcp_to_quic_test.cpp src/cmake/libuv_relay_tcp_to_quic.cmake
rtk git commit -m "feat: add libuv tcp to quic relay path"
```

---

### Task 7 [WT-C]: libuv 独立 metrics、allocator 诊断与 source 准入

**Files:**
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/runtime/memory_stats.cpp`
- Create: `src/unittest/libuv_relay_metrics_test.cpp`
- Create: `src/cmake/libuv_relay_metrics.cmake`
- Modify: `tests/scripts/test_check_libuv_backend_api.py`

**Interfaces:**
- Consumes: `TqUvRelayRuntime::SnapshotWorkers()`、`TqUvAllocatorStatus()`。
- Produces: `compiled_relay_backend=libuv`、allocator mode/install status、libuv worker/queue/loop/pending/error JSON。

- [ ] **Step 1: 写 JSON schema 测试**

断言 libuv build 的 aggregate/worker snapshot 包含 `backend=libuv`、worker identity、active relay、
pending bytes、queue depth、wake/coalesce、loop lag、errors；allocator dump 包含 mode/attempted/
installed/status。断言 native build 的既有字段和值不变。

- [ ] **Step 2: 运行测试确认失败**

Run: `rtk cmake --build build/libuv-metrics --target tcpquic_libuv_relay_metrics_test -j2`
Expected: FAIL，libuv conversion 尚不存在。

- [ ] **Step 3: 实现条件编译的 libuv snapshot conversion**

```cpp
#if TCPQUIC_RELAY_BACKEND_LIBUV
TqRelayWorkerSnapshot ConvertUvRelayWorkerSnapshot(const TqUvRelayWorkerSnapshot& source) {
    TqRelayWorkerSnapshot out{};
    out.Backend = "libuv";
    out.WorkerIndex = source.WorkerIndex;
    out.WorkerId = "libuv-" + std::to_string(source.WorkerIndex);
    out.ActiveRelays = source.ActiveRelays;
    out.PendingBytes = source.PendingBytes;
    out.Errors = source.Errors;
    return out;
}
#endif
```

只增加 libuv 分支，不重构或重命名 native snapshot 类型。

- [ ] **Step 4: 扩充静态扫描正反例**

增加 wrapper 绕过、平台宏、`uv_fileno` 后 native call 等负例，以及合法 `uv_tcp_open`、
`uv_shutdown`、mimalloc、MsQuic、zstd 调用正例。

- [ ] **Step 5: 验证 metrics 与扫描**

Run: `rtk cmake --build build/libuv-metrics --target tcpquic_libuv_relay_metrics_test -j2`
Expected: PASS。

Run: `rtk ./build/libuv-metrics/src/tcpquic_libuv_relay_metrics_test`
Expected: exit 0。

Run: `rtk python3 -m unittest tests/scripts/test_check_libuv_backend_api.py -v`
Expected: PASS。

- [ ] **Step 6: 提交 WT-C metrics**

```bash
rtk git add src/runtime/relay_metrics.cpp src/runtime/memory_stats.cpp src/unittest/libuv_relay_metrics_test.cpp src/cmake/libuv_relay_metrics.cmake tests/scripts/test_check_libuv_backend_api.py
rtk git commit -m "feat: expose libuv relay runtime diagnostics"
```

---

### Task 8 [WT-C]: DGX 对比工具的 backend/allocator 证据

**Files:**
- Modify: `scripts/run-dgx-netem-delay-loss-matrix.sh`
- Create: `tests/scripts/test_run_dgx_netem_backend_matrix.py`
- Modify: `docs/test/dgx-netem-delay-loss-matrix_cn.md`

**Interfaces:**
- Produces: `native/`、`libuv/` 独立结果目录，`build-metadata.txt` allocator/backend 字段，`comparison.csv`。
- Consumes: D-24 固定 14 case 矩阵，不增加淘汰线。

- [ ] **Step 1: 写脚本 dry-run 测试**

使用 fake `ssh/tc/curl` fixture 验证 backend 必须为 native 或 libuv、两端 backend 一致、metadata
包含 commit/submodule/CMake/compiler/hash/tuning/mimalloc/sanitizer/allocator，汇总按至少三轮中位数
计算 `libuv_delta_pct`，不输出 backend pass/fail。

- [ ] **Step 2: 运行测试确认失败**

Run: `rtk python3 -m unittest tests/scripts/test_run_dgx_netem_backend_matrix.py -v`
Expected: FAIL，现有脚本没有双 backend metadata/summary contract。

- [ ] **Step 3: 实现结果目录和比较字段**

```text
docs/dgx-netem-delay-loss-matrix-20260715-libuv-eval/native/build-metadata.txt
docs/dgx-netem-delay-loss-matrix-20260715-libuv-eval/native/cases/download-10ms-5pct-r1/
docs/dgx-netem-delay-loss-matrix-20260715-libuv-eval/libuv/build-metadata.txt
docs/dgx-netem-delay-loss-matrix-20260715-libuv-eval/libuv/cases/download-10ms-5pct-r1/
docs/dgx-netem-delay-loss-matrix-20260715-libuv-eval/comparison.csv
docs/dgx-netem-delay-loss-matrix-20260715-libuv-eval/comparison.md
```

保留参考脚本 hard/soft anomaly 和 `baseline_delta_pct > 20%` 的数据有效性语义；不得把它解释为
libuv 淘汰线。

- [ ] **Step 4: 验证脚本测试和 shell 语法**

Run: `rtk python3 -m unittest tests/scripts/test_run_dgx_netem_backend_matrix.py -v`
Expected: PASS。

Run: `rtk bash -n scripts/run-dgx-netem-delay-loss-matrix.sh`
Expected: exit 0。

- [ ] **Step 5: 提交 WT-C 工具**

```bash
rtk git add scripts/run-dgx-netem-delay-loss-matrix.sh tests/scripts/test_run_dgx_netem_backend_matrix.py docs/test/dgx-netem-delay-loss-matrix_cn.md
rtk git commit -m "test: compare native and libuv DGX netem results"
```

该任务可与 Task 5/6 并行开发，但真实 DGX 执行必须等待 Task 10 完成。

---

### Task 9: 合并并行 lanes，实现 FIN、异常和 terminal convergence

**Files:**
- Create: `src/tunnel/libuv_relay_terminal.cpp`
- Create: `src/unittest/libuv_terminal_convergence_test.cpp`
- Modify: `src/tunnel/libuv_relay_worker.cpp`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 5/6 的 pending write/send/receive ownership 和 Task 4 的 Prepared/Active binding。
- Produces: `TqUvBeginTerminal()` 唯一入口、normal half-close predicate、Darwin correctness + Linux command/fallback fast path。

- [ ] **Step 1: 集成三个 worktree commit 并跑方向测试**

```bash
rtk git cherry-pick feat/libuv-q2t
rtk git cherry-pick feat/libuv-t2q
rtk git cherry-pick feat/libuv-observability~1
rtk git cherry-pick feat/libuv-observability
```

Run: `rtk cmake --build build/libuv-plan --target tcpquic_libuv_relay_quic_to_tcp_test tcpquic_libuv_relay_tcp_to_quic_test tcpquic_libuv_relay_metrics_test -j2`
Expected: PASS；若 internal contract 冲突，回退 cherry-pick 并由集成负责人统一修订 contract，禁止在两个方向各加旁路字段。

- [ ] **Step 2: 写 FIN/terminal 竞态测试**

覆盖 TCP EOF→compressor flush→QUIC FIN→SEND_COMPLETE；QUIC FIN→drain writes→`uv_shutdown`；
activation 与 terminal 同时发生；TCP reset/write/shutdown failure；queue full fallback；迟到
SEND_COMPLETE/RECEIVE/SHUTDOWN_COMPLETE；旧 generation；`uv_close` callback 晚于 MsQuic terminal。

- [ ] **Step 3: 运行测试确认失败**

Run: `rtk cmake --build build/libuv-plan --target tcpquic_libuv_terminal_convergence_test -j2`
Expected: FAIL，统一 terminal 尚未实现。

- [ ] **Step 4: 实现唯一 terminal state machine**

```text
seal Prepared/Active -> Terminal under activation mutex
-> reject active-only commands
-> uv_read_stop
-> settle precommit once
-> drain/transfer local operation ownership
-> install shutdown sink
-> TqStreamLifetime::BeginTerminalShutdown
-> uv_close
-> observe real MsQuic SHUTDOWN_COMPLETE
-> wait uv callbacks + MsQuic completions
-> release accounting/retired storage once
```

normal FIN 使用现有完整 convergence predicate；异常 abort 只跳过正常 FIN 等待，不跳过
ownership、terminal ledger 或 close callback。所有 handle API 在 loop 线程调用。

- [ ] **Step 5: 验证 terminal 和全部 libuv 单测**

Run: `rtk cmake --build build/libuv-plan --target tcpquic_libuv_terminal_convergence_test -j2`
Expected: PASS。

Run: `rtk ./build/libuv-plan/src/tcpquic_libuv_terminal_convergence_test`
Expected: exit 0，exactly-once counters 无重复。

Run: `rtk ctest --test-dir build/libuv-plan --output-on-failure -R 'libuv_(allocator|relay|terminal)'`
Expected: 100% tests passed。

- [ ] **Step 6: 提交 terminal convergence**

```bash
rtk git add src/CMakeLists.txt src/tunnel/libuv_relay_worker.cpp src/tunnel/libuv_relay_terminal.cpp src/unittest/libuv_terminal_convergence_test.cpp
rtk git commit -m "feat: converge libuv relay terminal ownership"
```

---

### Task 10: Linux 正常功能与故障矩阵验证

**Files:**
- Create: `scripts/run-linux-libuv-relay-functional.sh`
- Create: `tests/scripts/test_run_linux_libuv_relay_functional.py`
- Create: `docs/test/libuv-relay-linux-functional_cn.md`

**Interfaces:**
- Consumes: libuv `raypx2` 和现有 SOCKS5、HTTP CONNECT、port-forward 测试入口。
- Produces: Linux 功能报告、每 case 日志/metrics/allocator snapshot、明确的通过/失败计数。

- [ ] **Step 1: 写 runner dry-run/cleanup 测试**

断言 case 覆盖无压缩/压缩、upload/download、FIN/half-close、TCP refused/reset、QUIC abort、queue
pressure、allocation failure；失败也执行进程和临时目录清理，且报告 compiled backend 必须为
libuv。

- [ ] **Step 2: 运行脚本测试确认失败**

Run: `rtk python3 -m unittest tests/scripts/test_run_linux_libuv_relay_functional.py -v`
Expected: FAIL，runner 尚不存在。

- [ ] **Step 3: 实现 runner 和证据清单**

runner 启动两端 libuv build，逐 case 保存命令、exit code、proxy logs、admin snapshot、allocator
状态和 terminal counters；正常 case 之间不替换二进制。文档记录本轮只验证 Linux。

- [ ] **Step 4: 运行 Linux 功能验证**

Run: `rtk bash scripts/run-linux-libuv-relay-functional.sh --build-dir build/libuv-plan --output-dir build/libuv-functional-results`
Expected: 所有 normal transfer、compression、backpressure、FIN 和单 relay terminal cases PASS；
报告中 `compiled_relay_backend=libuv`、allocator=`mimalloc`、duplicate settlement=0。

- [ ] **Step 5: 回归 native build**

Run: `rtk cmake --build build/native-plan -j2`
Expected: PASS。

Run: `rtk ctest --test-dir build/native-plan --output-on-failure`
Expected: 100% tests passed；若存在任务开始前已知失败，报告精确测试名并停止合入。

- [ ] **Step 6: 提交 Linux 功能 runner**

```bash
rtk git add scripts/run-linux-libuv-relay-functional.sh tests/scripts/test_run_linux_libuv_relay_functional.py docs/test/libuv-relay-linux-functional_cn.md
rtk git commit -m "test: validate libuv relay functionality on Linux"
```

---

### Task 11: 执行 Linux DGX native/libuv 对比

**Files:**
- Create: `docs/dgx-netem-delay-loss-matrix-20260715-libuv-eval/`（测试产物，不在代码提交中自动加入大体积原始文件）
- Modify: `docs/test/libuv-relay-linux-functional_cn.md`（只追加结果索引）

**Interfaces:**
- Consumes: Task 8 runner、Task 10 已验证的两个独立 build。
- Produces: 14 个 delay×loss case、双方向、每 case 至少三轮的 native/libuv 原始证据和比较报告。

- [ ] **Step 1: 验证两套构建 metadata 一致性**

Run: `rtk scripts/run-dgx-netem-delay-loss-matrix.sh --preflight --native-build build/native-plan --libuv-build build/libuv-plan`
Expected: PASS；唯一预期 source-set 差异为 backend，commit、依赖、编译器、优化级别、tuning 和
mimalloc 条件一致。

- [ ] **Step 2: 执行 native 完整矩阵和 anchor**

Run: `rtk scripts/run-dgx-netem-delay-loss-matrix.sh --backend native --rounds 3 --matrix full`
Expected: 14 case × 2 directions × ≥3 rounds 完整，矩阵前后 `10ms+5%` anchor 存在。

- [ ] **Step 3: 执行 libuv 完整矩阵和 anchor**

Run: `rtk scripts/run-dgx-netem-delay-loss-matrix.sh --backend libuv --rounds 3 --matrix full`
Expected: 同样完整；任何二进制替换/进程重启使该 backend 整套矩阵作废并重跑。

- [ ] **Step 4: 生成比较报告但不输出淘汰决定**

Run: `rtk scripts/run-dgx-netem-delay-loss-matrix.sh --compare-only docs/dgx-netem-delay-loss-matrix-20260715-libuv-eval`
Expected: `comparison.csv`/`.md` 包含吞吐中位数/范围、失败、重传、CPU、RSS、context switch、
pending bytes、错误和 `libuv_delta_pct`；不存在 backend pass/fail 或自动淘汰线。

- [ ] **Step 5: 提交轻量结果索引**

```bash
rtk git add docs/test/libuv-relay-linux-functional_cn.md
rtk git commit -m "docs: index Linux libuv relay comparison results"
```

项目负责人依据完整报告人工决定是否继续；本任务不替其作取舍结论。

---

### Task 12: 最后实现 D-19 有界 graceful drain 与超时 abort

**Files:**
- Modify: `src/tunnel/libuv_relay_worker.h`
- Modify: `src/tunnel/libuv_relay_worker.cpp`
- Modify: `src/tunnel/libuv_relay_terminal.cpp`
- Modify: `src/tunnel/libuv_relay.cpp`
- Create: `src/unittest/libuv_relay_runtime_stop_test.cpp`
- Modify: `src/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 9 已验证的单 relay terminal 和 operation convergence。
- Produces: `TqUvRelayRuntime::Stop(deadline)`：Running→Draining→Closed，有界 graceful drain，超时统一 abort。

- [ ] **Step 1: 写 stop 时序和竞态测试**

覆盖拒绝新注册、Queued command 取消并 signal、Executing command 返回 ownership、grace 内正常
drain、deadline 后 abort、late `uv_async_send` gate、async handle close、loop 清空后
`uv_loop_close`、snapshot 与 stop 竞争、连续 100 次 Start/Stop。

- [ ] **Step 2: 运行测试确认失败**

Run: `rtk cmake --build build/libuv-plan --target tcpquic_libuv_relay_runtime_stop_test -j2`
Expected: FAIL，完整 D-19 尚不存在。

- [ ] **Step 3: 实现有界停止状态机**

```text
Running -> Draining
-> close registration admission
-> cancel Queued commands
-> grace deadline 内等待 D-17 normal convergence
-> deadline 到达，对 remaining relays 调用 D-18 abort
-> wait local ops/terminal handoff/uv_close callbacks
-> close uv_async_t admission then uv_close(async)
-> uv_run until no closing handles
-> uv_loop_close
-> join worker
-> Closed
```

强制 abort 仍不能越过 callback、operation、ledger 和 close ownership。记录 graceful drained、
forced abort、deadline exceeded、remaining handles/operations 和各阶段耗时。

- [ ] **Step 4: 验证 stop tests、ASan 和反复启停**

Run: `rtk cmake --build build/libuv-plan --target tcpquic_libuv_relay_runtime_stop_test -j2`
Expected: PASS。

Run: `rtk ./build/libuv-plan/src/tcpquic_libuv_relay_runtime_stop_test --iterations 100`
Expected: exit 0，无 remaining handle/operation、无 duplicate release。

Run: `rtk cmake -S . -B build/libuv-asan -DTCPQUIC_RELAY_BACKEND=libuv -DQUIC_ENABLE_ASAN=ON`
Expected: configure PASS，allocator mode 为 system。

Run: `rtk cmake --build build/libuv-asan --target tcpquic_libuv_relay_runtime_stop_test -j2`
Expected: PASS。

Run: `rtk ./build/libuv-asan/src/tcpquic_libuv_relay_runtime_stop_test --iterations 100`
Expected: exit 0，ASan 无 error。

- [ ] **Step 5: 提交完整停止**

```bash
rtk git add src/CMakeLists.txt src/tunnel/libuv_relay_worker.h src/tunnel/libuv_relay_worker.cpp src/tunnel/libuv_relay_terminal.cpp src/tunnel/libuv_relay.cpp src/unittest/libuv_relay_runtime_stop_test.cpp
rtk git commit -m "feat: add bounded libuv relay runtime shutdown"
```

---

### Task 13: 全量验证、设计追踪与集成到 master

**Files:**
- Modify: `docs/libuv_relay_backend_integration_design_cn.md`
- Modify: `docs/test/libuv-relay-linux-functional_cn.md`

**Interfaces:**
- Consumes: Tasks 1～12 全部 commit 和测试证据。
- Produces: D-01～D-27 requirement-to-test traceability、Linux 验证状态和最终集成 commit。

> 2026-07-16 范围更新：负责人确认本次只闭环 libuv backend。下述 native regression 与
> native 零改动审计取消，不再作为 Task 13 完成门禁。后续如需从当前状态派生只保留 libuv
> 代码的分支，作为独立目标另行设计和执行；本任务不删除 native 实现。

- [x] **Step 1: 建立 D-01～D-27 追踪表**

每个编号记录实现文件、测试 target、验证平台和证据目录。D-24 标注“负责人判断，无自动淘汰线”；
D-19 标注最后完成；macOS/Windows 标注“未在本轮验证”，不能写成 PASS。

- [x] **Step 2: 执行完整 libuv build/test**

Run: `rtk cmake -S . -B build/libuv-final -DTCPQUIC_RELAY_BACKEND=libuv -DTCPQUIC_USE_MIMALLOC=ON -DCMAKE_BUILD_TYPE=Release`
Expected: configure PASS，compiled backend=libuv，allocator=mimalloc。

Run: `rtk cmake --build build/libuv-final -j2`
Expected: PASS。

Run: `rtk ctest --test-dir build/libuv-final --output-on-failure`
Actual: exit 0，但项目未注册 CTest，输出 `No tests were found`，不计为测试通过证据；Task 13
改为逐个执行 `build/libuv-final/bin/Release/tcpquic_*test`，53/53 路径通过。

Run: `rtk python3 scripts/check-libuv-backend-api.py src/tunnel/libuv_allocator.cpp src/tunnel/libuv_relay.cpp src/tunnel/libuv_relay_worker.cpp src/tunnel/libuv_relay_quic_to_tcp.cpp src/tunnel/libuv_relay_tcp_to_quic.cpp src/tunnel/libuv_relay_terminal.cpp`
Expected: exit 0。

- [x] **Step 3: 执行完整 native regression（已取消）**

负责人于 2026-07-16 取消本步骤；本次目标只要求完成 libuv 相关工作。此前已产生的 native
构建/测试输出仅作为附加信息，不属于完成条件。

Run: `rtk cmake -S . -B build/native-final -DTCPQUIC_RELAY_BACKEND=native -DCMAKE_BUILD_TYPE=Release`
Expected: configure PASS，compiled backend=native。

Run: `rtk cmake --build build/native-final -j2`
Expected: PASS。

Run: `rtk ctest --test-dir build/native-final --output-on-failure`
Expected: 100% tests passed。

- [x] **Step 4: 审计 native 零改动和 source-set 互斥（已取消）**

负责人于 2026-07-16 明确取消 native 代码零改动审计。source-set 的 libuv 选择、非法 backend
拒绝和 libuv 生产源码 API 门禁仍保留在本次 libuv 验证范围内。

Run: `rtk git diff master...HEAD -- src/tunnel/relay.cpp src/tunnel/linux_relay_worker.cpp src/tunnel/linux_relay_worker.h src/tunnel/darwin_relay_worker.cpp src/tunnel/darwin_relay_worker.h src/tunnel/windows_relay_worker.cpp src/tunnel/windows_relay_worker.h`
Expected: 无输出。

Run: `rtk git diff --check master...HEAD`
Expected: exit 0。

- [x] **Step 5: 提交追踪文档**

```bash
rtk git add docs/libuv_relay_backend_integration_design_cn.md docs/test/libuv-relay-linux-functional_cn.md
rtk git commit -m "docs: record libuv relay verification traceability"
```

- [x] **Step 6: 合入 master 前最终评审**

使用 `requesting-code-review` 对 source-set、allocator 顺序、registration ownership、两个数据方向、
terminal、D-19 和平台 API 门禁逐项审查。评审和全部 fresh verification 通过后，将集成分支以
非破坏方式合入 `master`；不删除 native，也不执行 D-25 最终淘汰，直到负责人依据 D-24 报告
作出明确决定。

---

## 并行 subagent 启动指令

三个并行 agent 均使用 `fork_turns="none"`，避免继承整个会话历史。创建固定分支/worktree：

```bash
rtk git worktree add .worktrees/libuv-q2t -b feat/libuv-q2t feat/libuv-relay-integration
rtk git worktree add .worktrees/libuv-t2q -b feat/libuv-t2q feat/libuv-relay-integration
rtk git worktree add .worktrees/libuv-observability -b feat/libuv-observability feat/libuv-relay-integration
```

WT-A prompt：

```text
在 feat/libuv-q2t worktree 完成本计划 Task 5。完整阅读设计 D-11～D-13、D-16～D-18 和计划
Task 5。只修改 libuv_relay_quic_to_tcp.cpp、libuv_relay_quic_to_tcp_test.cpp、
libuv_relay_quic_to_tcp.cmake。禁止修改 internal header、worker core、native 文件和其他 lane。
执行 Task 5 的 red-green 命令和 API 扫描。公共契约缺项时停止修改，返回字段名、类型、
ownership 理由和失败测试。完成后提交聚焦 commit并返回 hash、变更摘要和 fresh test 输出。
```

WT-B prompt：

```text
在 feat/libuv-t2q worktree 完成本计划 Task 6。完整阅读设计 D-14～D-18 和计划 Task 6。只修改
libuv_relay_tcp_to_quic.cpp、libuv_relay_tcp_to_quic_test.cpp、libuv_relay_tcp_to_quic.cmake。
禁止修改 internal header、worker core、native 文件和其他 lane。执行 Task 6 的 red-green 命令
和 API 扫描。公共契约缺项时停止修改，返回字段名、类型、ownership 理由和失败测试。完成后
提交聚焦 commit并返回 hash、变更摘要和 fresh test 输出。
```

WT-C prompt：

```text
在 feat/libuv-observability worktree依次完成本计划 Task 7 和 Task 8。完整阅读设计 D-22～D-24、
D-26～D-27 和对应计划任务。只修改 Task 7/8 文件清单，保留 relay_metrics.cpp 的 native 分支
行为以及当前 DGX 脚本已有修改。禁止修改 worker core、internal header和任何 native worker。
分别执行两个任务的 red-green、JSON/schema、shell和 API 扫描命令，每个任务各提交一个聚焦
commit；最后返回两个 hash、变更摘要和 fresh test 输出。
```

集成负责人对每个 agent 结果执行两阶段检查：先核对 D 编号和文件边界，再 cherry-pick 后运行
该 lane 测试与全体已合入 libuv tests。subagent 报告不能替代集成负责人的 fresh verification。
