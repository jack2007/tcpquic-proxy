#if defined(__APPLE__)

#include "darwin_relay_worker.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

namespace {

void CheckImpl(bool condition, int line) {
    if (!condition) {
        std::fprintf(stderr, "check failed at line %d\n", line);
        std::fflush(stderr);
        std::exit(line % 125 + 1);
    }
}

#define CHECK(condition) CheckImpl((condition), __LINE__)

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

void StopCleansRegisteredPublicHandle() {
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
    StopCleansRegisteredPublicHandle();
    RegisterFilterFailureRollsBackRelayAndHandle();
    RegisterAfterStopFailsWithoutPublishingHandle();
    return 0;
}

#endif
