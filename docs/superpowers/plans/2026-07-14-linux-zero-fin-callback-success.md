# Linux FIN-only callback SUCCESS 实施计划

> **目标：** 修复 Linux relay 在 FIN-only RECEIVE callback active 期间调用
> `ReceiveComplete(0)` 丢失 completion、导致 MsQuic `ReceiveDataPending` 永久不清零的问题。

> **设计依据：**
> [Linux FIN-only callback SUCCESS 修复设计](../specs/2026-07-14-linux-zero-fin-callback-success-design.md)

## 约束

- 仅修改 Linux relay 及其单元测试；不修改 Windows、macOS 或第三方 MsQuic。
- FIN-only RECEIVE 发布 worker 控制 view 后返回 `QUIC_STATUS_SUCCESS`，且该 view 不持有
  zero-length receive completion obligation。
- payload-only 与 data + FIN 继续返回 `QUIC_STATUS_PENDING` 并按正字节数完成。
- 不提交 git commit，除非用户另行明确要求。
- 保留工作区已有 `log/` 和其他用户改动。

## 任务 1：建立 FIN-only 回归测试（RED）

**文件：**

- 修改：`src/unittest/linux_relay_worker_io_test.cpp`
- 修改：`src/unittest/linux_terminal_convergence_test.cpp`

**步骤：**

1. 找出所有显式构造 `BufferCount=0 / TotalBufferLength=0 / FIN` 的 Linux callback 测试，
   区分真实 FIN-only、fake FIN 和 data + FIN。
2. 先修改主 FIN-only 测试，使其断言：
   - callback 返回 `QUIC_STATUS_SUCCESS`；
   - worker drain 后 TCP 对端读取 EOF；
   - fake `StreamReceiveComplete` 调用次数不增加；
   - `DeferredReceiveCompletes` 不增加；
   - relay 不进入 fatal stop。
3. 只构建并运行目标测试，记录旧实现因返回 `QUIC_STATUS_PENDING` 或零字节 completion
   计数变化而失败，确认失败原因正是新契约尚未实现：

   ```bash
   rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j 4
   rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
   ```

## 任务 2：实现最小 FIN-only ownership 修复（GREEN）

**文件：**

- 修改：`src/tunnel/linux_relay_worker.h`
- 修改：`src/tunnel/linux_relay_worker.cpp`

**步骤：**

1. 为 callback 到 `BuildPendingQuicReceive()` 的现有 precommit/fallback 调用链增加显式
   completion ownership 参数，避免 builder 根据 `fin && TotalLength == 0` 自动创建义务。
2. builder 仅在调用方声明需要时设置 `ZeroLengthFinCompletionPending=true`；保留
   `CompleteZeroLengthFinReceive()` 作为旧 view、cleanup 测试和防御性路径。
3. RECEIVE callback 在 fake FIN 和 closing 判断之后识别真实 FIN-only：

   ```text
   fin && TotalBufferLength == 0
   ```

4. FIN-only 仍发布现有 `QuicReceiveView`，但传入“无 completion obligation”；发布成功返回
   `QUIC_STATUS_SUCCESS`。正长度 RECEIVE 继续返回 `QUIC_STATUS_PENDING`。
5. 若 view 构造或发布失败，沿用 abort 路径并返回失败 status，不能返回 PENDING 后又没有
   可完成的 view。
6. 重新运行任务 1 的测试，确认通过。

## 任务 3：锁定 data + FIN 与边界行为

**文件：**

- 修改：`src/unittest/linux_relay_worker_io_test.cpp`

**步骤：**

1. 增补或强化 data + FIN 测试：至少 1 字节 payload、callback 返回
   `QUIC_STATUS_PENDING`、TCP 先收到完整 payload 再 EOF、completion bytes 等于 payload
   长度且没有额外零字节 completion。
2. 更新其他真实 FIN-only focused tests（包括 late TCP error 场景）的 status 与 completion
   断言；fake FIN、closing 和 fatal-stop 语义保持原断言。
3. 运行 Linux relay worker I/O 测试：

   ```bash
   rtk cmake --build build --target tcpquic_linux_relay_worker_io_test -j 4
   rtk build/bin/Release/tcpquic_linux_relay_worker_io_test
   ```

## 任务 4：回归验证与差异审查

**文件：**

- 验证：`src/tunnel/linux_relay_worker.cpp`
- 验证：`src/tunnel/linux_relay_worker.h`
- 验证：`src/unittest/linux_relay_worker_io_test.cpp`
- 验证：`docs/linux_zero_length_fin_callback_active_race_root_cause_cn.md`
- 验证：`docs/linux_zero_length_fin_terminal_retention_root_cause_cn.md`

**步骤：**

1. 运行 terminal convergence 测试：

   ```bash
   rtk cmake --build build --target tcpquic_linux_terminal_convergence_test -j 4
   rtk build/bin/Release/tcpquic_linux_terminal_convergence_test
   ```

2. 构建生产 binary：

   ```bash
   rtk cmake --build build --target tcpquic-proxy -j 4
   ```

3. 检查格式和最终差异：

   ```bash
   rtk git diff --check
   rtk git status --short
   rtk git diff -- src/tunnel/linux_relay_worker.cpp src/tunnel/linux_relay_worker.h \
     src/unittest/linux_relay_worker_io_test.cpp docs/
   ```

4. 确认最终差异满足：
   - 生产 FIN-only callback 不再创建或完成零字节 obligation；
   - worker 顺序、TCP `SHUT_WR` 和 terminal handoff 不变；
   - data + FIN 的正字节 PENDING 契约不变；
   - 未修改 `log/`，未处置仍运行的 PID 28214；
   - 未创建未经请求的 commit。
