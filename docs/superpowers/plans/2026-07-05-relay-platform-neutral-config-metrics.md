# Relay Platform-Neutral Config Metrics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce platform-neutral relay tuning/configuration names and neutral relay metrics JSON keys while preserving existing Linux-named compatibility.

**Architecture:** Add neutral `Relay*` fields as the canonical tuning surface, keep existing `LinuxRelay*` fields synchronized as legacy mirrors, and parse new `relay.common.*` / `--relay-*` overrides with higher priority than old `relay.linux.*` / `--linux-relay-*`. Metrics output gains `relay_*` aliases while continuing to emit legacy keys for existing admin UI and scripts.

**Tech Stack:** C++17, nlohmann/json, existing CMake unit tests, Darwin/Linux relay runtime code.

---

## File Structure

- Modify: `src/config/tuning.h`
  - Add neutral `Relay*` tuning fields next to existing `LinuxRelay*` fields.
  - Keep old fields for compatibility.
- Modify: `src/config/tuning.cpp`
  - Compute defaults into neutral fields.
  - Apply old and new overrides in the required precedence order.
  - Synchronize legacy `LinuxRelay*` mirrors after tuning changes.
  - Update relay backend/tuning print output if it names shared fields as Linux-only.
- Modify: `src/config/config.h`
  - Add new `TuningOverrideRelay*` fields for `relay.common` / `--relay-*`.
- Modify: `src/config/config.cpp`
  - Parse `relay.common.*`.
  - Parse new `--relay-*` CLI flags.
  - Preserve `relay.linux.*` and `--linux-relay-*`.
  - Update usage text and runtime config JSON serialization.
- Modify: `src/tunnel/darwin_relay_worker.cpp`
  - Read neutral `Relay*` tuning fields in `TqDarwinRelayRuntime::Start()`.
- Modify: `src/tunnel/linux_relay_worker.cpp`
  - Replace shared data-plane tuning reads with neutral `Relay*` fields where applicable.
- Modify: `src/runtime/relay_metrics.cpp`
  - Add `relay_*` JSON aliases before or alongside existing `linux_relay_*` output.
- Modify: `src/runtime/admin_console.cpp`
  - Prefer `data.relay_backend` in `platformRelayBackend()` while keeping legacy fallbacks.
- Modify: `src/unittest/tuning_test.cpp`
  - Add canonical/legacy mirror tests and CLI tests.
- Modify: `src/unittest/config_router_test.cpp`
  - Add JSON config parsing and usage assertions.
- Modify: `src/unittest/admin_http_test.cpp`
  - Update JS expectations for neutral backend key.
- Modify or add tests near existing admin/metrics tests if present.
- Modify: `docs/relay_macos.md`
  - Mark the P0 config/metrics item as implemented after code lands.

---

### Task 1: Add Neutral Tuning Fields and Synchronization

**Files:**
- Modify: `src/config/tuning.h`
- Modify: `src/config/tuning.cpp`
- Test: `src/unittest/tuning_test.cpp`

- [ ] **Step 1: Add neutral fields to `TqTuningConfig`**

In `src/config/tuning.h`, extend `TqTuningConfig` so the shared relay data-plane fields exist under neutral names while old Linux names remain present.

Add these fields before the existing `LinuxRelay*` block:

```cpp
    uint32_t RelayWorkerCount{0};
    uint32_t RelayMaxIov{16};
    size_t RelayReadChunkSize{128 * 1024};
    size_t RelayReadBatchBytes{1024 * 1024};
    size_t RelayQuicRecvBatchBytes{1024 * 1024};
    uint64_t RelayTcpWriteMaxBytes{0};
    uint64_t RelayTcpWriteBurstBytes{0};
    uint64_t RelayGlobalPendingBytes{256ull * 1024 * 1024};
    uint64_t RelayPerTunnelPendingBytes{4ull * 1024 * 1024};
    uint32_t RelayWorkerEventBudget{4096};
    uint32_t RelayEventQueueCapacity{4096};
    uint64_t RelayWorkerByteBudgetPerTick{64ull * 1024 * 1024};
    uint64_t RelayQuicReceiveCompleteBatchBytes{0};
```

Keep the existing `LinuxRelay*` fields immediately after these as legacy mirrors. Do not remove or rename them in this task.

- [ ] **Step 2: Add synchronization helper in `src/config/tuning.cpp`**

Add this helper in the anonymous namespace after `ApplyHighBdpPipelineScaling()` or near the other tuning helpers:

```cpp
void SyncLinuxRelayLegacyFields(TqTuningConfig& out) {
    out.LinuxRelayWorkerCount = out.RelayWorkerCount;
    out.LinuxRelayMaxIov = out.RelayMaxIov;
    out.LinuxRelayReadChunkSize = out.RelayReadChunkSize;
    out.LinuxRelayReadBatchBytes = out.RelayReadBatchBytes;
    out.LinuxRelayQuicRecvBatchBytes = out.RelayQuicRecvBatchBytes;
    out.LinuxRelayTcpWriteMaxBytes = out.RelayTcpWriteMaxBytes;
    out.LinuxRelayTcpWriteBurstBytes = out.RelayTcpWriteBurstBytes;
    out.LinuxRelayGlobalPendingBytes = out.RelayGlobalPendingBytes;
    out.LinuxRelayPerTunnelPendingBytes = out.RelayPerTunnelPendingBytes;
    out.LinuxRelayWorkerEventBudget = out.RelayWorkerEventBudget;
    out.LinuxRelayEventQueueCapacity = out.RelayEventQueueCapacity;
    out.LinuxRelayWorkerByteBudgetPerTick = out.RelayWorkerByteBudgetPerTick;
    out.LinuxRelayQuicReceiveCompleteBatchBytes = out.RelayQuicReceiveCompleteBatchBytes;
}
```

- [ ] **Step 3: Move default calculations to neutral fields**

In `TqApplyLinuxRelayDefaults()`, update assignments so they write neutral fields first.

Replace the start of the function with this shape:

```cpp
void TqApplyLinuxRelayDefaults(TqTuningConfig& out, TqTuningMode mode, uint64_t autoBudgetBytes) {
    const uint32_t detectedWorkers = TqDetectRelayWorkers();
    out.RelayWorkerCount = detectedWorkers;
    out.WindowsRelayWorkerCount = detectedWorkers;

    if (mode == TqTuningMode::Lan) {
        out.RelayMaxIov = 8;
        out.RelayReadChunkSize = 128 * 1024;
        out.RelayReadBatchBytes = 256 * 1024;
        out.RelayQuicRecvBatchBytes = 256 * 1024;
        out.RelayWorkerEventBudget = 1024;
        out.RelayEventQueueCapacity = 4096;
        out.RelayWorkerByteBudgetPerTick = 16ull * 1024 * 1024;
    } else {
        out.RelayMaxIov = 16;
        out.RelayReadChunkSize = 128 * 1024;
        out.RelayReadBatchBytes = 1024 * 1024;
        out.RelayQuicRecvBatchBytes = 1024 * 1024;
        out.RelayWorkerEventBudget = 4096;
        out.RelayEventQueueCapacity = 4096;
        out.RelayWorkerByteBudgetPerTick = 64ull * 1024 * 1024;
    }

    const uint64_t relayMemoryBytes =
        ClampU64(autoBudgetBytes, kAutoRelayBudgetMinBytes, kAutoRelayBudgetMaxBytes);
    const uint64_t targetPendingBytes = ComputeTargetPendingPerTunnelBytes(out);
    out.RelayGlobalPendingBytes = relayMemoryBytes / 2;
    out.RelayPerTunnelPendingBytes = targetPendingBytes;
    out.MaxPendingBufferBytesPerRelay = std::max<uint64_t>(
        out.RelayPerTunnelPendingBytes,
        out.RelayGlobalPendingBytes / std::max<uint32_t>(out.RelayWorkerCount, 1));
    SyncLinuxRelayLegacyFields(out);
}
```

- [ ] **Step 4: Update high-BDP scaling to neutral fields**

In `ApplyHighBdpPipelineScaling()`, update assignments to neutral fields:

```cpp
    out.RelayPerTunnelPendingBytes = pendingBytes;
    out.MaxPendingBufferBytesPerRelay = std::max<uint64_t>(
        pendingBytes,
        out.RelayGlobalPendingBytes / std::max<uint32_t>(out.RelayWorkerCount, 1u));
    out.RelayWorkerByteBudgetPerTick =
        std::min<uint64_t>(pendingBytes, 512ull * 1024 * 1024);
    out.InitialQuicReadAheadBytes =
        std::max<uint64_t>(out.InitialQuicReadAheadBytes, pendingBytes);
    out.RelayReadBatchBytes =
        std::max<uint64_t>(out.RelayReadBatchBytes, 4ull * 1024 * 1024);
    out.RelayQuicRecvBatchBytes =
        std::max<uint64_t>(out.RelayQuicRecvBatchBytes, 4ull * 1024 * 1024);
    out.RelayMaxIov = std::max<uint32_t>(out.RelayMaxIov, 32);
```

Ensure `SyncLinuxRelayLegacyFields(out)` is called after `ApplyHighBdpPipelineScaling(out)` in `TqComputeTuning()` so old fields match final neutral values.

- [ ] **Step 5: Add mirror assertions to `tuning_test.cpp`**

Add a local helper near the top of `src/unittest/tuning_test.cpp`:

```cpp
static void RequireRelayLegacyMirror(const TqTuningConfig& tuning) {
    TQ_TEST_REQUIRE(tuning.LinuxRelayWorkerCount == tuning.RelayWorkerCount);
    TQ_TEST_REQUIRE(tuning.LinuxRelayMaxIov == tuning.RelayMaxIov);
    TQ_TEST_REQUIRE(tuning.LinuxRelayReadChunkSize == tuning.RelayReadChunkSize);
    TQ_TEST_REQUIRE(tuning.LinuxRelayReadBatchBytes == tuning.RelayReadBatchBytes);
    TQ_TEST_REQUIRE(tuning.LinuxRelayQuicRecvBatchBytes == tuning.RelayQuicRecvBatchBytes);
    TQ_TEST_REQUIRE(tuning.LinuxRelayTcpWriteMaxBytes == tuning.RelayTcpWriteMaxBytes);
    TQ_TEST_REQUIRE(tuning.LinuxRelayTcpWriteBurstBytes == tuning.RelayTcpWriteBurstBytes);
    TQ_TEST_REQUIRE(tuning.LinuxRelayGlobalPendingBytes == tuning.RelayGlobalPendingBytes);
    TQ_TEST_REQUIRE(tuning.LinuxRelayPerTunnelPendingBytes == tuning.RelayPerTunnelPendingBytes);
    TQ_TEST_REQUIRE(tuning.LinuxRelayWorkerEventBudget == tuning.RelayWorkerEventBudget);
    TQ_TEST_REQUIRE(tuning.LinuxRelayEventQueueCapacity == tuning.RelayEventQueueCapacity);
    TQ_TEST_REQUIRE(tuning.LinuxRelayWorkerByteBudgetPerTick == tuning.RelayWorkerByteBudgetPerTick);
    TQ_TEST_REQUIRE(tuning.LinuxRelayQuicReceiveCompleteBatchBytes == tuning.RelayQuicReceiveCompleteBatchBytes);
}
```

Call this helper after representative `TqComputeTuning(cfg, cfg.Tuning);` calls in existing WAN, LAN, high-BDP, and override test blocks.

- [ ] **Step 6: Run tuning test**

Run:

```bash
cmake --build build --target tcpquic_tuning_test -j$(sysctl -n hw.ncpu)
build/bin/Release/tcpquic_tuning_test
```

Expected: build succeeds and test exits 0.

- [ ] **Step 7: Commit Task 1**

```bash
git add src/config/tuning.h src/config/tuning.cpp src/unittest/tuning_test.cpp
git commit -m "Add neutral relay tuning mirrors"
```

---

### Task 2: Add Neutral Config and CLI Overrides

**Files:**
- Modify: `src/config/config.h`
- Modify: `src/config/config.cpp`
- Modify: `src/config/tuning.cpp`
- Test: `src/unittest/config_router_test.cpp`
- Test: `src/unittest/tuning_test.cpp`

- [ ] **Step 1: Add new override fields**

In `src/config/config.h`, add these fields before the existing `TuningOverrideLinuxRelay*` fields:

```cpp
    uint32_t TuningOverrideRelayReadChunkSize{0};
    uint32_t TuningOverrideRelayTcpWriteMaxBytes{0};
    uint32_t TuningOverrideRelayTcpWriteBurstBytes{0};
    uint32_t TuningOverrideRelayEventQueueCapacity{0};
    uint32_t TuningOverrideRelayWorkerCount{0};
```

- [ ] **Step 2: Apply old overrides first, then new overrides**

In `ApplyCustomOverrides()` in `src/config/tuning.cpp`, change old Linux override assignments to write neutral fields, then add new override checks after the old block.

Use this order:

```cpp
    if (cfg.TuningOverrideLinuxRelayReadChunkSize > 0) {
        out.RelayReadChunkSize = cfg.TuningOverrideLinuxRelayReadChunkSize;
    }
    if (cfg.TuningOverrideLinuxRelayTcpWriteMaxBytes > 0) {
        out.RelayTcpWriteMaxBytes = cfg.TuningOverrideLinuxRelayTcpWriteMaxBytes;
    }
    if (cfg.TuningOverrideLinuxRelayTcpWriteBurstBytes > 0) {
        out.RelayTcpWriteBurstBytes = cfg.TuningOverrideLinuxRelayTcpWriteBurstBytes;
    }
    if (cfg.TuningOverrideLinuxRelayEventQueueCapacity > 0) {
        out.RelayEventQueueCapacity =
            TqNormalizeLinuxRelayEventQueueCapacity(cfg.TuningOverrideLinuxRelayEventQueueCapacity);
    }
    if (cfg.TuningOverrideLinuxRelayWorkerCount > 0) {
        out.RelayWorkerCount =
            TqNormalizeRelayWorkerCount(cfg.TuningOverrideLinuxRelayWorkerCount);
    }

    if (cfg.TuningOverrideRelayReadChunkSize > 0) {
        out.RelayReadChunkSize = cfg.TuningOverrideRelayReadChunkSize;
    }
    if (cfg.TuningOverrideRelayTcpWriteMaxBytes > 0) {
        out.RelayTcpWriteMaxBytes = cfg.TuningOverrideRelayTcpWriteMaxBytes;
    }
    if (cfg.TuningOverrideRelayTcpWriteBurstBytes > 0) {
        out.RelayTcpWriteBurstBytes = cfg.TuningOverrideRelayTcpWriteBurstBytes;
    }
    if (cfg.TuningOverrideRelayEventQueueCapacity > 0) {
        out.RelayEventQueueCapacity =
            TqNormalizeLinuxRelayEventQueueCapacity(cfg.TuningOverrideRelayEventQueueCapacity);
    }
    if (cfg.TuningOverrideRelayWorkerCount > 0) {
        out.RelayWorkerCount =
            TqNormalizeRelayWorkerCount(cfg.TuningOverrideRelayWorkerCount);
    }
```

Ensure `TqComputeTuning()` calls `SyncLinuxRelayLegacyFields(out)` after `ApplyCustomOverrides(cfg, out)`.

- [ ] **Step 3: Add `ParseCommonRelayConfig()`**

In `src/config/config.cpp`, add a method beside `ParseLinuxRelayConfig()`:

```cpp
    bool ParseCommonRelayConfig(const nlohmann::json& object, TqConfig& cfg) {
        if (!RequireObject(object, "relay.common must be an object")) return false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "read_chunk_size") {
                if (!ReadNonZeroUint32(item.value(), cfg.TuningOverrideRelayReadChunkSize)) return Error("invalid relay.common.read_chunk_size");
            } else if (key == "tcp_write_max_bytes") {
                if (!ReadNonZeroUint32(item.value(), cfg.TuningOverrideRelayTcpWriteMaxBytes)) return Error("invalid relay.common.tcp_write_max_bytes");
            } else if (key == "tcp_write_burst_bytes") {
                if (!ReadNonZeroUint32(item.value(), cfg.TuningOverrideRelayTcpWriteBurstBytes)) return Error("invalid relay.common.tcp_write_burst_bytes");
            } else if (key == "event_queue_capacity") {
                if (!ReadUint32InRange(
                        item.value(),
                        TqLinuxRelayEventQueueCapacityMin,
                        TqLinuxRelayEventQueueCapacityMax,
                        cfg.TuningOverrideRelayEventQueueCapacity)) {
                    return Error("invalid relay.common.event_queue_capacity");
                }
            } else if (key == "worker_count") {
                if (!ReadUint32InRange(
                        item.value(),
                        TqRelayWorkerCountMin,
                        TqRelayWorkerCountMax,
                        cfg.TuningOverrideRelayWorkerCount)) {
                    return Error("invalid relay.common.worker_count");
                }
            } else {
                return Error("unknown relay.common key: " + key);
            }
        }
        return true;
    }
```

- [ ] **Step 4: Wire `relay.common` into `ParseRelayConfig()`**

Update `ParseRelayConfig()` so it accepts `common`:

```cpp
            } else if (key == "common") {
                if (!ParseCommonRelayConfig(item.value(), cfg)) return false;
            } else if (key == "linux") {
```

- [ ] **Step 5: Add neutral CLI flags**

In `TqParseArgs()`, add branches before the old `--linux-relay-*` branches:

```cpp
        } else if (GetOptionValue(arg, "--relay-read-chunk-size", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--relay-read-chunk-size", err);
                if (value == nullptr) return false;
            }
            if (!ParseUint32(value, cfg.TuningOverrideRelayReadChunkSize) ||
                cfg.TuningOverrideRelayReadChunkSize == 0) {
                err = "invalid value for --relay-read-chunk-size";
                return false;
            }
        } else if (GetOptionValue(arg, "--relay-tcp-write-max-bytes", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--relay-tcp-write-max-bytes", err);
                if (value == nullptr) return false;
            }
            if (!ParseUint32(value, cfg.TuningOverrideRelayTcpWriteMaxBytes) ||
                cfg.TuningOverrideRelayTcpWriteMaxBytes == 0) {
                err = "invalid value for --relay-tcp-write-max-bytes";
                return false;
            }
        } else if (GetOptionValue(arg, "--relay-tcp-write-burst-bytes", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--relay-tcp-write-burst-bytes", err);
                if (value == nullptr) return false;
            }
            if (!ParseUint32(value, cfg.TuningOverrideRelayTcpWriteBurstBytes) ||
                cfg.TuningOverrideRelayTcpWriteBurstBytes == 0) {
                err = "invalid value for --relay-tcp-write-burst-bytes";
                return false;
            }
        } else if (GetOptionValue(arg, "--relay-event-queue-capacity", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--relay-event-queue-capacity", err);
                if (value == nullptr) return false;
            }
            if (!ParseUint32InRange(
                    value,
                    TqLinuxRelayEventQueueCapacityMin,
                    TqLinuxRelayEventQueueCapacityMax,
                    cfg.TuningOverrideRelayEventQueueCapacity)) {
                err = "invalid value for --relay-event-queue-capacity";
                return false;
            }
        } else if (GetOptionValue(arg, "--relay-worker-count", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--relay-worker-count", err);
                if (value == nullptr) return false;
            }
            if (!ParseUint32InRange(
                    value,
                    TqRelayWorkerCountMin,
                    TqRelayWorkerCountMax,
                    cfg.TuningOverrideRelayWorkerCount) ||
                cfg.TuningOverrideRelayWorkerCount == 0) {
                err = "invalid value for --relay-worker-count";
                return false;
            }
```

- [ ] **Step 6: Update usage text**

In `TqPrintUsage()`, add neutral flags before the legacy Linux flags:

```cpp
        "  --relay-read-chunk-size <bytes>\n"
        "                              Shared Linux/Darwin relay TCP read chunk size\n"
        "  --relay-tcp-write-max-bytes <bytes>\n"
        "                              Cap each shared relay TCP sendmsg\n"
        "  --relay-tcp-write-burst-bytes <bytes>\n"
        "                              Cap bytes per shared relay TCP write flush\n"
        "  --relay-event-queue-capacity <events>\n"
        "                              Shared relay event queue capacity (default 4096, 1024..1048576)\n"
        "  --relay-worker-count <n>\n"
        "                              Shared Linux/Darwin relay worker count (default auto-detect, 1..8)\n"
```

Update the old Linux descriptions to say `Legacy alias for ...` instead of Linux-only tuning.

- [ ] **Step 7: Update runtime config JSON serialization**

In `RuntimeConfigJson()`, build a `commonRelay` object before `linuxRelay`:

```cpp
    nlohmann::json commonRelay = nlohmann::json::object();
    if (cfg.TuningOverrideRelayReadChunkSize != 0) commonRelay["read_chunk_size"] = cfg.TuningOverrideRelayReadChunkSize;
    if (cfg.TuningOverrideRelayTcpWriteMaxBytes != 0) commonRelay["tcp_write_max_bytes"] = cfg.TuningOverrideRelayTcpWriteMaxBytes;
    if (cfg.TuningOverrideRelayTcpWriteBurstBytes != 0) commonRelay["tcp_write_burst_bytes"] = cfg.TuningOverrideRelayTcpWriteBurstBytes;
    if (cfg.TuningOverrideRelayEventQueueCapacity != 0) commonRelay["event_queue_capacity"] = cfg.TuningOverrideRelayEventQueueCapacity;
    if (cfg.TuningOverrideRelayWorkerCount != 0) commonRelay["worker_count"] = cfg.TuningOverrideRelayWorkerCount;
    if (!commonRelay.empty()) relay["common"] = std::move(commonRelay);
```

Keep the existing `linuxRelay` serialization after this block.

- [ ] **Step 8: Add tests for JSON and CLI neutral overrides**

In `src/unittest/config_router_test.cpp`, add assertions near existing config parsing tests that a config containing this block parses:

```json
"relay": {
  "linux": { "read_chunk_size": 131072 },
  "common": {
    "read_chunk_size": 262144,
    "tcp_write_max_bytes": 1048576,
    "tcp_write_burst_bytes": 2097152,
    "event_queue_capacity": 8192,
    "worker_count": 2
  }
}
```

After loading/finalizing, assert:

```cpp
if (cfg.Tuning.RelayReadChunkSize != 262144) return 201;
if (cfg.Tuning.LinuxRelayReadChunkSize != 262144) return 202;
if (cfg.Tuning.RelayTcpWriteMaxBytes != 1048576) return 203;
if (cfg.Tuning.RelayTcpWriteBurstBytes != 2097152) return 204;
if (cfg.Tuning.RelayEventQueueCapacity != 8192) return 205;
if (cfg.Tuning.RelayWorkerCount != 2) return 206;
```

Also update usage capture assertions to require `--relay-read-chunk-size` and old `--linux-relay-read-chunk-size`.

In `src/unittest/tuning_test.cpp`, add a CLI parse block for `--relay-read-chunk-size 262144` and assert both neutral and legacy fields equal `262144`.

- [ ] **Step 9: Run config and tuning tests**

Run:

```bash
cmake --build build --target tcpquic_config_router_test tcpquic_tuning_test -j$(sysctl -n hw.ncpu)
build/bin/Release/tcpquic_config_router_test
build/bin/Release/tcpquic_tuning_test
```

Expected: both tests exit 0.

- [ ] **Step 10: Commit Task 2**

```bash
git add src/config/config.h src/config/config.cpp src/config/tuning.cpp src/unittest/config_router_test.cpp src/unittest/tuning_test.cpp
git commit -m "Add neutral relay config overrides"
```

---

### Task 3: Switch Runtime Backends to Neutral Tuning Reads

**Files:**
- Modify: `src/tunnel/darwin_relay_worker.cpp`
- Modify: `src/tunnel/linux_relay_worker.cpp`
- Test: `src/unittest/darwin_relay_worker_io_test.cpp`
- Test: existing Linux relay tests if available on current platform

- [ ] **Step 1: Update Darwin runtime config mapping**

In `TqDarwinRelayRuntime::Start()` in `src/tunnel/darwin_relay_worker.cpp`, replace the tuning reads with neutral names:

```cpp
    const uint32_t count = std::max<uint32_t>(1, tuning.RelayWorkerCount);
```

Inside the worker config loop, use:

```cpp
        config.EventBudget = tuning.RelayWorkerEventBudget;
        config.ByteBudgetPerTick = tuning.RelayWorkerByteBudgetPerTick;
        config.ReadChunkSize = tuning.RelayReadChunkSize;
        config.ReadBatchBytes = tuning.RelayReadBatchBytes;
        config.MaxIov = tuning.RelayMaxIov;
        config.TcpWriteMaxBytes = tuning.RelayTcpWriteMaxBytes;
        config.TcpWriteBurstBytes = tuning.RelayTcpWriteBurstBytes;
        config.MaxPendingQuicReceiveBytesPerRelay = tuning.RelayPerTunnelPendingBytes;
        config.DeferredReceiveCompleteBatchBytes = tuning.RelayQuicReceiveCompleteBatchBytes;
```

Do not change `RelayMaxInFlightSends` or `MaxPendingBufferBytesPerRelay` in this task.

- [ ] **Step 2: Update Linux relay shared tuning reads**

In `src/tunnel/linux_relay_worker.cpp`, replace uses of these shared fields where they configure relay worker/data-plane behavior:

- `LinuxRelayWorkerCount` -> `RelayWorkerCount`
- `LinuxRelayWorkerEventBudget` -> `RelayWorkerEventBudget`
- `LinuxRelayWorkerByteBudgetPerTick` -> `RelayWorkerByteBudgetPerTick`
- `LinuxRelayReadChunkSize` -> `RelayReadChunkSize`
- `LinuxRelayReadBatchBytes` -> `RelayReadBatchBytes`
- `LinuxRelayQuicRecvBatchBytes` -> `RelayQuicRecvBatchBytes`
- `LinuxRelayMaxIov` -> `RelayMaxIov`
- `LinuxRelayTcpWriteMaxBytes` -> `RelayTcpWriteMaxBytes`
- `LinuxRelayTcpWriteBurstBytes` -> `RelayTcpWriteBurstBytes`
- `LinuxRelayPerTunnelPendingBytes` -> `RelayPerTunnelPendingBytes`
- `LinuxRelayQuicReceiveCompleteBatchBytes` -> `RelayQuicReceiveCompleteBatchBytes`
- `LinuxRelayEventQueueCapacity` -> `RelayEventQueueCapacity`

Keep metrics snapshot field names unchanged for now.

- [ ] **Step 3: Search for remaining behavior reads**

Search for `tuning.LinuxRelay` in `src/tunnel` and `src/runtime`. Remaining hits should be metrics/legacy output or places intentionally not migrated. If a remaining hit configures shared Linux/Darwin data-plane behavior, replace it with the matching neutral field.

- [ ] **Step 4: Build Darwin relay test target**

Run:

```bash
cmake --build build --target tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
build/bin/Release/tcpquic_darwin_relay_worker_io_test
```

Expected: build succeeds and test exits 0.

- [ ] **Step 5: Build proxy target**

Run:

```bash
cmake --build build --target tcpquic-proxy -j$(sysctl -n hw.ncpu)
```

Expected: build succeeds.

- [ ] **Step 6: Commit Task 3**

```bash
git add src/tunnel/darwin_relay_worker.cpp src/tunnel/linux_relay_worker.cpp
git commit -m "Use neutral relay tuning in runtimes"
```

---

### Task 4: Add Neutral Metrics Aliases and Admin Fallback

**Files:**
- Modify: `src/runtime/relay_metrics.cpp`
- Modify: `src/runtime/admin_console.cpp`
- Modify: `src/unittest/admin_http_test.cpp`
- Modify: `src/unittest/server_admin_test.cpp` if its stub JSON expectations need neutral key coverage

- [ ] **Step 1: Add helper for neutral metrics JSON output**

In `src/runtime/relay_metrics.cpp`, add a helper next to `TqAppendRelayMetricsJson()` or split the first neutral fields into a new function:

```cpp
void TqAppendNeutralRelayMetricsJson(std::ostringstream& out, const TqRelayMetricsSnapshot& metrics) {
    out << ',"relay_wakeups":' << metrics.Wakeups;
    out << ",\"relay_events_processed\":" << metrics.EventsProcessed;
    out << ",\"relay_pending_events\":" << metrics.PendingEvents;
    out << ",\"relay_pending_bytes\":" << metrics.PendingBytes;
    out << ",\"relay_buffer_bytes_in_use\":" << metrics.RelayBufferBytesInUse;
    out << ",\"relay_active_relays\":" << metrics.ActiveRelays;
    out << ",\"relay_current_pending_quic_receive_bytes\":" << metrics.CurrentPendingQuicReceiveBytes;
    out << ",\"relay_outstanding_quic_sends\":" << metrics.OutstandingQuicSends;
    out << ",\"relay_outstanding_quic_send_bytes\":" << metrics.OutstandingQuicSendBytes;
    out << ",\"relay_tcp_read_bytes\":" << metrics.TcpReadBytes;
    out << ",\"relay_tcp_write_bytes\":" << metrics.TcpWriteBytes;
    out << ",\"relay_deferred_receive_completes\":" << metrics.DeferredReceiveCompletes;
    out << ",\"relay_quic_receive_view_count\":" << metrics.QuicReceiveViewCount;
    out << ",\"relay_quic_receive_view_bytes\":" << metrics.QuicReceiveViewBytes;
    out << ",\"relay_quic_receive_paused_count\":" << metrics.QuicReceivePausedCount;
    out << ",\"relay_quic_receive_resumed_count\":" << metrics.QuicReceiveResumedCount;
    out << ",\"relay_errors\":" << metrics.Errors;
    out << ",\"relay_event_queue_full_errors\":" << metrics.EventQueueFullErrors;
    out << ",\"relay_event_queue_capacity\":" << metrics.LinuxRelayEventQueueCapacity;
    out << ",\"relay_last_quic_send_status\":" << metrics.LastQuicSendStatus;
    out << ',';
    TqAppendJsonString(out, "relay_backend", metrics.Backend);
}
```

If the existing function already appends an initial comma convention differently, adjust commas to keep valid JSON. The expected final `TqRelayMetricsFieldsJson()` result must parse through `nlohmann::json::parse` as it does today.

- [ ] **Step 2: Call neutral metrics helper before legacy metrics**

In `TqRelayMetricsFieldsJson()` or `TqAppendRelayMetricsJson()`, call `TqAppendNeutralRelayMetricsJson(out, metrics)` before the existing `linux_relay_*` append block. Keep every existing legacy key.

- [ ] **Step 3: Prefer neutral backend in admin console**

In `src/runtime/admin_console.cpp`, update `platformRelayBackend(data)` to read neutral key first:

```javascript
    function platformRelayBackend(data) {
      return firstDefined(
        data.relay_backend,
        data.linux_relay_backend,
        data.darwin_relay_backend,
        data.windows_relay_backend,
        data.backend,
        'unsupported');
    }
```

- [ ] **Step 4: Update admin console unit assertions**

In `src/unittest/admin_http_test.cpp`, update the existing JS text assertions to require `data.relay_backend` and keep the old key assertions:

```cpp
        if (js.find("data.relay_backend") == std::string_view::npos) return 437;
        if (js.find("data.linux_relay_backend") == std::string_view::npos) return 433;
        if (js.find("data.darwin_relay_backend") == std::string_view::npos) return 434;
        if (js.find("data.windows_relay_backend") == std::string_view::npos) return 435;
```

- [ ] **Step 5: Add metrics alias test coverage**

If there is an existing relay metrics unit test, add assertions that a synthetic `TqRelayMetricsSnapshot` with `Backend = "test"`, `ActiveRelays = 7`, and `Errors = 3` produces JSON containing both:

```cpp
"relay_backend":"test"
"linux_relay_backend":"test"
"relay_active_relays":7
"linux_relay_active_relays":7
"relay_errors":3
"linux_relay_errors":3
```

If no dedicated test exists, add minimal assertions in the nearest admin/server metrics test that calls `TqRelayMetricsFieldsJson()` directly.

- [ ] **Step 6: Run admin and metrics tests**

Run:

```bash
cmake --build build --target tcpquic_admin_http_test tcpquic_server_admin_test -j$(sysctl -n hw.ncpu)
build/bin/Release/tcpquic_admin_http_test
build/bin/Release/tcpquic_server_admin_test
```

Expected: both tests exit 0.

- [ ] **Step 7: Commit Task 4**

```bash
git add src/runtime/relay_metrics.cpp src/runtime/admin_console.cpp src/unittest/admin_http_test.cpp src/unittest/server_admin_test.cpp
git commit -m "Add neutral relay metrics aliases"
```

---

### Task 5: Documentation and Final Verification

**Files:**
- Modify: `docs/relay_macos.md`
- Modify: `docs/config_guide_cn.md` if it documents relay tuning keys
- Test/build: changed targets from Tasks 1-4 plus `tcpquic-proxy`

- [ ] **Step 1: Update `docs/relay_macos.md` P0 status**

In `docs/relay_macos.md`, update the P0 section “Darwin 配置复用 Linux 字段，语义和用户接口不清晰” to say the first phase is complete after this plan:

```markdown
### P0：Darwin 配置复用 Linux 字段，语义和用户接口不清晰 — **第一阶段已完成**

第一阶段已增加 `relay.common.*` / `--relay-*` 平台中性配置入口，并新增 `relay_*` metrics alias；旧 `relay.linux.*`、`--linux-relay-*` 和 `linux_relay_*` key 保留兼容。后续仍可继续清理内部 legacy `LinuxRelay*` 字段命名，但不再要求 macOS 用户通过 Linux 命名调优 Darwin relay。
```

Keep any receive sink P0 section unchanged.

- [ ] **Step 2: Update config guide if relay keys are documented**

Search `docs/config_guide_cn.md` for `relay.linux`, `linux-relay`, or relay tuning keys. If present, add `relay.common` as the recommended form and mark `relay.linux` as legacy compatibility. Use Chinese documentation style.

Recommended JSON example:

```json
{
  "relay": {
    "io_size": 1048576,
    "common": {
      "read_chunk_size": 262144,
      "event_queue_capacity": 8192,
      "worker_count": 4
    }
  }
}
```

- [ ] **Step 3: Full targeted build/test pass**

Run:

```bash
cmake --build build --target tcpquic-proxy tcpquic_tuning_test tcpquic_config_router_test tcpquic_admin_http_test tcpquic_server_admin_test tcpquic_darwin_relay_worker_io_test -j$(sysctl -n hw.ncpu)
build/bin/Release/tcpquic_tuning_test
build/bin/Release/tcpquic_config_router_test
build/bin/Release/tcpquic_admin_http_test
build/bin/Release/tcpquic_server_admin_test
build/bin/Release/tcpquic_darwin_relay_worker_io_test
```

Expected: build succeeds and all tests exit 0.

- [ ] **Step 4: Inspect final diff**

Run:

```bash
git diff -- src/config/tuning.h src/config/tuning.cpp src/config/config.h src/config/config.cpp src/tunnel/darwin_relay_worker.cpp src/tunnel/linux_relay_worker.cpp src/runtime/relay_metrics.cpp src/runtime/admin_console.cpp src/unittest/tuning_test.cpp src/unittest/config_router_test.cpp src/unittest/admin_http_test.cpp src/unittest/server_admin_test.cpp docs/relay_macos.md docs/config_guide_cn.md
```

Expected: diff contains no removal of legacy config or metrics keys, and no Darwin data-plane behavior changes beyond reading neutral fields.

- [ ] **Step 5: Commit Task 5**

```bash
git add docs/relay_macos.md docs/config_guide_cn.md
git commit -m "Document neutral relay config metrics"
```

If Step 4 reveals small fixes in code/tests, include those files in the commit only after rerunning the relevant tests.

---

## Self-Review Checklist

- Every new neutral config path has a legacy compatibility path.
- New `relay.common.*` overrides old `relay.linux.*` only when both are explicitly specified.
- `Relay*` and `LinuxRelay*` tuning fields are synchronized after defaults, high-BDP scaling, pool-budget scaling, and custom overrides.
- Darwin runtime reads neutral fields.
- Metrics JSON emits both `relay_*` and existing legacy keys.
- Admin console prefers `relay_backend` but retains old fallbacks.
- Documentation does not claim receive sink support.
- No build-system or dependency files are changed; use target-level incremental builds.

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-05-relay-platform-neutral-config-metrics.md`. Two execution options:

1. Subagent-Driven (recommended) - dispatch a fresh subagent per task, review between tasks, fast iteration.
2. Inline Execution - execute tasks in this session using executing-plans, batch execution with checkpoints.
