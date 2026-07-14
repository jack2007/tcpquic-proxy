# Linux FIN-only callback SUCCESS 修复设计

## 1. 背景

服务端 PID 28214 的 `srv-1 / tun-935 / relay 454` 出现长期 terminal retention。
现场与源码分析见
[Linux 零长度 FIN callback-active completion 竞态根因分析](../../linux_zero_length_fin_callback_active_race_root_cause_cn.md)。

MsQuic 在 RECEIVE callback active 期间使用 `RecvCompletionLength` 的字节累加值传递
并发 receive completion。`ReceiveComplete(0)` 不改变该值，因此 worker 若在 callback
返回 `QUIC_STATUS_PENDING` 前调用该 API，MsQuic 无法观察到 completion，最终永久保持
`ReceiveDataPending=1`。

## 2. 范围

本修复只修改 Linux/epoll relay：

- `src/tunnel/linux_relay_worker.cpp`
- `src/tunnel/linux_relay_worker.h`
- `src/unittest/linux_relay_worker_io_test.cpp`
- `src/unittest/linux_terminal_convergence_test.cpp`

Windows、macOS、MsQuic 第三方源码、wire protocol 和 terminal watchdog 不在本次范围内。

## 3. 行为契约

### 3.1 FIN-only RECEIVE

满足以下条件的真实 RECEIVE 为 FIN-only：

```text
QUIC_RECEIVE_FLAG_FIN != 0
TotalBufferLength == 0
```

fake FIN 检测仍先于本规则执行，保持现有 fake FIN terminal 路径不变。

FIN-only callback 必须：

1. 将 FIN 控制语义发布给 relay worker；
2. 不创建 receive completion obligation；
3. 返回 `QUIC_STATUS_SUCCESS`；
4. 不调用 `StreamReceiveComplete(0)`。

MsQuic 在 callback 返回路径内同步消费 FIN。worker 继续负责在前序 TCP write 排空后执行
`shutdown(SHUT_WR)`，并在双向关闭条件满足后进入 graceful terminal handoff。

### 3.2 正长度 RECEIVE

以下路径保持现有行为：

- payload-only RECEIVE；
- payload + FIN；
- compressed payload；
- sink receive；
- precommit 和 callback fallback queue。

它们继续返回 `QUIC_STATUS_PENDING`，并按实际正字节数调用 `ReceiveComplete(bytes)`。

## 4. 数据流

FIN-only 继续复用 `TqPendingQuicReceive` 和 `QuicReceiveView`，避免新增一套 queue-full、
precommit 和 stale-generation 控制路径。

callback 在构造 view 时显式传入 completion ownership：

```text
FIN-only: ZeroLengthFinCompletionPending=false
其他 PENDING receive: 保持现有 completion 字节语义
```

worker 处理 FIN-only view：

```text
按 event queue 顺序等待前序 payload
-> 不调用 ReceiveComplete(0)
-> 设置 TcpWriteShutdownQueued
-> PendingQuicReceives 为空后 shutdown(SHUT_WR)
-> MaybeStopFullyClosedRelay
```

若普通 event queue 满，view 仍进入 `CallbackPendingQuicReceives`。由于 FIN-only 没有 payload
指针和 completion obligation，callback 返回 `SUCCESS` 后保留该控制 view 是安全的。

## 5. 错误和终止语义

- view 分配或发布失败时沿用现有 callback abort 路径；callback 返回失败 status，MsQuic
  将该 RECEIVE 按非 PENDING 路径处理，不遗留 completion obligation。
- relay 已 closing 时 callback 继续返回 `SUCCESS`，不创建 view。
- terminal 后到达的 stale FIN control view 只做 bookkeeping discard，不调用 stream API。
- `CompleteZeroLengthFinReceive()` 暂时保留，用于兼容旧 pending view、显式 cleanup 测试和
  防御性路径；正常生产 FIN-only callback 不再创建此 obligation。
- 本修复不新增 graceful fatal watchdog，也不通过 connection abort 掩盖单 stream 问题。

## 6. 测试设计

遵循 TDD，先修改测试并观察旧实现失败。

### 6.1 FIN-only callback 行为

使用真实 `TqLinuxRelayWorker::DispatchStreamEventForTest()`：

- 输入 `BufferCount=0 / TotalBufferLength=0 / FIN`；
- 断言返回 `QUIC_STATUS_SUCCESS`；
- worker drain 后对端 TCP 读取 EOF，证明完成 `SHUT_WR`；
- fake `StreamReceiveComplete` 调用次数不增加；
- `DeferredReceiveCompletes` 不增加；
- relay 不进入 fatal stop。

旧实现返回 `QUIC_STATUS_PENDING` 且调用 `ReceiveComplete(0)`，因此测试必须先以预期原因
失败。

### 6.2 data + FIN 保持 PENDING

- 输入至少 1 字节 payload 并携带 FIN；
- 断言 callback 仍返回 `QUIC_STATUS_PENDING`；
- TCP 收到完整 payload 后才 EOF；
- completion bytes 等于 payload 长度且没有零字节额外 completion。

### 6.3 既有竞态回归

更新所有明确构造 FIN-only RECEIVE 的 Linux focused tests，使其断言新契约；fake FIN、
late TCP error、closing、queue fallback 和 terminal convergence 的其他断言保持不变。

## 7. 验证门禁

至少运行：

```bash
rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j 4
rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
rtk cmake --build build --target tcpquic_linux_terminal_convergence_test -j 4
rtk build/bin/Release/tcpquic_linux_terminal_convergence_test
rtk cmake --build build --target tcpquic-proxy -j 4
```

验收条件：

- focused test 红绿过程可证明测试能捕获旧行为；
- 所有上述命令退出码为 0；
- FIN-only 路径没有 `ReceiveComplete(0)`；
- data + FIN 仍按正字节数完成；
- `git diff --check` 无错误；
- 不修改用户已有的 `log/`。

## 8. 非目标与后续工作

本修复不处理已经卡住的 PID 28214 stream；旧进程需要 connection 级恢复或替换 binary
后重启。

后续可独立评估：

- Windows/macOS 是否也应采用 FIN-only callback `SUCCESS`；
- 删除不再可达的 Linux production zero-completion obligation；
- graceful terminal 超龄诊断和受控恢复策略。
