# Auto Relay Memory Budget Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove user-facing relay memory budget input and derive relay memory budgets internally from tuning, target bandwidth/RTT, active relays, and a process soft limit.

**Architecture:** Keep the existing `TqTuningConfig` fields and backpressure entry points, but replace `TqConfig::MaxMemoryMb` with an automatically computed internal budget. `TqComputeTuning()` will set an auto budget before Linux relay defaults consume it, and active relay scaling will continue through `TqApplyRelayPoolBudget()`.

**Tech Stack:** C++17, existing `src/config` parser/tuning code, existing assert-based unit tests, CMake targets `tcpquic_config_router_test`, `tcpquic_tuning_test`, and `tcpquic-proxy`.

---

## File Structure

- Modify `src/config/config.h`: remove `TqConfig::MaxMemoryMb`.
- Modify `src/config/config.cpp`: remove CLI/JSON parsing and usage text for `--max-memory-mb` / `tuning.max_memory_mb`.
- Modify `src/config/tuning.h`: expose auto budget helper declarations only if tests need them; otherwise keep helpers private to `tuning.cpp`.
- Modify `src/config/tuning.cpp`: add automatic relay memory budget calculation, system memory detection, and correct ordering so Linux defaults see the computed budget.
- Modify `src/main.cpp`: remove `TqSetRelayMemoryBudget(cfg.MaxMemoryMb)` and its user-facing startup log.
- Modify `src/unittest/config_router_test.cpp`: assert help omits `--max-memory-mb`, CLI rejects it, JSON `tuning.max_memory_mb` is rejected.
- Modify `src/unittest/tuning_test.cpp`: replace manual `TqSetRelayMemoryBudget()` user-style tests with auto budget expectations.
- Modify `src/docs/key_parameter.md` and `docs/20gbps-paramenter.md`: remove `--max-memory-mb` from active examples.

## Task 1: Remove User-Facing max-memory Entry Points

**Files:**
- Modify: `src/config/config.h`
- Modify: `src/config/config.cpp`
- Modify: `src/main.cpp`
- Test: `src/unittest/config_router_test.cpp`

- [ ] **Step 1: Write failing config/router tests**

In `src/unittest/config_router_test.cpp`, extend the existing `CaptureUsage()` block:

```cpp
if (usage.find("--max-memory-mb") != std::string::npos) return 149;
```

Add a CLI rejection case near other CLI parser negative tests:

```cpp
{
    const char* args[] = {
        "tcpquic-proxy",
        "server",
        "--listen",
        "0.0.0.0:4433",
        "--allow-targets",
        "127.0.0.1/32",
        "--max-memory-mb",
        "4096",
        "--cert",
        "a.crt",
        "--key",
        "a.key",
        "--ca",
        "ca.crt"};
    TqConfig cfg;
    std::string err;
    if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 150;
    if (err.find("unknown option: --max-memory-mb") == std::string::npos) return 151;
}
```

Add a JSON rejection case:

```cpp
{
    std::string file = WriteTempConfig(R"json({
        "tls":{"cert":"server.crt","key":"server.key","ca":"ca.crt"},
        "server":{
            "proto_listen":"0.0.0.0:4433",
            "allow_targets":["127.0.0.1/32"]
        },
        "tuning":{"max_memory_mb":4096}
    })json");
    const char* args[] = {"tcpquic-proxy", "server", "--config", file.c_str()};
    TqConfig cfg;
    std::string err;
    if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 152;
    if (err.find("unknown tuning key: max_memory_mb") == std::string::npos) return 153;
}
```

- [ ] **Step 2: Run the failing test**

Run:

```bash
rtk cmake --build build-asan --target tcpquic_config_router_test -j2
rtk build-asan/bin/Release/tcpquic_config_router_test
```

Expected: the binary fails before implementation because usage still contains `--max-memory-mb` or parser still accepts it.

- [ ] **Step 3: Remove production entry points**

Make these exact code changes:

- Delete `uint32_t MaxMemoryMb{0};` from `TqConfig` in `src/config/config.h`.
- Delete this `ParseTuningConfig()` branch from `src/config/config.cpp`:

```cpp
} else if (key == "max_memory_mb") {
    if (!ParseUint32(cfg.MaxMemoryMb)) return Error("invalid tuning.max_memory_mb");
```

- Delete this usage line from `TqPrintUsage()`:

```cpp
"  --max-memory-mb <n>          Cap relay pool memory across tunnels\n"
```

- Delete this CLI parser branch:

```cpp
} else if (GetOptionValue(arg, "--max-memory-mb", value)) {
    if (value == nullptr) {
        value = NextArg(i, argc, argv, "--max-memory-mb", err);
        if (value == nullptr) {
            return false;
        }
    }
    if (!ParseUint32(value, cfg.MaxMemoryMb)) {
        err = "invalid value for --max-memory-mb";
        return false;
    }
```

- Delete this startup code from `src/main.cpp`:

```cpp
TqSetRelayMemoryBudget(cfg.MaxMemoryMb);
if (cfg.MaxMemoryMb > 0) {
    std::fprintf(stderr,
        "tcpquic-proxy relay memory budget: %u MB (pool scales by active tunnels)\n",
        cfg.MaxMemoryMb);
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run:

```bash
rtk cmake --build build-asan --target tcpquic_config_router_test -j2
rtk build-asan/bin/Release/tcpquic_config_router_test
```

Expected: build succeeds and test exits 0.

## Task 2: Implement Automatic Relay Memory Budget

**Files:**
- Modify: `src/config/tuning.cpp`
- Modify: `src/config/tuning.h` if helper declarations are needed for tests
- Test: `src/unittest/tuning_test.cpp`

- [ ] **Step 1: Write failing tuning tests**

In `src/unittest/tuning_test.cpp`, add cases that express automatic behavior:

```cpp
{
    TqSetRelayMemoryBudget(0);
    TqConfig cfg{};
    cfg.TuningMode = TqTuningMode::Auto;
    cfg.TargetBandwidthMbps = 1000;
    cfg.TargetRttMs = 50;
    TqComputeTuning(cfg, cfg.Tuning);

    assert(TqGetRelayMemoryBudget() >= 256);
    assert(cfg.Tuning.LinuxRelayPerTunnelPendingBytes >= 16ull * 1024 * 1024);
    assert(cfg.Tuning.MaxPendingBufferBytesPerRelay >= cfg.Tuning.LinuxRelayPerTunnelPendingBytes);
}

{
    TqSetRelayMemoryBudget(0);
    TqConfig cfg{};
    cfg.TuningMode = TqTuningMode::Auto;
    cfg.TargetBandwidthMbps = 20000;
    cfg.TargetRttMs = 100;
    TqComputeTuning(cfg, cfg.Tuning);

    assert(cfg.Tuning.LinuxRelayPerTunnelPendingBytes == 500ull * 1024 * 1024);
    assert(cfg.Tuning.MaxPendingBufferBytesPerRelay >= 500ull * 1024 * 1024);
    assert(TqGetRelayMemoryBudget() >= 512);
}
```

Update existing tests that manually call `TqSetRelayMemoryBudget(512)` before `TqComputeTuning()`: these should either use auto budget or explicitly test internal budget functions after computation.

- [ ] **Step 2: Run the failing test**

Run:

```bash
rtk cmake --build build-asan --target tcpquic_tuning_test -j2
rtk build-asan/bin/Release/tcpquic_tuning_test
```

Expected: failure because `TqComputeTuning()` does not yet set an automatic budget.

- [ ] **Step 3: Add automatic budget helpers**

In `src/config/tuning.cpp`, add private helpers in the anonymous namespace:

```cpp
constexpr uint64_t kAutoRelayBudgetFallbackBytes = 512ull * MiB;
constexpr uint64_t kAutoRelayBudgetMinBytes = 256ull * MiB;
constexpr uint64_t kAutoRelayBudgetMaxBytes = 8ull * 1024ull * MiB;
constexpr uint64_t kRelayPerTunnelMinBytes = 4ull * MiB;
constexpr uint64_t kRelayPerTunnelTargetMinBytes = 16ull * MiB;
constexpr uint64_t kRelayPerTunnelTargetMaxBytes = 512ull * MiB;

uint64_t DetectSystemMemoryBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) && status.ullTotalPhys > 0) {
        return static_cast<uint64_t>(status.ullTotalPhys);
    }
    return kAutoRelayBudgetFallbackBytes;
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && pageSize > 0) {
        return static_cast<uint64_t>(pages) * static_cast<uint64_t>(pageSize);
    }
    return kAutoRelayBudgetFallbackBytes;
#endif
}

uint64_t ComputeSystemSoftLimitBytes() {
    const uint64_t detected = DetectSystemMemoryBytes();
    const uint64_t candidate = detected / 8;
    return ClampU64(candidate, kAutoRelayBudgetMinBytes, kAutoRelayBudgetMaxBytes);
}

uint64_t ComputeTargetPendingPerTunnelBytes(const TqTuningConfig& tuning) {
    return ClampU64(
        tuning.RelayDefaultIdealSend,
        kRelayPerTunnelTargetMinBytes,
        kRelayPerTunnelTargetMaxBytes);
}

uint32_t BytesToMbRoundUp(uint64_t bytes) {
    return static_cast<uint32_t>((bytes + MiB - 1) / MiB);
}
```

Add required includes:

```cpp
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif
```

- [ ] **Step 4: Apply auto budget during tuning computation**

Change `TqApplyLinuxRelayDefaults()` to receive an auto budget instead of reading a user-set global:

```cpp
void TqApplyLinuxRelayDefaults(TqTuningConfig& out, TqTuningMode mode, uint64_t autoBudgetBytes) {
    ...
    const uint64_t relayMemoryBytes =
        autoBudgetBytes == 0 ? kAutoRelayBudgetFallbackBytes : autoBudgetBytes;
    out.LinuxRelayGlobalPendingBytes = relayMemoryBytes / 2;
    ...
}
```

In `TqComputeTuning()`, after profile/BDP defaults and before Linux defaults:

```cpp
const uint64_t softLimitBytes = ComputeSystemSoftLimitBytes();
const uint64_t targetPendingBytes = ComputeTargetPendingPerTunnelBytes(out);
const uint64_t autoBudgetBytes = ClampU64(
    std::max<uint64_t>(softLimitBytes, targetPendingBytes),
    kAutoRelayBudgetMinBytes,
    kAutoRelayBudgetMaxBytes);
TqSetRelayMemoryBudget(BytesToMbRoundUp(autoBudgetBytes));
TqApplyLinuxRelayDefaults(out, cfg.TuningMode, autoBudgetBytes);
```

After `ApplyHighBdpPipelineScaling(out)`, keep high-BDP pending at the BDP-derived value, but do not exceed `kRelayPerTunnelTargetMaxBytes`.

- [ ] **Step 5: Run tuning test**

Run:

```bash
rtk cmake --build build-asan --target tcpquic_tuning_test -j2
rtk build-asan/bin/Release/tcpquic_tuning_test
```

Expected: build succeeds and test exits 0.

## Task 3: Active Relay Budget Scaling and Runtime Safety

**Files:**
- Modify: `src/config/tuning.cpp`
- Test: `src/unittest/tuning_test.cpp`

- [ ] **Step 1: Write failing active relay scaling test**

In `src/unittest/tuning_test.cpp`, add:

```cpp
{
    TqSetRelayMemoryBudget(0);
    TqConfig cfg{};
    cfg.TuningMode = TqTuningMode::Wan;
    TqComputeTuning(cfg, cfg.Tuning);

    const uint32_t autoBudget = TqGetRelayMemoryBudget();
    assert(autoBudget >= 256);

    TqApplyRelayPoolBudget(cfg.Tuning, 100);
    assert(cfg.Tuning.RelayMaxFreeSendContexts >= 1);
    assert(cfg.Tuning.RelayMaxInFlightSends >= 1);
    assert(cfg.Tuning.RelayDefaultIdealSend <= static_cast<uint64_t>(autoBudget) * 1024ull * 1024ull / 100ull);
}
```

- [ ] **Step 2: Run failing test**

Run:

```bash
rtk cmake --build build-asan --target tcpquic_tuning_test -j2
rtk build-asan/bin/Release/tcpquic_tuning_test
```

Expected: if the auto budget from Task 2 is incomplete, this fails because `TqGetRelayMemoryBudget()` remains 0 or active relay scaling does not apply.

- [ ] **Step 3: Ensure pool budget uses internal auto budget only**

Keep `TqApplyRelayPoolBudget()` using `g_TqRelayMemoryBudgetMb`, but make sure the only production writer is `TqComputeTuning()`.

If `activeRelays` is high and `perTunnelBytes / RelayIoSize` is 0, current `ClampU32(..., 1, ...)` should keep one context. Preserve this behavior.

- [ ] **Step 4: Run tuning test**

Run:

```bash
rtk cmake --build build-asan --target tcpquic_tuning_test -j2
rtk build-asan/bin/Release/tcpquic_tuning_test
```

Expected: build succeeds and test exits 0.

## Task 4: Documentation Cleanup and Final Verification

**Files:**
- Modify: `src/docs/key_parameter.md`
- Modify: `docs/20gbps-paramenter.md`
- Optionally modify active scripts under `scripts/` only if they still pass `--max-memory-mb` as a current command-line option.

- [ ] **Step 1: Remove active documentation references**

Remove `--max-memory-mb` from active parameter examples in:

```text
src/docs/key_parameter.md
docs/20gbps-paramenter.md
```

Do not edit historical run logs or benchmark result directories under `docs/dgx-*`.

- [ ] **Step 2: Scan for active code references**

Run:

```bash
rtk rg -n -S -- "MaxMemoryMb|--max-memory-mb|tuning\\.max_memory_mb|relay memory budget:" src README.md docs/20gbps-paramenter.md src/docs scripts
```

Expected: no production parser/config field references remain. Historical docs under `docs/dgx-*` may still contain old command logs and should not be edited.

- [ ] **Step 3: Final build and tests**

Run:

```bash
rtk cmake --build build-asan --target tcpquic_config_router_test tcpquic_tuning_test tcpquic-proxy -j2
rtk build-asan/bin/Release/tcpquic_config_router_test
rtk build-asan/bin/Release/tcpquic_tuning_test
```

Expected: all commands exit 0.
