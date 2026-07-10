#include "compress.h"
#include "platform_socket.h"
#include "stream_lifetime.h"
#include "trace.h"
#include "windows_relay_worker.h"

#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
namespace {

uint64_t g_StreamReceiveCompleteBytes = 0;
uint64_t g_StreamReceiveCompleteCalls = 0;
uint64_t g_StreamReceiveSetEnabledCalls = 0;
BOOLEAN g_LastStreamReceiveEnabled = TRUE;
std::mutex g_StreamSendMutex;
std::vector<void*> g_StreamSendContexts;
std::vector<std::vector<uint8_t>> g_StreamSendPayloads;
QUIC_STATUS g_FakeStreamSendStatus = QUIC_STATUS_SUCCESS;

void QUIC_API FakeStreamReceiveComplete(HQUIC, uint64_t bufferLength) {
    g_StreamReceiveCompleteBytes += bufferLength;
    ++g_StreamReceiveCompleteCalls;
}

QUIC_STATUS QUIC_API FakeStreamReceiveSetEnabled(HQUIC, BOOLEAN isEnabled) {
    ++g_StreamReceiveSetEnabledCalls;
    g_LastStreamReceiveEnabled = isEnabled;
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API FakeStreamSend(
    HQUIC,
    const QUIC_BUFFER* const buffers,
    uint32_t bufferCount,
    QUIC_SEND_FLAGS,
    void* clientSendContext) {
    std::lock_guard<std::mutex> guard(g_StreamSendMutex);
    g_StreamSendContexts.push_back(clientSendContext);
    std::vector<uint8_t> payload;
    for (uint32_t i = 0; i < bufferCount; ++i) {
        const QUIC_BUFFER& buffer = buffers[i];
        if (buffer.Length != 0 && buffer.Buffer != nullptr) {
            payload.insert(payload.end(), buffer.Buffer, buffer.Buffer + buffer.Length);
        }
    }
    g_StreamSendPayloads.push_back(std::move(payload));
    if (QUIC_FAILED(g_FakeStreamSendStatus)) {
        return g_FakeStreamSendStatus;
    }
    return QUIC_STATUS_SUCCESS;
}

void ResetFakeStreamSends() {
    std::lock_guard<std::mutex> guard(g_StreamSendMutex);
    g_StreamSendContexts.clear();
    g_StreamSendPayloads.clear();
}

std::vector<void*> TakeFakeStreamSendContexts() {
    std::lock_guard<std::mutex> guard(g_StreamSendMutex);
    std::vector<void*> contexts;
    contexts.swap(g_StreamSendContexts);
    return contexts;
}

size_t FakeStreamSendPayloadCount() {
    std::lock_guard<std::mutex> guard(g_StreamSendMutex);
    return g_StreamSendPayloads.size();
}

void CompleteFakeStreamSends(TqWindowsRelayWorker& worker, MsQuicStream* stream, void* /*callbackContext*/) {
    (void)worker;
    for (void* context : TakeFakeStreamSendContexts()) {
        QUIC_STREAM_EVENT complete{};
        complete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
        complete.SEND_COMPLETE.ClientContext = context;
        (void)TqWindowsRelayDispatchTestStreamEvent(stream, &complete);
    }
}

bool WaitForFakeStreamSendCount(size_t expected, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (FakeStreamSendPayloadCount() >= expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return FakeStreamSendPayloadCount() >= expected;
}

bool WaitForTcpBytes(TqSocketHandle socket, std::vector<uint8_t>& output, size_t expected, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    uint8_t buffer[1024];
    while (std::chrono::steady_clock::now() < deadline && output.size() < expected) {
        const int received = TqRecv(socket, buffer, sizeof(buffer), TqRecvFlags::DontWait);
        if (received > 0) {
            output.insert(output.end(), buffer, buffer + received);
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return output.size() >= expected;
}

class FlushOnlyCompressor final : public ITqCompressor {
public:
    bool Compress(const uint8_t*, size_t inLen, std::vector<uint8_t>&, bool) override {
        InputBytes.fetch_add(inLen, std::memory_order_relaxed);
        CompressCalls.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool Flush(std::vector<uint8_t>& out) override {
        FlushCalls.fetch_add(1, std::memory_order_relaxed);
        out.push_back(0x5a);
        return true;
    }

    void Reset() override {}

    std::atomic<uint64_t> InputBytes{0};
    std::atomic<uint64_t> CompressCalls{0};
    std::atomic<uint64_t> FlushCalls{0};
};

class EmptyOutputCompressor final : public ITqCompressor {
public:
    TqWindowsRelayWorker* Worker{nullptr};
    uint64_t RelayId{0};
    bool CloseTcpBeforeReturning{false};

    bool Compress(const uint8_t*, size_t, std::vector<uint8_t>& out, bool) override {
        out.clear();
        if (CloseTcpBeforeReturning && Worker != nullptr && RelayId != 0) {
            (void)Worker->TestCloseRelayTcpSocketForPostRecvFailure(RelayId);
        }
        return true;
    }

    bool Flush(std::vector<uint8_t>& out) override {
        out.clear();
        return true;
    }

    void Reset() override {}
};

class TestDecompressor final : public ITqDecompressor {
public:
    bool Decompress(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out) override {
        out.assign(in, in + inLen);
        return true;
    }

    bool DecompressInto(
        const uint8_t* input,
        size_t inputLength,
        uint8_t* output,
        size_t outputCapacity,
        TqDecompressResult* result) override {
        const size_t produced = inputLength < outputCapacity ? inputLength : outputCapacity;
        if (produced != 0 && input != nullptr && output != nullptr) {
            std::memcpy(output, input, produced);
        }
        if (result != nullptr) {
            result->InputConsumed = produced;
            result->OutputProduced = produced;
            result->NeedsMoreInput = produced == inputLength;
            result->NeedsMoreOutput = produced < inputLength;
        }
        return true;
    }

    void Reset() override {}
};

bool StartRelayWorkerForTest(TqWindowsRelayWorker& worker) {
    return worker.Start() && worker.TestHasIocpForTest();
}

bool TestWindowsRelayReceiveViewIocpCallbackQueue() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
    g_StreamReceiveCompleteBytes = 0;
    g_StreamReceiveCompleteCalls = 0;

    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        MsQuic = nullptr;
        return false;
    }
    if (!TqSetNonBlocking(pair[1])) {
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    TqWindowsRelayWorker receiveWorker;
    if (!receiveWorker.Start()) {
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 64 * 1024;
    if (!receiveWorker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    const std::vector<uint8_t> expected{'h', 'e', 'l', 'l', 'o'};
    uint8_t first[] = {'h', 'e', 'l'};
    uint8_t second[] = {'l', 'o'};
    QUIC_BUFFER buffers[2]{};
    buffers[0].Buffer = first;
    buffers[0].Length = sizeof(first);
    buffers[1].Buffer = second;
    buffers[1].Length = sizeof(second);

    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.BufferCount = 2;
    event.RECEIVE.Buffers = buffers;

    const QUIC_STATUS status = TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event);
    if (status != QUIC_STATUS_PENDING) {
        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    std::vector<uint8_t> received;
    if (!WaitForTcpBytes(pair[1], received, expected.size(), 2000)) {
        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }
    if (received != expected) {
        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    TqWindowsRelayWorkerSnapshot snapshot{};
    do {
        snapshot = receiveWorker.Snapshot();
        if (snapshot.PendingQuicReceiveBytes == 0 && snapshot.PendingQuicReceiveQueueDepth == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);

    const bool ok = snapshot.DeferredReceiveCompleteBytes == 5 && snapshot.DeferredReceiveCompletes > 0 &&
        g_StreamReceiveCompleteBytes == 5 && g_StreamReceiveCompleteCalls > 0 &&
        snapshot.PendingQuicReceiveQueueDepth == 0 && snapshot.PendingQuicReceiveBytes == 0;

    receiveWorker.Stop();
    TqCloseSocket(pair[1]);
    MsQuic = nullptr;
    return ok;
}

bool TestWindowsRelayTcpReadBackpressureWatermarks() {
    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        return false;
    }

    const uint64_t relayId = handle.WindowsRelayId;
    worker.TestConfigureQuicSendBacklog(relayId, 8, 8);

    if (worker.MaybePostTcpRecvForTest(relayId)) {
        worker.Stop();
        return false;
    }
    if (!worker.TestGetTcpReadPausedByQuicBacklog(relayId)) {
        worker.Stop();
        return false;
    }

    worker.TestProcessQuicSendCompleteForTest(relayId, 5);

    if (worker.TestGetTcpReadPausedByQuicBacklog(relayId)) {
        worker.Stop();
        return false;
    }

    worker.Stop();
    return true;
}

bool TestWindowsRelaySnapshotObservability() {
    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        return false;
    }

    const uint64_t relayId = handle.WindowsRelayId;
    TqWindowsRelayWorkerSnapshot snapshot = worker.Snapshot();
    if (snapshot.WindowsCallbackIocpPostCount != 0 ||
        snapshot.WindowsCallbackIocpPostFailedCount != 0 ||
        snapshot.WindowsReceiveReadyPostCount != 0 ||
        snapshot.WindowsReceiveDrainScheduledCount != 0 ||
        snapshot.WindowsReceiveDrainCoalescedCount != 0 ||
        snapshot.WindowsPostedCallbackStaleDropCount != 0 ||
        snapshot.EventsProcessed != 0 ||
        snapshot.ActiveRelayStates.size() != 1) {
        worker.Stop();
        return false;
    }

    const TqWindowsRelayActiveSnapshot& active = snapshot.ActiveRelayStates.front();
    if (active.RelayId != relayId ||
        active.CallbackPendingQuicReceiveDepth != 0) {
        worker.Stop();
        return false;
    }

    worker.TestConfigureQuicSendBacklog(relayId, 8, 8);
    (void)worker.MaybePostTcpRecvForTest(relayId);
    snapshot = worker.Snapshot();
    if (snapshot.ActiveRelayStates.empty() ||
        !snapshot.ActiveRelayStates.front().TcpReadPausedByQuicBacklog ||
        snapshot.ActiveRelayStates.front().OutstandingQuicSendBytes != 8) {
        worker.Stop();
        return false;
    }

    const uint64_t resumeBefore = snapshot.TcpReadResumeByBacklogEvents;
    worker.TestProcessQuicSendCompleteForTest(relayId, 5);
    if (worker.TestGetTcpReadPausedByQuicBacklog(relayId)) {
        worker.Stop();
        return false;
    }
    snapshot = worker.Snapshot();
    if (snapshot.TcpReadResumeByBacklogEvents <= resumeBefore) {
        worker.Stop();
        return false;
    }

    worker.Stop();
    return true;
}

bool TestWindowsRelayLockAndCallbackMetricsInitialState() {
    TqWindowsRelayWorker worker;
    const TqWindowsRelayWorkerSnapshot snapshot = worker.Snapshot();
    return snapshot.WorkerLockAcquireCount == 1 &&
           snapshot.WorkerLockWaitNanos >= 0 &&
           snapshot.FindRelayByIdCount == 0 &&
           snapshot.CallbackDispatchNanos == 0 &&
           snapshot.CallbackReceiveBudgetRejectedCount == 0 &&
           snapshot.CallbackReceiveBudgetPausedCount == 0 &&
           snapshot.CallbackReceiveCopyBytes == 0 &&
           snapshot.CallbackReceiveCopyNanos == 0 &&
           snapshot.SnapshotBuildNanos > 0 &&
           snapshot.SnapshotActiveRelaysScanned == 0 &&
           snapshot.MaintenanceDrainCount == 0 &&
           snapshot.MaintenanceDrainNanos == 0 &&
           snapshot.MaintenanceRelaysProcessed == 0 &&
           snapshot.MaintenanceFullScanCount == 0 &&
           snapshot.MaintenanceFullScanRelaysScanned == 0 &&
           snapshot.ReceiveViewFinishLinearSearchCount == 0 &&
           snapshot.ReceiveViewFinishLinearSearchNanos == 0 &&
           snapshot.ReceiveViewFinishNotFrontCount == 0;
}

bool TestWindowsRelayFinishReceiveViewFrontPathAvoidsLinearSearch() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        worker.Stop();
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        return false;
    }
    if (!worker.TestEnqueueReceiveViewForTest(handle.WindowsRelayId, 128)) {
        worker.Stop();
        return false;
    }
    const auto before = worker.Snapshot();
    if (!worker.TestCompleteReceiveViewForCleanup(handle.WindowsRelayId, 128)) {
        worker.Stop();
        return false;
    }
    const auto after = worker.Snapshot();
    worker.Stop();
    return after.ReceiveViewFinishLinearSearchCount == before.ReceiveViewFinishLinearSearchCount &&
           after.ReceiveViewFinishNotFrontCount == before.ReceiveViewFinishNotFrontCount;
}

bool TestWindowsRelayFinishReceiveViewNotFrontIsCounted() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        worker.Stop();
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        return false;
    }
    if (!worker.TestEnqueueReceiveViewForTest(handle.WindowsRelayId, 64) ||
        !worker.TestEnqueueReceiveViewForTest(handle.WindowsRelayId, 96)) {
        worker.Stop();
        return false;
    }
    const auto before = worker.Snapshot();
    if (!worker.TestCompleteSecondReceiveViewForTest(handle.WindowsRelayId, 96)) {
        worker.Stop();
        return false;
    }
    const auto after = worker.Snapshot();
    worker.Stop();
    return after.ReceiveViewFinishLinearSearchCount == before.ReceiveViewFinishLinearSearchCount + 1 &&
           after.ReceiveViewFinishNotFrontCount == before.ReceiveViewFinishNotFrontCount + 1;
}

bool TestWindowsRelayCallbackReceiveCopyMetricsForTest() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        MsQuic = nullptr;
        worker.Stop();
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay = 4096;
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        MsQuic = nullptr;
        return false;
    }

    const auto before = worker.Snapshot();
    uint8_t payload[7]{1, 2, 3, 4, 5, 6, 7};
    QUIC_BUFFER buffer{sizeof(payload), payload};
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.Buffers = &buffer;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.TotalBufferLength = sizeof(payload);
    const QUIC_STATUS status = TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event);
    const auto after = worker.Snapshot();
    worker.Stop();
    MsQuic = nullptr;
    return status == QUIC_STATUS_PENDING &&
           after.CallbackReceiveCopyBytes == before.CallbackReceiveCopyBytes + sizeof(payload) &&
           after.CallbackReceiveCopyNanos >= before.CallbackReceiveCopyNanos;
}

bool TestWindowsRelayCallbackReceiveBudgetRejectsBeforeCopyForTest() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
    g_StreamReceiveSetEnabledCalls = 0;
    g_LastStreamReceiveEnabled = TRUE;

    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        MsQuic = nullptr;
        worker.Stop();
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay = 8;
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        MsQuic = nullptr;
        return false;
    }

    const auto before = worker.Snapshot();
    uint8_t payload[16]{};
    QUIC_BUFFER buffer{sizeof(payload), payload};
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.Buffers = &buffer;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.TotalBufferLength = sizeof(payload);
    const QUIC_STATUS status = TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event);
    const auto after = worker.Snapshot();
    worker.Stop();
    MsQuic = nullptr;
    return status == QUIC_STATUS_SUCCESS &&
           after.CallbackReceiveBudgetRejectedCount ==
               before.CallbackReceiveBudgetRejectedCount + 1 &&
           after.CallbackReceiveBudgetPausedCount ==
               before.CallbackReceiveBudgetPausedCount + 1 &&
           after.CallbackReceiveCopyBytes == before.CallbackReceiveCopyBytes &&
           g_StreamReceiveSetEnabledCalls == 1 &&
           g_LastStreamReceiveEnabled == FALSE;
}

bool TestWindowsRelayCallbackReceiveBudgetDoesNotRejectFinForTest() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
    g_StreamReceiveSetEnabledCalls = 0;

    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        MsQuic = nullptr;
        worker.Stop();
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay = 8;
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        MsQuic = nullptr;
        return false;
    }

    uint8_t payload[16]{};
    QUIC_BUFFER buffer{sizeof(payload), payload};
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.Buffers = &buffer;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.TotalBufferLength = sizeof(payload);
    event.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;
    const auto before = worker.Snapshot();
    const QUIC_STATUS status = TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event);
    const auto after = worker.Snapshot();
    worker.Stop();
    MsQuic = nullptr;
    return status == QUIC_STATUS_PENDING &&
           after.CallbackReceiveBudgetRejectedCount == before.CallbackReceiveBudgetRejectedCount &&
           after.CallbackReceiveBudgetPausedCount == before.CallbackReceiveBudgetPausedCount &&
           after.CallbackReceiveCopyBytes >= before.CallbackReceiveCopyBytes + sizeof(payload);
}

bool TestWindowsRelayCallbackReceiveBudgetSkipsStaleGenerationForTest() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
    g_StreamReceiveSetEnabledCalls = 0;

    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        MsQuic = nullptr;
        worker.Stop();
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay = 8;
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        MsQuic = nullptr;
        return false;
    }
    if (!worker.TestBumpCallbackBindingGenerationForTest(handle.WindowsRelayId, 1)) {
        worker.Stop();
        MsQuic = nullptr;
        return false;
    }

    const auto before = worker.Snapshot();
    uint8_t payload[16]{};
    QUIC_BUFFER buffer{sizeof(payload), payload};
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.Buffers = &buffer;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.TotalBufferLength = sizeof(payload);
    const QUIC_STATUS status = TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event);
    const auto after = worker.Snapshot();
    worker.Stop();
    MsQuic = nullptr;
    return status == QUIC_STATUS_PENDING &&
           after.CallbackReceiveBudgetRejectedCount == before.CallbackReceiveBudgetRejectedCount &&
           after.CallbackReceiveBudgetPausedCount == before.CallbackReceiveBudgetPausedCount &&
           g_StreamReceiveSetEnabledCalls == 0;
}

bool TestWindowsRelayCallbackReceiveBudgetSkipsClosingRelayForTest() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
    g_StreamReceiveSetEnabledCalls = 0;

    TqWindowsRelayWorker worker;
    if (!worker.TestCreateIocpForCallbackPostOnly()) {
        MsQuic = nullptr;
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay = 8;
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        MsQuic = nullptr;
        return false;
    }
    if (!worker.TestArmRelayClosingOnRelayOnlyForTest(handle.WindowsRelayId)) {
        worker.Stop();
        MsQuic = nullptr;
        return false;
    }

    const auto before = worker.Snapshot();
    const uint64_t setEnabledBefore = g_StreamReceiveSetEnabledCalls;
    uint8_t payload[16]{};
    QUIC_BUFFER buffer{sizeof(payload), payload};
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.Buffers = &buffer;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.TotalBufferLength = sizeof(payload);
    const QUIC_STATUS status = TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event);
    const auto after = worker.Snapshot();
    MsQuic = nullptr;
    return (status == QUIC_STATUS_PENDING || status == QUIC_STATUS_SUCCESS) &&
           after.CallbackReceiveBudgetRejectedCount == before.CallbackReceiveBudgetRejectedCount &&
           after.CallbackReceiveBudgetPausedCount == before.CallbackReceiveBudgetPausedCount &&
           g_StreamReceiveSetEnabledCalls == setEnabledBefore;
}

bool TestWindowsRelayMaintenanceQueueBudgetForTest() {
    TqWindowsRelayWorker worker;
    worker.SetMaintenanceBudgetForTest(1);
    alignas(MsQuicStream) unsigned char streamStorageA[sizeof(MsQuicStream)]{};
    alignas(MsQuicStream) unsigned char streamStorageB[sizeof(MsQuicStream)]{};
    auto* streamA = reinterpret_cast<MsQuicStream*>(streamStorageA);
    auto* streamB = reinterpret_cast<MsQuicStream*>(streamStorageB);
    streamA->Callback = MsQuicStream::NoOpCallback;
    streamB->Callback = MsQuicStream::NoOpCallback;
    TqRelayHandle handleA{};
    TqRelayHandle handleB{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelayForTest(streamA, &handleA, tuning, TqCompressAlgo::None) ||
        !worker.RegisterRelayForTest(streamB, &handleB, tuning, TqCompressAlgo::None)) {
        return false;
    }
    worker.TestScheduleMaintenanceForTest(handleA.WindowsRelayId);
    worker.TestScheduleMaintenanceForTest(handleB.WindowsRelayId);
    const auto before = worker.Snapshot();
    worker.TestDrainMaintenanceForTest();
    const auto afterOne = worker.Snapshot();
    worker.TestDrainMaintenanceForTest();
    const auto afterTwo = worker.Snapshot();
    return afterOne.MaintenanceRelaysProcessed == before.MaintenanceRelaysProcessed + 1 &&
           afterTwo.MaintenanceRelaysProcessed == before.MaintenanceRelaysProcessed + 2;
}

bool TestWindowsRelayCallbackOperationGenerationMismatchDropsForTest() {
    TqWindowsRelayWorker worker;
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        return false;
    }
    const uint64_t before = worker.Snapshot().WindowsPostedCallbackStaleDropCount;
    if (!worker.TestResolveStaleCallbackForTest(handle.WindowsRelayId)) {
        return false;
    }
    const uint64_t after = worker.Snapshot().WindowsPostedCallbackStaleDropCount;
    return after == before + 1;
}

bool TestWindowsRelayCallbackOperationByIdIdealBufferDispatchForTest() {
    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        worker.Stop();
        return false;
    }

    const bool ok = worker.TestDispatchIdealSendBufferByIdForTest(handle.WindowsRelayId, 12345);
    worker.Stop();
    return ok;
}

bool TestWindowsRelaySnapshotConcurrentWithRegisterAndStop() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        MsQuic = nullptr;
        return false;
    }

    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::thread sampler([&] {
        while (!stop.load(std::memory_order_acquire)) {
            const TqWindowsRelayWorkerSnapshot snapshot = worker.Snapshot();
            if (snapshot.ActiveRelays > snapshot.ActiveRelayStates.size()) {
                failed.store(true, std::memory_order_release);
            }
        }
    });

    using StreamStorage = std::aligned_storage_t<sizeof(MsQuicStream), alignof(MsQuicStream)>;
    std::vector<std::unique_ptr<StreamStorage>> streamStorage;
    streamStorage.reserve(16);
    std::vector<std::unique_ptr<TqRelayHandle>> handles;
    handles.reserve(16);
    for (int i = 0; i < 16; ++i) {
        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        if (!TqSocketPair(pair)) {
            failed.store(true, std::memory_order_release);
            break;
        }
        auto storage = std::make_unique<StreamStorage>();
        auto* stream = reinterpret_cast<MsQuicStream*>(storage.get());
        std::memset(stream, 0, sizeof(*stream));
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1 + i));
        auto handleOwner = std::make_unique<TqRelayHandle>();
        auto* handle = handleOwner.get();
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 4096;
        if (!worker.RegisterRelay(pair[0], stream, nullptr, nullptr, handle, tuning, TqCompressAlgo::None)) {
            failed.store(true, std::memory_order_release);
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            break;
        }
        worker.StopRelay(handle->WindowsRelayId);
        streamStorage.push_back(std::move(storage));
        handles.push_back(std::move(handleOwner));
        TqCloseSocket(pair[1]);
    }

    stop.store(true, std::memory_order_release);
    sampler.join();
    worker.Stop();
    MsQuic = nullptr;
    return !failed.load(std::memory_order_acquire);
}

bool TestWindowsRelayRegisterRunsOnWorkerForTest() {
    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        return false;
    }
    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        worker.Stop();
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    std::memset(stream, 0, sizeof(*stream));
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    const bool ok = worker.RegisterRelay(
        pair[0],
        stream,
        nullptr,
        nullptr,
        &handle,
        tuning,
        TqCompressAlgo::None);
    const TqWindowsRelayWorkerSnapshot snapshot = worker.Snapshot();
    worker.StopRelay(handle.WindowsRelayId);
    TqCloseSocket(pair[1]);
    worker.Stop();
    return ok && handle.Backend == TqRelayBackendType::WindowsWorker &&
        snapshot.ActiveRelays == 1 && snapshot.ActiveRelayStates.size() == 1;
}

int TestWindowsRelayQuicTeardownOnWorker() {
    {
        TqWindowsRelayWorker receiveWorker;
        if (!StartRelayWorkerForTest(receiveWorker)) {
            return 147;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 150;
        }
        const TqWindowsRelayWorkerSnapshot before = receiveWorker.Snapshot();
        QUIC_STREAM_EVENT aborted{};
        aborted.Type = QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED;
        aborted.PEER_RECEIVE_ABORTED.ErrorCode = 1;
        (void)TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &aborted);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        TqWindowsRelayWorkerSnapshot after{};
        do {
            after = receiveWorker.Snapshot();
            if (after.GracefulRelayDrains > before.GracefulRelayDrains &&
                handle.Stop.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (after.FatalRelayResets != before.FatalRelayResets ||
            after.GracefulRelayDrains <= before.GracefulRelayDrains ||
            !handle.Stop.load(std::memory_order_acquire)) {
            receiveWorker.Stop();
            if (after.FatalRelayResets != before.FatalRelayResets) {
                return 1511;
            }
            if (after.GracefulRelayDrains <= before.GracefulRelayDrains) {
                return 1512;
            }
            return 1513;
        }
        receiveWorker.Stop();
    }
    {
        TqWindowsRelayWorker receiveWorker;
        if (!StartRelayWorkerForTest(receiveWorker)) {
            receiveWorker.Stop();
            return 146;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 152;
        }
        const TqWindowsRelayWorkerSnapshot before = receiveWorker.Snapshot();
        QUIC_STREAM_EVENT aborted{};
        aborted.Type = QUIC_STREAM_EVENT_PEER_SEND_ABORTED;
        aborted.PEER_SEND_ABORTED.ErrorCode = 1;
        (void)TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &aborted);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        TqWindowsRelayWorkerSnapshot after{};
        do {
            after = receiveWorker.Snapshot();
            if (after.GracefulRelayDrains > before.GracefulRelayDrains &&
                handle.Stop.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (after.FatalRelayResets != before.FatalRelayResets ||
            after.GracefulRelayDrains <= before.GracefulRelayDrains ||
            !handle.Stop.load(std::memory_order_acquire)) {
            receiveWorker.Stop();
            return 153;
        }
        receiveWorker.Stop();
    }
    {
        TqWindowsRelayWorker receiveWorker;
        if (!StartRelayWorkerForTest(receiveWorker)) {
            receiveWorker.Stop();
            return 146;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 160;
        }
        QUIC_STREAM_EVENT shutdown{};
        shutdown.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        shutdown.SHUTDOWN_COMPLETE.ConnectionShutdown = TRUE;
        (void)TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &shutdown);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        TqWindowsRelayWorkerSnapshot snapshot{};
        do {
            snapshot = receiveWorker.Snapshot();
            if (handle.Stop.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (!handle.Stop.load(std::memory_order_acquire) || snapshot.FatalRelayResets != 0) {
            receiveWorker.Stop();
            return 161;
        }
        receiveWorker.Stop();
    }
    {
        TqWindowsRelayWorker receiveWorker;
        if (!StartRelayWorkerForTest(receiveWorker)) {
            receiveWorker.Stop();
            return 146;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 162;
        }
        QUIC_STREAM_EVENT shutdown{};
        shutdown.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        shutdown.SHUTDOWN_COMPLETE.ConnectionShutdown = FALSE;
        (void)TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &shutdown);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        TqWindowsRelayWorkerSnapshot snapshot{};
        do {
            snapshot = receiveWorker.Snapshot();
            if (handle.Stop.load(std::memory_order_acquire)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (snapshot.FatalRelayResets != 0 || !handle.Stop.load(std::memory_order_acquire)) {
            receiveWorker.Stop();
            return 163;
        }
        receiveWorker.Stop();
    }
    {
        TqWindowsRelayWorker receiveWorker;
        if (!StartRelayWorkerForTest(receiveWorker)) {
            receiveWorker.Stop();
            return 146;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 230;
        }
        const uint64_t relayId = handle.WindowsRelayId;
        if (!receiveWorker.TestMarkTcpSendInFlightForTest(relayId)) {
            receiveWorker.Stop();
            return 231;
        }
        const TqWindowsRelayWorkerSnapshot before = receiveWorker.Snapshot();
        if (!receiveWorker.TestHandleTcpPostFailureForTest(relayId, WSAECONNABORTED)) {
            receiveWorker.Stop();
            return 232;
        }
        bool closeAfterDrained = false;
        bool tcpRecvClosed = false;
        if (!receiveWorker.TestGetRelayDrainFlagsForTest(
                relayId, &closeAfterDrained, &tcpRecvClosed) ||
            !closeAfterDrained || !tcpRecvClosed) {
            receiveWorker.Stop();
            return 233;
        }
        if (handle.Stop.load(std::memory_order_acquire)) {
            receiveWorker.Stop();
            return 234;
        }
        const TqWindowsRelayWorkerSnapshot after = receiveWorker.Snapshot();
        if (after.FatalRelayResets != before.FatalRelayResets ||
            after.GracefulRelayDrains <= before.GracefulRelayDrains) {
            receiveWorker.Stop();
            return 235;
        }
        receiveWorker.Stop();
    }
    {
        TqWindowsRelayWorker receiveWorker;
        if (!StartRelayWorkerForTest(receiveWorker)) {
            receiveWorker.Stop();
            return 146;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 236;
        }
        const uint64_t relayId = handle.WindowsRelayId;
        receiveWorker.SetRelayTraceContext(relayId, 42, "example.test:443");
        if (!receiveWorker.TestMarkQuicSendInFlightForRetirement(relayId)) {
            receiveWorker.Stop();
            return 237;
        }
        const TqWindowsRelayWorkerSnapshot before = receiveWorker.Snapshot();
        if (!receiveWorker.TestHandleTcpPostFailureForTest(relayId, WSAECONNRESET)) {
            receiveWorker.Stop();
            return 238;
        }
        bool closeAfterDrained = false;
        bool tcpRecvClosed = false;
        if (!receiveWorker.TestGetRelayDrainFlagsForTest(
                relayId, &closeAfterDrained, &tcpRecvClosed) ||
            !closeAfterDrained || !tcpRecvClosed) {
            receiveWorker.Stop();
            return 239;
        }
        if (handle.Stop.load(std::memory_order_acquire)) {
            receiveWorker.Stop();
            return 240;
        }
        receiveWorker.TestProcessQuicSendCompleteForTest(relayId, 24);
        const TqWindowsRelayWorkerSnapshot after = receiveWorker.Snapshot();
        if (after.FatalRelayResets != before.FatalRelayResets ||
            after.GracefulRelayDrains <= before.GracefulRelayDrains) {
            receiveWorker.Stop();
            return 241;
        }
        receiveWorker.Stop();
    }
    {
        TqWindowsRelayWorker receiveWorker;
        if (!StartRelayWorkerForTest(receiveWorker)) {
            receiveWorker.Stop();
            return 146;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 242;
        }
        const TqWindowsRelayWorkerSnapshot before = receiveWorker.Snapshot();
        if (!receiveWorker.TestRecordIocpCompletionErrorForTest(
                handle.WindowsRelayId, true, ERROR_SEM_TIMEOUT)) {
            receiveWorker.Stop();
            return 243;
        }
        const TqWindowsRelayWorkerSnapshot after = receiveWorker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) ||
            after.FatalRelayResets <= before.FatalRelayResets ||
            after.TcpHardErrors <= before.TcpHardErrors) {
            receiveWorker.Stop();
            return 244;
        }
        receiveWorker.Stop();
    }
    {
        TqWindowsRelayWorker receiveWorker;
        if (!StartRelayWorkerForTest(receiveWorker)) {
            receiveWorker.Stop();
            return 146;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 245;
        }
        const TqWindowsRelayWorkerSnapshot before = receiveWorker.Snapshot();
        if (!receiveWorker.TestRecordIocpCompletionErrorForTest(
                handle.WindowsRelayId, false, ERROR_SEM_TIMEOUT)) {
            receiveWorker.Stop();
            return 246;
        }
        const TqWindowsRelayWorkerSnapshot after = receiveWorker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) ||
            after.FatalRelayResets <= before.FatalRelayResets ||
            after.TcpHardErrors <= before.TcpHardErrors) {
            receiveWorker.Stop();
            return 247;
        }
        receiveWorker.Stop();
    }
    return 0;
}

bool TestWindowsRelaySendCompleteIocpCallbackQueue() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamSend = FakeStreamSend;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
    g_FakeStreamSendStatus = QUIC_STATUS_SUCCESS;
    ResetFakeStreamSends();

    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        MsQuic = nullptr;
        return false;
    }

    TqWindowsRelayWorker receiveWorker;
    if (!receiveWorker.Start()) {
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!receiveWorker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    const char payload[] = "send-complete-queue-test";
    if (TqSend(pair[1], payload, std::strlen(payload), TqSendFlags::None) !=
        static_cast<int>(std::strlen(payload))) {
        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }
    if (!WaitForFakeStreamSendCount(1, 2000)) {
        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    TqWindowsRelayWorkerSnapshot before = receiveWorker.Snapshot();
    if (before.QuicSendCompleteEvents != 0) {
        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    void* callbackContext = stream->Context;
    CompleteFakeStreamSends(receiveWorker, stream, callbackContext);

    if (receiveWorker.Snapshot().PostTcpRecvFromSendCompleteCallbackCount != 0) {
        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    TqWindowsRelayWorkerSnapshot after{};
    do {
        after = receiveWorker.Snapshot();
        if (after.QuicSendCompleteEvents >= 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);

    bool inFlightZero = false;
    for (const auto& active : after.ActiveRelayStates) {
        if (active.RelayId == handle.WindowsRelayId && active.InFlightQuicSends == 0) {
            inFlightZero = true;
            break;
        }
    }

    const bool ok = after.QuicSendCompleteEvents >= 1 && after.PostTcpRecvFromSendCompleteCallbackCount == 0 &&
        inFlightZero;

    receiveWorker.Stop();
    TqCloseSocket(pair[1]);
    MsQuic = nullptr;
    return ok;
}

bool TestWindowsRelaySendCompleteCallbackDoesNotFindRelayForTest() {
    TqWindowsRelayWorker worker;
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 64 * 1024;
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        return false;
    }
    if (!worker.TestCreateIocpForCallbackPostOnly()) {
        return false;
    }

    auto* operation = worker.TestCreateQuicSendOperationForTest(handle.WindowsRelayId, 7);
    if (operation == nullptr) {
        return false;
    }

    auto owner = worker.LastTestStreamOwnerForTest();
    if (!owner) {
        return false;
    }
    void* completionKey = owner->RegisterSendCompletion(operation);
    if (completionKey == nullptr) {
        return false;
    }

    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = completionKey;

    const TqWindowsRelayWorkerSnapshot before = worker.Snapshot();
    if (TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event) !=
        QUIC_STATUS_SUCCESS) {
        return false;
    }
    const TqWindowsRelayWorkerSnapshot after = worker.Snapshot();
    const bool noCallbackFind = after.FindRelayByIdCount == before.FindRelayByIdCount;
    const bool drained = worker.TestDrainSingleQuicSendCompleteForTest();
    worker.Stop();
    return noCallbackFind && drained;
}

bool TestWindowsRelayReceiveCallbackDoesNotFindRelayForTest() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
    g_StreamReceiveCompleteBytes = 0;
    g_StreamReceiveCompleteCalls = 0;

    TqWindowsRelayWorker worker;
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 64 * 1024;
    if (!worker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
        MsQuic = nullptr;
        return false;
    }
    if (!worker.TestCreateIocpForCallbackPostOnly()) {
        MsQuic = nullptr;
        return false;
    }

    uint8_t data[] = {'r', 'x'};
    QUIC_BUFFER buffer{};
    buffer.Buffer = data;
    buffer.Length = sizeof(data);
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.Buffers = &buffer;

    const TqWindowsRelayWorkerSnapshot before = worker.Snapshot();
    const QUIC_STATUS status = TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event);
    const TqWindowsRelayWorkerSnapshot after = worker.Snapshot();
    const bool noCallbackFind = after.FindRelayByIdCount == before.FindRelayByIdCount;
    const bool drained = worker.TestDrainSingleReceiveReadyForTest();
    worker.Stop();
    MsQuic = nullptr;
    return status == QUIC_STATUS_PENDING && noCallbackFind && drained &&
        g_StreamReceiveCompleteBytes == sizeof(data) && g_StreamReceiveCompleteCalls == 1;
}

bool TestWindowsRelayTraceContextUsesWorkerQueue() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        MsQuic = nullptr;
        return false;
    }
    if (!TqSetNonBlocking(pair[1])) {
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
        if (handle.WindowsRelayId == 0) {
            TqCloseSocket(pair[0]);
        }
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    TqTraceLinuxRelayStreamState before{};
    std::string beforeTarget;
    if (!worker.TestGetRelayTraceStateForTest(handle.WindowsRelayId, &before, &beforeTarget)) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }
    if (before.TunnelId != 0 || before.Target != nullptr) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    TqWindowsRelayWorkerQueueBlockForTest block{};
    if (!worker.TestPostWorkerQueueBlockForTest(&block) ||
        !worker.TestWaitWorkerQueueBlockEnteredForTest(block, 2000)) {
        worker.TestReleaseWorkerQueueBlockForTest(block);
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    worker.SetRelayTraceContext(handle.WindowsRelayId, 12345, "queued.example:443");

    TqTraceLinuxRelayStreamState immediate{};
    std::string immediateTarget;
    if (!worker.TestGetRelayTraceStateForTest(handle.WindowsRelayId, &immediate, &immediateTarget)) {
        worker.TestReleaseWorkerQueueBlockForTest(block);
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }
    if (immediate.TunnelId != 0 || immediate.Target != nullptr) {
        worker.TestReleaseWorkerQueueBlockForTest(block);
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    worker.TestReleaseWorkerQueueBlockForTest(block);

    (void)worker.Snapshot();
    TqTraceLinuxRelayStreamState after{};
    std::string afterTarget;
    const bool sawTrace =
        worker.TestGetRelayTraceStateForTest(handle.WindowsRelayId, &after, &afterTarget) &&
        after.TunnelId == 12345 &&
        after.Target != nullptr &&
        std::strcmp(after.Target, "queued.example:443") == 0;

    worker.Stop();
    TqCloseSocket(pair[1]);
    MsQuic = nullptr;
    return sawTrace;
}

bool TestWindowsRelayTraceContextDropsStaleGeneration() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        MsQuic = nullptr;
        return false;
    }

    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
        if (handle.WindowsRelayId == 0) {
            TqCloseSocket(pair[0]);
        }
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    const uint64_t staleBefore = worker.Snapshot().WindowsPostedTraceContextStaleDropCount;
    const uint64_t staleGeneration = 999999;
    if (!worker.TestPostTraceContextForTest(
            handle.WindowsRelayId,
            staleGeneration,
            777,
            "stale.example:443")) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    (void)worker.Snapshot();
    const uint64_t staleAfter = worker.Snapshot().WindowsPostedTraceContextStaleDropCount;

    TqTraceLinuxRelayStreamState state{};
    std::string targetStorage;
    const bool ok = worker.TestGetRelayTraceStateForTest(
        handle.WindowsRelayId,
        &state,
        &targetStorage);
    worker.Stop();
    TqCloseSocket(pair[1]);
    MsQuic = nullptr;
    return ok && state.TunnelId == 0 && state.Target == nullptr &&
           staleAfter == staleBefore + 1;
}

bool TestWindowsRelayTraceContextAllowsNullTarget() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        MsQuic = nullptr;
        return false;
    }

    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
        if (handle.WindowsRelayId == 0) {
            TqCloseSocket(pair[0]);
        }
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    worker.SetRelayTraceContext(handle.WindowsRelayId, 888, nullptr);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    bool sawTrace = false;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)worker.Snapshot();
        TqTraceLinuxRelayStreamState state{};
        std::string targetStorage;
        if (worker.TestGetRelayTraceStateForTest(handle.WindowsRelayId, &state, &targetStorage) &&
            state.TunnelId == 888 &&
            state.Target == nullptr) {
            sawTrace = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    worker.Stop();
    TqCloseSocket(pair[1]);
    MsQuic = nullptr;
    return sawTrace;
}

bool TestWindowsRelayTraceContextIgnoresZeroIds() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        MsQuic = nullptr;
        return false;
    }

    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
        if (handle.WindowsRelayId == 0) {
            TqCloseSocket(pair[0]);
        }
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    const uint64_t errorsBefore = worker.Snapshot().Errors;
    worker.SetRelayTraceContext(0, 123, "ignored.example:443");
    worker.SetRelayTraceContext(handle.WindowsRelayId, 0, "ignored.example:443");
    const uint64_t errorsAfter = worker.Snapshot().Errors;

    TqTraceLinuxRelayStreamState state{};
    std::string targetStorage;
    const bool ok = worker.TestGetRelayTraceStateForTest(
        handle.WindowsRelayId,
        &state,
        &targetStorage);

    worker.Stop();
    TqCloseSocket(pair[1]);
    MsQuic = nullptr;
    return ok && state.TunnelId == 0 && state.Target == nullptr && errorsAfter == errorsBefore;
}

bool TestWindowsRelayTraceContextIgnoredWhenRelayMissing() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        MsQuic = nullptr;
        return false;
    }

    const uint64_t errorsBefore = worker.Snapshot().Errors;
    worker.SetRelayTraceContext(424242, 123, "missing.example:443");
    const uint64_t errorsAfter = worker.Snapshot().Errors;

    worker.Stop();
    MsQuic = nullptr;
    return errorsAfter == errorsBefore;
}

bool TestWindowsRelayTraceContextIgnoredWhenWorkerStopped() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        MsQuic = nullptr;
        return false;
    }

    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
        if (handle.WindowsRelayId == 0) {
            TqCloseSocket(pair[0]);
        }
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    const uint64_t errorsBefore = worker.Snapshot().Errors;
    worker.Stop();
    worker.SetRelayTraceContext(handle.WindowsRelayId, 123, "stopped.example:443");
    const uint64_t errorsAfter = worker.Snapshot().Errors;

    TqCloseSocket(pair[1]);
    MsQuic = nullptr;
    return errorsAfter == errorsBefore;
}

bool TestWindowsRelayTraceContextPostFailureIncrementsErrors() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        MsQuic = nullptr;
        return false;
    }

    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
        if (handle.WindowsRelayId == 0) {
            TqCloseSocket(pair[0]);
        }
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    const uint64_t errorsBefore = worker.Snapshot().Errors;
    worker.TestForceNextTraceContextPostFailureForTest();
    worker.SetRelayTraceContext(handle.WindowsRelayId, 555, "fail.example:443");
    const uint64_t errorsAfter = worker.Snapshot().Errors;

    worker.Stop();
    TqCloseSocket(pair[1]);
    MsQuic = nullptr;
    return errorsAfter == errorsBefore + 1;
}

bool TestWindowsRelayTraceContextDropsWhenClosing() {
    QUIC_API_TABLE fakeApi{};
    fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
    fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        MsQuic = nullptr;
        return false;
    }

    TqWindowsRelayWorker worker;
    if (!worker.Start()) {
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    tuning.RelayIoSize = 4096;
    if (!worker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
        if (handle.WindowsRelayId == 0) {
            TqCloseSocket(pair[0]);
        }
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    if (!worker.TestArmRelayClosingForLateDiscard(handle.WindowsRelayId)) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
        return false;
    }

    const uint64_t staleBefore = worker.Snapshot().WindowsPostedTraceContextStaleDropCount;
    worker.SetRelayTraceContext(handle.WindowsRelayId, 666, "closing.example:443");
    (void)worker.Snapshot();
    const uint64_t staleAfter = worker.Snapshot().WindowsPostedTraceContextStaleDropCount;

    TqTraceLinuxRelayStreamState state{};
    std::string targetStorage;
    const bool ok = worker.TestGetRelayTraceStateForTest(
        handle.WindowsRelayId,
        &state,
        &targetStorage);

    worker.Stop();
    TqCloseSocket(pair[1]);
    MsQuic = nullptr;
    return ok && state.TunnelId == 0 && state.Target == nullptr &&
           staleAfter == staleBefore + 1;
}

std::shared_ptr<TqStreamLifetime> MakeStartedRelayStreamOwner(MsQuicStream* stream) {
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    if (!owner || stream == nullptr || !owner->InstallDetachedStreamForTest(stream)) {
        return nullptr;
    }
    return owner;
}

bool TestWindowsRelayRegistrationPrepareRollbackForTest() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        return false;
    }
    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        worker.Stop();
        return false;
    }
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    TqWindowsRelayRegistration registration{};
    registration.TcpFd = pair[0];
    registration.StreamOwner = owner;
    registration.FailPrepareForTest = true;
    const auto result = worker.RegisterRelayWithId(registration);
    const bool ok = !result.Ok && !result.TcpFdConsumed &&
        worker.Snapshot().ActiveRelays == 0;
    worker.Stop();
    TqCloseSocket(pair[0]);
    TqCloseSocket(pair[1]);
    return ok;
}

bool TestWindowsRelayRegistrationCommitRollbackForTest() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        return false;
    }
    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        worker.Stop();
        return false;
    }
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    TqWindowsRelayRegistration registration{};
    registration.TcpFd = pair[0];
    registration.StreamOwner = owner;
    registration.FailCommitForTest = true;
    const auto result = worker.RegisterRelayWithId(registration);
    const bool ok = !result.Ok && result.TcpFdConsumed && worker.Snapshot().ActiveRelays == 0;
    worker.Stop();
    TqCloseSocket(pair[1]);
    return ok;
}

bool TestWindowsRelayHandleReuseControlIsolationForTest() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    if (!worker.RegisterRelayForTest(stream, &handle, tuning)) {
        worker.Stop();
        return false;
    }
    const auto firstControl = handle.Control;
    const uint64_t firstControlGeneration =
        firstControl != nullptr ? firstControl->Generation.load(std::memory_order_acquire) : 0;
    const uint64_t firstRelayId = handle.WindowsRelayId;
    if (firstControl == nullptr || firstControlGeneration == 0) {
        worker.StopRelay(firstRelayId);
        worker.Stop();
        return false;
    }

    handle.Backend = TqRelayBackendType::None;
    handle.WindowsWorker = nullptr;
    handle.WindowsRelayId = 0;
    handle.WindowsRelayGeneration = 0;
    handle.WindowsWorkerIndex = 0;
    handle.Control.reset();

    if (!worker.RegisterRelayForTest(stream, &handle, tuning)) {
        worker.StopRelay(firstRelayId);
        worker.Stop();
        return false;
    }
    const auto secondControl = handle.Control;
    const bool uniqueControls = secondControl != nullptr && secondControl != firstControl &&
        secondControl->Generation.load(std::memory_order_acquire) != firstControlGeneration;
    if (uniqueControls) {
        firstControl->Stop.store(true, std::memory_order_release);
    }
    const bool ok = uniqueControls &&
        firstControl->Stop.load(std::memory_order_acquire) &&
        !secondControl->Stop.load(std::memory_order_acquire);
    worker.StopRelay(handle.WindowsRelayId);
    worker.Stop();
    return ok;
}

bool TestWindowsRelayManagedRegistrationForTest() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(2));
    auto owner = MakeStartedRelayStreamOwner(stream);
    if (!owner) {
        worker.Stop();
        return false;
    }
    TqWindowsRelayRegistration registration{};
    registration.StreamOwner = owner;
    const auto result = worker.RegisterRelayWithId(registration);
    const bool ok = result.Ok && result.StopControl != nullptr &&
        result.StopControl->Generation.load(std::memory_order_acquire) != 0 &&
        result.RelayGeneration != 0;
    if (result.Ok) {
        worker.StopRelay(result.StopControl, result.RelayId, result.RelayGeneration,
            result.StopControl->Generation.load(std::memory_order_acquire));
    }
    worker.Stop();
    return ok;
}

std::shared_ptr<TqStreamLifetime> g_WindowsPrecommitOwner;
std::array<uint8_t, 32> g_WindowsPrecommitPayload{};
bool g_WindowsPrecommitSnapshotHidden = false;
bool g_WindowsPrecommitPublishTerminal = false;
QUIC_STATUS g_WindowsPrecommitReceiveStatus = QUIC_STATUS_INTERNAL_ERROR;

bool WaitForTerminalPendingClearedForTest(
    TqWindowsRelayWorker& worker,
    uint32_t timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        TqWindowsTerminalOperationSnapshotForTest snapshot{};
        (void)worker.TestGetTerminalOperationSnapshotForTest(&snapshot);
        if (!snapshot.Pending) {
            return true;
        }
        (void)worker.TestDrainSingleTerminalShutdownForTest();
    }
    TqWindowsTerminalOperationSnapshotForTest snapshot{};
    (void)worker.TestGetTerminalOperationSnapshotForTest(&snapshot);
    return !snapshot.Pending;
}

bool WaitForTerminalOperationDrainForTest(
    TqWindowsRelayWorker& worker,
    uint32_t timeoutMs) {
    if (!WaitForTerminalPendingClearedForTest(worker, timeoutMs)) {
        return false;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.Snapshot().ActiveRelays == 0) {
            return true;
        }
        std::this_thread::yield();
    }
    return worker.Snapshot().ActiveRelays == 0;
}

void WindowsAfterManagedPublishHook(TqWindowsRelayWorker* worker, uint64_t relayId) {
    (void)relayId;
    g_WindowsPrecommitSnapshotHidden =
        worker != nullptr && !worker->TestHasCommittedActiveRelayForTest();
    if (g_WindowsPrecommitPublishTerminal && g_WindowsPrecommitOwner != nullptr) {
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        g_WindowsPrecommitReceiveStatus =
            g_WindowsPrecommitOwner->DispatchForTest(&terminal);
        return;
    }
    if (g_WindowsPrecommitOwner == nullptr) {
        return;
    }
    QUIC_BUFFER buffer{};
    buffer.Buffer = g_WindowsPrecommitPayload.data();
    buffer.Length = static_cast<uint32_t>(g_WindowsPrecommitPayload.size());
    QUIC_STREAM_EVENT receive{};
    receive.Type = QUIC_STREAM_EVENT_RECEIVE;
    receive.RECEIVE.BufferCount = 1;
    receive.RECEIVE.Buffers = &buffer;
    g_WindowsPrecommitReceiveStatus = g_WindowsPrecommitOwner->DispatchForTest(&receive);
    if (worker != nullptr &&
        worker->TestLastPrecommitReceiveStatusForTest() == QUIC_STATUS_PENDING) {
        g_WindowsPrecommitReceiveStatus = QUIC_STATUS_PENDING;
    }
}

bool TestWindowsRelayTerminalCallbackSealBeforeWorkerDrainForTest() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        return false;
    }
    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        worker.Stop();
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(11));
    auto owner = MakeStartedRelayStreamOwner(stream);
    if (!owner) {
        worker.Stop();
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        return false;
    }

    TqWindowsRelayRegistration registration{};
    registration.TcpFd = pair[0];
    registration.StreamOwner = owner;
    const auto result = worker.RegisterRelayWithId(registration);
    if (!result.Ok) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        return false;
    }

    const void* expectedCallback = stream->Callback;
    const void* expectedContext = stream->Context;
    std::weak_ptr<TqStreamLifetime> weak = owner;

    TqWindowsRelayWorkerQueueBlockForTest block{};
    if (!worker.TestPostWorkerQueueBlockForTest(&block) ||
        !worker.TestWaitWorkerQueueBlockEnteredForTest(block, 2000)) {
        worker.TestReleaseWorkerQueueBlockForTest(block);
        worker.StopRelay(result.StopControl, result.RelayId, result.RelayGeneration,
            result.StopControl->Generation.load(std::memory_order_acquire));
        worker.Stop();
        TqCloseSocket(pair[1]);
        return false;
    }

    QUIC_STREAM_EVENT shutdown{};
    shutdown.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    shutdown.SHUTDOWN_COMPLETE.ConnectionErrorCode = 42;
    shutdown.SHUTDOWN_COMPLETE.ConnectionCloseStatus = QUIC_STATUS_SUCCESS;
    (void)owner->DispatchForTest(&shutdown);

    const bool phaseOk =
        owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished;
    const bool routeEmpty = owner->TargetContextForTest() == nullptr;
    bool closing = false;
    bool relayHintNull = false;
    const bool sealOk = worker.TestGetBindingTerminalSealForTest(
        result.RelayId, &closing, &relayHintNull) &&
        closing && relayHintNull;
    TqWindowsTerminalOperationSnapshotForTest terminal{};
    const bool terminalPending =
        worker.TestGetTerminalOperationSnapshotForTest(&terminal) &&
        terminal.Pending &&
        terminal.HasBindingOwner &&
        terminal.RelayId == result.RelayId &&
        terminal.Generation == result.RelayGeneration &&
        terminal.ConnectionErrorCode == 42;
    const bool routerStable =
        stream->Callback == expectedCallback && stream->Context == expectedContext;
    const bool retainedWhilePending = weak.use_count() > 1;

    worker.TestReleaseWorkerQueueBlockForTest(block);
    const bool drained = WaitForTerminalPendingClearedForTest(worker, 2000);
    worker.StopRelay(result.StopControl, result.RelayId, result.RelayGeneration,
        result.StopControl->Generation.load(std::memory_order_acquire));
    const bool retired = WaitForTerminalOperationDrainForTest(worker, 2000);

    worker.Stop();
    TqCloseSocket(pair[1]);
    // Direct router dispatch proves state transition only; owner destroy count is
    // asserted separately in TestWindowsRelayTerminalOwnerRetentionForTest.
    return phaseOk && routeEmpty && sealOk && terminalPending && routerStable &&
        retainedWhilePending && drained && retired;
}

bool TestWindowsRelayTerminalOperationStrongOwnerForTest() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(12));
    auto owner = MakeStartedRelayStreamOwner(stream);
    if (!owner) {
        worker.Stop();
        return false;
    }
    TqWindowsRelayRegistration registration{};
    registration.StreamOwner = owner;
    const auto result = worker.RegisterRelayWithId(registration);
    if (!result.Ok) {
        worker.Stop();
        return false;
    }

    TqWindowsRelayWorkerQueueBlockForTest block{};
    if (!worker.TestPostWorkerQueueBlockForTest(&block) ||
        !worker.TestWaitWorkerQueueBlockEnteredForTest(block, 2000)) {
        worker.TestReleaseWorkerQueueBlockForTest(block);
        worker.Stop();
        return false;
    }

    QUIC_STREAM_EVENT shutdown{};
    shutdown.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    shutdown.SHUTDOWN_COMPLETE.ConnectionErrorCode = 7;
    shutdown.SHUTDOWN_COMPLETE.ConnectionCloseStatus = QUIC_STATUS_ABORTED;
    (void)owner->DispatchForTest(&shutdown);

    TqWindowsTerminalOperationSnapshotForTest terminal{};
    const bool ok = worker.TestTerminalCleanupHasStreamPointerForTest() &&
        worker.TestGetTerminalOperationSnapshotForTest(&terminal) &&
        terminal.Pending &&
        terminal.HasBindingOwner &&
        terminal.RelayId == result.RelayId &&
        terminal.Generation == result.RelayGeneration &&
        terminal.ConnectionErrorCode == 7 &&
        terminal.ConnectionCloseStatus == static_cast<uint32_t>(QUIC_STATUS_ABORTED);

    worker.TestReleaseWorkerQueueBlockForTest(block);
    (void)WaitForTerminalOperationDrainForTest(worker, 2000);
    worker.Stop();
    return ok;
}

bool TestWindowsRelayTerminalOwnerRetentionForTest() {
    TqStreamLifetime::ResetTestDetachedOwnerDestroyCountForTest();
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        return false;
    }
    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        worker.Stop();
        return false;
    }

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(13));
    auto owner = MakeStartedRelayStreamOwner(stream);
    if (!owner) {
        worker.Stop();
        TqCloseSocket(pair[0]);
        TqCloseSocket(pair[1]);
        return false;
    }

    TqWindowsRelayRegistration registration{};
    registration.TcpFd = pair[0];
    registration.StreamOwner = owner;
    const auto result = worker.RegisterRelayWithId(registration);
    registration = {};
    if (!result.Ok) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        return false;
    }

    std::weak_ptr<TqStreamLifetime> weak = owner;
    TqWindowsRelayWorkerQueueBlockForTest block{};
    if (!worker.TestPostWorkerQueueBlockForTest(&block) ||
        !worker.TestWaitWorkerQueueBlockEnteredForTest(block, 2000)) {
        worker.TestReleaseWorkerQueueBlockForTest(block);
        worker.StopRelay(result.StopControl, result.RelayId, result.RelayGeneration,
            result.StopControl->Generation.load(std::memory_order_acquire));
        worker.Stop();
        TqCloseSocket(pair[1]);
        return false;
    }

    QUIC_STREAM_EVENT shutdown{};
    shutdown.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    shutdown.SHUTDOWN_COMPLETE.ConnectionErrorCode = 99;
    shutdown.SHUTDOWN_COMPLETE.ConnectionCloseStatus = QUIC_STATUS_SUCCESS;
    (void)owner->DispatchForTest(&shutdown);

    TqWindowsTerminalOperationSnapshotForTest terminal{};
    const bool phaseOk =
        owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished;
    const bool terminalPending =
        worker.TestGetTerminalOperationSnapshotForTest(&terminal) && terminal.Pending;

    owner.reset();
    const bool retainedWhilePending = !weak.expired();

    worker.TestReleaseWorkerQueueBlockForTest(block);
    if (!WaitForTerminalPendingClearedForTest(worker, 2000)) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        return false;
    }
    worker.StopRelay(result.StopControl, result.RelayId, result.RelayGeneration,
        result.StopControl->Generation.load(std::memory_order_acquire));
    (void)WaitForTerminalOperationDrainForTest(worker, 2000);
    worker.Stop();
    TqCloseSocket(pair[1]);
    const bool ownerDestroyed =
        weak.expired() &&
        TqStreamLifetime::TestDetachedOwnerDestroyCountForTest() == 1;
    return phaseOk && terminalPending && retainedWhilePending && ownerDestroyed;
}

bool WindowsSocketDescriptorConsumedForTest(TqSocketHandle socket) {
    if (!TqSocketValid(socket)) {
        return true;
    }
    int type = 0;
    int typeLen = sizeof(type);
    return ::getsockopt(socket, SOL_SOCKET, SO_TYPE,
        reinterpret_cast<char*>(&type), &typeLen) == SOCKET_ERROR;
}

bool TestWindowsRelayPrecommitReceiveHiddenUntilCommitForTest() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        return false;
    }
    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        worker.Stop();
        return false;
    }
    const TqSocketHandle consumedFd = pair[0];
    g_WindowsPrecommitOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    g_WindowsPrecommitReceiveStatus = QUIC_STATUS_INTERNAL_ERROR;
    g_WindowsPrecommitSnapshotHidden = false;
    g_WindowsPrecommitPublishTerminal = false;
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(14));
    (void)g_WindowsPrecommitOwner->InstallDetachedStreamForTest(stream);

    TqWindowsRelayRegistration registration{};
    registration.TcpFd = consumedFd;
    registration.StreamOwner = g_WindowsPrecommitOwner;
    registration.FailCommitForTest = true;
    registration.AfterPublishHookForTest = WindowsAfterManagedPublishHook;
    registration.Tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay =
        g_WindowsPrecommitPayload.size();
    const auto result = worker.RegisterRelayWithId(registration);
    if (result.Ok) {
        g_WindowsPrecommitOwner.reset();
        g_WindowsPrecommitPublishTerminal = false;
        worker.Stop();
        TqCloseSocket(pair[1]);
        return false;
    }
    if (!result.TcpFdConsumed) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        return false;
    }
    if (!g_WindowsPrecommitSnapshotHidden) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        return false;
    }
    if (g_WindowsPrecommitReceiveStatus != QUIC_STATUS_PENDING) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        return false;
    }
    if (!WindowsSocketDescriptorConsumedForTest(consumedFd)) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        return false;
    }
    if (worker.Snapshot().ActiveRelays != 0) {
        worker.Stop();
        TqCloseSocket(pair[1]);
        return false;
    }

    TqSocketHandle reused[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(reused)) {
        g_WindowsPrecommitOwner.reset();
        worker.Stop();
        TqCloseSocket(pair[1]);
        return false;
    }

    g_WindowsPrecommitOwner.reset();
    worker.Stop();
    const bool reusedOk = TqSocketValid(reused[0]) && TqSocketValid(reused[1]);
    TqCloseSocket(pair[1]);
    TqCloseSocket(reused[0]);
    TqCloseSocket(reused[1]);
    g_WindowsPrecommitPublishTerminal = false;
    return reusedOk;
}

bool TestWindowsRelayTerminalAtPublishBoundaryForTest() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        return false;
    }
    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        worker.Stop();
        return false;
    }
    g_WindowsPrecommitOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    g_WindowsPrecommitPublishTerminal = true;
    g_WindowsPrecommitSnapshotHidden = false;
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(15));
    (void)g_WindowsPrecommitOwner->InstallDetachedStreamForTest(stream);

    TqWindowsRelayRegistration registration{};
    registration.TcpFd = pair[0];
    registration.StreamOwner = g_WindowsPrecommitOwner;
    registration.AfterPublishHookForTest = WindowsAfterManagedPublishHook;
    const auto result = worker.RegisterRelayWithId(registration);
    const bool ok = !result.Ok && result.TcpFdConsumed && g_WindowsPrecommitSnapshotHidden &&
        g_WindowsPrecommitOwner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished &&
        worker.Snapshot().ActiveRelays == 0;
    g_WindowsPrecommitOwner.reset();
    g_WindowsPrecommitPublishTerminal = false;
    (void)WaitForTerminalOperationDrainForTest(worker, 2000);
    worker.Stop();
    TqCloseSocket(pair[1]);
    return ok;
}

bool TestWindowsRelayTerminalAtCommitBoundaryForTest() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        return false;
    }
    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        worker.Stop();
        return false;
    }
    g_WindowsPrecommitOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    g_WindowsPrecommitPublishTerminal = true;
    g_WindowsPrecommitSnapshotHidden = false;
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(151));
    (void)g_WindowsPrecommitOwner->InstallDetachedStreamForTest(stream);

    TqWindowsRelayRegistration registration{};
    registration.TcpFd = pair[0];
    registration.StreamOwner = g_WindowsPrecommitOwner;
    registration.FailCommitForTest = true;
    registration.AfterPublishHookForTest = WindowsAfterManagedPublishHook;
    const auto result = worker.RegisterRelayWithId(registration);
    const bool ok = !result.Ok && result.TcpFdConsumed && g_WindowsPrecommitSnapshotHidden &&
        g_WindowsPrecommitOwner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished &&
        WindowsSocketDescriptorConsumedForTest(pair[0]) &&
        worker.Snapshot().ActiveRelays == 0;
    g_WindowsPrecommitOwner.reset();
    g_WindowsPrecommitPublishTerminal = false;
    (void)WaitForTerminalOperationDrainForTest(worker, 2000);
    worker.Stop();
    TqCloseSocket(pair[1]);
    return ok;
}

bool TestWindowsRelayTerminalAtPrepareBoundaryForTest() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        return false;
    }
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    (void)owner->DispatchForTest(&terminal);
    TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
    if (!TqSocketPair(pair)) {
        worker.Stop();
        return false;
    }
    TqWindowsRelayRegistration registration{};
    registration.TcpFd = pair[0];
    registration.StreamOwner = owner;
    const auto result = worker.RegisterRelayWithId(registration);
    const bool ok = !result.Ok && !result.TcpFdConsumed && TqSocketValid(pair[0]) &&
        worker.Snapshot().ActiveRelays == 0;
    owner.reset();
    worker.Stop();
    TqCloseSocket(pair[0]);
    TqCloseSocket(pair[1]);
    return ok;
}

bool TestWindowsRelayHandleDestroyBeforeTerminalDrainForTest() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(16));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    if (!worker.RegisterRelayForTest(stream, &handle, tuning)) {
        worker.Stop();
        return false;
    }
    const auto owner = worker.LastTestStreamOwnerForTest();
    if (!owner) {
        worker.Stop();
        return false;
    }
    const auto control = handle.Control;
    const uint64_t controlGeneration =
        control != nullptr ? control->Generation.load(std::memory_order_acquire) : 0;
    const uint64_t relayGeneration = handle.WindowsRelayGeneration;

    handle.Backend = TqRelayBackendType::None;
    handle.WindowsWorker = nullptr;
    handle.WindowsRelayId = 0;
    handle.WindowsRelayGeneration = 0;
    handle.WindowsWorkerIndex = 0;
    handle.Control.reset();

    TqWindowsRelayWorkerQueueBlockForTest block{};
    if (!worker.TestPostWorkerQueueBlockForTest(&block) ||
        !worker.TestWaitWorkerQueueBlockEnteredForTest(block, 2000)) {
        worker.TestReleaseWorkerQueueBlockForTest(block);
        worker.Stop();
        return false;
    }

    QUIC_STREAM_EVENT shutdown{};
    shutdown.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    (void)owner->DispatchForTest(&shutdown);

    worker.TestReleaseWorkerQueueBlockForTest(block);
    (void)WaitForTerminalPendingClearedForTest(worker, 2000);

    const auto stopDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    bool stopPublished = false;
    while (std::chrono::steady_clock::now() < stopDeadline) {
        if (control != nullptr && control->Stop.load(std::memory_order_acquire)) {
            stopPublished = true;
            break;
        }
        (void)worker.Snapshot();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.Stop();
    return stopPublished && controlGeneration != 0 && relayGeneration != 0;
}

bool TestWindowsRelayStaleGenerationStopTraceIsolationForTest() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(17));

    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    if (!worker.RegisterRelayForTest(stream, &handle, tuning)) {
        worker.Stop();
        return false;
    }
    const auto firstControl = handle.Control;
    const auto firstOwner = worker.LastTestStreamOwnerForTest();
    const uint64_t firstControlGeneration =
        firstControl != nullptr ? firstControl->Generation.load(std::memory_order_acquire) : 0;
    const uint64_t firstRelayId = handle.WindowsRelayId;
    const uint64_t firstRelayGeneration = handle.WindowsRelayGeneration;

    handle.Backend = TqRelayBackendType::None;
    handle.WindowsWorker = nullptr;
    handle.WindowsRelayId = 0;
    handle.WindowsRelayGeneration = 0;
    handle.WindowsWorkerIndex = 0;
    handle.Control.reset();

    alignas(MsQuicStream) unsigned char streamStorage2[sizeof(MsQuicStream)]{};
    auto* stream2 = reinterpret_cast<MsQuicStream*>(streamStorage2);
    stream2->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(170));
    if (!worker.RegisterRelayForTest(stream2, &handle, tuning)) {
        worker.Stop();
        return false;
    }
    const auto secondControl = handle.Control;
    const bool uniqueControls = secondControl != nullptr && secondControl != firstControl;
    const uint64_t secondRelayId = handle.WindowsRelayId;

    if (firstOwner != nullptr) {
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        (void)firstOwner->DispatchForTest(&terminal);
        (void)WaitForTerminalPendingClearedForTest(worker, 2000);
    }
    if (secondControl != nullptr && secondControl->Stop.load(std::memory_order_acquire)) {
        worker.Stop();
        return false;
    }

    worker.StopRelay(firstControl, firstRelayId, firstRelayGeneration, firstControlGeneration);
    if (secondControl->Stop.load(std::memory_order_acquire)) {
        worker.Stop();
        return false;
    }

    worker.SetRelayTraceContext(
        secondControl,
        handle.WindowsRelayId,
        handle.WindowsRelayGeneration,
        secondControl->Generation.load(std::memory_order_acquire),
        999,
        "new.example:443");
    worker.SetRelayTraceContext(
        firstControl,
        firstRelayId,
        firstRelayGeneration,
        firstControlGeneration,
        111,
        "stale.example:443");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    bool secondStopOnly = false;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)worker.Snapshot();
        if (firstControl->Stop.load(std::memory_order_acquire) &&
            !secondControl->Stop.load(std::memory_order_acquire)) {
            secondStopOnly = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const bool secondStillActive = worker.Snapshot().ActiveRelays > 0 && secondRelayId != 0;
    worker.StopRelay(handle.WindowsRelayId);
    worker.Stop();
    return firstOwner != nullptr && uniqueControls && secondStopOnly && secondStillActive;
}

bool TestWindowsRelayWorkerDestroyedLateCallbackForTest() {
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(19));
    std::shared_ptr<TqStreamLifetime> owner;
    std::shared_ptr<TqRelayStopControl> control;
    uint64_t controlGeneration = 0;
    bool stopAfterWorkerStop = false;
    {
        TqWindowsRelayWorker worker;
        if (!StartRelayWorkerForTest(worker)) {
            return false;
        }
        owner = MakeStartedRelayStreamOwner(stream);
        if (!owner) {
            worker.Stop();
            return false;
        }
        TqWindowsRelayRegistration registration{};
        registration.StreamOwner = owner;
        const auto result = worker.RegisterRelayWithId(registration);
        if (!result.Ok) {
            worker.Stop();
            return false;
        }
        control = result.StopControl;
        controlGeneration =
            control != nullptr ? control->Generation.load(std::memory_order_acquire) : 0;
        worker.Stop();
        if (control != nullptr) {
            stopAfterWorkerStop = control->Stop.load(std::memory_order_acquire);
        }
    }

    QUIC_STREAM_EVENT shutdown{};
    shutdown.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    const QUIC_STATUS status = owner->DispatchForTest(&shutdown);
    const bool phaseOk =
        owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished;
    const bool controlUnchanged =
        control == nullptr ||
        control->Stop.load(std::memory_order_acquire) == stopAfterWorkerStop;
    owner.reset();
    return QUIC_SUCCEEDED(status) && phaseOk && controlUnchanged && controlGeneration != 0;
}

bool TestWindowsRelayWorkerDetachDuringCallbackForTest() {
    TqWindowsRelayWorker worker;
    if (!StartRelayWorkerForTest(worker)) {
        return false;
    }
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(18));
    TqRelayHandle handle{};
    TqTuningConfig tuning{};
    if (!worker.RegisterRelayForTest(stream, &handle, tuning)) {
        worker.Stop();
        return false;
    }

    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::mutex gateLock;
    std::condition_variable gateCv;
    TqWindowsRelayWorkerQueueBlockForTest block{};
    worker.TestPostWorkerQueueBlockForTest(&block);
    worker.TestWaitWorkerQueueBlockEnteredForTest(block, 2000);

    std::thread callbackThread([&] {
        QUIC_STREAM_EVENT receive{};
        receive.Type = QUIC_STREAM_EVENT_RECEIVE;
        receive.RECEIVE.BufferCount = 0;
        receive.RECEIVE.Buffers = nullptr;
        {
            std::lock_guard<std::mutex> guard(gateLock);
            entered.store(true, std::memory_order_release);
        }
        gateCv.notify_all();
        while (!release.load(std::memory_order_acquire)) {
            (void)TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &receive);
        }
    });

    {
        std::unique_lock<std::mutex> guard(gateLock);
        gateCv.wait_for(guard, std::chrono::milliseconds(500), [&] {
            return entered.load(std::memory_order_acquire);
        });
    }

    worker.StopRelay(handle.WindowsRelayId);
    release.store(true, std::memory_order_release);
    worker.TestReleaseWorkerQueueBlockForTest(block);
    callbackThread.join();

    const bool routerIntact =
        stream->Callback == TqStreamLifetime::Callback && stream->Context != nullptr;
    worker.Stop();
    return routerIntact;
}

}  // namespace
#endif

int main() {
#if defined(_WIN32)
    TqSocketStartup startup;
    assert(startup.Ok());

    if (!TestWindowsRelayRegistrationPrepareRollbackForTest()) {
        return 200;
    }
    if (!TestWindowsRelayRegistrationCommitRollbackForTest()) {
        return 201;
    }
    if (!TestWindowsRelayHandleReuseControlIsolationForTest()) {
        return 202;
    }
    if (!TestWindowsRelayManagedRegistrationForTest()) {
        return 203;
    }
    if (!TestWindowsRelayTerminalCallbackSealBeforeWorkerDrainForTest()) {
        return 204;
    }
    if (!TestWindowsRelayTerminalOperationStrongOwnerForTest()) {
        return 205;
    }
    if (!TestWindowsRelayTerminalOwnerRetentionForTest()) {
        return 206;
    }
    if (!TestWindowsRelayPrecommitReceiveHiddenUntilCommitForTest()) {
        return 207;
    }
    if (!TestWindowsRelayTerminalAtPublishBoundaryForTest()) {
        return 208;
    }
    if (!TestWindowsRelayTerminalAtCommitBoundaryForTest()) {
        return 213;
    }
    if (!TestWindowsRelayTerminalAtPrepareBoundaryForTest()) {
        return 214;
    }
    if (!TestWindowsRelayHandleDestroyBeforeTerminalDrainForTest()) {
        return 209;
    }
    if (!TestWindowsRelayStaleGenerationStopTraceIsolationForTest()) {
        return 210;
    }
    if (!TestWindowsRelayWorkerDestroyedLateCallbackForTest()) {
        return 212;
    }
    if (!TestWindowsRelayWorkerDetachDuringCallbackForTest()) {
        return 211;
    }

    if (!TestWindowsRelayReceiveViewIocpCallbackQueue()) {
        return 39;
    }

    if (!TestWindowsRelaySendCompleteIocpCallbackQueue()) {
        return 40;
    }

    if (!TestWindowsRelayTcpReadBackpressureWatermarks()) {
        return 41;
    }

    if (!TestWindowsRelaySnapshotObservability()) {
        return 42;
    }

    if (!TestWindowsRelayLockAndCallbackMetricsInitialState()) {
        return 52;
    }

    if (!TestWindowsRelayFinishReceiveViewFrontPathAvoidsLinearSearch()) {
        return 124;
    }

    if (!TestWindowsRelayFinishReceiveViewNotFrontIsCounted()) {
        return 125;
    }

    if (!TestWindowsRelayCallbackReceiveCopyMetricsForTest()) {
        return 126;
    }

    if (!TestWindowsRelayCallbackReceiveBudgetRejectsBeforeCopyForTest()) {
        return 128;
    }

    if (!TestWindowsRelayCallbackReceiveBudgetDoesNotRejectFinForTest()) {
        return 129;
    }

    if (!TestWindowsRelayCallbackReceiveBudgetSkipsStaleGenerationForTest()) {
        return 140;
    }

    if (!TestWindowsRelayCallbackReceiveBudgetSkipsClosingRelayForTest()) {
        return 141;
    }

    if (!TestWindowsRelayMaintenanceQueueBudgetForTest()) {
        return 127;
    }

    if (!TestWindowsRelaySnapshotConcurrentWithRegisterAndStop()) {
        return 53;
    }

    if (!TestWindowsRelayRegisterRunsOnWorkerForTest()) {
        return 55;
    }

    if (!TestWindowsRelayCallbackOperationGenerationMismatchDropsForTest()) {
        return 54;
    }

    if (!TestWindowsRelayCallbackOperationByIdIdealBufferDispatchForTest()) {
        return 68;
    }

    if (!TestWindowsRelaySendCompleteCallbackDoesNotFindRelayForTest()) {
        return 69;
    }

    if (!TestWindowsRelayReceiveCallbackDoesNotFindRelayForTest()) {
        return 76;
    }

    if (!TestWindowsRelayTraceContextUsesWorkerQueue()) {
        return 123;
    }

    if (!TestWindowsRelayTraceContextDropsStaleGeneration()) {
        return 140;
    }

    if (!TestWindowsRelayTraceContextAllowsNullTarget()) {
        return 141;
    }

    if (!TestWindowsRelayTraceContextIgnoresZeroIds()) {
        return 142;
    }

    if (!TestWindowsRelayTraceContextIgnoredWhenRelayMissing()) {
        return 143;
    }

    if (!TestWindowsRelayTraceContextIgnoredWhenWorkerStopped()) {
        return 144;
    }

    if (!TestWindowsRelayTraceContextPostFailureIncrementsErrors()) {
        return 145;
    }

    if (!TestWindowsRelayTraceContextDropsWhenClosing()) {
        return 146;
    }

    {
        const int teardown = TestWindowsRelayQuicTeardownOnWorker();
        if (teardown != 0) {
            return teardown;
        }
    }

    TqWindowsRelayWorker worker;
    assert(worker.Start());
    worker.Stop();
    worker.Stop();
    {
        TqWindowsRelayWorker snapshotWorker;
        assert(snapshotWorker.Start());
        const TqWindowsRelayWorkerSnapshot snapshot = snapshotWorker.Snapshot();
        assert(snapshot.Errors == 0);
        assert(snapshot.DeferredReceiveQueued == 0);
        assert(snapshot.DeferredReceiveCompleteBytes == 0);
        assert(snapshot.PendingQuicReceiveBytes == 0);
        assert(snapshot.WindowsCallbackIocpPostCount == 0);
        assert(snapshot.WindowsCallbackIocpPostFailedCount == 0);
        assert(snapshot.WindowsReceiveReadyPostCount == 0);
        assert(snapshot.WindowsReceiveDrainScheduledCount == 0);
        assert(snapshot.WindowsReceiveDrainCoalescedCount == 0);
        assert(snapshot.WindowsPostedCallbackStaleDropCount == 0);
        assert(snapshot.EventsProcessed == 0);
        snapshotWorker.Stop();
    }
    {
        TqWindowsRelayWorker receiveWorker;

        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;

        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            return 23;
        }
        receiveWorker.SetQuicReceiveViewDrainEnabledForTest(false);
        if (!receiveWorker.Start()) {
            return 7;
        }

        if (stream->Context == nullptr) {
            const TqWindowsRelayWorkerSnapshot failedSnapshot = receiveWorker.Snapshot();
            if (failedSnapshot.Errors != 0) {
                return 25;
            }
            return 22;
        }

        uint8_t first[] = {1, 2, 3};
        uint8_t second[] = {4, 5};
        QUIC_BUFFER buffers[2]{};
        buffers[0].Buffer = first;
        buffers[0].Length = sizeof(first);
        buffers[1].Buffer = second;
        buffers[1].Length = sizeof(second);

        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.BufferCount = 2;
        event.RECEIVE.Buffers = buffers;

        const QUIC_STATUS status =
            TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event);
        if (status != QUIC_STATUS_PENDING) {
            const TqWindowsRelayWorkerSnapshot failedSnapshot = receiveWorker.Snapshot();
            if (failedSnapshot.DeferredReceiveQueued == 1) {
                return 21;
            }
            if (failedSnapshot.Errors != 0) {
                return 20;
            }
            return 2;
        }

        const auto receiveQueuedDeadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        TqWindowsRelayWorkerSnapshot snapshot{};
        do {
            snapshot = receiveWorker.Snapshot();
            if (snapshot.DeferredReceiveQueued == 1 &&
                snapshot.DeferredReceiveBytesQueued == 5 &&
                snapshot.PendingQuicReceiveQueueDepth == 1 &&
                snapshot.PendingQuicReceiveBytes == 5) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < receiveQueuedDeadline);
        if (snapshot.DeferredReceiveQueued != 1) {
            return 3;
        }
        if (snapshot.DeferredReceiveBytesQueued != 5) {
            return 4;
        }
        if (snapshot.PendingQuicReceiveQueueDepth != 1) {
            return 5;
        }
        if (snapshot.PendingQuicReceiveBytes != 5) {
            return 6;
        }

        receiveWorker.SetQuicReceiveViewDrainEnabledForTest(true);
        receiveWorker.StopRelay(handle.WindowsRelayId);
        receiveWorker.Stop();
        const TqWindowsRelayWorkerSnapshot stoppedSnapshot = receiveWorker.Snapshot();
        if (stoppedSnapshot.PendingQuicReceiveQueueDepth != 0) {
            return 26;
        }
        if (stoppedSnapshot.PendingQuicReceiveBytes != 0) {
            return 27;
        }
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
        fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
        fakeApi.StreamSend = FakeStreamSend;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        ResetFakeStreamSends();
        g_StreamReceiveCompleteBytes = 0;
        g_StreamReceiveCompleteCalls = 0;

        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        if (!TqSocketPair(pair)) {
            MsQuic = nullptr;
            return 196;
        }
        if (!TqSetNonBlocking(pair[1])) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 197;
        }

        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 198;
        }

        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 199;
        }

        if (!TqShutdownSend(pair[1])) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 200;
        }
        if (!WaitForFakeStreamSendCount(1, 2000)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 201;
        }

        void* callbackContext = stream->Context;
        CompleteFakeStreamSends(receiveWorker, stream, callbackContext);

        const char expectedResponse[] = "half-close-response";
        char response[] = "half-close-response";
        QUIC_BUFFER buffer{};
        buffer.Buffer = reinterpret_cast<uint8_t*>(response);
        buffer.Length = static_cast<uint32_t>(sizeof(expectedResponse) - 1);
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.BufferCount = 1;
        event.RECEIVE.Buffers = &buffer;
        if (TqWindowsRelayDispatchTestStreamEvent(stream, callbackContext, &event) != QUIC_STATUS_PENDING) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 202;
        }
        std::memset(response, 'x', sizeof(response) - 1);

        std::vector<uint8_t> output;
        if (!WaitForTcpBytes(pair[1], output, sizeof(expectedResponse) - 1, 2000)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 203;
        }
        if (output.size() != sizeof(expectedResponse) - 1 ||
            std::memcmp(output.data(), expectedResponse, sizeof(expectedResponse) - 1) != 0) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 204;
        }

        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
    }
    {
        TqWindowsRelayWorker receiveWorker(3);
        if (!receiveWorker.Start()) {
            return 225;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 226;
        }
        const TqWindowsRelayWorkerSnapshot snapshot = receiveWorker.Snapshot();
        if (snapshot.ActiveRelayStates.size() != 1) {
            receiveWorker.Stop();
            return 227;
        }
        const auto& relay = snapshot.ActiveRelayStates.front();
        if (relay.WorkerIndex != 3 || relay.RelayId != handle.WindowsRelayId ||
            relay.InFlightTcpRecvs != 0 || relay.InFlightTcpSends != 0 ||
            relay.InFlightQuicSends != 0 || relay.TcpReadBytes != 0 ||
            relay.TcpWriteBytes != 0 || relay.Closing || relay.TcpReadClosed ||
            relay.TcpWriteClosed || relay.CloseAfterDrained || relay.StreamDetached) {
            receiveWorker.Stop();
            return 228;
        }
        receiveWorker.Stop();
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
        fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        g_StreamReceiveCompleteBytes = 0;
        g_StreamReceiveCompleteCalls = 0;

        TqWindowsRelayWorker receiveWorker;
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            MsQuic = nullptr;
            return 39;
        }
        receiveWorker.SetQuicReceiveViewDrainEnabledForTest(false);
        if (!receiveWorker.Start()) {
            MsQuic = nullptr;
            return 40;
        }

        uint8_t data[] = {9, 8, 7, 6, 5};
        QUIC_BUFFER buffer{};
        buffer.Buffer = data;
        buffer.Length = sizeof(data);
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.BufferCount = 1;
        event.RECEIVE.Buffers = &buffer;
        if (TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event) != QUIC_STATUS_PENDING) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 41;
        }
        if (!receiveWorker.TestCompleteReceiveViewForCleanup(handle.WindowsRelayId, 2)) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 42;
        }
        const TqWindowsRelayWorkerSnapshot snapshot = receiveWorker.Snapshot();
        if (g_StreamReceiveCompleteBytes != 3 || g_StreamReceiveCompleteCalls != 1 ||
            snapshot.PendingQuicReceiveBytes != 0 || snapshot.PendingQuicReceiveQueueDepth != 0) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 43;
        }
        if (!receiveWorker.TestLateReceiveViewCompletionIgnored(handle.WindowsRelayId, 1, 1)) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 50;
        }
        const TqWindowsRelayWorkerSnapshot lateSnapshot = receiveWorker.Snapshot();
        if (g_StreamReceiveCompleteBytes != 3 || g_StreamReceiveCompleteCalls != 1 ||
            lateSnapshot.Errors != snapshot.Errors) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 51;
        }
        receiveWorker.Stop();
        MsQuic = nullptr;
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
        fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

        TqWindowsRelayWorker receiveWorker;
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            MsQuic = nullptr;
            return 130;
        }
        receiveWorker.SetQuicReceiveViewDrainEnabledForTest(false);
        if (!receiveWorker.Start()) {
            MsQuic = nullptr;
            return 133;
        }

        uint8_t data[] = {'r', 'e', 'a', 'd', 'y'};
        QUIC_BUFFER buffer{};
        buffer.Buffer = data;
        buffer.Length = sizeof(data);
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.BufferCount = 1;
        event.RECEIVE.Buffers = &buffer;

        if (TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event) != QUIC_STATUS_PENDING) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 131;
        }
        if (!receiveWorker.TestLastPostedCallbackWasReceiveReadyForTest(handle.WindowsRelayId)) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 132;
        }
        receiveWorker.Stop();
        MsQuic = nullptr;
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
        fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);

        TqWindowsRelayWorker callbackOrderWorker;
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!callbackOrderWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            MsQuic = nullptr;
            return 139;
        }
        callbackOrderWorker.SetQuicReceiveViewDrainEnabledForTest(false);
        if (!callbackOrderWorker.TestCreateIocpForCallbackPostOnly()) {
            MsQuic = nullptr;
            return 140;
        }

        const uint64_t findRelayBeforeCallbacks =
            callbackOrderWorker.Snapshot().FindRelayByIdCount;

        uint8_t data[] = {'o', 'r', 'd', 'e', 'r'};
        QUIC_BUFFER buffer{};
        buffer.Buffer = data;
        buffer.Length = sizeof(data);
        QUIC_STREAM_EVENT receive{};
        receive.Type = QUIC_STREAM_EVENT_RECEIVE;
        receive.RECEIVE.BufferCount = 1;
        receive.RECEIVE.Buffers = &buffer;
        if (TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &receive) !=
            QUIC_STATUS_PENDING) {
            callbackOrderWorker.Stop();
            MsQuic = nullptr;
            return 141;
        }

        QUIC_STREAM_EVENT ideal{};
        ideal.Type = QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE;
        ideal.IDEAL_SEND_BUFFER_SIZE.ByteCount = 4096;
        if (TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &ideal) !=
            QUIC_STATUS_SUCCESS) {
            callbackOrderWorker.Stop();
            MsQuic = nullptr;
            return 142;
        }

        QUIC_STREAM_EVENT peerReceiveAborted{};
        peerReceiveAborted.Type = QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED;
        peerReceiveAborted.PEER_RECEIVE_ABORTED.ErrorCode = 17;
        if (TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &peerReceiveAborted) !=
            QUIC_STATUS_SUCCESS) {
            callbackOrderWorker.Stop();
            MsQuic = nullptr;
            return 143;
        }

        QUIC_STREAM_EVENT shutdownComplete{};
        shutdownComplete.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        shutdownComplete.SHUTDOWN_COMPLETE.ConnectionErrorCode = 23;
        shutdownComplete.SHUTDOWN_COMPLETE.ConnectionCloseStatus = QUIC_STATUS_SUCCESS;
        if (TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &shutdownComplete) !=
            QUIC_STATUS_SUCCESS) {
            callbackOrderWorker.Stop();
            MsQuic = nullptr;
            return 144;
        }

        const uint64_t findRelayAfterCallbacks =
            callbackOrderWorker.Snapshot().FindRelayByIdCount;
        if (findRelayAfterCallbacks != findRelayBeforeCallbacks) {
            callbackOrderWorker.Stop();
            MsQuic = nullptr;
            return 146;
        }

        if (!callbackOrderWorker.TestPostedCallbackSequenceForTest(
                "RelayReceiveReady,QuicIdealSendBuffer,QuicPeerReceiveAborted,QuicShutdownComplete")) {
            callbackOrderWorker.Stop();
            MsQuic = nullptr;
            return 145;
        }
        if (!callbackOrderWorker.TestDrainPostedCallbackOperationsForTest(4)) {
            callbackOrderWorker.Stop();
            MsQuic = nullptr;
            return 147;
        }
        callbackOrderWorker.Stop();
        MsQuic = nullptr;
    }
    {
        TqWindowsRelayWorker sendCompleteWorker;
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!sendCompleteWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            return 134;
        }
        if (!sendCompleteWorker.Start()) {
            return 135;
        }

        auto* operation =
            sendCompleteWorker.TestCreateQuicSendOperationForTest(handle.WindowsRelayId, 7);
        if (operation == nullptr) {
            sendCompleteWorker.Stop();
            return 136;
        }

        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
        event.SEND_COMPLETE.ClientContext = operation;
        if (TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event) !=
            QUIC_STATUS_SUCCESS) {
            sendCompleteWorker.Stop();
            return 137;
        }
        if (!sendCompleteWorker.TestLastPostedCallbackWasQuicSendCompleteForTest(
                handle.WindowsRelayId)) {
            sendCompleteWorker.Stop();
            return 138;
        }
        sendCompleteWorker.Stop();
    }
    {
        TqWindowsRelayWorker sendCompleteWorker;
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!sendCompleteWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            return 404;
        }
        if (!sendCompleteWorker.TestMarkTcpRecvInFlightForRetirement(handle.WindowsRelayId)) {
            return 405;
        }
        TqResetRelayTraceCallCountsForTest();
        auto* operation =
            sendCompleteWorker.TestCreateQuicSendOperationForTest(handle.WindowsRelayId, 7);
        if (operation == nullptr) {
            return 406;
        }

        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
        event.SEND_COMPLETE.ClientContext = operation;
        if (TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event) !=
            QUIC_STATUS_SUCCESS) {
            return 407;
        }
        const TqWindowsRelayWorkerSnapshot snapshot = sendCompleteWorker.Snapshot();
        if (snapshot.Errors != 1 || snapshot.WindowsCallbackIocpPostFailedCount != 1 ||
            snapshot.FatalRelayResets != 1 || snapshot.ActiveRelayStates.size() != 1 ||
            snapshot.ActiveRelayStates.front().InFlightQuicSends != 0 ||
            snapshot.ActiveRelayStates.front().OutstandingQuicSendBytes != 0) {
            return 408;
        }
        if (TqRelayTraceStopConditionCountForTest() != 0 ||
            TqRelayTraceUnregisterCountForTest() != 0) {
            return 409;
        }
    }
    {
        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        if (!TqSocketPair(pair)) {
            return 44;
        }
        if (!TqSetNonBlocking(pair[1])) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            return 45;
        }

        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            return 46;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            return 47;
        }

        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.BufferCount = 0;
        event.RECEIVE.Buffers = nullptr;
        event.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;
        if (TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event) != QUIC_STATUS_PENDING) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            return 48;
        }

        uint8_t byte = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        bool sawFin = false;
        while (std::chrono::steady_clock::now() < deadline) {
            const int received = TqRecv(pair[1], &byte, sizeof(byte), TqRecvFlags::DontWait);
            if (received == 0) {
                sawFin = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const TqWindowsRelayWorkerSnapshot snapshot = receiveWorker.Snapshot();
        if (!sawFin || snapshot.PendingQuicReceiveBytes != 0 || snapshot.PendingQuicReceiveQueueDepth != 0) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            return 49;
        }
        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
    }
    {
        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            return 55;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 56;
        }

        void* callbackContext = stream->Context;
        std::atomic<bool> callbackThreadStarted{false};
        std::atomic<bool> callbackThreadStop{false};
        std::thread callbackThread([&] {
            QUIC_STREAM_EVENT event{};
            event.Type = QUIC_STREAM_EVENT_RECEIVE;
            event.RECEIVE.BufferCount = 0;
            event.RECEIVE.Buffers = nullptr;
            callbackThreadStarted.store(true, std::memory_order_release);
            while (!callbackThreadStop.load(std::memory_order_acquire)) {
                (void)TqWindowsRelayDispatchTestStreamEvent(stream, callbackContext, &event);
            }
        });

        while (!callbackThreadStarted.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        receiveWorker.StopRelay(handle.WindowsRelayId);
        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (!handle.Stop.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < closeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        callbackThreadStop.store(true, std::memory_order_release);
        callbackThread.join();
        if (!handle.Stop.load(std::memory_order_acquire)) {
            receiveWorker.Stop();
            return 57;
        }

        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.BufferCount = 0;
        event.RECEIVE.Buffers = nullptr;
        if (TqWindowsRelayDispatchTestStreamEvent(stream, callbackContext, &event) != QUIC_STATUS_SUCCESS) {
            receiveWorker.Stop();
            return 58;
        }
        if (stream->Callback != TqStreamLifetime::Callback || stream->Context == nullptr) {
            receiveWorker.Stop();
            return 59;
        }
        receiveWorker.Stop();
    }
    {
        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            return 200;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 201;
        }
        const uint64_t relayId = handle.WindowsRelayId;
        if (!receiveWorker.TestMarkTcpRecvInFlightForRetirement(relayId)) {
            receiveWorker.Stop();
            return 202;
        }
        receiveWorker.StopRelay(relayId);
        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        TqWindowsRelayWorkerSnapshot snapshot{};
        do {
            snapshot = receiveWorker.Snapshot();
            if (snapshot.ActiveRelays == 1 &&
                stream->Callback == TqStreamLifetime::Callback &&
                stream->Context != nullptr) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < closeDeadline);
        if (stream->Callback != TqStreamLifetime::Callback ||
            stream->Context == nullptr ||
            handle.Stop.load(std::memory_order_acquire)) {
            receiveWorker.Stop();
            return 203;
        }
        if (!receiveWorker.TestCompleteTcpRecvInFlightForRetirement(relayId)) {
            receiveWorker.Stop();
            return 204;
        }
        const auto retireDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (!handle.Stop.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < retireDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        snapshot = receiveWorker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) || snapshot.ActiveRelays != 0) {
            receiveWorker.Stop();
            return 205;
        }
        receiveWorker.Stop();
    }
    {
        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            return 206;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 207;
        }
        const uint64_t relayId = handle.WindowsRelayId;
        if (!receiveWorker.TestMarkQuicSendInFlightForRetirement(relayId)) {
            receiveWorker.Stop();
            return 208;
        }
        receiveWorker.StopRelay(relayId);
        const auto retireDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (!handle.Stop.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < retireDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const TqWindowsRelayWorkerSnapshot snapshot = receiveWorker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) || snapshot.ActiveRelays != 0) {
            receiveWorker.Stop();
            return 209;
        }
        receiveWorker.Stop();
    }
    {
        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            return 210;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 211;
        }
        const uint64_t relayId = handle.WindowsRelayId;
        const TqWindowsRelayWorkerSnapshot before = receiveWorker.Snapshot();
        if (!receiveWorker.TestMarkTcpRecvInFlightForRetirement(relayId)) {
            receiveWorker.Stop();
            return 215;
        }
        receiveWorker.StopRelay(relayId);
        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (stream->Context != nullptr &&
               std::chrono::steady_clock::now() < closeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (stream->Context != nullptr) {
            receiveWorker.Stop();
            return 212;
        }
        if (!receiveWorker.TestBufferedTcpSendZeroCompletion(relayId)) {
            receiveWorker.Stop();
            return 213;
        }
        const TqWindowsRelayWorkerSnapshot after = receiveWorker.Snapshot();
        if (after.FatalRelayResets != before.FatalRelayResets ||
            after.GracefulRelayDrains <= before.GracefulRelayDrains) {
            receiveWorker.Stop();
            return 214;
        }
        (void)receiveWorker.TestCompleteTcpRecvInFlightForRetirement(relayId);
        receiveWorker.Stop();
    }
    {
        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            return 221;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 222;
        }
        const uint64_t relayId = handle.WindowsRelayId;
        if (!receiveWorker.TestCloseRelayAfterTcpHalfCloseDrain(relayId)) {
            receiveWorker.Stop();
            return 223;
        }
        const auto retireDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (!handle.Stop.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < retireDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!handle.Stop.load(std::memory_order_acquire)) {
            receiveWorker.Stop();
            return 224;
        }
        receiveWorker.Stop();
    }
    {
        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            return 216;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            return 217;
        }
        const uint64_t relayId = handle.WindowsRelayId;
        void* callbackContext = stream->Context;
        const TqWindowsRelayWorkerSnapshot before = receiveWorker.Snapshot();
        if (!receiveWorker.TestArmRelayClosingForLateDiscard(relayId)) {
            receiveWorker.Stop();
            return 218;
        }
        uint8_t data[] = {'l', 'a', 't', 'e'};
        QUIC_BUFFER buffer{};
        buffer.Buffer = data;
        buffer.Length = sizeof(data);
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.BufferCount = 1;
        event.RECEIVE.Buffers = &buffer;
        event.RECEIVE.Flags = static_cast<QUIC_RECEIVE_FLAGS>(0);
        if (TqWindowsRelayDispatchTestStreamEvent(stream, callbackContext, &event) !=
            QUIC_STATUS_SUCCESS) {
            receiveWorker.Stop();
            return 219;
        }
        const TqWindowsRelayWorkerSnapshot after = receiveWorker.Snapshot();
        if (after.DeferredReceiveQueued != before.DeferredReceiveQueued ||
            after.Errors != before.Errors ||
            after.FatalRelayResets != before.FatalRelayResets) {
            receiveWorker.Stop();
            return 220;
        }
        receiveWorker.Stop();
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
        fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        g_StreamReceiveCompleteBytes = 0;
        g_StreamReceiveCompleteCalls = 0;

        TqWindowsRelayWorker receiveWorker;
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        tuning.WindowsRelayQuicReceiveCompleteBatchBytes = 4;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            MsQuic = nullptr;
            return 70;
        }
        receiveWorker.SetQuicReceiveViewDrainEnabledForTest(false);
        if (!receiveWorker.Start()) {
            MsQuic = nullptr;
            return 71;
        }

        uint8_t firstData[] = {'a', 'b'};
        QUIC_BUFFER firstBuffer{};
        firstBuffer.Buffer = firstData;
        firstBuffer.Length = sizeof(firstData);
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.BufferCount = 1;
        event.RECEIVE.Buffers = &firstBuffer;
        if (TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event) != QUIC_STATUS_PENDING) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 72;
        }
        uint8_t secondData[] = {'c', 'd', 'e'};
        QUIC_BUFFER secondBuffer{};
        secondBuffer.Buffer = secondData;
        secondBuffer.Length = sizeof(secondData);
        event.RECEIVE.Buffers = &secondBuffer;
        if (TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event) != QUIC_STATUS_PENDING) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 403;
        }
        if (!receiveWorker.TestAdvanceReceiveViewForCompletion(handle.WindowsRelayId, 2)) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 73;
        }
        if (!receiveWorker.TestNoWorkerEventQueueReceiveViewForTest()) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 400;
        }
        TqWindowsRelayWorkerSnapshot snapshot = receiveWorker.Snapshot();
        if (g_StreamReceiveCompleteBytes != 0 || g_StreamReceiveCompleteCalls != 0 ||
            snapshot.PendingQuicReceiveBytes != 3 || snapshot.PendingQuicReceiveQueueDepth != 1) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 74;
        }
        if (!receiveWorker.TestAdvanceReceiveViewForCompletion(handle.WindowsRelayId, 3)) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 75;
        }
        if (!receiveWorker.TestNoWorkerEventQueueReceiveViewForTest()) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 401;
        }
        snapshot = receiveWorker.Snapshot();
        if (g_StreamReceiveCompleteBytes != 5 || g_StreamReceiveCompleteCalls != 1 ||
            snapshot.PendingQuicReceiveBytes != 0 || snapshot.PendingQuicReceiveQueueDepth != 0) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 402;
        }
        receiveWorker.Stop();
        MsQuic = nullptr;
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
        fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        g_StreamReceiveCompleteBytes = 0;
        g_StreamReceiveCompleteCalls = 0;

        TqWindowsRelayWorker receiveWorker;
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        tuning.WindowsRelayQuicReceiveCompleteBatchBytes = 4;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            MsQuic = nullptr;
            return 77;
        }
        receiveWorker.SetQuicReceiveViewDrainEnabledForTest(false);
        if (!receiveWorker.Start()) {
            MsQuic = nullptr;
            return 78;
        }

        uint8_t data[] = {'x', 'y', 'z', 'w', 'q'};
        QUIC_BUFFER buffer{};
        buffer.Buffer = data;
        buffer.Length = sizeof(data);
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.BufferCount = 1;
        event.RECEIVE.Buffers = &buffer;
        if (TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event) != QUIC_STATUS_PENDING) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 79;
        }
        if (!receiveWorker.TestAdvanceReceiveViewForCompletion(handle.WindowsRelayId, 2)) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 80;
        }
        if (g_StreamReceiveCompleteBytes != 0 || g_StreamReceiveCompleteCalls != 0) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 81;
        }
        receiveWorker.StopRelay(handle.WindowsRelayId);
        const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (!handle.Stop.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < closeDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const TqWindowsRelayWorkerSnapshot snapshot = receiveWorker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) || g_StreamReceiveCompleteBytes != 5 ||
            g_StreamReceiveCompleteCalls != 2 || snapshot.PendingQuicReceiveBytes != 0 ||
            snapshot.PendingQuicReceiveQueueDepth != 0) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 82;
        }
        receiveWorker.Stop();
        MsQuic = nullptr;
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
        fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        g_StreamReceiveSetEnabledCalls = 0;
        g_LastStreamReceiveEnabled = TRUE;

        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            MsQuic = nullptr;
            return 60;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        tuning.WindowsRelayMaxPendingQuicReceiveBytesPerRelay = 4;
        if (!receiveWorker.RegisterRelayForTest(stream, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 61;
        }

        uint8_t data[] = {1, 2, 3, 4, 5};
        QUIC_BUFFER buffer{};
        buffer.Buffer = data;
        buffer.Length = sizeof(data);
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.BufferCount = 1;
        event.RECEIVE.Buffers = &buffer;

        const QUIC_STATUS status = TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event);
        if (status != QUIC_STATUS_PENDING) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 62;
        }
        const auto enqueueDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        TqWindowsRelayWorkerSnapshot snapshot{};
        while (std::chrono::steady_clock::now() < enqueueDeadline) {
            snapshot = receiveWorker.Snapshot();
            if (snapshot.PendingQuicReceiveBytes == sizeof(data) &&
                snapshot.PendingQuicReceiveQueueDepth == 1) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (snapshot.PendingQuicReceiveBytes != sizeof(data) ||
            snapshot.PendingQuicReceiveQueueDepth != 1) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 63;
        }
        if (snapshot.QuicReceivePausedCount != 1 ||
            snapshot.CallbackReceiveBudgetRejectedCount != 0 ||
            snapshot.CallbackReceiveBudgetPausedCount != 0) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 64;
        }
        if (snapshot.Errors != 0) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 66;
        }
        if (g_StreamReceiveSetEnabledCalls != 1 || g_LastStreamReceiveEnabled != FALSE) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 67;
        }
        if (handle.Stop.load(std::memory_order_acquire)) {
            receiveWorker.Stop();
            MsQuic = nullptr;
            return 65;
        }
        receiveWorker.Stop();
        MsQuic = nullptr;
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
        fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        g_StreamReceiveCompleteBytes = 0;
        g_StreamReceiveCompleteCalls = 0;

        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        if (!TqSocketPair(pair)) {
            MsQuic = nullptr;
            return 174;
        }
        if (!TqSetNonBlocking(pair[1])) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 175;
        }

        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 176;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 190;
        }

        uint8_t zeroByte = 0;
        QUIC_BUFFER buffer{};
        buffer.Buffer = &zeroByte;
        buffer.Length = 0;
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.BufferCount = 1;
        event.RECEIVE.Buffers = &buffer;
        event.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;
        if (TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event) != QUIC_STATUS_PENDING) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 191;
        }

        const auto finDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        TqWindowsRelayWorkerSnapshot snapshot{};
        do {
            snapshot = receiveWorker.Snapshot();
            if (snapshot.GracefulRelayDrains >= 1) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < finDeadline);
        if (snapshot.GracefulRelayDrains < 1 || snapshot.FatalRelayResets != 0) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 191;
        }

        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
        fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        g_StreamReceiveCompleteBytes = 0;
        g_StreamReceiveCompleteCalls = 0;

        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        if (!TqSocketPair(pair)) {
            MsQuic = nullptr;
            return 177;
        }
        if (!TqSetNonBlocking(pair[1])) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 178;
        }

        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 179;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 192;
        }

        const char payload[] = "fin-drain";
        QUIC_BUFFER buffer{};
        buffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
        buffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.BufferCount = 1;
        event.RECEIVE.Buffers = &buffer;
        event.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;
        if (TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event) != QUIC_STATUS_PENDING) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 193;
        }

        std::vector<uint8_t> output;
        if (!WaitForTcpBytes(pair[1], output, sizeof(payload) - 1, 2000)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 193;
        }

        QUIC_STREAM_EVENT shutdown{};
        shutdown.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        shutdown.SHUTDOWN_COMPLETE.ConnectionShutdown = FALSE;
        (void)TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &shutdown);

        const auto shutdownDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        TqWindowsRelayWorkerSnapshot snapshot{};
        do {
            snapshot = receiveWorker.Snapshot();
            if (snapshot.GracefulRelayDrains >= 1 && snapshot.FatalRelayResets == 0 &&
                snapshot.TcpHardErrors == 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < shutdownDeadline);
        if (snapshot.GracefulRelayDrains < 1 || snapshot.FatalRelayResets != 0 ||
            snapshot.TcpHardErrors != 0) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 193;
        }

        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamSend = FakeStreamSend;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        g_FakeStreamSendStatus = QUIC_STATUS_SUCCESS;
        ResetFakeStreamSends();

        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        if (!TqSocketPair(pair)) {
            MsQuic = nullptr;
            return 164;
        }

        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 165;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 4096;
        if (!receiveWorker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 166;
        }

        const char first[] = "backpressure-test";
        if (TqSend(pair[1], first, std::strlen(first), TqSendFlags::None) != static_cast<int>(std::strlen(first))) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 167;
        }
        if (!WaitForFakeStreamSendCount(1, 2000)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 168;
        }
        void* callbackContext = stream->Context;
        CompleteFakeStreamSends(receiveWorker, stream, callbackContext);

        g_FakeStreamSendStatus = QUIC_STATUS_OUT_OF_MEMORY;
        const char second[] = "backpressure-more";
        if (TqSend(pair[1], second, std::strlen(second), TqSendFlags::None) != static_cast<int>(std::strlen(second))) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 169;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        TqWindowsRelayWorkerSnapshot snapshot{};
        do {
            snapshot = receiveWorker.Snapshot();
            if (snapshot.QuicSendBackpressureEvents > 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);

        if (snapshot.QuicSendBackpressureEvents == 0 ||
            handle.Stop.load(std::memory_order_acquire) ||
            snapshot.FatalRelayResets != 0) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            g_FakeStreamSendStatus = QUIC_STATUS_SUCCESS;
            return 170;
        }

        g_FakeStreamSendStatus = QUIC_STATUS_SUCCESS;
        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamSend = FakeStreamSend;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        g_FakeStreamSendStatus = QUIC_STATUS_INTERNAL_ERROR;
        ResetFakeStreamSends();

        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        if (!TqSocketPair(pair)) {
            MsQuic = nullptr;
            g_FakeStreamSendStatus = QUIC_STATUS_SUCCESS;
            return 173;
        }

        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            g_FakeStreamSendStatus = QUIC_STATUS_SUCCESS;
            return 173;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 4096;
        if (!receiveWorker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            g_FakeStreamSendStatus = QUIC_STATUS_SUCCESS;
            return 172;
        }

        const char payload[] = "fatal-send-test";
        if (TqSend(pair[1], payload, std::strlen(payload), TqSendFlags::None) != static_cast<int>(std::strlen(payload))) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            g_FakeStreamSendStatus = QUIC_STATUS_SUCCESS;
            return 173;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        TqWindowsRelayWorkerSnapshot snapshot{};
        do {
            snapshot = receiveWorker.Snapshot();
            if (snapshot.QuicSendFatalErrors > 0 &&
                handle.Stop.load(std::memory_order_acquire) &&
                snapshot.FatalRelayResets > 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);

        if (snapshot.QuicSendFatalErrors == 0 ||
            !handle.Stop.load(std::memory_order_acquire) ||
            snapshot.FatalRelayResets == 0) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            g_FakeStreamSendStatus = QUIC_STATUS_SUCCESS;
            return 171;
        }

        g_FakeStreamSendStatus = QUIC_STATUS_SUCCESS;
        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamSend = FakeStreamSend;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        ResetFakeStreamSends();

        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        if (!TqSocketPair(pair)) {
            MsQuic = nullptr;
            return 83;
        }

        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 84;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 4096;
        if (!receiveWorker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 85;
        }
        void* callbackContext = stream->Context;

        const char first[] = "tcp-recv-pool-first";
        if (TqSend(pair[1], first, std::strlen(first), TqSendFlags::None) != static_cast<int>(std::strlen(first))) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 86;
        }
        if (!WaitForFakeStreamSendCount(1, 2000)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 87;
        }
        TqWindowsRelayWorkerSnapshot snapshot = receiveWorker.Snapshot();
        if (snapshot.RelayBufferBytesInUse == 0) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 88;
        }
        CompleteFakeStreamSends(receiveWorker, stream, callbackContext);

        const char second[] = "tcp-recv-pool-second";
        if (TqSend(pair[1], second, std::strlen(second), TqSendFlags::None) != static_cast<int>(std::strlen(second))) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 89;
        }
        if (!WaitForFakeStreamSendCount(2, 2000)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 90;
        }
        CompleteFakeStreamSends(receiveWorker, stream, callbackContext);

        snapshot = receiveWorker.Snapshot();
        if (snapshot.TcpRecvOperationsCreated != 1 || snapshot.TcpRecvOperationsReused < 1 ||
            snapshot.RelayBufferAllocateCount < 2) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 91;
        }

        TqCloseSocket(pair[1]);
        pair[1] = TqInvalidSocket;
        receiveWorker.Stop();
        snapshot = receiveWorker.Snapshot();
        if (snapshot.RelayBufferBytesInUse != 0) {
            MsQuic = nullptr;
            return 93;
        }
        if (snapshot.PendingQuicReceiveBytes != 0 || snapshot.PendingQuicReceiveQueueDepth != 0) {
            MsQuic = nullptr;
            return 92;
        }

        MsQuic = nullptr;
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamSend = FakeStreamSend;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        ResetFakeStreamSends();

        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        if (!TqSocketPair(pair)) {
            MsQuic = nullptr;
            return 94;
        }

        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 95;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 4096;
        if (!receiveWorker.RegisterRelay(pair[0], stream, nullptr, nullptr, &handle, tuning, TqCompressAlgo::None)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 96;
        }
        void* callbackContext = stream->Context;

        const char payload[] = "post-recv-failure";
        if (TqSend(pair[1], payload, std::strlen(payload), TqSendFlags::None) != static_cast<int>(std::strlen(payload))) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 97;
        }
        if (!WaitForFakeStreamSendCount(1, 2000)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 98;
        }
        if (!receiveWorker.TestCloseRelayTcpSocketForPostRecvFailure(handle.WindowsRelayId)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 99;
        }
        receiveWorker.StopRelay(handle.WindowsRelayId);
        CompleteFakeStreamSends(receiveWorker, stream, callbackContext);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (!handle.Stop.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const TqWindowsRelayWorkerSnapshot snapshot = receiveWorker.Snapshot();
        if (!handle.Stop.load(std::memory_order_acquire) || snapshot.RelayBufferBytesInUse != 0) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 100;
        }

        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamSend = FakeStreamSend;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        g_FakeStreamSendStatus = QUIC_STATUS_SUCCESS;
        ResetFakeStreamSends();

        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        if (!TqSocketPair(pair)) {
            MsQuic = nullptr;
            return 190;
        }

        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 191;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        FlushOnlyCompressor compressor;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 4096;
        if (!receiveWorker.RegisterRelay(pair[0], stream, &compressor, nullptr, &handle, tuning, TqCompressAlgo::Zstd)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 192;
        }

        const char payload[] = "z";
        if (TqSend(pair[1], payload, sizeof(payload) - 1, TqSendFlags::None) !=
            static_cast<int>(sizeof(payload) - 1)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 193;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        do {
            if (compressor.FlushCalls.load(std::memory_order_acquire) != 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);

        if (compressor.CompressCalls.load(std::memory_order_acquire) < 1 ||
            compressor.InputBytes.load(std::memory_order_acquire) < sizeof(payload) - 1 ||
            compressor.FlushCalls.load(std::memory_order_acquire) < 1) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 194;
        }
        if (!WaitForFakeStreamSendCount(1, 2000)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 195;
        }
        {
            std::lock_guard<std::mutex> guard(g_StreamSendMutex);
            if (g_StreamSendPayloads.size() != 1 || g_StreamSendPayloads[0].size() != 1 ||
                g_StreamSendPayloads[0][0] != 0x5a) {
                receiveWorker.Stop();
                TqCloseSocket(pair[1]);
                MsQuic = nullptr;
                return 196;
            }
        }
        if (receiveWorker.Snapshot().Errors != 0) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 197;
        }

        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamSend = FakeStreamSend;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        ResetFakeStreamSends();

        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        if (!TqSocketPair(pair)) {
            MsQuic = nullptr;
            return 101;
        }

        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 102;
        }
        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));
        EmptyOutputCompressor compressor;
        compressor.Worker = &receiveWorker;
        compressor.CloseTcpBeforeReturning = true;
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 4096;
        if (!receiveWorker.RegisterRelay(pair[0], stream, &compressor, nullptr, &handle, tuning, TqCompressAlgo::Zstd)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 103;
        }
        compressor.RelayId = handle.WindowsRelayId;

        const char payload[] = "empty-compress-post-recv-failure";
        if (TqSend(pair[1], payload, std::strlen(payload), TqSendFlags::None) != static_cast<int>(std::strlen(payload))) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 104;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        TqWindowsRelayWorkerSnapshot snapshot{};
        do {
            snapshot = receiveWorker.Snapshot();
            if (handle.Stop.load(std::memory_order_acquire) && snapshot.GracefulRelayDrains > 0 &&
                snapshot.TcpHardErrors == 0 && snapshot.FatalRelayResets == 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (snapshot.GracefulRelayDrains == 0 || snapshot.TcpHardErrors != 0 ||
            snapshot.FatalRelayResets != 0 ||
            !handle.Stop.load(std::memory_order_acquire)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 181;
        }
        if (snapshot.RelayBufferBytesInUse != 0) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 105;
        }

        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
        fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        g_StreamReceiveCompleteBytes = 0;
        g_StreamReceiveCompleteCalls = 0;

        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        if (!TqSocketPair(pair)) {
            MsQuic = nullptr;
            return 106;
        }

        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 107;
        }

        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        TestDecompressor decompressor;

        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 64 * 1024;
        if (!receiveWorker.RegisterRelay(
                pair[0], stream, nullptr, &decompressor, &handle, tuning, TqCompressAlgo::Zstd)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 109;
        }
        receiveWorker.SetQuicReceiveViewDrainEnabledForTest(false);

        std::vector<uint8_t> compressed{'z', 's', 't', 'd', '-', 'p', 'e', 'n', 'd', 'i', 'n', 'g'};

        QUIC_BUFFER buffer{};
        buffer.Buffer = compressed.data();
        buffer.Length = static_cast<uint32_t>(compressed.size());
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.BufferCount = 1;
        event.RECEIVE.Buffers = &buffer;

        const QUIC_STATUS status = TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event);
        if (status != QUIC_STATUS_PENDING) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 111;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        TqWindowsRelayWorkerSnapshot snapshot{};
        do {
            snapshot = receiveWorker.Snapshot();
            if (snapshot.DeferredReceiveQueued == 1 &&
                snapshot.DeferredReceiveBytesQueued == compressed.size() &&
                snapshot.PendingQuicReceiveQueueDepth == 1 &&
                snapshot.PendingQuicReceiveBytes == compressed.size()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);

        if (snapshot.DeferredReceiveQueued != 1 ||
            snapshot.DeferredReceiveBytesQueued != compressed.size() ||
            snapshot.PendingQuicReceiveQueueDepth != 1 ||
            snapshot.PendingQuicReceiveBytes != compressed.size()) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 112;
        }

        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
    }
    {
        QUIC_API_TABLE fakeApi{};
        fakeApi.StreamReceiveComplete = FakeStreamReceiveComplete;
        fakeApi.StreamReceiveSetEnabled = FakeStreamReceiveSetEnabled;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
        g_StreamReceiveCompleteBytes = 0;
        g_StreamReceiveCompleteCalls = 0;

        TqSocketHandle pair[2]{TqInvalidSocket, TqInvalidSocket};
        if (!TqSocketPair(pair)) {
            MsQuic = nullptr;
            return 113;
        }
        if (!TqSetNonBlocking(pair[1])) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 114;
        }

        TqWindowsRelayWorker receiveWorker;
        if (!receiveWorker.Start()) {
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 115;
        }

        alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
        auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
        stream->Callback = MsQuicStream::NoOpCallback;
        stream->Context = nullptr;
        stream->Handle = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(1));

        auto compressor = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
        auto decompressor = TqCreateDecompressor(TqCompressAlgo::Zstd);
        if (!compressor || !decompressor) {
            receiveWorker.Stop();
            TqCloseSocket(pair[0]);
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 116;
        }
        TqRelayHandle handle{};
        TqTuningConfig tuning{};
        tuning.RelayIoSize = 4096;
        if (!receiveWorker.RegisterRelay(
                pair[0], stream, nullptr, decompressor.get(), &handle, tuning, TqCompressAlgo::Zstd)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 117;
        }

        std::vector<uint8_t> plain(1024 * 1024, 0x41);
        std::vector<uint8_t> compressed;
        if (!compressor->Compress(plain.data(), plain.size(), compressed, true) ||
            compressed.empty()) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 118;
        }
        QUIC_BUFFER buffer{};
        buffer.Buffer = compressed.data();
        buffer.Length = static_cast<uint32_t>(compressed.size());
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.BufferCount = 1;
        event.RECEIVE.Buffers = &buffer;

        if (TqWindowsRelayDispatchTestStreamEvent(stream, stream->Context, &event) != QUIC_STATUS_PENDING) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 119;
        }

        std::vector<uint8_t> output;
        if (!WaitForTcpBytes(pair[1], output, plain.size(), 5000)) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 120;
        }
        if (output != plain) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 121;
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        TqWindowsRelayWorkerSnapshot snapshot{};
        do {
            snapshot = receiveWorker.Snapshot();
            if (snapshot.PendingQuicReceiveBytes == 0 && snapshot.PendingQuicReceiveQueueDepth == 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);

        if (snapshot.ZstdDecompressInputBytes != compressed.size() ||
            snapshot.ZstdDecompressOutputBytes < plain.size() ||
            snapshot.ZstdDecompressCalls == 0 ||
            snapshot.ZstdDecompressFailures != 0 ||
            snapshot.DeferredReceiveCompleteBytes != compressed.size() ||
            snapshot.PendingQuicReceiveBytes != 0 ||
            snapshot.PendingQuicReceiveQueueDepth != 0 ||
            g_StreamReceiveCompleteBytes != compressed.size() ||
            g_StreamReceiveCompleteCalls == 0) {
            receiveWorker.Stop();
            TqCloseSocket(pair[1]);
            MsQuic = nullptr;
            return 122;
        }

        receiveWorker.Stop();
        TqCloseSocket(pair[1]);
        MsQuic = nullptr;
    }
#endif
    return 0;
}
