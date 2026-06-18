#if defined(__APPLE__)

#include "darwin_relay_worker.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <memory>
#include <chrono>
#include <thread>

namespace {

std::atomic<QUIC_STATUS> g_sendStatus{QUIC_STATUS_SUCCESS};
std::atomic<uint64_t> g_sendCalls{0};
std::atomic<void*> g_lastSendContext{nullptr};
std::atomic<void*> g_syncCallbackContext{nullptr};
std::atomic<bool> g_completeBeforeSendReturns{false};

void CheckImpl(bool condition, int line);
#define CHECK(condition) CheckImpl((condition), __LINE__)

QUIC_STATUS FakeStreamSend(
    MsQuicStream*,
    const QUIC_BUFFER*,
    uint32_t,
    QUIC_SEND_FLAGS,
    void* context) {
    g_sendCalls.fetch_add(1, std::memory_order_relaxed);
    g_lastSendContext.store(context, std::memory_order_release);
    if (g_completeBeforeSendReturns.load(std::memory_order_acquire)) {
        void* callbackContext = g_syncCallbackContext.load(std::memory_order_acquire);
        CHECK(callbackContext != nullptr);
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
        event.SEND_COMPLETE.ClientContext = context;
        CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
    }
    return g_sendStatus.load(std::memory_order_acquire);
}

void ResetFakeStreamSend(QUIC_STATUS status) {
    g_sendStatus.store(status, std::memory_order_release);
    g_sendCalls.store(0, std::memory_order_release);
    g_lastSendContext.store(nullptr, std::memory_order_release);
    g_syncCallbackContext.store(nullptr, std::memory_order_release);
    g_completeBeforeSendReturns.store(false, std::memory_order_release);
}

void CheckImpl(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "check failed at line %d\n", line);
        std::fflush(stderr);
        std::exit(line % 125 + 1);
    }
}

void SocketPairEnvironmentWorks() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(fds[0] != TqInvalidSocket);
    CHECK(fds[1] != TqInvalidSocket);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void WorkerStartsAndStopsCleanly() {
    TqDarwinRelayWorkerConfig config{};
    config.WorkerIndex = 3;
    config.EventBudget = 8;
    config.EventQueueCapacity = 16;

    TqDarwinRelayWorker worker(config);
    CHECK(worker.Start());

    TqDarwinRelayWorkerSnapshot snapshot = worker.Snapshot();
    CHECK(snapshot.Errors == 0);
    CHECK(snapshot.ActiveRelays == 0);
    CHECK(snapshot.TcpReadArmedRelays == 0);
    CHECK(snapshot.TcpWriteArmedRelays == 0);

    worker.Stop();

    snapshot = worker.Snapshot();
    CHECK(snapshot.Errors == 0);
    CHECK(snapshot.ActiveRelays == 0);
}

void WorkerRegistersTcpReadinessShell() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqDarwinRelayWorkerConfig config{};
    TqDarwinRelayWorker worker(config);
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    CHECK(result.RelayId != 0);
    CHECK(handle.Backend == TqRelayBackendType::DarwinWorker);
    CHECK(handle.DarwinWorker == &worker);
    CHECK(handle.DarwinRelayId == result.RelayId);

    TqDarwinRelayWorkerSnapshot snapshot = worker.Snapshot();
    CHECK(snapshot.Errors == 0);
    CHECK(snapshot.ActiveRelays == 1);
    CHECK(snapshot.TcpReadArmedRelays == 1);
    CHECK(snapshot.TcpWriteArmedRelays == 0);

    worker.UnregisterRelay(result.RelayId);
    snapshot = worker.Snapshot();
    CHECK(snapshot.Errors == 0);
    CHECK(snapshot.ActiveRelays == 0);
    CHECK(handle.Backend == TqRelayBackendType::None);
    CHECK(handle.DarwinWorker == nullptr);
    CHECK(handle.DarwinRelayId == 0);

    worker.Stop();
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void WorkerObservesTcpReadBytes() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxBufferedQuicSendBytes = 64 * 1024;

    TqDarwinRelayWorker worker(config);
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "darwin-readiness";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    TqDarwinRelayWorkerSnapshot snapshot{};
    do {
        snapshot = worker.Snapshot();
        if (snapshot.TcpReadBytes >= sizeof(payload) - 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    CHECK(snapshot.TcpReadBytes >= sizeof(payload) - 1);
    CHECK(snapshot.Errors == 0);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void WorkerObservesTcpReadWithSmallByteBudget() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.ByteBudgetPerTick = 8;
    config.MaxBufferedQuicSendBytes = 64 * 1024;

    TqDarwinRelayWorker worker(config);
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "budget";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    TqDarwinRelayWorkerSnapshot snapshot{};
    do {
        snapshot = worker.Snapshot();
        if (snapshot.TcpReadBytes >= sizeof(payload) - 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    CHECK(snapshot.TcpReadBytes >= sizeof(payload) - 1);
    CHECK(snapshot.Errors == 0);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void TransientSendFailureQueuesWithoutSelfRetry() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_OUT_OF_MEMORY);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "transient";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    TqDarwinRelayWorkerSnapshot snapshot{};
    do {
        snapshot = worker.Snapshot();
        if (worker.PendingQuicSendCountForTest(result.RelayId) == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    CHECK(g_sendCalls.load(std::memory_order_acquire) == 1);
    CHECK(worker.PendingQuicSendCountForTest(result.RelayId) == 1);
    CHECK(snapshot.PendingEvents == 0);
    CHECK(snapshot.Errors == 0);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void SendCompleteAfterUnregisterReleasesOperation() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "complete";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (worker.InFlightQuicSendCountForTest(result.RelayId) == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    void* callbackContext = worker.StreamCallbackContextForTest(result.RelayId);
    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(callbackContext != nullptr);
    CHECK(sendContext != nullptr);
    worker.UnregisterRelay(result.RelayId);
    CHECK(handle.Backend == TqRelayBackendType::None);
    CHECK(handle.DarwinWorker == nullptr);
    CHECK(handle.DarwinRelayId == 0);

    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);

    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void SynchronousSendCompleteBeforeFailureDoesNotDoubleRelease() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_OUT_OF_MEMORY);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    void* callbackContext = worker.StreamCallbackContextForTest(result.RelayId);
    CHECK(callbackContext != nullptr);
    g_syncCallbackContext.store(callbackContext, std::memory_order_release);
    g_completeBeforeSendReturns.store(true, std::memory_order_release);

    const char payload[] = "sync-fail";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (g_sendCalls.load(std::memory_order_acquire) == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    CHECK(g_sendCalls.load(std::memory_order_acquire) == 1);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.PendingQuicSendCountForTest(result.RelayId) == 0);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void SynchronousSendCompleteBeforeSuccessDoesNotLeak() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    void* callbackContext = worker.StreamCallbackContextForTest(result.RelayId);
    CHECK(callbackContext != nullptr);
    g_syncCallbackContext.store(callbackContext, std::memory_order_release);
    g_completeBeforeSendReturns.store(true, std::memory_order_release);

    const char payload[] = "sync-ok";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (g_sendCalls.load(std::memory_order_acquire) == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    CHECK(g_sendCalls.load(std::memory_order_acquire) == 1);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.PendingQuicSendCountForTest(result.RelayId) == 0);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void MagicMismatchKnownOperationCleansAccounting() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "badmagic";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (worker.InFlightQuicSendCountForTest(result.RelayId) == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    void* callbackContext = worker.StreamCallbackContextForTest(result.RelayId);
    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(callbackContext != nullptr);
    CHECK(sendContext != nullptr);
    CHECK(worker.CorruptOneInFlightSendMagicForTest(result.RelayId));

    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.Snapshot().Errors == 1);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void StopThenLateCompletionDoesNotUseDanglingWorker() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "late";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (worker.InFlightQuicSendCountForTest(result.RelayId) == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    void* callbackContext = worker.StreamCallbackContextForTest(result.RelayId);
    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(callbackContext != nullptr);
    CHECK(sendContext != nullptr);
    worker.UnregisterRelay(result.RelayId);
    CHECK(handle.Backend == TqRelayBackendType::None);

    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.KnownSendOperationCountForTest() == 0);

    worker.Stop();
    CHECK(worker.KnownSendOperationCountForTest() == 0);
    worker.SetStreamSendForTest(nullptr);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void UnknownSendCompleteContextIsIgnoredWithoutFreeing() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    void* callbackContext = worker.StreamCallbackContextForTest(result.RelayId);
    CHECK(callbackContext != nullptr);

    uintptr_t unknownContext = 0x12345678u;
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = reinterpret_cast<void*>(unknownContext);
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.KnownSendOperationCountForTest() == 0);
    CHECK(worker.Snapshot().Errors == 1);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void StopReturnedLateCompletionUsesCurrentStreamCallback() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "current-stream-late";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (worker.InFlightQuicSendCountForTest(result.RelayId) == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(sendContext != nullptr);
    CHECK(stream->Callback == TqDarwinRelayWorker::StreamCallback);
    CHECK(stream->Context != nullptr);

    worker.Stop();
    CHECK(stream->Callback == TqDarwinRelayWorker::StreamCallback);
    CHECK(stream->Context != nullptr);

    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(stream->Callback(stream, stream->Context, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.KnownSendOperationCountForTest() == 0);
    CHECK(stream->Callback == MsQuicStream::NoOpCallback);
    CHECK(stream->Context == nullptr);
    CHECK(stream->Callback(stream, stream->Context, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.KnownSendOperationCountForTest() == 0);

    worker.SetStreamSendForTest(nullptr);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void StopReturnedLateCompletionClearsProductionStreamContextWithoutExternalOwner() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "production-stream-late";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (worker.InFlightQuicSendCountForTest(result.RelayId) == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(sendContext != nullptr);
    CHECK(stream->Callback == TqDarwinRelayWorker::StreamCallback);
    CHECK(stream->Context != nullptr);

    worker.Stop();
    CHECK(stream->Callback == TqDarwinRelayWorker::StreamCallback);
    CHECK(stream->Context != nullptr);

    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendContext;
    auto callback = stream->Callback;
    CHECK(callback(stream, stream->Context, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.KnownSendOperationCountForTest() == 0);
    CHECK(stream->Callback == MsQuicStream::NoOpCallback);
    CHECK(stream->Context == nullptr);

    worker.SetStreamSendForTest(nullptr);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void RetireWithoutKnownOperationsClearsCurrentStreamCallback() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    CHECK(stream->Callback == TqDarwinRelayWorker::StreamCallback);
    CHECK(stream->Context != nullptr);

    worker.UnregisterRelay(result.RelayId);
    CHECK(stream->Callback == MsQuicStream::NoOpCallback);
    CHECK(stream->Context == nullptr);

    worker.Stop();
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void StopReturnedLateCompletionReleasesKnownOperation() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "stop-late";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (worker.InFlightQuicSendCountForTest(result.RelayId) == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    std::shared_ptr<void> callbackOwner = worker.StreamCallbackContextOwnerForTest(result.RelayId);
    void* callbackContext = callbackOwner.get();
    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(callbackContext != nullptr);
    CHECK(sendContext != nullptr);

    worker.Stop();
    CHECK(handle.Backend == TqRelayBackendType::None);

    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.KnownSendOperationCountForTest() == 0);
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.KnownSendOperationCountForTest() == 0);

    worker.SetStreamSendForTest(nullptr);
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void DestroyedWorkerLateCompletionUsesSharedOwner() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    std::shared_ptr<void> callbackOwner;
    void* callbackContext = nullptr;
    void* sendContext = nullptr;
    {
        TqDarwinRelayWorkerConfig config{};
        config.ReadChunkSize = 4096;
        config.ReadBatchBytes = 4096;
        config.MaxBufferedQuicSendBytes = 64 * 1024;
        TqDarwinRelayWorker worker(config);
        worker.SetStreamSendForTest(FakeStreamSend);
        TqRelayHandle handle{};
        CHECK(worker.Start());

        TqDarwinRelayRegistration registration{};
        registration.TcpFd = fds[1];
        registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
        registration.Handle = &handle;
        registration.EnableQuicSends = true;

        TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
        CHECK(result.Ok);

        const char payload[] = "destroy-late";
        CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        do {
            if (worker.InFlightQuicSendCountForTest(result.RelayId) == 1) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);

        CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
        callbackOwner = worker.StreamCallbackContextOwnerForTest(result.RelayId);
        callbackContext = callbackOwner.get();
        sendContext = g_lastSendContext.load(std::memory_order_acquire);
        CHECK(callbackContext != nullptr);
        CHECK(sendContext != nullptr);
    }

    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);

    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void UnknownSendCompleteAfterStopIsIgnored() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    std::shared_ptr<void> callbackOwner = worker.StreamCallbackContextOwnerForTest(result.RelayId);
    void* callbackContext = callbackOwner.get();
    CHECK(callbackContext != nullptr);

    worker.Stop();
    uintptr_t unknownContext = 0x87654321u;
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = reinterpret_cast<void*>(unknownContext);
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.KnownSendOperationCountForTest() == 0);

    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void StopAfterCompletionLeavesNoKnownOperations() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    CHECK(handle.Backend == TqRelayBackendType::DarwinWorker);

    worker.Stop();

    TqDarwinRelayWorkerSnapshot snapshot = worker.Snapshot();
    CHECK(snapshot.ActiveRelays == 0);
    CHECK(handle.Backend == TqRelayBackendType::None);
    CHECK(handle.DarwinWorker == nullptr);
    CHECK(handle.DarwinRelayId == 0);

    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void RegisterFilterFailureRollsBackRelayAndHandle() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    TqRelayHandle handle{};
    CHECK(worker.Start());
    worker.SetRegisterTcpFiltersFailureForTest(true);

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(!result.Ok);
    CHECK(result.RelayId == 0);

    TqDarwinRelayWorkerSnapshot snapshot = worker.Snapshot();
    CHECK(snapshot.ActiveRelays == 0);
    CHECK(handle.Backend == TqRelayBackendType::None);
    CHECK(handle.DarwinWorker == nullptr);
    CHECK(handle.DarwinRelayId == 0);

    worker.Stop();
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void RegisterAfterStopFailsWithoutPublishingHandle() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    TqRelayHandle handle{};
    CHECK(worker.Start());
    worker.Stop();

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(!result.Ok);
    CHECK(result.RelayId == 0);

    TqDarwinRelayWorkerSnapshot snapshot = worker.Snapshot();
    CHECK(snapshot.ActiveRelays == 0);
    CHECK(handle.Backend == TqRelayBackendType::None);
    CHECK(handle.DarwinWorker == nullptr);
    CHECK(handle.DarwinRelayId == 0);

    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

} // namespace

int main() {
    SocketPairEnvironmentWorks();
    WorkerStartsAndStopsCleanly();
    WorkerRegistersTcpReadinessShell();
    WorkerObservesTcpReadBytes();
    WorkerObservesTcpReadWithSmallByteBudget();
    TransientSendFailureQueuesWithoutSelfRetry();
    SendCompleteAfterUnregisterReleasesOperation();
    SynchronousSendCompleteBeforeFailureDoesNotDoubleRelease();
    SynchronousSendCompleteBeforeSuccessDoesNotLeak();
    MagicMismatchKnownOperationCleansAccounting();
    StopThenLateCompletionDoesNotUseDanglingWorker();
    StopAfterCompletionLeavesNoKnownOperations();
    UnknownSendCompleteContextIsIgnoredWithoutFreeing();
    StopReturnedLateCompletionUsesCurrentStreamCallback();
    StopReturnedLateCompletionClearsProductionStreamContextWithoutExternalOwner();
    RetireWithoutKnownOperationsClearsCurrentStreamCallback();
    StopReturnedLateCompletionReleasesKnownOperation();
    DestroyedWorkerLateCompletionUsesSharedOwner();
    UnknownSendCompleteAfterStopIsIgnored();
    RegisterFilterFailureRollsBackRelayAndHandle();
    RegisterAfterStopFailsWithoutPublishingHandle();
    return 0;
}

#endif
