#include "libuv_relay_worker.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <deque>
#include <future>
#include <limits>
#include <thread>
#include <vector>

namespace {

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n",               \
                         __FILE__, __LINE__, #condition);                       \
            std::abort();                                                       \
        }                                                                       \
    } while (false)

struct SendCapture {
    std::uint64_t Calls{0};
    std::vector<std::uint8_t> Payload;
    QUIC_SEND_FLAGS Flags{QUIC_SEND_FLAG_NONE};
    void* Context{nullptr};
};

SendCapture gSend;
std::uint64_t gReadStarts{0};
std::uint64_t gReadStops{0};
TqStreamLifetime* gSynchronousCompletionOwner{nullptr};
std::deque<QUIC_STATUS> gSendStatuses;
std::uint64_t gTerminalHandoffs{0};
TqUvTerminalTrigger gLastTerminalTrigger{TqUvTerminalTrigger::RuntimeStop};
std::uint64_t gConvergenceChecks{0};
std::atomic<std::uint64_t> gWrongTerminalThread{0};
std::atomic<std::uint64_t> gWrongConvergenceThread{0};
std::atomic<std::uint64_t> gDrainFaults{0};
std::atomic<bool> gFailNextQueuePop{false};
std::vector<void*> gSendContexts;

template <typename Predicate>
bool WaitUntil(Predicate predicate, std::uint32_t timeoutMs = 2000) {
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
    CHECK(WaitUntil([&] {
        return worker.Post([&](TqUvRelayWorker& local) {
            CHECK(local.IsLoopThreadForTest());
            function(local);
            completed.set_value();
        });
    }));
    CHECK(ready.wait_for(std::chrono::seconds(2)) ==
          std::future_status::ready);
}

void FailQueuePopOnce() {
    if (gFailNextQueuePop.exchange(false, std::memory_order_acq_rel)) {
        gDrainFaults.fetch_add(1, std::memory_order_relaxed);
        throw std::bad_alloc{};
    }
}

class CompletionTarget final : public TqStreamLifetime::Target {
public:
    QUIC_STATUS OnStreamEvent(
        MsQuicStream*, QUIC_STREAM_EVENT* event, std::uint64_t) noexcept override {
        if (event != nullptr && event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
            auto relay = Relay.lock();
            if (relay && Worker != nullptr) {
                auto* operation = static_cast<TqUvQuicSendOperation*>(
                    event->SEND_COMPLETE.ClientContext);
                TqUvHandleSendComplete(
                    *Worker, *operation, event->SEND_COMPLETE.Canceled != 0);
            }
        }
        return QUIC_STATUS_SUCCESS;
    }

    TqUvRelayWorker* Worker{nullptr};
    std::weak_ptr<TqUvRelayState> Relay;
};

QUIC_STATUS CaptureSend(
    MsQuicStream*,
    const QUIC_BUFFER* buffers,
    std::uint32_t count,
    QUIC_SEND_FLAGS flags,
    void* context) {
    ++gSend.Calls;
    gSend.Flags = flags;
    gSend.Context = context;
    gSendContexts.push_back(context);
    gSend.Payload.clear();
    for (std::uint32_t index = 0; index < count; ++index) {
        gSend.Payload.insert(
            gSend.Payload.end(),
            buffers[index].Buffer,
            buffers[index].Buffer + buffers[index].Length);
    }
    if (gSynchronousCompletionOwner != nullptr) {
        auto* owner = gSynchronousCompletionOwner;
        std::thread callback([owner, context]() {
            QUIC_STREAM_EVENT event{};
            event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
            event.SEND_COMPLETE.ClientContext = context;
            (void)owner->DispatchForTest(&event);
        });
        callback.join();
    }
    if (gSendStatuses.empty()) {
        return QUIC_STATUS_SUCCESS;
    }
    const auto status = gSendStatuses.front();
    gSendStatuses.pop_front();
    return status;
}

int ReadStart(uv_stream_t*, uv_alloc_cb, uv_read_cb) {
    ++gReadStarts;
    return 0;
}

int ReadStop(uv_stream_t*) {
    ++gReadStops;
    return 0;
}

void NoopClose(uv_handle_t* handle, uv_close_cb callback) {
    if (handle->type == UV_UNKNOWN_HANDLE) {
        callback(handle);
        return;
    }
    uv_close(handle, callback);
}

int TcpOpenSuccess(uv_tcp_t*, uv_os_sock_t) { return 0; }
int AsyncSendFailure(uv_async_t*) { return UV_EIO; }

TqUvCallAdapter Calls() {
    auto calls = TqUvProductionCalls();
    calls.ReadStart = &ReadStart;
    calls.ReadStop = &ReadStop;
    calls.Close = &NoopClose;
    return calls;
}

std::shared_ptr<TqUvRelayState> MakeRelay(
    TqUvRelayWorker& worker,
    ITqCompressor* compressor = nullptr,
    TqCompressAlgo algorithm = TqCompressAlgo::None) {
    auto relay = std::make_shared<TqUvRelayState>();
    relay->Worker = &worker;
    relay->RelayId = 41;
    relay->RouteGeneration = 7;
    relay->ControlGeneration = 9;
    relay->Stream = reinterpret_cast<MsQuicStream*>(0x1);
    auto target = std::make_shared<TqUvStreamBinding>();
    CHECK(relay->ActivationMutex.Initialize());
    target->Worker = &worker;
    target->Relay = relay;
    target->RelayId = relay->RelayId;
    target->ControlGeneration = relay->ControlGeneration;
    target->Activation.store(TqUvActivation::Active);
    relay->Binding = target;
    relay->Activation = TqUvActivation::Active;
    relay->StreamOwner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::Started, target);
    relay->RouteGeneration = relay->StreamOwner->RouteGeneration();
    target->RouteGeneration = relay->RouteGeneration;
    relay->Compressor = compressor;
    relay->CompressAlgo = algorithm;
    relay->TcpHandle.data = relay.get();
    relay->TcpHandleInitialized = true;
    relay->TcpReadStarted = true;
    relay->TcpReadChunkSize = 128;
    relay->TcpReadBufferBudget.MaxPendingBufferBytes = 1024 * 1024;
    return relay;
}

void TerminalHook(TqUvRelayState& relay, TqUvTerminalTrigger trigger) {
    if (relay.Worker != nullptr && relay.Worker->Snapshot().Running &&
        !relay.Worker->IsLoopThreadForTest()) {
        gWrongTerminalThread.fetch_add(1, std::memory_order_relaxed);
    }
    gLastTerminalTrigger = trigger;
    ++gTerminalHandoffs;
}

void ConvergenceHook(TqUvRelayState& relay) {
    if (relay.Worker != nullptr && relay.Worker->Snapshot().Running &&
        !relay.Worker->IsLoopThreadForTest()) {
        gWrongConvergenceThread.fetch_add(1, std::memory_order_relaxed);
    }
    ++gConvergenceChecks;
}

uv_buf_t ReadBuffer(TqUvRelayState& relay, const std::vector<std::uint8_t>& data) {
    auto buffer = TqUvStageTcpReadBufferForTest(relay, data.size());
    CHECK(buffer.base != nullptr);
    std::memcpy(buffer.base, data.data(), data.size());
    return buffer;
}

void Complete(TqUvRelayWorker& worker, TqUvRelayState& relay) {
    CHECK(gSend.Context != nullptr);
    CHECK(relay.QuicSends.size() == 1);
    auto found = relay.QuicSends.begin();
    (void)relay.StreamOwner->CancelSendCompletion(gSend.Context);
    TqUvHandleSendComplete(worker, *found->second, false);
}

class RecordingCompressor final : public ITqCompressor {
public:
    bool Compress(
        const std::uint8_t* input,
        std::size_t length,
        std::vector<std::uint8_t>& output,
        bool endStream) override {
        ++CompressCalls;
        if (endStream) {
            ++EndCalls;
            output.push_back(0xfe);
            return true;
        }
        output.push_back(0xc0);
        output.insert(output.end(), input, input + length);
        return true;
    }
    bool Flush(std::vector<std::uint8_t>&) override { return true; }
    void Reset() override {}

    std::uint64_t CompressCalls{0};
    std::uint64_t EndCalls{0};
};

class BufferingCompressor final : public ITqCompressor {
public:
    bool Compress(
        const std::uint8_t*,
        std::size_t,
        std::vector<std::uint8_t>&,
        bool) override {
        ++CompressCalls;
        return true;
    }
    bool Flush(std::vector<std::uint8_t>& output) override {
        ++FlushCalls;
        output = {0xd1, 0xd2};
        return true;
    }
    void Reset() override {}

    std::uint64_t CompressCalls{0};
    std::uint64_t FlushCalls{0};
};

class EmptyBufferingCompressor final : public ITqCompressor {
public:
    bool Compress(
        const std::uint8_t*, std::size_t, std::vector<std::uint8_t>&,
        bool) override {
        ++CompressCalls;
        return true;
    }
    bool Flush(std::vector<std::uint8_t>&) override {
        ++FlushCalls;
        return true;
    }
    void Reset() override {}

    std::uint64_t CompressCalls{0};
    std::uint64_t FlushCalls{0};
};

void TestOneReadIsOneSendAndOwnerLivesToCompletion() {
    auto calls = Calls();
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{.Calls = &calls});
    auto relay = MakeRelay(worker);
    const auto before = relay->TcpReadBufferBudget.PendingBufferBytes.load();
    const std::vector<std::uint8_t> bytes{1, 2, 3, 4};
    auto buffer = ReadBuffer(*relay, bytes);

    TqUvHandleTcpRead(worker, *relay, 4, buffer);

    CHECK(gSend.Calls == 1);
    CHECK(gSend.Payload == bytes);
    CHECK(relay->QuicSends.size() == 1);
    CHECK(relay->PendingQuicSendBytes == 4);
    CHECK(worker.Snapshot().TcpToQuicPendingBytes == 4);
    CHECK(worker.Snapshot().TcpReadBytes == 4);
    CHECK(worker.Snapshot().CompressedTcpBytes == 0);
    CHECK(relay->TcpReadBufferBudget.PendingBufferBytes.load() > before);

    Complete(worker, *relay);
    CHECK(relay->QuicSends.empty());
    CHECK(relay->PendingQuicSendBytes == 0);
    CHECK(worker.Snapshot().TcpToQuicPendingBytes == 0);
    CHECK(relay->TcpReadBufferBudget.PendingBufferBytes.load() == before);
}

void TestCompressionReleasesInputAndRetainsOutput() {
    RecordingCompressor compressor;
    auto calls = Calls();
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{.Calls = &calls});
    auto relay = MakeRelay(worker, &compressor, TqCompressAlgo::Zstd);
    auto buffer = ReadBuffer(*relay, {5, 6, 7});

    TqUvHandleTcpRead(worker, *relay, 3, buffer);

    CHECK(compressor.CompressCalls == 1);
    CHECK((gSend.Payload == std::vector<std::uint8_t>{0xc0, 5, 6, 7}));
    CHECK(relay->TcpReadBuffers.empty());
    CHECK(relay->QuicSends.size() == 1);
    const auto metrics = worker.Snapshot();
    CHECK(metrics.TcpReadBytes == 3);
    CHECK(metrics.CompressedTcpBytes == 4);
    CHECK(metrics.TcpToQuicCompressFailures == 0);
    CHECK(relay->TcpReadBufferBudget.PendingBufferBytes.load() != 0);
    Complete(worker, *relay);
    CHECK(relay->TcpReadBufferBudget.PendingBufferBytes.load() == 0);
}

void TestBufferedCompressionFlushesBeforeSubmittingRead() {
    BufferingCompressor compressor;
    auto calls = Calls();
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{.Calls = &calls});
    auto relay = MakeRelay(worker, &compressor, TqCompressAlgo::Zstd);
    auto buffer = ReadBuffer(*relay, {5, 6, 7});

    TqUvHandleTcpRead(worker, *relay, 3, buffer);

    CHECK(compressor.CompressCalls == 1);
    CHECK(compressor.FlushCalls == 1);
    CHECK(gSend.Calls == 1);
    CHECK((gSend.Payload == std::vector<std::uint8_t>{0xd1, 0xd2}));
    Complete(worker, *relay);
}

void TestBufferedCompressionWithEmptyFlushDoesNotSubmitEmptySend() {
    EmptyBufferingCompressor compressor;
    auto calls = Calls();
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{.Calls = &calls});
    auto relay = MakeRelay(worker, &compressor, TqCompressAlgo::Zstd);
    auto buffer = ReadBuffer(*relay, {5, 6, 7});

    TqUvHandleTcpRead(worker, *relay, 3, buffer);

    CHECK(compressor.CompressCalls == 1);
    CHECK(compressor.FlushCalls == 1);
    CHECK(gSend.Calls == 0);
    CHECK(relay->QuicSends.empty());
    CHECK(relay->TerminalTriggerMask.load(std::memory_order_acquire) == 0);
}

void TestBackpressureStopsAndCompletionResumesRead() {
    auto calls = Calls();
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{.Calls = &calls});
    auto relay = MakeRelay(worker);
    relay->MaxBufferedQuicSendBytes = 4;
    relay->ResumeBufferedQuicSendBytes = 1;
    auto buffer = ReadBuffer(*relay, {1, 2, 3, 4});

    TqUvHandleTcpRead(worker, *relay, 4, buffer);
    CHECK(gReadStops == 1);
    CHECK(relay->TcpReadPausedByQuicBacklog);
    CHECK(!relay->TcpReadStarted);

    Complete(worker, *relay);
    CHECK(gReadStarts == 1);
    CHECK(!relay->TcpReadPausedByQuicBacklog);
    CHECK(relay->TcpReadStarted);
}

void TestEofFlushesCompressorAndSubmitsFin() {
    RecordingCompressor compressor;
    auto calls = Calls();
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{.Calls = &calls});
    auto relay = MakeRelay(worker, &compressor, TqCompressAlgo::Zstd);
    uv_buf_t empty = uv_buf_init(nullptr, 0);

    TqUvHandleTcpRead(worker, *relay, UV_EOF, empty);

    CHECK(relay->TcpReadClosed);
    CHECK(!relay->TcpReadStarted);
    CHECK(compressor.EndCalls == 1);
    CHECK(gSend.Flags == QUIC_SEND_FLAG_FIN);
    CHECK((gSend.Payload == std::vector<std::uint8_t>{0xfe}));
    CHECK(relay->QuicFinSubmitted);
    Complete(worker, *relay);
    CHECK(relay->QuicFinCompleted);
}

void TestStaleCompletionOnlySettlesItsOriginalRelay() {
    auto calls = Calls();
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{.Calls = &calls});
    auto oldRelay = MakeRelay(worker);
    auto buffer = ReadBuffer(*oldRelay, {8, 9});
    TqUvHandleTcpRead(worker, *oldRelay, 2, buffer);
    CHECK(oldRelay->QuicSends.size() == 1);
    auto* operation = oldRelay->QuicSends.begin()->second.get();
    (void)oldRelay->StreamOwner->CancelSendCompletion(gSend.Context);
    ++oldRelay->RouteGeneration;

    auto replacement = MakeRelay(worker);
    replacement->RelayId = oldRelay->RelayId;
    replacement->RouteGeneration = oldRelay->RouteGeneration;
    replacement->PendingQuicSendBytes = 77;
    TqUvHandleSendComplete(worker, *operation, false);

    CHECK(oldRelay->QuicSends.empty());
    CHECK(replacement->PendingQuicSendBytes == 77);
}

void TestSynchronousAndDuplicateCompletionReleaseExactlyOnce() {
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{});
    CHECK(worker.StartAndWaitReady());
    auto relay = MakeRelay(worker);
    const auto duplicatesBefore =
        TqStreamLifetime::SnapshotSendCompletions().DuplicateClaims;
    const auto terminalBefore = gTerminalHandoffs;
    const auto wrongThreadBefore =
        gWrongTerminalThread.load(std::memory_order_relaxed);
    gSendStatuses = {QUIC_STATUS_ABORTED};
    gSynchronousCompletionOwner = relay->StreamOwner.get();
    RunOnLoop(worker, [&](TqUvRelayWorker& local) {
        auto buffer = ReadBuffer(*relay, {3, 4, 5});
        TqUvHandleTcpRead(local, *relay, 3, buffer);
        // The callback has returned, but the loop cannot drain its command
        // until this loop-local send downcall returns.
        CHECK(relay->QuicSends.size() == 1);
    });

    gSynchronousCompletionOwner = nullptr;
    for (std::uint32_t attempt = 0;
         attempt < 10000 && worker.Snapshot().SendCompletionCommands == 0;
         ++attempt) {
        std::this_thread::yield();
    }
    const auto completionSnapshot = worker.Snapshot();
    if (completionSnapshot.SendCompletionCommands != 1) {
        std::fprintf(
            stderr,
            "completion commands=%llu fallbacks=%llu wakes=%llu failures=%llu\n",
            static_cast<unsigned long long>(
                completionSnapshot.SendCompletionCommands),
            static_cast<unsigned long long>(
                completionSnapshot.SendCompletionFallbacks),
            static_cast<unsigned long long>(completionSnapshot.AsyncWakeAttempts),
            static_cast<unsigned long long>(completionSnapshot.AsyncWakeFailures));
    }
    CHECK(completionSnapshot.SendCompletionCommands == 1);
    RunOnLoop(worker, [&](TqUvRelayWorker&) {
        CHECK(relay->QuicSends.empty());
        CHECK(relay->PendingQuicSendBytes == 0);
        CHECK(relay->TcpReadBufferBudget.PendingBufferBytes.load() == 0);
    });
    CHECK(gTerminalHandoffs == terminalBefore);
    CHECK(gWrongTerminalThread.load(std::memory_order_relaxed) ==
          wrongThreadBefore);
    QUIC_STREAM_EVENT duplicate{};
    duplicate.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    duplicate.SEND_COMPLETE.ClientContext = gSend.Context;
    (void)relay->StreamOwner->DispatchForTest(&duplicate);
    RunOnLoop(worker, [&](TqUvRelayWorker&) {
        CHECK(relay->PendingQuicSendBytes == 0);
    });
    CHECK(TqStreamLifetime::SnapshotSendCompletions().DuplicateClaims ==
           duplicatesBefore + 1);
    CHECK(worker.StopForTest());
}

void TestReservationAndFatalSubmitFailuresRollbackOwnership() {
    auto calls = Calls();
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{.Calls = &calls});
    auto relay = MakeRelay(worker);
    const auto terminalBefore = gTerminalHandoffs;
    TqStreamLifetime::SetFailNextRegisterSendCompletionForTest(true);
    auto reserveFailure = ReadBuffer(*relay, {1, 2});
    TqUvHandleTcpRead(worker, *relay, 2, reserveFailure);
    CHECK(relay->QuicSends.empty());
    CHECK(relay->PendingQuicSendBytes == 0);
    CHECK(relay->TcpReadBufferBudget.PendingBufferBytes.load() == 0);
    CHECK(gTerminalHandoffs == terminalBefore + 1);

    auto fatalRelay = MakeRelay(worker);
    gSendStatuses = {QUIC_STATUS_ABORTED};
    auto fatalBuffer = ReadBuffer(*fatalRelay, {3, 4});
    TqUvHandleTcpRead(worker, *fatalRelay, 2, fatalBuffer);
    CHECK(fatalRelay->QuicSends.empty());
    CHECK(fatalRelay->PendingQuicSendBytes == 0);
    CHECK(fatalRelay->TcpReadBufferBudget.PendingBufferBytes.load() == 0);
    CHECK(gTerminalHandoffs == terminalBefore + 2);
}

void TestRegistrationCarriesProductionTuningIntoAllocCallback() {
    auto calls = Calls();
    calls.TcpOpen = &TcpOpenSuccess;
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{.Calls = &calls});
    CHECK(worker.StartAndWaitReady());
    auto initial = std::make_shared<CompletionTarget>();
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::Started, initial);
    auto control = std::make_shared<TqRelayStopControl>();
    TqUvRelayRegistration registration{};
    registration.TcpSocket = static_cast<TqSocketHandle>(17);
    registration.Stream = reinterpret_cast<MsQuicStream*>(0x1);
    registration.StreamOwner = owner;
    registration.StopControl = control;
    registration.ControlGeneration = control->Generation;
    registration.TcpReadChunkSize = 4096;
    registration.MaxPendingBufferBytes = 32768;
    registration.MaxBufferedQuicSendBytes = 16384;
    registration.ResumeBufferedQuicSendBytes = 8192;
    registration.PrecommitMaxPendingBytes = 12288;

    const auto result = worker.RegisterRelayWithId(std::move(registration));
    CHECK(result.Ok);
    auto relay = worker.RelayForTest(result.RelayId);
    CHECK(relay != nullptr);
    CHECK(relay->TcpReadChunkSize == 4096);
    CHECK(relay->TcpReadBufferBudget.MaxPendingBufferBytes == 32768);
    CHECK(relay->MaxBufferedQuicSendBytes == 16384);
    CHECK(relay->ResumeBufferedQuicSendBytes == 8192);
    uv_buf_t allocated{};
    TqUvOnTcpAlloc(
        reinterpret_cast<uv_handle_t*>(&relay->TcpHandle), 65536, &allocated);
    CHECK(allocated.base != nullptr);
    CHECK(allocated.len == 4096);
    TqUvOnTcpRead(
        reinterpret_cast<uv_stream_t*>(&relay->TcpHandle), 0, &allocated);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    CHECK(worker.StopForTest());
}

void TestQueueFullAndWakeFailureUseReliableCompletionFallback() {
    const auto wrongThreadBefore =
        gWrongTerminalThread.load(std::memory_order_relaxed);
    auto calls = Calls();
    calls.AsyncSend = &AsyncSendFailure;
    TqUvRelayWorkerConfig config{};
    config.Calls = &calls;
    config.QueueCapacity = 1;
    TqUvRelayWorker worker(config);
    CHECK(worker.StartAndWaitReady());
    auto relay = MakeRelay(worker);
    RunOnLoop(worker, [&](TqUvRelayWorker& local) {
        auto buffer = ReadBuffer(*relay, {9, 8, 7});
        TqUvHandleTcpRead(local, *relay, 3, buffer);
        CHECK(relay->QuicSends.size() == 1);
    });
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    CHECK(worker.Post([&](TqUvRelayWorker&) {
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }));
    CHECK(WaitUntil([&]() { return entered.load(std::memory_order_acquire); }));
    CHECK(worker.Post([](TqUvRelayWorker&) {}));

    QUIC_STREAM_EVENT completion{};
    completion.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    completion.SEND_COMPLETE.ClientContext = gSend.Context;
    std::thread callback([&]() {
        (void)relay->StreamOwner->DispatchForTest(&completion);
    });
    callback.join();
    CHECK(worker.Snapshot().SendCompletionFallbacks == 1);
    CHECK(worker.Snapshot().AsyncWakeFailures != 0);
    release.store(true, std::memory_order_release);
    CHECK(WaitUntil([&]() {
        return worker.Snapshot().SendCompletionCommands == 1;
    }));
    RunOnLoop(worker, [&](TqUvRelayWorker& local) {
        CHECK(relay->QuicSends.empty());
        CHECK(relay->PendingQuicSendBytes == 0);
        TqUvRequestTerminal(*relay, TqUvTerminalTrigger::RuntimeStop);
        TqUvProcessTerminalFactsLocal(local, *relay);
        CHECK(relay->TerminalStarted);
    });
    CHECK(gWrongTerminalThread.load(std::memory_order_relaxed) ==
          wrongThreadBefore);
    CHECK(worker.StopForTest());
}

void TestDrainFaultWithQueuedAndFallbackCompletionsIsExactlyOnce() {
    const auto wrongConvergenceBefore =
        gWrongConvergenceThread.load(std::memory_order_relaxed);
    TqUvRelayWorkerConfig config{};
    config.QueueCapacity = 2;
    config.BeforeQueuePopForTest = &FailQueuePopOnce;
    TqUvRelayWorker worker(config);
    CHECK(worker.StartAndWaitReady());
    auto first = MakeRelay(worker);
    auto second = MakeRelay(worker);
    auto third = MakeRelay(worker);
    const auto contextBase = gSendContexts.size();
    RunOnLoop(worker, [&](TqUvRelayWorker& local) {
        auto firstBuffer = ReadBuffer(*first, {1});
        TqUvHandleTcpRead(local, *first, 1, firstBuffer);
        auto secondBuffer = ReadBuffer(*second, {2, 3});
        TqUvHandleTcpRead(local, *second, 2, secondBuffer);
        uv_buf_t empty = uv_buf_init(nullptr, 0);
        TqUvHandleTcpRead(local, *third, UV_EOF, empty);
    });
    CHECK(gSendContexts.size() == contextBase + 3);
    const std::vector<void*> contexts{
        gSendContexts[contextBase],
        gSendContexts[contextBase + 1],
        gSendContexts[contextBase + 2]};

    std::atomic<bool> blockerEntered{false};
    std::atomic<bool> releaseBlocker{false};
    CHECK(worker.Post([&](TqUvRelayWorker&) {
        blockerEntered.store(true, std::memory_order_release);
        while (!releaseBlocker.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }));
    CHECK(WaitUntil([&] {
        return blockerEntered.load(std::memory_order_acquire);
    }));

    gFailNextQueuePop.store(true, std::memory_order_release);
    std::thread callback([&] {
        const std::shared_ptr<TqUvRelayState> relays[]{first, second, third};
        for (std::size_t index = 0; index < 3; ++index) {
            QUIC_STREAM_EVENT completion{};
            completion.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
            completion.SEND_COMPLETE.ClientContext = contexts[index];
            (void)relays[index]->StreamOwner->DispatchForTest(&completion);
        }
    });
    callback.join();
    CHECK(worker.Snapshot().SendCompletionFallbacks == 1);
    releaseBlocker.store(true, std::memory_order_release);
    CHECK(WaitUntil([&] {
        return worker.Snapshot().SendCompletionCommands == 3;
    }));
    CHECK(gDrainFaults.load(std::memory_order_relaxed) != 0);
    CHECK(gWrongConvergenceThread.load(std::memory_order_relaxed) ==
          wrongConvergenceBefore);
    RunOnLoop(worker, [&](TqUvRelayWorker&) {
        for (const auto& relay : {first, second, third}) {
            CHECK(relay->QuicSends.empty());
            CHECK(relay->PendingQuicSendBytes == 0);
            CHECK(relay->TcpReadBufferBudget.PendingBufferBytes.load() == 0);
        }
        CHECK(third->QuicFinCompleted);
    });

    const auto duplicatesBefore =
        TqStreamLifetime::SnapshotSendCompletions().DuplicateClaims;
    for (std::size_t index = 0; index < 3; ++index) {
        QUIC_STREAM_EVENT duplicate{};
        duplicate.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
        duplicate.SEND_COMPLETE.ClientContext = contexts[index];
        const std::shared_ptr<TqUvRelayState> relays[]{first, second, third};
        (void)relays[index]->StreamOwner->DispatchForTest(&duplicate);
    }
    CHECK(TqStreamLifetime::SnapshotSendCompletions().DuplicateClaims ==
          duplicatesBefore + 3);
    CHECK(worker.StopForTest());
}

void TestProductionAllocCallbackUsesRelayBudgetAndReadChunk() {
    auto calls = Calls();
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{.Calls = &calls});
    auto relay = MakeRelay(worker);
    relay->TcpReadChunkSize = 8;
    relay->TcpReadBufferBudget.MaxPendingBufferBytes = 8;
    uv_buf_t buffer{};

    TqUvOnTcpAlloc(
        reinterpret_cast<uv_handle_t*>(&relay->TcpHandle), 64, &buffer);

    CHECK(buffer.base != nullptr);
    CHECK(buffer.len == 8);
    CHECK(relay->TcpReadBuffers.size() == 1);
    TqUvOnTcpRead(
        reinterpret_cast<uv_stream_t*>(&relay->TcpHandle), 3, &buffer);
    CHECK(gSend.Calls == 1);
    Complete(worker, *relay);
}

void TestReadBudgetPressurePausesAfterEnobufsAndResumesAfterCompletion() {
    auto calls = Calls();
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{.Calls = &calls});
    auto relay = MakeRelay(worker);
    relay->TcpReadChunkSize = 8;
    relay->TcpReadBufferBudget.MaxPendingBufferBytes = 16;
    relay->MaxBufferedQuicSendBytes = 16;
    relay->ResumeBufferedQuicSendBytes = 4;
    auto firstBuffer = TqUvStageTcpReadBufferForTest(*relay, 8);
    CHECK(firstBuffer.base != nullptr);
    const std::uint8_t payload[]{1, 2, 3};
    std::memcpy(firstBuffer.base, payload, sizeof(payload));

    TqUvHandleTcpRead(worker, *relay, 3, firstBuffer);
    CHECK(relay->QuicSends.size() == 1);
    auto* firstOperation = relay->QuicSends.begin()->second.get();
    void* firstContext = gSend.Context;
    auto secondBuffer = TqUvStageTcpReadBufferForTest(*relay, 8);
    CHECK(secondBuffer.base != nullptr);
    std::memcpy(secondBuffer.base, payload, sizeof(payload));

    TqUvHandleTcpRead(worker, *relay, 3, secondBuffer);
    CHECK(relay->QuicSends.size() == 2);
    TqUvQuicSendOperation* secondOperation = nullptr;
    for (const auto& [key, operation] : relay->QuicSends) {
        (void)key;
        if (operation.get() != firstOperation) {
            secondOperation = operation.get();
        }
    }
    CHECK(secondOperation != nullptr);
    void* secondContext = gSend.Context;

    CHECK(relay->PendingQuicSendBytes == 6);
    CHECK(relay->TcpReadBufferBudget.PendingBufferBytes.load() == 16);
    const auto terminalBefore = gTerminalHandoffs;
    const auto readStopsBefore = gReadStops;
    const auto readStartsBefore = gReadStarts;
    uv_buf_t exhausted{};
    TqUvOnTcpAlloc(
        reinterpret_cast<uv_handle_t*>(&relay->TcpHandle), 8, &exhausted);
    CHECK(exhausted.base == nullptr);
    CHECK(exhausted.len == 0);
    CHECK(relay->TcpReadAcquireFailure ==
          TqBufferAcquireFailure::PendingBytesLimit);
    CHECK(gTerminalHandoffs == terminalBefore);
    CHECK(gReadStops == readStopsBefore);

    TqUvOnTcpRead(
        reinterpret_cast<uv_stream_t*>(&relay->TcpHandle),
        UV_ENOBUFS,
        &exhausted);

    CHECK(gReadStops == readStopsBefore + 1);
    CHECK(relay->TcpReadPausedByQuicBacklog);
    CHECK(!relay->TcpReadStarted);
    CHECK(gTerminalHandoffs == terminalBefore);

    CHECK(relay->StreamOwner->CancelSendCompletion(firstContext));
    TqUvHandleSendComplete(worker, *firstOperation, false);
    CHECK(relay->TcpReadBufferBudget.PendingBufferBytes.load() == 8);
    CHECK(relay->PendingQuicSendBytes == 3);
    CHECK(gReadStarts == readStartsBefore);
    CHECK(relay->TcpReadPausedByQuicBacklog);
    CHECK(!relay->TcpReadStarted);

    CHECK(relay->StreamOwner->CancelSendCompletion(secondContext));
    TqUvHandleSendComplete(worker, *secondOperation, false);
    CHECK(relay->TcpReadBufferBudget.PendingBufferBytes.load() == 0);
    CHECK(relay->PendingQuicSendBytes == 0);
    CHECK(gReadStarts == readStartsBefore + 1);
    CHECK(relay->TcpReadStarted);
    CHECK(!relay->TcpReadPausedByQuicBacklog);
}

void TestTrueReadAllocationFailureTerminatesFromReadCallback() {
    auto calls = Calls();
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{.Calls = &calls});
    auto relay = MakeRelay(worker);
    relay->TcpReadChunkSize = std::numeric_limits<std::size_t>::max();
    relay->TcpReadBufferBudget.MaxPendingBufferBytes =
        std::numeric_limits<std::uint64_t>::max();
    const auto terminalBefore = gTerminalHandoffs;
    const auto readStopsBefore = gReadStops;
    uv_buf_t allocationFailure{};

    TqUvOnTcpAlloc(
        reinterpret_cast<uv_handle_t*>(&relay->TcpHandle),
        std::numeric_limits<std::size_t>::max(),
        &allocationFailure);

    CHECK(allocationFailure.base == nullptr);
    CHECK(allocationFailure.len == 0);
    CHECK(relay->TcpReadAcquireFailure ==
          TqBufferAcquireFailure::AllocationFailure);
    CHECK(relay->TcpReadBufferBudget.PendingBufferBytes.load() == 0);
    CHECK(gTerminalHandoffs == terminalBefore);
    CHECK(gReadStops == readStopsBefore);
    uv_buf_t empty{};

    TqUvOnTcpRead(
        reinterpret_cast<uv_stream_t*>(&relay->TcpHandle),
        UV_ENOBUFS,
        &empty);

    CHECK(gTerminalHandoffs == terminalBefore + 1);
    CHECK(gLastTerminalTrigger ==
          TqUvTerminalTrigger::AllocationFailure);
    CHECK(relay->TerminalBeginCount.load(std::memory_order_acquire) == 1);

    TqUvOnTcpRead(
        reinterpret_cast<uv_stream_t*>(&relay->TcpHandle),
        UV_ENOBUFS,
        &empty);

    CHECK(gTerminalHandoffs == terminalBefore + 1);
    CHECK(relay->TerminalBeginCount.load(std::memory_order_acquire) == 1);
}

void TestResourceFailureRetainsAndRetriesOperation() {
    auto calls = Calls();
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{.Calls = &calls});
    auto relay = MakeRelay(worker);
    gSendStatuses = {QUIC_STATUS_OUT_OF_MEMORY, QUIC_STATUS_SUCCESS};
    auto buffer = ReadBuffer(*relay, {1, 2, 3});

    TqUvHandleTcpRead(worker, *relay, 3, buffer);

    CHECK(relay->PendingQuicSendBytes == 3);
    CHECK(relay->PendingQuicSendRetries.size() == 1);
    CHECK(relay->TcpReadPausedByQuicBacklog);
    CHECK(relay->TcpReadBufferBudget.PendingBufferBytes.load() != 0);
    TqUvRetryPendingQuicSends(worker, *relay);
    CHECK(relay->PendingQuicSendRetries.empty());
    CHECK(relay->QuicSends.size() == 1);
    Complete(worker, *relay);
    CHECK(relay->PendingQuicSendBytes == 0);
    CHECK(relay->TcpReadBufferBudget.PendingBufferBytes.load() == 0);
}

void TestRetryFatalAndCanceledFinUseSingleTerminalHandoff() {
    auto calls = Calls();
    TqUvRelayWorker worker(TqUvRelayWorkerConfig{.Calls = &calls});
    auto relay = MakeRelay(worker);
    const auto terminalBefore = gTerminalHandoffs;
    gSendStatuses = {QUIC_STATUS_OUT_OF_MEMORY, QUIC_STATUS_ABORTED};
    auto buffer = ReadBuffer(*relay, {4, 5});
    TqUvHandleTcpRead(worker, *relay, 2, buffer);
    TqUvRetryPendingQuicSends(worker, *relay);
    CHECK(gTerminalHandoffs == terminalBefore + 1);
    TqUvRequestTerminal(*relay, TqUvTerminalTrigger::TcpError);
    TqUvProcessTerminalFactsLocal(worker, *relay);
    CHECK(gTerminalHandoffs == terminalBefore + 1);

    auto finRelay = MakeRelay(worker);
    gSendStatuses = {QUIC_STATUS_OUT_OF_MEMORY, QUIC_STATUS_SUCCESS};
    uv_buf_t empty = uv_buf_init(nullptr, 0);
    TqUvHandleTcpRead(worker, *finRelay, UV_EOF, empty);
    CHECK(finRelay->PendingQuicSendRetries.size() == 1);
    CHECK(finRelay->PendingQuicSendRetries.front()->Fin);
    TqUvRetryPendingQuicSends(worker, *finRelay);
    CHECK(finRelay->QuicSends.size() == 1);
    const auto finConvergenceBefore = gConvergenceChecks;
    Complete(worker, *finRelay);
    CHECK(finRelay->QuicFinCompleted);
    CHECK(gConvergenceChecks == finConvergenceBefore + 2);

    auto canceledFinRelay = MakeRelay(worker);
    gSend = {};
    TqUvHandleTcpRead(worker, *canceledFinRelay, UV_EOF, empty);
    CHECK(canceledFinRelay->QuicSends.size() == 1);
    auto* operation = canceledFinRelay->QuicSends.begin()->second.get();
    (void)canceledFinRelay->StreamOwner->CancelSendCompletion(gSend.Context);
    const auto cancelConvergenceBefore = gConvergenceChecks;
    TqUvHandleSendComplete(worker, *operation, true);
    CHECK(gTerminalHandoffs == terminalBefore + 2);
    CHECK(gConvergenceChecks == cancelConvergenceBefore + 2);
}

} // namespace

int main() {
    TqUvSetStreamSendHookForTest(&CaptureSend);
    TqUvSetTerminalHookForTest(&TerminalHook);
    TqUvSetConvergenceHookForTest(&ConvergenceHook);
    TestOneReadIsOneSendAndOwnerLivesToCompletion();
    gSend = {};
    TestCompressionReleasesInputAndRetainsOutput();
    gSend = {};
    TestBufferedCompressionFlushesBeforeSubmittingRead();
    gSend = {};
    TestBufferedCompressionWithEmptyFlushDoesNotSubmitEmptySend();
    gSend = {};
    TestBackpressureStopsAndCompletionResumesRead();
    gSend = {};
    TestEofFlushesCompressorAndSubmitsFin();
    gSend = {};
    TestStaleCompletionOnlySettlesItsOriginalRelay();
    gSend = {};
    TestSynchronousAndDuplicateCompletionReleaseExactlyOnce();
    gSend = {};
    TestReservationAndFatalSubmitFailuresRollbackOwnership();
    gSend = {};
    TestRegistrationCarriesProductionTuningIntoAllocCallback();
    gSend = {};
    TestQueueFullAndWakeFailureUseReliableCompletionFallback();
    gSend = {};
    TestDrainFaultWithQueuedAndFallbackCompletionsIsExactlyOnce();
    gSend = {};
    TestProductionAllocCallbackUsesRelayBudgetAndReadChunk();
    gSend = {};
    TestReadBudgetPressurePausesAfterEnobufsAndResumesAfterCompletion();
    gSend = {};
    TestTrueReadAllocationFailureTerminatesFromReadCallback();
    gSend = {};
    TestResourceFailureRetainsAndRetriesOperation();
    gSend = {};
    TestRetryFatalAndCanceledFinUseSingleTerminalHandoff();
    TqUvSetConvergenceHookForTest(nullptr);
    TqUvSetTerminalHookForTest(nullptr);
    TqUvSetStreamSendHookForTest(nullptr);
    std::cout << "libuv relay TCP to QUIC tests passed\n";
    return 0;
}
