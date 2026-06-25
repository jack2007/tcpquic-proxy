# Windows IOCP Optimization Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the optimization and refactoring for Windows IOCP Relay Worker to prevent fatal exceptions on legitimate connection teardowns and prevent memory bursts by enforcing TCP backpressure.

**Architecture:** Modifies the existing `TqWindowsRelayWorker` and `RelayContext`. Enhances error checking in the `Run()` loop to downgrade teardown errors from fatal to graceful closures. Adds `PendingTcpSends` to `RelayContext` to implement in-flight connection backpressure, aligning the `InFlightTcpSends` metric with `RelayMaxInFlightSends`.

**Tech Stack:** C++, Windows IOCP, MsQuic

---

### Task 1: Differentiate Closing from Fatal (Soft Teardown)

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`

- [ ] **Step 1: Relax `HandleTcpSend` 0-byte completion error**

In `TqWindowsRelayWorker::HandleTcpSend()`, change the error reporting for `bytes == 0`. Instead of triggering a hard error unconditionally, check if the relay is closing or drained.

```cpp
    if (op->ReceiveView) {
        auto view = op->ReceiveView;
        if (view->Drained) {
            return;
        }
        if (bytes == 0 || static_cast<uint64_t>(bytes) > op->PostedLength) {
            (void)CompletePendingQuicReceive(relay, view);
            if (relay->CloseAfterDrained.load(std::memory_order_acquire) || relay->Closing.load(std::memory_order_acquire)) {
                CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "tcp_send_zero_bytes_closing");
            } else {
                RecordTcpHardErrorAndFail(relay, "tcp_send_receive_view_completion_error");
            }
            return;
        }
```

- [ ] **Step 2: Relax `Run()` IOCP general completion errors**

In `TqWindowsRelayWorker::Run()`, when `!ok` happens and it's not explicitly caught by `IsIocpTeardownError`, gracefully close instead of hard abort if we are in the teardown phase.

Modify the `!ok` block near the end:
```cpp
            if (relay && relay->Closing.load(std::memory_order_acquire)) {
                TryRetireRelay(relay);
                continue;
            }
            if (relay && (relay->CloseAfterDrained.load(std::memory_order_acquire) || relay->TcpRecvClosed.load(std::memory_order_acquire) || relay->TcpWriteClosed.load(std::memory_order_acquire))) {
                GracefulRelayDrains_.fetch_add(1, std::memory_order_relaxed);
                CloseRelay(relay, TqRelayCloseMode::GracefulDrain, "iocp_completion_error_teardown");
                continue;
            }
            RecordTcpHardErrorAndFail(relay, "iocp_completion_error", completionError);
            continue;
```

### Task 2: Implement TCP Backpressure Limit (PendingTcpSends)

**Files:**
- Modify: `src/tunnel/windows_relay_worker.h`
- Modify: `src/tunnel/windows_relay_worker.cpp`

- [ ] **Step 1: Add `PendingTcpSends` tracking in `RelayContext`**

In `src/tunnel/windows_relay_worker.cpp`, add a mutex and deque to `RelayContext`:
```cpp
    std::mutex PendingTcpSendLock;
    std::deque<std::unique_ptr<IoOperation>> PendingTcpSends;
```

- [ ] **Step 2: Declare `RetryPendingTcpSends` in the header**

In `src/tunnel/windows_relay_worker.h`, under `private:`, declare:
```cpp
    void RetryPendingTcpSends(const std::shared_ptr<RelayContext>& relay);
    bool PostTcpSendRaw(std::unique_ptr<IoOperation> op);
```

- [ ] **Step 3: Implement `PostTcpSendRaw` and modify `PostTcpSend`**

In `src/tunnel/windows_relay_worker.cpp`, rename the existing `PostTcpSend` body to `PostTcpSendRaw` and modify `PostTcpSend` to check limits:

```cpp
bool TqWindowsRelayWorker::PostTcpSendRaw(std::unique_ptr<IoOperation> op) {
    auto relay = op->Relay;
    if (!relay || relay->Closing.load() || op->Offset >= op->Buffer.size()) {
        return false;
    }
    op->WsaBuffer.buf = reinterpret_cast<char*>(op->Buffer.data() + op->Offset);
    op->WsaBuffer.len = static_cast<ULONG>(op->Buffer.size() - op->Offset);
    DWORD sent = 0;
    relay->InFlightTcpSends.fetch_add(1);
    IoOperation* raw = op.release();
    const int rc = ::WSASend(relay->TcpFd, &raw->WsaBuffer, 1, &sent, 0, &raw->Overlapped, nullptr);
    const int error = rc == 0 ? 0 : ::WSAGetLastError();
    if (rc != 0 && error != WSA_IO_PENDING) {
        relay->InFlightTcpSends.fetch_sub(1);
        delete raw;
        HandleTcpPostFailure(relay, "wsa_send_buffer_failed", error);
        return false;
    }
    TcpSendOperationsPosted_.fetch_add(1, std::memory_order_relaxed);
    if (error == WSA_IO_PENDING) {
        TcpSendWouldBlockOrPendingCount_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool TqWindowsRelayWorker::PostTcpSend(std::unique_ptr<IoOperation> op) {
    auto relay = op->Relay;
    if (!relay || relay->Closing.load()) {
        return false;
    }
    
    if (relay->InFlightTcpSends.load(std::memory_order_relaxed) >= relay->Tuning.RelayMaxInFlightSends) {
        std::lock_guard<std::mutex> guard(relay->PendingTcpSendLock);
        relay->PendingTcpSends.push_back(std::move(op));
        return true;
    }
    
    return PostTcpSendRaw(std::move(op));
}
```

- [ ] **Step 4: Implement `RetryPendingTcpSends`**

```cpp
void TqWindowsRelayWorker::RetryPendingTcpSends(const std::shared_ptr<RelayContext>& relay) {
    if (!relay || relay->Closing.load(std::memory_order_acquire)) {
        return;
    }
    while (relay->InFlightTcpSends.load(std::memory_order_relaxed) < relay->Tuning.RelayMaxInFlightSends) {
        std::unique_ptr<IoOperation> op;
        {
            std::lock_guard<std::mutex> guard(relay->PendingTcpSendLock);
            if (relay->PendingTcpSends.empty()) {
                return;
            }
            op = std::move(relay->PendingTcpSends.front());
            relay->PendingTcpSends.pop_front();
        }
        if (op) {
            (void)PostTcpSendRaw(std::move(op));
        }
    }
}
```

- [ ] **Step 5: Call `RetryPendingTcpSends` at the end of `HandleTcpSend`**

At the very end of `TqWindowsRelayWorker::HandleTcpSend()`, right after decrementing `InFlightTcpSends` and processing all logic, add:
```cpp
    RetryPendingTcpSends(relay);
```
Make sure to add it before any `return` or at the end of every block where a send completes and resources are cleaned up. 
*Note: Due to the multiple `return` paths in `HandleTcpSend`, the best approach is to call `RetryPendingTcpSends` near the places where `relay->InFlightTcpSends.fetch_sub(1)` happens, or wrap `HandleTcpSend` cleanup.*

Wait, `InFlightTcpSends.fetch_sub(1)` is currently called at the very beginning of `HandleTcpSend`. So we just need to place `RetryPendingTcpSends(relay);` at the end of the method, and inside any `return` paths that finish the event without creating new sends.

```cpp
    // Ensure RetryPendingTcpSends(relay); is called before returns in HandleTcpSend!
```

### Task 3: Cleanup and Memory Management during Teardown

**Files:**
- Modify: `src/tunnel/windows_relay_worker.cpp`

- [ ] **Step 1: Ensure `PendingTcpSends` are cleared on close**

In `TqWindowsRelayWorker::CloseRelay`, right before `TryRetireRelay(relay);`, clear `PendingTcpSends` to release memory:
```cpp
    {
        std::lock_guard<std::mutex> guard(relay->PendingTcpSendLock);
        relay->PendingTcpSends.clear();
    }
```

- [ ] **Step 2: Commit all changes**

```bash
git add docs/superpowers/specs/2026-06-25-windows-iocp-refactor-design.md
git add docs/superpowers/plans/2026-06-25-windows-iocp-refactor-plan.md
git add src/tunnel/windows_relay_worker.h
git add src/tunnel/windows_relay_worker.cpp
git commit -m "docs: write Windows IOCP refactor design and plan"
```
