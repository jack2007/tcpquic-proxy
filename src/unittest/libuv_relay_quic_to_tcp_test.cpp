#include "libuv_relay_worker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

void Check(bool condition) {
    if (!condition) {
        std::abort();
    }
}

std::atomic<std::uint64_t> gCompleteCalls{0};
std::atomic<std::uint64_t> gCompleteBytes{0};
std::vector<bool> gReceiveEnabled;
uv_write_t* gWriteRequest{nullptr};
uv_write_cb gWriteCallback{nullptr};
std::vector<std::vector<std::uint8_t>> gWrittenBuffers;
std::mutex gIoMutex;
std::condition_variable gIoCondition;
std::vector<std::uint64_t> gCompletionLengths;
std::vector<std::uint8_t> gWrittenIds;
std::vector<std::uint8_t> gAllWrittenBytes;
std::atomic<TqUvRelayState*> gTerminalOnAdmission{nullptr};
std::atomic<bool> gFailAsyncSend{false};
std::atomic<bool> gFailDeferredPost{false};
std::atomic<int> gTerminalTrigger{-1};
std::atomic<std::size_t> gQueueCapacity{4096};

void CompleteWrite(int status);

int MaybeFailAsyncSend(uv_async_t* async) {
    return gFailAsyncSend.load() ? UV_EIO : uv_async_send(async);
}

bool FailDeferredPost() {
    return gFailDeferredPost.exchange(false, std::memory_order_acq_rel);
}

void CaptureTerminalTrigger(TqUvRelayState&, TqUvTerminalTrigger trigger) {
    gTerminalTrigger.store(static_cast<int>(trigger), std::memory_order_release);
}

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::uint32_t timeoutMs = 5000) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    }
    return predicate();
}

template <typename Function>
void RunOnLoop(TqUvRelayWorker& worker, Function function) {
    std::promise<void> completed;
    auto ready = completed.get_future();
    Check(worker.Post([&](TqUvRelayWorker& local) {
        function(local);
        completed.set_value();
    }));
    Check(ready.wait_for(std::chrono::seconds(5)) ==
        std::future_status::ready);
}

void TerminalOnAdmission() {
    auto* relay = gTerminalOnAdmission.exchange(nullptr);
    if (relay != nullptr && relay->Binding != nullptr) {
        relay->Binding->Activation.store(
            TqUvActivation::Terminal, std::memory_order_release);
    }
}

void QUIC_API FakeReceiveComplete(HQUIC, std::uint64_t bytes) {
    ++gCompleteCalls;
    gCompleteBytes += bytes;
    std::lock_guard<std::mutex> guard(gIoMutex);
    gCompletionLengths.push_back(bytes);
    gIoCondition.notify_all();
}

QUIC_STATUS QUIC_API FakeReceiveSetEnabled(HQUIC, BOOLEAN enabled) {
    gReceiveEnabled.push_back(enabled != FALSE);
    return QUIC_STATUS_SUCCESS;
}

QUIC_STATUS QUIC_API FakeStreamShutdown(
    HQUIC, QUIC_STREAM_SHUTDOWN_FLAGS, QUIC_UINT62) {
    return QUIC_STATUS_SUCCESS;
}

int FakeWrite(
    uv_write_t* request,
    uv_stream_t*,
    const uv_buf_t buffers[],
    unsigned count,
    uv_write_cb callback) {
    std::lock_guard<std::mutex> guard(gIoMutex);
    Check(gWriteRequest == nullptr);
    gWriteRequest = request;
    gWriteCallback = callback;
    gWrittenBuffers.clear();
    for (unsigned index = 0; index < count; ++index) {
        const auto* begin = reinterpret_cast<const std::uint8_t*>(buffers[index].base);
        gWrittenBuffers.emplace_back(begin, begin + buffers[index].len);
        gAllWrittenBytes.insert(
            gAllWrittenBytes.end(), begin, begin + buffers[index].len);
    }
    if (count != 0 && buffers[0].len != 0) {
        gWrittenIds.push_back(
            static_cast<std::uint8_t>(buffers[0].base[0]));
    }
    gIoCondition.notify_all();
    return 0;
}

TqUvRelayWorkerConfig Config() {
    static TqUvCallAdapter calls;
    calls = TqUvProductionCalls();
    calls.Write = &FakeWrite;
    calls.AsyncSend = &MaybeFailAsyncSend;
    TqUvRelayWorkerConfig config{};
    config.Calls = &calls;
    config.AfterAdmissionCheckForTest = &TerminalOnAdmission;
    config.FailDeferredPostForTest = &FailDeferredPost;
    config.QueueCapacity = gQueueCapacity.load();
    return config;
}

struct RelayFixture {
    RelayFixture(TqCompressAlgo algo = TqCompressAlgo::None) : Worker(Config()) {
        static QUIC_API_TABLE api{};
        api.StreamReceiveComplete = &FakeReceiveComplete;
        api.StreamReceiveSetEnabled = &FakeReceiveSetEnabled;
        api.StreamShutdown = &FakeStreamShutdown;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&api);
        Stream = reinterpret_cast<MsQuicStream*>(StreamStorage);
        Stream->Handle = reinterpret_cast<HQUIC>(static_cast<std::uintptr_t>(1));
        Stream->CleanUpMode = CleanUpManual;
        Stream->Callback = MsQuicStream::NoOpCallback;
        Stream->Context = nullptr;
        Owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
        Check(Owner->InstallDetachedStreamForTest(Stream));
        Relay = std::make_shared<TqUvRelayState>();
        // Direction tests drive the loop-owned processor explicitly. A
        // dedicated test below covers callback command-admission failure.
        Relay->Worker = nullptr;
        Relay->DirectQuicReceiveForTest = true;
        Relay->RelayId = 17;
        Relay->RouteGeneration = 9;
        Relay->ControlGeneration = 3;
        Relay->Stream = Stream;
        Relay->StreamOwner = Owner;
        Check(Relay->ActivationMutex.Initialize());
        Relay->Activation = TqUvActivation::Active;
        Relay->CompressAlgo = algo;
        Relay->PrecommitMaxPendingBytes = 8;
        Relay->TcpHandle.data = Relay.get();
        Relay->Binding = std::make_shared<TqUvStreamBinding>();
        Relay->Binding->Relay = Relay;
        Relay->Binding->RouteGeneration = Relay->RouteGeneration;
        Relay->Binding->Activation.store(TqUvActivation::Active);
        ResetCalls();
    }

    ~RelayFixture() {
        Owner->ReleaseStreamForTest();
        MsQuic = nullptr;
    }

    void ResetCalls() {
        gCompleteCalls = 0;
        gCompleteBytes = 0;
        gReceiveEnabled.clear();
        gWriteRequest = nullptr;
        gWriteCallback = nullptr;
        gWrittenBuffers.clear();
        gCompletionLengths.clear();
        gWrittenIds.clear();
        gAllWrittenBytes.clear();
    }

    QUIC_STATUS Receive(
        std::vector<QUIC_BUFFER>& buffers,
        bool throughBinding = false,
        QUIC_RECEIVE_FLAGS flags = QUIC_RECEIVE_FLAG_NONE) {
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_RECEIVE;
        event.RECEIVE.Buffers = buffers.data();
        event.RECEIVE.BufferCount = static_cast<std::uint32_t>(buffers.size());
        event.RECEIVE.Flags = flags;
        for (const auto& buffer : buffers) {
            event.RECEIVE.TotalBufferLength += buffer.Length;
        }
        return throughBinding
            ? Relay->Binding->OnStreamEvent(
                  Stream, &event, Relay->Binding->RouteGeneration)
            : TqUvAcceptQuicReceive(Relay, Stream, event);
    }

    TqUvRelayWorker Worker;
    alignas(MsQuicStream) unsigned char StreamStorage[sizeof(MsQuicStream)]{};
    MsQuicStream* Stream{nullptr};
    std::shared_ptr<TqStreamLifetime> Owner;
    std::shared_ptr<TqUvRelayState> Relay;
};

void TestActiveZeroLengthFinPublishesDurableFactWithoutOwnership() {
    RelayFixture fixture;
    std::vector<QUIC_BUFFER> buffers;
    Check(fixture.Receive(
        buffers, false, QUIC_RECEIVE_FLAG_FIN) == QUIC_STATUS_SUCCESS);
    Check(fixture.Relay->QuicFinObserved.load(std::memory_order_acquire));
    Check(fixture.Relay->PendingQuicReceiveBytes == 0);
    Check(gWriteRequest == nullptr);
    const auto metrics = fixture.Worker.Snapshot();
    Check(metrics.DecompressedTcpBytes == 0);
    Check(metrics.ZstdDecompressInputBytes == 0);
    Check(metrics.ZstdDecompressOutputBytes == 0);
    Check(metrics.ZstdDecompressCalls == 0);
    Check(metrics.ZstdDecompressFailures == 0);
    Check(metrics.QuicReceiveDecompressFailures == 0);
}

void TestPreparedZeroLengthFinSurvivesActivation() {
    RelayFixture fixture;
    fixture.Relay->Activation = TqUvActivation::Prepared;
    fixture.Relay->Binding->Activation.store(TqUvActivation::Prepared);
    std::vector<QUIC_BUFFER> buffers;
    Check(fixture.Receive(
        buffers, true, QUIC_RECEIVE_FLAG_FIN) == QUIC_STATUS_SUCCESS);
    Check(fixture.Relay->QuicFinObserved.load(std::memory_order_acquire));
    Check(fixture.Relay->PrecommitReceives.empty());
}

void TestDataAndFinPublishesFinOnlyAfterPayloadWriteCompletes() {
    RelayFixture fixture;
    std::uint8_t data[]{1, 2, 3};
    std::vector<QUIC_BUFFER> buffers{{3, data}};
    Check(fixture.Receive(
        buffers, false, QUIC_RECEIVE_FLAG_FIN) == QUIC_STATUS_PENDING);
    Check(!fixture.Relay->QuicFinObserved.load(std::memory_order_acquire));
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    CompleteWrite(0);
    Check(fixture.Relay->QuicFinObserved.load(std::memory_order_acquire));
}

void TestZeroLengthFinLosingTerminalRaceTakesNoOwnership() {
    RelayFixture fixture;
    fixture.Relay->Activation = TqUvActivation::Terminal;
    fixture.Relay->Binding->Activation.store(TqUvActivation::Terminal);
    std::vector<QUIC_BUFFER> buffers;
    Check(fixture.Receive(
        buffers, false, QUIC_RECEIVE_FLAG_FIN) == QUIC_STATUS_SUCCESS);
    Check(fixture.Relay->PendingQuicReceiveBytes == 0);
    Check(fixture.Relay->PrecommitReceives.empty());
    Check(gWriteRequest == nullptr);
}

class PartialDecompressor final : public ITqDecompressor {
public:
    bool Decompress(const std::uint8_t*, size_t, std::vector<std::uint8_t>&) override {
        return false;
    }
    bool DecompressInto(
        const std::uint8_t* input,
        size_t inputLength,
        std::uint8_t* output,
        size_t outputCapacity,
        TqDecompressResult* result) override {
        ++Calls;
        const size_t take = std::min<size_t>(3, std::min(inputLength, outputCapacity));
        std::memcpy(output, input, take);
        *result = {take, take, take == inputLength, take < inputLength};
        return true;
    }
    void Reset() override {}
    std::uint64_t Calls{0};
};

class ZeroThenOutputDecompressor final : public ITqDecompressor {
public:
    bool Decompress(const std::uint8_t*, size_t, std::vector<std::uint8_t>&) override {
        return false;
    }
    bool DecompressInto(
        const std::uint8_t* input,
        size_t inputLength,
        std::uint8_t* output,
        size_t outputCapacity,
        TqDecompressResult* result) override {
        ++Calls;
        if (Calls == 1) {
            *result = {inputLength, 0, true, false};
            return true;
        }
        const auto take = std::min(inputLength, outputCapacity);
        std::memcpy(output, input, take);
        *result = {take, take, true, false};
        return true;
    }
    void Reset() override {}
    std::uint64_t Calls{0};
};

class ExpandingDecompressor final : public ITqDecompressor {
public:
    bool Decompress(const std::uint8_t*, size_t, std::vector<std::uint8_t>&) override {
        return false;
    }
    bool DecompressInto(
        const std::uint8_t*,
        size_t inputLength,
        std::uint8_t* output,
        size_t outputCapacity,
        TqDecompressResult* result) override {
        if (inputLength == 0 || outputCapacity < 7) {
            return false;
        }
        std::memset(output, 0x5a, 7);
        *result = {1, 7, inputLength == 1, inputLength != 1};
        return true;
    }
    void Reset() override {}
};

class BufferedTailDecompressor final : public ITqDecompressor {
public:
    bool Decompress(const std::uint8_t*, size_t, std::vector<std::uint8_t>&) override {
        return false;
    }
    bool DecompressInto(
        const std::uint8_t* input,
        size_t inputLength,
        std::uint8_t* output,
        size_t outputCapacity,
        TqDecompressResult* result) override {
        ++Calls;
        if (Calls == 1) {
            Check(input != nullptr && inputLength == 1 && outputCapacity != 0);
            std::memset(output, 'h', outputCapacity);
            *result = {1, outputCapacity, false, true};
            return true;
        }
        Check(input == nullptr && inputLength == 0);
        if (Calls == 2) {
            Check(outputCapacity >= 4);
            std::memcpy(output, "tail", 4);
            *result = {0, 4, true, false};
            return true;
        }
        *result = {0, 0, true, false};
        return true;
    }
    void Reset() override {}
    std::uint64_t Calls{0};
};

class OneByteThenDrainDecompressor final : public ITqDecompressor {
public:
    bool Decompress(const std::uint8_t*, size_t, std::vector<std::uint8_t>&) override {
        return false;
    }
    bool DecompressInto(
        const std::uint8_t* input,
        size_t inputLength,
        std::uint8_t* output,
        size_t outputCapacity,
        TqDecompressResult* result) override {
        ++Calls;
        if (inputLength != 0) {
            Check(input != nullptr && inputLength == 1 && outputCapacity != 0);
            output[0] = input[0];
            *result = {1, 1, true, false};
            return true;
        }
        Check(input == nullptr);
        *result = {0, 0, true, false};
        return true;
    }
    void Reset() override {}
    std::uint64_t Calls{0};
};

class ManySlicesThenOutputDecompressor final : public ITqDecompressor {
public:
    bool Decompress(const std::uint8_t*, size_t, std::vector<std::uint8_t>&) override {
        return false;
    }
    bool DecompressInto(
        const std::uint8_t* input,
        size_t inputLength,
        std::uint8_t* output,
        size_t outputCapacity,
        TqDecompressResult* result) override {
        ++Calls;
        if (inputLength != 0) {
            Check(input != nullptr && inputLength == 1);
            *result = {1, 0, true, false};
            return true;
        }
        Check(input == nullptr && outputCapacity >= 4);
        if (!OutputProduced) {
            std::memcpy(output, "done", 4);
            OutputProduced = true;
            *result = {0, 4, true, false};
            return true;
        }
        *result = {0, 0, true, false};
        return true;
    }
    void Reset() override {}
    std::uint64_t Calls{0};
    bool OutputProduced{false};
};

class RecordingInputLengthDecompressor final : public ITqDecompressor {
public:
    bool Decompress(const std::uint8_t*, size_t, std::vector<std::uint8_t>&) override {
        return false;
    }
    bool DecompressInto(
        const std::uint8_t* input,
        size_t inputLength,
        std::uint8_t* output,
        size_t outputCapacity,
        TqDecompressResult* result) override {
        InputLengths.push_back(inputLength);
        if (inputLength != 0) {
            Check(input != nullptr);
            TotalConsumed += inputLength;
            *result = {inputLength, 0, true, false};
            return true;
        }
        Check(input == nullptr && outputCapacity >= 4);
        if (!OutputProduced) {
            std::memcpy(output, "done", 4);
            OutputProduced = true;
            *result = {0, 4, true, false};
            return true;
        }
        *result = {0, 0, true, false};
        return true;
    }
    void Reset() override {}
    std::vector<std::size_t> InputLengths;
    std::uint64_t TotalConsumed{0};
    bool OutputProduced{false};
};

void CompleteWrite(int status) {
    uv_write_t* request = nullptr;
    uv_write_cb callback = nullptr;
    {
        std::lock_guard<std::mutex> guard(gIoMutex);
        Check(gWriteRequest != nullptr && gWriteCallback != nullptr);
        request = gWriteRequest;
        callback = gWriteCallback;
        gWriteRequest = nullptr;
        gWriteCallback = nullptr;
    }
    callback(request, status);
}

bool WaitForWrite() {
    std::unique_lock<std::mutex> lock(gIoMutex);
    return gIoCondition.wait_for(lock, std::chrono::seconds(5), [] {
        return gWriteRequest != nullptr;
    });
}

void TestUncompressedSlicesCompleteOnlyAfterWrite() {
    RelayFixture fixture;
    std::uint8_t first[]{'a', 'b'};
    std::uint8_t second[]{'c', 'd', 'e'};
    std::vector<QUIC_BUFFER> buffers{{2, first}, {3, second}};
    Check(fixture.Receive(buffers) == QUIC_STATUS_PENDING);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    Check(gCompleteCalls == 0 && gWrittenBuffers.size() == 2);
    Check(gWrittenBuffers[0] == std::vector<std::uint8_t>({'a', 'b'}));
    Check(gWrittenBuffers[1] == std::vector<std::uint8_t>({'c', 'd', 'e'}));
    CompleteWrite(0);
    Check(gCompleteCalls == 1 && gCompleteBytes == 5);
    auto snapshot = TqUvQuicToTcpSnapshot(*fixture.Relay);
    Check(snapshot.PendingQuicReceiveBytes == 0 && snapshot.PendingTcpWriteBytes == 0);
}

void TestTerminalWriteErrorSettlesOnlyOnce() {
    RelayFixture fixture;
    std::uint8_t data[]{1, 2, 3};
    std::vector<QUIC_BUFFER> buffers{{3, data}};
    Check(fixture.Receive(buffers) == QUIC_STATUS_PENDING);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    CompleteWrite(UV_EPIPE);
    Check(gCompleteCalls == 1 && gCompleteBytes == 3);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    Check(gCompleteCalls == 1 && gCompleteBytes == 3);
    Check(!fixture.Relay->TerminalStarted);
    Check(fixture.Relay->TerminalTriggerMask.load(std::memory_order_acquire) != 0);
}

void TestDecompressedInputCompletesBeforeOutputWrite() {
    RelayFixture fixture(TqCompressAlgo::Zstd);
    PartialDecompressor decompressor;
    fixture.Relay->Decompressor = &decompressor;
    std::uint8_t data[]{'h', 'e', 'l', 'l', 'o'};
    std::vector<QUIC_BUFFER> buffers{{5, data}};
    Check(fixture.Receive(buffers) == QUIC_STATUS_PENDING);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    Check(gCompleteCalls == 1 && gCompleteBytes == 3);
    Check(gWrittenBuffers.size() == 1 && gWrittenBuffers[0].size() == 3);
    auto snapshot = TqUvQuicToTcpSnapshot(*fixture.Relay);
    Check(snapshot.PendingQuicReceiveBytes == 2 && snapshot.PendingTcpWriteBytes == 3);
    Check(fixture.Relay->BufferBudget.PendingBufferBytes == 3);
    CompleteWrite(0);
    Check(gCompleteCalls == 1 && gCompleteBytes == 3);
    Check(fixture.Relay->BufferBudget.PendingBufferBytes == 0);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    Check(gCompleteCalls == 2 && gCompleteBytes == 5);
    Check(fixture.Relay->BufferBudget.PendingBufferBytes == 2);
    snapshot = TqUvQuicToTcpSnapshot(*fixture.Relay);
    Check(snapshot.PendingQuicReceiveBytes == 0 && snapshot.PendingTcpWriteBytes == 2);
    CompleteWrite(0);
    Check(fixture.Relay->BufferBudget.PendingBufferBytes == 0);
    const auto metrics = fixture.Worker.Snapshot();
    Check(metrics.DecompressedTcpBytes == 5);
    Check(metrics.ZstdDecompressInputBytes == 5);
    Check(metrics.ZstdDecompressOutputBytes == 5);
    Check(metrics.ZstdDecompressCalls >= 2);
    Check(metrics.ZstdDecompressFailures == 0);
    Check(metrics.QuicReceiveDecompressFailures == 0);
}

void TestBackpressureUsesHighAndHalfLowWatermarks() {
    RelayFixture fixture;
    std::uint8_t data[]{1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<QUIC_BUFFER> buffers{{8, data}};
    Check(fixture.Receive(buffers) == QUIC_STATUS_PENDING);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    const auto metrics = fixture.Worker.Snapshot();
    Check(metrics.DecompressedTcpBytes == 0);
    Check(metrics.ZstdDecompressCalls == 0);
    Check(metrics.ZstdDecompressFailures == 0);
    Check(metrics.QuicReceiveDecompressFailures == 0);
    Check(gReceiveEnabled.size() == 1 && !gReceiveEnabled.front());
    CompleteWrite(0);
    Check(gReceiveEnabled.size() == 2 && gReceiveEnabled.back());
}

void TestTotalPressureIncludesPendingDecompressedOutput() {
    RelayFixture fixture(TqCompressAlgo::Zstd);
    ExpandingDecompressor decompressor;
    fixture.Relay->Decompressor = &decompressor;
    std::uint8_t compressed[]{1};
    std::vector<QUIC_BUFFER> compressedBuffers{{1, compressed}};
    Check(fixture.Receive(compressedBuffers) == QUIC_STATUS_PENDING);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    auto snapshot = TqUvQuicToTcpSnapshot(*fixture.Relay);
    Check(snapshot.PressureBytes == 7 && snapshot.PendingTcpWriteBytes == 7);
    Check(gReceiveEnabled.size() == 1 && !gReceiveEnabled.front());

    std::uint8_t rejected[]{2, 3};
    std::vector<QUIC_BUFFER> rejectedBuffers{{2, rejected}};
    Check(fixture.Receive(rejectedBuffers) == QUIC_STATUS_OUT_OF_MEMORY);
    Check(TqUvQuicToTcpSnapshot(*fixture.Relay).PressureBytes == 7);

    std::uint8_t exact[]{4};
    std::vector<QUIC_BUFFER> exactBuffers{{1, exact}};
    Check(fixture.Receive(exactBuffers) == QUIC_STATUS_PENDING);
    Check(TqUvQuicToTcpSnapshot(*fixture.Relay).PressureBytes == 8);
    CompleteWrite(0);
    snapshot = TqUvQuicToTcpSnapshot(*fixture.Relay);
    Check(snapshot.PressureBytes == 1);
    Check(gReceiveEnabled.size() == 2 && gReceiveEnabled.back());

    fixture.Relay->Binding->Activation.store(TqUvActivation::Terminal);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    snapshot = TqUvQuicToTcpSnapshot(*fixture.Relay);
    Check(snapshot.PressureBytes == 0 && snapshot.PendingReceives == 0);
    Check(fixture.Relay->AdmittedQuicReceiveBytes == 0);
}

void TestDecompressionAllocationFailureKeepsInputOwned() {
    RelayFixture fixture(TqCompressAlgo::Zstd);
    PartialDecompressor decompressor;
    fixture.Relay->Decompressor = &decompressor;
    fixture.Relay->BufferBudget.MaxPendingBufferBytes = 1;
    std::uint8_t data[]{1, 2, 3};
    std::vector<QUIC_BUFFER> buffers{{3, data}};
    Check(fixture.Receive(buffers) == QUIC_STATUS_PENDING);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    Check(!fixture.Relay->TerminalStarted);
    Check(fixture.Relay->TerminalTriggerMask.load(std::memory_order_acquire) != 0);
    Check(gCompleteCalls == 0 && gCompleteBytes == 0);
    Check(gWriteRequest == nullptr);
    const auto metrics = fixture.Worker.Snapshot();
    Check(metrics.ZstdDecompressOutputBytes == 3);
    Check(metrics.DecompressedTcpBytes == 0);
    Check(metrics.TcpWriteBytes == 0);
}

void TestZeroOutputProgressContinuesAcrossSlices() {
    RelayFixture fixture(TqCompressAlgo::Zstd);
    ZeroThenOutputDecompressor decompressor;
    fixture.Relay->Decompressor = &decompressor;
    std::uint8_t first[]{1};
    std::uint8_t second[]{2, 3};
    std::vector<QUIC_BUFFER> buffers{{1, first}, {2, second}};
    Check(fixture.Receive(buffers) == QUIC_STATUS_PENDING);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    Check(decompressor.Calls == 2);
    Check(gCompleteCalls == 2 && gCompleteBytes == 3);
    Check(gWriteRequest != nullptr && gWrittenBuffers.size() == 1);
    Check(gWrittenBuffers[0] == std::vector<std::uint8_t>({2, 3}));
    CompleteWrite(0);
}

void TestCompressedReceiveDrainsBufferedOutputAfterInputCompletion() {
    RelayFixture fixture(TqCompressAlgo::Zstd);
    BufferedTailDecompressor decompressor;
    fixture.Relay->Decompressor = &decompressor;
    fixture.Relay->PrecommitMaxPendingBytes = 128 * 1024;
    std::uint8_t compressed[]{1};
    std::vector<QUIC_BUFFER> buffers{{1, compressed}};
    Check(fixture.Receive(
        buffers, false, QUIC_RECEIVE_FLAG_FIN) == QUIC_STATUS_PENDING);

    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    Check(decompressor.Calls == 1);
    Check(gCompleteCalls == 1 && gCompleteBytes == 1);
    Check(gAllWrittenBytes.size() == 64 * 1024);
    Check(std::all_of(gAllWrittenBytes.begin(), gAllWrittenBytes.end(),
        [](std::uint8_t byte) { return byte == 'h'; }));
    Check(!fixture.Relay->QuicFinObserved.load(std::memory_order_acquire));

    CompleteWrite(0);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    Check(decompressor.Calls == 2);
    Check(gAllWrittenBytes.size() == 64 * 1024 + 4);
    Check(std::equal(
        gAllWrittenBytes.end() - 4, gAllWrittenBytes.end(), "tail"));
    Check(!fixture.Relay->QuicFinObserved.load(std::memory_order_acquire));

    CompleteWrite(0);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    Check(decompressor.Calls == 3);
    Check(fixture.Relay->QuicFinObserved.load(std::memory_order_acquire));
}

void TestDrainedCompressedReceiveAdvancesQueuedReceive() {
    RelayFixture fixture(TqCompressAlgo::Zstd);
    OneByteThenDrainDecompressor decompressor;
    fixture.Relay->Decompressor = &decompressor;
    std::uint8_t first[]{1};
    std::uint8_t second[]{2};
    std::vector<QUIC_BUFFER> firstBuffers{{1, first}};
    std::vector<QUIC_BUFFER> secondBuffers{{1, second}};

    Check(fixture.Receive(firstBuffers) == QUIC_STATUS_PENDING);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    Check(gWriteRequest != nullptr);
    Check(gAllWrittenBytes == std::vector<std::uint8_t>({1}));
    Check(gCompleteCalls == 1 && gCompleteBytes == 1);

    Check(fixture.Receive(secondBuffers) == QUIC_STATUS_PENDING);
    Check(gCompleteCalls == 1);
    fixture.Relay->Worker = &fixture.Worker;
    CompleteWrite(0);

    Check(gWriteRequest != nullptr);
    Check(gAllWrittenBytes == std::vector<std::uint8_t>({1, 2}));
    Check(gCompleteCalls == 2 && gCompleteBytes == 2);
    Check(gCompletionLengths == std::vector<std::uint64_t>({1, 1}));
    CompleteWrite(0);
    Check(TqUvQuicToTcpSnapshot(*fixture.Relay).PendingReceives == 0);
}

void TestCompressedReceiveBudgetYieldsToQueuedMarker() {
    RelayFixture fixture(TqCompressAlgo::Zstd);
    ManySlicesThenOutputDecompressor decompressor;
    fixture.Relay->Decompressor = &decompressor;
    fixture.Relay->PrecommitMaxPendingBytes = 128;
    fixture.Relay->QuicToTcpCallBudget = 2;
    fixture.Relay->QuicToTcpByteBudgetPerTick = 2;
    fixture.Relay->Worker = &fixture.Worker;
    Check(fixture.Worker.StartAndWaitReady());

    std::mutex blockMutex;
    std::condition_variable blockCondition;
    bool entered = false;
    bool release = false;
    Check(fixture.Worker.Post([&](TqUvRelayWorker&) {
        std::unique_lock<std::mutex> lock(blockMutex);
        entered = true;
        blockCondition.notify_all();
        blockCondition.wait(lock, [&] { return release; });
    }));
    {
        std::unique_lock<std::mutex> lock(blockMutex);
        Check(blockCondition.wait_for(lock, std::chrono::seconds(5), [&] {
            return entered;
        }));
    }

    std::vector<std::uint8_t> bytes(65, 1);
    std::vector<QUIC_BUFFER> buffers;
    buffers.reserve(bytes.size());
    for (auto& byte : bytes) {
        buffers.push_back({1, &byte});
    }
    Check(fixture.Receive(buffers, true) == QUIC_STATUS_PENDING);

    std::mutex markerMutex;
    std::condition_variable markerCondition;
    bool markerRan = false;
    std::uint64_t callsAtMarker = 0;
    Check(fixture.Worker.Post([&](TqUvRelayWorker&) {
        std::lock_guard<std::mutex> guard(markerMutex);
        callsAtMarker = decompressor.Calls;
        markerRan = true;
        markerCondition.notify_all();
    }));
    {
        std::lock_guard<std::mutex> guard(blockMutex);
        release = true;
        blockCondition.notify_all();
    }
    {
        std::unique_lock<std::mutex> lock(markerMutex);
        Check(markerCondition.wait_for(lock, std::chrono::seconds(5), [&] {
            return markerRan;
        }));
    }

    Check(callsAtMarker == 2);
    Check(WaitForWrite());
    Check(gCompleteCalls == 65 && gCompleteBytes == 65);
    Check(gCompletionLengths == std::vector<std::uint64_t>(65, 1));
    Check(gAllWrittenBytes == std::vector<std::uint8_t>({'d', 'o', 'n', 'e'}));
    RunOnLoop(fixture.Worker, [&](TqUvRelayWorker&) { CompleteWrite(0); });
    RunOnLoop(fixture.Worker, [&](TqUvRelayWorker&) {
        Check(TqUvQuicToTcpSnapshot(*fixture.Relay).PendingReceives == 0);
    });
    Check(fixture.Relay->TerminalTriggerMask.load(std::memory_order_acquire) == 0);
    Check(fixture.Worker.StopForTest());
}

void TestCompressedReceiveDeferredEnqueueFailureIsTerminal() {
    RelayFixture fixture(TqCompressAlgo::Zstd);
    ManySlicesThenOutputDecompressor decompressor;
    fixture.Relay->Decompressor = &decompressor;
    fixture.Relay->PrecommitMaxPendingBytes = 128;
    fixture.Relay->QuicToTcpCallBudget = 2;
    fixture.Relay->QuicToTcpByteBudgetPerTick = 2;
    fixture.Relay->Worker = &fixture.Worker;
    Check(fixture.Worker.StartAndWaitReady());

    std::vector<std::uint8_t> bytes(65, 1);
    std::vector<QUIC_BUFFER> buffers;
    buffers.reserve(bytes.size());
    for (auto& byte : bytes) {
        buffers.push_back({1, &byte});
    }
    gFailDeferredPost.store(true, std::memory_order_release);
    gTerminalTrigger.store(-1, std::memory_order_release);
    TqUvSetTerminalHookForTest(&CaptureTerminalTrigger);
    Check(fixture.Receive(buffers, true) == QUIC_STATUS_PENDING);
    Check(WaitUntil([&] {
        return gTerminalTrigger.load(std::memory_order_acquire) != -1;
    }));
    RunOnLoop(fixture.Worker, [&](TqUvRelayWorker&) {
        Check(TqUvQuicToTcpSnapshot(*fixture.Relay).PendingReceives == 0);
    });
    Check(gTerminalTrigger.load(std::memory_order_acquire) ==
        static_cast<int>(TqUvTerminalTrigger::QueueFailure));
    Check(gCompleteBytes == 65);
    Check(gWriteRequest == nullptr);
    Check(fixture.Worker.StopForTest());
    TqUvSetTerminalHookForTest(nullptr);
}

void TestCompressedReceiveCapsLargeSliceBeforeByteBudget() {
    RelayFixture fixture(TqCompressAlgo::Zstd);
    RecordingInputLengthDecompressor decompressor;
    fixture.Relay->Decompressor = &decompressor;
    fixture.Relay->PrecommitMaxPendingBytes = 128;
    fixture.Relay->QuicToTcpCallBudget = 32;
    fixture.Relay->QuicToTcpByteBudgetPerTick = 3;
    fixture.Relay->Worker = &fixture.Worker;
    Check(fixture.Worker.StartAndWaitReady());

    std::mutex blockMutex;
    std::condition_variable blockCondition;
    bool entered = false;
    bool release = false;
    Check(fixture.Worker.Post([&](TqUvRelayWorker&) {
        std::unique_lock<std::mutex> lock(blockMutex);
        entered = true;
        blockCondition.notify_all();
        blockCondition.wait(lock, [&] { return release; });
    }));
    {
        std::unique_lock<std::mutex> lock(blockMutex);
        Check(blockCondition.wait_for(lock, std::chrono::seconds(5), [&] {
            return entered;
        }));
    }

    std::uint8_t bytes[9]{1, 2, 3, 4, 5, 6, 7, 8, 9};
    std::vector<QUIC_BUFFER> buffers{{9, bytes}};
    Check(fixture.Receive(buffers, true) == QUIC_STATUS_PENDING);

    std::mutex markerMutex;
    std::condition_variable markerCondition;
    bool markerRan = false;
    std::uint64_t consumedAtMarker = 0;
    Check(fixture.Worker.Post([&](TqUvRelayWorker&) {
        std::lock_guard<std::mutex> guard(markerMutex);
        consumedAtMarker = decompressor.TotalConsumed;
        markerRan = true;
        markerCondition.notify_all();
    }));
    {
        std::lock_guard<std::mutex> guard(blockMutex);
        release = true;
        blockCondition.notify_all();
    }
    {
        std::unique_lock<std::mutex> lock(markerMutex);
        Check(markerCondition.wait_for(lock, std::chrono::seconds(5), [&] {
            return markerRan;
        }));
    }

    Check(consumedAtMarker <= 3);
    Check(WaitForWrite());
    Check(decompressor.InputLengths ==
        std::vector<std::size_t>({3, 3, 3, 0}));
    Check(gCompleteBytes == 9);
    Check(gCompletionLengths == std::vector<std::uint64_t>({3, 3, 3}));
    RunOnLoop(fixture.Worker, [&](TqUvRelayWorker&) { CompleteWrite(0); });
    RunOnLoop(fixture.Worker, [&](TqUvRelayWorker&) {
        Check(TqUvQuicToTcpSnapshot(*fixture.Relay).PendingReceives == 0);
    });
    Check(fixture.Relay->TerminalTriggerMask.load(std::memory_order_acquire) == 0);
    Check(fixture.Worker.StopForTest());
}

void TestCompressedLeaseRetryDoesNotRepeatDecompressionOrWrite() {
    RelayFixture fixture(TqCompressAlgo::Zstd);
    PartialDecompressor decompressor;
    fixture.Relay->Decompressor = &decompressor;
    std::uint8_t data[]{4, 5, 6};
    std::vector<QUIC_BUFFER> buffers{{3, data}};
    Check(fixture.Receive(buffers) == QUIC_STATUS_PENDING);
    fixture.Owner->DenyReceiveApiLeasesForTest(1);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    Check(decompressor.Calls == 1 && gCompleteCalls == 0);
    Check(gWriteRequest == nullptr);
    Check(fixture.Relay->BufferBudget.PendingBufferBytes == 3);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    Check(decompressor.Calls == 1);
    Check(gCompleteCalls == 1 && gCompleteBytes == 3);
    Check(gWriteRequest != nullptr);
    Check(gWrittenBuffers[0] == std::vector<std::uint8_t>({4, 5, 6}));
    CompleteWrite(0);
    Check(fixture.Relay->BufferBudget.PendingBufferBytes == 0);
}

void TestCompressedPartialLeaseFailureThenTerminalSettlesWholeView() {
    RelayFixture fixture(TqCompressAlgo::Zstd);
    PartialDecompressor decompressor;
    fixture.Relay->Decompressor = &decompressor;
    fixture.Relay->PrecommitMaxPendingBytes = 16;
    std::uint8_t data[]{1, 2, 3, 4, 5};
    std::vector<QUIC_BUFFER> buffers{{5, data}};
    Check(fixture.Receive(buffers) == QUIC_STATUS_PENDING);
    const auto pending = fixture.Relay->FallbackReceiveHead;
    Check(pending != nullptr && pending->TotalBytes == 5);
    fixture.Owner->DenyReceiveApiLeasesForTest(1);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    Check(decompressor.Calls == 1 && gCompleteCalls == 0);
    Check(fixture.Relay->BufferBudget.PendingBufferBytes == 3);
    Check(TqUvQuicToTcpSnapshot(*fixture.Relay).PressureBytes == 8);

    fixture.Relay->Binding->Activation.store(TqUvActivation::Terminal);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    const auto snapshot = TqUvQuicToTcpSnapshot(*fixture.Relay);
    Check(decompressor.Calls == 1 && gWriteRequest == nullptr);
    Check(gCompletionLengths == std::vector<std::uint64_t>({3, 2}));
    Check(gCompleteBytes == 5 && fixture.Relay->BufferBudget.PendingBufferBytes == 0);
    Check(snapshot.PendingQuicReceiveBytes == 0 && snapshot.PendingReceives == 0);
    Check(snapshot.PressureBytes == 0);
    Check(fixture.Relay->AdmittedQuicReceiveBytes == 0);
    Check(pending->Settled.load() && pending->CompletedBytes == pending->TotalBytes);
}

void TestRealZstdDecompressesOneByteSlices() {
    auto compressor = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
    auto decompressor = TqCreateDecompressor(TqCompressAlgo::Zstd);
    Check(compressor != nullptr && decompressor != nullptr);
    const std::vector<std::uint8_t> original{
        'r', 'e', 'a', 'l', '-', 'z', 's', 't', 'd', '-', 's', 'l', 'i', 'c', 'e'};
    std::vector<std::uint8_t> compressed;
    Check(compressor->Compress(
        original.data(), original.size(), compressed, true));
    Check(!compressed.empty() && compressed.size() < 64);

    RelayFixture fixture(TqCompressAlgo::Zstd);
    fixture.Relay->Decompressor = decompressor.get();
    fixture.Relay->PrecommitMaxPendingBytes = 1024 * 1024;
    fixture.Relay->BufferBudget.MaxPendingBufferBytes = 1024 * 1024;
    std::vector<QUIC_BUFFER> slices;
    for (auto& byte : compressed) {
        slices.push_back({1, &byte});
    }
    Check(fixture.Receive(slices) == QUIC_STATUS_PENDING);
    for (unsigned step = 0;
         step < 128 && fixture.Relay->AdmittedQuicReceiveBytes != 0;
         ++step) {
        TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
        if (gWriteRequest != nullptr) {
            CompleteWrite(0);
        }
    }
    Check(gCompleteBytes == compressed.size());
    Check(fixture.Relay->AdmittedQuicReceiveBytes == 0);
    Check(gAllWrittenBytes == original);
}

void TestRealZstdDrainsOutputLargerThanWriteBuffer() {
    auto compressor = TqCreateCompressor(TqCompressAlgo::Zstd, 1);
    auto decompressor = TqCreateDecompressor(TqCompressAlgo::Zstd);
    Check(compressor != nullptr && decompressor != nullptr);
    std::vector<std::uint8_t> original(64 * 1024 + 116, 'z');
    std::vector<std::uint8_t> compressed;
    Check(compressor->Compress(
        original.data(), original.size(), compressed, true));
    Check(!compressed.empty());

    RelayFixture fixture(TqCompressAlgo::Zstd);
    fixture.Relay->Decompressor = decompressor.get();
    fixture.Relay->PrecommitMaxPendingBytes = original.size() * 2;
    QUIC_BUFFER buffer{
        static_cast<std::uint32_t>(compressed.size()), compressed.data()};
    std::vector<QUIC_BUFFER> buffers{buffer};
    Check(fixture.Receive(
        buffers, false, QUIC_RECEIVE_FLAG_FIN) == QUIC_STATUS_PENDING);

    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    Check(gAllWrittenBytes.size() == 64 * 1024);
    Check(!fixture.Relay->QuicFinObserved.load(std::memory_order_acquire));
    CompleteWrite(0);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    Check(gAllWrittenBytes == original);
    Check(!fixture.Relay->QuicFinObserved.load(std::memory_order_acquire));
    CompleteWrite(0);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    Check(fixture.Relay->QuicFinObserved.load(std::memory_order_acquire));
}

void TestReceiveCompletionLeaseFailureRetriesWithoutRewritingTcp() {
    RelayFixture fixture;
    std::uint8_t data[]{1, 2, 3, 4};
    std::vector<QUIC_BUFFER> buffers{{4, data}};
    Check(fixture.Receive(buffers) == QUIC_STATUS_PENDING);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    fixture.Owner->DenyReceiveApiLeasesForTest(1);
    CompleteWrite(0);
    auto snapshot = TqUvQuicToTcpSnapshot(*fixture.Relay);
    Check(gCompleteCalls == 0 && snapshot.PendingQuicReceiveBytes == 4);
    Check(snapshot.PendingReceives == 1 && snapshot.PendingWrites == 0);
    Check(gWriteRequest == nullptr);
    TqUvProcessQuicToTcp(fixture.Worker, *fixture.Relay);
    snapshot = TqUvQuicToTcpSnapshot(*fixture.Relay);
    Check(gCompleteCalls == 1 && gCompleteBytes == 4);
    Check(snapshot.PendingQuicReceiveBytes == 0 && snapshot.PendingReceives == 0);
    Check(gWriteRequest == nullptr);
}

void TestTerminalShutdownSettlesCancelledReceiveWithoutApiLease() {
    RelayFixture fixture;
    fixture.Relay->Worker = &fixture.Worker;
    fixture.Relay->PrecommitSettled = true;
    fixture.Relay->Binding->PrecommitSettled.store(
        true, std::memory_order_release);
    Check(fixture.Worker.StartAndWaitReady());
    std::uint64_t publishedGeneration = 0;
    Check(fixture.Owner->PublishTarget(
        fixture.Owner->RouteGeneration(), fixture.Relay->Binding,
        &publishedGeneration));
    fixture.Relay->RouteGeneration = publishedGeneration;
    fixture.Relay->Binding->RouteGeneration = publishedGeneration;

    std::uint8_t data[]{1, 2, 3, 4};
    std::vector<QUIC_BUFFER> buffers{{4, data}};
    Check(fixture.Receive(buffers, true) == QUIC_STATUS_PENDING);
    Check(WaitForWrite());

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    Check(fixture.Owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    Check(fixture.Owner->GetPhase() ==
        TqStreamLifetime::Phase::TerminalPublished);
    Check(fixture.Relay->QuicShutdownObserved.load(std::memory_order_acquire));

    RunOnLoop(fixture.Worker, [&](TqUvRelayWorker& worker) {
        TqUvProcessTerminalFactsLocal(worker, *fixture.Relay);
        Check(fixture.Relay->TerminalStarted);
        CompleteWrite(UV_ECANCELED);
        TqUvSettleQuicReceivesAtTerminal(*fixture.Relay);
        TqUvSettleQuicReceivesAtTerminal(*fixture.Relay);
    });

    const auto snapshot = TqUvQuicToTcpSnapshot(*fixture.Relay);
    Check(gCompleteCalls == 0 && gCompleteBytes == 0);
    Check(snapshot.PendingQuicReceiveBytes == 0);
    Check(snapshot.PendingTcpWriteBytes == 0);
    Check(snapshot.PendingReceives == 0 && snapshot.PendingWrites == 0);
    Check(snapshot.PressureBytes == 0);
    Check(fixture.Relay->AdmittedQuicReceiveBytes == 0);
    Check(fixture.Relay->AccountedQuicToTcpBytes == 0);
    Check(fixture.Worker.Snapshot().QuicToTcpPendingBytes == 0);
    Check(fixture.Worker.StopForTest());
}

void TestCommandAdmissionFailureRollsBackPendingReceive() {
    RelayFixture fixture;
    fixture.Relay->Worker = &fixture.Worker;
    std::uint8_t data[]{1, 2, 3, 4};
    std::vector<QUIC_BUFFER> buffers{{4, data}};
    Check(fixture.Receive(buffers) == QUIC_STATUS_OUT_OF_MEMORY);
    const auto snapshot = TqUvQuicToTcpSnapshot(*fixture.Relay);
    Check(snapshot.PendingQuicReceiveBytes == 0 && snapshot.PendingReceives == 0);
    Check(gCompleteCalls == 0 && gWriteRequest == nullptr);
}

void TestNullWorkerAndTerminalDoNotTakeReceiveOwnership() {
    RelayFixture fixture;
    fixture.Relay->DirectQuicReceiveForTest = false;
    std::uint8_t data[]{1, 2};
    std::vector<QUIC_BUFFER> buffers{{2, data}};
    Check(fixture.Receive(buffers) == QUIC_STATUS_OUT_OF_MEMORY);
    Check(fixture.Relay->AdmittedQuicReceiveBytes == 0);

    fixture.Relay->Binding->Activation.store(TqUvActivation::Terminal);
    Check(fixture.Receive(buffers) == QUIC_STATUS_SUCCESS);
    Check(fixture.Relay->AdmittedQuicReceiveBytes == 0);
}

void TestStartedWorkerConcurrentCallbacksAreExactlyOnceAndBounded() {
    RelayFixture fixture;
    fixture.Relay->Worker = &fixture.Worker;
    fixture.Relay->PrecommitMaxPendingBytes = 10;
    Check(fixture.Worker.StartAndWaitReady());

    constexpr std::size_t count = 4;
    std::uint8_t data[count][4]{};
    std::atomic<int> statuses[count]{};
    std::atomic<unsigned> pending{0};
    std::atomic<unsigned> rejected{0};
    std::vector<std::thread> producers;
    for (std::size_t index = 0; index < count; ++index) {
        producers.emplace_back([&, index] {
            data[index][0] = static_cast<std::uint8_t>(index + 1);
            std::vector<QUIC_BUFFER> buffers{{4, data[index]}};
            const auto status = fixture.Receive(buffers, true);
            if (status == QUIC_STATUS_PENDING) {
                statuses[index] = 1;
                ++pending;
            } else if (status == QUIC_STATUS_OUT_OF_MEMORY) {
                statuses[index] = 2;
                ++rejected;
            } else {
                std::abort();
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    Check(pending == 2 && rejected == 2);
    Check(fixture.Worker.Snapshot().QuicToTcpPendingBytes == 8);

    for (unsigned completed = 0; completed < pending.load(); ++completed) {
        Check(WaitForWrite());
        std::mutex doneMutex;
        std::condition_variable doneCondition;
        bool done = false;
        Check(fixture.Worker.Post([&](TqUvRelayWorker&) {
            CompleteWrite(0);
            std::lock_guard<std::mutex> guard(doneMutex);
            done = true;
            doneCondition.notify_all();
        }));
        std::unique_lock<std::mutex> lock(doneMutex);
        Check(doneCondition.wait_for(lock, std::chrono::seconds(5), [&] {
            return done;
        }));
    }
    Check(gCompleteCalls == 2 && gCompleteBytes == 8);
    {
        std::lock_guard<std::mutex> guard(gIoMutex);
        Check(gCompletionLengths.size() == 2);
        Check(gCompletionLengths[0] == 4 && gCompletionLengths[1] == 4);
        std::vector<std::uint8_t> acceptedIds;
        for (std::size_t index = 0; index < count; ++index) {
            if (statuses[index] == 1) {
                acceptedIds.push_back(static_cast<std::uint8_t>(index + 1));
            }
        }
        auto writtenIds = gWrittenIds;
        std::sort(acceptedIds.begin(), acceptedIds.end());
        std::sort(writtenIds.begin(), writtenIds.end());
        Check(writtenIds == acceptedIds);
    }
    Check(fixture.Relay->AdmittedQuicReceiveBytes == 0);
    Check(fixture.Relay->QuicToTcpPressureBytes == 0);
    Check(fixture.Worker.Snapshot().QuicToTcpPendingBytes == 0);
    Check(fixture.Worker.StopForTest());
}

void TestConsecutiveQueueAllocationFailuresRemainExactlyOnceAtTerminal() {
    RelayFixture fixture;
    fixture.Relay->Worker = &fixture.Worker;
    Check(fixture.Worker.StartAndWaitReady());
    std::mutex blockMutex;
    std::condition_variable blockCondition;
    bool entered = false;
    bool release = false;
    Check(fixture.Worker.Post([&](TqUvRelayWorker&) {
        std::unique_lock<std::mutex> lock(blockMutex);
        entered = true;
        blockCondition.notify_all();
        blockCondition.wait(lock, [&] { return release; });
    }));
    {
        std::unique_lock<std::mutex> lock(blockMutex);
        Check(blockCondition.wait_for(lock, std::chrono::seconds(5), [&] {
            return entered;
        }));
    }

    fixture.Relay->FailActiveReceiveQueueAdmissionsForTest = 2;
    std::uint8_t first[]{1};
    std::uint8_t second[]{2, 2};
    std::uint8_t third[]{3, 3, 3};
    std::vector<QUIC_BUFFER> firstBuffers{{1, first}};
    std::vector<QUIC_BUFFER> secondBuffers{{2, second}};
    std::vector<QUIC_BUFFER> thirdBuffers{{3, third}};
    Check(fixture.Receive(firstBuffers, true) == QUIC_STATUS_PENDING);
    Check(fixture.Receive(secondBuffers, true) == QUIC_STATUS_PENDING);
    std::mutex inspectionMutex;
    std::condition_variable inspectionCondition;
    bool inspectionEntered = false;
    bool releaseInspection = false;
    Check(fixture.Worker.Post([&](TqUvRelayWorker&) {
        std::unique_lock<std::mutex> lock(inspectionMutex);
        inspectionEntered = true;
        inspectionCondition.notify_all();
        inspectionCondition.wait(lock, [&] { return releaseInspection; });
    }));
    Check(fixture.Receive(thirdBuffers, true) == QUIC_STATUS_PENDING);
    fixture.Owner->DenyReceiveApiLeasesForTest(8);
    fixture.Relay->Binding->Activation.store(
        TqUvActivation::Terminal, std::memory_order_release);
    {
        std::lock_guard<std::mutex> guard(blockMutex);
        release = true;
        blockCondition.notify_all();
    }
    {
        std::unique_lock<std::mutex> lock(inspectionMutex);
        Check(inspectionCondition.wait_for(
            lock, std::chrono::seconds(5), [&] { return inspectionEntered; }));
    }
    Check(gCompleteCalls == 0);
    Check(fixture.Relay->FallbackReceiveHead != nullptr);
    Check(fixture.Relay->FallbackReceiveHead->TotalBytes == 1);
    Check(fixture.Relay->FallbackReceiveHead->FallbackNext != nullptr);
    Check(fixture.Relay->FallbackReceiveHead->FallbackNext->TotalBytes == 2);
    Check(fixture.Relay->FallbackReceiveTail ==
        fixture.Relay->FallbackReceiveHead->FallbackNext);
    Check(fixture.Relay->FailActiveReceiveQueueAdmissionsForTest == 0);
    Check(fixture.Relay->ActiveReceiveFallbackMode);

    // The injected failures are exhausted. This later ordinary command must
    // stay behind both fallback nodes after the permanent lane switch.
    fixture.Owner->DenyReceiveApiLeasesForTest(0);
    {
        std::lock_guard<std::mutex> guard(inspectionMutex);
        releaseInspection = true;
        inspectionCondition.notify_all();
    }
    {
        std::unique_lock<std::mutex> lock(gIoMutex);
        Check(gIoCondition.wait_for(lock, std::chrono::seconds(5), [] {
            return gCompleteCalls.load() == 3;
        }));
    }
    std::mutex doneMutex;
    std::condition_variable doneCondition;
    bool done = false;
    Check(fixture.Worker.Post([&](TqUvRelayWorker&) {
        std::lock_guard<std::mutex> guard(doneMutex);
        done = true;
        doneCondition.notify_all();
    }));
    {
        std::unique_lock<std::mutex> lock(doneMutex);
        Check(doneCondition.wait_for(lock, std::chrono::seconds(5), [&] {
            return done;
        }));
    }
    Check(gCompletionLengths == std::vector<std::uint64_t>({1, 2, 3}));
    Check(gCompleteBytes == 6 && fixture.Relay->FallbackReceiveHead == nullptr);
    Check(fixture.Relay->FailActiveReceiveQueueAdmissionsForTest == 0);
    Check(fixture.Relay->ActiveReceiveFallbackMode);
    Check(fixture.Relay->AdmittedQuicReceiveBytes == 0);
    Check(fixture.Relay->QuicToTcpPressureBytes == 0);
    Check(fixture.Worker.StopForTest());
}

void TestTerminalWinningDuringCommandAdmissionSettlesPendingView() {
    RelayFixture fixture;
    fixture.Relay->Worker = &fixture.Worker;
    Check(fixture.Worker.StartAndWaitReady());
    gTerminalOnAdmission.store(fixture.Relay.get());
    std::uint8_t data[]{9, 8, 7};
    std::vector<QUIC_BUFFER> buffers{{3, data}};
    Check(fixture.Receive(buffers, true) == QUIC_STATUS_PENDING);
    std::unique_lock<std::mutex> lock(gIoMutex);
    Check(gIoCondition.wait_for(lock, std::chrono::seconds(5), [] {
        return gCompleteCalls.load() == 1;
    }));
    lock.unlock();
    Check(gCompleteBytes == 3);
    Check(fixture.Relay->AdmittedQuicReceiveBytes == 0);
    Check(gWriteRequest == nullptr);
    Check(fixture.Worker.StopForTest());
}

void TestQueueFullRollsBackSecondReceiveReservation() {
    gQueueCapacity.store(1);
    RelayFixture fixture;
    gQueueCapacity.store(4096);
    fixture.Relay->Worker = &fixture.Worker;
    Check(fixture.Worker.StartAndWaitReady());
    std::mutex blockMutex;
    std::condition_variable blockCondition;
    bool entered = false;
    bool release = false;
    Check(fixture.Worker.Post([&](TqUvRelayWorker&) {
        std::unique_lock<std::mutex> lock(blockMutex);
        entered = true;
        blockCondition.notify_all();
        blockCondition.wait(lock, [&] { return release; });
    }));
    {
        std::unique_lock<std::mutex> lock(blockMutex);
        Check(blockCondition.wait_for(lock, std::chrono::seconds(5), [&] {
            return entered;
        }));
    }
    std::uint8_t first[]{1, 2, 3};
    std::uint8_t second[]{4, 5, 6};
    std::vector<QUIC_BUFFER> firstBuffers{{3, first}};
    std::vector<QUIC_BUFFER> secondBuffers{{3, second}};
    Check(fixture.Receive(firstBuffers, true) == QUIC_STATUS_PENDING);
    Check(fixture.Receive(secondBuffers, true) == QUIC_STATUS_OUT_OF_MEMORY);
    Check(fixture.Relay->AdmittedQuicReceiveBytes == 3);
    {
        std::lock_guard<std::mutex> guard(blockMutex);
        release = true;
        blockCondition.notify_all();
    }
    Check(WaitForWrite());
    Check(fixture.Worker.Post([](TqUvRelayWorker&) { CompleteWrite(0); }));
    std::unique_lock<std::mutex> lock(gIoMutex);
    Check(gIoCondition.wait_for(lock, std::chrono::seconds(5), [] {
        return gCompleteCalls.load() == 1;
    }));
    lock.unlock();
    Check(fixture.Relay->AdmittedQuicReceiveBytes == 0);
    Check(fixture.Worker.StopForTest());
}

void TestAsyncWakeFailureUsesSafetyTimerWithoutLosingReceive() {
    RelayFixture fixture;
    fixture.Relay->Worker = &fixture.Worker;
    Check(fixture.Worker.StartAndWaitReady());
    gFailAsyncSend.store(true);
    std::uint8_t data[]{7, 7, 7};
    std::vector<QUIC_BUFFER> buffers{{3, data}};
    Check(fixture.Receive(buffers, true) == QUIC_STATUS_PENDING);
    Check(WaitForWrite());
    gFailAsyncSend.store(false);
    Check(fixture.Worker.Post([](TqUvRelayWorker&) { CompleteWrite(0); }));
    std::unique_lock<std::mutex> lock(gIoMutex);
    Check(gIoCondition.wait_for(lock, std::chrono::seconds(5), [] {
        return gCompleteCalls.load() == 1;
    }));
    lock.unlock();
    Check(fixture.Relay->AdmittedQuicReceiveBytes == 0);
    Check(fixture.Worker.StopForTest());
}

} // namespace

int main() {
    TestActiveZeroLengthFinPublishesDurableFactWithoutOwnership();
    TestPreparedZeroLengthFinSurvivesActivation();
    TestDataAndFinPublishesFinOnlyAfterPayloadWriteCompletes();
    TestZeroLengthFinLosingTerminalRaceTakesNoOwnership();
    TestUncompressedSlicesCompleteOnlyAfterWrite();
    TestTerminalWriteErrorSettlesOnlyOnce();
    TestDecompressedInputCompletesBeforeOutputWrite();
    TestBackpressureUsesHighAndHalfLowWatermarks();
    TestTotalPressureIncludesPendingDecompressedOutput();
    TestDecompressionAllocationFailureKeepsInputOwned();
    TestReceiveCompletionLeaseFailureRetriesWithoutRewritingTcp();
    TestTerminalShutdownSettlesCancelledReceiveWithoutApiLease();
    TestCommandAdmissionFailureRollsBackPendingReceive();
    TestNullWorkerAndTerminalDoNotTakeReceiveOwnership();
    TestZeroOutputProgressContinuesAcrossSlices();
    TestCompressedReceiveDrainsBufferedOutputAfterInputCompletion();
    TestDrainedCompressedReceiveAdvancesQueuedReceive();
    TestCompressedReceiveBudgetYieldsToQueuedMarker();
    TestCompressedReceiveDeferredEnqueueFailureIsTerminal();
    TestCompressedReceiveCapsLargeSliceBeforeByteBudget();
    TestCompressedLeaseRetryDoesNotRepeatDecompressionOrWrite();
    TestCompressedPartialLeaseFailureThenTerminalSettlesWholeView();
    TestStartedWorkerConcurrentCallbacksAreExactlyOnceAndBounded();
    TestConsecutiveQueueAllocationFailuresRemainExactlyOnceAtTerminal();
    TestTerminalWinningDuringCommandAdmissionSettlesPendingView();
    TestQueueFullRollsBackSecondReceiveReservation();
    TestAsyncWakeFailureUsesSafetyTimerWithoutLosingReceive();
    TestRealZstdDecompressesOneByteSlices();
    TestRealZstdDrainsOutputLargerThanWriteBuffer();
    return 0;
}
