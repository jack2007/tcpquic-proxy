#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "compress.h"
#include "platform_socket.h"
#include "tuning.h"

struct MsQuicStream;
class TqLinuxRelayWorker;
class TqWindowsRelayWorker;
class TqDarwinRelayWorker;
class TqStreamLifetime;

enum class TqRelayBackendType {
    None,
    LinuxWorker,
    WindowsWorker,
    DarwinWorker,
};

using TqRelayBackend = TqRelayBackendType;

// Application error code for QUIC_STREAM_EVENT_CANCEL_ON_LOSS (62-bit wire range).
inline constexpr uint64_t TqRelayStreamErrorCancelOnLoss = 0x54510001ull;

inline uint64_t TqRelayNextControlGeneration() {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

inline std::atomic<uint64_t>& TqRelayControlGenerationMismatchCount() {
    static std::atomic<uint64_t> mismatches{0};
    return mismatches;
}

inline std::atomic<uint64_t>& TqRelayControlStopSignaledCount() {
    static std::atomic<uint64_t> signaled{0};
    return signaled;
}

inline std::atomic<uint64_t>& TqRelayAccountingDuplicateReleaseCount() {
    static std::atomic<uint64_t> duplicates{0};
    return duplicates;
}

// Shared stop/accounting control observed by tunnel reaper and relay workers.
// Does not own worker, relay, binding, tunnel context, or stream owner.
struct TqRelayStopControl {
    const uint64_t Generation;
    std::atomic<bool> Stop{false};
    std::atomic<bool> ActiveAccountingReleased{false};
    std::atomic<bool> WorkerEndpointAlive{true};

    explicit TqRelayStopControl(uint64_t generation) : Generation(generation) {}
    TqRelayStopControl() : Generation(TqRelayNextControlGeneration()) {}

    // Generation mismatch records a diagnostic and leaves state unchanged.
    bool SignalStop(uint64_t expectedGeneration) {
        if (expectedGeneration != Generation) {
            TqRelayControlGenerationMismatchCount().fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const bool wasStopped = Stop.exchange(true, std::memory_order_acq_rel);
        if (!wasStopped) {
            TqRelayControlStopSignaledCount().fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    bool ReleaseActiveAccountingOnce() {
        bool expected = false;
        if (!ActiveAccountingReleased.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            TqRelayAccountingDuplicateReleaseCount().fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        TqRelayUnregisterActive();
        return true;
    }
};

using TqRelayControl = TqRelayStopControl;

struct TqRelayLinuxCommittedState {
    uintptr_t WorkerIdentity{0};
    uint64_t RelayId{0};
    uint32_t WorkerIndex{0};
    std::shared_ptr<TqRelayStopControl> Control;
    uint64_t ControlGeneration{0};
};

struct TqRelayHandle {
    std::atomic<bool> Stop{false};
    std::shared_ptr<TqRelayStopControl> Control{std::make_shared<TqRelayStopControl>()};
    uint64_t ControlGeneration{0};
    TqRelayBackendType Backend{TqRelayBackendType::None};
    TqLinuxRelayWorker* LinuxWorker{nullptr};
    uint64_t LinuxRelayId{0};
    uint32_t LinuxWorkerIndex{0};
    // Authoritative Linux publication. Backend/worker/id fields above remain
    // compatibility mirrors for non-concurrent diagnostics.
    std::shared_ptr<const TqRelayLinuxCommittedState> LinuxCommitted;
    TqWindowsRelayWorker* WindowsWorker{nullptr};
    uint64_t WindowsRelayId{0};
    uint64_t WindowsRelayGeneration{0};
    uint32_t WindowsWorkerIndex{0};
    TqDarwinRelayWorker* DarwinWorker{nullptr};
    uint64_t DarwinRelayId{0};
    uint32_t DarwinWorkerIndex{0};
};

inline std::shared_ptr<const TqRelayLinuxCommittedState>
TqRelayLinuxCommittedSnapshot(const TqRelayHandle* handle) {
    return handle != nullptr
        ? std::atomic_load(&handle->LinuxCommitted)
        : std::shared_ptr<const TqRelayLinuxCommittedState>{};
}

bool TqRelayStart(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo = TqCompressAlgo::None);

bool TqRelayStartManaged(
    TqSocketHandle tcpFd,
    MsQuicStream* stream,
    std::shared_ptr<TqStreamLifetime> streamOwner,
    ITqCompressor* compressor,
    ITqDecompressor* decompressor,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    TqCompressAlgo compressAlgo = TqCompressAlgo::None,
    bool* tcpFdConsumed = nullptr);

bool TqRelayStartQuicReceiveSink(
    MsQuicStream* stream,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    std::atomic<uint64_t>* receiveBytes);

bool TqRelayStartQuicReceiveSinkManaged(
    MsQuicStream* stream,
    std::shared_ptr<TqStreamLifetime> streamOwner,
    TqRelayHandle* handle,
    const TqTuningConfig& tuning,
    std::atomic<uint64_t>* receiveBytes);

void TqRelayStop(TqRelayHandle* handle);
void TqRelaySetTraceContext(TqRelayHandle* handle, uint64_t tunnelId, const char* target);
bool TqRelayLinuxFastPathEnabled(const TqRelayHandle* handle);
void TqRelayUpdateQuicReadAheadFromNetworkStats(
    uint64_t bandwidthBytesPerSecond,
    uint64_t smoothedRttUs);
uint64_t TqRelayCurrentQuicReadAheadBytes();
void TqRelayResetQuicReadAheadForTest(uint64_t initialBytes);
uint64_t TqRelayCurrentQuicReadAheadBytesForTest();
