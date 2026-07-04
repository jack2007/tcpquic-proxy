#if defined(__APPLE__)

#include "darwin_relay_worker.h"
#include "compress.h"

#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <zstd.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>
#include <chrono>
#include <thread>

namespace {

std::atomic<QUIC_STATUS> g_sendStatus{QUIC_STATUS_SUCCESS};
std::atomic<uint64_t> g_sendCalls{0};
std::atomic<void*> g_lastSendContext{nullptr};
std::atomic<void*> g_syncCallbackContext{nullptr};
std::atomic<bool> g_completeBeforeSendReturns{false};
std::atomic<uint64_t> g_receiveCompleteCalls{0};
std::atomic<uint64_t> g_receiveCompleteBytes{0};
std::atomic<uint64_t> g_sendMsgCallLimit{0};
std::atomic<uint64_t> g_sendMsgCalls{0};
std::atomic<int> g_sendMsgMode{0};
std::atomic<QUIC_STATUS> g_receiveSetEnabledStatus{QUIC_STATUS_SUCCESS};
std::atomic<uint64_t> g_receiveSetEnabledCalls{0};
std::atomic<int> g_lastReceiveSetEnabled{-1};
std::mutex g_blockingSendMutex;
std::condition_variable g_blockingSendReady;
std::condition_variable g_blockingSendContinue;
bool g_blockingSendEntered{false};
bool g_blockingSendMayContinue{false};
bool g_blockingSendReturned{false};

struct FailingDecompressor final : ITqDecompressor {
    bool Decompress(const uint8_t*, size_t, std::vector<uint8_t>&) override {
        return false;
    }

    bool DecompressInto(
        const uint8_t*,
        size_t,
        uint8_t*,
        size_t,
        TqDecompressResult*) override {
        return false;
    }

    void Reset() override {}
};

struct FlushOnlyCompressor final : ITqCompressor {
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

struct ZstdTestDecompressor final : ITqDecompressor {
    ZstdTestDecompressor() {
        Context = ZSTD_createDCtx();
    }

    ~ZstdTestDecompressor() override {
        if (Context != nullptr) {
            ZSTD_freeDCtx(Context);
        }
    }

    bool Decompress(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out) override {
        if (Context == nullptr || (inLen != 0 && in == nullptr)) {
            return false;
        }
        ZSTD_inBuffer input{in, inLen, 0};
        std::vector<uint8_t> scratch(ZSTD_DStreamOutSize());
        while (input.pos < input.size) {
            ZSTD_outBuffer output{scratch.data(), scratch.size(), 0};
            const size_t ret = ZSTD_decompressStream(Context, &output, &input);
            if (ZSTD_isError(ret)) {
                return false;
            }
            out.insert(out.end(), scratch.begin(), scratch.begin() + output.pos);
        }
        return true;
    }

    bool DecompressInto(
        const uint8_t* input,
        size_t inputLength,
        uint8_t* output,
        size_t outputCapacity,
        TqDecompressResult* result) override {
        if (result == nullptr || Context == nullptr || (inputLength != 0 && input == nullptr) ||
            (outputCapacity != 0 && output == nullptr)) {
            return false;
        }
        *result = TqDecompressResult{};
        ZSTD_inBuffer inBuffer{input, inputLength, 0};
        ZSTD_outBuffer outBuffer{output, outputCapacity, 0};
        const size_t ret = ZSTD_decompressStream(Context, &outBuffer, &inBuffer);
        if (ZSTD_isError(ret)) {
            return false;
        }
        result->InputConsumed = inBuffer.pos;
        result->OutputProduced = outBuffer.pos;
        result->NeedsMoreInput = (ret != 0 && inBuffer.pos == inBuffer.size);
        result->NeedsMoreOutput = (outBuffer.pos == outBuffer.size && ret != 0);
        return true;
    }

    void Reset() override {
        if (Context != nullptr) {
            ZSTD_DCtx_reset(Context, ZSTD_reset_session_only);
        }
    }

    ZSTD_DCtx* Context{nullptr};
};

bool CompressZstdFrame(const uint8_t* input, size_t inputLength, std::vector<uint8_t>& output) {
    ZSTD_CCtx* context = ZSTD_createCCtx();
    if (context == nullptr) {
        return false;
    }
    ZSTD_inBuffer inBuffer{input, inputLength, 0};
    std::vector<uint8_t> scratch(ZSTD_CStreamOutSize());
    bool ok = true;
    for (;;) {
        ZSTD_outBuffer outBuffer{scratch.data(), scratch.size(), 0};
        const size_t ret = ZSTD_compressStream2(context, &outBuffer, &inBuffer, ZSTD_e_end);
        if (ZSTD_isError(ret)) {
            ok = false;
            break;
        }
        output.insert(output.end(), scratch.begin(), scratch.begin() + outBuffer.pos);
        if (ret == 0) {
            break;
        }
    }
    ZSTD_freeCCtx(context);
    return ok;
};

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

void FakeReceiveComplete(MsQuicStream*, uint64_t byteCount) {
    g_receiveCompleteCalls.fetch_add(1, std::memory_order_relaxed);
    g_receiveCompleteBytes.fetch_add(byteCount, std::memory_order_relaxed);
}

QUIC_STATUS FakeReceiveSetEnabled(MsQuicStream*, bool enabled) {
    g_receiveSetEnabledCalls.fetch_add(1, std::memory_order_relaxed);
    g_lastReceiveSetEnabled.store(enabled ? 1 : 0, std::memory_order_release);
    return g_receiveSetEnabledStatus.load(std::memory_order_acquire);
}

void ResetFakeReceiveSetEnabled(QUIC_STATUS status) {
    g_receiveSetEnabledStatus.store(status, std::memory_order_release);
    g_receiveSetEnabledCalls.store(0, std::memory_order_release);
    g_lastReceiveSetEnabled.store(-1, std::memory_order_release);
}

ssize_t PartialSendMsg(TqSocketHandle fd, const struct msghdr* msg) {
    const uint64_t call = g_sendMsgCalls.fetch_add(1, std::memory_order_relaxed);
    const int mode = g_sendMsgMode.load(std::memory_order_acquire);
    if (mode == 1) {
        errno = EAGAIN;
        return -1;
    }
    if (mode == 2 && call != 0) {
        errno = ECONNRESET;
        return -1;
    }
    if (mode == 3 && call != 0) {
        errno = EAGAIN;
        return -1;
    }
    if (mode == 4) {
        errno = ECONNRESET;
        return -1;
    }
    if (call == 0) {
        const uint64_t limit = g_sendMsgCallLimit.load(std::memory_order_acquire);
        if (limit != 0 && msg != nullptr && msg->msg_iovlen != 0) {
            const auto* iov = static_cast<const iovec*>(msg->msg_iov);
            const size_t length = std::min<size_t>(static_cast<size_t>(limit), iov[0].iov_len);
            return send(fd, iov[0].iov_base, length, 0);
        }
    }
    return sendmsg(fd, msg, 0);
}

ssize_t BlockingSendMsg(TqSocketHandle fd, const struct msghdr* msg) {
    g_sendMsgCalls.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_blockingSendMutex);
        g_blockingSendEntered = true;
    }
    g_blockingSendReady.notify_one();
    {
        std::unique_lock<std::mutex> lock(g_blockingSendMutex);
        g_blockingSendContinue.wait(lock, [] { return g_blockingSendMayContinue; });
    }
    if (msg == nullptr || msg->msg_iov == nullptr || msg->msg_iovlen == 0) {
        errno = EINVAL;
        return -1;
    }
    const auto* iov = static_cast<const iovec*>(msg->msg_iov);
    const ssize_t result = send(fd, iov[0].iov_base, iov[0].iov_len, 0);
    {
        std::lock_guard<std::mutex> lock(g_blockingSendMutex);
        g_blockingSendReturned = true;
    }
    g_blockingSendReady.notify_one();
    return result;
}

void ResetBlockingSendMsg() {
    std::lock_guard<std::mutex> lock(g_blockingSendMutex);
    g_blockingSendEntered = false;
    g_blockingSendMayContinue = false;
    g_blockingSendReturned = false;
}

void ReleaseBlockingSendMsg() {
    {
        std::lock_guard<std::mutex> lock(g_blockingSendMutex);
        g_blockingSendMayContinue = true;
    }
    g_blockingSendContinue.notify_one();
}

bool WaitForBlockingSendMsg() {
    std::unique_lock<std::mutex> lock(g_blockingSendMutex);
    return g_blockingSendReady.wait_for(lock, std::chrono::seconds(2), [] { return g_blockingSendEntered; });
}

bool WaitForBlockingSendMsgReturned() {
    std::unique_lock<std::mutex> lock(g_blockingSendMutex);
    return g_blockingSendReady.wait_for(lock, std::chrono::seconds(2), [] { return g_blockingSendReturned; });
}

void ResetFakeReceiveComplete() {
    g_receiveCompleteCalls.store(0, std::memory_order_release);
    g_receiveCompleteBytes.store(0, std::memory_order_release);
}

void ResetPartialSendMsg(uint64_t firstCallLimit) {
    g_sendMsgCallLimit.store(firstCallLimit, std::memory_order_release);
    g_sendMsgCalls.store(0, std::memory_order_release);
    g_sendMsgMode.store(0, std::memory_order_release);
}

void SetSendMsgMode(int mode) {
    g_sendMsgMode.store(mode, std::memory_order_release);
    g_sendMsgCalls.store(0, std::memory_order_release);
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

void CloseSocketPairBoth(int fds[2]) {
    CHECK(close(fds[0]) == 0);
    CHECK(close(fds[1]) == 0);
}

void CloseSocketPairAfterRelayOwned(int relayTcpFd, int fds[2]) {
    errno = 0;
    CHECK(fcntl(relayTcpFd, F_GETFD) == -1 && errno == EBADF);
    const int peerFd = relayTcpFd == fds[0] ? fds[1] : fds[0];
    CHECK(close(peerFd) == 0);
}

TqDarwinRelayEvent TestMarkerEvent(uint64_t value) {
    TqDarwinRelayEvent event{};
    event.Type = TqDarwinRelayEventType::TestMarker;
    event.Value = value;
    return event;
}

void SocketPairEnvironmentWorks() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(fds[0] != TqInvalidSocket);
    CHECK(fds[1] != TqInvalidSocket);
    CloseSocketPairBoth(fds);
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

void ControlEventRetriesAfterWakeFailure() {
    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    CHECK(worker.Start());
    worker.SetWakeFailuresForTest(1);

    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    TqDarwinRelayWorkerSnapshot snapshot{};
    std::thread snapshotThread([&] {
        snapshot = worker.Snapshot();
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        cv.notify_one();
    });

    bool doneBeforeStop = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        doneBeforeStop = cv.wait_for(lock, std::chrono::seconds(2), [&] { return done; });
    }

    worker.Stop();
    snapshotThread.join();

    CHECK(doneBeforeStop);
    CHECK(snapshot.Errors == 1);
}

void UnregisterWakesAfterFullQueueWakeFailures() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqDarwinRelayWorkerConfig config{};
    config.EventQueueCapacity = 2;
    TqDarwinRelayWorker worker(config);
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    worker.SetWakeFailuresForTest(2);
    CHECK(worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(worker.EnqueueForTest(TestMarkerEvent(2)));
    CHECK(worker.Snapshot().PendingEvents == 2);

    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::thread unregisterThread([&] {
        worker.UnregisterRelay(result.RelayId);
        {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
        }
        cv.notify_one();
    });

    bool doneBeforeStop = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        doneBeforeStop = cv.wait_for(lock, std::chrono::seconds(2), [&] { return done; });
    }

    worker.Stop();
    unregisterThread.join();

    CHECK(doneBeforeStop);
    CHECK(handle.Backend == TqRelayBackendType::None);
    CHECK(handle.DarwinWorker == nullptr);
    CHECK(handle.DarwinRelayId == 0);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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
    CHECK(handle.DarwinWorker != nullptr);
    CHECK(handle.DarwinWorker == &worker);
    CHECK(handle.DarwinRelayId == result.RelayId);

    TqDarwinRelayWorkerSnapshot snapshot = worker.Snapshot();
    CHECK(snapshot.Errors == 0);
    CHECK(snapshot.ActiveRelays == 1);
    CHECK(snapshot.TcpReadArmedRelays == 1);
    CHECK(snapshot.TcpWriteArmedRelays == 0);
    CHECK(snapshot.PendingBytes == 0);
    CHECK(snapshot.CurrentPendingQuicReceiveBytes == 0);
    CHECK(snapshot.PendingTcpWriteQueue == 0);
    CHECK(snapshot.PendingTcpWriteBytes == 0);
    CHECK(snapshot.OutstandingQuicSends == 0);
    CHECK(snapshot.OutstandingQuicSendBytes == 0);
    CHECK(snapshot.TcpReadBatches == 0);
    CHECK(snapshot.TcpWriteBatches == 0);
    CHECK(snapshot.QuicReceiveViewCount == 0);
    CHECK(snapshot.DeferredReceiveCompletes == 0);
    CHECK(snapshot.QuicSendBackpressureEvents == 0);

    worker.UnregisterRelay(result.RelayId);
    snapshot = worker.Snapshot();
    CHECK(snapshot.Errors == 0);
    CHECK(snapshot.ActiveRelays == 0);
    CHECK(handle.Backend == TqRelayBackendType::None);
    CHECK(handle.DarwinWorker == nullptr);
    CHECK(handle.DarwinRelayId == 0);

    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void EventizedUnregisterClearsHandleAndRelayCount() {
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
    CHECK(worker.Snapshot().ActiveRelays == 1);

    worker.UnregisterRelay(result.RelayId);

    CHECK(handle.Backend == TqRelayBackendType::None);
    CHECK(handle.DarwinWorker == nullptr);
    CHECK(handle.DarwinRelayId == 0);
    CHECK(worker.Snapshot().ActiveRelays == 0);
    CHECK(worker.Snapshot().Errors == 0);

    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const uint64_t lockedBefore = worker.FindRelayLockedCountForTest();
    const uint64_t localBefore = worker.FindRelayLocalCountForTest();
    const char payload[] = "darwin-readiness";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));

    TqDarwinRelayWorkerSnapshot snapshot = worker.Snapshot();

    CHECK(snapshot.TcpReadBytes >= sizeof(payload) - 1);
    CHECK(snapshot.Errors == 0);
    CHECK(worker.FindRelayLockedCountForTest() == lockedBefore);
    CHECK(worker.FindRelayLocalCountForTest() > localBefore);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void CompressedTcpReadFlushesWhenCompressorBuffersInput() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxIov = 1;
    config.MaxBufferedQuicSendBytes = 1024 * 1024;

    FlushOnlyCompressor compressor;
    TqDarwinRelayWorker worker(config);
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
    registration.CompressAlgo = TqCompressAlgo::Zstd;
    registration.Compressor = &compressor;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "z";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (compressor.FlushCalls.load(std::memory_order_acquire) != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    CHECK(compressor.CompressCalls.load(std::memory_order_acquire) >= 1);
    CHECK(compressor.InputBytes.load(std::memory_order_acquire) >= sizeof(payload) - 1);
    CHECK(compressor.FlushCalls.load(std::memory_order_acquire) >= 1);
    CHECK(worker.Snapshot().Errors == 0);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "transient";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));

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
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void SendCompleteCallbackQueuesUntilWorkerDrain() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 16;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "queued-send-complete";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 1);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);

    void* callbackContext = worker.StreamCallbackContextForTest(result.RelayId);
    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(callbackContext != nullptr);
    CHECK(sendContext != nullptr);

    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);

    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.KnownSendOperationCountForTest() == 0);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void ActiveWorkerSendCompleteDoesNotPurgeRetiredRelays() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 16;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "active-complete-no-purge";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 1);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);

    const uint64_t purgeCount = worker.RetiredRelayPurgeCountForTest();
    CHECK(worker.CompleteOneInFlightSendForTest(result.RelayId) != 0);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.KnownSendOperationCountForTest() == 0);
    CHECK(worker.RetiredRelayPurgeCountForTest() == purgeCount);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void SendCompleteEnqueueFailureWaitsForWorkerAccounting() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 2;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "fallback-complete";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 1);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);

    void* callbackContext = worker.StreamCallbackContextForTest(result.RelayId);
    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(callbackContext != nullptr);
    CHECK(sendContext != nullptr);

    g_sendStatus.store(QUIC_STATUS_OUT_OF_MEMORY, std::memory_order_release);
    const char pendingPayload[] = "pending-after-full-queue";
    CHECK(write(fds[0], pendingPayload, sizeof(pendingPayload) - 1) ==
        static_cast<ssize_t>(sizeof(pendingPayload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 2);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    CHECK(worker.PendingQuicSendCountForTest(result.RelayId) == 1);

    CHECK(worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(worker.EnqueueForTest(TestMarkerEvent(2)));
    CHECK(worker.Snapshot().PendingEvents == 2);

    g_sendStatus.store(QUIC_STATUS_SUCCESS, std::memory_order_release);
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendContext;
    std::atomic<bool> callbackEntered{false};
    std::atomic<bool> callbackReturned{false};
    std::thread callbackThread([&] {
        callbackEntered.store(true, std::memory_order_release);
        CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
        callbackReturned.store(true, std::memory_order_release);
    });

    const auto enteredDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!callbackEntered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < enteredDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(callbackEntered.load(std::memory_order_acquire));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!callbackReturned.load(std::memory_order_acquire));
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    CHECK(worker.PendingQuicSendCountForTest(result.RelayId) == 1);
    CHECK(worker.KnownSendOperationCountForTest() == 1);

    CHECK(worker.DrainOneEventForTest());
    const auto returnedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!callbackReturned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < returnedDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(callbackReturned.load(std::memory_order_acquire));
    callbackThread.join();

    CHECK(g_sendCalls.load(std::memory_order_acquire) == 2);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    CHECK(worker.PendingQuicSendCountForTest(result.RelayId) == 1);
    CHECK(worker.KnownSendOperationCountForTest() == 1);

    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    CHECK(worker.PendingQuicSendCountForTest(result.RelayId) == 1);
    CHECK(worker.KnownSendOperationCountForTest() == 1);

    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    CHECK(worker.PendingQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.KnownSendOperationCountForTest() == 1);
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 3);
    CHECK(worker.CompleteOneInFlightSendForTest(result.RelayId) != 0);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.KnownSendOperationCountForTest() == 0);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void SendCompleteAfterRunningFalseWaitsForWorkerExit() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 2;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "running-false-complete";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 1);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);

    void* callbackContext = worker.StreamCallbackContextForTest(result.RelayId);
    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(callbackContext != nullptr);
    CHECK(sendContext != nullptr);

    CHECK(worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(worker.EnqueueForTest(TestMarkerEvent(2)));
    CHECK(worker.Snapshot().PendingEvents == 2);

    worker.SetRunningForTest(false);
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendContext;
    std::atomic<bool> callbackEntered{false};
    std::atomic<bool> callbackReturned{false};
    std::thread callbackThread([&] {
        callbackEntered.store(true, std::memory_order_release);
        CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
        callbackReturned.store(true, std::memory_order_release);
    });

    const auto enteredDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!callbackEntered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < enteredDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(callbackEntered.load(std::memory_order_acquire));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!callbackReturned.load(std::memory_order_acquire));
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    CHECK(worker.KnownSendOperationCountForTest() == 1);

    worker.MarkWorkerThreadExitedForTest();
    const auto returnedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!callbackReturned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < returnedDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(callbackReturned.load(std::memory_order_acquire));
    callbackThread.join();
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.KnownSendOperationCountForTest() == 0);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void SendCompleteFallsBackToBindingRelayWhenMapLookupMisses() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 16;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSend);
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "binding-fallback";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);

    std::shared_ptr<void> callbackOwner = worker.StreamCallbackContextOwnerForTest(result.RelayId);
    void* callbackContext = callbackOwner.get();
    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(callbackContext != nullptr);
    CHECK(sendContext != nullptr);

    g_sendStatus.store(QUIC_STATUS_OUT_OF_MEMORY, std::memory_order_release);
    const char pendingPayload[] = "pending-after-detach";
    CHECK(write(fds[0], pendingPayload, sizeof(pendingPayload) - 1) ==
        static_cast<ssize_t>(sizeof(pendingPayload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 2);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    CHECK(worker.PendingQuicSendCountForTest(result.RelayId) == 1);

    std::shared_ptr<void> relayOwner = worker.DetachRelayFromActiveMapForTest(result.RelayId);
    CHECK(relayOwner != nullptr);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.InFlightQuicSendCountFromRelayForTest(relayOwner) == 1);
    CHECK(worker.PendingQuicSendCountFromRelayForTest(relayOwner) == 1);

    g_sendStatus.store(QUIC_STATUS_SUCCESS, std::memory_order_release);
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.InFlightQuicSendCountFromRelayForTest(relayOwner) == 0);
    CHECK(worker.PendingQuicSendCountFromRelayForTest(relayOwner) == 1);
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 2);
    CHECK(worker.KnownSendOperationCountForTest() == 0);

    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairBoth(fds);
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
    const auto completeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (worker.InFlightQuicSendCountForTest(result.RelayId) == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < completeDeadline);

    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.PendingQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.KnownSendOperationCountForTest() == 0);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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
    const auto completeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (worker.InFlightQuicSendCountForTest(result.RelayId) == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < completeDeadline);

    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.PendingQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.KnownSendOperationCountForTest() == 0);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "badmagic";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));

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
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.Snapshot().Errors == 1);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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
    const auto completionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (worker.KnownSendOperationCountForTest() == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < completionDeadline);
    CHECK(worker.KnownSendOperationCountForTest() == 0);

    worker.Stop();
    CHECK(worker.KnownSendOperationCountForTest() == 0);
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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

    CloseSocketPairAfterRelayOwned(fds[1], fds);
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

    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void StopThenLateTcpEventIgnoresRetiredRelay() {
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
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "late-tcp-event";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (worker.InFlightQuicSendCountForTest(result.RelayId) == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    std::shared_ptr<void> callbackOwner = worker.StreamCallbackContextOwnerForTest(result.RelayId);
    CHECK(callbackOwner != nullptr);
    worker.UnregisterRelay(result.RelayId);
    CHECK(worker.Snapshot().ActiveRelays == 0);

    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    CHECK(worker.Snapshot().Errors == 0);
    CHECK(worker.Snapshot().ActiveRelays == 0);

    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(sendContext != nullptr);
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackOwner.get(), &event) == QUIC_STATUS_SUCCESS);
    const auto completionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        if (worker.KnownSendOperationCountForTest() == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < completionDeadline);
    CHECK(worker.KnownSendOperationCountForTest() == 0);

    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void StreamShutdownStopsRelayAndPreventsFurtherTcpToQuicSends() {
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
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char firstPayload[] = "before-shutdown";
    CHECK(write(fds[0], firstPayload, sizeof(firstPayload) - 1) ==
        static_cast<ssize_t>(sizeof(firstPayload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));

    CHECK(g_sendCalls.load(std::memory_order_acquire) == 1);
    CHECK(worker.Snapshot().ActiveRelays == 1);

    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(sendContext != nullptr);
    QUIC_STREAM_EVENT sendComplete{};
    sendComplete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    sendComplete.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(stream->Callback(stream, stream->Context, &sendComplete) == QUIC_STATUS_SUCCESS);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);

    QUIC_STREAM_EVENT shutdown{};
    shutdown.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(stream->Callback(stream, stream->Context, &shutdown) == QUIC_STATUS_SUCCESS);
    CHECK(worker.Snapshot().ActiveRelays == 1);

    const char secondPayload[] = "after-shutdown";
    (void)send(fds[0], secondPayload, sizeof(secondPayload) - 1, MSG_NOSIGNAL);
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 1);
    CHECK(worker.Snapshot().Errors == 0);

    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.Snapshot().ActiveRelays == 0);

    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void ShutdownCallbackQueuesCloseUntilWorkerDrain() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqDarwinRelayWorkerConfig config{};
    config.EventQueueCapacity = 16;
    TqDarwinRelayWorker worker(config);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    CHECK(worker.Snapshot().ActiveRelays == 1);
    CHECK(handle.Backend == TqRelayBackendType::DarwinWorker);

    QUIC_STREAM_EVENT shutdown{};
    shutdown.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(stream->Callback(stream, stream->Context, &shutdown) == QUIC_STATUS_SUCCESS);
    CHECK(worker.Snapshot().ActiveRelays == 1);
    CHECK(handle.Backend == TqRelayBackendType::DarwinWorker);

    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.Snapshot().ActiveRelays == 0);
    CHECK(handle.Backend == TqRelayBackendType::None);
    CHECK(handle.DarwinWorker == nullptr);
    CHECK(handle.DarwinRelayId == 0);

    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void PeerReceiveAbortCallbackBlocksTcpToQuicUntilWorkerDrain() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.EventQueueCapacity = 16;
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
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    CHECK(worker.Snapshot().ActiveRelays == 1);

    QUIC_STREAM_EVENT aborted{};
    aborted.Type = QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED;
    CHECK(stream->Callback(stream, stream->Context, &aborted) == QUIC_STATUS_SUCCESS);
    CHECK(worker.Snapshot().ActiveRelays == 1);

    const char payload[] = "after-abort";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 0);

    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.Snapshot().ActiveRelays == 0);
    CHECK(handle.Backend == TqRelayBackendType::None);
    CHECK(handle.DarwinWorker == nullptr);
    CHECK(handle.DarwinRelayId == 0);

    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void ReceiveCallbackAfterShutdownDoesNotTakeMsQuicBufferOwnership() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqDarwinRelayWorkerConfig config{};
    config.EventQueueCapacity = 16;
    config.MaxPendingQuicReceiveBytesPerRelay = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    QUIC_STREAM_EVENT shutdown{};
    shutdown.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(stream->Callback(stream, stream->Context, &shutdown) == QUIC_STATUS_SUCCESS);

    uint8_t payload[] = {'c', 'l', 'o', 's', 'e', 'd'};
    QUIC_BUFFER buffer{};
    buffer.Buffer = payload;
    buffer.Length = sizeof(payload);
    QUIC_STREAM_EVENT receive{};
    receive.Type = QUIC_STREAM_EVENT_RECEIVE;
    receive.RECEIVE.Buffers = &buffer;
    receive.RECEIVE.BufferCount = 1;
    receive.RECEIVE.TotalBufferLength = sizeof(payload);

    CHECK(stream->Callback(stream, stream->Context, &receive) == QUIC_STATUS_SUCCESS);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);

    CHECK(worker.DrainOneEventForTest());
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void TcpErrorEventClosesAndRetiresRelay() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqDarwinRelayWorkerConfig config{};
    TqDarwinRelayWorker worker(config);
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.Handle = &handle;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    CHECK(worker.Snapshot().ActiveRelays == 1);

    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, EV_ERROR, ECONNRESET));
    TqDarwinRelayWorkerSnapshot snapshot = worker.Snapshot();
    CHECK(snapshot.ActiveRelays == 0);
    CHECK(handle.Backend == TqRelayBackendType::None);
    CHECK(handle.DarwinWorker == nullptr);
    CHECK(handle.DarwinRelayId == 0);

    const char payload[] = "after-error-close";
    (void)send(fds[0], payload, sizeof(payload) - 1, MSG_NOSIGNAL);
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    snapshot = worker.Snapshot();
    CHECK(snapshot.ActiveRelays == 0);

    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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

    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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
    CloseSocketPairBoth(fds);
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

    CloseSocketPairBoth(fds);
}

void QuicReceiveCallbackReturnsPending() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxIov = 4;
    config.MaxPendingQuicReceiveBytesPerRelay = 64 * 1024;

    TqDarwinRelayWorker worker(config);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    CHECK(stream->Callback == TqDarwinRelayWorker::StreamCallback);
    CHECK(stream->Context != nullptr);

    const char payload[] = "quic-to-tcp";
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);

    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;

    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &receiveEvent) == QUIC_STATUS_PENDING);

    char output[sizeof(payload)]{};
    ssize_t received = -1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        received = read(fds[1], output, sizeof(payload) - 1);
        if (received == static_cast<ssize_t>(sizeof(payload) - 1)) {
            break;
        }
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    CHECK(received == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(std::memcmp(output, payload, sizeof(payload) - 1) == 0);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);
    CHECK(worker.PendingTcpWriteBytesForTest(result.RelayId) == 0);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void QuicReceiveCallbackDefersPendingBytesUntilWorkerEvent() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeReceiveComplete();

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 16;
    config.MaxPendingQuicReceiveBytesPerRelay = 64 * 1024;

    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "eventized-receive";
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);

    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;

    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &receiveEvent) == QUIC_STATUS_PENDING);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);

    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.FlushTcpWritableForTest(result.RelayId));
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == sizeof(payload) - 1);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);
    CHECK(worker.PendingTcpWriteBytesForTest(result.RelayId) == 0);

    char output[sizeof(payload)]{};
    const ssize_t received = read(fds[1], output, sizeof(payload) - 1);
    CHECK(received == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(std::memcmp(output, payload, sizeof(payload) - 1) == 0);

    worker.SetReceiveCompleteForTest(nullptr);
    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void QuicReceiveCallbackQueuedEventCompletesOnStop() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeReceiveComplete();

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 16;
    config.MaxPendingQuicReceiveBytesPerRelay = 64 * 1024;

    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "queued-stop-receive";
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);
    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;

    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &receiveEvent) == QUIC_STATUS_PENDING);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);
    worker.Stop();
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == sizeof(payload) - 1);

    worker.SetReceiveCompleteForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void QuicReceiveCallbackAcceptsOneOversizedReceiveBeforeWorkerEvent() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeReceiveComplete();

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 16;
    config.MaxPendingQuicReceiveBytesPerRelay = 8;

    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "oversized-callback-receive";
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);
    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;

    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &receiveEvent) == QUIC_STATUS_PENDING);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);
    worker.Stop();
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == sizeof(payload) - 1);

    worker.SetReceiveCompleteForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void QuicReceiveCallbackRejectsOversizedReceiveAfterPendingFinOnlyEvent() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeReceiveComplete();

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 16;
    config.MaxPendingQuicReceiveBytesPerRelay = 8;

    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    QUIC_STREAM_EVENT finOnlyEvent{};
    finOnlyEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    finOnlyEvent.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;
    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &finOnlyEvent) == QUIC_STATUS_PENDING);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);

    const char payload[] = "oversized-after-fin";
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);
    QUIC_STREAM_EVENT oversizedEvent{};
    oversizedEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    oversizedEvent.RECEIVE.BufferCount = 1;
    oversizedEvent.RECEIVE.Buffers = &quicBuffer;

    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &oversizedEvent) == QUIC_STATUS_SUCCESS);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);

    worker.SetReceiveCompleteForTest(nullptr);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void QuicReceivePartialTcpWriteCompletesOnceAfterFullFlush() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeReceiveComplete();
    ResetPartialSendMsg(3);
    SetSendMsgMode(3);

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxIov = 1;
    config.TcpWriteMaxBytes = 3;

    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    worker.SetSendMsgForTest(PartialSendMsg);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = false;
    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    std::vector<uint8_t> payload(13, static_cast<uint8_t>('A'));
    const std::vector<uint8_t> expected = payload;
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = payload.data();
    quicBuffer.Length = static_cast<uint32_t>(payload.size());
    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;
    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &receiveEvent) == QUIC_STATUS_PENDING);
    CHECK(worker.DrainOneEventForTest());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_receiveCompleteCalls.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        CHECK(worker.FlushTcpWritableForTest(result.RelayId));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == expected.size());
    CHECK(worker.PendingTcpWriteBytesForTest(result.RelayId) == expected.size() - 3);
    TqDarwinRelayWorkerSnapshot pendingSnapshot = worker.Snapshot();
    CHECK(pendingSnapshot.CurrentPendingQuicReceiveBytes == expected.size());
    CHECK(pendingSnapshot.PendingTcpWriteBytes == expected.size() - 3);
    CHECK(pendingSnapshot.PendingBytes == expected.size());
    std::fill(payload.begin(), payload.end(), static_cast<uint8_t>('X'));

    ResetPartialSendMsg(0);
    const auto flushDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (worker.PendingTcpWriteBytesForTest(result.RelayId) != 0 &&
           std::chrono::steady_clock::now() < flushDeadline) {
        CHECK(worker.FlushTcpWritableForTest(result.RelayId));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(worker.PendingTcpWriteBytesForTest(result.RelayId) == 0);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == expected.size());

    std::vector<uint8_t> output(expected.size());
    size_t total = 0;
    while (total < expected.size()) {
        const ssize_t n = read(fds[1], output.data() + total, expected.size() - total);
        CHECK(n > 0);
        total += static_cast<size_t>(n);
    }
    CHECK(std::find(output.begin(), output.end(), static_cast<uint8_t>('X')) == output.end());
    CHECK(std::find(output.begin(), output.end(), static_cast<uint8_t>('A')) != output.end());

    worker.SetSendMsgForTest(nullptr);
    worker.SetReceiveCompleteForTest(nullptr);
    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void QuicReceiveFlushHoldsBuffersAcrossConcurrentUnregister() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeReceiveComplete();
    ResetBlockingSendMsg();

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxIov = 1;

    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    worker.SetSendMsgForTest(BlockingSendMsg);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = false;
    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "flush-unregister-race";
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);
    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;
    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &receiveEvent) == QUIC_STATUS_PENDING);

    CHECK(WaitForBlockingSendMsg());
    std::mutex unregisterMutex;
    std::condition_variable unregisterCv;
    bool unregisterDone = false;
    std::thread unregisterThread([&] {
        worker.UnregisterRelay(result.RelayId);
        {
            std::lock_guard<std::mutex> lock(unregisterMutex);
            unregisterDone = true;
        }
        unregisterCv.notify_one();
    });

    bool doneDuringFlush = false;
    {
        std::unique_lock<std::mutex> lock(unregisterMutex);
        doneDuringFlush = unregisterCv.wait_for(lock, std::chrono::milliseconds(50), [&] { return unregisterDone; });
    }
    CHECK(!doneDuringFlush);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    ReleaseBlockingSendMsg();
    CHECK(WaitForBlockingSendMsgReturned());
    {
        std::unique_lock<std::mutex> lock(unregisterMutex);
        CHECK(unregisterCv.wait_for(lock, std::chrono::seconds(2), [&] { return unregisterDone; }));
    }
    unregisterThread.join();
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == sizeof(payload) - 1);
    CHECK(handle.Backend == TqRelayBackendType::None);
    CHECK(handle.DarwinWorker == nullptr);
    CHECK(handle.DarwinRelayId == 0);

    worker.SetSendMsgForTest(nullptr);
    worker.SetReceiveCompleteForTest(nullptr);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void QuicReceivePartialWriteThenErrorCompletesFullReceiveOnce() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeReceiveComplete();
    ResetPartialSendMsg(3);
    SetSendMsgMode(3);

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxIov = 1;
    config.TcpWriteMaxBytes = 3;

    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    worker.SetSendMsgForTest(PartialSendMsg);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = false;
    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "partial-then-error";
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);
    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;
    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &receiveEvent) == QUIC_STATUS_PENDING);
    CHECK(worker.DrainOneEventForTest());

    const auto firstDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_sendMsgCalls.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < firstDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(g_sendMsgCalls.load(std::memory_order_acquire) != 0);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == sizeof(payload) - 1);
    CHECK(worker.PendingTcpWriteBytesForTest(result.RelayId) == sizeof(payload) - 1 - 3);

    SetSendMsgMode(1);
    CHECK(worker.FlushTcpWritableForTest(result.RelayId));
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == sizeof(payload) - 1);

    SetSendMsgMode(4);
    CHECK(!worker.FlushTcpWritableForTest(result.RelayId));
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == sizeof(payload) - 1);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);
    CHECK(worker.PendingTcpWriteBytesForTest(result.RelayId) == 0);

    worker.SetSendMsgForTest(nullptr);
    worker.SetReceiveCompleteForTest(nullptr);
    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void QuicReceivePauseAndResumeFollowsTcpWritePressure() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeReceiveComplete();
    ResetPartialSendMsg(0);
    SetSendMsgMode(1);

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxIov = 1;
    config.MaxPendingQuicReceiveBytesPerRelay = 8;

    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    worker.SetSendMsgForTest(PartialSendMsg);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = false;
    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "pause-resume";
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);
    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;
    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &receiveEvent) == QUIC_STATUS_PENDING);
    CHECK(worker.DrainOneEventForTest());

    const auto pauseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    TqDarwinRelayWorkerSnapshot snapshot{};
    do {
        snapshot = worker.Snapshot();
        if (snapshot.QuicReceivePausedCount == 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < pauseDeadline);
    CHECK(snapshot.QuicReceivePausedCount == 1);

    const auto completeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_receiveCompleteCalls.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < completeDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == sizeof(payload) - 1);
    CHECK(worker.PendingTcpWriteBytesForTest(result.RelayId) == sizeof(payload) - 1);

    CHECK(worker.FlushTcpWritableForTest(result.RelayId));
    snapshot = worker.Snapshot();
    CHECK(snapshot.QuicReceiveResumedCount == 0);
    CHECK(worker.PendingTcpWriteBytesForTest(result.RelayId) == sizeof(payload) - 1);

    SetSendMsgMode(0);
    const auto resumeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        CHECK(worker.FlushTcpWritableForTest(result.RelayId));
        snapshot = worker.Snapshot();
        if (snapshot.QuicReceiveResumedCount == 1 &&
            worker.PendingTcpWriteBytesForTest(result.RelayId) <= config.MaxPendingQuicReceiveBytesPerRelay / 2) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < resumeDeadline);
    snapshot = worker.Snapshot();
    CHECK(snapshot.QuicReceiveResumedCount == 1);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);
    CHECK(worker.PendingTcpWriteBytesForTest(result.RelayId) == 0);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == sizeof(payload) - 1);

    worker.SetSendMsgForTest(nullptr);
    worker.SetReceiveCompleteForTest(nullptr);
    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void MissingRelayQuicReceiveCompletesAndReleases() {
    ResetFakeReceiveComplete();
    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    CHECK(worker.StartForTest());

    const char payload[] = "missing-relay";
    auto receive = std::make_shared<TqDarwinPendingQuicReceive>();
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    receive->Stream = stream;
    receive->RelayId = 999999;
    receive->Slices.push_back(TqDarwinQuicReceiveSlice{
        reinterpret_cast<const uint8_t*>(payload),
        static_cast<uint32_t>(sizeof(payload) - 1)});
    receive->TotalLength = sizeof(payload) - 1;

    CHECK(worker.InvokeQuicReceiveViewForTest(receive));
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == sizeof(payload) - 1);
    CHECK(receive->CompletedLength == receive->TotalLength);
    worker.SetReceiveCompleteForTest(nullptr);
    worker.Stop();
}

void CompressedQuicReceiveDecompressesToTcpAndCompletesCompressedBytes() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(TqSetNonBlocking(fds[1]));
    ResetFakeReceiveComplete();

    ZstdTestDecompressor decompressor;
    const char plaintext[] = "darwin-zstd-quic-to-tcp-happy-path";
    std::vector<uint8_t> compressed;
    CHECK(CompressZstdFrame(
        reinterpret_cast<const uint8_t*>(plaintext),
        sizeof(plaintext) - 1,
        compressed));
    CHECK(!compressed.empty());
    CHECK(compressed.size() != sizeof(plaintext) - 1 ||
          std::memcmp(compressed.data(), plaintext, compressed.size()) != 0);

    TqDarwinRelayWorkerConfig config{};
    config.EventBudget = 16;
    config.EventQueueCapacity = 32;
    config.ReadChunkSize = 5;
    config.ReadBatchBytes = 4096;
    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = false;
    registration.CompressAlgo = TqCompressAlgo::Zstd;
    registration.Decompressor = &decompressor;
    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = compressed.data();
    quicBuffer.Length = static_cast<uint32_t>(compressed.size());
    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;
    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &receiveEvent) == QUIC_STATUS_PENDING);

    char output[sizeof(plaintext)]{};
    size_t total = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do {
        const ssize_t n = read(fds[1], output + total, sizeof(plaintext) - 1 - total);
        if (n > 0) {
            total += static_cast<size_t>(n);
            if (total == sizeof(plaintext) - 1) {
                break;
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            CHECK(false);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    CHECK(total == sizeof(plaintext) - 1);
    CHECK(std::memcmp(output, plaintext, sizeof(plaintext) - 1) == 0);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == compressed.size());
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);
    CHECK(worker.PendingTcpWriteBytesForTest(result.RelayId) == 0);

    worker.SetReceiveCompleteForTest(nullptr);
    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void QuicReceivePauseFailureRollsBackAndCompletesReceive() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeReceiveComplete();
    ResetFakeReceiveSetEnabled(static_cast<QUIC_STATUS>(1));

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxIov = 1;
    config.MaxPendingQuicReceiveBytesPerRelay = 8;

    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    worker.SetReceiveSetEnabledForTest(FakeReceiveSetEnabled);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = false;
    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "pause-failure";
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);
    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;
    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &receiveEvent) == QUIC_STATUS_PENDING);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    TqDarwinRelayWorkerSnapshot snapshot{};
    while (g_receiveCompleteCalls.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        snapshot = worker.Snapshot();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    snapshot = worker.Snapshot();
    CHECK(g_receiveSetEnabledCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_lastReceiveSetEnabled.load(std::memory_order_acquire) == 0);
    CHECK(snapshot.QuicReceivePausedCount == 0);
    CHECK(snapshot.QuicReceiveResumedCount == 0);
    CHECK(snapshot.Errors != 0);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == sizeof(payload) - 1);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);
    CHECK(worker.PendingTcpWriteBytesForTest(result.RelayId) == 0);

    worker.SetReceiveSetEnabledForTest(nullptr);
    worker.SetReceiveCompleteForTest(nullptr);
    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void QuicReceiveSnapshotAggregatesPendingTcpWriteMetrics() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeReceiveComplete();
    ResetPartialSendMsg(0);
    SetSendMsgMode(1);

    TqDarwinRelayWorkerConfig config{};
    config.EventBudget = 16;
    config.EventQueueCapacity = 32;
    config.MaxPendingQuicReceiveBytesPerRelay = 1024 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    worker.SetSendMsgForTest(PartialSendMsg);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = false;
    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "snapshot-pending-tcp-write";
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);
    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;
    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &receiveEvent) == QUIC_STATUS_PENDING);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    TqDarwinRelayWorkerSnapshot snapshot{};
    do {
        snapshot = worker.Snapshot();
        if (snapshot.PendingTcpWriteQueue == 1 && snapshot.PendingTcpWriteBytes == sizeof(payload) - 1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    CHECK(snapshot.PendingTcpWriteQueue == 1);
    CHECK(snapshot.PendingTcpWriteBytes == sizeof(payload) - 1);
    CHECK(snapshot.PendingBytes == sizeof(payload) - 1);
    CHECK(snapshot.CurrentPendingQuicReceiveBytes == sizeof(payload) - 1);
    CHECK(snapshot.QuicReceiveViewCount == 1);
    CHECK(snapshot.QuicReceiveViewBytes == sizeof(payload) - 1);
    CHECK(snapshot.TcpWriteBatches == 0);
    CHECK(snapshot.TcpWriteBytes == 0);
    CHECK(snapshot.DeferredReceiveCompletes == 0);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == sizeof(payload) - 1);
    CHECK(worker.PendingTcpWriteBytesForTest(result.RelayId) == sizeof(payload) - 1);

    worker.SetSendMsgForTest(nullptr);
    worker.SetReceiveCompleteForTest(nullptr);
    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void CompressedQuicReceiveFailsClosedWithoutWritingCorruptData() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeReceiveComplete();

    TqDarwinRelayWorkerConfig config{};
    config.EventBudget = 16;
    config.EventQueueCapacity = 32;
    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.Handle = &handle;
    registration.EnableQuicSends = false;
    registration.CompressAlgo = TqCompressAlgo::Zstd;
    FailingDecompressor decompressor;
    registration.Decompressor = &decompressor;
    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const char payload[] = "not-zstd-frame";
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);
    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;
    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &receiveEvent) == QUIC_STATUS_PENDING);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (g_receiveCompleteCalls.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == sizeof(payload) - 1);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);
    CHECK(worker.PendingTcpWriteBytesForTest(result.RelayId) == 0);

    timeval timeout{};
    timeout.tv_usec = 1000;
    CHECK(setsockopt(fds[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    char output[1]{};
    const ssize_t received = read(fds[1], output, sizeof(output));
    CHECK(received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));

    worker.SetReceiveCompleteForTest(nullptr);
    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void CallbackReceiveDoesNotUseLockedRelayLookup() {
    ResetFakeReceiveComplete();
    TqDarwinRelayWorkerConfig config{};
    config.MaxPendingQuicReceiveBytesPerRelay = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    CHECK(worker.StartForTest());

    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    MsQuicStream stream{};
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = &stream;
    registration.Handle = &handle;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const uint64_t before = worker.FindRelayLockedCountForTest();
    uint8_t payload[] = {'h', 'i'};
    QUIC_BUFFER buffer{};
    buffer.Buffer = payload;
    buffer.Length = sizeof(payload);
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.Buffers = &buffer;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.TotalBufferLength = sizeof(payload);
    CHECK(TqDarwinRelayWorker::StreamCallback(&stream, stream.Context, &event) == QUIC_STATUS_PENDING);
    CHECK(worker.FindRelayLockedCountForTest() == before);
    const uint64_t drainLockedBefore = worker.FindRelayLockedCountForTest();
    const uint64_t drainLocalBefore = worker.FindRelayLocalCountForTest();
    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.FindRelayLockedCountForTest() == drainLockedBefore);
    CHECK(worker.FindRelayLocalCountForTest() > drainLocalBefore);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void QuicShutdownCallbackClosesViaWorkerEventWithoutLockedLookup() {
    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    CHECK(worker.StartForTest());

    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    MsQuicStream stream{};
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = &stream;
    registration.Handle = &handle;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const uint64_t before = worker.FindRelayLockedCountForTest();
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(TqDarwinRelayWorker::StreamCallback(&stream, stream.Context, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.FindRelayLockedCountForTest() == before);

    CHECK(worker.Snapshot().ActiveRelays == 1);
    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.Snapshot().ActiveRelays == 0);

    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void QuicShutdownCallbackClosesOnEnqueueFailureWithoutLockedLookup() {
    TqDarwinRelayWorkerConfig config{};
    config.EventQueueCapacity = 2;
    TqDarwinRelayWorker worker(config);
    CHECK(worker.StartForTest());

    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    MsQuicStream stream{};
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = &stream;
    registration.Handle = &handle;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    CHECK(worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(worker.EnqueueForTest(TestMarkerEvent(2)));
    CHECK(worker.Snapshot().PendingEvents == 2);

    const uint64_t before = worker.FindRelayLockedCountForTest();
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    std::mutex callbackMutex;
    std::condition_variable callbackCv;
    bool callbackDone = false;
    std::thread callbackThread([&] {
        CHECK(TqDarwinRelayWorker::StreamCallback(&stream, stream.Context, &event) == QUIC_STATUS_SUCCESS);
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            callbackDone = true;
        }
        callbackCv.notify_one();
    });

    bool doneBeforeDrain = false;
    {
        std::unique_lock<std::mutex> lock(callbackMutex);
        doneBeforeDrain = callbackCv.wait_for(lock, std::chrono::milliseconds(50), [&] { return callbackDone; });
    }
    CHECK(!doneBeforeDrain);
    CHECK(worker.FindRelayLockedCountForTest() == before);
    CHECK(worker.Snapshot().ActiveRelays == 1);

    CHECK(worker.DrainOneEventForTest());
    {
        std::unique_lock<std::mutex> lock(callbackMutex);
        CHECK(callbackCv.wait_for(lock, std::chrono::seconds(2), [&] { return callbackDone; }));
    }
    callbackThread.join();
    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.Snapshot().ActiveRelays == 0);

    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void StopPurgesQueuedShutdownCloseWithoutLockedLookup() {
    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    CHECK(worker.StartForTest());

    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    MsQuicStream stream{};
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = &stream;
    registration.Handle = &handle;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    const uint64_t before = worker.FindRelayLockedCountForTest();
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(TqDarwinRelayWorker::StreamCallback(&stream, stream.Context, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.FindRelayLockedCountForTest() == before);

    worker.Stop();
    CHECK(worker.FindRelayLockedCountForTest() == before);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void RegisteredBindingSurvivesCallbackWithoutMapLookupRequirement() {
    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    CHECK(worker.Start());

    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    MsQuicStream stream{};
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = &stream;
    registration.Handle = &handle;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    CHECK(stream.Context != nullptr);
    CHECK(stream.Callback == TqDarwinRelayWorker::StreamCallback);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void SnapshotDuringConcurrentUnregisterRemainsBestEffort() {
    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    CHECK(worker.Start());

    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    MsQuicStream stream{};
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = &stream;
    registration.Handle = &handle;

    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);

    std::atomic<bool> done{false};
    std::thread snapshotter([&] {
        while (!done.load(std::memory_order_acquire)) {
            (void)worker.Snapshot();
        }
    });

    worker.UnregisterRelay(result.RelayId);
    done.store(true, std::memory_order_release);
    snapshotter.join();

    CHECK(worker.Snapshot().ActiveRelays == 0);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

} // namespace

int main() {
    SocketPairEnvironmentWorks();
    WorkerStartsAndStopsCleanly();
    ControlEventRetriesAfterWakeFailure();
    UnregisterWakesAfterFullQueueWakeFailures();
    WorkerRegistersTcpReadinessShell();
    EventizedUnregisterClearsHandleAndRelayCount();
    WorkerObservesTcpReadBytes();
    WorkerObservesTcpReadWithSmallByteBudget();
    CompressedTcpReadFlushesWhenCompressorBuffersInput();
    TransientSendFailureQueuesWithoutSelfRetry();
    SendCompleteAfterUnregisterReleasesOperation();
    SendCompleteCallbackQueuesUntilWorkerDrain();
    ActiveWorkerSendCompleteDoesNotPurgeRetiredRelays();
    SendCompleteEnqueueFailureWaitsForWorkerAccounting();
    SendCompleteAfterRunningFalseWaitsForWorkerExit();
    SendCompleteFallsBackToBindingRelayWhenMapLookupMisses();
    SynchronousSendCompleteBeforeFailureDoesNotDoubleRelease();
    SynchronousSendCompleteBeforeSuccessDoesNotLeak();
    MagicMismatchKnownOperationCleansAccounting();
    StopThenLateCompletionDoesNotUseDanglingWorker();
    StopThenLateTcpEventIgnoresRetiredRelay();
    StreamShutdownStopsRelayAndPreventsFurtherTcpToQuicSends();
    ShutdownCallbackQueuesCloseUntilWorkerDrain();
    PeerReceiveAbortCallbackBlocksTcpToQuicUntilWorkerDrain();
    ReceiveCallbackAfterShutdownDoesNotTakeMsQuicBufferOwnership();
    TcpErrorEventClosesAndRetiresRelay();
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
    QuicReceiveCallbackReturnsPending();
    QuicReceiveCallbackDefersPendingBytesUntilWorkerEvent();
    QuicReceiveCallbackQueuedEventCompletesOnStop();
    QuicReceiveCallbackAcceptsOneOversizedReceiveBeforeWorkerEvent();
    QuicReceiveCallbackRejectsOversizedReceiveAfterPendingFinOnlyEvent();
    QuicReceivePartialTcpWriteCompletesOnceAfterFullFlush();
    QuicReceiveFlushHoldsBuffersAcrossConcurrentUnregister();
    QuicReceivePartialWriteThenErrorCompletesFullReceiveOnce();
    QuicReceivePauseAndResumeFollowsTcpWritePressure();
    QuicReceivePauseFailureRollsBackAndCompletesReceive();
    MissingRelayQuicReceiveCompletesAndReleases();
    CompressedQuicReceiveDecompressesToTcpAndCompletesCompressedBytes();
    QuicReceiveSnapshotAggregatesPendingTcpWriteMetrics();
    CompressedQuicReceiveFailsClosedWithoutWritingCorruptData();
    CallbackReceiveDoesNotUseLockedRelayLookup();
    QuicShutdownCallbackClosesViaWorkerEventWithoutLockedLookup();
    QuicShutdownCallbackClosesOnEnqueueFailureWithoutLockedLookup();
    StopPurgesQueuedShutdownCloseWithoutLockedLookup();
    RegisteredBindingSurvivesCallbackWithoutMapLookupRequirement();
    SnapshotDuringConcurrentUnregisterRemainsBestEffort();
    return 0;
}

#endif
