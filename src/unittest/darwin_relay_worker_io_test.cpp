#if defined(__APPLE__)

#include "darwin_relay_worker.h"
#include "compress.h"
#include "relay_runtime_snapshot.h"
#include "stream_lifetime.h"

#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <zstd.h>

#include <algorithm>
#include <array>
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
std::shared_ptr<TqStreamLifetime> g_SendRegisterBarrierOwner;
std::atomic<bool> g_SendRegisterBarrierFired{false};
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

std::shared_ptr<TqStreamLifetime> g_PrecommitOwner;
std::array<uint8_t, 8> g_PrecommitPayload{};
QUIC_STATUS g_PrecommitReceiveStatus = QUIC_STATUS_INTERNAL_ERROR;
bool g_PrecommitUncommitted = false;
bool g_PrecommitPublishTerminal = false;

unsigned g_fakeShutdownCount = 0;
QUIC_STREAM_SHUTDOWN_FLAGS g_fakeLastShutdownFlags = QUIC_STREAM_SHUTDOWN_FLAG_NONE;
QUIC_UINT62 g_fakeLastShutdownErrorCode = 0;
bool g_fakeShutdownFail = false;
unsigned g_fakeCloseCount = 0;
void* g_fakeHandler = nullptr;
void* g_fakeHandlerContext = nullptr;

struct FakeShutdownCall {
    QUIC_STREAM_SHUTDOWN_FLAGS Flags{QUIC_STREAM_SHUTDOWN_FLAG_NONE};
    QUIC_UINT62 ErrorCode{0};
};
std::vector<FakeShutdownCall> g_fakeShutdownCalls;

void QUIC_API FakeSetCallbackHandler(HQUIC, void* handler, void* context) {
    g_fakeHandler = handler;
    g_fakeHandlerContext = context;
}

void QUIC_API FakeStreamClose(HQUIC) {
    ++g_fakeCloseCount;
}

QUIC_STATUS QUIC_API FakeStreamShutdown(
    HQUIC,
    QUIC_STREAM_SHUTDOWN_FLAGS flags,
    QUIC_UINT62 errorCode) {
    ++g_fakeShutdownCount;
    g_fakeLastShutdownFlags = flags;
    g_fakeLastShutdownErrorCode = errorCode;
    g_fakeShutdownCalls.push_back(FakeShutdownCall{flags, errorCode});
    return g_fakeShutdownFail ? QUIC_STATUS_INTERNAL_ERROR : QUIC_STATUS_SUCCESS;
}

bool DispatchFakeAdapter(HQUIC handle, QUIC_STREAM_EVENT& event) {
    if (g_fakeHandler == nullptr) {
        return false;
    }
    const auto callback = reinterpret_cast<QUIC_STREAM_CALLBACK_HANDLER>(g_fakeHandler);
    return QUIC_SUCCEEDED(callback(handle, g_fakeHandlerContext, &event));
}

class CountingTarget final : public TqStreamLifetime::Target {
public:
    QUIC_STATUS OnStreamEvent(
        MsQuicStream*,
        QUIC_STREAM_EVENT* event,
        uint64_t generation) noexcept override {
        ++Calls;
        LastType = event->Type;
        LastGeneration = generation;
        return QUIC_STATUS_SUCCESS;
    }

    std::atomic<unsigned> Calls{0};
    QUIC_STREAM_EVENT_TYPE LastType{QUIC_STREAM_EVENT_START_COMPLETE};
    uint64_t LastGeneration{0};
};

struct ManagedRelayHarness {
    TqDarwinRelayWorkerConfig Config{};
    int Fds[2]{TqInvalidSocket, TqInvalidSocket};
    alignas(MsQuicStream) unsigned char StreamStorage[sizeof(MsQuicStream)]{};
    MsQuicStream* Stream{nullptr};
    TqRelayHandle Handle{};
    std::shared_ptr<TqStreamLifetime> Owner;
    TqDarwinRelayRegistrationResult Result{};
    TqDarwinRelayWorker Worker;

    explicit ManagedRelayHarness(TqDarwinRelayWorkerConfig config = {})
        : Config(std::move(config)), Worker(this->Config) {}

    bool OpenSocketPair() {
        return socketpair(AF_UNIX, SOCK_STREAM, 0, Fds) == 0;
    }

    static void PublishHandle(
        TqRelayHandle& handle,
        TqDarwinRelayWorker& worker,
        const TqDarwinRelayRegistrationResult& result,
        const std::shared_ptr<TqRelayStopControl>& control) {
        if (!result.Ok || control == nullptr) {
            return;
        }
        handle.Stop.store(false, std::memory_order_release);
        handle.Control = control;
        handle.ControlGeneration = control->Generation;
        handle.Backend = TqRelayBackendType::DarwinWorker;
        handle.DarwinWorker = &worker;
        handle.DarwinRelayId = result.RelayId;
    }

    static void FillControl(TqDarwinRelayRegistration& registration, TqRelayHandle& handle) {
        registration.Control = handle.Control;
        registration.ControlGeneration =
            handle.Control != nullptr ? handle.Control->Generation : 0;
    }

    bool Register(bool enableQuicSends = false) {
        Stream = reinterpret_cast<MsQuicStream*>(StreamStorage);
        Stream->Callback = TqStreamLifetime::Callback;
        Stream->Context = nullptr;
        Owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
        Stream->Context = Owner.get();
        if (!Worker.StartForTest()) {
            return false;
        }
        TqDarwinRelayRegistration registration{};
        registration.TcpFd = Fds[1];
        registration.Stream = Stream;
        registration.StreamOwner = Owner;
        FillControl(registration, Handle);
        registration.EnableQuicSends = enableQuicSends;
        Result = Worker.RegisterRelayWithId(registration);
        if (Result.Ok) {
            PublishHandle(Handle, Worker, Result, registration.Control);
        }
        return Result.Ok;
    }

    QUIC_STATUS DispatchViaRouter(QUIC_STREAM_EVENT& event) {
        return Owner->DispatchForTest(&event);
    }

    void StopAndClosePeer(bool leavePendingEvents = false) {
        if (!leavePendingEvents) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (Worker.PendingEventsForTest() != 0 &&
                   std::chrono::steady_clock::now() < deadline) {
                if (!Worker.DrainOneEventForTest()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
        Worker.Stop();
        if (Stream != nullptr) {
            Stream->Callback = MsQuicStream::NoOpCallback;
            Stream->Context = nullptr;
        }
        Owner.reset();
        if (Fds[0] != TqInvalidSocket) {
            ::close(Fds[0]);
            Fds[0] = TqInvalidSocket;
        }
    }
};

bool OwnerRouteTargetPresent(const std::shared_ptr<TqStreamLifetime>& owner) {
    auto probe = std::make_shared<CountingTarget>();
    return owner->PublishTarget(owner->RouteGeneration(), probe);
}

struct ScopedFakeMsQuicApi {
    const MsQuicApi* Previous{MsQuic};
    QUIC_API_TABLE Table{};

    ScopedFakeMsQuicApi() {
        Table.SetCallbackHandler = FakeSetCallbackHandler;
        Table.StreamClose = FakeStreamClose;
        Table.StreamShutdown = FakeStreamShutdown;
        MsQuic = reinterpret_cast<const MsQuicApi*>(&Table);
        g_fakeHandler = nullptr;
        g_fakeHandlerContext = nullptr;
        g_fakeCloseCount = 0;
        g_fakeShutdownCount = 0;
        g_fakeLastShutdownFlags = QUIC_STREAM_SHUTDOWN_FLAG_NONE;
        g_fakeLastShutdownErrorCode = 0;
        g_fakeShutdownFail = false;
        g_fakeShutdownCalls.clear();
    }

    ~ScopedFakeMsQuicApi() {
        MsQuic = Previous;
    }
};

struct AdoptedManagedHarness {
    ScopedFakeMsQuicApi FakeApi;
    TqDarwinRelayWorkerConfig Config{};
    int Fds[2]{TqInvalidSocket, TqInvalidSocket};
    TqRelayHandle Handle{};
    std::shared_ptr<TqStreamLifetime> Owner;
    TqDarwinRelayRegistrationResult Result{};
    HQUIC Raw{nullptr};
    TqDarwinRelayWorker Worker;

    explicit AdoptedManagedHarness(TqDarwinRelayWorkerConfig config = {})
        : Config(std::move(config)), Worker(this->Config) {}

    bool OpenSocketPair() {
        return socketpair(AF_UNIX, SOCK_STREAM, 0, Fds) == 0;
    }

    bool Register(std::shared_ptr<TqStreamLifetime::Target> target = nullptr) {
        if (target == nullptr) {
            target = std::make_shared<CountingTarget>();
        }
        Raw = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x7000 + g_fakeCloseCount));
        const uint64_t id = reinterpret_cast<uintptr_t>(Raw);
        Owner = TqStreamLifetime::AdoptAccepted(
            Raw,
            std::move(target),
            TqTerminalIdentity{
                id, id, id, id,
                TqTunnelRole::ServerOpen, TqRelayBackendType::DarwinWorker},
            5);
        if (Owner == nullptr) {
            return false;
        }
        if (!Worker.StartForTest()) {
            return false;
        }
        if (Handle.Control == nullptr) {
            Handle.Control = std::make_shared<TqRelayStopControl>();
            Handle.ControlGeneration = Handle.Control->Generation;
        }
        TqDarwinRelayRegistration registration{};
        registration.TcpFd = Fds[1];
        registration.Stream = Owner->StreamForInitialization();
        registration.StreamOwner = Owner;
        ManagedRelayHarness::FillControl(registration, Handle);
        Result = Worker.RegisterRelayWithId(registration);
        if (Result.Ok) {
            ManagedRelayHarness::PublishHandle(Handle, Worker, Result, registration.Control);
        }
        return Result.Ok;
    }

    QUIC_STATUS DispatchViaRouter(QUIC_STREAM_EVENT& event) {
        return Owner->DispatchForTest(&event);
    }

    void StopAndClosePeer(bool leavePendingEvents = false) {
        if (!leavePendingEvents) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (Worker.PendingEventsForTest() != 0 &&
                   std::chrono::steady_clock::now() < deadline) {
                if (!Worker.DrainOneEventForTest()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }
        Worker.Stop();
        Owner.reset();
        if (Fds[0] != TqInvalidSocket) {
            ::close(Fds[0]);
            Fds[0] = TqInvalidSocket;
        }
    }
};

bool TcpRelayFdClosedOnce(int fd) {
    errno = 0;
    return fcntl(fd, F_GETFD) == -1 && errno == EBADF;
}

void SynchronizeConcurrentStart(std::atomic<int>& gate, std::mutex& mutex, std::condition_variable& cv) {
    std::unique_lock<std::mutex> lock(mutex);
    const int arrived = gate.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (arrived < 2) {
        cv.wait(lock, [&] { return gate.load(std::memory_order_acquire) >= 2; });
    } else {
        cv.notify_all();
    }
}

void AfterManagedPublishHook(TqDarwinRelayWorker* worker, uint64_t) {
    g_PrecommitUncommitted =
        worker != nullptr && worker->CommittedRelayCountForTest() == 0;
    if (g_PrecommitPublishTerminal) {
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        g_PrecommitReceiveStatus = g_PrecommitOwner->DispatchForTest(&terminal);
        return;
    }
    QUIC_BUFFER buffer{};
    buffer.Buffer = g_PrecommitPayload.data();
    buffer.Length = static_cast<uint32_t>(g_PrecommitPayload.size());
    QUIC_STREAM_EVENT receive{};
    receive.Type = QUIC_STREAM_EVENT_RECEIVE;
    receive.RECEIVE.BufferCount = 1;
    receive.RECEIVE.Buffers = &buffer;
    g_PrecommitReceiveStatus = g_PrecommitOwner->DispatchForTest(&receive);
}

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

struct ShutdownDuringCompressCompressor final : ITqCompressor {
    explicit ShutdownDuringCompressCompressor(MsQuicStream* stream) : Stream(stream) {}

    bool Compress(const uint8_t*, size_t inLen, std::vector<uint8_t>& out, bool) override {
        InputBytes.fetch_add(inLen, std::memory_order_relaxed);
        if (!ShutdownDispatched.exchange(true, std::memory_order_acq_rel)) {
            if (Stream != nullptr && Stream->Context != nullptr) {
                QUIC_STREAM_EVENT shutdown{};
                shutdown.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
                CallbackOk.store(
                    Stream->Callback(Stream, Stream->Context, &shutdown) == QUIC_STATUS_SUCCESS,
                    std::memory_order_release);
            }
        }
        out.push_back(0x51);
        return true;
    }

    bool Flush(std::vector<uint8_t>& out) override {
        out.push_back(0x5a);
        return true;
    }

    void Reset() override {}

    MsQuicStream* Stream{nullptr};
    std::atomic<bool> ShutdownDispatched{false};
    std::atomic<bool> CallbackOk{false};
    std::atomic<uint64_t> InputBytes{0};
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

QUIC_STATUS FakeStreamSendViaOwner(
    MsQuicStream*,
    const QUIC_BUFFER*,
    uint32_t,
    QUIC_SEND_FLAGS,
    void* context) {
    g_sendCalls.fetch_add(1, std::memory_order_relaxed);
    g_lastSendContext.store(context, std::memory_order_release);
    if (g_completeBeforeSendReturns.load(std::memory_order_acquire)) {
        CHECK(g_SendRegisterBarrierOwner != nullptr);
        QUIC_STREAM_EVENT event{};
        event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
        event.SEND_COMPLETE.ClientContext = context;
        CHECK(g_SendRegisterBarrierOwner->DispatchForTest(&event) == QUIC_STATUS_SUCCESS);
    }
    return g_sendStatus.load(std::memory_order_acquire);
}

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

QUIC_STATUS FakeStreamSendCompletesSynchronously(
    MsQuicStream*,
    const QUIC_BUFFER*,
    uint32_t,
    QUIC_SEND_FLAGS,
    void* context) {
    g_sendCalls.fetch_add(1, std::memory_order_relaxed);
    g_lastSendContext.store(context, std::memory_order_release);
    void* callbackContext = g_syncCallbackContext.load(std::memory_order_acquire);
    CHECK(callbackContext != nullptr);
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = context;
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
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



inline TqDarwinRelayRegistrationResult RegisterAndPublish(
    TqDarwinRelayWorker& worker,
    TqDarwinRelayRegistration& registration,
    TqRelayHandle& handle) {
    if (handle.Control == nullptr) {
        handle.Control = std::make_shared<TqRelayStopControl>();
        handle.ControlGeneration = handle.Control->Generation;
    }
    ManagedRelayHarness::FillControl(registration, handle);
    TqDarwinRelayRegistrationResult result = worker.RegisterRelayWithId(registration);
    if (result.Ok) {
        ManagedRelayHarness::PublishHandle(handle, worker, result, registration.Control);
    }
    return result;
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

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);
    worker.SetWakeFailuresForTest(2);
    CHECK(worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(worker.EnqueueForTest(TestMarkerEvent(2)));
    CHECK(worker.PendingEventsForTest() == 2);

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
    CHECK(handle.Control != nullptr);
    CHECK(handle.Control->Stop.load(std::memory_order_acquire));
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void SnapshotRetriesAfterFullQueue() {
    TqDarwinRelayWorkerConfig config{};
    config.EventQueueCapacity = 2;
    TqDarwinRelayWorker worker(config);
    CHECK(worker.StartForTest());

    CHECK(worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(worker.EnqueueForTest(TestMarkerEvent(2)));

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

    bool doneBeforeDrain = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        doneBeforeDrain = cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return done; });
    }
    CHECK(!doneBeforeDrain);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        (void)worker.DrainOneEventForTest();
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (cv.wait_for(lock, std::chrono::milliseconds(10), [&] { return done; })) {
                break;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(done);
    }
    snapshotThread.join();
    CHECK(snapshot.PendingEvents == 0);
    worker.Stop();
}

#if defined(TQ_UNIT_TESTING)
struct SnapshotDispatchHoldState {
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Entered{false};
    bool Release{false};
};

SnapshotDispatchHoldState* g_SnapshotDispatchHold = nullptr;

void HoldSnapshotDispatchUntilReleased(TqDarwinRelayWorker*) {
    auto* state = g_SnapshotDispatchHold;
    CHECK(state != nullptr);
    std::unique_lock<std::mutex> lock(state->Mutex);
    state->Entered = true;
    state->Cv.notify_all();
    state->Cv.wait(lock, [&] { return state->Release; });
}

void SnapshotDeadlineReturnsIncompleteWhenDispatchBlocked() {
    SnapshotDispatchHoldState hold;
    g_SnapshotDispatchHold = &hold;

    TqDarwinRelayWorkerConfig config{};
    config.WorkerIndex = 2;
    config.EventQueueCapacity = 16;
    config.BeforeSnapshotHookForTest = HoldSnapshotDispatchUntilReleased;
    TqDarwinRelayWorker worker(config);
    CHECK(worker.Start());

    TqRelayRuntimeSnapshotExecutionGate gate;
    auto permit = gate.TryAcquire(
        std::chrono::steady_clock::now() + std::chrono::seconds(1));
    CHECK(permit != nullptr);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    const TqDarwinRelayWorkerSnapshot snapshot =
        worker.Snapshot(deadline, std::move(permit));
    CHECK(!snapshot.SnapshotComplete);
    CHECK(snapshot.WorkerIndex == 2);
    CHECK(snapshot.EventQueueCapacity == 16);

    {
        std::unique_lock<std::mutex> lock(hold.Mutex);
        CHECK(hold.Cv.wait_for(lock, std::chrono::seconds(2), [&] { return hold.Entered; }));
    }
    // Permit stays busy while the detached late command is outstanding.
    CHECK(gate.Stats().Busy == 1u);

    {
        std::lock_guard<std::mutex> lock(hold.Mutex);
        hold.Release = true;
    }
    hold.Cv.notify_all();

    const auto releaseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < releaseDeadline) {
        if (gate.Stats().Busy == 0u) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(gate.Stats().Busy == 0u);

    const TqDarwinRelayWorkerSnapshot after = worker.Snapshot();
    CHECK(after.SnapshotComplete);
    CHECK(after.WorkerIndex == 2);
    worker.Stop();
    g_SnapshotDispatchHold = nullptr;
}

void ConsecutiveSnapshotTimeoutsDoNotDangleCommands() {
    SnapshotDispatchHoldState hold;
    g_SnapshotDispatchHold = &hold;

    TqDarwinRelayWorkerConfig config{};
    config.WorkerIndex = 4;
    config.EventQueueCapacity = 32;
    config.BeforeSnapshotHookForTest = HoldSnapshotDispatchUntilReleased;
    TqDarwinRelayWorker worker(config);
    CHECK(worker.Start());

    const auto first = worker.Snapshot(
        std::chrono::steady_clock::now() + std::chrono::milliseconds(150));
    CHECK(!first.SnapshotComplete);

    {
        std::unique_lock<std::mutex> lock(hold.Mutex);
        CHECK(hold.Cv.wait_for(lock, std::chrono::seconds(2), [&] { return hold.Entered; }));
    }

    // Second timeout while the worker is still blocked on the first command.
    // Both commands are heap shared_ptr — late completion must not UAF.
    const auto second = worker.Snapshot(
        std::chrono::steady_clock::now() + std::chrono::milliseconds(150));
    CHECK(!second.SnapshotComplete);

    {
        std::lock_guard<std::mutex> lock(hold.Mutex);
        hold.Release = true;
    }
    hold.Cv.notify_all();

    const auto third = worker.Snapshot(
        std::chrono::steady_clock::now() + std::chrono::seconds(2));
    CHECK(third.SnapshotComplete);
    CHECK(third.WorkerIndex == 4);
    worker.Stop();
    g_SnapshotDispatchHold = nullptr;
}

void SnapshotLocalThrowKeepsWorkerAliveAndReturnsIncomplete() {
    TqDarwinRelayWorkerConfig config{};
    config.WorkerIndex = 5;
    config.EventQueueCapacity = 16;
    config.FailNextSnapshotLocalForTest = true;
    TqDarwinRelayWorker worker(config);
    CHECK(worker.Start());

    const TqDarwinRelayWorkerSnapshot failed = worker.Snapshot(
        std::chrono::steady_clock::now() + std::chrono::seconds(2));
    CHECK(!failed.SnapshotComplete);
    CHECK(failed.WorkerIndex == 5);
    CHECK(worker.Snapshot().Errors >= 1);

    const TqDarwinRelayWorkerSnapshot recovered = worker.Snapshot(
        std::chrono::steady_clock::now() + std::chrono::seconds(2));
    CHECK(recovered.SnapshotComplete);
    CHECK(recovered.WorkerIndex == 5);
    worker.Stop();
}

void StopPurgesDetachedSnapshotAndReleasesPermit() {
    TqDarwinRelayWorkerConfig config{};
    config.WorkerIndex = 9;
    config.EventQueueCapacity = 16;
    TqDarwinRelayWorker worker(config);
    CHECK(worker.StartForTest());

    TqRelayRuntimeSnapshotExecutionGate gate;
    auto permit = gate.TryAcquire(
        std::chrono::steady_clock::now() + std::chrono::seconds(1));
    CHECK(permit != nullptr);
    CHECK(worker.EnqueueDetachedSnapshotCommandForTest(std::move(permit)));
    CHECK(gate.Stats().Busy == 1u);
    CHECK(gate.Stats().Outstanding == 1u);
    CHECK(worker.PendingEventsForTest() >= 1);

    worker.Stop();
    CHECK(worker.PendingEventsForTest() == 0);
    CHECK(gate.Stats().Busy == 0u);
    CHECK(gate.Stats().Outstanding == 0u);
}

std::mutex g_RuntimeSnapshotHookMutex;
std::condition_variable g_RuntimeSnapshotHookCv;
bool g_RuntimeSnapshotHookEntered = false;
bool g_RuntimeSnapshotHookRelease = false;
std::atomic<uint32_t> g_RuntimeSnapshotHookEnterCount{0};

void BlockFirstRuntimeWorkerSnapshot(TqDarwinRelayWorker*) {
    g_RuntimeSnapshotHookEnterCount.fetch_add(1, std::memory_order_acq_rel);
    std::unique_lock<std::mutex> lock(g_RuntimeSnapshotHookMutex);
    g_RuntimeSnapshotHookEntered = true;
    g_RuntimeSnapshotHookCv.notify_all();
    g_RuntimeSnapshotHookCv.wait(lock, []() {
        return g_RuntimeSnapshotHookRelease;
    });
}

void DarwinRuntimeSnapshotWorkersIdentityAndCapacity() {
    auto& runtime = TqDarwinRelayRuntime::Instance();
    runtime.Stop();

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 2;
    tuning.RelayEventQueueCapacity = 1000; // non-power-of-2 → normalized 1024
    CHECK(runtime.Start(tuning));
    CHECK(runtime.Start(tuning)); // Running Start is idempotent

    const auto workers = runtime.SnapshotWorkers();
    CHECK(workers.SnapshotComplete);
    CHECK(workers.IdentitiesComplete);
    CHECK(workers.Workers.size() == 2);
    CHECK(workers.Workers[0].WorkerIndex == 0);
    CHECK(workers.Workers[1].WorkerIndex == 1);
    const uint64_t expectedCapacity =
        TqDarwinRelayEventQueue::NormalizeCapacityForTest(1000);
    CHECK(expectedCapacity == 1024);
    CHECK(workers.Workers[0].EventQueueCapacity == expectedCapacity);
    CHECK(workers.Workers[1].EventQueueCapacity == expectedCapacity);

    const auto aggregate = runtime.Snapshot();
    CHECK(aggregate.SnapshotComplete);
    CHECK(aggregate.EventQueueCapacity == expectedCapacity);

    runtime.Stop();
    const auto stoppedWorkers = runtime.SnapshotWorkers();
    CHECK(stoppedWorkers.SnapshotComplete);
    CHECK(stoppedWorkers.IdentitiesComplete);
    CHECK(stoppedWorkers.Workers.empty());
    const auto stoppedAggregate = runtime.Snapshot();
    CHECK(stoppedAggregate.SnapshotComplete);
}

void DarwinRuntimeTqRelayStartSetsWorkerIndexIdentity() {
    // Production path: TqRelayStartManaged publishes DarwinWorkerIndex from the
    // accepting worker slot. Do not assign the field manually.
    auto& runtime = TqDarwinRelayRuntime::Instance();
    runtime.Stop();

    ScopedFakeMsQuicApi fakeApi;

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 2;
    tuning.RelayEventQueueCapacity = 1000;

    struct StartedRelay {
        int Fds[2]{TqInvalidSocket, TqInvalidSocket};
        alignas(MsQuicStream) unsigned char StreamStorage[sizeof(MsQuicStream)]{};
        MsQuicStream* Stream{nullptr};
        std::shared_ptr<TqStreamLifetime> Owner;
        TqRelayHandle Handle{};
    };

    StartedRelay relays[2]{};
    uint32_t seenMask = 0;
    for (uint32_t i = 0; i < 2; ++i) {
        auto& relay = relays[i];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, relay.Fds) == 0);
        relay.Stream = reinterpret_cast<MsQuicStream*>(relay.StreamStorage);
        std::memset(relay.Stream, 0, sizeof(MsQuicStream));
        relay.Stream->Callback = MsQuicStream::NoOpCallback;
        relay.Stream->Handle =
            reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0xE300 + i));
        relay.Owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
        CHECK(relay.Owner != nullptr);
        CHECK(relay.Owner->InstallStreamForTest(relay.Stream));

        bool tcpFdConsumed = false;
        CHECK(TqRelayStartManaged(
            relay.Fds[1],
            relay.Stream,
            relay.Owner,
            nullptr,
            nullptr,
            &relay.Handle,
            tuning,
            TqCompressAlgo::None,
            &tcpFdConsumed));
        CHECK(tcpFdConsumed);
        CHECK(relay.Handle.Backend == TqRelayBackendType::DarwinWorker);
        CHECK(relay.Handle.DarwinWorker != nullptr);
        CHECK(relay.Handle.DarwinRelayId != 0);
        // Field must come from production start, matching the worker slot.
        CHECK(relay.Handle.DarwinWorkerIndex == relay.Handle.DarwinWorker->WorkerIndex());
        CHECK(relay.Handle.DarwinWorkerIndex < 2);
        seenMask |= (1u << relay.Handle.DarwinWorkerIndex);
    }
    CHECK(seenMask == 0x3u);
    CHECK(relays[0].Handle.DarwinWorkerIndex != relays[1].Handle.DarwinWorkerIndex);

    const auto snapshots = runtime.SnapshotWorkers();
    CHECK(snapshots.SnapshotComplete);
    CHECK(snapshots.Workers.size() == 2);
    CHECK(snapshots.Workers[0].WorkerIndex == 0);
    CHECK(snapshots.Workers[1].WorkerIndex == 1);

    for (auto& relay : relays) {
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        CHECK(relay.Owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
        TqRelayStop(&relay.Handle);
        CHECK(relay.Handle.DarwinWorkerIndex == 0);
        CHECK(relay.Handle.DarwinWorker == nullptr);
        relay.Owner->ReleaseStreamForTest();
        ::close(relay.Fds[0]);
    }
    runtime.Stop();
}

void DarwinRuntimeSnapshotTimeoutHoldsPermitAndBlocksSecondRequest() {
    auto& runtime = TqDarwinRelayRuntime::Instance();
    runtime.Stop();
    {
        std::lock_guard<std::mutex> lock(g_RuntimeSnapshotHookMutex);
        g_RuntimeSnapshotHookEntered = false;
        g_RuntimeSnapshotHookRelease = false;
    }
    g_RuntimeSnapshotHookEnterCount.store(0, std::memory_order_release);
    runtime.SetBeforeWorkerSnapshotHookForTest(BlockFirstRuntimeWorkerSnapshot);

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 2;
    tuning.RelayEventQueueCapacity = 2048;
    CHECK(runtime.Start(tuning));

    std::mutex resultMutex;
    std::condition_variable resultCv;
    bool resultReady = false;
    TqRelayRuntimeSnapshotResult<TqDarwinRelayWorkerSnapshot> first{};
    std::thread snapshotA([&]() {
        const auto sampled = runtime.SnapshotWorkers(
            std::chrono::steady_clock::now() + std::chrono::milliseconds(150));
        {
            std::lock_guard<std::mutex> lock(resultMutex);
            first = sampled;
            resultReady = true;
        }
        resultCv.notify_all();
    });

    {
        std::unique_lock<std::mutex> lock(g_RuntimeSnapshotHookMutex);
        CHECK(g_RuntimeSnapshotHookCv.wait_for(lock, std::chrono::seconds(2), []() {
            return g_RuntimeSnapshotHookEntered;
        }));
    }
    {
        std::unique_lock<std::mutex> lock(resultMutex);
        CHECK(resultCv.wait_for(lock, std::chrono::seconds(2), [&]() {
            return resultReady;
        }));
    }
    snapshotA.join();
    CHECK(!first.SnapshotComplete);
    CHECK(first.IdentitiesComplete);
    CHECK(first.Workers.size() == 2);
    CHECK(runtime.SnapshotExecutionGateStatsForTest().Outstanding <= 1u);
    CHECK(runtime.SnapshotExecutionGateStatsForTest().Busy == 1u);

    const uint32_t entersBeforeB =
        g_RuntimeSnapshotHookEnterCount.load(std::memory_order_acquire);
    const auto second = runtime.SnapshotWorkers(std::chrono::steady_clock::now());
    CHECK(!second.SnapshotComplete);
    CHECK(!second.IdentitiesComplete);
    CHECK(second.Workers.empty());
    CHECK(g_RuntimeSnapshotHookEnterCount.load(std::memory_order_acquire) ==
          entersBeforeB);
    CHECK(runtime.SnapshotExecutionGateStatsForTest().Outstanding <= 1u);

    {
        std::lock_guard<std::mutex> lock(g_RuntimeSnapshotHookMutex);
        g_RuntimeSnapshotHookRelease = true;
    }
    g_RuntimeSnapshotHookCv.notify_all();

    const auto releaseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < releaseDeadline) {
        if (runtime.SnapshotExecutionGateStatsForTest().Busy == 0u) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(runtime.SnapshotExecutionGateStatsForTest().Busy == 0u);
    runtime.Stop();
    runtime.SetBeforeWorkerSnapshotHookForTest(nullptr);
}

void DarwinRuntimeStopWaitsForSnapshotLease() {
    auto& runtime = TqDarwinRelayRuntime::Instance();
    runtime.Stop();
    {
        std::lock_guard<std::mutex> lock(g_RuntimeSnapshotHookMutex);
        g_RuntimeSnapshotHookEntered = false;
        g_RuntimeSnapshotHookRelease = false;
    }
    runtime.SetBeforeWorkerSnapshotHookForTest(BlockFirstRuntimeWorkerSnapshot);

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 2;
    tuning.RelayEventQueueCapacity = 2048;
    CHECK(runtime.Start(tuning));

    std::thread snapshotThread([&]() {
        (void)runtime.SnapshotWorkers(
            std::chrono::steady_clock::now() + std::chrono::seconds(5));
    });

    {
        std::unique_lock<std::mutex> lock(g_RuntimeSnapshotHookMutex);
        CHECK(g_RuntimeSnapshotHookCv.wait_for(lock, std::chrono::seconds(2), []() {
            return g_RuntimeSnapshotHookEntered;
        }));
    }

    std::mutex stopMutex;
    std::condition_variable stopCv;
    bool stopEntered = false;
    bool stopFinished = false;
    std::thread stopThread([&]() {
        {
            std::lock_guard<std::mutex> lock(stopMutex);
            stopEntered = true;
        }
        stopCv.notify_all();
        runtime.Stop();
        {
            std::lock_guard<std::mutex> lock(stopMutex);
            stopFinished = true;
        }
        stopCv.notify_all();
    });

    {
        std::unique_lock<std::mutex> lock(stopMutex);
        CHECK(stopCv.wait_for(lock, std::chrono::seconds(2), [&]() {
            return stopEntered;
        }));
        // Stop must wait for the snapshot lease before finishing worker teardown.
        CHECK(!stopCv.wait_for(lock, std::chrono::milliseconds(100), [&]() {
            return stopFinished;
        }));
        CHECK(!stopFinished);
    }

    {
        std::lock_guard<std::mutex> lock(g_RuntimeSnapshotHookMutex);
        g_RuntimeSnapshotHookRelease = true;
    }
    g_RuntimeSnapshotHookCv.notify_all();

    {
        std::unique_lock<std::mutex> lock(stopMutex);
        CHECK(stopCv.wait_for(lock, std::chrono::seconds(2), [&]() {
            return stopFinished;
        }));
    }
    snapshotThread.join();
    stopThread.join();
    runtime.SetBeforeWorkerSnapshotHookForTest(nullptr);
}

void DarwinRuntimeStartFailureRollsBackAndRetryUsesFullRange() {
    auto& runtime = TqDarwinRelayRuntime::Instance();
    runtime.Stop();
    runtime.SetFailStartWorkerIndexForTest(1);

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 2;
    tuning.RelayEventQueueCapacity = 2048;
    CHECK(!runtime.Start(tuning));
    const auto stopped = runtime.SnapshotWorkers();
    CHECK(stopped.SnapshotComplete);
    CHECK(stopped.IdentitiesComplete);
    CHECK(stopped.Workers.empty());
    CHECK(runtime.PickWorker() == nullptr);

    runtime.SetFailStartWorkerIndexForTest(-1);
    CHECK(runtime.Start(tuning));
    const auto restarted = runtime.SnapshotWorkers();
    runtime.Stop();
    CHECK(restarted.SnapshotComplete);
    CHECK(restarted.IdentitiesComplete);
    CHECK(restarted.Workers.size() == 2);
    CHECK(restarted.Workers[0].WorkerIndex == 0);
    CHECK(restarted.Workers[1].WorkerIndex == 1);
}

void DarwinRuntimePickWorkerNotBlockedDuringSnapshot() {
    auto& runtime = TqDarwinRelayRuntime::Instance();
    runtime.Stop();
    {
        std::lock_guard<std::mutex> lock(g_RuntimeSnapshotHookMutex);
        g_RuntimeSnapshotHookEntered = false;
        g_RuntimeSnapshotHookRelease = false;
    }
    runtime.SetBeforeWorkerSnapshotHookForTest(BlockFirstRuntimeWorkerSnapshot);

    TqTuningConfig tuning{};
    tuning.RelayWorkerCount = 2;
    tuning.RelayEventQueueCapacity = 2048;
    CHECK(runtime.Start(tuning));

    std::atomic<bool> snapshotDone{false};
    std::thread snapshotThread([&]() {
        (void)runtime.SnapshotWorkers(
            std::chrono::steady_clock::now() + std::chrono::seconds(5));
        snapshotDone.store(true, std::memory_order_release);
    });

    {
        std::unique_lock<std::mutex> lock(g_RuntimeSnapshotHookMutex);
        CHECK(g_RuntimeSnapshotHookCv.wait_for(lock, std::chrono::seconds(2), []() {
            return g_RuntimeSnapshotHookEntered;
        }));
    }

    // Runtime lock is released while worker Snapshot waits, so PickWorker must
    // succeed without waiting for the in-flight snapshot command.
    TqDarwinRelayWorker* picked = runtime.PickWorker();
    CHECK(picked != nullptr);
    CHECK(!snapshotDone.load(std::memory_order_acquire));

    {
        std::lock_guard<std::mutex> lock(g_RuntimeSnapshotHookMutex);
        g_RuntimeSnapshotHookRelease = true;
    }
    g_RuntimeSnapshotHookCv.notify_all();
    snapshotThread.join();
    runtime.Stop();
    runtime.SetBeforeWorkerSnapshotHookForTest(nullptr);
}
#endif

void UnregisterQueuesDuringStartupWindow() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);

    worker.MarkWorkerThreadExitedForTest();

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

    bool doneBeforeDrain = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        doneBeforeDrain = cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return done; });
    }
    CHECK(!doneBeforeDrain);

    CHECK(worker.DrainOneEventForTest());
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(2), [&] { return done; }));
    }
    unregisterThread.join();

    worker.Stop();
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

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(handle.Control != nullptr);
    CHECK(handle.Control->Stop.load(std::memory_order_acquire));

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
    registration.Control = handle.Control;
    registration.ControlGeneration = handle.Control->Generation;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);
    CHECK(worker.Snapshot().ActiveRelays == 1);

    worker.UnregisterRelay(result.RelayId);

    CHECK(handle.Control->Stop.load(std::memory_order_acquire));
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
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    registration.CompressAlgo = TqCompressAlgo::Zstd;
    registration.Compressor = &compressor;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(handle.Control != nullptr);
    CHECK(handle.Control->Stop.load(std::memory_order_acquire));

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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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

void SendOperationStateTransitionsAreSingleClaim() {
    TqDarwinRelaySendOperation operation{};
    CHECK(operation.TryMarkRegistered());
    CHECK(!operation.TryMarkRegistered());
    CHECK(operation.TryClaimCompletion());
    CHECK(!operation.TryClaimCompletion());
    CHECK(!operation.MarkDetached());
    CHECK(operation.TryMarkCompleted());
    CHECK(!operation.TryMarkCompleted());
    CHECK(!operation.MarkDetached());
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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

void ActiveWorkerSendCompleteDoesNotUseKnownSendLocks() {
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);

    const uint64_t knownBefore = worker.KnownSendLockedCountForTest();
    const uint64_t completionBefore = worker.CompletionStateLockedCountForTest();
    const uint64_t fallbackBefore = worker.FallbackSendCompletionCountForTest();
    const uint64_t localRegisterBefore = worker.ActiveSendLocalRegisterCountForTest();
    const uint64_t localCompleteBefore = worker.ActiveSendLocalCompleteCountForTest();

    const char payload[] = "active-worker-send-lock-characterization";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 1);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);

    CHECK(worker.CompleteOneInFlightSendForTest(result.RelayId) != 0);
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.KnownSendOperationCountForTest() == 0);
    CHECK(worker.ActiveSendLocalRegisterCountForTest() > localRegisterBefore);
    CHECK(worker.ActiveSendLocalCompleteCountForTest() > localCompleteBefore);
    CHECK(worker.KnownSendLockedCountForTest() == knownBefore);
    CHECK(worker.CompletionStateLockedCountForTest() == completionBefore);
    CHECK(worker.FallbackSendCompletionCountForTest() == fallbackBefore);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void AsyncSendCompleteCallbackDoesNotLockCompletionState() {
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);
    const char payload[] = "async-callback-no-completion-lock";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));

    void* callbackContext = worker.StreamCallbackContextForTest(result.RelayId);
    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(callbackContext != nullptr);
    CHECK(sendContext != nullptr);
    const uint64_t completionBefore = worker.CompletionStateLockedCountForTest();

    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.CompletionStateLockedCountForTest() == completionBefore);
    CHECK(worker.KnownSendOperationCountForTest() == 0);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void SendCompleteEnqueueFailureSettlesWithoutBlocking() {
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(worker.PendingEventsForTest() == 2);

    g_sendStatus.store(QUIC_STATUS_SUCCESS, std::memory_order_release);
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);

    while (worker.PendingEventsForTest() > 0) {
        CHECK(worker.DrainOneEventForTest());
    }

    void* retrySendContext = g_lastSendContext.load(std::memory_order_acquire);
    if (worker.InFlightQuicSendCountForTest(result.RelayId) != 0 &&
        retrySendContext != nullptr &&
        retrySendContext != sendContext) {
        QUIC_STREAM_EVENT retryComplete{};
        retryComplete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
        retryComplete.SEND_COMPLETE.ClientContext = retrySendContext;
        CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &retryComplete) ==
            QUIC_STATUS_SUCCESS);
        while (worker.PendingEventsForTest() > 0) {
            CHECK(worker.DrainOneEventForTest());
        }
    }

    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.PendingQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.KnownSendOperationCountForTest() == 0);
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 3);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void SendCompleteAfterRunningFalseSettlesOnCallback() {
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(worker.PendingEventsForTest() == 2);

    worker.SetRunningForTest(false);
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    event.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, callbackContext, &event) == QUIC_STATUS_SUCCESS);
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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

void SynchronousSendCompleteDoesNotDoubleComplete() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 16;
    config.MaxBufferedQuicSendBytes = 64 * 1024;
    TqDarwinRelayWorker worker(config);
    worker.SetStreamSendForTest(FakeStreamSendCompletesSynchronously);
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);
    void* callbackContext = worker.StreamCallbackContextForTest(result.RelayId);
    CHECK(callbackContext != nullptr);
    g_syncCallbackContext.store(callbackContext, std::memory_order_release);
    g_completeBeforeSendReturns.store(true, std::memory_order_release);
    const char payload[] = "synchronous-send-complete";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 0);
    CHECK(worker.KnownSendOperationCountForTest() == 0);
    CHECK(worker.PendingEventsForTest() == 0);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    worker.SetStreamSendForTest(nullptr);
    g_completeBeforeSendReturns.store(false, std::memory_order_release);
    g_syncCallbackContext.store(nullptr, std::memory_order_release);
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(handle.Control != nullptr);
    CHECK(handle.Control->Stop.load(std::memory_order_acquire));

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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(stream->Callback == TqDarwinRelayWorker::StreamCallback);
    CHECK(stream->Context != nullptr);

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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(stream->Callback == TqDarwinRelayWorker::StreamCallback);
    CHECK(stream->Context != nullptr);

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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(handle.Control != nullptr);
    CHECK(handle.Control->Stop.load(std::memory_order_acquire));

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
        registration.EnableQuicSends = true;

        TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
        worker.Stop();
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
        CHECK(worker.DrainOneEventForTest());
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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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

void StreamShutdownDuringTcpBatchBlocksQuicSend() {
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
    ShutdownDuringCompressCompressor compressor(stream);
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = stream;
    registration.Compressor = &compressor;
    registration.CompressAlgo = TqCompressAlgo::Zstd;
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);

    const char payload[] = "shutdown-during-batch";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    CHECK(compressor.ShutdownDispatched.load(std::memory_order_acquire));
    CHECK(compressor.CallbackOk.load(std::memory_order_acquire));
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 0);

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
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(handle.Control != nullptr);
    CHECK(handle.Control->Stop.load(std::memory_order_acquire));

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
    registration.EnableQuicSends = true;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(handle.Control != nullptr);
    CHECK(handle.Control->Stop.load(std::memory_order_acquire));

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
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);
    CHECK(worker.Snapshot().ActiveRelays == 1);

    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, EV_ERROR, ECONNRESET));
    TqDarwinRelayWorkerSnapshot snapshot = worker.Snapshot();
    CHECK(snapshot.ActiveRelays == 0);
    CHECK(handle.Control != nullptr);
    CHECK(handle.Control->Stop.load(std::memory_order_acquire));

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
    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);
    CHECK(handle.Backend == TqRelayBackendType::DarwinWorker);

    worker.Stop();

    TqDarwinRelayWorkerSnapshot snapshot = worker.Snapshot();
    CHECK(snapshot.ActiveRelays == 0);
    CHECK(handle.Control != nullptr);
    CHECK(handle.Control->Stop.load(std::memory_order_acquire));

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
    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(!result.Ok);
    CHECK(result.RelayId == 0);

    TqDarwinRelayWorkerSnapshot snapshot = worker.Snapshot();
    CHECK(snapshot.ActiveRelays == 0);
    // Failure rolls back before public publish; control may be signaled by shutdown.
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
    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(!result.Ok);
    CHECK(result.RelayId == 0);

    TqDarwinRelayWorkerSnapshot snapshot = worker.Snapshot();
    CHECK(snapshot.ActiveRelays == 0);
    CHECK(handle.Backend == TqRelayBackendType::None);
    CHECK(handle.DarwinWorker == nullptr);
    CHECK(handle.DarwinRelayId == 0);
    CHECK(!handle.Control->Stop.load(std::memory_order_acquire));

    CloseSocketPairBoth(fds);
}

void ReceiveCallbackQueueFullReturnsPendingWithoutCompleting() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeReceiveComplete();
    ResetFakeReceiveSetEnabled(QUIC_STATUS_SUCCESS);

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 2;
    config.MaxPendingQuicReceiveBytesPerRelay = 64 * 1024;

    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    worker.SetReceiveSetEnabledForTest(FakeReceiveSetEnabled);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);

    CHECK(worker.EventQueueFullErrorsForTest() == 0);
    CHECK(worker.QuicReceiveEnqueueFailuresForTest() == 0);
    CHECK(worker.QuicReceiveViewBackpressureQueuedForTest() == 0);

    CHECK(worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(worker.EnqueueForTest(TestMarkerEvent(2)));
    CHECK(worker.PendingEventsForTest() == 2);

    const char payload[] = "enqueue-fail";
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);

    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;

    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &receiveEvent) == QUIC_STATUS_PENDING);
    CHECK(worker.EventQueueFullErrorsForTest() == 1);
    CHECK(worker.QuicReceiveEnqueueFailuresForTest() == 0);
    CHECK(worker.QuicReceiveViewBackpressureQueuedForTest() == 1);
    // Queue-full holds PENDING views; does not enqueue QuicActiveShutdown (Task 4).
    CHECK(worker.QuicActiveShutdownEnqueuedForTest() == 0);
    CHECK(worker.QuicShutdownCompleteEnqueuedForTest() == 0);
    CHECK(g_receiveSetEnabledCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_lastReceiveSetEnabled.load(std::memory_order_acquire) == 0);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);

    worker.SetReceiveSetEnabledForTest(nullptr);
    worker.SetReceiveCompleteForTest(nullptr);
    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void ReceiveCallbackQueueFullHoldsBudgetUntilFlush() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeReceiveComplete();
    ResetFakeReceiveSetEnabled(QUIC_STATUS_SUCCESS);

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 2;
    config.MaxPendingQuicReceiveBytesPerRelay = 64 * 1024;

    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    worker.SetReceiveSetEnabledForTest(FakeReceiveSetEnabled);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);


    CHECK(worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(worker.EnqueueForTest(TestMarkerEvent(2)));
    CHECK(worker.PendingEventsForTest() == 2);

    const char payload[] = "budget-held";
    const uint64_t payloadBytes = sizeof(payload) - 1;
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(payloadBytes);

    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;

    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &receiveEvent) == QUIC_STATUS_PENDING);
    CHECK(worker.CallbackPendingReceiveBytesForTest(result.RelayId) == payloadBytes);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);

    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.FlushTcpWritableForTest(result.RelayId));
    CHECK(worker.CallbackPendingReceiveBytesForTest(result.RelayId) == 0);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(worker.Snapshot().DeferredReceiveDiscards >= 1);

    worker.SetReceiveSetEnabledForTest(nullptr);
    worker.SetReceiveCompleteForTest(nullptr);
    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void ReceiveCallbackQueueFullBackpressureRetriesAfterDrain() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeReceiveComplete();
    ResetFakeReceiveSetEnabled(QUIC_STATUS_SUCCESS);

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 2;
    config.MaxPendingQuicReceiveBytesPerRelay = 64 * 1024;

    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    worker.SetReceiveSetEnabledForTest(FakeReceiveSetEnabled);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);


    CHECK(worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(worker.EnqueueForTest(TestMarkerEvent(2)));
    CHECK(worker.PendingEventsForTest() == 2);

    const char payload[] = "backpressure-retry";
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);

    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;

    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &receiveEvent) == QUIC_STATUS_PENDING);
    CHECK(worker.QuicReceiveViewBackpressureQueuedForTest() == 1);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);

    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.PendingEventsForTest() == 1);

    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.PendingEventsForTest() == 0);
    CHECK(worker.FlushTcpWritableForTest(result.RelayId));
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(worker.Snapshot().DeferredReceiveDiscards >= 1);
    CHECK(worker.PendingQuicReceiveBytesForTest(result.RelayId) == 0);
    CHECK(worker.QuicReceiveViewBackpressureQueuedForTest() == 1);

    char output[sizeof(payload)]{};
    const ssize_t received = read(fds[1], output, sizeof(payload) - 1);
    CHECK(received == static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(std::memcmp(output, payload, sizeof(payload) - 1) == 0);
    CHECK(g_lastReceiveSetEnabled.load(std::memory_order_acquire) == 1);

    worker.SetReceiveSetEnabledForTest(nullptr);
    worker.SetReceiveCompleteForTest(nullptr);
    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void ReceiveCallbackBudgetRejectPausesAndCompletes() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeReceiveComplete();
    ResetFakeReceiveSetEnabled(QUIC_STATUS_SUCCESS);

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxPendingQuicReceiveBytesPerRelay = 4;

    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    worker.SetReceiveSetEnabledForTest(FakeReceiveSetEnabled);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);

    CHECK(worker.CallbackReceiveBudgetRejectsForTest() == 0);

    const char firstPayload[] = "abc";
    QUIC_BUFFER firstBuffer{};
    firstBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(firstPayload));
    firstBuffer.Length = static_cast<uint32_t>(sizeof(firstPayload) - 1);

    QUIC_STREAM_EVENT firstEvent{};
    firstEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    firstEvent.RECEIVE.BufferCount = 1;
    firstEvent.RECEIVE.Buffers = &firstBuffer;

    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &firstEvent) == QUIC_STATUS_PENDING);
    CHECK(worker.CallbackReceiveBudgetRejectsForTest() == 0);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);

    const char secondPayload[] = "de";
    QUIC_BUFFER secondBuffer{};
    secondBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(secondPayload));
    secondBuffer.Length = static_cast<uint32_t>(sizeof(secondPayload) - 1);

    QUIC_STREAM_EVENT secondEvent{};
    secondEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    secondEvent.RECEIVE.BufferCount = 1;
    secondEvent.RECEIVE.Buffers = &secondBuffer;

    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &secondEvent) == QUIC_STATUS_SUCCESS);
    CHECK(worker.CallbackReceiveBudgetRejectsForTest() == 1);
    // Budget reject is non-terminal backpressure (immediate complete), not QuicActiveShutdown.
    CHECK(worker.QuicActiveShutdownEnqueuedForTest() == 0);
    CHECK(worker.QuicShutdownCompleteEnqueuedForTest() == 0);
    CHECK(g_receiveSetEnabledCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_lastReceiveSetEnabled.load(std::memory_order_acquire) == 0);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == sizeof(secondPayload) - 1);

    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.FlushTcpWritableForTest(result.RelayId));
    CHECK(g_receiveSetEnabledCalls.load(std::memory_order_acquire) == 2);
    CHECK(g_lastReceiveSetEnabled.load(std::memory_order_acquire) == 1);

    worker.SetReceiveSetEnabledForTest(nullptr);
    worker.SetReceiveCompleteForTest(nullptr);
    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
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
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(worker.Snapshot().DeferredReceiveDiscards >= 1);
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
    // Legacy (no StreamOwner): Stop must discard PENDING views without calling
    // ReceiveComplete via bare relay->Stream. Lease-only complete is managed-only.
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
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(worker.Snapshot().DeferredReceiveDiscards >= 1);
    CHECK(worker.QuicActiveShutdownEnqueuedForTest() == 0);

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
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(worker.Snapshot().DeferredReceiveDiscards >= 1);

    worker.SetReceiveCompleteForTest(nullptr);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void QuicReceiveCallbackRejectsOversizedReceiveAfterPendingFinOnlyEvent() {
    ResetFakeReceiveComplete();

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 16;
    config.MaxPendingQuicReceiveBytesPerRelay = 8;

    ManagedRelayHarness harness(config);
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    std::memset(harness.StreamStorage, 0, sizeof(harness.StreamStorage));
    harness.Stream = reinterpret_cast<MsQuicStream*>(harness.StreamStorage);
    harness.Stream->CleanUpMode = CleanUpManual;
    CHECK(harness.Owner->InstallStreamForTest(harness.Stream));
    harness.Worker.SetReceiveCompleteForTest(FakeReceiveComplete);

    QUIC_STREAM_EVENT finOnlyEvent{};
    finOnlyEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    finOnlyEvent.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;
    CHECK(harness.DispatchViaRouter(finOnlyEvent) == QUIC_STATUS_PENDING);
    CHECK(harness.Worker.PendingQuicReceiveBytesForTest(harness.Result.RelayId) == 0);
    CHECK(harness.Worker.PendingReceiveCompletionStateForTest(harness.Result.RelayId) ==
          TqDarwinReceiveCompletionState::Pending);

    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == 0);

    // Hold budget with an under-limit pending view, then reject the oversize.
    const char filler[] = "1234567";
    QUIC_BUFFER fillerBuffer{};
    fillerBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(filler));
    fillerBuffer.Length = static_cast<uint32_t>(sizeof(filler) - 1);
    QUIC_STREAM_EVENT fillerEvent{};
    fillerEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    fillerEvent.RECEIVE.BufferCount = 1;
    fillerEvent.RECEIVE.Buffers = &fillerBuffer;
    CHECK(harness.DispatchViaRouter(fillerEvent) == QUIC_STATUS_PENDING);

    const char payload[] = "oversized-after-fin";
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);
    QUIC_STREAM_EVENT oversizedEvent{};
    oversizedEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    oversizedEvent.RECEIVE.BufferCount = 1;
    oversizedEvent.RECEIVE.Buffers = &quicBuffer;
    CHECK(harness.DispatchViaRouter(oversizedEvent) == QUIC_STATUS_SUCCESS);

    while (harness.Worker.DrainOneEventForTest()) {
    }

    harness.Worker.SetReceiveCompleteForTest(nullptr);
    harness.Owner->ReleaseStreamForTest();
    harness.StopAndClosePeer();
}

void QuicReceiveZeroBufferFinCompletesAfterDrain() {
    ResetFakeReceiveComplete();

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.EventQueueCapacity = 16;
    config.MaxPendingQuicReceiveBytesPerRelay = 8;

    ManagedRelayHarness harness(config);
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    std::memset(harness.StreamStorage, 0, sizeof(harness.StreamStorage));
    harness.Stream = reinterpret_cast<MsQuicStream*>(harness.StreamStorage);
    harness.Stream->CleanUpMode = CleanUpManual;
    CHECK(harness.Owner->InstallStreamForTest(harness.Stream));
    harness.Worker.SetReceiveCompleteForTest(FakeReceiveComplete);

    uint8_t byte = 0;
    QUIC_BUFFER buffer{0, &byte};
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_RECEIVE;
    event.RECEIVE.Buffers = &buffer;
    event.RECEIVE.BufferCount = 1;
    event.RECEIVE.TotalBufferLength = 0;
    event.RECEIVE.AbsoluteOffset = 42;
    event.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;
    CHECK(harness.DispatchViaRouter(event) == QUIC_STATUS_PENDING);
    CHECK(harness.Worker.PendingQuicReceiveBytesForTest(harness.Result.RelayId) == 0);
    CHECK(harness.Worker.PendingReceiveCompletionStateForTest(harness.Result.RelayId) ==
          TqDarwinReceiveCompletionState::Pending);

    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == 0);

    uint8_t one = 0;
    const ssize_t received = read(harness.Fds[0], &one, sizeof(one));
    CHECK(received == 0);

    harness.Worker.SetReceiveCompleteForTest(nullptr);
    harness.Owner->ReleaseStreamForTest();
    harness.StopAndClosePeer();
}

void QuicReceiveZeroFinBudgetRejectReturnsSuccessWithoutCompletion() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeReceiveComplete();
    ResetFakeReceiveSetEnabled(QUIC_STATUS_SUCCESS);

    TqDarwinRelayWorkerConfig config{};
    config.ReadChunkSize = 4096;
    config.ReadBatchBytes = 4096;
    config.MaxPendingQuicReceiveBytesPerRelay = 4;

    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    worker.SetReceiveSetEnabledForTest(FakeReceiveSetEnabled);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;
    registration.EnableQuicSends = false;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);

    CHECK(worker.CallbackReceiveBudgetRejectsForTest() == 0);

    const char firstPayload[] = "12345678";
    QUIC_BUFFER firstBuffer{};
    firstBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(firstPayload));
    firstBuffer.Length = static_cast<uint32_t>(sizeof(firstPayload) - 1);

    QUIC_STREAM_EVENT firstEvent{};
    firstEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    firstEvent.RECEIVE.BufferCount = 1;
    firstEvent.RECEIVE.Buffers = &firstBuffer;

    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &firstEvent) == QUIC_STATUS_PENDING);
    CHECK(worker.CallbackReceiveBudgetRejectsForTest() == 0);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);

    uint8_t byte = 0;
    QUIC_BUFFER buffer{0, &byte};
    QUIC_STREAM_EVENT finEvent{};
    finEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    finEvent.RECEIVE.Buffers = &buffer;
    finEvent.RECEIVE.BufferCount = 1;
    finEvent.RECEIVE.TotalBufferLength = 0;
    finEvent.RECEIVE.AbsoluteOffset = 42;
    finEvent.RECEIVE.Flags = QUIC_RECEIVE_FLAG_FIN;

    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &finEvent) == QUIC_STATUS_SUCCESS);
    CHECK(worker.CallbackReceiveBudgetRejectsForTest() == 1);
    CHECK(g_receiveSetEnabledCalls.load(std::memory_order_acquire) == 1);
    CHECK(g_lastReceiveSetEnabled.load(std::memory_order_acquire) == 0);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(g_receiveCompleteBytes.load(std::memory_order_acquire) == 0);

    worker.SetReceiveSetEnabledForTest(nullptr);
    worker.SetReceiveCompleteForTest(nullptr);
    worker.UnregisterRelay(result.RelayId);
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
    registration.EnableQuicSends = false;
    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(worker.Snapshot().DeferredReceiveDiscards >= 1);

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
    registration.EnableQuicSends = false;
    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(worker.Snapshot().DeferredReceiveDiscards >= 1);
    CHECK(handle.Control != nullptr);
    CHECK(handle.Control->Stop.load(std::memory_order_acquire));

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
    registration.EnableQuicSends = false;
    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(worker.Snapshot().DeferredReceiveDiscards >= 1);
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
    registration.EnableQuicSends = false;
    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(worker.Snapshot().DeferredReceiveDiscards >= 1);

    worker.SetSendMsgForTest(nullptr);
    worker.SetReceiveCompleteForTest(nullptr);
    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void MissingRelayQuicReceiveDiscardsWithoutStreamApi() {
    ResetFakeReceiveComplete();
    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    CHECK(worker.StartForTest());

    const char payload[] = "missing-relay";
    auto receive = std::make_shared<TqDarwinPendingQuicReceive>();
    receive->RelayId = 999999;
    receive->Slices.push_back(TqDarwinQuicReceiveSlice{
        reinterpret_cast<const uint8_t*>(payload),
        static_cast<uint32_t>(sizeof(payload) - 1)});
    receive->TotalLength = sizeof(payload) - 1;

    CHECK(worker.InvokeQuicReceiveViewForTest(receive));
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
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
    registration.EnableQuicSends = false;
    registration.CompressAlgo = TqCompressAlgo::Zstd;
    registration.Decompressor = &decompressor;
    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(worker.Snapshot().DeferredReceiveDiscards >= 1);
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
    registration.EnableQuicSends = false;
    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(worker.Snapshot().DeferredReceiveDiscards >= 1);
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
    registration.EnableQuicSends = false;
    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    registration.EnableQuicSends = false;
    registration.CompressAlgo = TqCompressAlgo::Zstd;
    FailingDecompressor decompressor;
    registration.Decompressor = &decompressor;
    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(worker.Snapshot().DeferredReceiveDiscards >= 1);
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
    CHECK(worker.Start());

    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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

    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &event) == QUIC_STATUS_PENDING);
    CHECK(worker.FindRelayLockedCountForTest() == before);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void CallbackShutdownDoesNotUseLockedRelayLookup() {
    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    CHECK(worker.Start());

    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);

    const uint64_t before = worker.FindRelayLockedCountForTest();
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.FindRelayLockedCountForTest() == before);

    TqDarwinRelayWorkerSnapshot snapshot{};
    for (int i = 0; i < 200; ++i) {
        snapshot = worker.Snapshot();
        if (snapshot.ActiveRelays == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(snapshot.ActiveRelays == 0);

    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void QuicShutdownCallbackClosesViaWorkerEventWithoutLockedLookup() {
    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    CHECK(worker.StartForTest());

    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);

    const uint64_t before = worker.FindRelayLockedCountForTest();
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &event) == QUIC_STATUS_SUCCESS);
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

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);

    CHECK(worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(worker.EnqueueForTest(TestMarkerEvent(2)));
    CHECK(worker.PendingEventsForTest() == 2);

    const uint64_t before = worker.FindRelayLockedCountForTest();
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &event) == QUIC_STATUS_SUCCESS);
    CHECK(worker.FindRelayLockedCountForTest() == before);
    CHECK(handle.Control->Stop.load(std::memory_order_acquire));
    CHECK(worker.Snapshot().ActiveRelays == 1);

    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.DrainOneEventForTest());
    worker.Stop();
    CHECK(worker.Snapshot().ActiveRelays == 0);
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void StopPurgesQueuedShutdownCloseWithoutLockedLookup() {
    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    CHECK(worker.StartForTest());

    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);

    const uint64_t before = worker.FindRelayLockedCountForTest();
    QUIC_STREAM_EVENT event{};
    event.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(TqDarwinRelayWorker::StreamCallback(stream, stream->Context, &event) == QUIC_STATUS_SUCCESS);
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

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);
    CHECK(stream->Context != nullptr);
    CHECK(stream->Callback == TqDarwinRelayWorker::StreamCallback);

    worker.UnregisterRelay(result.RelayId);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void SnapshotDuringConcurrentUnregisterRemainsBestEffort() {
    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    CHECK(worker.Start());

    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = stream;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
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

void PreparedReceiveBufferedBeforeCommit() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    g_PrecommitPayload.fill(0xAB);
    g_PrecommitOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    g_PrecommitReceiveStatus = QUIC_STATUS_INTERNAL_ERROR;
    g_PrecommitUncommitted = false;

    TqDarwinRelayWorkerConfig config{};
    config.MaxPendingQuicReceiveBytesPerRelay = g_PrecommitPayload.size();
    config.FailCommitForTest = true;
    config.AfterPublishHookForTest = AfterManagedPublishHook;

    TqDarwinRelayWorker worker(config);
    CHECK(worker.StartForTest());
    TqRelayHandle handle{};
    const int consumedFd = fds[0];
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = consumedFd;
    registration.Stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    registration.StreamOwner = g_PrecommitOwner;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(!result.Ok);
    CHECK(result.TcpFdConsumed);
    CHECK(g_PrecommitUncommitted);
    CHECK(g_PrecommitReceiveStatus == QUIC_STATUS_PENDING);
    CHECK(fcntl(consumedFd, F_GETFD) == -1);
    CHECK(errno == EBADF);

    g_PrecommitOwner.reset();
    worker.Stop();
    ::close(fds[1]);
}

void PreparedReceivePrecommitDiscardIncrementsDeferredComplete() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    g_PrecommitPayload = {1, 2, 3, 4, 5, 6, 7, 8};
    g_PrecommitOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    g_PrecommitReceiveStatus = QUIC_STATUS_INTERNAL_ERROR;

    TqDarwinRelayWorkerConfig config{};
    config.MaxPendingQuicReceiveBytesPerRelay = g_PrecommitPayload.size();
    config.FailCommitForTest = true;
    config.AfterPublishHookForTest = AfterManagedPublishHook;

    TqDarwinRelayWorker worker(config);
    CHECK(worker.StartForTest());
    TqRelayHandle handle{};
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    registration.StreamOwner = g_PrecommitOwner;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(!result.Ok);
    CHECK(g_PrecommitReceiveStatus == QUIC_STATUS_PENDING);
    CHECK(worker.Snapshot().DeferredReceiveCompletes == 1);

    g_PrecommitOwner.reset();
    worker.Stop();
    ::close(fds[1]);
}

void TerminalOwnerSkipsReceiveCompleteOnPrecommitDiscard() {
    ResetFakeReceiveComplete();
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    g_PrecommitPayload = {9, 8, 7, 6, 5, 4, 3, 2};
    g_PrecommitOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    g_PrecommitReceiveStatus = QUIC_STATUS_INTERNAL_ERROR;

    TqDarwinRelayWorkerConfig config{};
    config.MaxPendingQuicReceiveBytesPerRelay = g_PrecommitPayload.size();
    config.FailCommitForTest = true;
    config.AfterPublishHookForTest = [](TqDarwinRelayWorker* worker, uint64_t relayId) {
        AfterManagedPublishHook(worker, relayId);
        (void)g_PrecommitOwner->PublishTerminalAndTakeTarget();
    };

    TqDarwinRelayWorker worker(config);
    worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    CHECK(worker.StartForTest());
    TqRelayHandle handle{};
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    registration.StreamOwner = g_PrecommitOwner;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(!result.Ok);
    CHECK(g_PrecommitReceiveStatus == QUIC_STATUS_PENDING);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(g_PrecommitOwner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished);
    CHECK(worker.Snapshot().DeferredReceiveDiscards == 1);
    CHECK(worker.Snapshot().DeferredReceiveCompletes == 0);

    worker.SetReceiveCompleteForTest(nullptr);
    g_PrecommitOwner.reset();
    worker.Stop();
    ::close(fds[1]);
}

void PublishWindowReceiveSeesInitializedIdentity() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    g_PrecommitPayload.fill(0xCD);
    g_PrecommitOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    g_PrecommitReceiveStatus = QUIC_STATUS_INTERNAL_ERROR;
    const uint64_t expectedGenerationBeforePublish = g_PrecommitOwner->RouteGeneration();

    TqDarwinRelayWorkerConfig config{};
    config.MaxPendingQuicReceiveBytesPerRelay = g_PrecommitPayload.size();
    config.FailCommitForTest = true;
    config.AfterPublishHookForTest = AfterManagedPublishHook;

    TqDarwinRelayWorker worker(config);
    CHECK(worker.StartForTest());
    TqRelayHandle handle{};
    const int consumedFd = fds[0];
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = consumedFd;
    registration.Stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    registration.StreamOwner = g_PrecommitOwner;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(!result.Ok);
    CHECK(result.TcpFdConsumed);
    CHECK(g_PrecommitReceiveStatus == QUIC_STATUS_PENDING);

    const auto identity = worker.LastPublishIdentityForTest();
    CHECK(identity.RelayId != 0);
    // Binding stores the post-publish immutable route generation.
    CHECK(identity.RouteGeneration == expectedGenerationBeforePublish + 1);
    CHECK(identity.RouteGeneration == g_PrecommitOwner->RouteGeneration());
    CHECK(identity.ControlGeneration != 0);
    CHECK(identity.RelayLockable);
    CHECK(identity.StreamOwnerLockable);
    CHECK(identity.PrecommitDepth == 1);
    CHECK(TcpRelayFdClosedOnce(consumedFd));
    CHECK(worker.TcpFdCloseCountForTest() == 1);
    CHECK(handle.Backend == TqRelayBackendType::None);

    g_PrecommitOwner.reset();
    worker.Stop();
    ::close(fds[1]);
}

void PublishWindowTerminalRollsBackWithoutPublicHandle() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    g_PrecommitOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    g_PrecommitPublishTerminal = true;

    TqDarwinRelayWorkerConfig config{};
    config.AfterPublishHookForTest = AfterManagedPublishHook;

    TqDarwinRelayWorker worker(config);
    CHECK(worker.StartForTest());
    TqRelayHandle handle{};
    const int consumedFd = fds[0];
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = consumedFd;
    registration.Stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    registration.StreamOwner = g_PrecommitOwner;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    g_PrecommitPublishTerminal = false;
    CHECK(!result.Ok);
    CHECK(result.TcpFdConsumed);
    CHECK(result.RelayId == 0);
    CHECK(handle.Backend == TqRelayBackendType::None);
    CHECK(handle.DarwinWorker == nullptr);
    CHECK(handle.DarwinRelayId == 0);
    CHECK(TcpRelayFdClosedOnce(consumedFd));
    CHECK(worker.TcpFdCloseCountForTest() == 1);
    CHECK(worker.MapPublicationCountForTest() == 1);
    CHECK(worker.TcpFilterInstallCountForTest() <= 1);
    CHECK(worker.CommittedRelayCountForTest() == 0);

    g_PrecommitOwner.reset();
    worker.Stop();
    ::close(fds[1]);
}

void PublishWindowPeerAbortSeesLockableIdentity() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    g_PrecommitOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    static std::atomic<bool> sawLockableIdentity{false};
    sawLockableIdentity.store(false, std::memory_order_release);

    TqDarwinRelayWorkerConfig config{};
    // Peer abort during Prepared does not by itself fail commit (Task 2 scope).
    // This test only asserts publish-window identity is complete and lockable.
    config.AfterPublishHookForTest = [](TqDarwinRelayWorker* worker, uint64_t relayId) {
        CHECK(relayId != 0);
        const auto identity = worker->LastPublishIdentityForTest();
        CHECK(identity.RelayId == relayId);
        CHECK(identity.RelayLockable);
        CHECK(identity.StreamOwnerLockable);
        CHECK(identity.RouteGeneration != 0);
        sawLockableIdentity.store(true, std::memory_order_release);
        QUIC_STREAM_EVENT abortEvent{};
        abortEvent.Type = QUIC_STREAM_EVENT_PEER_SEND_ABORTED;
        abortEvent.PEER_SEND_ABORTED.ErrorCode = 0x41;
        g_PrecommitReceiveStatus = g_PrecommitOwner->DispatchForTest(&abortEvent);
        CHECK(g_PrecommitReceiveStatus == QUIC_STATUS_SUCCESS);
    };

    TqDarwinRelayWorker worker(config);
    CHECK(worker.StartForTest());
    TqRelayHandle handle{};
    const int consumedFd = fds[0];
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = consumedFd;
    registration.Stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    registration.StreamOwner = g_PrecommitOwner;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(sawLockableIdentity.load(std::memory_order_acquire));
    // Registration may still succeed; peer-abort→commit-failure is out of Task 2.
    if (result.Ok) {
        CHECK(result.RelayId != 0);
        CHECK(handle.Backend == TqRelayBackendType::DarwinWorker);
        TqRelayStop(&handle);
    } else {
        CHECK(result.TcpFdConsumed);
        CHECK(TcpRelayFdClosedOnce(consumedFd));
    }

    g_PrecommitOwner.reset();
    worker.Stop();
    ::close(fds[1]);
}

void CommitBarrierTerminalBeforeFinalCheckRollsBack() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    g_PrecommitPayload = {1, 2, 3, 4};
    g_PrecommitOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    g_PrecommitReceiveStatus = QUIC_STATUS_INTERNAL_ERROR;

    TqDarwinRelayWorkerConfig config{};
    config.MaxPendingQuicReceiveBytesPerRelay = g_PrecommitPayload.size();
    config.AfterPublishHookForTest = AfterManagedPublishHook;
    config.BeforeCommitFinalCheckHookForTest = [](TqDarwinRelayWorker*, uint64_t) {
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        (void)g_PrecommitOwner->DispatchForTest(&terminal);
    };

    TqDarwinRelayWorker worker(config);
    CHECK(worker.StartForTest());
    TqRelayHandle handle{};
    const int consumedFd = fds[0];
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = consumedFd;
    registration.Stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    registration.StreamOwner = g_PrecommitOwner;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(!result.Ok);
    CHECK(result.TcpFdConsumed);
    CHECK(handle.Backend == TqRelayBackendType::None);
    CHECK(TcpRelayFdClosedOnce(consumedFd));
    CHECK(worker.TcpFilterInstallCountForTest() == 1);
    CHECK(worker.TcpFilterDeleteCountForTest() == 1);
    CHECK(worker.TcpFdCloseCountForTest() == 1);
    CHECK(worker.MapPublicationCountForTest() == 1);
    CHECK(worker.CommittedRelayCountForTest() == 0);
    CHECK(worker.Snapshot().DeferredReceiveDiscards +
              worker.Snapshot().DeferredReceiveCompletes >=
          1);

    g_PrecommitOwner.reset();
    worker.Stop();
    ::close(fds[1]);
}

void CommitBarrierTerminalAfterActivationSucceedsThenStops() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    g_PrecommitPayload = {5, 6, 7, 8};
    g_PrecommitOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    g_PrecommitReceiveStatus = QUIC_STATUS_INTERNAL_ERROR;
    std::shared_ptr<TqRelayStopControl> observedControl;

    TqDarwinRelayWorkerConfig config{};
    config.MaxPendingQuicReceiveBytesPerRelay = g_PrecommitPayload.size();
    config.AfterPublishHookForTest = AfterManagedPublishHook;
    config.AfterCommitActivationHookForTest = [](TqDarwinRelayWorker*, uint64_t) {
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        (void)g_PrecommitOwner->DispatchForTest(&terminal);
    };

    TqDarwinRelayWorker worker(config);
    CHECK(worker.StartForTest());
    TqRelayHandle handle{};
    handle.Control = std::make_shared<TqRelayStopControl>();
    handle.ControlGeneration = handle.Control->Generation;
    observedControl = handle.Control;
    const int consumedFd = fds[0];
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = consumedFd;
    registration.Stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    registration.StreamOwner = g_PrecommitOwner;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);
    CHECK(result.TcpFdConsumed);
    CHECK(result.RelayId != 0);
    CHECK(handle.Backend == TqRelayBackendType::DarwinWorker);
    CHECK(observedControl->Stop.load(std::memory_order_acquire));
    CHECK(worker.TcpFilterInstallCountForTest() == 1);
    CHECK(worker.MapPublicationCountForTest() == 1);

    // Drain terminal cleanup and unregister so filter/FD settle exactly once.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (worker.PendingEventsForTest() != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        (void)worker.DrainOneEventForTest();
    }
    TqRelayStop(&handle);
    CHECK(worker.TcpFilterDeleteCountForTest() == 1);
    CHECK(worker.TcpFdCloseCountForTest() == 1);
    CHECK(TcpRelayFdClosedOnce(consumedFd));
    // Terminal raced after Prepared->Active: precommit must be discarded once
    // (not left PENDING until binding dtor). Completes stay 0 for terminal owner.
    CHECK(worker.Snapshot().DeferredReceiveDiscards >= 1);
    CHECK(worker.Snapshot().DeferredReceiveCompletes == 0);
    CHECK(
        worker.Snapshot().DeferredReceiveDiscards +
            worker.Snapshot().DeferredReceiveCompletes >=
        1);

    g_PrecommitOwner.reset();
    worker.Stop();
    ::close(fds[1]);
}

void ManagedBindingWeakPointersExpireWithoutOwnershipCycle() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    const uint64_t relayId = harness.Result.RelayId;
    std::weak_ptr<TqStreamLifetime> weakOwner = harness.Owner;
    std::weak_ptr<void> weakRelay = harness.Worker.ActiveRelayOwnerForTest(relayId);
    CHECK(!weakOwner.expired());
    CHECK(!weakRelay.expired());

    const uint64_t bindingDtorsBefore = harness.Worker.StreamBindingDestructorCountForTest();
    const uint64_t relayDtorsBefore = harness.Worker.RelayStateDestructorCountForTest();

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (harness.Worker.PendingEventsForTest() != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        (void)harness.Worker.DrainOneEventForTest();
    }
    TqRelayStop(&harness.Handle);
    harness.Owner.reset();
    harness.StopAndClosePeer();

    CHECK(weakOwner.expired());
    CHECK(weakRelay.expired());
    CHECK(harness.Worker.StreamBindingDestructorCountForTest() > bindingDtorsBefore);
    CHECK(harness.Worker.RelayStateDestructorCountForTest() > relayDtorsBefore);
}

void ManagedStaleSendCompletionEnvelopeDoesNotClaimTwice() {
    auto target = std::make_shared<CountingTarget>();
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started, target);
    unsigned cleanups = 0;
    void* firstKey = owner->RegisterSendCompletion(nullptr, [&] { ++cleanups; });
    void* secondKey = owner->RegisterSendCompletion(nullptr, [&] { ++cleanups; });
    CHECK(firstKey != nullptr);
    CHECK(secondKey != nullptr);
    CHECK(firstKey != secondKey);

    QUIC_STREAM_EVENT firstComplete{};
    firstComplete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    firstComplete.SEND_COMPLETE.ClientContext = firstKey;
    CHECK(owner->DispatchForTest(&firstComplete) == QUIC_STATUS_SUCCESS);
    CHECK(cleanups == 1);

    QUIC_STREAM_EVENT duplicate{};
    duplicate.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    duplicate.SEND_COMPLETE.ClientContext = firstKey;
    CHECK(owner->DispatchForTest(&duplicate) == QUIC_STATUS_SUCCESS);
    CHECK(cleanups == 1);

    QUIC_STREAM_EVENT secondComplete{};
    secondComplete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    secondComplete.SEND_COMPLETE.ClientContext = secondKey;
    CHECK(owner->DispatchForTest(&secondComplete) == QUIC_STATUS_SUCCESS);
    CHECK(cleanups == 2);
}

void LateReceiveAfterInactiveBindingUsesFailSafeSink() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    g_PrecommitPayload = {1, 2, 3, 4};
    g_PrecommitOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);

    TqDarwinRelayWorkerConfig config{};
    config.FailCommitForTest = true;
    config.AfterPublishHookForTest = AfterManagedPublishHook;

    TqDarwinRelayWorker worker(config);
    CHECK(worker.StartForTest());
    TqRelayHandle handle{};
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[0];
    registration.Stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    registration.StreamOwner = g_PrecommitOwner;

    TqDarwinRelayRegistrationResult result = RegisterAndPublish(worker, registration, handle);
    CHECK(!result.Ok);

    QUIC_BUFFER buffer{};
    buffer.Buffer = g_PrecommitPayload.data();
    buffer.Length = static_cast<uint32_t>(g_PrecommitPayload.size());
    QUIC_STREAM_EVENT receive{};
    receive.Type = QUIC_STREAM_EVENT_RECEIVE;
    receive.RECEIVE.BufferCount = 1;
    receive.RECEIVE.Buffers = &buffer;
    receive.RECEIVE.TotalBufferLength = buffer.Length;
    CHECK(g_PrecommitOwner->DispatchForTest(&receive) == QUIC_STATUS_SUCCESS);
    CHECK(receive.RECEIVE.TotalBufferLength == 0);
    CHECK(worker.Snapshot().ReceiveFailSafeCount == 1);
    CHECK(worker.Snapshot().QuicReceiveViewCount == 0);

    g_PrecommitOwner.reset();
    worker.Stop();
    ::close(fds[1]);
}

void LeaseHeldDeferredReceiveDoesNotDiscard() {
    ResetFakeReceiveComplete();

    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    std::memset(harness.StreamStorage, 0, sizeof(harness.StreamStorage));
    harness.Stream = reinterpret_cast<MsQuicStream*>(harness.StreamStorage);
    CHECK(harness.Owner->InstallStreamForTest(harness.Stream));
    harness.Worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    harness.Worker.SetSendMsgForTest(PartialSendMsg);

    const char payload[] = "lease-hold";
    QUIC_BUFFER buffer{};
    buffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    buffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);
    QUIC_STREAM_EVENT receive{};
    receive.Type = QUIC_STREAM_EVENT_RECEIVE;
    receive.RECEIVE.BufferCount = 1;
    receive.RECEIVE.Buffers = &buffer;
    SetSendMsgMode(1);
    CHECK(harness.DispatchViaRouter(receive) == QUIC_STATUS_PENDING);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(harness.Worker.Snapshot().DeferredReceiveDiscards == 0);

    auto lease = harness.Owner->TryAcquireApi();
    CHECK(static_cast<bool>(lease));
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);

    lease = {};
    SetSendMsgMode(0);
    CHECK(harness.Worker.FlushTcpWritableForTest(harness.Result.RelayId));
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 1);

    harness.Worker.SetSendMsgForTest(nullptr);
    harness.Worker.SetReceiveCompleteForTest(nullptr);
    harness.Owner->ReleaseStreamForTest();
    harness.StopAndClosePeer();
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"

void ManagedRouterDispatchesShutdownComplete() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished);
    CHECK(!harness.Worker.BindingActiveForTest(harness.Result.RelayId));
    CHECK(!OwnerRouteTargetPresent(harness.Owner));
    CHECK(!harness.Owner->TryAcquireApi());
    CHECK(harness.Stream->Callback == TqStreamLifetime::Callback);
    CHECK(harness.Stream->Context == harness.Owner.get());
    CHECK(harness.Worker.Snapshot().ActiveRelays == 1);
    harness.StopAndClosePeer();
}

void ManagedTerminalCallbackReturnsBeforeEventDrained() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished);
    CHECK(!harness.Worker.BindingActiveForTest(harness.Result.RelayId));
    CHECK(!OwnerRouteTargetPresent(harness.Owner));
    CHECK(harness.Stream->Callback == TqStreamLifetime::Callback);
    CHECK(harness.Worker.Snapshot().ActiveRelays == 1);
    CHECK(harness.Worker.PendingEventsForTest() >= 1);
    harness.StopAndClosePeer(true);
}

void ManagedTunnelOwnerReleaseRetainsBinding() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    std::weak_ptr<TqStreamLifetime> weakOwner = harness.Owner;
    std::shared_ptr<void> binding = harness.Worker.StreamCallbackContextOwnerForTest(harness.Result.RelayId);
    CHECK(binding != nullptr);
    harness.Owner.reset();
    CHECK(!weakOwner.expired());
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    auto retained = weakOwner.lock();
    CHECK(retained != nullptr);
    CHECK(retained->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    CHECK(!weakOwner.expired());
    retained.reset();
    CHECK(harness.Worker.DrainOneEventForTest());
    harness.StopAndClosePeer();
    CHECK(weakOwner.expired());
}

void ManagedCloseRetirePurgePreservesStreamCallback() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    void* contextBefore = harness.Stream->Context;
    auto callbackBefore = harness.Stream->Callback;
    const uint64_t retiredBefore = harness.Worker.RetiredStreamBindingCountForTest();
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.BindingTerminalForTest(harness.Result.RelayId));
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(harness.Worker.Snapshot().ActiveRelays == 0);
    CHECK(harness.Worker.RelayStreamForTest(harness.Result.RelayId) == nullptr);
    CHECK(harness.Worker.RetiredStreamBindingCountForTest() >= retiredBefore);
    CHECK(harness.Stream->Callback == callbackBefore);
    CHECK(harness.Stream->Context == contextBefore);
    harness.StopAndClosePeer();
}

void ManagedPeerAbortUsesActiveShutdownWithoutTerminal() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    QUIC_STREAM_EVENT abort{};
    abort.Type = QUIC_STREAM_EVENT_PEER_SEND_ABORTED;
    abort.PEER_SEND_ABORTED.ErrorCode = 1;
    CHECK(harness.DispatchViaRouter(abort) == QUIC_STATUS_SUCCESS);
    CHECK(!harness.Worker.BindingTerminalForTest(harness.Result.RelayId));
    CHECK(harness.Worker.StreamOwnerForTest(harness.Result.RelayId) != nullptr);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(!harness.Worker.BindingTerminalForTest(harness.Result.RelayId));
    CHECK(harness.Worker.StreamOwnerForTest(harness.Result.RelayId) != nullptr);
    harness.StopAndClosePeer();
}

void ManagedPeerAbortBeforeTerminalOnlyTerminalDetaches() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    std::weak_ptr<TqStreamLifetime> weakOwner = harness.Owner;

    QUIC_STREAM_EVENT abort{};
    abort.Type = QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED;
    abort.PEER_RECEIVE_ABORTED.ErrorCode = 1;
    CHECK(harness.DispatchViaRouter(abort) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(!harness.Worker.BindingTerminalForTest(harness.Result.RelayId));
    CHECK(harness.Worker.StreamOwnerForTest(harness.Result.RelayId) != nullptr);
    CHECK(!weakOwner.expired());

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished);
    CHECK(harness.Worker.DrainOneEventForTest());
    harness.StopAndClosePeer();
    CHECK(weakOwner.expired());
}

void ManagedTerminalBeforePeerAbortIgnoresActiveShutdown() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(harness.Owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished);
    const uint64_t retiredAfterTerminal = harness.Worker.RetiredRelayCountForTest();

    QUIC_STREAM_EVENT abort{};
    abort.Type = QUIC_STREAM_EVENT_PEER_SEND_ABORTED;
    abort.PEER_SEND_ABORTED.ErrorCode = 1;
    CHECK(harness.DispatchViaRouter(abort) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.PendingEventsForTest() == 0);
    CHECK(harness.Worker.RetiredRelayCountForTest() == retiredAfterTerminal);
    harness.StopAndClosePeer();
}

void ManagedDuplicateTerminalEventIsIdempotent() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    const uint64_t retiredBindingsBefore = harness.Worker.RetiredStreamBindingCountForTest();

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    auto relayOwner = harness.Worker.ActiveRelayOwnerForTest(harness.Result.RelayId);
    CHECK(relayOwner != nullptr);
    CHECK(harness.Worker.EnqueueRelayCloseEventForTest(
        relayOwner,
        TqDarwinRelayEventType::QuicShutdownComplete,
        harness.Result.RelayId));
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(harness.Owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished);
    CHECK(harness.Worker.RetiredStreamBindingCountForTest() == retiredBindingsBefore);
    harness.StopAndClosePeer();
}

void ManagedQueuedPeerAbortBeforeTerminalPreservesOwnerUntilTerminal() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    std::weak_ptr<TqStreamLifetime> weakOwner = harness.Owner;

    QUIC_STREAM_EVENT abort{};
    abort.Type = QUIC_STREAM_EVENT_PEER_SEND_ABORTED;
    abort.PEER_SEND_ABORTED.ErrorCode = 1;
    CHECK(harness.DispatchViaRouter(abort) == QUIC_STATUS_SUCCESS);

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished);

    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(!weakOwner.expired());
    CHECK(harness.Worker.StreamOwnerForTest(harness.Result.RelayId) != nullptr);

    CHECK(harness.Worker.DrainOneEventForTest());
    harness.StopAndClosePeer();
    CHECK(weakOwner.expired());
}

void ManagedNonTerminalHalfCloseEventsDoNotPublishTerminal() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());

    QUIC_STREAM_EVENT sendShutdown{};
    sendShutdown.Type = QUIC_STREAM_EVENT_SEND_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(sendShutdown) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(!harness.Worker.BindingTerminalForTest(harness.Result.RelayId));
    CHECK(harness.Owner->GetPhase() == TqStreamLifetime::Phase::Started);

    QUIC_STREAM_EVENT peerShutdown{};
    peerShutdown.Type = QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN;
    CHECK(harness.DispatchViaRouter(peerShutdown) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(!harness.Worker.BindingTerminalForTest(harness.Result.RelayId));

    QUIC_STREAM_EVENT cancelOnLoss{};
    cancelOnLoss.Type = QUIC_STREAM_EVENT_CANCEL_ON_LOSS;
    cancelOnLoss.CANCEL_ON_LOSS.ErrorCode = 0;
    CHECK(harness.DispatchViaRouter(cancelOnLoss) == QUIC_STATUS_SUCCESS);
    CHECK(cancelOnLoss.CANCEL_ON_LOSS.ErrorCode == TqRelayStreamErrorCancelOnLoss);
    CHECK(harness.Worker.Snapshot().CancelOnLossCount == 1);
    CHECK(!harness.Worker.BindingTerminalForTest(harness.Result.RelayId));
    CHECK(harness.Owner->GetPhase() == TqStreamLifetime::Phase::Started);

    QUIC_STREAM_EVENT canceledSend{};
    canceledSend.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    canceledSend.SEND_COMPLETE.Canceled = true;
    canceledSend.SEND_COMPLETE.ClientContext = nullptr;
    CHECK(harness.DispatchViaRouter(canceledSend) == QUIC_STATUS_SUCCESS);
    CHECK(!harness.Worker.BindingTerminalForTest(harness.Result.RelayId));
    harness.StopAndClosePeer();
}

void ManagedStopRetentionMetricsSurfaceTerminalRetainedOwners() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    const auto activeSnapshot = harness.Worker.Snapshot();
    CHECK(activeSnapshot.TerminalRetainedOwnerCount >= 1);
    CHECK(activeSnapshot.StopRemaining >= 1);

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    const auto pendingSnapshot = harness.Worker.Snapshot();
    CHECK(pendingSnapshot.StopRemaining >= 1);
    CHECK(pendingSnapshot.TerminalRetainedOwnerCount <= activeSnapshot.TerminalRetainedOwnerCount);

    CHECK(harness.Worker.DrainOneEventForTest());
    harness.StopAndClosePeer();
}

void ManagedStopPurgeProcessesPeerAbortBeforeTerminal() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());

    QUIC_STREAM_EVENT abort{};
    abort.Type = QUIC_STREAM_EVENT_PEER_SEND_ABORTED;
    abort.PEER_SEND_ABORTED.ErrorCode = 1;
    CHECK(harness.DispatchViaRouter(abort) == QUIC_STATUS_SUCCESS);

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);

    harness.Worker.Stop();
    CHECK(harness.Owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished);
    harness.Owner.reset();
    ::close(harness.Fds[0]);
    harness.Fds[0] = TqInvalidSocket;
}

void ManagedPeerSendAbortedRequestsAbortReceiveFirst() {
    AdoptedManagedHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    g_fakeShutdownCalls.clear();
    g_fakeShutdownCount = 0;

    QUIC_STREAM_EVENT abort{};
    abort.Type = QUIC_STREAM_EVENT_PEER_SEND_ABORTED;
    abort.PEER_SEND_ABORTED.ErrorCode = 0x41;
    CHECK(harness.DispatchViaRouter(abort) == QUIC_STATUS_SUCCESS);
    CHECK(!g_fakeShutdownCalls.empty());
    CHECK((g_fakeShutdownCalls.front().Flags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE) != 0);
    CHECK((g_fakeShutdownCalls.front().Flags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND) == 0);
    CHECK(g_fakeShutdownCalls.front().ErrorCode == 0x41);

    CHECK(harness.Worker.DrainOneEventForTest());
    // CloseRelay upgrades AbortBoth; ledger only submits the other direction.
    CHECK(g_fakeShutdownCalls.size() >= 2);
    const auto& upgrade = g_fakeShutdownCalls[1];
    CHECK((upgrade.Flags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND) != 0);
    CHECK((upgrade.Flags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE) == 0);
    CHECK(harness.Owner->GetPhase() == TqStreamLifetime::Phase::Started);

    // AdoptAccepted retains until terminal; release before FakeApi tears down.
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    harness.StopAndClosePeer();
}

void ManagedPeerReceiveAbortedRequestsAbortSendFirst() {
    AdoptedManagedHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    g_fakeShutdownCalls.clear();
    g_fakeShutdownCount = 0;

    QUIC_STREAM_EVENT abort{};
    abort.Type = QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED;
    abort.PEER_RECEIVE_ABORTED.ErrorCode = 0x42;
    CHECK(harness.DispatchViaRouter(abort) == QUIC_STATUS_SUCCESS);
    CHECK(!g_fakeShutdownCalls.empty());
    CHECK((g_fakeShutdownCalls.front().Flags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND) != 0);
    CHECK((g_fakeShutdownCalls.front().Flags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE) == 0);
    CHECK(g_fakeShutdownCalls.front().ErrorCode == 0x42);

    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(g_fakeShutdownCalls.size() >= 2);
    const auto& upgrade = g_fakeShutdownCalls[1];
    CHECK((upgrade.Flags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE) != 0);
    CHECK((upgrade.Flags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND) == 0);
    CHECK(harness.Owner->GetPhase() == TqStreamLifetime::Phase::Started);

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    harness.StopAndClosePeer();
}

struct RealWorkerExitPurgeHookState {
    std::atomic<uint64_t> RelayId{0};
    std::shared_ptr<TqStreamLifetime> Owner;
    std::shared_ptr<void> RelayOwner;
    std::shared_ptr<void> BindingOwner;
    std::atomic<bool> Fired{false};
};
RealWorkerExitPurgeHookState* g_RealWorkerExitPurgeHook = nullptr;

void RealWorkerExitPurgeHook(TqDarwinRelayWorker* worker) {
    auto* state = g_RealWorkerExitPurgeHook;
    CHECK(state != nullptr);
    state->Fired.store(true, std::memory_order_release);
    const uint64_t id = state->RelayId.load(std::memory_order_acquire);
    auto relayOwner = state->RelayOwner;
    if (relayOwner == nullptr) {
        relayOwner = worker->ActiveRelayOwnerForTest(id);
    }
    CHECK(relayOwner != nullptr);

    CHECK(worker->EnqueueRelayCloseEventForTest(
        relayOwner, TqDarwinRelayEventType::QuicPeerSendShutdown, id));
    CHECK(worker->EnqueueRelayCloseEventForTest(
        relayOwner, TqDarwinRelayEventType::QuicSendShutdownComplete, id));
    CHECK(worker->EnqueueRelayCloseEventForTest(
        relayOwner, TqDarwinRelayEventType::QuicPeerSendAborted, id));
    CHECK(worker->EnqueueRelayCloseEventForTest(
        relayOwner, TqDarwinRelayEventType::QuicShutdownComplete, id));

    auto receive = std::make_shared<TqDarwinPendingQuicReceive>();
    receive->RelayId = id;
    receive->StreamOwner = state->Owner;
    receive->BindingOwner = state->BindingOwner;
    TqDarwinRelayEvent receiveEvent{};
    receiveEvent.Type = TqDarwinRelayEventType::QuicReceiveView;
    receiveEvent.RelayId = id;
    receiveEvent.ReceiveView = receive;
    CHECK(worker->EnqueueForTest(std::move(receiveEvent)));

    auto* operation = new TqDarwinRelaySendOperation();
    operation->RelayId = id;
    operation->CompletionRelayId = id;
    operation->CompletionBindingOwner = state->BindingOwner;
    TqDarwinRelayEvent sendComplete{};
    sendComplete.Type = TqDarwinRelayEventType::QuicSendComplete;
    sendComplete.RelayId = id;
    sendComplete.Value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(operation));
    CHECK(worker->EnqueueForTest(std::move(sendComplete)));
}

void RealWorkerExitPurgeSettlesMixedEventsWithoutAssert() {
    RealWorkerExitPurgeHookState state;
    g_RealWorkerExitPurgeHook = &state;

    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    TqDarwinRelayWorkerConfig config{};
    config.EventQueueCapacity = 64;
    config.BeforeWorkerExitHookForTest = RealWorkerExitPurgeHook;
    TqDarwinRelayWorker worker(config);

    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    auto* stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    stream->Callback = TqStreamLifetime::Callback;
    stream->Context = nullptr;
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    stream->Context = owner.get();
    state.Owner = owner;

    TqRelayHandle handle{};
    CHECK(worker.Start());

    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = stream;
    registration.StreamOwner = owner;
    registration.Control = handle.Control;
    registration.ControlGeneration = handle.Control->Generation;
    registration.EnableQuicSends = true;
    const auto result = worker.RegisterRelayWithId(registration);
    CHECK(result.Ok);
    handle.Stop.store(false, std::memory_order_release);
    handle.Control = registration.Control;
    handle.ControlGeneration = registration.Control->Generation;
    handle.Backend = TqRelayBackendType::DarwinWorker;
    handle.DarwinWorker = &worker;
    handle.DarwinRelayId = result.RelayId;

    state.RelayId.store(result.RelayId, std::memory_order_release);
    state.RelayOwner = worker.ActiveRelayOwnerForTest(result.RelayId);
    state.BindingOwner = worker.StreamCallbackContextOwnerForTest(result.RelayId);
    CHECK(state.RelayOwner != nullptr);
    CHECK(state.BindingOwner != nullptr);

    const uint64_t eventsBefore = worker.Snapshot().EventsProcessed;
    worker.Stop();
    CHECK(state.Fired.load(std::memory_order_acquire));
    CHECK(worker.PendingEventsForTest() == 0);
    CHECK(worker.Snapshot().EventsProcessed >= eventsBefore + 6);
    CHECK(worker.Snapshot().Errors == 0);

    stream->Callback = MsQuicStream::NoOpCallback;
    stream->Context = nullptr;
    owner.reset();
    state.Owner.reset();
    state.RelayOwner.reset();
    state.BindingOwner.reset();
    g_RealWorkerExitPurgeHook = nullptr;
    ::close(fds[0]);
}

void ManagedTerminalWithTcpErrorClosesOnce() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    CHECK(harness.Worker.InvokeTcpEventForTest(
        harness.Result.RelayId, EVFILT_READ, EV_EOF, 0));
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(harness.Worker.Snapshot().ActiveRelays == 0);
    CHECK(TcpRelayFdClosedOnce(harness.Fds[1]));
    harness.StopAndClosePeer();
}

void ManagedTerminalBeforeTcpErrorClosesOnce() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(TcpRelayFdClosedOnce(harness.Fds[1]));
    CHECK(harness.Worker.InvokeTcpEventForTest(
        harness.Result.RelayId, EVFILT_READ, EV_ERROR, ECONNRESET));
    CHECK(harness.Worker.Snapshot().ActiveRelays == 0);
    CHECK(TcpRelayFdClosedOnce(harness.Fds[1]));
    harness.StopAndClosePeer();
}

void ManagedTcpErrorBeforeTerminalClosesOnce() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    CHECK(harness.Worker.InvokeTcpEventForTest(
        harness.Result.RelayId, EVFILT_READ, EV_ERROR, ECONNRESET));
    CHECK(TcpRelayFdClosedOnce(harness.Fds[1]));
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    while (harness.Worker.PendingEventsForTest() > 0) {
        CHECK(harness.Worker.DrainOneEventForTest());
    }
    CHECK(harness.Worker.Snapshot().ActiveRelays == 0);
    CHECK(TcpRelayFdClosedOnce(harness.Fds[1]));
    harness.StopAndClosePeer();
}

void ManagedTerminalBeforeTcpWriteFailureClosesOnce() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(TcpRelayFdClosedOnce(harness.Fds[1]));

    CHECK(harness.Worker.InvokeTcpEventForTest(
        harness.Result.RelayId, EVFILT_WRITE, EV_ERROR, EPIPE));
    CHECK(harness.Worker.Snapshot().ActiveRelays == 0);
    CHECK(TcpRelayFdClosedOnce(harness.Fds[1]));
    harness.StopAndClosePeer();
}

void ManagedTcpWriteFailureBeforeTerminalClosesOnce() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    CHECK(harness.Worker.InvokeTcpEventForTest(
        harness.Result.RelayId, EVFILT_WRITE, EV_ERROR, EPIPE));
    CHECK(TcpRelayFdClosedOnce(harness.Fds[1]));

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    while (harness.Worker.PendingEventsForTest() > 0) {
        CHECK(harness.Worker.DrainOneEventForTest());
    }
    CHECK(harness.Worker.Snapshot().ActiveRelays == 0);
    CHECK(TcpRelayFdClosedOnce(harness.Fds[1]));
    harness.StopAndClosePeer();
}

void ManagedInFlightSendCompletesAfterRetireWithoutRelayStream() {
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorker worker(TqDarwinRelayWorkerConfig{});
    worker.SetStreamSendForTest(FakeStreamSend);
    TqRelayHandle handle{};
    CHECK(worker.StartForTest());
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(1));
    registration.EnableQuicSends = true;
    const auto result = RegisterAndPublish(worker, registration, handle);
    CHECK(result.Ok);
    std::shared_ptr<void> binding = worker.StreamCallbackContextOwnerForTest(result.RelayId);
    CHECK(binding != nullptr);
    const char payload[] = "retired-send";
    CHECK(write(fds[0], payload, sizeof(payload) - 1) ==
        static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(worker.InvokeTcpEventForTest(result.RelayId, EVFILT_READ, 0, 0));
    CHECK(worker.InFlightQuicSendCountForTest(result.RelayId) == 1);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, binding.get(), &terminal) == QUIC_STATUS_SUCCESS);
    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.RelayStreamForTest(result.RelayId) == nullptr);
    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(sendContext != nullptr);
    QUIC_STREAM_EVENT complete{};
    complete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    complete.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(TqDarwinRelayWorker::StreamCallback(nullptr, binding.get(), &complete) == QUIC_STATUS_SUCCESS);
    if (worker.KnownSendOperationCountForTest() != 0 &&
        worker.PendingEventsForTest() > 0) {
        CHECK(worker.DrainOneEventForTest());
    }
    CHECK(worker.KnownSendOperationCountForTest() == 0);
    worker.SetStreamSendForTest(nullptr);
    worker.Stop();
    CloseSocketPairAfterRelayOwned(registration.TcpFd, fds);
}

void ManagedTerminalRejectsRoutePublishAndApiLease() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished);
    CHECK(!OwnerRouteTargetPresent(harness.Owner));
    CHECK(!harness.Owner->TryAcquireApi());
    auto probe = std::make_shared<CountingTarget>();
    CHECK(!harness.Owner->PublishTarget(harness.Owner->RouteGeneration(), probe));
    harness.StopAndClosePeer();
}

void ManagedDirectRouterCallbackManualCleanupOnce() {
    auto target = std::make_shared<CountingTarget>();
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started, target);
    std::weak_ptr<TqStreamLifetime> weak = owner;
    void* callbackContext = owner.get();
    owner.reset();
    CHECK(!weak.expired());
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(TqStreamLifetime::Callback(nullptr, callbackContext, &terminal) == QUIC_STATUS_SUCCESS);
    CHECK(target->Calls == 1);
    CHECK(weak.expired());
}

void ManagedHandoffConcurrentTerminalHandledOnce() {
    auto first = std::make_shared<CountingTarget>();
    auto second = std::make_shared<CountingTarget>();
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started, first);
    std::atomic<int> gate{0};
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> terminalDone{false};
    std::thread publishThread([&] {
        SynchronizeConcurrentStart(gate, mutex, cv);
        uint64_t published = 0;
        (void)owner->PublishTarget(owner->RouteGeneration(), second, &published);
    });
    std::thread terminalThread([&] {
        SynchronizeConcurrentStart(gate, mutex, cv);
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        terminalDone.store(
            QUIC_SUCCEEDED(owner->DispatchForTest(&terminal)),
            std::memory_order_release);
    });
    publishThread.join();
    terminalThread.join();
    CHECK(terminalDone.load(std::memory_order_acquire));
    CHECK(owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished);
    CHECK(first->Calls.load(std::memory_order_acquire) +
              second->Calls.load(std::memory_order_acquire) ==
          1);
    CHECK((first->Calls.load(std::memory_order_acquire) == 1) ^
          (second->Calls.load(std::memory_order_acquire) == 1));
}

void ManagedStartCompleteSuccessBeforeStartReturns() {
    auto target = std::make_shared<CountingTarget>();
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::CreatedNotStarted, target);
    CHECK(owner->BeginStart());
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    harness.Owner = owner;
    harness.Stream = reinterpret_cast<MsQuicStream*>(harness.StreamStorage);
    harness.Stream->Callback = TqStreamLifetime::Callback;
    harness.Stream->Context = owner.get();
    CHECK(harness.Worker.StartForTest());
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = harness.Fds[1];
    registration.Stream = harness.Stream;
    registration.StreamOwner = owner;
    harness.Result = RegisterAndPublish(harness.Worker, registration, harness.Handle);
    CHECK(harness.Result.Ok);
    QUIC_STREAM_EVENT start{};
    start.Type = QUIC_STREAM_EVENT_START_COMPLETE;
    start.START_COMPLETE.Status = QUIC_STATUS_SUCCESS;
    CHECK(owner->DispatchForTest(&start) == QUIC_STATUS_SUCCESS);
    CHECK(owner->GetPhase() == TqStreamLifetime::Phase::Started);
    CHECK(target->Calls == 0);
    harness.StopAndClosePeer();
}

void ManagedStartFailureRejectsPublishAndLease() {
    auto target = std::make_shared<CountingTarget>();
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Starting, target);
    QUIC_STREAM_EVENT failed{};
    failed.Type = QUIC_STREAM_EVENT_START_COMPLETE;
    failed.START_COMPLETE.Status = QUIC_STATUS_INTERNAL_ERROR;
    CHECK(owner->DispatchForTest(&failed) == QUIC_STATUS_SUCCESS);
    CHECK(owner->GetPhase() == TqStreamLifetime::Phase::StartFailed);
    CHECK(target->Calls == 1);
    CHECK(!owner->PublishTarget(owner->RouteGeneration(), target));
    CHECK(!owner->TryAcquireApi());
}

void ManagedSendStartFailureRetainedUntilSendCompleteCanceled() {
    auto target = std::make_shared<CountingTarget>();
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Starting, target);
    unsigned cleanups = 0;
    void* completionKey = owner->RegisterSendCompletion(nullptr, [&] { ++cleanups; });
    CHECK(completionKey != nullptr);
    QUIC_STREAM_EVENT failed{};
    failed.Type = QUIC_STREAM_EVENT_START_COMPLETE;
    failed.START_COMPLETE.Status = QUIC_STATUS_INTERNAL_ERROR;
    CHECK(owner->DispatchForTest(&failed) == QUIC_STATUS_SUCCESS);
    std::weak_ptr<TqStreamLifetime> weak = owner;
    owner.reset();
    CHECK(!weak.expired());
    QUIC_STREAM_EVENT canceled{};
    canceled.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    canceled.SEND_COMPLETE.Canceled = true;
    canceled.SEND_COMPLETE.ClientContext = completionKey;
    CHECK(TqStreamLifetime::Callback(nullptr, weak.lock().get(), &canceled) == QUIC_STATUS_SUCCESS);
    CHECK(cleanups == 1);
    weak.lock().reset();
    CHECK(weak.expired());
}

void ManagedLeaseHeldAcrossTerminalReleasesOnce() {
    AdoptedManagedHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    auto lease = harness.Owner->TryAcquireApi();
    CHECK(static_cast<bool>(lease));
    std::atomic<bool> terminalReturned{false};
    std::atomic<bool> terminalOk{false};
    std::thread terminalThread([&] {
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        terminalOk.store(
            QUIC_SUCCEEDED(harness.DispatchViaRouter(terminal)),
            std::memory_order_release);
        terminalReturned.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!terminalReturned.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    terminalThread.join();
    CHECK(terminalReturned.load(std::memory_order_acquire));
    CHECK(terminalOk.load(std::memory_order_acquire));
    CHECK(harness.Owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished);
    CHECK(!harness.Owner->TryAcquireApi());
    std::weak_ptr<TqStreamLifetime> weakOwner = harness.Owner;
    std::shared_ptr<void> binding =
        harness.Worker.StreamCallbackContextOwnerForTest(harness.Result.RelayId);
    CHECK(binding != nullptr);
    lease = {};
    CHECK(!weakOwner.expired());
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(harness.Worker.RelayStreamForTest(harness.Result.RelayId) == nullptr);
    binding.reset();
    harness.Worker.Stop();
    harness.Owner.reset();
    CHECK(weakOwner.expired());
    if (harness.Fds[0] != TqInvalidSocket) {
        ::close(harness.Fds[0]);
        harness.Fds[0] = TqInvalidSocket;
    }
}

void ManagedRegisterStartFailedPublishRejectsAndTerminalClosesOnce() {
    auto target = std::make_shared<CountingTarget>();
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Starting, target);
    QUIC_STREAM_EVENT failed{};
    failed.Type = QUIC_STREAM_EVENT_START_COMPLETE;
    failed.START_COMPLETE.Status = QUIC_STATUS_INTERNAL_ERROR;
    CHECK(owner->DispatchForTest(&failed) == QUIC_STATUS_SUCCESS);
    CHECK(owner->GetPhase() == TqStreamLifetime::Phase::StartFailed);

    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    harness.Owner = owner;
    harness.Stream = reinterpret_cast<MsQuicStream*>(harness.StreamStorage);
    harness.Stream->Callback = TqStreamLifetime::Callback;
    harness.Stream->Context = owner.get();
    CHECK(harness.Worker.StartForTest());
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = harness.Fds[1];
    registration.Stream = harness.Stream;
    registration.StreamOwner = owner;
    harness.Result = RegisterAndPublish(harness.Worker, registration, harness.Handle);
    CHECK(!harness.Result.Ok);
    CHECK(!harness.Result.TcpFdConsumed);
    CHECK(fcntl(harness.Fds[1], F_GETFD) >= 0);

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    CHECK(owner->GetPhase() == TqStreamLifetime::Phase::StartFailed);
    harness.StopAndClosePeer();
}

void ManagedRegisterBindingAllocFailureRejectsAndTerminalClosesOnce() {
    auto target = std::make_shared<CountingTarget>();
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started, target);

    TqDarwinRelayWorkerConfig config{};
    config.FailManagedBindingForTest = true;
    ManagedRelayHarness harness(config);
    CHECK(harness.OpenSocketPair());
    harness.Owner = owner;
    harness.Stream = reinterpret_cast<MsQuicStream*>(harness.StreamStorage);
    harness.Stream->Callback = TqStreamLifetime::Callback;
    harness.Stream->Context = owner.get();
    CHECK(harness.Worker.StartForTest());
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = harness.Fds[1];
    registration.Stream = harness.Stream;
    registration.StreamOwner = owner;
    harness.Result = RegisterAndPublish(harness.Worker, registration, harness.Handle);
    CHECK(!harness.Result.Ok);
    CHECK(!harness.Result.TcpFdConsumed);
    CHECK(fcntl(harness.Fds[1], F_GETFD) >= 0);

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    CHECK(owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished);
    harness.StopAndClosePeer();
}

void ManagedReceiveAllocationFailureUsesActiveShutdownNotTerminal() {
    ResetFakeReceiveComplete();
    TqDarwinRelayWorkerConfig config{};
    config.FailNextPendingReceiveAllocationForTest = true;
    config.MaxPendingQuicReceiveBytesPerRelay = 64 * 1024;
    AdoptedManagedHarness harness(config);
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    harness.Worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    g_fakeShutdownCount = 0;
    g_fakeLastShutdownFlags = QUIC_STREAM_SHUTDOWN_FLAG_NONE;
    std::weak_ptr<TqStreamLifetime> weakOwner = harness.Owner;

    const char payload[] = "alloc-fail";
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);

    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;
    receiveEvent.RECEIVE.TotalBufferLength = sizeof(payload) - 1;

    CHECK(harness.DispatchViaRouter(receiveEvent) == QUIC_STATUS_OUT_OF_MEMORY);
    CHECK(receiveEvent.RECEIVE.TotalBufferLength == 0);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(
        harness.Owner->GetPhase() == TqStreamLifetime::Phase::Started ||
        harness.Owner->GetPhase() == TqStreamLifetime::Phase::Starting);
    CHECK(!harness.Worker.BindingTerminalForTest(harness.Result.RelayId));
    CHECK(harness.Worker.QuicShutdownCompleteEnqueuedForTest() == 0);
    CHECK(harness.Worker.QuicActiveShutdownEnqueuedForTest() == 1);
    CHECK(
        harness.Worker.LastActiveShutdownReasonForTest() ==
        TqDarwinActiveShutdownReason::ReceiveAllocationFailed);
    CHECK(g_fakeShutdownCount == 1);
    CHECK((g_fakeLastShutdownFlags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND) != 0);
    CHECK((g_fakeLastShutdownFlags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT_RECEIVE) != 0);
    CHECK(harness.Worker.PendingEventsForTest() == 1);

    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(!harness.Worker.BindingTerminalForTest(harness.Result.RelayId));
    CHECK(
        harness.Owner->GetPhase() == TqStreamLifetime::Phase::Started ||
        harness.Owner->GetPhase() == TqStreamLifetime::Phase::Starting);
    CHECK(g_fakeShutdownCount == 1);
    CHECK(harness.Worker.StreamOwnerForTest(harness.Result.RelayId) != nullptr);

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished);
    CHECK(harness.Worker.QuicShutdownCompleteEnqueuedForTest() == 1);
    CHECK(harness.Worker.DrainOneEventForTest());

    harness.Worker.SetReceiveCompleteForTest(nullptr);
    harness.StopAndClosePeer();
    CHECK(weakOwner.expired());
}

void FullyClosedStopAfterEofAndPeerFinWithoutShutdownComplete() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register(/*enableQuicSends=*/false));
    auto control = harness.Handle.Control;
    CHECK(control != nullptr);
    CHECK(!control->Stop.load(std::memory_order_acquire));

    CHECK(::shutdown(harness.Fds[0], SHUT_WR) == 0);
    CHECK(harness.Worker.InvokeTcpEventForTest(
        harness.Result.RelayId, EVFILT_READ, 0, 0));

    QUIC_STREAM_EVENT peerShutdown{};
    peerShutdown.Type = QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN;
    CHECK(harness.DispatchViaRouter(peerShutdown) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(harness.Worker.FlushTcpWritableForTest(harness.Result.RelayId));

    CHECK(control->Stop.load(std::memory_order_acquire));
    CHECK(TqRelayControlStopSignaledCount().load(std::memory_order_relaxed) >= 1);
    harness.StopAndClosePeer();
}

void FullyClosedPeerSendShutdownStickySurvivesQueueFull() {
    TqDarwinRelayWorkerConfig config{};
    config.EventQueueCapacity = 2;
    ManagedRelayHarness harness(config);
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register(/*enableQuicSends=*/false));

    CHECK(harness.Worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(harness.Worker.EnqueueForTest(TestMarkerEvent(2)));
    CHECK(harness.Worker.PendingEventsForTest() >= 2);

    QUIC_STREAM_EVENT peerShutdown{};
    peerShutdown.Type = QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN;
    CHECK(harness.DispatchViaRouter(peerShutdown) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.PeerSendShutdownStickyForTest(harness.Result.RelayId));

    while (harness.Worker.DrainOneEventForTest()) {
    }
    CHECK(harness.Worker.DrainWakeForTest() >= 0);
    CHECK(harness.Worker.TcpWriteShutdownQueuedOrClosedForTest(harness.Result.RelayId));
    harness.StopAndClosePeer();
}

void FullyClosedOffWorkerFinSetsConvergenceStickyThenStops() {
    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelayWorkerConfig config{};
    config.EventQueueCapacity = 2;
    ManagedRelayHarness harness(config);
    CHECK(harness.OpenSocketPair());
    harness.Worker.SetStreamSendForTest(FakeStreamSend);
    CHECK(harness.Register(/*enableQuicSends=*/true));
    CHECK(harness.Owner->InstallStreamForTest(harness.Stream));

    auto control = harness.Handle.Control;
    CHECK(control != nullptr);
    CHECK(!control->Stop.load(std::memory_order_acquire));

    CHECK(::shutdown(harness.Fds[0], SHUT_WR) == 0);
    CHECK(harness.Worker.InvokeTcpEventForTest(
        harness.Result.RelayId, EVFILT_READ, 0, 0));
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 1);
    CHECK(harness.Worker.InFlightQuicSendCountForTest(harness.Result.RelayId) == 1);

    CHECK(harness.Worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(harness.Worker.EnqueueForTest(TestMarkerEvent(2)));
    CHECK(harness.Worker.PendingEventsForTest() >= 2);

    void* callbackContext = harness.Worker.StreamCallbackContextForTest(harness.Result.RelayId);
    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(callbackContext != nullptr);
    CHECK(sendContext != nullptr);

    const uint64_t fallbackBefore = harness.Worker.FallbackSendCompletionCountForTest();
    harness.Worker.MarkWorkerThreadExitedForTest();

    QUIC_STREAM_EVENT finComplete{};
    finComplete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    finComplete.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(harness.DispatchViaRouter(finComplete) == QUIC_STATUS_SUCCESS);

    CHECK(harness.Worker.FallbackSendCompletionCountForTest() > fallbackBefore);
    CHECK(harness.Worker.ConvergenceCheckStickyForTest(harness.Result.RelayId));
    CHECK(!control->Stop.load(std::memory_order_acquire));

    CHECK(harness.Worker.StartForTest());
    while (harness.Worker.DrainOneEventForTest()) {
    }

    QUIC_STREAM_EVENT peerShutdown{};
    peerShutdown.Type = QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN;
    CHECK(harness.DispatchViaRouter(peerShutdown) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(harness.Worker.DrainWakeForTest() >= 0);
    CHECK(harness.Worker.FlushTcpWritableForTest(harness.Result.RelayId));

    CHECK(control->Stop.load(std::memory_order_acquire));
    CHECK(TqRelayControlStopSignaledCount().load(std::memory_order_relaxed) >= 1);

    harness.Worker.SetStreamSendForTest(nullptr);
    harness.Owner->ReleaseStreamForTest();
    harness.StopAndClosePeer();
}

void FullyClosedStopPeerFinBeforeTcpEof() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register(false));
    auto control = harness.Handle.Control;

    QUIC_STREAM_EVENT peerShutdown{};
    peerShutdown.Type = QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN;
    CHECK(harness.DispatchViaRouter(peerShutdown) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(harness.Worker.FlushTcpWritableForTest(harness.Result.RelayId));
    CHECK(!control->Stop.load(std::memory_order_acquire)); // missing TcpReadClosed

    CHECK(::shutdown(harness.Fds[0], SHUT_WR) == 0);
    CHECK(harness.Worker.InvokeTcpEventForTest(
        harness.Result.RelayId, EVFILT_READ, 0, 0));
    CHECK(control->Stop.load(std::memory_order_acquire));
    harness.StopAndClosePeer();
}

void FullyClosedCallbackPendingBlocksStopUntilDrained() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register(/*enableQuicSends=*/false));
    auto control = harness.Handle.Control;
    CHECK(control != nullptr);

    harness.Worker.SetCallbackPendingReceiveForTest(harness.Result.RelayId, 64);
    CHECK(harness.Worker.CallbackPendingReceiveBytesForTest(harness.Result.RelayId) > 0);

    CHECK(::shutdown(harness.Fds[0], SHUT_WR) == 0);
    CHECK(harness.Worker.InvokeTcpEventForTest(
        harness.Result.RelayId, EVFILT_READ, 0, 0));

    QUIC_STREAM_EVENT peerShutdown{};
    peerShutdown.Type = QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN;
    CHECK(harness.DispatchViaRouter(peerShutdown) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(harness.Worker.FlushTcpWritableForTest(harness.Result.RelayId));
    CHECK(harness.Worker.CallbackPendingReceiveBytesForTest(harness.Result.RelayId) > 0);
    CHECK(!control->Stop.load(std::memory_order_acquire));

    harness.Worker.DrainCallbackPendingReceiveForTest(harness.Result.RelayId);
    CHECK(harness.Worker.CallbackPendingReceiveBytesForTest(harness.Result.RelayId) == 0);
    CHECK(control->Stop.load(std::memory_order_acquire));

    harness.StopAndClosePeer();
}

void FullyClosedDuplicatePeerFinStopIdempotent() {
    TqDarwinRelayWorkerConfig config{};
    config.EventQueueCapacity = 2;
    ManagedRelayHarness harness(config);
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register(/*enableQuicSends=*/false));
    auto control = harness.Handle.Control;
    CHECK(control != nullptr);

    const uint64_t stopBefore =
        TqRelayControlStopSignaledCount().load(std::memory_order_relaxed);

    CHECK(::shutdown(harness.Fds[0], SHUT_WR) == 0);
    CHECK(harness.Worker.InvokeTcpEventForTest(
        harness.Result.RelayId, EVFILT_READ, 0, 0));

    CHECK(harness.Worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(harness.Worker.EnqueueForTest(TestMarkerEvent(2)));

    QUIC_STREAM_EVENT peerShutdown{};
    peerShutdown.Type = QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN;
    CHECK(harness.DispatchViaRouter(peerShutdown) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.PeerSendShutdownStickyForTest(harness.Result.RelayId));
    CHECK(harness.DispatchViaRouter(peerShutdown) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.PeerSendShutdownStickyForTest(harness.Result.RelayId));

    while (harness.Worker.DrainOneEventForTest()) {
    }
    CHECK(harness.Worker.DrainWakeForTest() >= 0);
    CHECK(harness.Worker.FlushTcpWritableForTest(harness.Result.RelayId));

    CHECK(control->Stop.load(std::memory_order_acquire));
    CHECK(
        TqRelayControlStopSignaledCount().load(std::memory_order_relaxed) == stopBefore + 1);

    CHECK(harness.DispatchViaRouter(peerShutdown) == QUIC_STATUS_SUCCESS);
    while (harness.Worker.DrainOneEventForTest()) {
    }
    CHECK(harness.Worker.FlushTcpWritableForTest(harness.Result.RelayId));
    CHECK(control->Stop.load(std::memory_order_acquire));
    CHECK(
        TqRelayControlStopSignaledCount().load(std::memory_order_relaxed) == stopBefore + 1);

    harness.StopAndClosePeer();
}

void FullyClosedCanceledFinDoesNotCompleteOrStop() {
    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    harness.Worker.SetStreamSendForTest(FakeStreamSend);
    CHECK(harness.Register(/*enableQuicSends=*/true));
    CHECK(harness.Owner->InstallStreamForTest(harness.Stream));
    auto control = harness.Handle.Control;
    CHECK(control != nullptr);
    CHECK(!control->Stop.load(std::memory_order_acquire));

    CHECK(::shutdown(harness.Fds[0], SHUT_WR) == 0);
    CHECK(harness.Worker.InvokeTcpEventForTest(
        harness.Result.RelayId, EVFILT_READ, 0, 0));
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 1);
    CHECK(harness.Worker.InFlightQuicSendCountForTest(harness.Result.RelayId) == 1);

    void* sendContext = g_lastSendContext.load(std::memory_order_acquire);
    CHECK(sendContext != nullptr);

    QUIC_STREAM_EVENT canceledFin{};
    canceledFin.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    canceledFin.SEND_COMPLETE.Canceled = true;
    canceledFin.SEND_COMPLETE.ClientContext = sendContext;
    CHECK(harness.DispatchViaRouter(canceledFin) == QUIC_STATUS_SUCCESS);
    (void)harness.Worker.DrainOneEventForTest();
    CHECK(harness.Worker.InFlightQuicSendCountForTest(harness.Result.RelayId) == 0);

    CHECK(harness.Worker.Snapshot().QuicSendFinCompletedRelays == 0);
    CHECK(!control->Stop.load(std::memory_order_acquire));

    QUIC_STREAM_EVENT peerShutdown{};
    peerShutdown.Type = QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN;
    CHECK(harness.DispatchViaRouter(peerShutdown) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(harness.Worker.FlushTcpWritableForTest(harness.Result.RelayId));
    CHECK(!control->Stop.load(std::memory_order_acquire));

    harness.Owner->ReleaseStreamForTest();
    harness.Worker.SetStreamSendForTest(nullptr);
    harness.StopAndClosePeer();
}

void ManagedReceiveAllocationFailureQueueFullKeepsOwnerViaSink() {
    ResetFakeReceiveComplete();
    TqDarwinRelayWorkerConfig config{};
    config.FailNextPendingReceiveAllocationForTest = true;
    config.EventQueueCapacity = 2;
    config.MaxPendingQuicReceiveBytesPerRelay = 64 * 1024;
    AdoptedManagedHarness harness(config);
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    harness.Worker.SetReceiveCompleteForTest(FakeReceiveComplete);
    g_fakeShutdownCount = 0;
    std::weak_ptr<TqStreamLifetime> weakOwner = harness.Owner;
    std::shared_ptr<TqRelayStopControl> control = harness.Handle.Control;
    CHECK(control != nullptr);

    CHECK(harness.Worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(harness.Worker.EnqueueForTest(TestMarkerEvent(2)));
    CHECK(harness.Worker.PendingEventsForTest() == 2);

    const char payload[] = "alloc-qfull";
    QUIC_BUFFER quicBuffer{};
    quicBuffer.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(payload));
    quicBuffer.Length = static_cast<uint32_t>(sizeof(payload) - 1);

    QUIC_STREAM_EVENT receiveEvent{};
    receiveEvent.Type = QUIC_STREAM_EVENT_RECEIVE;
    receiveEvent.RECEIVE.BufferCount = 1;
    receiveEvent.RECEIVE.Buffers = &quicBuffer;
    receiveEvent.RECEIVE.TotalBufferLength = sizeof(payload) - 1;

    CHECK(harness.DispatchViaRouter(receiveEvent) == QUIC_STATUS_OUT_OF_MEMORY);
    CHECK(receiveEvent.RECEIVE.TotalBufferLength == 0);
    CHECK(g_receiveCompleteCalls.load(std::memory_order_acquire) == 0);
    CHECK(harness.Worker.QuicActiveShutdownEnqueuedForTest() == 0);
    CHECK(harness.Worker.QuicShutdownCompleteEnqueuedForTest() == 0);
    CHECK(harness.Worker.PendingEventsForTest() == 2);
    CHECK(g_fakeShutdownCount == 1);
    CHECK(control->Stop.load(std::memory_order_acquire));
    CHECK(
        harness.Owner->GetPhase() == TqStreamLifetime::Phase::Started ||
        harness.Owner->GetPhase() == TqStreamLifetime::Phase::Starting);
    CHECK(harness.Worker.StreamOwnerForTest(harness.Result.RelayId) != nullptr);
    CHECK(!weakOwner.expired());

    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(harness.Worker.DrainOneEventForTest());
    harness.Worker.MarkWorkerThreadExitedForTest();

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished);

    harness.Worker.SetReceiveCompleteForTest(nullptr);
    harness.StopAndClosePeer();
    CHECK(weakOwner.expired());
    CHECK(control->Stop.load(std::memory_order_acquire));
}

void ManagedLateKqueueEventAfterFdReuseMissesNewRelay() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    const uint64_t staleRelayId = harness.Result.RelayId;
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());
    const int relayFd = harness.Fds[1];
    CHECK(TcpRelayFdClosedOnce(relayFd));

    int fdsB[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fdsB) == 0);
    alignas(MsQuicStream) unsigned char streamStorageB[sizeof(MsQuicStream)]{};
    auto* streamB = reinterpret_cast<MsQuicStream*>(streamStorageB);
    auto ownerB = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    streamB->Callback = TqStreamLifetime::Callback;
    streamB->Context = ownerB.get();
    TqRelayHandle handleB{};
    TqDarwinRelayRegistration registrationB{};
    registrationB.TcpFd = fdsB[1];
    registrationB.Stream = streamB;
    registrationB.StreamOwner = ownerB;
    const auto resultB = RegisterAndPublish(harness.Worker, registrationB, handleB);
    CHECK(resultB.Ok);
    CHECK(resultB.RelayId != staleRelayId);

    CHECK(harness.Worker.InvokeTcpEventForTest(
        staleRelayId, EVFILT_READ, EV_ERROR, ECONNRESET));
    CHECK(harness.Worker.Snapshot().ActiveRelays == 1);
    CHECK(harness.Worker.BindingActiveForTest(resultB.RelayId));
    CHECK(fcntl(fdsB[1], F_GETFD) >= 0);

    QUIC_STREAM_EVENT terminalB{};
    terminalB.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(ownerB->DispatchForTest(&terminalB) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());
    CHECK(TcpRelayFdClosedOnce(fdsB[1]));

    int reused[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, reused) == 0);
    if (reused[1] != relayFd) {
        CHECK(dup2(reused[1], relayFd) == relayFd);
        ::close(reused[1]);
    }
    ::close(reused[0]);
    CHECK(fcntl(relayFd, F_GETFD) >= 0);

    CHECK(harness.Worker.InvokeTcpEventForTest(
        staleRelayId, EVFILT_READ, EV_ERROR, ECONNRESET));
    CHECK(fcntl(relayFd, F_GETFD) >= 0);
    ::close(relayFd);
    harness.Owner.reset();
    harness.Worker.Stop();
    ::close(fdsB[0]);
}

void ManagedSyncNoCallbackStartRejectRejectsPublishAndLease() {
    auto target = std::make_shared<CountingTarget>();
    auto owner = TqStreamLifetime::CreateForTest(
        TqStreamLifetime::Phase::CreatedNotStarted, target);
    CHECK(owner->BeginStart());
    const auto snapshot = owner->PublishStartFailureAndTakeTarget(QUIC_STATUS_INTERNAL_ERROR);
    CHECK(snapshot.TargetOwner == target);
    CHECK(owner->GetPhase() == TqStreamLifetime::Phase::StartFailed);
    CHECK(!owner->PublishTarget(owner->RouteGeneration(), target));
    CHECK(!owner->TryAcquireApi());
}

void ManagedShutdownApiFailureRetainsDesiredIntent() {
    ScopedFakeMsQuicApi fakeApi;
    const HQUIC raw = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x6300));
    auto target = std::make_shared<CountingTarget>();
    auto owner = TqStreamLifetime::AdoptAccepted(
        raw,
        target,
        TqTerminalIdentity{
            0x6300, 0x6300, 0x6300, 0x6300,
            TqTunnelRole::ServerOpen, TqRelayBackendType::DarwinWorker},
        5);
    CHECK(owner != nullptr);
    g_fakeShutdownFail = true;
    CHECK(QUIC_FAILED(owner->RequestShutdown(TqStreamLifetime::ShutdownIntent::GracefulSend)));
    CHECK(g_fakeShutdownCount == 1);
    g_fakeShutdownFail = false;
    CHECK(QUIC_SUCCEEDED(owner->RequestShutdown(TqStreamLifetime::ShutdownIntent::GracefulSend)));
    CHECK(g_fakeShutdownCount == 2);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    (void)owner->DispatchForTest(&terminal);
    owner.reset();
}

void ManagedShutdownDuplicateGracefulUpgradesAbort() {
    ScopedFakeMsQuicApi fakeApi;
    const HQUIC raw = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x5000));
    auto target = std::make_shared<CountingTarget>();
    auto owner = TqStreamLifetime::AdoptAccepted(
        raw,
        target,
        TqTerminalIdentity{
            0x5000, 0x5000, 0x5000, 0x5000,
            TqTunnelRole::ServerOpen, TqRelayBackendType::DarwinWorker},
        5);
    CHECK(owner != nullptr);
    CHECK(QUIC_SUCCEEDED(owner->RequestShutdown(TqStreamLifetime::ShutdownIntent::GracefulSend)));
    CHECK(QUIC_SUCCEEDED(owner->RequestShutdown(TqStreamLifetime::ShutdownIntent::GracefulSend)));
    CHECK(g_fakeShutdownCount == 1);
    CHECK(QUIC_SUCCEEDED(owner->RequestShutdown(TqStreamLifetime::ShutdownIntent::AbortSend)));
    CHECK(g_fakeShutdownCount == 2);
    CHECK((g_fakeLastShutdownFlags & QUIC_STREAM_SHUTDOWN_FLAG_ABORT_SEND) != 0);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    (void)owner->DispatchForTest(&terminal);
    owner.reset();
}

void ManagedShutdownTerminalWinsOverDesired() {
    ScopedFakeMsQuicApi fakeApi;
    const HQUIC raw = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x5100));
    auto target = std::make_shared<CountingTarget>();
    auto owner = TqStreamLifetime::AdoptAccepted(
        raw,
        target,
        TqTerminalIdentity{
            0x5100, 0x5100, 0x5100, 0x5100,
            TqTunnelRole::ServerOpen, TqRelayBackendType::DarwinWorker},
        5);
    CHECK(owner != nullptr);
    CHECK(QUIC_SUCCEEDED(owner->RequestShutdown(TqStreamLifetime::ShutdownIntent::GracefulSend)));
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
    CHECK(owner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished);
    CHECK(QUIC_FAILED(owner->RequestShutdown(TqStreamLifetime::ShutdownIntent::AbortSend)));
    CHECK(g_fakeShutdownCount == 1);
    owner.reset();
}

void ManagedWorkerStopAfterTerminalQueuesCleanup() {
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    harness.Worker.Stop();
    errno = 0;
    CHECK(fcntl(harness.Fds[1], F_GETFD) == -1 && errno == EBADF);
    ::close(harness.Fds[0]);
    harness.Fds[0] = TqInvalidSocket;
}

void ManagedLateSendCompletionAfterTargetSwitch() {
    auto first = std::make_shared<CountingTarget>();
    auto second = std::make_shared<CountingTarget>();
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started, first);
    unsigned cleanups = 0;
    void* completionKey = owner->RegisterSendCompletion(nullptr, [&] { ++cleanups; });
    CHECK(completionKey != nullptr);
    uint64_t published = 0;
    CHECK(owner->PublishTarget(owner->RouteGeneration(), second, &published));
    QUIC_STREAM_EVENT complete{};
    complete.Type = QUIC_STREAM_EVENT_SEND_COMPLETE;
    complete.SEND_COMPLETE.ClientContext = completionKey;
    CHECK(owner->DispatchForTest(&complete) == QUIC_STATUS_SUCCESS);
    CHECK(first->Calls == 1);
    CHECK(second->Calls == 0);
    CHECK(cleanups == 1);
}

void ManagedPrepareFailureLeavesFdWithCaller() {
    TqDarwinRelayWorkerConfig config{};
    config.FailPrepareForTest = true;
    ManagedRelayHarness harness(config);
    CHECK(harness.OpenSocketPair());
    harness.Owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    harness.Stream = reinterpret_cast<MsQuicStream*>(harness.StreamStorage);
    CHECK(harness.Worker.StartForTest());
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = harness.Fds[1];
    registration.Stream = harness.Stream;
    registration.StreamOwner = harness.Owner;
    harness.Result = RegisterAndPublish(harness.Worker, registration, harness.Handle);
    CHECK(!harness.Result.Ok);
    CHECK(!harness.Result.TcpFdConsumed);
    CHECK(fcntl(harness.Fds[1], F_GETFD) >= 0);
    harness.StopAndClosePeer();
    ::close(harness.Fds[1]);
}

void ManagedCommitFailureClosesFdOnceWithoutReuseHit() {
    g_PrecommitOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    g_PrecommitPublishTerminal = false;
    TqDarwinRelayWorkerConfig config{};
    config.FailCommitForTest = true;
    config.AfterPublishHookForTest = AfterManagedPublishHook;
    TqDarwinRelayWorker worker(config);
    CHECK(worker.StartForTest());
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    const int consumedFd = fds[1];
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = consumedFd;
    registration.Stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    registration.StreamOwner = g_PrecommitOwner;
    const auto result = RegisterAndPublish(worker, registration, handle);
    CHECK(!result.Ok);
    CHECK(result.TcpFdConsumed);
    CHECK(fcntl(consumedFd, F_GETFD) == -1 && errno == EBADF);
    int reused[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, reused) == 0);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    (void)g_PrecommitOwner->DispatchForTest(&terminal);
    g_PrecommitOwner.reset();
    worker.Stop();
    CHECK(fcntl(reused[0], F_GETFD) >= 0);
    CHECK(fcntl(reused[1], F_GETFD) >= 0);
    ::close(fds[0]);
    ::close(reused[0]);
    ::close(reused[1]);
}

void ManagedRegisterRejectsTerminalOwnerBeforePublish() {
    auto owner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    (void)owner->PublishTerminalAndTakeTarget();
    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    harness.Owner = owner;
    harness.Stream = reinterpret_cast<MsQuicStream*>(harness.StreamStorage);
    CHECK(harness.Worker.StartForTest());
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = harness.Fds[1];
    registration.Stream = harness.Stream;
    registration.StreamOwner = owner;
    harness.Result = RegisterAndPublish(harness.Worker, registration, harness.Handle);
    CHECK(!harness.Result.Ok);
    CHECK(!harness.Result.TcpFdConsumed);
    CHECK(fcntl(harness.Fds[1], F_GETFD) >= 0);
    harness.StopAndClosePeer();
}

// Task 3 / P1-2: completion key registered, then inject a pre-Send exit.
enum class SendRegisterExitKind : int {
    None = 0,
    // binding null/inactive via terminal seal before active recheck
    BindingInactiveViaTerminal = 1,
    RelayClosing = 2,
    StreamMismatch = 3,
    TryMarkRegisteredFailure = 4,
};

std::atomic<int> g_SendRegisterExitKind{
    static_cast<int>(SendRegisterExitKind::None)};

void AfterRegisterSendCompletionExitHook(
    TqDarwinRelayWorker* worker,
    uint64_t relayId,
    TqDarwinRelaySendOperation* operation) {
    g_SendRegisterBarrierFired.store(true, std::memory_order_release);
    const auto kind = static_cast<SendRegisterExitKind>(
        g_SendRegisterExitKind.load(std::memory_order_acquire));
    switch (kind) {
    case SendRegisterExitKind::BindingInactiveViaTerminal:
        CHECK(g_SendRegisterBarrierOwner != nullptr);
        {
            QUIC_STREAM_EVENT terminal{};
            terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
            CHECK(g_SendRegisterBarrierOwner->DispatchForTest(&terminal) ==
                  QUIC_STATUS_SUCCESS);
        }
        break;
    case SendRegisterExitKind::RelayClosing:
        CHECK(worker != nullptr);
        worker->MarkRelayClosingForTest(relayId);
        break;
    case SendRegisterExitKind::StreamMismatch:
        CHECK(worker != nullptr);
        worker->SetRelayStreamForTest(
            relayId,
            reinterpret_cast<MsQuicStream*>(static_cast<uintptr_t>(0xDEADBEEF)));
        break;
    case SendRegisterExitKind::TryMarkRegisteredFailure:
        CHECK(operation != nullptr);
        CHECK(operation->TryMarkRegistered());
        break;
    case SendRegisterExitKind::None:
        break;
    }
}

void AfterRegisterSendCompletionTerminalHook(
    TqDarwinRelayWorker* worker,
    uint64_t relayId,
    TqDarwinRelaySendOperation* operation) {
    g_SendRegisterExitKind.store(
        static_cast<int>(SendRegisterExitKind::BindingInactiveViaTerminal),
        std::memory_order_release);
    AfterRegisterSendCompletionExitHook(worker, relayId, operation);
}

void ManagedSendRegisterThenTerminalBeforeActiveRecheckRollsBack() {
    ResetFakeStreamSend(QUIC_STATUS_SUCCESS);
    TqDarwinRelaySendOperation::DestructorCount.store(0, std::memory_order_relaxed);
    const auto baseline = TqStreamLifetime::SnapshotSendCompletions();

    TqDarwinRelayWorkerConfig config{};
    config.AfterRegisterSendCompletionHookForTest = AfterRegisterSendCompletionTerminalHook;
    ManagedRelayHarness harness(config);
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register(true));
    std::memset(harness.StreamStorage, 0, sizeof(harness.StreamStorage));
    harness.Stream = reinterpret_cast<MsQuicStream*>(harness.StreamStorage);
    CHECK(harness.Owner->InstallStreamForTest(harness.Stream));
    g_SendRegisterBarrierOwner = harness.Owner;
    g_SendRegisterBarrierFired.store(false, std::memory_order_release);
    g_SendRegisterExitKind.store(
        static_cast<int>(SendRegisterExitKind::BindingInactiveViaTerminal),
        std::memory_order_release);
    harness.Worker.SetStreamSendForTest(FakeStreamSend);

    const uint64_t dtorsBefore = harness.Worker.SendOperationDestructorCountForTest();
    const uint64_t bufferBefore =
        harness.Worker.PendingQuicSendBufferBytesForTest(harness.Result.RelayId);
    const char payload[] = "p1-2-barrier";
    CHECK(write(harness.Fds[0], payload, sizeof(payload) - 1) ==
          static_cast<ssize_t>(sizeof(payload) - 1));
    CHECK(harness.Worker.InvokeTcpEventForTest(harness.Result.RelayId, EVFILT_READ, 0, 0));

    CHECK(g_SendRegisterBarrierFired.load(std::memory_order_acquire));
    CHECK(g_sendCalls.load(std::memory_order_acquire) == 0);
    CHECK(harness.Worker.SendOperationDestructorCountForTest() == dtorsBefore + 1);
    CHECK(harness.Worker.InFlightQuicSendCountForTest(harness.Result.RelayId) == 0);
    CHECK(harness.Worker.PendingQuicSendBufferBytesForTest(harness.Result.RelayId) ==
          bufferBefore);

    const auto after = TqStreamLifetime::SnapshotSendCompletions();
    CHECK(after.ActiveCount == baseline.ActiveCount);
    CHECK(after.PreSubmitRollbacks >= baseline.PreSubmitRollbacks + 1);
    CHECK(after.OldestAgeMs == 0 || after.ActiveCount == 0);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (harness.Worker.PendingEventsForTest() != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        (void)harness.Worker.DrainOneEventForTest();
    }
    TqRelayStop(&harness.Handle);
    std::weak_ptr<TqStreamLifetime> weakOwner = harness.Owner;
    g_SendRegisterBarrierOwner.reset();
    harness.Owner->ReleaseStreamForTest();
    harness.Owner.reset();
    harness.Worker.SetStreamSendForTest(nullptr);
    harness.StopAndClosePeer();
    CHECK(weakOwner.expired());

    const auto finalSnap = harness.Worker.Snapshot();
    CHECK(finalSnap.ActiveSendReservations == 0);
    CHECK(finalSnap.SendReservationOldestAgeMs == 0);
    g_SendRegisterBarrierFired.store(false, std::memory_order_release);
    g_SendRegisterExitKind.store(
        static_cast<int>(SendRegisterExitKind::None),
        std::memory_order_release);
}

void ManagedSendRegisterExitPointsRollBackRegistry() {
    enum class ExitKind {
        BindingInactiveViaTerminal,
        RelayClosing,
        StreamMismatch,
        TryMarkRegisteredFailure,
        RegistryFailureAfterRegister,
        // Fatal Send sync failure: destroys/cancels without OOM requeue.
        SendSyncFailureFatal,
        // OOM takes the requeue path — labeled separately from sync-failure rollback.
        SendOomRequeue,
        CallbackBeforeReturn,
        TerminalBeforeSend,
    };
    struct Case {
        const char* Name;
        ExitKind Kind;
        QUIC_STATUS SendStatus;
        bool ExpectSendCalled;
        bool ExpectOpDtor;
        bool ExpectPreSubmitRollback;
        bool NeedsFakeApi;
        bool ExpectPendingRequeue;
    };

    const Case cases[] = {
        {"binding_inactive_via_terminal",
         ExitKind::BindingInactiveViaTerminal,
         QUIC_STATUS_SUCCESS,
         false,
         true,
         true,
         false,
         false},
        {"relay_closing",
         ExitKind::RelayClosing,
         QUIC_STATUS_SUCCESS,
         false,
         true,
         true,
         true,
         false},
        {"stream_mismatch",
         ExitKind::StreamMismatch,
         QUIC_STATUS_SUCCESS,
         false,
         true,
         true,
         true,
         false},
        {"try_mark_registered_failure",
         ExitKind::TryMarkRegisteredFailure,
         QUIC_STATUS_SUCCESS,
         false,
         true,
         true,
         true,
         false},
        {"registry_failure_after_register",
         ExitKind::RegistryFailureAfterRegister,
         QUIC_STATUS_SUCCESS,
         false,
         true,
         true,
         true,
         false},
        {"send_sync_failure",
         ExitKind::SendSyncFailureFatal,
         QUIC_STATUS_INTERNAL_ERROR,
         true,
         true,
         true,
         true,
         false},
        {"send_oom_requeue",
         ExitKind::SendOomRequeue,
         QUIC_STATUS_OUT_OF_MEMORY,
         true,
         false,
         true,
         true,
         true},
        {"callback_before_return",
         ExitKind::CallbackBeforeReturn,
         QUIC_STATUS_SUCCESS,
         true,
         true,
         false,
         false,
         false},
        {"terminal_before_send",
         ExitKind::TerminalBeforeSend,
         QUIC_STATUS_SUCCESS,
         false,
         true,
         true,
         false,
         false},
    };

    for (const auto& testCase : cases) {
        std::unique_ptr<ScopedFakeMsQuicApi> fakeApi;
        if (testCase.NeedsFakeApi) {
            fakeApi = std::make_unique<ScopedFakeMsQuicApi>();
        }

        ResetFakeStreamSend(testCase.SendStatus);
        TqDarwinRelaySendOperation::DestructorCount.store(0, std::memory_order_relaxed);
        const auto baseline = TqStreamLifetime::SnapshotSendCompletions();

        TqDarwinRelayWorkerConfig config{};
        switch (testCase.Kind) {
        case ExitKind::BindingInactiveViaTerminal:
        case ExitKind::TerminalBeforeSend:
            g_SendRegisterExitKind.store(
                static_cast<int>(SendRegisterExitKind::BindingInactiveViaTerminal),
                std::memory_order_release);
            config.AfterRegisterSendCompletionHookForTest =
                AfterRegisterSendCompletionExitHook;
            break;
        case ExitKind::RelayClosing:
            g_SendRegisterExitKind.store(
                static_cast<int>(SendRegisterExitKind::RelayClosing),
                std::memory_order_release);
            config.AfterRegisterSendCompletionHookForTest =
                AfterRegisterSendCompletionExitHook;
            break;
        case ExitKind::StreamMismatch:
            g_SendRegisterExitKind.store(
                static_cast<int>(SendRegisterExitKind::StreamMismatch),
                std::memory_order_release);
            config.AfterRegisterSendCompletionHookForTest =
                AfterRegisterSendCompletionExitHook;
            break;
        case ExitKind::TryMarkRegisteredFailure:
            g_SendRegisterExitKind.store(
                static_cast<int>(SendRegisterExitKind::TryMarkRegisteredFailure),
                std::memory_order_release);
            config.AfterRegisterSendCompletionHookForTest =
                AfterRegisterSendCompletionExitHook;
            break;
        case ExitKind::RegistryFailureAfterRegister:
            config.FailNextSendCompletionRegisterForTest = true;
            break;
        case ExitKind::SendSyncFailureFatal:
        case ExitKind::SendOomRequeue:
        case ExitKind::CallbackBeforeReturn:
            g_SendRegisterExitKind.store(
                static_cast<int>(SendRegisterExitKind::None),
                std::memory_order_release);
            break;
        }

        ManagedRelayHarness harness(config);
        CHECK(harness.OpenSocketPair());
        CHECK(harness.Register(true));
        std::memset(harness.StreamStorage, 0, sizeof(harness.StreamStorage));
        harness.Stream = reinterpret_cast<MsQuicStream*>(harness.StreamStorage);
        CHECK(harness.Owner->InstallStreamForTest(harness.Stream));
        g_SendRegisterBarrierOwner = harness.Owner;
        g_SendRegisterBarrierFired.store(false, std::memory_order_release);

        if (testCase.Kind == ExitKind::CallbackBeforeReturn) {
            g_completeBeforeSendReturns.store(true, std::memory_order_release);
            harness.Worker.SetStreamSendForTest(FakeStreamSendViaOwner);
        } else {
            harness.Worker.SetStreamSendForTest(FakeStreamSend);
        }

        const uint64_t dtorsBefore = harness.Worker.SendOperationDestructorCountForTest();
        const uint64_t bufferBefore =
            harness.Worker.PendingQuicSendBufferBytesForTest(harness.Result.RelayId);
        const uint64_t inFlightBefore =
            harness.Worker.InFlightQuicSendCountForTest(harness.Result.RelayId);

        const char payload[] = "exit-point";
        CHECK(write(harness.Fds[0], payload, sizeof(payload) - 1) ==
              static_cast<ssize_t>(sizeof(payload) - 1));
        CHECK(harness.Worker.InvokeTcpEventForTest(harness.Result.RelayId, EVFILT_READ, 0, 0));

        if (testCase.Kind == ExitKind::StreamMismatch) {
            // Restore a non-dangling Stream pointer before teardown/CloseRelay.
            harness.Worker.SetRelayStreamForTest(harness.Result.RelayId, harness.Stream);
        }

        if (testCase.ExpectSendCalled) {
            CHECK(g_sendCalls.load(std::memory_order_acquire) >= 1);
        } else {
            CHECK(g_sendCalls.load(std::memory_order_acquire) == 0);
        }

        if (testCase.ExpectOpDtor) {
            CHECK(harness.Worker.SendOperationDestructorCountForTest() == dtorsBefore + 1);
        } else {
            CHECK(harness.Worker.SendOperationDestructorCountForTest() == dtorsBefore);
        }

        CHECK(harness.Worker.InFlightQuicSendCountForTest(harness.Result.RelayId) ==
              inFlightBefore);
        if (!testCase.ExpectPendingRequeue) {
            CHECK(harness.Worker.PendingQuicSendBufferBytesForTest(harness.Result.RelayId) ==
                  bufferBefore);
            CHECK(harness.Worker.PendingQuicSendCountForTest(harness.Result.RelayId) == 0);
        } else {
            CHECK(harness.Worker.PendingQuicSendCountForTest(harness.Result.RelayId) == 1);
        }

        const auto after = TqStreamLifetime::SnapshotSendCompletions();
        CHECK(after.ActiveCount == baseline.ActiveCount);
        if (testCase.ExpectPreSubmitRollback) {
            CHECK(after.PreSubmitRollbacks >= baseline.PreSubmitRollbacks + 1);
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (harness.Worker.PendingEventsForTest() != 0 &&
               std::chrono::steady_clock::now() < deadline) {
            (void)harness.Worker.DrainOneEventForTest();
        }

        const bool alreadyTerminal =
            testCase.Kind == ExitKind::BindingInactiveViaTerminal ||
            testCase.Kind == ExitKind::TerminalBeforeSend;
        if (!alreadyTerminal) {
            QUIC_STREAM_EVENT terminal{};
            terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
            CHECK(harness.Owner->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS);
            while (harness.Worker.PendingEventsForTest() != 0 &&
                   std::chrono::steady_clock::now() < deadline) {
                (void)harness.Worker.DrainOneEventForTest();
            }
        }

        if (testCase.ExpectPendingRequeue) {
            // Terminal/close should destroy the requeued operation.
            CHECK(harness.Worker.SendOperationDestructorCountForTest() >= dtorsBefore + 1);
            CHECK(harness.Worker.PendingQuicSendCountForTest(harness.Result.RelayId) == 0);
        }

        std::weak_ptr<TqStreamLifetime> weakOwner = harness.Owner;
        g_SendRegisterBarrierOwner.reset();
        TqRelayStop(&harness.Handle);
        harness.Owner->ReleaseStreamForTest();
        harness.Owner.reset();
        harness.Worker.SetStreamSendForTest(nullptr);
        g_completeBeforeSendReturns.store(false, std::memory_order_release);
        g_SendRegisterExitKind.store(
            static_cast<int>(SendRegisterExitKind::None),
            std::memory_order_release);
        harness.StopAndClosePeer();
        CHECK(weakOwner.expired());
        CHECK(harness.Worker.Snapshot().ActiveSendReservations == 0);
        CHECK(harness.Worker.Snapshot().SendReservationOldestAgeMs == 0);
        (void)testCase.Name;
    }
}

void ManagedPublishHookTerminalRejectsCommitAfterTask3() {
    g_PrecommitOwner = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    g_PrecommitPublishTerminal = true;
    TqDarwinRelayWorkerConfig config{};
    config.AfterPublishHookForTest = AfterManagedPublishHook;
    TqDarwinRelayWorker worker(config);
    CHECK(worker.StartForTest());
    int fds[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    alignas(MsQuicStream) unsigned char streamStorage[sizeof(MsQuicStream)]{};
    TqRelayHandle handle{};
    TqDarwinRelayRegistration registration{};
    registration.TcpFd = fds[1];
    registration.Stream = reinterpret_cast<MsQuicStream*>(streamStorage);
    registration.StreamOwner = g_PrecommitOwner;
    const auto result = RegisterAndPublish(worker, registration, handle);
    CHECK(!result.Ok);
    CHECK(result.TcpFdConsumed);
    CHECK(g_PrecommitOwner->GetPhase() == TqStreamLifetime::Phase::TerminalPublished);
    CHECK(fcntl(fds[1], F_GETFD) == -1);
    g_PrecommitOwner.reset();
    g_PrecommitPublishTerminal = false;
    worker.Stop();
    ::close(fds[0]);
}

void ManagedNormalTerminalSignalsControlStop() {
    AdoptedManagedHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    auto control = harness.Handle.Control;
    CHECK(control != nullptr);
    CHECK(!control->Stop.load(std::memory_order_acquire));
    CHECK(!harness.Handle.Stop.load(std::memory_order_acquire));

    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
    CHECK(harness.Worker.DrainOneEventForTest());

    // P1-1: normal terminal must signal shared control so tunnel reaper can observe stop.
    CHECK(control->Stop.load(std::memory_order_acquire));
    harness.StopAndClosePeer();
}

struct PlacementReuseHandoffState {
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Entered{false};
    bool Released{false};
    TqDarwinRelayWorker* Worker{nullptr};
    uint64_t RelayIdA{0};
    std::shared_ptr<TqRelayStopControl> ControlA;

    void Reset() {
        Entered = false;
        Released = false;
        Worker = nullptr;
        RelayIdA = 0;
        ControlA.reset();
    }
};

PlacementReuseHandoffState g_PlacementReuse{};

void PlacementReuseBeforeHandoffHook(TqDarwinRelayWorker* worker, uint64_t relayId) {
    std::unique_lock<std::mutex> lock(g_PlacementReuse.Mutex);
    if (g_PlacementReuse.Worker != worker || g_PlacementReuse.RelayIdA != relayId) {
        return;
    }
    g_PlacementReuse.Entered = true;
    g_PlacementReuse.Cv.notify_all();
    g_PlacementReuse.Cv.wait(lock, [] { return g_PlacementReuse.Released; });
}

void ManagedHandleStorageReleasedAndReusedDuringFallback() {
    // P1-6: late queue-full fallback must not touch bare handle storage after reuse.
    TqDarwinRelayWorkerConfig config{};
    config.EventQueueCapacity = 2;
    config.BeforeTerminalHandoffHookForTest = PlacementReuseBeforeHandoffHook;
    TqDarwinRelayWorker worker(config);
    CHECK(worker.StartForTest());

    int fdsA[2]{TqInvalidSocket, TqInvalidSocket};
    int fdsB[2]{TqInvalidSocket, TqInvalidSocket};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fdsA) == 0);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fdsB) == 0);

    alignas(MsQuicStream) unsigned char streamStorageA[sizeof(MsQuicStream)]{};
    alignas(MsQuicStream) unsigned char streamStorageB[sizeof(MsQuicStream)]{};
    alignas(TqRelayHandle) unsigned char handleStorage[sizeof(TqRelayHandle)]{};

    auto* handleA = new (handleStorage) TqRelayHandle();
    auto ownerA = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);
    auto controlA = handleA->Control;

    TqDarwinRelayRegistration registrationA{};
    registrationA.TcpFd = fdsA[1];
    registrationA.Stream = reinterpret_cast<MsQuicStream*>(streamStorageA);
    registrationA.StreamOwner = ownerA;
    registrationA.Control = controlA;
    registrationA.ControlGeneration = controlA->Generation;
    const auto resultA = worker.RegisterRelayWithId(registrationA);
    CHECK(resultA.Ok);
    ManagedRelayHarness::PublishHandle(*handleA, worker, resultA, controlA);

    CHECK(worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(worker.EnqueueForTest(TestMarkerEvent(2)));
    CHECK(worker.PendingEventsForTest() == 2);

    g_PlacementReuse.Reset();
    g_PlacementReuse.Worker = &worker;
    g_PlacementReuse.RelayIdA = resultA.RelayId;
    g_PlacementReuse.ControlA = controlA;

    auto ownerB = TqStreamLifetime::CreateForTest(TqStreamLifetime::Phase::Started);

    std::atomic<bool> terminalReturned{false};
    std::atomic<bool> terminalOk{false};
    std::thread terminalThread([&] {
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        terminalOk.store(
            ownerA->DispatchForTest(&terminal) == QUIC_STATUS_SUCCESS,
            std::memory_order_release);
        terminalReturned.store(true, std::memory_order_release);
    });

    {
        std::unique_lock<std::mutex> lock(g_PlacementReuse.Mutex);
        CHECK(g_PlacementReuse.Cv.wait_for(lock, std::chrono::seconds(2), [] {
            return g_PlacementReuse.Entered;
        }));
    }

    // Destroy A and construct B in the same storage while fallback is paused.
    // Do not retain handleA owner after this point.
    handleA->~TqRelayHandle();
    auto* handleB = new (handleStorage) TqRelayHandle();
    auto controlB = handleB->Control;

    TqDarwinRelayRegistration registrationB{};
    registrationB.TcpFd = fdsB[1];
    registrationB.Stream = reinterpret_cast<MsQuicStream*>(streamStorageB);
    registrationB.StreamOwner = ownerB;
    registrationB.Control = controlB;
    registrationB.ControlGeneration = controlB->Generation;
    const auto resultB = worker.RegisterRelayWithId(registrationB);
    CHECK(resultB.Ok);
    ManagedRelayHarness::PublishHandle(*handleB, worker, resultB, controlB);

    const auto backendBefore = handleB->Backend;
    const auto* workerBefore = handleB->DarwinWorker;
    const auto relayIdBefore = handleB->DarwinRelayId;
    const bool stopBefore = handleB->Stop.load(std::memory_order_acquire);
    auto controlBBefore = handleB->Control;
    const auto generationBefore = handleB->ControlGeneration;

    {
        std::lock_guard<std::mutex> lock(g_PlacementReuse.Mutex);
        g_PlacementReuse.Released = true;
        g_PlacementReuse.Cv.notify_all();
    }
    terminalThread.join();
    CHECK(terminalReturned.load(std::memory_order_acquire));
    CHECK(terminalOk.load(std::memory_order_acquire));

    // Late fallback for A may only update control A.
    CHECK(controlA->Stop.load(std::memory_order_acquire));
    CHECK(handleB->Backend == backendBefore);
    CHECK(handleB->DarwinWorker == workerBefore);
    CHECK(handleB->DarwinRelayId == relayIdBefore);
    CHECK(handleB->Stop.load(std::memory_order_acquire) == stopBefore);
    CHECK(handleB->Control == controlBBefore);
    CHECK(handleB->ControlGeneration == generationBefore);
    CHECK(!controlBBefore->Stop.load(std::memory_order_acquire));
    CHECK(controlB == controlBBefore);

    CHECK(worker.DrainOneEventForTest());
    CHECK(worker.DrainOneEventForTest());
    QUIC_STREAM_EVENT terminalB{};
    terminalB.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(ownerB->DispatchForTest(&terminalB) == QUIC_STATUS_SUCCESS);
    CHECK(worker.DrainOneEventForTest());
    // Relay A remains after queue-full handoff (control signaled, map entry until stop).
    CHECK(worker.Snapshot().ActiveRelays == 1);

    handleB->~TqRelayHandle();
    worker.Stop();
    CHECK(worker.Snapshot().ActiveRelays == 0);
    ::close(fdsA[0]);
    ::close(fdsB[0]);
    ownerA.reset();
    ownerB.reset();
    g_PlacementReuse.Reset();
}

void SignalStopGenerationMismatchRecordsDiagnostic() {
    auto control = std::make_shared<TqRelayStopControl>();
    const uint64_t before = TqRelayControlGenerationMismatchCount().load(std::memory_order_relaxed);
    CHECK(!control->SignalStop(control->Generation + 1));
    CHECK(!control->Stop.load(std::memory_order_acquire));
    CHECK(TqRelayControlGenerationMismatchCount().load(std::memory_order_relaxed) == before + 1);
    CHECK(control->SignalStop(control->Generation));
    CHECK(control->Stop.load(std::memory_order_acquire));
    const uint64_t afterMatch =
        TqRelayControlGenerationMismatchCount().load(std::memory_order_relaxed);
    CHECK(afterMatch == before + 1);
}

enum class CompetitionStopPath {
    NormalTerminal,
    ExplicitStop,
    QueueFullTerminal,
    WorkerStop,
};

struct CompetitionCleanupSnapshot {
    bool ControlStopped{false};
    bool AccountingReleased{false};
    bool FdClosed{false};
    bool HandleCleared{false};
    uint64_t ActiveRelays{0};
};

CompetitionCleanupSnapshot RunCompetitionCleanup(CompetitionStopPath path) {
    CompetitionCleanupSnapshot out{};
    const uint32_t accountingBaseline = TqGetActiveRelayCount();
    (void)TqRelayRegisterActive();

    TqDarwinRelayWorkerConfig config{};
    if (path == CompetitionStopPath::QueueFullTerminal) {
        config.EventQueueCapacity = 2;
    }
    ManagedRelayHarness harness(config);
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    const int relayFd = harness.Fds[1];
    auto control = harness.Handle.Control;
    CHECK(control != nullptr);
    const uint64_t relayId = harness.Result.RelayId;
    const uint64_t generation = harness.Handle.ControlGeneration;

    switch (path) {
    case CompetitionStopPath::NormalTerminal: {
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
        CHECK(harness.Worker.DrainOneEventForTest());
        break;
    }
    case CompetitionStopPath::ExplicitStop:
        break;
    case CompetitionStopPath::QueueFullTerminal: {
        CHECK(harness.Worker.EnqueueForTest(TestMarkerEvent(1)));
        CHECK(harness.Worker.EnqueueForTest(TestMarkerEvent(2)));
        CHECK(harness.Worker.PendingEventsForTest() == 2);
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        CHECK(harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS);
        break;
    }
    case CompetitionStopPath::WorkerStop:
        harness.Worker.Stop();
        break;
    }

    auto stopControl = harness.Handle.Control;
    const uint64_t stopGeneration = harness.Handle.ControlGeneration != 0
        ? harness.Handle.ControlGeneration
        : generation;
    const uint64_t stopRelayId = harness.Handle.DarwinRelayId != 0
        ? harness.Handle.DarwinRelayId
        : relayId;
    TqDarwinRelayWorker* stopWorker = harness.Handle.DarwinWorker;
    harness.Handle.Backend = TqRelayBackendType::None;
    harness.Handle.DarwinWorker = nullptr;
    harness.Handle.DarwinRelayId = 0;
    harness.Handle.Control.reset();
    harness.Handle.ControlGeneration = 0;
    harness.Handle.Stop.store(true, std::memory_order_release);
    if (stopControl != nullptr) {
        (void)stopControl->SignalStop(stopGeneration);
    }
    if (path != CompetitionStopPath::WorkerStop && stopWorker != nullptr && stopRelayId != 0) {
        stopWorker->UnregisterRelay(stopRelayId);
    }
    CHECK(stopControl != nullptr);
    CHECK(stopControl->ReleaseActiveAccountingOnce());
    CHECK(!stopControl->ReleaseActiveAccountingOnce());

    out.ControlStopped = control->Stop.load(std::memory_order_acquire);
    out.AccountingReleased =
        control->ActiveAccountingReleased.load(std::memory_order_acquire) &&
        TqGetActiveRelayCount() == accountingBaseline;
    out.FdClosed = TcpRelayFdClosedOnce(relayFd);
    out.HandleCleared =
        harness.Handle.Backend == TqRelayBackendType::None &&
        harness.Handle.DarwinWorker == nullptr &&
        harness.Handle.DarwinRelayId == 0 &&
        harness.Handle.Control == nullptr;
    out.ActiveRelays = harness.Worker.Snapshot().ActiveRelays;

    if (path != CompetitionStopPath::WorkerStop) {
        while (harness.Worker.PendingEventsForTest() != 0) {
            if (!harness.Worker.DrainOneEventForTest()) {
                break;
            }
        }
        harness.StopAndClosePeer();
    } else {
        if (harness.Stream != nullptr) {
            harness.Stream->Callback = MsQuicStream::NoOpCallback;
            harness.Stream->Context = nullptr;
        }
        harness.Owner.reset();
        if (harness.Fds[0] != TqInvalidSocket) {
            ::close(harness.Fds[0]);
            harness.Fds[0] = TqInvalidSocket;
        }
    }
    CHECK(TqGetActiveRelayCount() == accountingBaseline);
    return out;
}

// Serial harness coverage for each stop path in isolation. Does not claim
// arbitrary concurrent interleaving — see CompetitionQueueFullHandoffRacesTqRelayStopOnce.
// Full reaper lost-terminal coverage lives in tcp_tunnel_test.
void CompetitionEachPathCleansExactlyOnceSerial() {
    const auto normal = RunCompetitionCleanup(CompetitionStopPath::NormalTerminal);
    CHECK(normal.ControlStopped);
    CHECK(normal.AccountingReleased);
    CHECK(normal.FdClosed);
    CHECK(normal.HandleCleared);
    CHECK(normal.ActiveRelays == 0);

    const auto explicitStop = RunCompetitionCleanup(CompetitionStopPath::ExplicitStop);
    CHECK(explicitStop.ControlStopped);
    CHECK(explicitStop.AccountingReleased);
    CHECK(explicitStop.FdClosed);
    CHECK(explicitStop.HandleCleared);
    CHECK(explicitStop.ActiveRelays == 0);

    const auto queueFull = RunCompetitionCleanup(CompetitionStopPath::QueueFullTerminal);
    CHECK(queueFull.ControlStopped);
    CHECK(queueFull.AccountingReleased);
    CHECK(queueFull.FdClosed);
    CHECK(queueFull.HandleCleared);
    CHECK(queueFull.ActiveRelays == 0);

    const auto workerStop = RunCompetitionCleanup(CompetitionStopPath::WorkerStop);
    CHECK(workerStop.ControlStopped);
    CHECK(workerStop.AccountingReleased);
    CHECK(workerStop.FdClosed);
    CHECK(workerStop.HandleCleared);
    CHECK(workerStop.ActiveRelays == 0);

    CHECK(normal.ControlStopped == queueFull.ControlStopped);
    CHECK(normal.AccountingReleased == queueFull.AccountingReleased);
    CHECK(normal.FdClosed == queueFull.FdClosed);
    CHECK(normal.HandleCleared == queueFull.HandleCleared);
    CHECK(normal.ActiveRelays == queueFull.ActiveRelays);
}

struct CompetitionHandoffRaceState {
    std::mutex Mutex;
    std::condition_variable Cv;
    bool Entered{false};
    bool Released{false};
    TqDarwinRelayWorker* Worker{nullptr};
    uint64_t RelayId{0};

    void Reset() {
        Entered = false;
        Released = false;
        Worker = nullptr;
        RelayId = 0;
    }
};

CompetitionHandoffRaceState g_CompetitionHandoffRace{};

void CompetitionHandoffRaceBeforeHook(TqDarwinRelayWorker* worker, uint64_t relayId) {
    std::unique_lock<std::mutex> lock(g_CompetitionHandoffRace.Mutex);
    if (g_CompetitionHandoffRace.Worker != worker || g_CompetitionHandoffRace.RelayId != relayId) {
        return;
    }
    g_CompetitionHandoffRace.Entered = true;
    g_CompetitionHandoffRace.Cv.notify_all();
    g_CompetitionHandoffRace.Cv.wait(lock, [] { return g_CompetitionHandoffRace.Released; });
}

// Barrier-driven race: queue-full terminal handoff (production callback path) vs
// TqRelayStop (production explicit stop). No sleep-based interleaving.
void CompetitionQueueFullHandoffRacesTqRelayStopOnce() {
    const uint32_t accountingBaseline = TqGetActiveRelayCount();
    (void)TqRelayRegisterActive();

    TqDarwinRelayWorkerConfig config{};
    config.EventQueueCapacity = 2;
    config.BeforeTerminalHandoffHookForTest = CompetitionHandoffRaceBeforeHook;
    ManagedRelayHarness harness(config);
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    const int relayFd = harness.Fds[1];
    auto control = harness.Handle.Control;
    CHECK(control != nullptr);

    CHECK(harness.Worker.EnqueueForTest(TestMarkerEvent(1)));
    CHECK(harness.Worker.EnqueueForTest(TestMarkerEvent(2)));
    CHECK(harness.Worker.PendingEventsForTest() == 2);

    g_CompetitionHandoffRace.Reset();
    g_CompetitionHandoffRace.Worker = &harness.Worker;
    g_CompetitionHandoffRace.RelayId = harness.Result.RelayId;

    std::atomic<bool> terminalReturned{false};
    std::atomic<bool> terminalOk{false};
    std::thread terminalThread([&] {
        QUIC_STREAM_EVENT terminal{};
        terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
        terminalOk.store(
            harness.DispatchViaRouter(terminal) == QUIC_STATUS_SUCCESS,
            std::memory_order_release);
        terminalReturned.store(true, std::memory_order_release);
    });

    {
        std::unique_lock<std::mutex> lock(g_CompetitionHandoffRace.Mutex);
        CHECK(g_CompetitionHandoffRace.Cv.wait_for(lock, std::chrono::seconds(2), [] {
            return g_CompetitionHandoffRace.Entered;
        }));
    }

    // Production explicit stop while terminal handoff is paused mid-callback.
    TqRelayStop(&harness.Handle);

    {
        std::lock_guard<std::mutex> lock(g_CompetitionHandoffRace.Mutex);
        g_CompetitionHandoffRace.Released = true;
        g_CompetitionHandoffRace.Cv.notify_all();
    }
    terminalThread.join();
    CHECK(terminalReturned.load(std::memory_order_acquire));
    CHECK(terminalOk.load(std::memory_order_acquire));

    CHECK(control->Stop.load(std::memory_order_acquire));
    CHECK(control->ActiveAccountingReleased.load(std::memory_order_acquire));
    CHECK(!control->ReleaseActiveAccountingOnce());
    CHECK(TqGetActiveRelayCount() == accountingBaseline);
    CHECK(TcpRelayFdClosedOnce(relayFd));
    CHECK(harness.Handle.Backend == TqRelayBackendType::None);
    CHECK(harness.Handle.DarwinWorker == nullptr);
    CHECK(harness.Handle.DarwinRelayId == 0);
    CHECK(harness.Handle.Control == nullptr);
    CHECK(harness.Handle.ControlGeneration == 0);
    CHECK(harness.Worker.Snapshot().ActiveRelays == 0);

    // Second unregister after once-cleanup must be a no-op (no double-close crash).
    harness.Worker.UnregisterRelay(harness.Result.RelayId);
    CHECK(TcpRelayFdClosedOnce(relayFd));
    CHECK(TqGetActiveRelayCount() == accountingBaseline);

    while (harness.Worker.PendingEventsForTest() != 0) {
        if (!harness.Worker.DrainOneEventForTest()) {
            break;
        }
    }
    harness.StopAndClosePeer();
    g_CompetitionHandoffRace.Reset();
}

void CompetitionLostTerminalStillCleansViaControlStop() {
    const uint32_t accountingBaseline = TqGetActiveRelayCount();
    (void)TqRelayRegisterActive();

    ManagedRelayHarness harness;
    CHECK(harness.OpenSocketPair());
    CHECK(harness.Register());
    const int relayFd = harness.Fds[1];
    auto control = harness.Handle.Control;
    CHECK(control != nullptr);
    const uint64_t relayId = harness.Result.RelayId;
    const uint64_t generation = harness.Handle.ControlGeneration;

    CHECK(control->SignalStop(generation));
    CHECK(control->Stop.load(std::memory_order_acquire));
    CHECK(harness.Worker.Snapshot().ActiveRelays == 1);
    CHECK(fcntl(relayFd, F_GETFD) >= 0);

    harness.Handle.Backend = TqRelayBackendType::None;
    harness.Handle.DarwinWorker = nullptr;
    harness.Handle.DarwinRelayId = 0;
    harness.Handle.Control.reset();
    harness.Handle.ControlGeneration = 0;
    harness.Handle.Stop.store(true, std::memory_order_release);
    harness.Worker.UnregisterRelay(relayId);
    CHECK(control->ReleaseActiveAccountingOnce());
    CHECK(!control->ReleaseActiveAccountingOnce());
    CHECK(TqGetActiveRelayCount() == accountingBaseline);
    CHECK(TcpRelayFdClosedOnce(relayFd));
    CHECK(harness.Handle.Backend == TqRelayBackendType::None);
    CHECK(harness.Worker.Snapshot().ActiveRelays == 0);

    harness.StopAndClosePeer();
}

void ManagedEmergencyAcceptedRejectClosesOnce() {
    const MsQuicApi* previous = MsQuic;
    QUIC_API_TABLE fakeApi{};
    fakeApi.SetCallbackHandler = FakeSetCallbackHandler;
    fakeApi.StreamClose = FakeStreamClose;
    fakeApi.StreamShutdown = FakeStreamShutdown;
    MsQuic = reinterpret_cast<const MsQuicApi*>(&fakeApi);
    g_fakeHandler = nullptr;
    g_fakeHandlerContext = nullptr;
    g_fakeCloseCount = 0;
    g_fakeShutdownCount = 0;

    const HQUIC rejected = reinterpret_cast<HQUIC>(static_cast<uintptr_t>(0x6000));
    TqStreamLifetime::RejectAccepted(rejected);
    CHECK(g_fakeShutdownCount == 1);
    QUIC_STREAM_EVENT terminal{};
    terminal.Type = QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE;
    CHECK(DispatchFakeAdapter(rejected, terminal));
    CHECK(g_fakeCloseCount == 1);
    MsQuic = previous;
}

#pragma clang diagnostic pop

} // namespace

int main() {
    SocketPairEnvironmentWorks();
    WorkerStartsAndStopsCleanly();
    ControlEventRetriesAfterWakeFailure();
    UnregisterWakesAfterFullQueueWakeFailures();
    SnapshotRetriesAfterFullQueue();
#if defined(TQ_UNIT_TESTING)
    SnapshotDeadlineReturnsIncompleteWhenDispatchBlocked();
    ConsecutiveSnapshotTimeoutsDoNotDangleCommands();
    SnapshotLocalThrowKeepsWorkerAliveAndReturnsIncomplete();
    StopPurgesDetachedSnapshotAndReleasesPermit();
    DarwinRuntimeSnapshotWorkersIdentityAndCapacity();
    DarwinRuntimeTqRelayStartSetsWorkerIndexIdentity();
    DarwinRuntimeSnapshotTimeoutHoldsPermitAndBlocksSecondRequest();
    DarwinRuntimeStopWaitsForSnapshotLease();
    DarwinRuntimeStartFailureRollsBackAndRetryUsesFullRange();
    DarwinRuntimePickWorkerNotBlockedDuringSnapshot();
#endif
    UnregisterQueuesDuringStartupWindow();
    WorkerRegistersTcpReadinessShell();
    EventizedUnregisterClearsHandleAndRelayCount();
    WorkerObservesTcpReadBytes();
    WorkerObservesTcpReadWithSmallByteBudget();
    CompressedTcpReadFlushesWhenCompressorBuffersInput();
    TransientSendFailureQueuesWithoutSelfRetry();
    SendOperationStateTransitionsAreSingleClaim();
    SendCompleteAfterUnregisterReleasesOperation();
    SendCompleteCallbackQueuesUntilWorkerDrain();
    ActiveWorkerSendCompleteDoesNotPurgeRetiredRelays();
    ActiveWorkerSendCompleteDoesNotUseKnownSendLocks();
    AsyncSendCompleteCallbackDoesNotLockCompletionState();
    SendCompleteEnqueueFailureSettlesWithoutBlocking();
    SendCompleteAfterRunningFalseSettlesOnCallback();
    SendCompleteFallsBackToBindingRelayWhenMapLookupMisses();
    SynchronousSendCompleteBeforeFailureDoesNotDoubleRelease();
    SynchronousSendCompleteBeforeSuccessDoesNotLeak();
    SynchronousSendCompleteDoesNotDoubleComplete();
    MagicMismatchKnownOperationCleansAccounting();
    StopThenLateCompletionDoesNotUseDanglingWorker();
    StopThenLateTcpEventIgnoresRetiredRelay();
    StreamShutdownStopsRelayAndPreventsFurtherTcpToQuicSends();
    StreamShutdownDuringTcpBatchBlocksQuicSend();
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
    ReceiveCallbackQueueFullReturnsPendingWithoutCompleting();
    ReceiveCallbackQueueFullHoldsBudgetUntilFlush();
    ReceiveCallbackQueueFullBackpressureRetriesAfterDrain();
    ReceiveCallbackBudgetRejectPausesAndCompletes();
    QuicReceiveCallbackReturnsPending();
    QuicReceiveCallbackDefersPendingBytesUntilWorkerEvent();
    QuicReceiveCallbackQueuedEventCompletesOnStop();
    QuicReceiveCallbackAcceptsOneOversizedReceiveBeforeWorkerEvent();
    QuicReceiveCallbackRejectsOversizedReceiveAfterPendingFinOnlyEvent();
    QuicReceiveZeroBufferFinCompletesAfterDrain();
    QuicReceiveZeroFinBudgetRejectReturnsSuccessWithoutCompletion();
    QuicReceivePartialTcpWriteCompletesOnceAfterFullFlush();
    QuicReceiveFlushHoldsBuffersAcrossConcurrentUnregister();
    QuicReceivePartialWriteThenErrorCompletesFullReceiveOnce();
    QuicReceivePauseAndResumeFollowsTcpWritePressure();
    QuicReceivePauseFailureRollsBackAndCompletesReceive();
    MissingRelayQuicReceiveDiscardsWithoutStreamApi();
    CompressedQuicReceiveDecompressesToTcpAndCompletesCompressedBytes();
    QuicReceiveSnapshotAggregatesPendingTcpWriteMetrics();
    CompressedQuicReceiveFailsClosedWithoutWritingCorruptData();
    CallbackReceiveDoesNotUseLockedRelayLookup();
    CallbackShutdownDoesNotUseLockedRelayLookup();
    QuicShutdownCallbackClosesViaWorkerEventWithoutLockedLookup();
    QuicShutdownCallbackClosesOnEnqueueFailureWithoutLockedLookup();
    StopPurgesQueuedShutdownCloseWithoutLockedLookup();
    RegisteredBindingSurvivesCallbackWithoutMapLookupRequirement();
    SnapshotDuringConcurrentUnregisterRemainsBestEffort();
    PreparedReceiveBufferedBeforeCommit();
    PreparedReceivePrecommitDiscardIncrementsDeferredComplete();
    TerminalOwnerSkipsReceiveCompleteOnPrecommitDiscard();
    PublishWindowReceiveSeesInitializedIdentity();
    PublishWindowTerminalRollsBackWithoutPublicHandle();
    PublishWindowPeerAbortSeesLockableIdentity();
    CommitBarrierTerminalBeforeFinalCheckRollsBack();
    CommitBarrierTerminalAfterActivationSucceedsThenStops();
    ManagedBindingWeakPointersExpireWithoutOwnershipCycle();
    ManagedStaleSendCompletionEnvelopeDoesNotClaimTwice();
    LateReceiveAfterInactiveBindingUsesFailSafeSink();
    LeaseHeldDeferredReceiveDoesNotDiscard();
    ManagedRouterDispatchesShutdownComplete();
    ManagedTerminalCallbackReturnsBeforeEventDrained();
    ManagedTunnelOwnerReleaseRetainsBinding();
    ManagedCloseRetirePurgePreservesStreamCallback();
    ManagedPeerAbortUsesActiveShutdownWithoutTerminal();
    ManagedPeerAbortBeforeTerminalOnlyTerminalDetaches();
    ManagedTerminalBeforePeerAbortIgnoresActiveShutdown();
    ManagedDuplicateTerminalEventIsIdempotent();
    ManagedQueuedPeerAbortBeforeTerminalPreservesOwnerUntilTerminal();
    ManagedNonTerminalHalfCloseEventsDoNotPublishTerminal();
    FullyClosedStopAfterEofAndPeerFinWithoutShutdownComplete();
    FullyClosedStopPeerFinBeforeTcpEof();
    FullyClosedCallbackPendingBlocksStopUntilDrained();
    FullyClosedDuplicatePeerFinStopIdempotent();
    FullyClosedCanceledFinDoesNotCompleteOrStop();
    FullyClosedPeerSendShutdownStickySurvivesQueueFull();
    FullyClosedOffWorkerFinSetsConvergenceStickyThenStops();
    ManagedStopRetentionMetricsSurfaceTerminalRetainedOwners();
    ManagedStopPurgeProcessesPeerAbortBeforeTerminal();
    ManagedPeerSendAbortedRequestsAbortReceiveFirst();
    ManagedPeerReceiveAbortedRequestsAbortSendFirst();
    RealWorkerExitPurgeSettlesMixedEventsWithoutAssert();
    ManagedTerminalWithTcpErrorClosesOnce();
    ManagedTerminalBeforeTcpErrorClosesOnce();
    ManagedTcpErrorBeforeTerminalClosesOnce();
    ManagedTerminalBeforeTcpWriteFailureClosesOnce();
    ManagedTcpWriteFailureBeforeTerminalClosesOnce();
    ManagedInFlightSendCompletesAfterRetireWithoutRelayStream();
    ManagedTerminalRejectsRoutePublishAndApiLease();
    ManagedDirectRouterCallbackManualCleanupOnce();
    ManagedHandoffConcurrentTerminalHandledOnce();
    ManagedLeaseHeldAcrossTerminalReleasesOnce();
    ManagedStartCompleteSuccessBeforeStartReturns();
    ManagedStartFailureRejectsPublishAndLease();
    ManagedSendStartFailureRetainedUntilSendCompleteCanceled();
    ManagedShutdownDuplicateGracefulUpgradesAbort();
    ManagedShutdownTerminalWinsOverDesired();
    ManagedShutdownApiFailureRetainsDesiredIntent();
    ManagedSyncNoCallbackStartRejectRejectsPublishAndLease();
    ManagedWorkerStopAfterTerminalQueuesCleanup();
    ManagedLateSendCompletionAfterTargetSwitch();
    ManagedPrepareFailureLeavesFdWithCaller();
    ManagedCommitFailureClosesFdOnceWithoutReuseHit();
    ManagedRegisterRejectsTerminalOwnerBeforePublish();
    ManagedRegisterStartFailedPublishRejectsAndTerminalClosesOnce();
    ManagedRegisterBindingAllocFailureRejectsAndTerminalClosesOnce();
    ManagedReceiveAllocationFailureUsesActiveShutdownNotTerminal();
    ManagedReceiveAllocationFailureQueueFullKeepsOwnerViaSink();
    ManagedSendRegisterThenTerminalBeforeActiveRecheckRollsBack();
    ManagedSendRegisterExitPointsRollBackRegistry();
    ManagedPublishHookTerminalRejectsCommitAfterTask3();
    ManagedNormalTerminalSignalsControlStop();
    ManagedHandleStorageReleasedAndReusedDuringFallback();
    ManagedLateKqueueEventAfterFdReuseMissesNewRelay();
    ManagedEmergencyAcceptedRejectClosesOnce();
    SignalStopGenerationMismatchRecordsDiagnostic();
    CompetitionEachPathCleansExactlyOnceSerial();
    CompetitionQueueFullHandoffRacesTqRelayStopOnce();
    CompetitionLostTerminalStillCleansViaControlStop();
    // Avoid libc++ static-destructor races on MsQuic/stream-owner globals after a
    // full suite run (mutex lock failed: Invalid argument). Test body already passed.
    std::_Exit(0);
}

#endif
