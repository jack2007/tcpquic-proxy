#include "linux_relay_worker.h"

#include <msquic.hpp>
#include "quic_receive_guard.h"
#include "trace.h"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <climits>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <new>
#include <thread>

#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#if defined(__GNUC__)
__attribute__((weak)) const MsQuicApi* MsQuic = nullptr;
extern "C" void TqTraceLinuxRelayStreamShutdownEvent(
    uint32_t workerIndex,
    uint64_t relayId,
    uint64_t outstandingQuicSends,
    uint64_t outstandingQuicSendBytes,
    uint64_t pendingTcpWriteQueue,
    uint64_t pendingTcpWriteBytes,
    uint64_t pendingQuicReceiveBytes,
    uint64_t tcpReadBytes,
    uint64_t tcpWriteBytes,
    uint64_t tcpWriteErrno,
    bool tcpReadClosed,
    bool tcpWriteClosed,
    bool quicSendFinSubmitted,
    bool quicSendFinCompleted,
    bool tcpWriteShutdownQueued,
    bool streamDetached) __attribute__((weak));
extern "C" void TqTraceLinuxRelayUnregisterEvent(
    uint32_t workerIndex,
    uint64_t relayId,
    uint64_t outstandingQuicSends,
    uint64_t outstandingQuicSendBytes,
    uint64_t pendingTcpWriteQueue,
    uint64_t pendingTcpWriteBytes,
    uint64_t pendingQuicReceiveBytes,
    uint64_t tcpReadBytes,
    uint64_t tcpWriteBytes,
    uint64_t tcpWriteErrno,
    bool tcpReadClosed,
    bool tcpWriteClosed,
    bool quicSendFinSubmitted,
    bool quicSendFinCompleted,
    bool tcpWriteShutdownQueued,
    bool streamDetached) __attribute__((weak));
extern "C" void TqTraceLinuxRelayStopConditionEvent(
    uint32_t workerIndex,
    uint64_t relayId,
    const char* trigger,
    uint64_t outstandingQuicSends,
    uint64_t outstandingQuicSendBytes,
    uint64_t pendingTcpWriteQueue,
    uint64_t pendingTcpWriteBytes,
    uint64_t pendingQuicReceiveBytes,
    uint64_t tcpReadBytes,
    uint64_t tcpWriteBytes,
    uint64_t tcpWriteErrno,
    bool tcpReadClosed,
    bool tcpWriteClosed,
    bool quicSendFinSubmitted,
    bool quicSendFinCompleted,
    bool tcpWriteShutdownQueued,
    bool streamDetached) __attribute__((weak));
extern "C" void TqTraceLinuxRelayStreamEvent(
    uint32_t workerIndex,
    uint64_t relayId,
    const char* streamEvent,
    uint64_t errorCode,
    uint32_t status,
    uint64_t absoluteOffset,
    uint64_t totalBufferLength,
    uint32_t bufferCount,
    uint32_t receiveFlags,
    bool fin,
    uint64_t outstandingQuicSends,
    uint64_t outstandingQuicSendBytes,
    uint64_t pendingTcpWriteQueue,
    uint64_t pendingTcpWriteBytes,
    uint64_t pendingQuicReceiveBytes,
    uint64_t tcpReadBytes,
    uint64_t tcpWriteBytes,
    uint64_t tcpWriteErrno,
    bool tcpReadClosed,
    bool tcpWriteClosed,
    bool quicSendFinSubmitted,
    bool quicSendFinCompleted,
    bool tcpWriteShutdownQueued,
    bool streamDetached) __attribute__((weak));
extern "C" void TqTraceLinuxRelayBackpressureEvent(
    uint32_t workerIndex,
    uint64_t relayId,
    const char* action,
    const char* reason,
    uint64_t outstandingQuicSendBytes,
    uint64_t pauseThreshold,
    uint64_t resumeThreshold,
    uint64_t readAheadBytes) __attribute__((weak));
extern "C" void TqTraceLinuxRelayIdealSendBufferEvent(
    uint32_t workerIndex,
    uint64_t relayId,
    uint64_t idealSendBytes,
    uint64_t outstandingQuicSendBytes) __attribute__((weak));
#endif

namespace {

#if defined(TQ_UNIT_TESTING)
TqLinuxRelayStreamSendForTest g_linuxRelayStreamSendForTest = nullptr;
#endif

constexpr uint32_t kLinuxRelayControlEnqueueRetries = 8;

bool TqRelayDebugEnabled() {
    static const bool enabled = []() {
        const char* value = std::getenv("TQ_TUNNEL_DEBUG");
        return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

void TqRelayDebugLog(const char* fmt, ...) {
    if (!TqRelayDebugEnabled() && !TqTraceEnabled()) {
        return;
    }
    char buffer[1400];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (TqTraceEnabled()) {
        TqTraceLogLine(buffer);
    } else {
        std::fprintf(stderr, "tcpquic-proxy relay-debug: %s\n", buffer);
    }
}

QUIC_STATUS TqLinuxRelayStreamSend(
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    QUIC_SEND_FLAGS flags,
    void* context) {
#if defined(TQ_UNIT_TESTING)
    if (g_linuxRelayStreamSendForTest != nullptr) {
        return g_linuxRelayStreamSendForTest(stream, buffers, bufferCount, flags, context);
    }
#endif
    return stream->Send(buffers, bufferCount, flags, context);
}

template <typename Command>
bool WaitForCommandDone(
    Command& command,
    uint32_t timeoutMs,
    std::atomic<uint64_t>& waitNanos,
    std::atomic<uint64_t>& waitCount,
    std::atomic<uint64_t>& timeouts) {
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeoutMs);
    std::unique_lock<std::mutex> lock(command.Mutex);
    const bool done = command.Cv.wait_until(lock, deadline, [&command]() {
        return command.Done;
    });
    const auto elapsed = std::chrono::steady_clock::now() - start;
    command.WaitNanos = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    waitNanos.fetch_add(command.WaitNanos, std::memory_order_relaxed);
    waitCount.fetch_add(1, std::memory_order_relaxed);
    if (!done) {
        command.Cancelled = true;
        timeouts.fetch_add(1, std::memory_order_relaxed);
    }
    return done;
}

ssize_t WritevNoSignal(int fd, const iovec* iov, int iovcnt) {
    msghdr message{};
    message.msg_iov = const_cast<iovec*>(iov);
    message.msg_iovlen = static_cast<size_t>(iovcnt);
    return ::sendmsg(fd, &message, MSG_NOSIGNAL);
}

void AbandonOrphanedEventBuffers(TqLinuxRelayEvent& event) {
    event.Buffer.abandon();
    for (auto& buffer : event.Buffers) {
        buffer.abandon();
    }
    event.Buffers.clear();
}

void UpdateAtomicMax(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t previous = target.load(std::memory_order_relaxed);
    while (previous < value &&
           !target.compare_exchange_weak(
               previous, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

uint64_t PendingTcpWriteBytes(const std::deque<TqBufferView>& writes) {
    uint64_t bytes = 0;
    for (const auto& view : writes) {
        bytes += view.Len;
    }
    return bytes;
}

void ConfigureRelayTcpLiveness(
    int fd,
    uint32_t keepIdleSec,
    uint32_t keepIntervalSec,
    uint32_t keepCount,
    uint32_t userTimeoutMs) {
    if (fd < 0) {
        return;
    }
    int enabled = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
    if (keepIdleSec != 0) {
        int value = static_cast<int>(keepIdleSec);
        (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &value, sizeof(value));
    }
    if (keepIntervalSec != 0) {
        int value = static_cast<int>(keepIntervalSec);
        (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &value, sizeof(value));
    }
    if (keepCount != 0) {
        int value = static_cast<int>(keepCount);
        (void)::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &value, sizeof(value));
    }
    if (userTimeoutMs != 0) {
        int value = static_cast<int>(userTimeoutMs);
        (void)::setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &value, sizeof(value));
    }
}

int GetSocketError(int fd) {
    if (fd < 0) {
        return EBADF;
    }
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
        return errno != 0 ? errno : EIO;
    }
    return error;
}

void LogLinuxRelayError(
    uint32_t workerIndex,
    const char* reason,
    uint64_t relayId,
    int tcpFd,
    int64_t status,
    uint64_t err,
    uint64_t pendingTcpWriteBytes,
    uint64_t pendingQuicReceiveBytes,
    uint64_t outstandingQuicSends,
    uint64_t outstandingQuicSendBytes) {
    TqTraceRelayFatalError(
        "linux",
        workerIndex,
        reason,
        relayId,
        static_cast<uint64_t>(tcpFd),
        pendingQuicReceiveBytes,
        0,
        outstandingQuicSends,
        outstandingQuicSends,
        0);
    (void)status;
    (void)err;
    (void)pendingTcpWriteBytes;
    (void)outstandingQuicSendBytes;
}

std::string SocketAddressToString(const sockaddr_storage& storage, socklen_t length) {
    char host[INET6_ADDRSTRLEN]{};
    uint16_t port = 0;
    if (storage.ss_family == AF_INET && length >= sizeof(sockaddr_in)) {
        const auto* addr = reinterpret_cast<const sockaddr_in*>(&storage);
        if (::inet_ntop(AF_INET, &addr->sin_addr, host, sizeof(host)) == nullptr) {
            return "inet:unknown";
        }
        port = ntohs(addr->sin_port);
    } else if (storage.ss_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
        const auto* addr = reinterpret_cast<const sockaddr_in6*>(&storage);
        if (::inet_ntop(AF_INET6, &addr->sin6_addr, host, sizeof(host)) == nullptr) {
            return "inet6:unknown";
        }
        port = ntohs(addr->sin6_port);
    } else if (storage.ss_family == AF_UNIX) {
        return "unix";
    } else {
        return "family:" + std::to_string(storage.ss_family);
    }
    return std::string(host) + ":" + std::to_string(port);
}

std::string GetSocketNameString(int fd, bool peer, int* errNo = nullptr) {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    const int rc = peer
        ? ::getpeername(fd, reinterpret_cast<sockaddr*>(&storage), &length)
        : ::getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &length);
    if (rc != 0) {
        if (errNo != nullptr) {
            *errNo = errno;
        }
        return peer ? "peer:unknown" : "local:unknown";
    }
    if (errNo != nullptr) {
        *errNo = 0;
    }
    return SocketAddressToString(storage, length);
}

}  // namespace

#if defined(TQ_UNIT_TESTING)
void TqLinuxRelaySetStreamSendForTest(TqLinuxRelayStreamSendForTest sendFn) {
    g_linuxRelayStreamSendForTest = sendFn;
}
#endif

struct TqLinuxRelayWorker::StreamRelayBinding {
    TqLinuxRelayWorker* Worker{nullptr};
    std::atomic<RelayState*> Relay{nullptr};
    std::atomic<TqRelayHandle*> Handle{nullptr};
    std::atomic<uint32_t> CallbackRefs{0};
    std::atomic<bool> Closing{false};
};

struct TqLinuxRelayWorker::RelayState : TqRelayBufferBudget {
    uint64_t Id{0};
    int TcpFd{-1};
    MsQuicStream* Stream{nullptr};
    TqRelayHandle* Handle{nullptr};
    ITqCompressor* Compressor{nullptr};
    ITqDecompressor* Decompressor{nullptr};
    TqCompressAlgo CompressAlgo{TqCompressAlgo::None};
    std::vector<uint8_t> CompressionOutput;
    std::vector<uint8_t> DecompressionOutput;
    std::vector<uint8_t> CapturedQuicBytesForTest;
    bool EnableQuicSends{true};
    bool SinkQuicReceives{false};
    std::atomic<uint64_t>* SinkQuicReceiveBytes{nullptr};
    bool Closing{false};
    bool TcpReadClosed{false};
    bool TcpWriteClosed{false};
    bool QuicSendFinSubmitted{false};
    bool QuicSendFinCompleted{false};
    uint64_t OutstandingQuicSends{0};
    uint64_t OutstandingQuicSendBytes{0};
    uint64_t IdealSendBufferBytes{TqValidationInitialIdealSendFallbackBytes};
    bool HasIdealSendBufferEvent{false};
    std::deque<std::unique_ptr<TqLinuxRelaySendOperation>> PendingQuicSendRetries;
    uint64_t TcpReadBytes{0};
    std::deque<TqBufferView> PendingTcpWrites;
    std::deque<std::shared_ptr<TqPendingQuicReceive>> PendingQuicReceives;
    std::mutex CallbackPendingQuicReceiveLock;
    std::deque<std::shared_ptr<TqPendingQuicReceive>> CallbackPendingQuicReceives;
    uint64_t PendingQuicReceiveBytes{0};
    bool QuicReceivePaused{false};
    bool TcpWriteShutdownQueued{false};
    bool TcpReadArmed{true};
    bool TcpWriteArmed{false};
    bool TcpReadPausedByQuicBacklog{false};
    uint64_t TcpWriteBytes{0};
    uint64_t LastTcpWriteErrno{0};
    uint64_t TcpWriteEagainCount{0};
    uint64_t EpollOutEvents{0};
    StreamRelayBinding* StreamBinding{nullptr};

    RelayState(const TqLinuxRelayRegistration& registration, const TqLinuxRelayWorkerConfig& config)
        : TcpFd(registration.TcpFd),
          Stream(registration.Stream),
          Handle(registration.Handle),
          Compressor(registration.Compressor),
          Decompressor(registration.Decompressor),
          CompressAlgo(registration.CompressAlgo),
          EnableQuicSends(registration.EnableQuicSends),
          SinkQuicReceives(registration.SinkQuicReceives),
          SinkQuicReceiveBytes(registration.SinkQuicReceiveBytes) {
        MaxPendingBufferBytes = config.MaxPendingBufferBytes;
        IdealSendBufferBytes = config.MaxBufferedQuicSendBytes != 0
            ? config.MaxBufferedQuicSendBytes
            : TqValidationInitialIdealSendFallbackBytes;
    }
};

TqLinuxRelayWorker::TqLinuxRelayWorker(const TqLinuxRelayWorkerConfig& config)
    : Config(config),
      EventQueue(config.EventQueueCapacity) {}

TqLinuxRelayWorker::~TqLinuxRelayWorker() {
    Stop();
}

bool TqLinuxRelayWorker::Start() {
    auto controlGuard = AcquireControlLockForMetrics();
    if (Running.load()) {
        return false;
    }
    WakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (WakeFd < 0) {
        return false;
    }
    EpollFd = ::epoll_create1(EPOLL_CLOEXEC);
    if (EpollFd < 0) {
        ::close(WakeFd);
        WakeFd = -1;
        return false;
    }
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = WakeFd;
    if (::epoll_ctl(EpollFd, EPOLL_CTL_ADD, WakeFd, &event) != 0) {
        ::close(EpollFd);
        ::close(WakeFd);
        EpollFd = -1;
        WakeFd = -1;
        return false;
    }
    Running.store(true);
    Thread = std::thread(&TqLinuxRelayWorker::Run, this);
    WorkerThreadId = Thread.get_id();
    return true;
}

bool TqLinuxRelayWorker::StartForTest() {
    auto controlGuard = AcquireControlLockForMetrics();
    if (Running.exchange(true)) {
        return false;
    }
    WakeFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (WakeFd < 0) {
        Running.store(false);
        return false;
    }
    EpollFd = ::epoll_create1(EPOLL_CLOEXEC);
    if (EpollFd < 0) {
        ::close(WakeFd);
        WakeFd = -1;
        Running.store(false);
        return false;
    }
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = WakeFd;
    if (::epoll_ctl(EpollFd, EPOLL_CTL_ADD, WakeFd, &event) != 0) {
        ::close(EpollFd);
        ::close(WakeFd);
        EpollFd = -1;
        WakeFd = -1;
        Running.store(false);
        return false;
    }
    WorkerThreadId = std::this_thread::get_id();
    return true;
}

void TqLinuxRelayWorker::Stop() {
    auto controlGuard = AcquireControlLockForMetrics();
    if (!Running.exchange(false)) {
        return;
    }
    Wake();
    if (Thread.joinable()) {
        Thread.join();
    }
    if (EpollFd >= 0) {
        ::close(EpollFd);
        EpollFd = -1;
    }
    if (WakeFd >= 0) {
        ::close(WakeFd);
        WakeFd = -1;
    }
    WorkerThreadId = std::thread::id{};
}

bool TqLinuxRelayWorker::Enqueue(TqLinuxRelayEvent event) {
    RecordEventProducer();
    if (!EventQueue.TryPush(std::move(event))) {
        RecordError(RelayErrorKind::EventQueueFull);
        return false;
    }
    Wake();
    return true;
}

void TqLinuxRelayWorker::RecordError(RelayErrorKind kind) {
    Errors.fetch_add(1);
    switch (kind) {
    case RelayErrorKind::EventQueueFull:
        EventQueueFullErrors.fetch_add(1);
        break;
    case RelayErrorKind::TcpReadBufferAcquire:
        TcpReadBufferAcquireFailures.fetch_add(1);
        break;
    case RelayErrorKind::TcpToQuicCompress:
        TcpToQuicCompressFailures.fetch_add(1);
        break;
    case RelayErrorKind::TcpToQuicBufferAcquire:
        TcpToQuicBufferAcquireFailures.fetch_add(1);
        break;
    case RelayErrorKind::QuicSend:
        QuicSendFailures.fetch_add(1);
        break;
    case RelayErrorKind::QuicReceiveView:
        QuicReceiveViewFailures.fetch_add(1);
        break;
    case RelayErrorKind::QuicReceiveDecompress:
        QuicReceiveDecompressFailures.fetch_add(1);
        break;
    case RelayErrorKind::QuicReceiveTcpBufferAcquire:
        QuicReceiveTcpBufferAcquireFailures.fetch_add(1);
        break;
    case RelayErrorKind::TcpWriteHard:
        TcpWriteHardErrors.fetch_add(1);
        break;
    case RelayErrorKind::TcpReadHard:
        TcpReadHardErrors.fetch_add(1);
        break;
    }
}

void TqLinuxRelayWorker::RecordBufferAcquireFailure(
    RelayErrorKind kind,
    TqBufferAcquireFailure failure) {
    RecordError(kind);

    auto recordByReason = [failure](
        std::atomic<uint64_t>& pendingBudget,
        std::atomic<uint64_t>& alloc) {
        switch (failure) {
        case TqBufferAcquireFailure::PendingBytesLimit:
            pendingBudget.fetch_add(1);
            break;
        case TqBufferAcquireFailure::AllocationFailure:
            alloc.fetch_add(1);
            break;
        case TqBufferAcquireFailure::None:
            break;
        }
    };

    switch (kind) {
    case RelayErrorKind::TcpReadBufferAcquire:
        recordByReason(
            TcpReadBufferAcquirePendingBudgetFailures,
            TcpReadBufferAcquireAllocFailures);
        break;
    case RelayErrorKind::TcpToQuicBufferAcquire:
        recordByReason(
            TcpToQuicBufferAcquirePendingBudgetFailures,
            TcpToQuicBufferAcquireAllocFailures);
        break;
    case RelayErrorKind::QuicReceiveTcpBufferAcquire:
        recordByReason(
            QuicReceiveTcpBufferAcquirePendingBudgetFailures,
            QuicReceiveTcpBufferAcquireAllocFailures);
        break;
    default:
        break;
    }
}

void TqLinuxRelayWorker::RecordTcpWriteAttempt(uint64_t bytes) {
    if (bytes == 0) {
        return;
    }
    TcpWriteAttemptBytes.fetch_add(bytes);
    UpdateAtomicMax(MaxTcpWriteAttemptBytes, bytes);
    if (bytes <= 64ull * 1024) {
        TcpWriteAttemptBytesLe64K.fetch_add(1);
    } else if (bytes <= 256ull * 1024) {
        TcpWriteAttemptBytesLe256K.fetch_add(1);
    } else if (bytes <= 1024ull * 1024) {
        TcpWriteAttemptBytesLe1M.fetch_add(1);
    } else if (bytes <= 4ull * 1024 * 1024) {
        TcpWriteAttemptBytesLe4M.fetch_add(1);
    } else {
        TcpWriteAttemptBytesGt4M.fetch_add(1);
    }
}

void TqLinuxRelayWorker::RecordTcpWriteReturned(uint64_t bytes) {
    if (bytes == 0) {
        return;
    }
    if (bytes <= 64ull * 1024) {
        TcpWriteReturnedBytesLe64K.fetch_add(1);
    } else if (bytes <= 256ull * 1024) {
        TcpWriteReturnedBytesLe256K.fetch_add(1);
    } else if (bytes <= 1024ull * 1024) {
        TcpWriteReturnedBytesLe1M.fetch_add(1);
    } else if (bytes <= 4ull * 1024 * 1024) {
        TcpWriteReturnedBytesLe4M.fetch_add(1);
    } else {
        TcpWriteReturnedBytesGt4M.fetch_add(1);
    }
}

void TqLinuxRelayWorker::RecordQuicReceiveView(uint64_t bytes, uint64_t slices) {
    QuicReceiveViewCount.fetch_add(1);
    QuicReceiveViewBytes.fetch_add(bytes);
    UpdateAtomicMax(MaxQuicReceiveViewBytes, bytes);
    UpdateAtomicMax(MaxQuicReceiveViewSlices, slices);
    if (bytes <= 64ull * 1024) {
        QuicReceiveViewBytesLe64K.fetch_add(1);
    } else if (bytes <= 256ull * 1024) {
        QuicReceiveViewBytesLe256K.fetch_add(1);
    } else if (bytes <= 1024ull * 1024) {
        QuicReceiveViewBytesLe1M.fetch_add(1);
    } else if (bytes <= 4ull * 1024 * 1024) {
        QuicReceiveViewBytesLe4M.fetch_add(1);
    } else {
        QuicReceiveViewBytesGt4M.fetch_add(1);
    }

    if (slices <= 1) {
        QuicReceiveViewSlices1.fetch_add(1);
    } else if (slices <= 4) {
        QuicReceiveViewSlices2To4.fetch_add(1);
    } else if (slices <= 16) {
        QuicReceiveViewSlices5To16.fetch_add(1);
    } else {
        QuicReceiveViewSlicesGt16.fetch_add(1);
    }
}

bool TqLinuxRelayWorker::EnqueueForTest(TqLinuxRelayEvent event) {
    return Enqueue(std::move(event));
}

void TqLinuxRelayWorker::RecordEventProducer() {
    if (!Config.TrackEventProducers) {
        return;
    }
    size_t hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    if (hash == 0) {
        hash = 1;
    }
    size_t expected = 0;
    if (FirstEventProducerHash.compare_exchange_strong(
            expected, hash, std::memory_order_acq_rel, std::memory_order_acquire)) {
        uint64_t count = EventProducerThreadCount.load(std::memory_order_acquire);
        while (count < 1 && !EventProducerThreadCount.compare_exchange_weak(
                                count, 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
        return;
    }
    if (expected != hash && FirstEventProducerHash.load(std::memory_order_acquire) != hash) {
        MultipleEventProducerThreadsObserved.store(true, std::memory_order_release);
        uint64_t count = EventProducerThreadCount.load(std::memory_order_acquire);
        while (count < 2 && !EventProducerThreadCount.compare_exchange_weak(
                               count, 2, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
    }
}

void TqLinuxRelayWorker::TraceRelayStreamEvent(
    RelayState* relay,
    const char* streamEvent,
    uint64_t errorCode,
    uint32_t status,
    uint64_t absoluteOffset,
    uint64_t totalBufferLength,
    uint32_t bufferCount,
    uint32_t receiveFlags,
    bool fin) {
#if defined(__GNUC__)
    if (relay == nullptr || TqTraceLinuxRelayStreamEvent == nullptr) {
        return;
    }
    TqTraceLinuxRelayStreamEvent(
        Config.WorkerIndex,
        relay->Id,
        streamEvent,
        errorCode,
        status,
        absoluteOffset,
        totalBufferLength,
        bufferCount,
        receiveFlags,
        fin,
        relay->OutstandingQuicSends,
        relay->OutstandingQuicSendBytes,
        relay->PendingTcpWrites.size(),
        PendingTcpWriteBytes(relay->PendingTcpWrites),
        relay->PendingQuicReceiveBytes,
        relay->TcpReadBytes,
        relay->TcpWriteBytes,
        relay->LastTcpWriteErrno,
        relay->TcpReadClosed,
        relay->TcpWriteClosed,
        relay->QuicSendFinSubmitted,
        relay->QuicSendFinCompleted,
        relay->TcpWriteShutdownQueued,
        relay->Stream == nullptr);
#else
    (void)relay;
    (void)streamEvent;
    (void)errorCode;
    (void)status;
    (void)absoluteOffset;
    (void)totalBufferLength;
    (void)bufferCount;
    (void)receiveFlags;
    (void)fin;
#endif
}

void TqLinuxRelayWorker::SetRelayStop(RelayState* relay, const char* trigger) {
    if (relay == nullptr || relay->Handle == nullptr) {
        return;
    }
#if defined(__GNUC__)
    if (TqTraceLinuxRelayStopConditionEvent != nullptr) {
        TqTraceLinuxRelayStopConditionEvent(
            Config.WorkerIndex,
            relay->Id,
            trigger,
            relay->OutstandingQuicSends,
            relay->OutstandingQuicSendBytes,
            relay->PendingTcpWrites.size(),
            PendingTcpWriteBytes(relay->PendingTcpWrites),
            relay->PendingQuicReceiveBytes,
            relay->TcpReadBytes,
            relay->TcpWriteBytes,
            relay->LastTcpWriteErrno,
            relay->TcpReadClosed,
            relay->TcpWriteClosed,
            relay->QuicSendFinSubmitted,
            relay->QuicSendFinCompleted,
            relay->TcpWriteShutdownQueued,
            relay->Stream == nullptr);
    }
#else
    (void)trigger;
#endif
    relay->Handle->Stop.store(true, std::memory_order_release);
}

void TqLinuxRelayWorker::DetachRelayStreamBinding(
    RelayState* relay,
    MsQuicStream* stream,
    StreamRelayBinding* binding) {
    if (binding == nullptr) {
        if (relay != nullptr) {
            relay->Stream = nullptr;
            relay->StreamBinding = nullptr;
        }
        return;
    }
    binding->Closing.store(true, std::memory_order_release);
    binding->Relay.store(nullptr, std::memory_order_release);
    binding->Handle.store(nullptr, std::memory_order_release);

    MsQuicStream* boundStream = stream;
    if (boundStream == nullptr && relay != nullptr) {
        boundStream = relay->Stream;
    }
    if (boundStream != nullptr && boundStream->Context == binding) {
        boundStream->Callback = MsQuicStream::NoOpCallback;
        boundStream->Context = nullptr;
    }
    if (relay != nullptr) {
        relay->Stream = nullptr;
        relay->StreamBinding = nullptr;
    }
    std::lock_guard<std::mutex> guard(RetiredBindingLock);
    RetiredStreamBindings.emplace_back(binding);
}

void TqLinuxRelayWorker::AbortRelayAndRelease(
    RelayState* relay,
    const char* trigger,
    bool abortStream) {
    if (relay == nullptr) {
        return;
    }
    const bool alreadyClosing = relay->Closing;
    SetRelayStop(relay, trigger);
    relay->Closing = true;
    if (EpollFd >= 0 && relay->TcpFd >= 0) {
        (void)::epoll_ctl(EpollFd, EPOLL_CTL_DEL, relay->TcpFd, nullptr);
    }
    if (relay->TcpFd >= 0) {
        TqResetSocket(relay->TcpFd);
        relay->TcpFd = -1;
    }
    auto* binding = relay->StreamBinding;
    if (!alreadyClosing && abortStream && relay->Stream != nullptr && relay->Stream->Handle != nullptr) {
        (void)relay->Stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
    }
    if (binding != nullptr) {
        binding->Closing.store(true, std::memory_order_release);
        binding->Relay.store(nullptr, std::memory_order_release);
        binding->Handle.store(nullptr, std::memory_order_release);
    }
    if (relay->OutstandingQuicSends == 0) {
        DetachRelayStreamBinding(relay, relay->Stream, binding);
    }
    relay->PendingTcpWrites.clear();
    relay->PendingQuicSendRetries.clear();
    for (auto& pending : relay->PendingQuicReceives) {
        if (pending) {
            CompleteAndDiscardQuicReceive(*pending);
        }
    }
    relay->PendingQuicReceives.clear();
    relay->PendingQuicReceiveBytes = 0;
    {
        std::lock_guard<std::mutex> guard(relay->CallbackPendingQuicReceiveLock);
        for (auto& pending : relay->CallbackPendingQuicReceives) {
            if (pending) {
                CompleteAndDiscardQuicReceive(*pending);
            }
        }
        relay->CallbackPendingQuicReceives.clear();
    }
}

void TqLinuxRelayWorker::FailRelayFatal(RelayState* relay, const char* trigger, bool abortStream) {
    if (relay == nullptr || relay->Closing) {
        return;
    }
    FatalRelayResets.fetch_add(1);
    LogLinuxRelayError(
        Config.WorkerIndex,
        trigger,
        relay->Id,
        relay->TcpFd,
        LastQuicSendStatus.load(),
        LastTcpReadErrno.load(),
        PendingTcpWriteBytes(relay->PendingTcpWrites),
        relay->PendingQuicReceiveBytes,
        relay->OutstandingQuicSends,
        relay->OutstandingQuicSendBytes);
    AbortRelayAndRelease(relay, trigger, abortStream);
}

uint64_t TqLinuxRelayWorker::CurrentMaxBufferedQuicSendBytes() const {
    if (Config.UseDynamicQuicReadAhead) {
        return TqRelayCurrentQuicReadAheadBytes();
    }
    return Config.MaxBufferedQuicSendBytes;
}

uint64_t TqLinuxRelayWorker::CurrentResumeBufferedQuicSendBytes() const {
    const uint64_t pauseThreshold = CurrentMaxBufferedQuicSendBytes();
    return pauseThreshold / 2;
}

uint64_t TqLinuxRelayWorker::CurrentRelayIdealSendBytes(const RelayState* relay) const {
    if (relay == nullptr || relay->IdealSendBufferBytes == 0) {
        return TqValidationInitialIdealSendFallbackBytes;
    }
    return relay->IdealSendBufferBytes;
}

void TqLinuxRelayWorker::HandleQuicIdealSendBuffer(uint64_t relayId, uint64_t byteCount) {
    if (byteCount == 0) {
        return;
    }
    auto relay = FindRelayById(relayId);
    if (relay == nullptr || relay->Closing) {
        return;
    }
    relay->IdealSendBufferBytes = std::max(relay->IdealSendBufferBytes, byteCount);
    relay->HasIdealSendBufferEvent = true;
#if defined(__GNUC__)
    if (TqTraceLinuxRelayIdealSendBufferEvent != nullptr) {
        TqTraceLinuxRelayIdealSendBufferEvent(
            Config.WorkerIndex,
            relay->Id,
            relay->IdealSendBufferBytes,
            relay->OutstandingQuicSendBytes);
    }
#endif
    if (!relay->TcpReadClosed && ShouldResumeTcpReadForQuicBacklog(relay.get())) {
        SetTcpReadBackpressure(relay.get(), false, "ideal_send_buffer");
        ArmTcpReadable(relay.get(), true);
    }
}

bool TqLinuxRelayWorker::ShouldPauseTcpReadForQuicBacklog(const RelayState* relay) const {
    if (relay == nullptr) {
        return false;
    }
    const uint64_t pauseThreshold = CurrentRelayIdealSendBytes(relay);
    return pauseThreshold != 0 && relay->OutstandingQuicSendBytes >= pauseThreshold;
}

bool TqLinuxRelayWorker::ShouldResumeTcpReadForQuicBacklog(const RelayState* relay) const {
    if (relay == nullptr) {
        return false;
    }
    const uint64_t pauseThreshold = CurrentRelayIdealSendBytes(relay);
    if (pauseThreshold == 0) {
        return true;
    }
    return relay->OutstandingQuicSendBytes < pauseThreshold;
}

void TqLinuxRelayWorker::SetTcpReadBackpressure(
    RelayState* relay,
    bool paused,
    const char* reason) {
    if (relay == nullptr || relay->TcpReadPausedByQuicBacklog == paused) {
        return;
    }
    relay->TcpReadPausedByQuicBacklog = paused;
#if defined(__GNUC__)
    if (TqTraceLinuxRelayBackpressureEvent != nullptr) {
        const uint64_t pauseThreshold = CurrentRelayIdealSendBytes(relay);
        TqTraceLinuxRelayBackpressureEvent(
            Config.WorkerIndex,
            relay->Id,
            paused ? "pause" : "resume",
            reason,
            relay->OutstandingQuicSendBytes,
            pauseThreshold,
            pauseThreshold,
            pauseThreshold);
    }
#else
    (void)reason;
#endif
}

void TqLinuxRelayWorker::Wake() {
    if (WakeFd < 0) {
        return;
    }
    if (WakeArmed.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const uint64_t one = 1;
    const ssize_t written = ::write(WakeFd, &one, sizeof(one));
    if (written == static_cast<ssize_t>(sizeof(one))) {
        WakeupWrites.fetch_add(1);
        return;
    }
    if (errno != EAGAIN && errno != EINTR) {
        WakeArmed.store(false, std::memory_order_release);
    }
}

size_t TqLinuxRelayWorker::DrainForTest(size_t budget) {
    return DrainEvents(budget);
}

size_t TqLinuxRelayWorker::DrainEvents(size_t budget) {
    size_t processed = 0;
    while (processed < budget) {
        TqLinuxRelayEvent event{};
        if (!EventQueue.TryPop(event)) {
            break;
        }
        switch (event.Type) {
        case TqLinuxRelayEventType::QuicReceive:
            ProcessQuicReceiveEvent(event);
            break;
        case TqLinuxRelayEventType::QuicReceiveView:
            ProcessQuicReceiveViewEvent(event);
            break;
        case TqLinuxRelayEventType::QuicSendComplete:
            CompleteQuicSend(reinterpret_cast<void*>(event.Value));
            break;
        case TqLinuxRelayEventType::QuicIdealSendBuffer:
            HandleQuicIdealSendBuffer(event.RelayId, event.Value);
            break;
        case TqLinuxRelayEventType::QuicPeerSendAborted:
            ProcessQuicPeerSendAborted(event.RelayId, event.Value);
            break;
        case TqLinuxRelayEventType::QuicPeerReceiveAborted:
            ProcessQuicPeerReceiveAborted(event.RelayId, event.Value);
            break;
        case TqLinuxRelayEventType::QuicShutdownComplete:
            ProcessQuicShutdownComplete(
                event.RelayId,
                event.Value,
                static_cast<uint32_t>(event.Length));
            break;
        case TqLinuxRelayEventType::TcpWritable: {
            auto relay = FindRelayById(event.RelayId);
            if (relay != nullptr) {
                FlushTcpWrites(relay.get());
                FlushDeferredQuicReceives(relay.get());
                FlushTcpWrites(relay.get());
            }
            break;
        }
        case TqLinuxRelayEventType::RegisterRelay: {
            auto* command = static_cast<RegisterRelayCommand*>(event.Control);
            if (command != nullptr) {
                bool cancelled = false;
                {
                    std::lock_guard<std::mutex> guard(command->Mutex);
                    cancelled = command->Cancelled;
                    command->Done = cancelled;
                }
                if (cancelled) {
                    command->Cv.notify_one();
                    break;
                }

                const TqLinuxRelayRegistrationResult result =
                    RegisterRelayWithIdLocal(command->Registration);

                bool rollback = false;
                bool completed = false;
                {
                    std::lock_guard<std::mutex> guard(command->Mutex);
                    if (!command->Cancelled) {
                        command->Result = result;
                        command->Done = true;
                        completed = true;
                    } else {
                        rollback = result.Ok;
                    }
                }
                if (rollback) {
                    UnregisterRelayLocal(result.RelayId);
                }
                if (!completed) {
                    std::lock_guard<std::mutex> guard(command->Mutex);
                    command->Done = true;
                }
                command->Cv.notify_one();
            } else {
                CompleteRegisterCommand(command, {});
            }
            break;
        }
        case TqLinuxRelayEventType::UnregisterRelay: {
            auto* command = static_cast<UnregisterRelayCommand*>(event.Control);
            if (command != nullptr) {
                uint64_t relayId = 0;
                bool cancelled = false;
                {
                    std::lock_guard<std::mutex> guard(command->Mutex);
                    cancelled = command->Cancelled;
                    relayId = command->RelayId;
                    command->Done = cancelled;
                }
                if (cancelled) {
                    command->Cv.notify_one();
                    break;
                }

                UnregisterRelayLocal(relayId);

                {
                    std::lock_guard<std::mutex> guard(command->Mutex);
                    command->Done = true;
                }
                command->Cv.notify_one();
            } else {
                CompleteUnregisterCommand(command);
            }
            break;
        }
        case TqLinuxRelayEventType::Snapshot: {
            auto* command = static_cast<SnapshotCommand*>(event.Control);
            if (command != nullptr) {
                bool cancelled = false;
                {
                    std::lock_guard<std::mutex> guard(command->Mutex);
                    cancelled = command->Cancelled;
                    command->Done = cancelled;
                }
                if (cancelled) {
                    command->Cv.notify_one();
                    break;
                }

                TqLinuxRelayWorkerSnapshot snapshot = SnapshotLocal();

                {
                    std::lock_guard<std::mutex> guard(command->Mutex);
                    if (!command->Cancelled) {
                        command->Result = std::move(snapshot);
                    }
                    command->Done = true;
                }
                command->Cv.notify_one();
            } else {
                CompleteSnapshotCommand(command, {});
            }
            break;
        }
        case TqLinuxRelayEventType::Shutdown:
            UnregisterRelayLocal(event.RelayId);
            break;
        case TqLinuxRelayEventType::CallbackReceiveQueueFailedShutdown: {
            auto relay = FindRelayById(event.RelayId);
            SetRelayStop(relay.get(), "quic_receive_queue_failed");
            UnregisterRelayLocal(event.RelayId);
            break;
        }
        case TqLinuxRelayEventType::TakeCapturedQuicBytesForTest: {
            auto* command = static_cast<TakeCapturedQuicBytesForTestCommand*>(event.Control);
            CompleteTakeCapturedQuicBytesForTestCommand(
                command,
                command != nullptr ? TakeCapturedQuicBytesForTestLocal(command->TcpFd)
                                   : std::vector<uint8_t>{});
            break;
        }
        case TqLinuxRelayEventType::EnqueueQuicReceiveForTest: {
            auto* command = static_cast<EnqueueQuicReceiveForTestCommand*>(event.Control);
            const bool result =
                command != nullptr &&
                EnqueueQuicReceiveForTestLocal(
                    command->TcpFd,
                    command->Data.data(),
                    command->Data.size(),
                    command->Fin);
            CompleteEnqueueQuicReceiveForTestCommand(command, result);
            break;
        }
        case TqLinuxRelayEventType::FlushTcpWritableForTest: {
            auto* command = static_cast<FlushTcpWritableForTestCommand*>(event.Control);
            const bool result =
                command != nullptr && FlushTcpWritableForTestLocal(command->TcpFd);
            CompleteFlushTcpWritableForTestCommand(command, result);
            break;
        }
        case TqLinuxRelayEventType::RelayIndexesConsistentForTest: {
            auto* command = static_cast<RelayIndexesConsistentForTestCommand*>(event.Control);
            CompleteRelayIndexesConsistentForTestCommand(
                command,
                RelayIndexesConsistentForTestLocal());
            break;
        }
        case TqLinuxRelayEventType::DispatchTcpEventsForTest: {
            auto* command = static_cast<DispatchTcpEventsForTestCommand*>(event.Control);
            const bool result =
                command != nullptr &&
                DispatchTcpEventsForTestLocal(command->RelayId, command->Events);
            CompleteDispatchTcpEventsForTestCommand(command, result);
            break;
        }
        default:
            break;
        }
        ++processed;
    }
    EventsProcessed.fetch_add(processed);

    std::vector<std::shared_ptr<RelayState>> activeRelays(Relays.begin(), Relays.end());
    for (const auto& relay : activeRelays) {
        RetryPendingQuicSends(relay.get());
        DrainCallbackPendingQuicReceives(relay.get());
    }

    PurgeRetiredRelaysIfIdle();

    WakeArmed.store(false, std::memory_order_release);
    if (EventQueue.SizeApprox() != 0) {
        Wake();
    }
    return processed;
}

void TqLinuxRelayWorker::PurgeRetiredRelaysIfIdle() {
    if (EventQueue.SizeApprox() != 0) {
        return;
    }
    for (auto it = RetiredRelays.begin(); it != RetiredRelays.end();) {
        const auto& relay = *it;
        if (relay == nullptr ||
            (relay->OutstandingQuicSends == 0 && relay->StreamBinding == nullptr)) {
            if (relay != nullptr) {
                RetiredRelaysById.erase(relay->Id);
            }
            it = RetiredRelays.erase(it);
        } else {
            ++it;
        }
    }
}

void TqLinuxRelayWorker::CompleteRegisterCommand(
    RegisterRelayCommand* command,
    TqLinuxRelayRegistrationResult result) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(command->Mutex);
        command->Result = result;
        command->Done = true;
    }
    command->Cv.notify_one();
}

void TqLinuxRelayWorker::CompleteUnregisterCommand(UnregisterRelayCommand* command) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(command->Mutex);
        command->Done = true;
    }
    command->Cv.notify_one();
}

void TqLinuxRelayWorker::CompleteSnapshotCommand(
    SnapshotCommand* command,
    TqLinuxRelayWorkerSnapshot snapshot) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(command->Mutex);
        command->Result = std::move(snapshot);
        command->Done = true;
    }
    command->Cv.notify_one();
}

void TqLinuxRelayWorker::CompleteTakeCapturedQuicBytesForTestCommand(
    TakeCapturedQuicBytesForTestCommand* command,
    std::vector<uint8_t> result) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(command->Mutex);
        command->Result = std::move(result);
        command->Done = true;
    }
    command->Cv.notify_one();
}

void TqLinuxRelayWorker::CompleteEnqueueQuicReceiveForTestCommand(
    EnqueueQuicReceiveForTestCommand* command,
    bool result) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(command->Mutex);
        command->Result = result;
        command->Done = true;
    }
    command->Cv.notify_one();
}

void TqLinuxRelayWorker::CompleteFlushTcpWritableForTestCommand(
    FlushTcpWritableForTestCommand* command,
    bool result) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(command->Mutex);
        command->Result = result;
        command->Done = true;
    }
    command->Cv.notify_one();
}

void TqLinuxRelayWorker::CompleteRelayIndexesConsistentForTestCommand(
    RelayIndexesConsistentForTestCommand* command,
    bool result) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(command->Mutex);
        command->Result = result;
        command->Done = true;
    }
    command->Cv.notify_one();
}

void TqLinuxRelayWorker::CompleteDispatchTcpEventsForTestCommand(
    DispatchTcpEventsForTestCommand* command,
    bool result) {
    if (command == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(command->Mutex);
        command->Result = result;
        command->Done = true;
    }
    command->Cv.notify_one();
}

bool TqLinuxRelayWorker::WaitRegisterCommand(RegisterRelayCommand& command) const {
    return WaitForCommandDone(
        command,
        Config.ControlCommandTimeoutMs,
        ControlCommandWaitNanos,
        ControlCommandWaitCount,
        ControlCommandTimeouts);
}

bool TqLinuxRelayWorker::WaitUnregisterCommand(UnregisterRelayCommand& command) const {
    return WaitForCommandDone(
        command,
        Config.ControlCommandTimeoutMs,
        ControlCommandWaitNanos,
        ControlCommandWaitCount,
        ControlCommandTimeouts);
}

bool TqLinuxRelayWorker::WaitSnapshotCommand(SnapshotCommand& command) const {
    const bool done = WaitForCommandDone(
        command,
        Config.ControlCommandTimeoutMs,
        ControlCommandWaitNanos,
        ControlCommandWaitCount,
        ControlCommandTimeouts);
    SnapshotCommandWaitNanos.fetch_add(command.WaitNanos, std::memory_order_relaxed);
    SnapshotCommandWaitCount.fetch_add(1, std::memory_order_relaxed);
    if (!done) {
        SnapshotCommandTimeouts.fetch_add(1, std::memory_order_relaxed);
    }
    return done;
}

std::unique_lock<std::mutex> TqLinuxRelayWorker::AcquireControlLockForMetrics() const {
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(ControlLock);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ControlLockWaitNanos.fetch_add(
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
        std::memory_order_relaxed);
    ControlLockAcquireCount.fetch_add(1, std::memory_order_relaxed);
    return lock;
}

TqLinuxRelayWorker::ControlState TqLinuxRelayWorker::GetControlState() const {
    const auto current = std::this_thread::get_id();
    auto guard = AcquireControlLockForMetrics();
    ControlState state{};
    state.Running = Running.load(std::memory_order_acquire);
    state.IsWorkerThread = current == WorkerThreadId;
    return state;
}

TqLinuxRelayRegistrationResult TqLinuxRelayWorker::RegisterRelayWithId(
    const TqLinuxRelayRegistration& registration) {
    auto command = std::make_shared<RegisterRelayCommand>();
    command->Registration = registration;

    for (uint32_t attempt = 0; attempt < kLinuxRelayControlEnqueueRetries; ++attempt) {
        bool enqueued = false;
        {
            const auto current = std::this_thread::get_id();
            auto controlGuard = AcquireControlLockForMetrics();
            if (!Running.load(std::memory_order_acquire) || current == WorkerThreadId) {
                return RegisterRelayWithIdLocal(registration);
            }

            TqLinuxRelayEvent event{};
            event.Type = TqLinuxRelayEventType::RegisterRelay;
            event.Control = command.get();
            event.ControlOwner = command;
            enqueued = Enqueue(std::move(event));
        }

        if (!enqueued) {
            ControlCommandEnqueueFailures.fetch_add(1, std::memory_order_relaxed);
            Wake();
            std::this_thread::yield();
            continue;
        }

        if (!WaitRegisterCommand(*command)) {
            return {};
        }
        return command->Result;
    }
    return {};
}

bool TqLinuxRelayWorker::IsWorkerThread() const {
    const ControlState state = GetControlState();
    return state.IsWorkerThread;
}

TqLinuxRelayRegistrationResult TqLinuxRelayWorker::RegisterRelayWithIdLocal(
    const TqLinuxRelayRegistration& registration) {
    TqLinuxRelayRegistrationResult result{};
    if ((!registration.SinkQuicReceives && registration.TcpFd < 0) ||
        (registration.SinkQuicReceives && registration.Stream == nullptr) ||
        EpollFd < 0) {
        return result;
    }
    if (registration.TcpFd >= 0 && !TqSetNonBlocking(registration.TcpFd)) {
        return result;
    }
    if (registration.TcpFd >= 0) {
        ConfigureRelayTcpLiveness(
            registration.TcpFd,
            Config.TcpKeepaliveIdleSec,
            Config.TcpKeepaliveIntervalSec,
            Config.TcpKeepaliveCount,
            Config.TcpUserTimeoutMs);
    }

    auto relay = std::make_shared<RelayState>(registration, Config);

    epoll_event event{};
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLERR;

    relay->Id = NextRelayId++;
    event.data.u64 = relay->Id;
    Relays.push_back(relay);
    RelaysById.emplace(relay->Id, relay);
    result.Ok = true;
    result.RelayId = relay->Id;
    TqRelayDebugLog(
        "event=linux_relay_register worker=%u relay=%llu fd=%d local=%s peer=%s stream=%p handle=%p enable_quic_sends=%d sink_quic_receives=%d",
        Config.WorkerIndex,
        static_cast<unsigned long long>(relay->Id),
        registration.TcpFd,
        registration.TcpFd >= 0 ? GetSocketNameString(registration.TcpFd, false).c_str() : "none",
        registration.TcpFd >= 0 ? GetSocketNameString(registration.TcpFd, true).c_str() : "none",
        static_cast<void*>(registration.Stream),
        static_cast<void*>(registration.Handle),
        registration.EnableQuicSends ? 1 : 0,
        registration.SinkQuicReceives ? 1 : 0);

    if (registration.TcpFd >= 0 &&
        ::epoll_ctl(EpollFd, EPOLL_CTL_ADD, registration.TcpFd, &event) != 0) {
        for (auto it = Relays.begin(); it != Relays.end(); ++it) {
            if ((*it)->Id == relay->Id) {
                Relays.erase(it);
                break;
            }
        }
        RelaysById.erase(relay->Id);
        result.Ok = false;
        result.RelayId = 0;
        return result;
    }

    if (registration.Stream != nullptr) {
        std::unique_ptr<StreamRelayBinding> binding(new (std::nothrow) StreamRelayBinding{});
        if (!binding) {
            if (registration.TcpFd >= 0) {
                ::epoll_ctl(EpollFd, EPOLL_CTL_DEL, registration.TcpFd, nullptr);
            }
            for (auto it = Relays.begin(); it != Relays.end(); ++it) {
                if ((*it)->Id == relay->Id) {
                    Relays.erase(it);
                    break;
                }
            }
            RelaysById.erase(relay->Id);
            result.Ok = false;
            result.RelayId = 0;
            return result;
        }
        binding->Worker = this;
        binding->Relay.store(relay.get(), std::memory_order_release);
        binding->Handle.store(registration.Handle, std::memory_order_release);
        relay->StreamBinding = binding.get();
        registration.Stream->Callback = TqLinuxRelayWorker::StreamCallback;
        registration.Stream->Context = binding.release();
    }

    return result;
}

bool TqLinuxRelayWorker::RegisterRelay(const TqLinuxRelayRegistration& registration) {
    return RegisterRelayWithId(registration).Ok;
}

bool TqLinuxRelayWorker::RegisterRelayForTest(const TqLinuxRelayRegistration& registration) {
    return RegisterRelay(registration);
}

#if defined(TQ_UNIT_TESTING)
bool TqLinuxRelayWorker::RelayIndexesConsistentForTest() const {
    if (IsWorkerThread()) {
        return RelayIndexesConsistentForTestLocal();
    }

    std::unique_lock<std::mutex> controlGuard(ControlLock);
    for (;;) {
        if (!Running.load() || std::this_thread::get_id() == WorkerThreadId) {
            return RelayIndexesConsistentForTestLocal();
        }

        RelayIndexesConsistentForTestCommand command{};

        TqLinuxRelayEvent event{};
        event.Type = TqLinuxRelayEventType::RelayIndexesConsistentForTest;
        event.Control = &command;
        if (!const_cast<TqLinuxRelayWorker*>(this)->Enqueue(std::move(event))) {
            controlGuard.unlock();
            const_cast<TqLinuxRelayWorker*>(this)->Wake();
            std::this_thread::yield();
            controlGuard.lock();
            continue;
        }

        std::unique_lock<std::mutex> lock(command.Mutex);
        command.Cv.wait(lock, [&command]() {
            return command.Done;
        });
        return command.Result;
    }
}
#endif

bool TqLinuxRelayWorker::RelayIndexesConsistentForTestLocal() const {
    if (Relays.size() != RelaysById.size() ||
        RetiredRelays.size() != RetiredRelaysById.size()) {
        return false;
    }
    for (const auto& relay : Relays) {
        if (relay == nullptr) {
            return false;
        }
        auto found = RelaysById.find(relay->Id);
        if (found == RelaysById.end() || found->second.get() != relay.get()) {
            return false;
        }
    }
    for (const auto& relay : RetiredRelays) {
        if (relay == nullptr) {
            return false;
        }
        auto found = RetiredRelaysById.find(relay->Id);
        if (found == RetiredRelaysById.end() || found->second.get() != relay.get()) {
            return false;
        }
    }
    return true;
}

void TqLinuxRelayWorker::UnregisterRelay(uint64_t relayId) {
    auto command = std::make_shared<UnregisterRelayCommand>();
    command->RelayId = relayId;

    for (uint32_t attempt = 0; attempt < kLinuxRelayControlEnqueueRetries; ++attempt) {
        bool enqueued = false;
        {
            const auto current = std::this_thread::get_id();
            auto controlGuard = AcquireControlLockForMetrics();
            if (!Running.load(std::memory_order_acquire) || current == WorkerThreadId) {
                UnregisterRelayLocal(relayId);
                return;
            }

            TqLinuxRelayEvent event{};
            event.Type = TqLinuxRelayEventType::UnregisterRelay;
            event.Control = command.get();
            event.ControlOwner = command;
            enqueued = Enqueue(std::move(event));
        }

        if (!enqueued) {
            ControlCommandEnqueueFailures.fetch_add(1, std::memory_order_relaxed);
            Wake();
            std::this_thread::yield();
            continue;
        }

        (void)WaitUnregisterCommand(*command);
        return;
    }
}

void TqLinuxRelayWorker::UnregisterRelayLocal(uint64_t relayId) {
    std::shared_ptr<RelayState> removed;
    auto found = RelaysById.find(relayId);
    if (found != RelaysById.end()) {
        removed = found->second;
        RelaysById.erase(found);
        for (auto it = Relays.begin(); it != Relays.end(); ++it) {
            if ((*it)->Id == relayId) {
                Relays.erase(it);
                break;
            }
        }
    }
    if (!removed) {
        return;
    }
#if defined(__GNUC__)
    if (TqTraceLinuxRelayUnregisterEvent != nullptr) {
        TqTraceLinuxRelayUnregisterEvent(
            Config.WorkerIndex,
            removed->Id,
            removed->OutstandingQuicSends,
            removed->OutstandingQuicSendBytes,
            removed->PendingTcpWrites.size(),
            PendingTcpWriteBytes(removed->PendingTcpWrites),
            removed->PendingQuicReceiveBytes,
            removed->TcpReadBytes,
            removed->TcpWriteBytes,
            removed->LastTcpWriteErrno,
            removed->TcpReadClosed,
            removed->TcpWriteClosed,
            removed->QuicSendFinSubmitted,
            removed->QuicSendFinCompleted,
            removed->TcpWriteShutdownQueued,
            removed->Stream == nullptr);
    }
#endif
    int peerErrno = 0;
    TqRelayDebugLog(
        "event=linux_relay_unregister_detail worker=%u relay=%llu fd=%d local=%s peer=%s peer_errno=%d closing=%d stop=%d outstanding_quic_sends=%llu pending_quic_receive_bytes=%llu tcp_read_closed=%d tcp_write_closed=%d",
        Config.WorkerIndex,
        static_cast<unsigned long long>(removed->Id),
        removed->TcpFd,
        removed->TcpFd >= 0 ? GetSocketNameString(removed->TcpFd, false).c_str() : "none",
        removed->TcpFd >= 0 ? GetSocketNameString(removed->TcpFd, true, &peerErrno).c_str() : "none",
        peerErrno,
        removed->Closing ? 1 : 0,
        removed->Handle != nullptr && removed->Handle->Stop.load(std::memory_order_acquire) ? 1 : 0,
        static_cast<unsigned long long>(removed->OutstandingQuicSends),
        static_cast<unsigned long long>(removed->PendingQuicReceiveBytes),
        removed->TcpReadClosed ? 1 : 0,
        removed->TcpWriteClosed ? 1 : 0);
    removed->Closing = true;
    const int tcpFd = removed->TcpFd;
    removed->TcpFd = -1;
    if (EpollFd >= 0 && tcpFd >= 0) {
        ::epoll_ctl(EpollFd, EPOLL_CTL_DEL, tcpFd, nullptr);
    }
    if (tcpFd >= 0) {
        ::shutdown(tcpFd, SHUT_RDWR);
        TqCloseSocket(tcpFd);
    }
    auto* binding = removed->StreamBinding;
    if (binding != nullptr) {
        binding->Closing.store(true, std::memory_order_release);
        binding->Relay.store(nullptr, std::memory_order_release);
        binding->Handle.store(nullptr, std::memory_order_release);
    }
    if (removed->OutstandingQuicSends == 0) {
        DetachRelayStreamBinding(removed.get(), removed->Stream, binding);
    }
    removed->PendingTcpWrites.clear();
    removed->PendingQuicSendRetries.clear();
    for (auto& pending : removed->PendingQuicReceives) {
        if (pending) {
            CompleteAndDiscardQuicReceive(*pending);
        }
    }
    removed->PendingQuicReceives.clear();
    removed->PendingQuicReceiveBytes = 0;
    {
        std::lock_guard<std::mutex> guard(removed->CallbackPendingQuicReceiveLock);
        for (auto& pending : removed->CallbackPendingQuicReceives) {
            if (pending) {
                CompleteAndDiscardQuicReceive(*pending);
            }
        }
        removed->CallbackPendingQuicReceives.clear();
    }
    RetiredRelays.push_back(removed);
    RetiredRelaysById.emplace(removed->Id, removed);
}

bool TqLinuxRelayWorker::WaitForObservedTcpBytesForTest(uint64_t bytes, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (TcpReadBytes.load() >= bytes) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return TcpReadBytes.load() >= bytes;
}

std::vector<uint8_t> TqLinuxRelayWorker::TakeCapturedQuicBytesForTest(int tcpFd) {
    if (IsWorkerThread()) {
        return TakeCapturedQuicBytesForTestLocal(tcpFd);
    }

    std::unique_lock<std::mutex> controlGuard(ControlLock);
    for (;;) {
        if (!Running.load() || std::this_thread::get_id() == WorkerThreadId) {
            return TakeCapturedQuicBytesForTestLocal(tcpFd);
        }

        TakeCapturedQuicBytesForTestCommand command{};
        command.TcpFd = tcpFd;

        TqLinuxRelayEvent event{};
        event.Type = TqLinuxRelayEventType::TakeCapturedQuicBytesForTest;
        event.Control = &command;
        if (!Enqueue(std::move(event))) {
            controlGuard.unlock();
            Wake();
            std::this_thread::yield();
            controlGuard.lock();
            continue;
        }

        std::unique_lock<std::mutex> lock(command.Mutex);
        command.Cv.wait(lock, [&command]() {
            return command.Done;
        });
        return std::move(command.Result);
    }
}

std::vector<uint8_t> TqLinuxRelayWorker::TakeCapturedQuicBytesForTestLocal(int tcpFd) {
    auto relay = FindRelayByFd(tcpFd);
    if (relay == nullptr) {
        return {};
    }
    std::vector<uint8_t> out;
    out.swap(relay->CapturedQuicBytesForTest);
    return out;
}

void TqLinuxRelayWorker::DrainTcpReadable(RelayState* relay) {
    if (relay == nullptr || relay->Closing) {
        return;
    }
    if (ShouldPauseTcpReadForQuicBacklog(relay)) {
        SetTcpReadBackpressure(relay, true, "quic_send_backlog");
        ArmTcpReadable(relay, false);
        return;
    }

    uint64_t readBytes = 0;
    const uint64_t tickBudget = std::min<uint64_t>(Config.ReadBatchBytes, Config.ByteBudgetPerTick);
    while (readBytes < tickBudget) {
        std::vector<TqBufferRef> refs;
        std::vector<iovec> iov;
        const size_t maxIov = std::min<size_t>(Config.MaxIov, 1024);
        refs.reserve(maxIov);
        iov.reserve(maxIov);

        for (size_t i = 0; i < maxIov && readBytes + Config.ReadChunkSize <= tickBudget; ++i) {
            TqBufferAcquireFailure acquireFailure = TqBufferAcquireFailure::None;
            auto buffer = TqAllocateRelayBuffer(relay, Config.ReadChunkSize, &acquireFailure);
            if (!buffer) {
                if (acquireFailure == TqBufferAcquireFailure::PendingBytesLimit) {
                    ArmTcpReadable(relay, false);
                } else {
                    RecordBufferAcquireFailure(RelayErrorKind::TcpReadBufferAcquire, acquireFailure);
                }
                break;
            }
            iovec item{};
            item.iov_base = buffer->Data();
            item.iov_len = buffer->Capacity();
            iov.push_back(item);
            refs.push_back(std::move(buffer));
        }
        if (iov.empty()) {
            break;
        }

        const ssize_t received = ::readv(relay->TcpFd, iov.data(), static_cast<int>(iov.size()));
        if (received > 0) {
            size_t remaining = static_cast<size_t>(received);
            std::vector<TqBufferView> views;
            views.reserve(refs.size());
            for (auto& ref : refs) {
                if (remaining == 0) {
                    break;
                }
                const size_t len = std::min(ref->Capacity(), remaining);
                ref->SetLength(len);
                uint8_t* data = ref->Data();
                views.push_back(TqBufferView{data, len, std::move(ref)});
                remaining -= len;
            }

            readBytes += static_cast<uint64_t>(received);
            relay->TcpReadBytes += static_cast<uint64_t>(received);
            TcpReadBytes.fetch_add(static_cast<uint64_t>(received));
            TcpReadBatches.fetch_add(1);
            uint64_t previous = MaxTcpReadIovUsed.load();
            while (previous < views.size() &&
                   !MaxTcpReadIovUsed.compare_exchange_weak(previous, views.size())) {
            }
            std::vector<TqBufferView> sendViews;
            if (!BuildTcpToQuicViews(relay, views, sendViews) ||
                !SubmitTcpBatchToQuic(relay, sendViews)) {
                SetRelayStop(relay, "tcp_to_quic_submit_failed");
                break;
            }
            if (ShouldPauseTcpReadForQuicBacklog(relay)) {
                SetTcpReadBackpressure(relay, true, "quic_send_backlog");
                ArmTcpReadable(relay, false);
                break;
            }
            continue;
        }
        if (received == 0) {
            if (!relay->TcpReadClosed) {
                TqRelayDebugLog(
                    "event=linux_relay_tcp_read worker=%u relay=%llu fd=%d result=eof tcp_read_closed=%d tcp_write_closed=%d quic_fin_submitted=%d quic_fin_completed=%d",
                    Config.WorkerIndex,
                    static_cast<unsigned long long>(relay->Id),
                    relay->TcpFd,
                    relay->TcpReadClosed ? 1 : 0,
                    relay->TcpWriteClosed ? 1 : 0,
                    relay->QuicSendFinSubmitted ? 1 : 0,
                    relay->QuicSendFinCompleted ? 1 : 0);
                relay->TcpReadClosed = true;
                ArmTcpReadable(relay, false);
                if (!FinishTcpToQuic(relay)) {
                    SetRelayStop(relay, "tcp_read_fin_quic_fin_failed");
                }
            }
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        const uint64_t savedErrno = static_cast<uint64_t>(errno);
        TqRelayDebugLog(
            "event=linux_relay_tcp_read worker=%u relay=%llu fd=%d result=hard_error errno=%llu tcp_read_closed=%d tcp_write_closed=%d",
            Config.WorkerIndex,
            static_cast<unsigned long long>(relay->Id),
            relay->TcpFd,
            static_cast<unsigned long long>(savedErrno),
            relay->TcpReadClosed ? 1 : 0,
            relay->TcpWriteClosed ? 1 : 0);
        RecordError(RelayErrorKind::TcpReadHard);
        LastTcpReadErrno.store(savedErrno);
        FailRelayFatal(relay, "tcp_read_hard_error", true);
        break;
    }
}

bool TqLinuxRelayWorker::BuildTcpToQuicViews(
    RelayState* relay,
    std::vector<TqBufferView>& input,
    std::vector<TqBufferView>& output) {
    output.clear();
    if (relay == nullptr) {
        return false;
    }
    if (relay->Compressor == nullptr || relay->CompressAlgo == TqCompressAlgo::None) {
        output = std::move(input);
        return true;
    }

    relay->CompressionOutput.clear();
    for (const auto& view : input) {
        if (!relay->Compressor->Compress(view.Data, view.Len, relay->CompressionOutput, false)) {
            RecordError(RelayErrorKind::TcpToQuicCompress);
            TcpToQuicCompressUpdateFailures.fetch_add(1);
            return false;
        }
    }
    if (relay->CompressionOutput.empty() &&
        !relay->Compressor->Flush(relay->CompressionOutput)) {
        RecordError(RelayErrorKind::TcpToQuicCompress);
        TcpToQuicCompressFlushFailures.fetch_add(1);
        return false;
    }
    if (relay->CompressionOutput.empty()) {
        input.clear();
        return true;
    }

    size_t offset = 0;
    while (offset < relay->CompressionOutput.size()) {
        TqBufferAcquireFailure acquireFailure = TqBufferAcquireFailure::None;
        auto buffer = TqAllocateRelayBuffer(relay, Config.ReadChunkSize, &acquireFailure);
        if (!buffer) {
            RecordBufferAcquireFailure(
                RelayErrorKind::TcpToQuicBufferAcquire, acquireFailure);
            return false;
        }
        const size_t chunk = std::min(buffer->Capacity(), relay->CompressionOutput.size() - offset);
        std::memcpy(buffer->Data(), relay->CompressionOutput.data() + offset, chunk);
        buffer->SetLength(chunk);
        uint8_t* data = buffer->Data();
        output.push_back(TqBufferView{data, chunk, std::move(buffer)});
        offset += chunk;
    }
    CompressedTcpBytes.fetch_add(relay->CompressionOutput.size());
    input.clear();
    return true;
}

bool TqLinuxRelayWorker::FinishTcpToQuic(RelayState* relay) {
    if (relay == nullptr || relay->Closing) {
        return false;
    }

    std::vector<TqBufferView> sendViews;
    if (relay->Compressor != nullptr && relay->CompressAlgo != TqCompressAlgo::None) {
        relay->CompressionOutput.clear();
        if (!relay->Compressor->Compress(nullptr, 0, relay->CompressionOutput, true)) {
            RecordError(RelayErrorKind::TcpToQuicCompress);
            TcpToQuicCompressFlushFailures.fetch_add(1);
            return false;
        }

        size_t offset = 0;
        while (offset < relay->CompressionOutput.size()) {
            TqBufferAcquireFailure acquireFailure = TqBufferAcquireFailure::None;
            auto buffer = TqAllocateRelayBuffer(relay, Config.ReadChunkSize, &acquireFailure);
            if (!buffer) {
                RecordBufferAcquireFailure(
                    RelayErrorKind::TcpToQuicBufferAcquire, acquireFailure);
                return false;
            }
            const size_t chunk = std::min(buffer->Capacity(), relay->CompressionOutput.size() - offset);
            std::memcpy(buffer->Data(), relay->CompressionOutput.data() + offset, chunk);
            buffer->SetLength(chunk);
            uint8_t* data = buffer->Data();
            sendViews.push_back(TqBufferView{data, chunk, std::move(buffer)});
            offset += chunk;
        }
        CompressedTcpBytes.fetch_add(relay->CompressionOutput.size());
    }

    return SubmitTcpBatchToQuic(relay, sendViews, QUIC_SEND_FLAG_FIN);
}

void TqLinuxRelayWorker::MaybeStopFullyClosedRelay(RelayState* relay, const char* trigger) {
    if (relay == nullptr || relay->Closing || relay->Handle == nullptr) {
        return;
    }
    if (!relay->TcpReadClosed || !relay->TcpWriteClosed) {
        return;
    }
    if (!relay->QuicSendFinSubmitted || !relay->QuicSendFinCompleted) {
        return;
    }
    if (relay->OutstandingQuicSends != 0 ||
        !relay->PendingTcpWrites.empty() ||
        !relay->PendingQuicReceives.empty() ||
        relay->PendingQuicReceiveBytes != 0) {
        return;
    }
    SetRelayStop(relay, trigger);
}

bool TqLinuxRelayWorker::HasPendingAfterStreamShutdown(RelayState* relay) const {
    if (relay == nullptr) {
        return false;
    }
    bool hasCallbackPending = false;
    {
        std::lock_guard<std::mutex> guard(relay->CallbackPendingQuicReceiveLock);
        hasCallbackPending = !relay->CallbackPendingQuicReceives.empty();
    }
    return relay->OutstandingQuicSends != 0 ||
           relay->OutstandingQuicSendBytes != 0 ||
           !relay->PendingQuicSendRetries.empty() ||
           !relay->PendingTcpWrites.empty() ||
           !relay->PendingQuicReceives.empty() ||
           hasCallbackPending ||
           relay->PendingQuicReceiveBytes != 0 ||
           relay->TcpWriteShutdownQueued ||
           relay->QuicSendFinSubmitted != relay->QuicSendFinCompleted;
}

void TqLinuxRelayWorker::CompleteAndDiscardQuicReceive(TqPendingQuicReceive& view) {
    const uint64_t remaining = view.TotalLength >= view.CompletedLength
        ? view.TotalLength - view.CompletedLength
        : 0;
    if (remaining != 0) {
        view.PendingCompleteBytes += remaining;
        view.CompletedLength += remaining;
    }
    FlushDeferredReceiveCompletion(view, true);
}

// In production, ClientContext owns TqLinuxRelaySendOperation until
// QUIC_STREAM_EVENT_SEND_COMPLETE is delivered back to the owner worker.
// Tests can disable sends to verify readv batching without a live MsQuic stream.
bool TqLinuxRelayWorker::SubmitTcpBatchToQuic(
    RelayState* relay,
    std::vector<TqBufferView>& views,
    QUIC_SEND_FLAGS flags) {
    if (relay == nullptr) {
        return false;
    }
    if (views.empty()) {
        if (flags == QUIC_SEND_FLAG_FIN) {
            relay->QuicSendFinSubmitted = true;
        }
        if (!relay->EnableQuicSends) {
            if (flags == QUIC_SEND_FLAG_FIN) {
                relay->QuicSendFinCompleted = true;
                MaybeStopFullyClosedRelay(relay, "quic_fin_completed_no_send");
            }
            return true;
        }
        if (flags == QUIC_SEND_FLAG_FIN) {
            if (relay->Closing || relay->Stream == nullptr || relay->Stream->Handle == nullptr) {
                RecordError(RelayErrorKind::QuicSend);
                return false;
            }
            auto* operation = new (std::nothrow) TqLinuxRelaySendOperation{};
            if (operation == nullptr) {
                RecordError(RelayErrorKind::QuicSend);
                QuicSendOperationAllocFailures.fetch_add(1);
                QuicSendFatalErrors.fetch_add(1);
                LogLinuxRelayError(
                    Config.WorkerIndex,
                    "quic_fin_send_operation_alloc_failed",
                    relay->Id,
                    relay->TcpFd,
                    0,
                    0,
                    PendingTcpWriteBytes(relay->PendingTcpWrites),
                    relay->PendingQuicReceiveBytes,
                    relay->OutstandingQuicSends,
                    relay->OutstandingQuicSendBytes);
                return false;
            }
            operation->RelayId = relay->Id;
            operation->Fin = true;
            relay->QuicSendFinSubmitted = true;
            return TrySubmitQuicSendOperation(relay, operation);
        }
        return true;
    }
    if (!relay->EnableQuicSends) {
        if (flags == QUIC_SEND_FLAG_FIN) {
            relay->QuicSendFinSubmitted = true;
            relay->QuicSendFinCompleted = true;
            MaybeStopFullyClosedRelay(relay, "quic_fin_completed_test_capture");
        }
        for (const auto& view : views) {
            relay->CapturedQuicBytesForTest.insert(
                relay->CapturedQuicBytesForTest.end(),
                view.Data,
                view.Data + view.Len);
        }
        views.clear();
        return true;
    }
    if (relay->Closing || relay->Stream == nullptr || relay->Stream->Handle == nullptr) {
        views.clear();
        RecordError(RelayErrorKind::QuicSend);
        return false;
    }

    std::vector<QUIC_BUFFER> quicBuffers;
    quicBuffers.reserve(views.size());
    uint64_t totalBytes = 0;
    for (const auto& view : views) {
        if (view.Len > UINT32_MAX) {
            RecordError(RelayErrorKind::QuicSend);
            QuicSendBufferTooLargeFailures.fetch_add(1);
            QuicSendFatalErrors.fetch_add(1);
            LogLinuxRelayError(
                Config.WorkerIndex,
                "quic_send_buffer_too_large",
                relay->Id,
                relay->TcpFd,
                0,
                0,
                PendingTcpWriteBytes(relay->PendingTcpWrites),
                relay->PendingQuicReceiveBytes,
                relay->OutstandingQuicSends,
                relay->OutstandingQuicSendBytes);
            return false;
        }
        QUIC_BUFFER buffer{};
        buffer.Buffer = view.Data;
        buffer.Length = static_cast<uint32_t>(view.Len);
        quicBuffers.push_back(buffer);
        totalBytes += view.Len;
    }

    auto* operation = new (std::nothrow) TqLinuxRelaySendOperation{};
    if (operation == nullptr) {
        RecordError(RelayErrorKind::QuicSend);
        QuicSendOperationAllocFailures.fetch_add(1);
        QuicSendFatalErrors.fetch_add(1);
        LogLinuxRelayError(
            Config.WorkerIndex,
            "quic_send_operation_alloc_failed",
            relay->Id,
            relay->TcpFd,
            0,
            0,
            PendingTcpWriteBytes(relay->PendingTcpWrites),
            relay->PendingQuicReceiveBytes,
            relay->OutstandingQuicSends,
            relay->OutstandingQuicSendBytes);
        return false;
    }
    operation->RelayId = relay->Id;
    operation->TotalBytes = totalBytes;
    operation->Fin = (flags == QUIC_SEND_FLAG_FIN);
    if (operation->Fin) {
        relay->QuicSendFinSubmitted = true;
    }
    operation->Views = std::move(views);
    operation->QuicBuffers = std::move(quicBuffers);
    return TrySubmitQuicSendOperation(relay, operation);
}

bool TqLinuxRelayWorker::TrySubmitQuicSendOperation(
    RelayState* relay,
    TqLinuxRelaySendOperation* operation) {
    if (relay == nullptr || operation == nullptr) {
        delete operation;
        return false;
    }
    MsQuicStream* stream = relay->Stream;
    if (stream == nullptr || stream->Handle == nullptr || relay->Closing) {
        delete operation;
        RecordError(RelayErrorKind::QuicSend);
        QuicSendFatalErrors.fetch_add(1);
        LogLinuxRelayError(
            Config.WorkerIndex,
            "quic_send_invalid_stream",
            relay->Id,
            relay->TcpFd,
            0,
            0,
            PendingTcpWriteBytes(relay->PendingTcpWrites),
            relay->PendingQuicReceiveBytes,
            relay->OutstandingQuicSends,
            relay->OutstandingQuicSendBytes);
        return false;
    }

    const QUIC_STATUS status = TqLinuxRelayStreamSend(
        stream,
        operation->QuicBuffers.empty() ? nullptr : operation->QuicBuffers.data(),
        static_cast<uint32_t>(operation->QuicBuffers.size()),
        operation->Fin ? QUIC_SEND_FLAG_FIN : QUIC_SEND_FLAG_NONE,
        operation);
    if (QUIC_FAILED(status)) {
        LastQuicSendStatus.store(static_cast<int64_t>(status));
        if (IsQuicSendBackpressureStatus(status)) {
            QuicSendBackpressureEvents.fetch_add(1);
            SetTcpReadBackpressure(relay, true, "quic_send_resource");
            ArmTcpReadable(relay, false);
            relay->PendingQuicSendRetries.emplace_back(operation);
            return true;
        }
        delete operation;
        RecordError(RelayErrorKind::QuicSend);
        QuicSendApiFailures.fetch_add(1);
        QuicSendFatalErrors.fetch_add(1);
        LogLinuxRelayError(
            Config.WorkerIndex,
            "quic_send_api_failed",
            relay->Id,
            relay->TcpFd,
            static_cast<int64_t>(status),
            0,
            PendingTcpWriteBytes(relay->PendingTcpWrites),
            relay->PendingQuicReceiveBytes,
            relay->OutstandingQuicSends,
            relay->OutstandingQuicSendBytes);
        return false;
    }
    ++relay->OutstandingQuicSends;
    relay->OutstandingQuicSendBytes += operation->TotalBytes;
    QuicSendOperations.fetch_add(1);
    return true;
}

void TqLinuxRelayWorker::RetryPendingQuicSends(RelayState* relay) {
    if (relay == nullptr || relay->Closing) {
        return;
    }
    while (!relay->PendingQuicSendRetries.empty()) {
        std::unique_ptr<TqLinuxRelaySendOperation> operation =
            std::move(relay->PendingQuicSendRetries.front());
        relay->PendingQuicSendRetries.pop_front();
        if (operation == nullptr) {
            continue;
        }
        if (!TrySubmitQuicSendOperation(relay, operation.release())) {
            LogLinuxRelayError(
                Config.WorkerIndex,
                "quic_send_retry_failed",
                relay->Id,
                relay->TcpFd,
                LastQuicSendStatus.load(),
                0,
                PendingTcpWriteBytes(relay->PendingTcpWrites),
                relay->PendingQuicReceiveBytes,
                relay->OutstandingQuicSends,
                relay->OutstandingQuicSendBytes);
            SetRelayStop(relay, "quic_send_retry_failed");
            return;
        }
        if (!relay->PendingQuicSendRetries.empty()) {
            return;
        }
    }
    if (relay->PendingQuicSendRetries.empty() &&
        ShouldResumeTcpReadForQuicBacklog(relay)) {
        SetTcpReadBackpressure(relay, false, "quic_send_retry_drained");
        ArmTcpReadable(relay, true);
    }
}

bool TqLinuxRelayWorker::IsQuicSendBackpressureStatus(QUIC_STATUS status) const {
    return status == QUIC_STATUS_OUT_OF_MEMORY || status == QUIC_STATUS_BUFFER_TOO_SMALL;
}

void TqLinuxRelayWorker::CompleteQuicSend(void* context) {
    if (context == nullptr) {
        return;
    }
    auto* operation = static_cast<TqLinuxRelaySendOperation*>(context);
    if (operation->Magic != TqLinuxRelaySendOperation::MagicValue) {
        // Late tunnel-phase send completion after relay replaced the stream callback.
        std::free(context);
        return;
    }
    auto relay = FindRelayById(operation->RelayId);
    if (relay != nullptr && relay->OutstandingQuicSends > 0) {
        --relay->OutstandingQuicSends;
    }
    if (relay != nullptr) {
        relay->OutstandingQuicSendBytes =
            relay->OutstandingQuicSendBytes >= operation->TotalBytes
                ? relay->OutstandingQuicSendBytes - operation->TotalBytes
                : 0;
    }
    const bool finCompleted = operation->Fin;
    delete operation;
    if (relay != nullptr && relay->Closing) {
        if (relay->OutstandingQuicSends == 0) {
            DetachRelayStreamBinding(relay.get(), relay->Stream, relay->StreamBinding);
        }
        return;
    }
    if (relay != nullptr && !relay->Closing) {
        if (finCompleted) {
            relay->QuicSendFinCompleted = true;
        }
        FlushDeferredQuicReceives(relay.get());
        if (!relay->TcpReadClosed && ShouldResumeTcpReadForQuicBacklog(relay.get())) {
            SetTcpReadBackpressure(relay.get(), false, "quic_send_backlog");
            ArmTcpReadable(relay.get(), true);
        }
        MaybeStopFullyClosedRelay(relay.get(), finCompleted ? "quic_send_fin_completed" : "quic_send_completed");
    }
}

std::shared_ptr<TqLinuxRelayWorker::RelayState> TqLinuxRelayWorker::FindRelayById(uint64_t relayId) {
    auto active = RelaysById.find(relayId);
    if (active != RelaysById.end()) {
        return active->second;
    }
    auto retired = RetiredRelaysById.find(relayId);
    if (retired != RetiredRelaysById.end()) {
        return retired->second;
    }
    return nullptr;
}

std::shared_ptr<TqLinuxRelayWorker::RelayState> TqLinuxRelayWorker::FindRelayByFd(int tcpFd) {
    for (const auto& relay : Relays) {
        if (relay->TcpFd == tcpFd) {
            return relay;
        }
    }
    return nullptr;
}

uint64_t TqLinuxRelayWorker::FindRelayIdByStream(MsQuicStream* stream) {
    if (stream == nullptr) {
        return 0;
    }
    StreamLookupScanCount.fetch_add(1);
    for (const auto& relay : Relays) {
        if (relay->Stream == stream) {
            return relay->Id;
        }
    }
    return 0;
}

void TqLinuxRelayWorker::AbortRelayFromCallback(
    uint64_t relayId,
    StreamRelayBinding* binding,
    MsQuicStream* stream) {
    if (stream != nullptr && stream->Handle != nullptr) {
        (void)stream->Shutdown(0, QUIC_STREAM_SHUTDOWN_FLAG_ABORT);
    }
    TqLinuxRelayEvent shutdown{};
    shutdown.Type = TqLinuxRelayEventType::CallbackReceiveQueueFailedShutdown;
    shutdown.RelayId = relayId;
    if (!Enqueue(std::move(shutdown)) && binding != nullptr) {
        if (auto* handle = binding->Handle.load(std::memory_order_acquire);
            handle != nullptr) {
            handle->Stop.store(true, std::memory_order_release);
        }
    }
}

void TqLinuxRelayWorker::ProcessQuicPeerSendAborted(uint64_t relayId, uint64_t errorCode) {
    auto relay = FindRelayById(relayId);
    if (relay == nullptr || relay->Closing) {
        return;
    }
    TraceRelayStreamEvent(
        relay.get(),
        "peer_send_aborted",
        errorCode,
        0,
        0,
        0,
        0,
        0,
        false);
    FailRelayFatal(relay.get(), "stream_peer_send_aborted", false);
}

void TqLinuxRelayWorker::ProcessQuicPeerReceiveAborted(uint64_t relayId, uint64_t errorCode) {
    auto relay = FindRelayById(relayId);
    if (relay == nullptr || relay->Closing) {
        return;
    }
    TraceRelayStreamEvent(
        relay.get(),
        "peer_receive_aborted",
        errorCode,
        0,
        0,
        0,
        0,
        0,
        false);
    FailRelayFatal(relay.get(), "stream_peer_receive_aborted", false);
}

void TqLinuxRelayWorker::ProcessQuicShutdownComplete(
    uint64_t relayId,
    uint64_t errorCode,
    uint32_t status) {
    auto relay = FindRelayById(relayId);
    if (relay == nullptr || relay->Closing) {
        return;
    }
    TraceRelayStreamEvent(
        relay.get(),
        "shutdown_complete",
        errorCode,
        status,
        0,
        0,
        0,
        0,
        false);
#if defined(__GNUC__)
    if (TqTraceLinuxRelayStreamShutdownEvent != nullptr) {
        TqTraceLinuxRelayStreamShutdownEvent(
            Config.WorkerIndex,
            relay->Id,
            relay->OutstandingQuicSends,
            relay->OutstandingQuicSendBytes,
            relay->PendingTcpWrites.size(),
            PendingTcpWriteBytes(relay->PendingTcpWrites),
            relay->PendingQuicReceiveBytes,
            relay->TcpReadBytes,
            relay->TcpWriteBytes,
            relay->LastTcpWriteErrno,
            relay->TcpReadClosed,
            relay->TcpWriteClosed,
            relay->QuicSendFinSubmitted,
            relay->QuicSendFinCompleted,
            relay->TcpWriteShutdownQueued,
            false);
    }
#endif
    const bool hasPending = HasPendingAfterStreamShutdown(relay.get());
    if (hasPending) {
        FatalRelayResets.fetch_add(1);
        LogLinuxRelayError(
            Config.WorkerIndex,
            "stream_shutdown_complete_with_pending",
            relay->Id,
            relay->TcpFd,
            LastQuicSendStatus.load(),
            relay->LastTcpWriteErrno,
            PendingTcpWriteBytes(relay->PendingTcpWrites),
            relay->PendingQuicReceiveBytes,
            relay->OutstandingQuicSends,
            relay->OutstandingQuicSendBytes);
        AbortRelayAndRelease(relay.get(), "stream_shutdown_complete_with_pending", false);
    } else {
        DetachRelayStreamBinding(relay.get(), relay->Stream, relay->StreamBinding);
        MaybeStopFullyClosedRelay(relay.get(), "stream_shutdown_complete");
    }
}

bool TqLinuxRelayWorker::QueueDeferredQuicReceive(
    RelayState* relay,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    bool fin) {
    return QueueDeferredQuicReceiveFromOffset(relay, stream, buffers, bufferCount, 0, fin);
}

bool TqLinuxRelayWorker::QueueDeferredQuicReceiveFromOffset(
    RelayState* relay,
    MsQuicStream* stream,
    const QUIC_BUFFER* buffers,
    uint32_t bufferCount,
    uint64_t completedPrefix,
    bool fin) {
    if (relay == nullptr || relay->Closing ||
        (bufferCount != 0 && buffers == nullptr)) {
        return false;
    }

    std::shared_ptr<TqPendingQuicReceive> view(new (std::nothrow) TqPendingQuicReceive{});
    if (!view) {
        RecordError(RelayErrorKind::QuicReceiveView);
        QuicReceiveViewAllocFailures.fetch_add(1);
        return false;
    }
    view->Stream = stream;
    view->RelayId = relay->Id;
    view->Fin = fin;
    view->PendingCompleteBytes = completedPrefix;
    view->Slices.reserve(bufferCount);

    uint64_t skip = completedPrefix;
    for (uint32_t i = 0; i < bufferCount; ++i) {
        if (buffers[i].Length == 0) {
            continue;
        }
        if (buffers[i].Buffer == nullptr) {
            RecordError(RelayErrorKind::QuicReceiveView);
            QuicReceiveViewNullBufferFailures.fetch_add(1);
            return false;
        }
        const uint8_t* data = buffers[i].Buffer;
        uint32_t length = buffers[i].Length;
        if (skip >= length) {
            skip -= length;
            continue;
        }
        if (skip > 0) {
            data += skip;
            length -= static_cast<uint32_t>(skip);
            skip = 0;
        }
        view->Slices.push_back(TqQuicReceiveSlice{data, length});
        view->TotalLength += length;
    }
    if (view->TotalLength == 0 && !fin) {
        RecordError(RelayErrorKind::QuicReceiveView);
        QuicReceiveViewEmptyFailures.fetch_add(1);
        return false;
    }
    RecordQuicReceiveView(view->TotalLength, view->Slices.size());

    TqLinuxRelayEvent event{};
    event.Type = TqLinuxRelayEventType::QuicReceiveView;
    event.RelayId = relay->Id;
    event.TotalLength = view->TotalLength;
    event.Fin = fin;
    event.ReceiveView = view;
    if (!Enqueue(std::move(event))) {
        QuicReceiveViewBackpressureQueued.fetch_add(1);
        {
            std::lock_guard<std::mutex> guard(relay->CallbackPendingQuicReceiveLock);
            relay->CallbackPendingQuicReceives.push_back(std::move(view));
        }
        if (!relay->QuicReceivePaused) {
            relay->QuicReceivePaused = true;
            SetQuicReceiveEnabled(relay, false);
        }
        Wake();
        return true;
    }
    return true;
}

void TqLinuxRelayWorker::CompleteDeferredQuicReceive(MsQuicStream* stream, uint64_t bytes) {
    if (bytes == 0) {
        return;
    }
    DeferredReceiveCompleteBytes.fetch_add(bytes);
    DeferredReceiveCompletes.fetch_add(1);
    if (stream != nullptr && stream->Handle != nullptr) {
        stream->ReceiveComplete(bytes);
    }
}

void TqLinuxRelayWorker::FlushDeferredReceiveCompletion(
    TqPendingQuicReceive& view,
    bool force) {
    if (view.PendingCompleteBytes == 0) {
        return;
    }
    const uint64_t threshold = Config.DeferredReceiveCompleteBatchBytes;
    if (!force && threshold != 0 && view.PendingCompleteBytes < threshold) {
        return;
    }
    CompleteDeferredQuicReceive(view.Stream, view.PendingCompleteBytes);
    view.PendingCompleteBytes = 0;
    DeferredReceiveCompletionFlushes.fetch_add(1);
}

uint64_t TqLinuxRelayWorker::MaxPendingQuicReceiveBytesPerRelay() const {
    if (Config.MaxPendingQuicReceiveBytesPerRelay != 0) {
        return Config.MaxPendingQuicReceiveBytesPerRelay;
    }
    return Config.MaxPendingBufferBytes;
}

uint64_t TqLinuxRelayWorker::LowPendingQuicReceiveBytesPerRelay() const {
    return MaxPendingQuicReceiveBytesPerRelay();
}

void TqLinuxRelayWorker::SetQuicReceiveEnabled(RelayState* relay, bool enabled) {
    if (relay == nullptr || relay->Stream == nullptr) {
        return;
    }
    if (enabled) {
        QuicReceiveResumedCount.fetch_add(1);
    } else {
        QuicReceivePausedCount.fetch_add(1);
    }
    if (relay->Stream->Handle != nullptr) {
        (void)relay->Stream->ReceiveSetEnabled(enabled);
    }
}

void TqLinuxRelayWorker::MaybePauseQuicReceive(RelayState* relay) {
    if (relay == nullptr || relay->QuicReceivePaused) {
        return;
    }
    if (relay->PendingQuicReceiveBytes >= MaxPendingQuicReceiveBytesPerRelay()) {
        relay->QuicReceivePaused = true;
        SetQuicReceiveEnabled(relay, false);
    }
}

void TqLinuxRelayWorker::MaybeResumeQuicReceive(RelayState* relay) {
    if (relay == nullptr || !relay->QuicReceivePaused) {
        return;
    }
    if (relay->PendingQuicReceiveBytes < LowPendingQuicReceiveBytesPerRelay()) {
        relay->QuicReceivePaused = false;
        SetQuicReceiveEnabled(relay, true);
    }
}

void TqLinuxRelayWorker::ProcessQuicReceiveViewEvent(TqLinuxRelayEvent& event) {
    auto relay = FindRelayById(event.RelayId);
    if (relay == nullptr || relay->Closing || !event.ReceiveView) {
        if (event.ReceiveView) {
            CompleteAndDiscardQuicReceive(*event.ReceiveView);
        }
        return;
    }
    relay->PendingQuicReceiveBytes += event.ReceiveView->TotalLength - event.ReceiveView->CompletedLength;
    relay->PendingQuicReceives.push_back(std::move(event.ReceiveView));
    UpdateAtomicMax(MaxPendingQuicReceiveBytesObserved, relay->PendingQuicReceiveBytes);
    UpdateAtomicMax(MaxPendingQuicReceiveQueueObserved, relay->PendingQuicReceives.size());
    MaybePauseQuicReceive(relay.get());
    FlushDeferredQuicReceives(relay.get());
}

void TqLinuxRelayWorker::DrainCallbackPendingQuicReceives(RelayState* relay) {
    if (relay == nullptr || relay->Closing) {
        return;
    }
    std::deque<std::shared_ptr<TqPendingQuicReceive>> pending;
    {
        std::lock_guard<std::mutex> guard(relay->CallbackPendingQuicReceiveLock);
        pending.swap(relay->CallbackPendingQuicReceives);
    }
    while (!pending.empty()) {
        TqLinuxRelayEvent event{};
        event.Type = TqLinuxRelayEventType::QuicReceiveView;
        event.RelayId = relay->Id;
        event.ReceiveView = std::move(pending.front());
        pending.pop_front();
        if (event.ReceiveView != nullptr) {
            event.TotalLength = event.ReceiveView->TotalLength;
            event.Fin = event.ReceiveView->Fin;
        }
        ProcessQuicReceiveViewEvent(event);
    }
    MaybeResumeQuicReceive(relay);
}

void TqLinuxRelayWorker::FlushDeferredQuicReceives(RelayState* relay) {
    if (relay == nullptr || relay->Closing) {
        return;
    }

    uint64_t burstBytes = 0;
    while (!relay->PendingQuicReceives.empty()) {
        if (Config.TcpWriteBurstBytes != 0 && burstBytes >= Config.TcpWriteBurstBytes) {
            TcpWriteBurstStops.fetch_add(1);
            ArmTcpWritable(relay, true);
            break;
        }

        auto& view = relay->PendingQuicReceives.front();
        if (!view) {
            relay->PendingQuicReceives.pop_front();
            continue;
        }
        FlushDeferredReceiveCompletion(*view, false);

        if (relay->SinkQuicReceives) {
            const uint64_t remaining =
                view->TotalLength >= view->CompletedLength
                    ? view->TotalLength - view->CompletedLength
                    : 0;
            if (remaining > 0) {
                if (relay->SinkQuicReceiveBytes != nullptr) {
                    relay->SinkQuicReceiveBytes->fetch_add(remaining, std::memory_order_relaxed);
                }
                view->PendingCompleteBytes += remaining;
                view->CompletedLength += remaining;
                relay->PendingQuicReceiveBytes =
                    relay->PendingQuicReceiveBytes >= remaining
                        ? relay->PendingQuicReceiveBytes - remaining
                        : 0;
            }
            FlushDeferredReceiveCompletion(*view, true);
            if (view->Fin) {
                SetRelayStop(relay, "quic_receive_fin_sink");
            }
            relay->PendingQuicReceives.pop_front();
            MaybeResumeQuicReceive(relay);
            continue;
        }

        const bool needsDecompress =
            relay->Decompressor != nullptr && relay->CompressAlgo == TqCompressAlgo::Zstd;
        if (needsDecompress) {
            if (DrainCompressedQuicReceiveView(relay, *view)) {
                if (view->Fin) {
                    relay->TcpWriteShutdownQueued = true;
                }
                relay->PendingQuicReceives.pop_front();
                continue;
            }
            break;
        }

        std::vector<iovec> iov;
        iov.reserve(Config.MaxIov);
        size_t sliceIndex = view->SliceIndex;
        size_t sliceOffset = view->SliceOffset;
        uint64_t attemptedBytes = 0;
        uint64_t maxWriteBytes = Config.TcpWriteMaxBytes;
        if (Config.TcpWriteBurstBytes != 0) {
            const uint64_t remainingBurst = Config.TcpWriteBurstBytes - burstBytes;
            maxWriteBytes = maxWriteBytes == 0
                ? remainingBurst
                : std::min(maxWriteBytes, remainingBurst);
        }
        while (sliceIndex < view->Slices.size() &&
               iov.size() < Config.MaxIov &&
               (maxWriteBytes == 0 || attemptedBytes < maxWriteBytes)) {
            const auto& slice = view->Slices[sliceIndex];
            if (sliceOffset >= slice.Length) {
                ++sliceIndex;
                sliceOffset = 0;
                continue;
            }
            uint64_t length = slice.Length - sliceOffset;
            if (maxWriteBytes != 0 && attemptedBytes + length > maxWriteBytes) {
                length = maxWriteBytes - attemptedBytes;
            }
            if (length == 0) {
                break;
            }
            iovec item{};
            item.iov_base = const_cast<uint8_t*>(slice.Data + sliceOffset);
            item.iov_len = static_cast<size_t>(length);
            iov.push_back(item);
            attemptedBytes += length;
            ++sliceIndex;
            sliceOffset = 0;
        }

        if (iov.empty()) {
            FlushDeferredReceiveCompletion(*view, true);
            if (view->Fin) {
                relay->TcpWriteShutdownQueued = true;
            }
            relay->PendingQuicReceives.pop_front();
            continue;
        }

        RecordTcpWriteAttempt(attemptedBytes);
        const ssize_t sent = WritevNoSignal(relay->TcpFd, iov.data(), static_cast<int>(iov.size()));
        if (sent > 0) {
            size_t remaining = static_cast<size_t>(sent);
            burstBytes += static_cast<uint64_t>(sent);
            relay->TcpWriteBytes += static_cast<uint64_t>(sent);
            TcpWriteBytes.fetch_add(static_cast<uint64_t>(sent));
            TcpWriteBatches.fetch_add(1);
            TcpWriteSendmsgCalls.fetch_add(1);
            UpdateAtomicMax(MaxTcpWriteSendmsgBytes, static_cast<uint64_t>(sent));
            RecordTcpWriteReturned(static_cast<uint64_t>(sent));
            if (static_cast<uint64_t>(sent) < attemptedBytes) {
                TcpWritePartialCount.fetch_add(1);
            }
            uint64_t previous = MaxTcpWriteIovUsed.load();
            while (previous < iov.size() &&
                   !MaxTcpWriteIovUsed.compare_exchange_weak(previous, iov.size())) {
            }

            view->PendingCompleteBytes += static_cast<uint64_t>(sent);
            view->CompletedLength += static_cast<uint64_t>(sent);
            relay->PendingQuicReceiveBytes =
                relay->PendingQuicReceiveBytes >= static_cast<uint64_t>(sent)
                    ? relay->PendingQuicReceiveBytes - static_cast<uint64_t>(sent)
                    : 0;

            while (remaining > 0 && view->SliceIndex < view->Slices.size()) {
                const auto& front = view->Slices[view->SliceIndex];
                const size_t available = front.Length - view->SliceOffset;
                if (remaining >= available) {
                    remaining -= available;
                    ++view->SliceIndex;
                    view->SliceOffset = 0;
                } else {
                    view->SliceOffset += remaining;
                    remaining = 0;
                }
            }

            const bool viewComplete = view->CompletedLength >= view->TotalLength;
            FlushDeferredReceiveCompletion(*view, viewComplete);
            MaybeResumeQuicReceive(relay);

            if (viewComplete) {
                if (view->Fin) {
                    relay->TcpWriteShutdownQueued = true;
                }
                relay->PendingQuicReceives.pop_front();
            }
            if (Config.TcpWriteBurstBytes != 0 &&
                burstBytes >= Config.TcpWriteBurstBytes &&
                !relay->PendingQuicReceives.empty()) {
                TcpWriteBurstStops.fetch_add(1);
                ArmTcpWritable(relay, true);
                break;
            }
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            TcpWriteEagainCount.fetch_add(1);
            relay->TcpWriteEagainCount += 1;
            ArmTcpWritable(relay, true);
            break;
        }
        RecordError(RelayErrorKind::TcpWriteHard);
        const uint64_t savedErrno = static_cast<uint64_t>(errno);
        relay->LastTcpWriteErrno = savedErrno;
        LastTcpWriteErrno.store(savedErrno);
        LogLinuxRelayError(
            Config.WorkerIndex,
            "tcp_write_hard_error",
            relay->Id,
            relay->TcpFd,
            LastQuicSendStatus.load(),
            savedErrno,
            PendingTcpWriteBytes(relay->PendingTcpWrites),
            relay->PendingQuicReceiveBytes,
            relay->OutstandingQuicSends,
            relay->OutstandingQuicSendBytes);
        FatalRelayResets.fetch_add(1);
        AbortRelayAndRelease(relay, "tcp_write_hard_error", true);
        return;
    }

    if (relay->PendingQuicReceives.empty() && relay->TcpWriteShutdownQueued) {
        ::shutdown(relay->TcpFd, SHUT_WR);
        relay->TcpWriteShutdownQueued = false;
        relay->TcpWriteClosed = true;
        MaybeStopFullyClosedRelay(relay, "tcp_write_shutdown_complete");
    }
    MaybeResumeQuicReceive(relay);
    if (relay->PendingQuicReceives.empty() && relay->PendingTcpWrites.empty()) {
        ArmTcpWritable(relay, false);
    }
}

bool TqLinuxRelayWorker::DrainCompressedQuicReceiveView(
    RelayState* relay,
    TqPendingQuicReceive& view) {
    if (relay == nullptr || relay->Decompressor == nullptr) {
        return false;
    }

    while (true) {
        const bool hasInput = view.SliceIndex < view.Slices.size();
        const uint8_t* input = nullptr;
        size_t inputLength = 0;
        if (hasInput) {
            const auto& slice = view.Slices[view.SliceIndex];
            if (view.SliceOffset >= slice.Length) {
                ++view.SliceIndex;
                view.SliceOffset = 0;
                continue;
            }
            input = slice.Data + view.SliceOffset;
            inputLength = slice.Length - view.SliceOffset;
        }

        TqBufferAcquireFailure acquireFailure = TqBufferAcquireFailure::None;
        auto output = TqAllocateRelayBuffer(relay, Config.ReadChunkSize, &acquireFailure);
        if (!output) {
            RecordBufferAcquireFailure(RelayErrorKind::QuicReceiveTcpBufferAcquire, acquireFailure);
            ArmTcpWritable(relay, true);
            return false;
        }

        TqDecompressResult result{};
        ZstdDecompressCalls.fetch_add(1);
        if (!relay->Decompressor->DecompressInto(
                input,
                inputLength,
                output->Data(),
                output->Capacity(),
                &result)) {
            ZstdDecompressFailures.fetch_add(1);
            RecordError(RelayErrorKind::QuicReceiveDecompress);
            SetRelayStop(relay, "quic_receive_decompress_failed");
            return false;
        }
        if (result.InputConsumed > inputLength || result.OutputProduced > output->Capacity()) {
            ZstdDecompressFailures.fetch_add(1);
            RecordError(RelayErrorKind::QuicReceiveDecompress);
            SetRelayStop(relay, "quic_receive_decompress_invalid_output");
            return false;
        }
        if (result.NeedsMoreInput) {
            ZstdDecompressNeedInput.fetch_add(1);
        }
        if (result.NeedsMoreOutput) {
            ZstdDecompressNeedOutput.fetch_add(1);
        }

        if (result.InputConsumed > 0) {
            ZstdDecompressInputBytes.fetch_add(result.InputConsumed);
            view.PendingCompleteBytes += static_cast<uint64_t>(result.InputConsumed);
            view.CompletedLength += static_cast<uint64_t>(result.InputConsumed);
            relay->PendingQuicReceiveBytes =
                relay->PendingQuicReceiveBytes >= static_cast<uint64_t>(result.InputConsumed)
                    ? relay->PendingQuicReceiveBytes - static_cast<uint64_t>(result.InputConsumed)
                    : 0;
            view.SliceOffset += result.InputConsumed;
            while (view.SliceIndex < view.Slices.size()) {
                const auto& slice = view.Slices[view.SliceIndex];
                if (view.SliceOffset < slice.Length) {
                    break;
                }
                view.SliceOffset = 0;
                ++view.SliceIndex;
            }
            FlushDeferredReceiveCompletion(view, false);
            MaybeResumeQuicReceive(relay);
        }

        if (result.OutputProduced > 0) {
            output->SetLength(result.OutputProduced);
            uint8_t* data = output->Data();
            relay->PendingTcpWrites.push_back(
                TqBufferView{data, result.OutputProduced, std::move(output)});
            DecompressedTcpBytes.fetch_add(result.OutputProduced);
            ZstdDecompressOutputBytes.fetch_add(result.OutputProduced);
            FlushTcpWrites(relay);
            if (!relay->PendingTcpWrites.empty()) {
                return false;
            }
        }

        if (result.InputConsumed == 0 && result.OutputProduced == 0) {
            if (!hasInput && result.NeedsMoreInput) {
                break;
            }
            ZstdDecompressFailures.fetch_add(1);
            RecordError(RelayErrorKind::QuicReceiveDecompress);
            SetRelayStop(relay, "quic_receive_decompress_no_progress");
            return false;
        }

        if (view.SliceIndex >= view.Slices.size() &&
            result.OutputProduced == 0 &&
            result.NeedsMoreInput) {
            break;
        }
    }

    FlushDeferredReceiveCompletion(view, true);
    return true;
}

bool TqLinuxRelayWorker::EnqueueQuicReceiveForTest(
    int tcpFd,
    const uint8_t* data,
    size_t length,
    bool fin) {
    if (length > 0 && data == nullptr) {
        return false;
    }
    if (IsWorkerThread()) {
        return EnqueueQuicReceiveForTestLocal(tcpFd, data, length, fin);
    }

    EnqueueQuicReceiveForTestCommand command{};
    command.TcpFd = tcpFd;
    command.Fin = fin;
    if (length > 0) {
        command.Data.assign(data, data + length);
    }

    std::unique_lock<std::mutex> controlGuard(ControlLock);
    for (;;) {
        if (!Running.load() || std::this_thread::get_id() == WorkerThreadId) {
            return EnqueueQuicReceiveForTestLocal(
                tcpFd,
                command.Data.data(),
                command.Data.size(),
                fin);
        }

        TqLinuxRelayEvent event{};
        event.Type = TqLinuxRelayEventType::EnqueueQuicReceiveForTest;
        event.Control = &command;
        if (!Enqueue(std::move(event))) {
            controlGuard.unlock();
            Wake();
            std::this_thread::yield();
            controlGuard.lock();
            continue;
        }

        std::unique_lock<std::mutex> lock(command.Mutex);
        command.Cv.wait(lock, [&command]() {
            return command.Done;
        });
        return command.Result;
    }
}

bool TqLinuxRelayWorker::EnqueueQuicReceiveForTestLocal(
    int tcpFd,
    const uint8_t* data,
    size_t length,
    bool fin) {
    auto relay = FindRelayByFd(tcpFd);
    if (relay == nullptr) {
        return false;
    }
    if (!EnqueueQuicReceive(relay.get(), data, length, fin)) {
        return false;
    }
    FlushTcpWrites(relay.get());
    return true;
}

bool TqLinuxRelayWorker::FlushTcpWritableForTest(int tcpFd) {
    if (IsWorkerThread()) {
        return FlushTcpWritableForTestLocal(tcpFd);
    }

    std::unique_lock<std::mutex> controlGuard(ControlLock);
    for (;;) {
        if (!Running.load() || std::this_thread::get_id() == WorkerThreadId) {
            return FlushTcpWritableForTestLocal(tcpFd);
        }

        FlushTcpWritableForTestCommand command{};
        command.TcpFd = tcpFd;

        TqLinuxRelayEvent event{};
        event.Type = TqLinuxRelayEventType::FlushTcpWritableForTest;
        event.Control = &command;
        if (!Enqueue(std::move(event))) {
            controlGuard.unlock();
            Wake();
            std::this_thread::yield();
            controlGuard.lock();
            continue;
        }

        std::unique_lock<std::mutex> lock(command.Mutex);
        command.Cv.wait(lock, [&command]() {
            return command.Done;
        });
        return command.Result;
    }
}

bool TqLinuxRelayWorker::FlushTcpWritableForTestLocal(int tcpFd) {
    auto relay = FindRelayByFd(tcpFd);
    if (relay == nullptr) {
        return false;
    }
    FlushTcpWrites(relay.get());
    FlushDeferredQuicReceives(relay.get());
    FlushTcpWrites(relay.get());
    return true;
}

bool TqLinuxRelayWorker::DispatchTcpEventsForTest(uint64_t relayId, uint32_t events) {
    if (IsWorkerThread()) {
        return DispatchTcpEventsForTestLocal(relayId, events);
    }

    std::unique_lock<std::mutex> controlGuard(ControlLock);
    for (;;) {
        if (!Running.load() || std::this_thread::get_id() == WorkerThreadId) {
            return DispatchTcpEventsForTestLocal(relayId, events);
        }

        DispatchTcpEventsForTestCommand command{};
        command.RelayId = relayId;
        command.Events = events;

        TqLinuxRelayEvent event{};
        event.Type = TqLinuxRelayEventType::DispatchTcpEventsForTest;
        event.Control = &command;
        if (!Enqueue(std::move(event))) {
            controlGuard.unlock();
            Wake();
            std::this_thread::yield();
            controlGuard.lock();
            continue;
        }

        std::unique_lock<std::mutex> lock(command.Mutex);
        command.Cv.wait(lock, [&command]() {
            return command.Done;
        });
        return command.Result;
    }
}

bool TqLinuxRelayWorker::DispatchTcpEventsForTestLocal(uint64_t relayId, uint32_t events) {
    if (FindRelayById(relayId) == nullptr) {
        return false;
    }
    ProcessTcpEvents(relayId, events);
    return true;
}

bool TqLinuxRelayWorker::EnqueueQuicReceive(
    RelayState* relay,
    const uint8_t* data,
    size_t length,
    bool fin) {
    if (relay == nullptr || (length > 0 && data == nullptr)) {
        return false;
    }

    const uint8_t* writeData = data;
    size_t writeLength = length;
    if (relay->Decompressor != nullptr && relay->CompressAlgo != TqCompressAlgo::None) {
        relay->DecompressionOutput.clear();
        ZstdDecompressCalls.fetch_add(1);
        if (!relay->Decompressor->Decompress(data, length, relay->DecompressionOutput)) {
            ZstdDecompressFailures.fetch_add(1);
            RecordError(RelayErrorKind::QuicReceiveDecompress);
            return false;
        }
        DecompressedTcpBytes.fetch_add(relay->DecompressionOutput.size());
        ZstdDecompressInputBytes.fetch_add(length);
        ZstdDecompressOutputBytes.fetch_add(relay->DecompressionOutput.size());
        writeData = relay->DecompressionOutput.data();
        writeLength = relay->DecompressionOutput.size();
    }

    size_t offset = 0;
    while (offset < writeLength) {
        TqBufferAcquireFailure acquireFailure = TqBufferAcquireFailure::None;
        auto buffer = TqAllocateRelayBuffer(relay, Config.ReadChunkSize, &acquireFailure);
        if (!buffer) {
            RecordBufferAcquireFailure(
                RelayErrorKind::QuicReceiveTcpBufferAcquire, acquireFailure);
            return false;
        }
        const size_t chunk = std::min(buffer->Capacity(), writeLength - offset);
        std::memcpy(buffer->Data(), writeData + offset, chunk);
        buffer->SetLength(chunk);
        uint8_t* data = buffer->Data();
        relay->PendingTcpWrites.push_back(TqBufferView{data, chunk, std::move(buffer)});
        offset += chunk;
    }
    if (fin) {
        relay->TcpWriteShutdownQueued = true;
    }
    return true;
}

void TqLinuxRelayWorker::FlushTcpWrites(RelayState* relay) {
    if (relay == nullptr || relay->Closing) {
        return;
    }

    uint64_t burstBytes = 0;
    while (!relay->PendingTcpWrites.empty()) {
        if (Config.TcpWriteBurstBytes != 0 && burstBytes >= Config.TcpWriteBurstBytes) {
            TcpWriteBurstStops.fetch_add(1);
            ArmTcpWritable(relay, true);
            break;
        }

        std::vector<iovec> iov;
        iov.reserve(Config.MaxIov);
        uint64_t attemptedBytes = 0;
        uint64_t maxWriteBytes = Config.TcpWriteMaxBytes;
        if (Config.TcpWriteBurstBytes != 0) {
            const uint64_t remainingBurst = Config.TcpWriteBurstBytes - burstBytes;
            maxWriteBytes = maxWriteBytes == 0
                ? remainingBurst
                : std::min(maxWriteBytes, remainingBurst);
        }
        for (const auto& view : relay->PendingTcpWrites) {
            if (iov.size() >= Config.MaxIov ||
                (maxWriteBytes != 0 && attemptedBytes >= maxWriteBytes)) {
                break;
            }
            uint64_t length = view.Len;
            if (maxWriteBytes != 0 && attemptedBytes + length > maxWriteBytes) {
                length = maxWriteBytes - attemptedBytes;
            }
            if (length == 0) {
                break;
            }
            iovec item{};
            item.iov_base = view.Data;
            item.iov_len = static_cast<size_t>(length);
            iov.push_back(item);
            attemptedBytes += length;
        }

        if (iov.empty()) {
            break;
        }
        RecordTcpWriteAttempt(attemptedBytes);
        const ssize_t sent = WritevNoSignal(relay->TcpFd, iov.data(), static_cast<int>(iov.size()));
        if (sent > 0) {
            size_t remaining = static_cast<size_t>(sent);
            burstBytes += static_cast<uint64_t>(sent);
            relay->TcpWriteBytes += static_cast<uint64_t>(sent);
            TcpWriteBytes.fetch_add(static_cast<uint64_t>(sent));
            TcpWriteBatches.fetch_add(1);
            TcpWriteSendmsgCalls.fetch_add(1);
            UpdateAtomicMax(MaxTcpWriteSendmsgBytes, static_cast<uint64_t>(sent));
            RecordTcpWriteReturned(static_cast<uint64_t>(sent));
            if (static_cast<uint64_t>(sent) < attemptedBytes) {
                TcpWritePartialCount.fetch_add(1);
            }
            uint64_t previous = MaxTcpWriteIovUsed.load();
            while (previous < iov.size() &&
                   !MaxTcpWriteIovUsed.compare_exchange_weak(previous, iov.size())) {
            }
            while (remaining > 0 && !relay->PendingTcpWrites.empty()) {
                auto& front = relay->PendingTcpWrites.front();
                if (remaining >= front.Len) {
                    remaining -= front.Len;
                    relay->PendingTcpWrites.pop_front();
                } else {
                    front.Data += remaining;
                    front.Len -= remaining;
                    remaining = 0;
                }
            }
            if (Config.TcpWriteBurstBytes != 0 &&
                burstBytes >= Config.TcpWriteBurstBytes &&
                !relay->PendingTcpWrites.empty()) {
                TcpWriteBurstStops.fetch_add(1);
                ArmTcpWritable(relay, true);
                break;
            }
            continue;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            TcpWriteEagainCount.fetch_add(1);
            relay->TcpWriteEagainCount += 1;
            ArmTcpWritable(relay, true);
            break;
        }
        RecordError(RelayErrorKind::TcpWriteHard);
        const uint64_t savedErrno = static_cast<uint64_t>(errno);
        relay->LastTcpWriteErrno = savedErrno;
        LastTcpWriteErrno.store(savedErrno);
        FatalRelayResets.fetch_add(1);
        LogLinuxRelayError(
            Config.WorkerIndex,
            "tcp_write_hard_error",
            relay->Id,
            relay->TcpFd,
            LastQuicSendStatus.load(),
            savedErrno,
            PendingTcpWriteBytes(relay->PendingTcpWrites),
            relay->PendingQuicReceiveBytes,
            relay->OutstandingQuicSends,
            relay->OutstandingQuicSendBytes);
        AbortRelayAndRelease(relay, "tcp_write_hard_error", true);
        return;
    }

    if (relay->PendingTcpWrites.empty() && relay->TcpWriteShutdownQueued) {
        ::shutdown(relay->TcpFd, SHUT_WR);
        relay->TcpWriteShutdownQueued = false;
        relay->TcpWriteClosed = true;
        MaybeStopFullyClosedRelay(relay, "tcp_write_shutdown_complete");
    }
    if (relay->PendingTcpWrites.empty() && relay->PendingQuicReceives.empty()) {
        ArmTcpWritable(relay, false);
    }
}

void TqLinuxRelayWorker::ArmTcpReadable(RelayState* relay, bool enabled) {
    if (relay == nullptr || relay->TcpFd < 0 || EpollFd < 0 || relay->TcpReadArmed == enabled) {
        return;
    }
    relay->TcpReadArmed = enabled;
    if (!enabled) {
        ReadDisabledCount.fetch_add(1);
    }
    UpdateTcpInterest(relay);
}

void TqLinuxRelayWorker::ArmTcpWritable(RelayState* relay, bool enabled) {
    if (relay == nullptr || relay->TcpFd < 0 || EpollFd < 0 || relay->TcpWriteArmed == enabled) {
        return;
    }
    relay->TcpWriteArmed = enabled;
    UpdateTcpInterest(relay);
}

void TqLinuxRelayWorker::UpdateTcpInterest(RelayState* relay) {
    if (relay == nullptr || relay->TcpFd < 0 || EpollFd < 0) {
        return;
    }
    epoll_event event{};
    event.events = EPOLLRDHUP | EPOLLERR;
    if (relay->TcpReadArmed) {
        event.events |= EPOLLIN;
    }
    if (relay->TcpWriteArmed) {
        event.events |= EPOLLOUT;
    }
    event.data.u64 = relay->Id;
    (void)::epoll_ctl(EpollFd, EPOLL_CTL_MOD, relay->TcpFd, &event);
}

void TqLinuxRelayWorker::ProcessTcpEvents(uint64_t relayId, uint32_t events) {
    auto relay = FindRelayById(relayId);
    if (relay == nullptr || relay->Closing) {
        return;
    }
    if ((events & EPOLLERR) != 0) {
        const int socketError = GetSocketError(relay->TcpFd);
        TqRelayDebugLog(
            "event=linux_relay_tcp_so_error worker=%u relay=%llu fd=%d so_error=%d",
            Config.WorkerIndex,
            static_cast<unsigned long long>(relayId),
            relay->TcpFd,
            socketError);
        if (socketError != 0) {
            relay->LastTcpWriteErrno = static_cast<uint64_t>(socketError);
            LastTcpWriteErrno.store(static_cast<uint64_t>(socketError));
            RecordError(RelayErrorKind::TcpWriteHard);
            FatalRelayResets.fetch_add(1);
            LogLinuxRelayError(
                Config.WorkerIndex,
                "tcp_socket_error",
                relay->Id,
                relay->TcpFd,
                LastQuicSendStatus.load(),
                static_cast<uint64_t>(socketError),
                PendingTcpWriteBytes(relay->PendingTcpWrites),
                relay->PendingQuicReceiveBytes,
                relay->OutstandingQuicSends,
                relay->OutstandingQuicSendBytes);
            AbortRelayAndRelease(relay.get(), "tcp_socket_error", true);
            return;
        }
    }
    if ((events & EPOLLOUT) != 0) {
        relay->EpollOutEvents += 1;
        FlushTcpWrites(relay.get());
        FlushDeferredQuicReceives(relay.get());
        FlushTcpWrites(relay.get());
    }
    if ((events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0) {
        DrainTcpReadable(relay.get());
    }
}

void TqLinuxRelayWorker::ProcessQuicReceiveEvent(TqLinuxRelayEvent& event) {
    auto relay = FindRelayById(event.RelayId);
    if (relay == nullptr) {
        AbandonOrphanedEventBuffers(event);
        return;
    }
    if (relay->Closing) {
        return;
    }
    const bool needsDecompress =
        relay->Decompressor != nullptr && relay->CompressAlgo != TqCompressAlgo::None;
    if (needsDecompress) {
        if (!event.Buffers.empty()) {
            for (size_t i = 0; i < event.Buffers.size(); ++i) {
                auto& buffer = event.Buffers[i];
                if (!buffer) {
                    continue;
                }
                const bool fin = event.Fin && i + 1 == event.Buffers.size();
                if (!EnqueueQuicReceive(relay.get(), buffer->Data(), buffer->Length(), fin)) {
                    SetRelayStop(relay.get(), "quic_receive_enqueue_failed");
                    return;
                }
            }
            if (event.Fin && !relay->TcpWriteShutdownQueued) {
                relay->TcpWriteShutdownQueued = true;
            }
        } else {
            const uint8_t* data = nullptr;
            size_t length = 0;
            if (event.Buffer) {
                data = event.Buffer->Data();
                length = event.Length;
            }
            if (!EnqueueQuicReceive(relay.get(), data, length, event.Fin)) {
                SetRelayStop(relay.get(), "quic_receive_enqueue_failed");
                return;
            }
        }
    } else {
        if (!event.Buffers.empty()) {
            for (auto& buffer : event.Buffers) {
                if (buffer && buffer->Length() > 0) {
                    uint8_t* data = buffer->Data();
                    const size_t length = buffer->Length();
                    relay->PendingTcpWrites.push_back(
                        TqBufferView{data, length, std::move(buffer)});
                }
            }
        } else if (event.Buffer && event.Length > 0) {
            uint8_t* data = event.Buffer->Data();
            relay->PendingTcpWrites.push_back(
                TqBufferView{data, event.Length, std::move(event.Buffer)});
        }
        if (event.Fin) {
            relay->TcpWriteShutdownQueued = true;
        }
    }
    FlushTcpWrites(relay.get());
}

QUIC_STATUS TqLinuxRelayWorker::DispatchStreamEventForTest(
    MsQuicStream* stream,
    QUIC_STREAM_EVENT* event) {
    return OnStreamEvent(stream, event);
}

TqLinuxRelayWorkerSnapshot TqLinuxRelayWorker::Snapshot() const {
    auto command = std::make_shared<SnapshotCommand>();

    for (uint32_t attempt = 0; attempt < kLinuxRelayControlEnqueueRetries; ++attempt) {
        bool enqueued = false;
        {
            const auto current = std::this_thread::get_id();
            auto controlGuard = AcquireControlLockForMetrics();
            if (!Running.load(std::memory_order_acquire) || current == WorkerThreadId) {
                return SnapshotLocal();
            }

            TqLinuxRelayEvent event{};
            event.Type = TqLinuxRelayEventType::Snapshot;
            event.Control = command.get();
            event.ControlOwner = command;
            enqueued = const_cast<TqLinuxRelayWorker*>(this)->Enqueue(std::move(event));
        }

        if (!enqueued) {
            ControlCommandEnqueueFailures.fetch_add(1, std::memory_order_relaxed);
            const_cast<TqLinuxRelayWorker*>(this)->Wake();
            std::this_thread::yield();
            continue;
        }

        if (!WaitSnapshotCommand(*command)) {
            return {};
        }
        command->Result.ControlLockWaitNanos = ControlLockWaitNanos.load();
        command->Result.ControlLockAcquireCount = ControlLockAcquireCount.load();
        command->Result.ControlCommandWaitNanos = ControlCommandWaitNanos.load();
        command->Result.ControlCommandWaitCount = ControlCommandWaitCount.load();
        command->Result.ControlCommandTimeouts = ControlCommandTimeouts.load();
        command->Result.ControlCommandEnqueueFailures = ControlCommandEnqueueFailures.load();
        command->Result.SnapshotCommandWaitNanos = SnapshotCommandWaitNanos.load();
        command->Result.SnapshotCommandWaitCount = SnapshotCommandWaitCount.load();
        command->Result.SnapshotCommandTimeouts = SnapshotCommandTimeouts.load();
        return command->Result;
    }
    return {};
}

TqLinuxRelayWorkerSnapshot TqLinuxRelayWorker::SnapshotLocal() const {
    TqLinuxRelayWorkerSnapshot snapshot{};
    snapshot.WorkerIndex = Config.WorkerIndex;
    snapshot.EventsProcessed = EventsProcessed.load();
    snapshot.WakeupWrites = WakeupWrites.load();
    snapshot.PendingEvents = EventQueue.SizeApprox();
    snapshot.EventQueueCapacity = EventQueue.Capacity();
    const TqLinuxRelayEventQueueStats queueStats = EventQueue.Stats();
    snapshot.EventQueuePushCasRetries = queueStats.PushCasRetries;
    snapshot.EventQueuePopCasRetries = queueStats.PopCasRetries;
    snapshot.TcpReadBatches = TcpReadBatches.load();
    snapshot.TcpReadBytes = TcpReadBytes.load();
    snapshot.QuicSendOperations = QuicSendOperations.load();
    snapshot.MaxTcpReadIovUsed = MaxTcpReadIovUsed.load();
    snapshot.TcpWriteBatches = TcpWriteBatches.load();
    snapshot.TcpWriteBytes = TcpWriteBytes.load();
    snapshot.MaxTcpWriteIovUsed = MaxTcpWriteIovUsed.load();
    snapshot.TcpWriteSendmsgCalls = TcpWriteSendmsgCalls.load();
    snapshot.TcpWriteAttemptBytes = TcpWriteAttemptBytes.load();
    snapshot.MaxTcpWriteAttemptBytes = MaxTcpWriteAttemptBytes.load();
    snapshot.MaxTcpWriteSendmsgBytes = MaxTcpWriteSendmsgBytes.load();
    snapshot.TcpWriteAttemptBytesLe64K = TcpWriteAttemptBytesLe64K.load();
    snapshot.TcpWriteAttemptBytesLe256K = TcpWriteAttemptBytesLe256K.load();
    snapshot.TcpWriteAttemptBytesLe1M = TcpWriteAttemptBytesLe1M.load();
    snapshot.TcpWriteAttemptBytesLe4M = TcpWriteAttemptBytesLe4M.load();
    snapshot.TcpWriteAttemptBytesGt4M = TcpWriteAttemptBytesGt4M.load();
    snapshot.TcpWriteReturnedBytesLe64K = TcpWriteReturnedBytesLe64K.load();
    snapshot.TcpWriteReturnedBytesLe256K = TcpWriteReturnedBytesLe256K.load();
    snapshot.TcpWriteReturnedBytesLe1M = TcpWriteReturnedBytesLe1M.load();
    snapshot.TcpWriteReturnedBytesLe4M = TcpWriteReturnedBytesLe4M.load();
    snapshot.TcpWriteReturnedBytesGt4M = TcpWriteReturnedBytesGt4M.load();
    snapshot.TcpWriteEagainCount = TcpWriteEagainCount.load();
    snapshot.TcpWritePartialCount = TcpWritePartialCount.load();
    snapshot.TcpWriteBurstStops = TcpWriteBurstStops.load();
    snapshot.ReadDisabledCount = ReadDisabledCount.load();
    snapshot.StreamLookupScanCount = StreamLookupScanCount.load();
    snapshot.CompressedTcpBytes = CompressedTcpBytes.load();
    snapshot.DecompressedTcpBytes = DecompressedTcpBytes.load();
    snapshot.ZstdDecompressInputBytes = ZstdDecompressInputBytes.load();
    snapshot.ZstdDecompressOutputBytes = ZstdDecompressOutputBytes.load();
    snapshot.ZstdDecompressCalls = ZstdDecompressCalls.load();
    snapshot.ZstdDecompressNeedInput = ZstdDecompressNeedInput.load();
    snapshot.ZstdDecompressNeedOutput = ZstdDecompressNeedOutput.load();
    snapshot.ZstdDecompressFailures = ZstdDecompressFailures.load();
    snapshot.DeferredReceiveCompleteBytes = DeferredReceiveCompleteBytes.load();
    snapshot.DeferredReceiveCompletes = DeferredReceiveCompletes.load();
    snapshot.DeferredReceiveCompletionFlushes = DeferredReceiveCompletionFlushes.load();
    snapshot.MaxPendingQuicReceiveBytes = MaxPendingQuicReceiveBytesObserved.load();
    snapshot.MaxPendingQuicReceiveQueue = MaxPendingQuicReceiveQueueObserved.load();
    snapshot.QuicReceiveViewCount = QuicReceiveViewCount.load();
    snapshot.QuicReceiveViewBytes = QuicReceiveViewBytes.load();
    snapshot.MaxQuicReceiveViewBytes = MaxQuicReceiveViewBytes.load();
    snapshot.MaxQuicReceiveViewSlices = MaxQuicReceiveViewSlices.load();
    snapshot.QuicReceiveViewBytesLe64K = QuicReceiveViewBytesLe64K.load();
    snapshot.QuicReceiveViewBytesLe256K = QuicReceiveViewBytesLe256K.load();
    snapshot.QuicReceiveViewBytesLe1M = QuicReceiveViewBytesLe1M.load();
    snapshot.QuicReceiveViewBytesLe4M = QuicReceiveViewBytesLe4M.load();
    snapshot.QuicReceiveViewBytesGt4M = QuicReceiveViewBytesGt4M.load();
    snapshot.QuicReceiveViewSlices1 = QuicReceiveViewSlices1.load();
    snapshot.QuicReceiveViewSlices2To4 = QuicReceiveViewSlices2To4.load();
    snapshot.QuicReceiveViewSlices5To16 = QuicReceiveViewSlices5To16.load();
    snapshot.QuicReceiveViewSlicesGt16 = QuicReceiveViewSlicesGt16.load();
    snapshot.QuicReceivePausedCount = QuicReceivePausedCount.load();
    snapshot.QuicReceiveResumedCount = QuicReceiveResumedCount.load();
    snapshot.QuicReceiveViewBackpressureQueued = QuicReceiveViewBackpressureQueued.load();
    snapshot.ControlLockWaitNanos = ControlLockWaitNanos.load();
    snapshot.ControlLockAcquireCount = ControlLockAcquireCount.load();
    snapshot.ControlCommandWaitNanos = ControlCommandWaitNanos.load();
    snapshot.ControlCommandWaitCount = ControlCommandWaitCount.load();
    snapshot.ControlCommandTimeouts = ControlCommandTimeouts.load();
    snapshot.ControlCommandEnqueueFailures = ControlCommandEnqueueFailures.load();
    snapshot.SnapshotCommandWaitNanos = SnapshotCommandWaitNanos.load();
    snapshot.SnapshotCommandWaitCount = SnapshotCommandWaitCount.load();
    snapshot.SnapshotCommandTimeouts = SnapshotCommandTimeouts.load();
    snapshot.Errors = Errors.load();
    snapshot.EventQueueFullErrors = EventQueueFullErrors.load();
    snapshot.TcpReadBufferAcquireFailures = TcpReadBufferAcquireFailures.load();
    snapshot.TcpReadBufferAcquirePendingBudgetFailures =
        TcpReadBufferAcquirePendingBudgetFailures.load();
    snapshot.TcpReadBufferAcquireAllocFailures = TcpReadBufferAcquireAllocFailures.load();
    snapshot.TcpToQuicCompressFailures = TcpToQuicCompressFailures.load();
    snapshot.TcpToQuicCompressUpdateFailures = TcpToQuicCompressUpdateFailures.load();
    snapshot.TcpToQuicCompressFlushFailures = TcpToQuicCompressFlushFailures.load();
    snapshot.TcpToQuicBufferAcquireFailures = TcpToQuicBufferAcquireFailures.load();
    snapshot.TcpToQuicBufferAcquirePendingBudgetFailures =
        TcpToQuicBufferAcquirePendingBudgetFailures.load();
    snapshot.TcpToQuicBufferAcquireAllocFailures =
        TcpToQuicBufferAcquireAllocFailures.load();
    snapshot.QuicSendFailures = QuicSendFailures.load();
    snapshot.QuicSendBufferTooLargeFailures = QuicSendBufferTooLargeFailures.load();
    snapshot.QuicSendOperationAllocFailures = QuicSendOperationAllocFailures.load();
    snapshot.QuicSendApiFailures = QuicSendApiFailures.load();
    snapshot.QuicSendBackpressureEvents = QuicSendBackpressureEvents.load();
    snapshot.QuicSendFatalErrors = QuicSendFatalErrors.load();
    snapshot.QuicReceiveViewFailures = QuicReceiveViewFailures.load();
    snapshot.QuicReceiveViewAllocFailures = QuicReceiveViewAllocFailures.load();
    snapshot.QuicReceiveViewNullBufferFailures = QuicReceiveViewNullBufferFailures.load();
    snapshot.QuicReceiveViewEmptyFailures = QuicReceiveViewEmptyFailures.load();
    snapshot.QuicReceiveViewEnqueueFailures = QuicReceiveViewEnqueueFailures.load();
    snapshot.QuicReceiveDecompressFailures = QuicReceiveDecompressFailures.load();
    snapshot.QuicReceiveTcpBufferAcquireFailures = QuicReceiveTcpBufferAcquireFailures.load();
    snapshot.QuicReceiveTcpBufferAcquirePendingBudgetFailures =
        QuicReceiveTcpBufferAcquirePendingBudgetFailures.load();
    snapshot.QuicReceiveTcpBufferAcquireAllocFailures =
        QuicReceiveTcpBufferAcquireAllocFailures.load();
    snapshot.TcpWriteHardErrors = TcpWriteHardErrors.load();
    snapshot.LastTcpWriteErrno = LastTcpWriteErrno.load();
    snapshot.TcpReadHardErrors = TcpReadHardErrors.load();
    snapshot.LastTcpReadErrno = LastTcpReadErrno.load();
    snapshot.FatalRelayResets = FatalRelayResets.load();
    snapshot.LastQuicSendStatus = LastQuicSendStatus.load();
    snapshot.EventProducerThreadsObserved = EventProducerThreadCount.load();
    snapshot.MultipleEventProducerThreadsObserved =
        MultipleEventProducerThreadsObserved.load(std::memory_order_acquire);

    snapshot.ActiveRelays = Relays.size();
    snapshot.MaxWorkerActiveRelays = snapshot.ActiveRelays;
    snapshot.ActiveRelayStates.reserve(Relays.size());
    bool hasHotRelay = false;
    for (const auto& relay : Relays) {
        uint64_t relayPendingBytes =
            relay->PendingBufferBytes.load(std::memory_order_relaxed);
        snapshot.BufferAcquireCount +=
            relay->AllocateCount.load(std::memory_order_relaxed);
        snapshot.RelayBufferBytesInUse += relayPendingBytes;
        if (relay->TcpFd >= 0) {
            ++snapshot.ActiveTcpRelays;
        }
        if (relay->SinkQuicReceives) {
            ++snapshot.ActiveSinkRelays;
        }
        if (relay->EnableQuicSends) {
            ++snapshot.ActiveQuicSendRelays;
        }
        if (relay->TcpReadArmed) {
            ++snapshot.TcpReadArmedRelays;
        } else if (relay->TcpFd >= 0) {
            ++snapshot.TcpReadDisabledRelays;
        }
        if (relay->TcpWriteArmed) {
            ++snapshot.TcpWriteArmedRelays;
        }
        if (relay->Closing) {
            ++snapshot.ClosingRelays;
        }
        if (relay->TcpReadClosed) {
            ++snapshot.TcpReadClosedRelays;
        }
        if (relay->TcpWriteShutdownQueued) {
            ++snapshot.TcpWriteShutdownQueuedRelays;
        }
        snapshot.OutstandingQuicSends += relay->OutstandingQuicSends;
        snapshot.OutstandingQuicSendBytes += relay->OutstandingQuicSendBytes;
        snapshot.OutstandingQuicSends += relay->PendingQuicSendRetries.size();
        uint64_t retryBytes = 0;
        for (const auto& retry : relay->PendingQuicSendRetries) {
            if (retry != nullptr) {
                retryBytes += retry->TotalBytes;
            }
        }
        snapshot.OutstandingQuicSendBytes += retryBytes;
        relayPendingBytes += retryBytes;
        snapshot.MaxBufferedQuicSendBytes += CurrentRelayIdealSendBytes(relay.get());
        snapshot.PendingTcpWriteQueue += relay->PendingTcpWrites.size();
        const uint64_t pendingTcpWriteBytes = PendingTcpWriteBytes(relay->PendingTcpWrites);
        relayPendingBytes += pendingTcpWriteBytes;
        snapshot.PendingTcpWriteBytes += pendingTcpWriteBytes;
        uint64_t callbackPendingDepth = 0;
        {
            std::lock_guard<std::mutex> guard(relay->CallbackPendingQuicReceiveLock);
            callbackPendingDepth = relay->CallbackPendingQuicReceives.size();
        }
        relayPendingBytes += relay->PendingQuicReceiveBytes;
        snapshot.CurrentPendingQuicReceiveBytes += relay->PendingQuicReceiveBytes;
        snapshot.CurrentPendingQuicReceiveQueue += relay->PendingQuicReceives.size();
        snapshot.PendingBytes += relayPendingBytes;
        snapshot.MaxRelayPendingQuicReceiveBytes = std::max(
            snapshot.MaxRelayPendingQuicReceiveBytes,
            relay->PendingQuicReceiveBytes);
        snapshot.MaxRelayPendingQuicReceiveQueue = std::max<uint64_t>(
            snapshot.MaxRelayPendingQuicReceiveQueue,
            relay->PendingQuicReceives.size());
        snapshot.MaxRelayTcpWriteEagainCount = std::max(
            snapshot.MaxRelayTcpWriteEagainCount,
            relay->TcpWriteEagainCount);
        if (!hasHotRelay ||
            relay->PendingQuicReceiveBytes > snapshot.HotRelayPendingQuicReceiveBytes ||
            (relay->PendingQuicReceiveBytes == snapshot.HotRelayPendingQuicReceiveBytes &&
             relay->TcpWriteEagainCount > snapshot.HotRelayTcpWriteEagainCount)) {
            hasHotRelay = true;
            snapshot.HotRelayId = relay->Id;
            snapshot.HotRelayWorkerIndex = Config.WorkerIndex;
            snapshot.HotRelayTcpFd = relay->TcpFd;
            snapshot.HotRelayPendingQuicReceiveBytes = relay->PendingQuicReceiveBytes;
            snapshot.HotRelayPendingQuicReceiveQueue = relay->PendingQuicReceives.size();
            snapshot.HotRelayTcpWriteBytes = relay->TcpWriteBytes;
            snapshot.HotRelayTcpReadBytes = relay->TcpReadBytes;
            snapshot.HotRelayOutstandingQuicSends = relay->OutstandingQuicSends;
            snapshot.HotRelayOutstandingQuicSendBytes = relay->OutstandingQuicSendBytes;
            snapshot.HotRelayPendingQuicSendRetries = relay->PendingQuicSendRetries.size();
            snapshot.HotRelayIdealSendBytes = CurrentRelayIdealSendBytes(relay.get());
            snapshot.HotRelayTcpWriteEagainCount = relay->TcpWriteEagainCount;
            snapshot.HotRelayEpollOutEvents = relay->EpollOutEvents;
            snapshot.HotRelayTcpReadArmed = relay->TcpReadArmed;
            snapshot.HotRelayTcpWriteArmed = relay->TcpWriteArmed;
            snapshot.HotRelayLocalAddress = GetSocketNameString(relay->TcpFd, false);
            snapshot.HotRelayPeerAddress = GetSocketNameString(relay->TcpFd, true);
        }
        TqLinuxRelayActiveSnapshot active{};
        active.WorkerIndex = Config.WorkerIndex;
        active.RelayId = relay->Id;
        active.TcpFd = relay->TcpFd;
        active.InFlightQuicSends = static_cast<uint32_t>(
            relay->OutstandingQuicSends + relay->PendingQuicSendRetries.size());
        active.PendingQuicReceiveBytes = relay->PendingQuicReceiveBytes;
        active.PendingQuicReceiveQueueDepth = relay->PendingQuicReceives.size();
        active.CallbackPendingQuicReceiveDepth = callbackPendingDepth;
        active.OutstandingQuicSendBytes = relay->OutstandingQuicSendBytes + retryBytes;
        active.PendingQuicSendRetries = relay->PendingQuicSendRetries.size();
        active.IdealSendBytes = CurrentRelayIdealSendBytes(relay.get());
        active.TcpReadBytes = relay->TcpReadBytes;
        active.TcpWriteBytes = relay->TcpWriteBytes;
        active.TcpWriteEagainCount = relay->TcpWriteEagainCount;
        active.EpollOutEvents = relay->EpollOutEvents;
        active.PendingTcpWriteQueueDepth = relay->PendingTcpWrites.size();
        active.PendingTcpWriteBytes = pendingTcpWriteBytes;
        active.RelayBufferBytesInUse =
            relay->PendingBufferBytes.load(std::memory_order_relaxed) + retryBytes +
            pendingTcpWriteBytes;
        active.LastTcpWriteErrno = relay->LastTcpWriteErrno;
        active.Closing = relay->Closing;
        active.TcpReadClosed = relay->TcpReadClosed;
        active.TcpWriteClosed = relay->TcpWriteClosed;
        active.TcpReadArmed = relay->TcpReadArmed;
        active.TcpWriteArmed = relay->TcpWriteArmed;
        active.TcpReadPausedByQuicBacklog = relay->TcpReadPausedByQuicBacklog;
        active.QuicSendFinSubmitted = relay->QuicSendFinSubmitted;
        active.QuicSendFinCompleted = relay->QuicSendFinCompleted;
        active.StopPublished =
            relay->Handle != nullptr && relay->Handle->Stop.load(std::memory_order_acquire);
        active.StreamDetached = relay->StreamBinding == nullptr;
        active.LocalAddress = GetSocketNameString(relay->TcpFd, false);
        active.PeerAddress = GetSocketNameString(relay->TcpFd, true);
        snapshot.ActiveRelayStates.push_back(std::move(active));
    }
    snapshot.SnapshotActiveRelaysScanned = snapshot.ActiveRelayStates.size();
    snapshot.MaxWorkerPendingBytes = snapshot.PendingBytes;
    return snapshot;
}

void TqLinuxRelayWorker::Run() {
    epoll_event events[16]{};
    while (Running.load()) {
        const int count = ::epoll_wait(EpollFd, events, 16, 100);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            continue;
        }
        for (int i = 0; i < count; ++i) {
            if (events[i].data.fd == WakeFd) {
                uint64_t value = 0;
                while (::read(WakeFd, &value, sizeof(value)) > 0) {
                }
                DrainEvents(Config.EventBudget);
            } else {
                ProcessTcpEvents(events[i].data.u64, events[i].events);
            }
        }
    }
}

QUIC_STATUS QUIC_API TqLinuxRelayWorker::StreamCallback(
    _In_ MsQuicStream* stream,
    _In_opt_ void* context,
    _Inout_ QUIC_STREAM_EVENT* event) noexcept {
    auto* binding = static_cast<StreamRelayBinding*>(context);
    if (binding == nullptr || binding->Worker == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    return binding->Worker->OnStreamEventWithBinding(stream, event, binding);
}

QUIC_STATUS TqLinuxRelayWorker::OnStreamEvent(
    MsQuicStream* stream,
    QUIC_STREAM_EVENT* event) noexcept {
    auto* binding = static_cast<StreamRelayBinding*>(stream != nullptr ? stream->Context : nullptr);
    return OnStreamEventWithBinding(stream, event, binding);
}

QUIC_STATUS TqLinuxRelayWorker::OnStreamEventWithBinding(
    MsQuicStream* stream,
    QUIC_STREAM_EVENT* event,
    StreamRelayBinding* binding) noexcept {
    if (event == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_SEND_COMPLETE) {
        TqLinuxRelayEvent queued{};
        queued.Type = TqLinuxRelayEventType::QuicSendComplete;
        queued.Value = reinterpret_cast<uintptr_t>(event->SEND_COMPLETE.ClientContext);
        if (!Enqueue(std::move(queued))) {
            return QUIC_STATUS_OUT_OF_MEMORY;
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (binding == nullptr || binding->Worker != this) {
        return QUIC_STATUS_SUCCESS;
    }
    binding->CallbackRefs.fetch_add(1, std::memory_order_acq_rel);
    struct CallbackRefGuard {
        StreamRelayBinding* Binding{nullptr};
        ~CallbackRefGuard() {
            Binding->CallbackRefs.fetch_sub(1, std::memory_order_acq_rel);
        }
    } guard{binding};
    RelayState* relay = binding->Relay.load(std::memory_order_acquire);
    if (binding->Closing.load(std::memory_order_acquire) || relay == nullptr) {
        return QUIC_STATUS_SUCCESS;
    }
    const uint64_t relayId = relay->Id;
    if (event->Type == QUIC_STREAM_EVENT_IDEAL_SEND_BUFFER_SIZE) {
        TqLinuxRelayEvent queued{};
        queued.Type = TqLinuxRelayEventType::QuicIdealSendBuffer;
        queued.RelayId = relayId;
        queued.Value = event->IDEAL_SEND_BUFFER_SIZE.ByteCount;
        if (!Enqueue(std::move(queued))) {
            return QUIC_STATUS_OUT_OF_MEMORY;
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_RECEIVE) {
        const bool fin = (event->RECEIVE.Flags & QUIC_RECEIVE_FLAG_FIN) != 0;
        if (TqIsMsQuicFakeFinReceive(
                event->RECEIVE.AbsoluteOffset,
                event->RECEIVE.TotalBufferLength,
                event->RECEIVE.BufferCount,
                event->RECEIVE.Flags)) {
            TraceRelayStreamEvent(
                relay,
                "receive_fake_fin",
                0,
                0,
                event->RECEIVE.AbsoluteOffset,
                event->RECEIVE.TotalBufferLength,
                event->RECEIVE.BufferCount,
                static_cast<uint32_t>(event->RECEIVE.Flags),
                true);
            assert(false && "MsQuic delivered FIN-only receive without known final size");
            std::abort();
        }
        if (relay->Closing) {
            return QUIC_STATUS_SUCCESS;
        }
        if (fin) {
            TraceRelayStreamEvent(
                relay,
                "receive_fin",
                0,
                0,
                event->RECEIVE.AbsoluteOffset,
                event->RECEIVE.TotalBufferLength,
                event->RECEIVE.BufferCount,
                static_cast<uint32_t>(event->RECEIVE.Flags),
                true);
        }
        if (!QueueDeferredQuicReceive(
                relay,
                stream,
                event->RECEIVE.Buffers,
                event->RECEIVE.BufferCount,
                fin)) {
            AbortRelayFromCallback(relayId, binding, stream);
        }
        return QUIC_STATUS_PENDING;
    }
    if (event->Type == QUIC_STREAM_EVENT_PEER_SEND_ABORTED) {
        TqLinuxRelayEvent queued{};
        queued.Type = TqLinuxRelayEventType::QuicPeerSendAborted;
        queued.RelayId = relayId;
        queued.Value = event->PEER_SEND_ABORTED.ErrorCode;
        if (!Enqueue(std::move(queued))) {
            return QUIC_STATUS_OUT_OF_MEMORY;
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE) {
        TqLinuxRelayEvent queued{};
        queued.Type = TqLinuxRelayEventType::QuicShutdownComplete;
        queued.RelayId = relayId;
        queued.Value = event->SHUTDOWN_COMPLETE.ConnectionErrorCode;
        queued.Length = static_cast<size_t>(event->SHUTDOWN_COMPLETE.ConnectionCloseStatus);
        if (!Enqueue(std::move(queued))) {
            return QUIC_STATUS_OUT_OF_MEMORY;
        }
        return QUIC_STATUS_SUCCESS;
    }
    if (event->Type == QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED) {
        TqLinuxRelayEvent queued{};
        queued.Type = TqLinuxRelayEventType::QuicPeerReceiveAborted;
        queued.RelayId = relayId;
        queued.Value = event->PEER_RECEIVE_ABORTED.ErrorCode;
        if (!Enqueue(std::move(queued))) {
            return QUIC_STATUS_OUT_OF_MEMORY;
        }
        return QUIC_STATUS_SUCCESS;
    }
    return QUIC_STATUS_SUCCESS;
}

TqLinuxRelayRuntime& TqLinuxRelayRuntime::Instance() {
    static TqLinuxRelayRuntime runtime;
    return runtime;
}

bool TqLinuxRelayRuntime::Start(const TqTuningConfig& tuning) {
    std::lock_guard<std::mutex> guard(Lock);
    if (!Workers.empty()) {
        return true;
    }

    TqRelayResetQuicReadAhead(tuning.InitialQuicReadAheadBytes);
    const uint32_t workerCount = std::max<uint32_t>(1, tuning.LinuxRelayWorkerCount);
    for (uint32_t i = 0; i < workerCount; ++i) {
        TqLinuxRelayWorkerConfig config{};
        config.EventBudget = tuning.LinuxRelayWorkerEventBudget;
        config.EventQueueCapacity = tuning.LinuxRelayEventQueueCapacity;
        config.TrackEventProducers = true;
        config.WorkerIndex = i;
        config.ByteBudgetPerTick = tuning.LinuxRelayWorkerByteBudgetPerTick;
        config.ReadChunkSize = tuning.LinuxRelayReadChunkSize;
        config.ReadBatchBytes = tuning.LinuxRelayReadBatchBytes;
        config.MaxIov = tuning.LinuxRelayMaxIov;
        config.TcpWriteMaxBytes = tuning.LinuxRelayTcpWriteMaxBytes;
        config.TcpWriteBurstBytes = tuning.LinuxRelayTcpWriteBurstBytes;
        config.MaxPendingBufferBytes = tuning.MaxPendingBufferBytesPerRelay;
        config.MaxPendingQuicReceiveBytesPerRelay = tuning.LinuxRelayPerTunnelPendingBytes;
        config.DeferredReceiveCompleteBatchBytes = tuning.LinuxRelayQuicReceiveCompleteBatchBytes;
        config.MaxInFlightQuicSends = tuning.RelayMaxInFlightSends;
        config.MaxBufferedQuicSendBytes = tuning.InitialQuicReadAheadBytes;
        config.UseDynamicQuicReadAhead = true;
        auto worker = std::make_unique<TqLinuxRelayWorker>(config);
        if (!worker->Start()) {
            Workers.clear();
            return false;
        }
        Workers.push_back(std::move(worker));
    }
    return true;
}

void TqLinuxRelayRuntime::Stop() {
    std::lock_guard<std::mutex> guard(Lock);
    Workers.clear();
    NextWorker = 0;
}

TqLinuxRelayWorker* TqLinuxRelayRuntime::PickWorker() {
    std::lock_guard<std::mutex> guard(Lock);
    if (Workers.empty()) {
        return nullptr;
    }
    TqLinuxRelayWorker* worker = Workers[NextWorker % Workers.size()].get();
    ++NextWorker;
    return worker;
}

TqLinuxRelayWorkerSnapshot TqLinuxRelayRuntime::Snapshot() const {
    std::lock_guard<std::mutex> guard(Lock);
    TqLinuxRelayWorkerSnapshot total{};
    total.ActiveRelayStates.reserve(Workers.size());
    for (const auto& worker : Workers) {
        const auto snapshot = worker->Snapshot();
        total.EventsProcessed += snapshot.EventsProcessed;
        total.WakeupWrites += snapshot.WakeupWrites;
        total.PendingEvents += snapshot.PendingEvents;
        total.EventQueueCapacity = std::max(total.EventQueueCapacity, snapshot.EventQueueCapacity);
        total.EventQueuePushCasRetries += snapshot.EventQueuePushCasRetries;
        total.EventQueuePopCasRetries += snapshot.EventQueuePopCasRetries;
        total.EventProducerThreadsObserved =
            std::max(total.EventProducerThreadsObserved, snapshot.EventProducerThreadsObserved);
        total.MultipleEventProducerThreadsObserved =
            total.MultipleEventProducerThreadsObserved ||
            snapshot.MultipleEventProducerThreadsObserved;
        total.PendingBytes += snapshot.PendingBytes;
        total.ActiveRelays += snapshot.ActiveRelays;
        total.SnapshotActiveRelaysScanned += snapshot.SnapshotActiveRelaysScanned;
        total.ActiveRelayStates.insert(
            total.ActiveRelayStates.end(),
            snapshot.ActiveRelayStates.begin(),
            snapshot.ActiveRelayStates.end());
        total.ActiveTcpRelays += snapshot.ActiveTcpRelays;
        total.ActiveSinkRelays += snapshot.ActiveSinkRelays;
        total.ActiveQuicSendRelays += snapshot.ActiveQuicSendRelays;
        total.CurrentPendingQuicReceiveBytes += snapshot.CurrentPendingQuicReceiveBytes;
        total.CurrentPendingQuicReceiveQueue += snapshot.CurrentPendingQuicReceiveQueue;
        total.RelayBufferBytesInUse += snapshot.RelayBufferBytesInUse;
        total.TcpReadArmedRelays += snapshot.TcpReadArmedRelays;
        total.TcpReadDisabledRelays += snapshot.TcpReadDisabledRelays;
        total.TcpWriteArmedRelays += snapshot.TcpWriteArmedRelays;
        total.ClosingRelays += snapshot.ClosingRelays;
        total.TcpReadClosedRelays += snapshot.TcpReadClosedRelays;
        total.TcpWriteShutdownQueuedRelays += snapshot.TcpWriteShutdownQueuedRelays;
        total.OutstandingQuicSends += snapshot.OutstandingQuicSends;
        total.OutstandingQuicSendBytes += snapshot.OutstandingQuicSendBytes;
        total.MaxBufferedQuicSendBytes += snapshot.MaxBufferedQuicSendBytes;
        total.PendingTcpWriteQueue += snapshot.PendingTcpWriteQueue;
        total.PendingTcpWriteBytes += snapshot.PendingTcpWriteBytes;
        total.MaxWorkerPendingBytes = std::max(total.MaxWorkerPendingBytes, snapshot.MaxWorkerPendingBytes);
        total.MaxWorkerActiveRelays = std::max(total.MaxWorkerActiveRelays, snapshot.MaxWorkerActiveRelays);
        total.MaxRelayPendingQuicReceiveBytes = std::max(total.MaxRelayPendingQuicReceiveBytes, snapshot.MaxRelayPendingQuicReceiveBytes);
        total.MaxRelayPendingQuicReceiveQueue = std::max(total.MaxRelayPendingQuicReceiveQueue, snapshot.MaxRelayPendingQuicReceiveQueue);
        total.MaxRelayTcpWriteEagainCount = std::max(total.MaxRelayTcpWriteEagainCount, snapshot.MaxRelayTcpWriteEagainCount);
        if (snapshot.HotRelayId != 0 &&
            (total.HotRelayId == 0 ||
             snapshot.HotRelayPendingQuicReceiveBytes > total.HotRelayPendingQuicReceiveBytes ||
             (snapshot.HotRelayPendingQuicReceiveBytes == total.HotRelayPendingQuicReceiveBytes &&
              snapshot.HotRelayTcpWriteEagainCount > total.HotRelayTcpWriteEagainCount))) {
            total.HotRelayId = snapshot.HotRelayId;
            total.HotRelayWorkerIndex = snapshot.HotRelayWorkerIndex;
            total.HotRelayTcpFd = snapshot.HotRelayTcpFd;
            total.HotRelayPendingQuicReceiveBytes = snapshot.HotRelayPendingQuicReceiveBytes;
            total.HotRelayPendingQuicReceiveQueue = snapshot.HotRelayPendingQuicReceiveQueue;
            total.HotRelayTcpWriteBytes = snapshot.HotRelayTcpWriteBytes;
            total.HotRelayTcpReadBytes = snapshot.HotRelayTcpReadBytes;
            total.HotRelayOutstandingQuicSends = snapshot.HotRelayOutstandingQuicSends;
            total.HotRelayOutstandingQuicSendBytes = snapshot.HotRelayOutstandingQuicSendBytes;
            total.HotRelayPendingQuicSendRetries = snapshot.HotRelayPendingQuicSendRetries;
            total.HotRelayIdealSendBytes = snapshot.HotRelayIdealSendBytes;
            total.HotRelayTcpWriteEagainCount = snapshot.HotRelayTcpWriteEagainCount;
            total.HotRelayEpollOutEvents = snapshot.HotRelayEpollOutEvents;
            total.HotRelayTcpReadArmed = snapshot.HotRelayTcpReadArmed;
            total.HotRelayTcpWriteArmed = snapshot.HotRelayTcpWriteArmed;
            total.HotRelayLocalAddress = snapshot.HotRelayLocalAddress;
            total.HotRelayPeerAddress = snapshot.HotRelayPeerAddress;
        }
        total.TcpReadBatches += snapshot.TcpReadBatches;
        total.TcpReadBytes += snapshot.TcpReadBytes;
        total.QuicSendOperations += snapshot.QuicSendOperations;
        total.MaxTcpReadIovUsed = std::max(total.MaxTcpReadIovUsed, snapshot.MaxTcpReadIovUsed);
        total.TcpWriteBatches += snapshot.TcpWriteBatches;
        total.TcpWriteBytes += snapshot.TcpWriteBytes;
        total.MaxTcpWriteIovUsed = std::max(total.MaxTcpWriteIovUsed, snapshot.MaxTcpWriteIovUsed);
        total.TcpWriteSendmsgCalls += snapshot.TcpWriteSendmsgCalls;
        total.TcpWriteAttemptBytes += snapshot.TcpWriteAttemptBytes;
        total.MaxTcpWriteAttemptBytes = std::max(total.MaxTcpWriteAttemptBytes, snapshot.MaxTcpWriteAttemptBytes);
        total.MaxTcpWriteSendmsgBytes = std::max(total.MaxTcpWriteSendmsgBytes, snapshot.MaxTcpWriteSendmsgBytes);
        total.TcpWriteAttemptBytesLe64K += snapshot.TcpWriteAttemptBytesLe64K;
        total.TcpWriteAttemptBytesLe256K += snapshot.TcpWriteAttemptBytesLe256K;
        total.TcpWriteAttemptBytesLe1M += snapshot.TcpWriteAttemptBytesLe1M;
        total.TcpWriteAttemptBytesLe4M += snapshot.TcpWriteAttemptBytesLe4M;
        total.TcpWriteAttemptBytesGt4M += snapshot.TcpWriteAttemptBytesGt4M;
        total.TcpWriteReturnedBytesLe64K += snapshot.TcpWriteReturnedBytesLe64K;
        total.TcpWriteReturnedBytesLe256K += snapshot.TcpWriteReturnedBytesLe256K;
        total.TcpWriteReturnedBytesLe1M += snapshot.TcpWriteReturnedBytesLe1M;
        total.TcpWriteReturnedBytesLe4M += snapshot.TcpWriteReturnedBytesLe4M;
        total.TcpWriteReturnedBytesGt4M += snapshot.TcpWriteReturnedBytesGt4M;
        total.TcpWriteEagainCount += snapshot.TcpWriteEagainCount;
        total.TcpWritePartialCount += snapshot.TcpWritePartialCount;
        total.TcpWriteBurstStops += snapshot.TcpWriteBurstStops;
        total.DeferredReceiveCompleteBytes += snapshot.DeferredReceiveCompleteBytes;
        total.DeferredReceiveCompletes += snapshot.DeferredReceiveCompletes;
        total.DeferredReceiveCompletionFlushes += snapshot.DeferredReceiveCompletionFlushes;
        total.MaxPendingQuicReceiveBytes = std::max(total.MaxPendingQuicReceiveBytes, snapshot.MaxPendingQuicReceiveBytes);
        total.MaxPendingQuicReceiveQueue = std::max(total.MaxPendingQuicReceiveQueue, snapshot.MaxPendingQuicReceiveQueue);
        total.QuicReceiveViewCount += snapshot.QuicReceiveViewCount;
        total.QuicReceiveViewBytes += snapshot.QuicReceiveViewBytes;
        total.MaxQuicReceiveViewBytes = std::max(total.MaxQuicReceiveViewBytes, snapshot.MaxQuicReceiveViewBytes);
        total.MaxQuicReceiveViewSlices = std::max(total.MaxQuicReceiveViewSlices, snapshot.MaxQuicReceiveViewSlices);
        total.QuicReceiveViewBytesLe64K += snapshot.QuicReceiveViewBytesLe64K;
        total.QuicReceiveViewBytesLe256K += snapshot.QuicReceiveViewBytesLe256K;
        total.QuicReceiveViewBytesLe1M += snapshot.QuicReceiveViewBytesLe1M;
        total.QuicReceiveViewBytesLe4M += snapshot.QuicReceiveViewBytesLe4M;
        total.QuicReceiveViewBytesGt4M += snapshot.QuicReceiveViewBytesGt4M;
        total.QuicReceiveViewSlices1 += snapshot.QuicReceiveViewSlices1;
        total.QuicReceiveViewSlices2To4 += snapshot.QuicReceiveViewSlices2To4;
        total.QuicReceiveViewSlices5To16 += snapshot.QuicReceiveViewSlices5To16;
        total.QuicReceiveViewSlicesGt16 += snapshot.QuicReceiveViewSlicesGt16;
        total.QuicReceivePausedCount += snapshot.QuicReceivePausedCount;
        total.QuicReceiveResumedCount += snapshot.QuicReceiveResumedCount;
        total.ReadDisabledCount += snapshot.ReadDisabledCount;
        total.CompressedTcpBytes += snapshot.CompressedTcpBytes;
        total.DecompressedTcpBytes += snapshot.DecompressedTcpBytes;
        total.ZstdDecompressInputBytes += snapshot.ZstdDecompressInputBytes;
        total.ZstdDecompressOutputBytes += snapshot.ZstdDecompressOutputBytes;
        total.ZstdDecompressCalls += snapshot.ZstdDecompressCalls;
        total.ZstdDecompressNeedInput += snapshot.ZstdDecompressNeedInput;
        total.ZstdDecompressNeedOutput += snapshot.ZstdDecompressNeedOutput;
        total.ZstdDecompressFailures += snapshot.ZstdDecompressFailures;
        total.Errors += snapshot.Errors;
        total.EventQueueFullErrors += snapshot.EventQueueFullErrors;
        total.TcpReadBufferAcquireFailures += snapshot.TcpReadBufferAcquireFailures;
        total.TcpReadBufferAcquirePendingBudgetFailures +=
            snapshot.TcpReadBufferAcquirePendingBudgetFailures;
        total.TcpReadBufferAcquireAllocFailures += snapshot.TcpReadBufferAcquireAllocFailures;
        total.TcpToQuicCompressFailures += snapshot.TcpToQuicCompressFailures;
        total.TcpToQuicCompressUpdateFailures += snapshot.TcpToQuicCompressUpdateFailures;
        total.TcpToQuicCompressFlushFailures += snapshot.TcpToQuicCompressFlushFailures;
        total.TcpToQuicBufferAcquireFailures += snapshot.TcpToQuicBufferAcquireFailures;
        total.TcpToQuicBufferAcquirePendingBudgetFailures +=
            snapshot.TcpToQuicBufferAcquirePendingBudgetFailures;
        total.TcpToQuicBufferAcquireAllocFailures +=
            snapshot.TcpToQuicBufferAcquireAllocFailures;
        total.QuicSendFailures += snapshot.QuicSendFailures;
        total.QuicSendBufferTooLargeFailures += snapshot.QuicSendBufferTooLargeFailures;
        total.QuicSendOperationAllocFailures += snapshot.QuicSendOperationAllocFailures;
        total.QuicSendApiFailures += snapshot.QuicSendApiFailures;
        total.QuicReceiveViewFailures += snapshot.QuicReceiveViewFailures;
        total.QuicReceiveViewAllocFailures += snapshot.QuicReceiveViewAllocFailures;
        total.QuicReceiveViewNullBufferFailures += snapshot.QuicReceiveViewNullBufferFailures;
        total.QuicReceiveViewEmptyFailures += snapshot.QuicReceiveViewEmptyFailures;
        total.QuicReceiveViewEnqueueFailures += snapshot.QuicReceiveViewEnqueueFailures;
        total.QuicReceiveDecompressFailures += snapshot.QuicReceiveDecompressFailures;
        total.QuicReceiveTcpBufferAcquireFailures += snapshot.QuicReceiveTcpBufferAcquireFailures;
        total.QuicReceiveTcpBufferAcquirePendingBudgetFailures +=
            snapshot.QuicReceiveTcpBufferAcquirePendingBudgetFailures;
        total.QuicReceiveTcpBufferAcquireAllocFailures +=
            snapshot.QuicReceiveTcpBufferAcquireAllocFailures;
        total.TcpWriteHardErrors += snapshot.TcpWriteHardErrors;
        total.LastTcpWriteErrno =
            snapshot.LastTcpWriteErrno != 0 ? snapshot.LastTcpWriteErrno : total.LastTcpWriteErrno;
        total.LastQuicSendStatus =
            snapshot.LastQuicSendStatus != 0 ? snapshot.LastQuicSendStatus : total.LastQuicSendStatus;
    }
    return total;
}

std::vector<TqLinuxRelayWorkerSnapshot> TqLinuxRelayRuntime::SnapshotWorkers() const {
    std::lock_guard<std::mutex> guard(Lock);
    std::vector<TqLinuxRelayWorkerSnapshot> snapshots;
    snapshots.reserve(Workers.size());
    for (const auto& worker : Workers) {
        snapshots.push_back(worker->Snapshot());
    }
    return snapshots;
}
