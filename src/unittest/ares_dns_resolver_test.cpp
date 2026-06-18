#include "ares_dns_resolver.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <netinet/in.h>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

bool AddressHasPort(const sockaddr_storage& address, uint16_t port) {
    if (address.ss_family == AF_INET) {
        const auto* addr4 = reinterpret_cast<const sockaddr_in*>(&address);
        return ntohs(addr4->sin_port) == port;
    }
    if (address.ss_family == AF_INET6) {
        const auto* addr6 = reinterpret_cast<const sockaddr_in6*>(&address);
        return ntohs(addr6->sin6_port) == port;
    }
    return false;
}

bool RunUntilCallback(TqAresDnsResolver& resolver, bool& callbackRan, int timeoutMs) {
    const auto start = std::chrono::steady_clock::now();
    while (!callbackRan) {
        (void)resolver.RunOnce(25);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        if (elapsed.count() >= timeoutMs) {
            return false;
        }
    }
    return true;
}

int TestStartAndLocalhostResolve() {
    TqAresDnsResolver resolver;
    if (!resolver.Start()) {
        return 1;
    }

    bool callbackRan = false;
    TqDnsResolveResult observed{};
    const uint64_t id = resolver.Resolve("localhost", 443,
        [&](const TqDnsResolveResult& result) {
            callbackRan = true;
            observed = result;
        });
    if (id == 0) {
        resolver.Stop();
        return 2;
    }
    if (!RunUntilCallback(resolver, callbackRan, 5000)) {
        resolver.Stop();
        return 3;
    }
    if (!observed.Completed) {
        resolver.Stop();
        return 4;
    }
    if (!observed.Success) {
        resolver.Stop();
        return 5;
    }
    if (observed.Addresses.empty()) {
        resolver.Stop();
        return 6;
    }
    for (const sockaddr_storage& address : observed.Addresses) {
        if (!AddressHasPort(address, 443)) {
            resolver.Stop();
            return 7;
        }
    }

    resolver.Stop();
    return 0;
}

int TestCancelSuppressesCallback() {
    TqAresDnsResolver resolver;
    if (!resolver.Start()) {
        return 20;
    }

    bool callbackRan = false;
    const uint64_t id = resolver.Resolve("localhost", 443,
        [&](const TqDnsResolveResult&) {
            callbackRan = true;
        });
    if (id == 0) {
        resolver.Stop();
        return 21;
    }
    resolver.Cancel(id);

    for (int i = 0; i < 20; ++i) {
        (void)resolver.RunOnce(25);
        if (callbackRan) {
            resolver.Stop();
            return 22;
        }
    }

    resolver.Stop();
    return 0;
}

int TestStopIsIdempotentAndResolveAfterStopFails() {
    TqAresDnsResolver resolver;
    if (!resolver.Start()) {
        return 40;
    }
    resolver.Stop();
    resolver.Stop();

    bool callbackRan = false;
    const uint64_t id = resolver.Resolve("localhost", 443,
        [&](const TqDnsResolveResult&) {
            callbackRan = true;
        });
    if (id != 0 || callbackRan) {
        return 41;
    }

    resolver.Stop();
    return 0;
}

int ChildRunOnceInfiniteTimeoutWithPendingQuery() {
    TqAresDnsResolver resolver;
    if (!resolver.Start()) {
        return 60;
    }

    const uint64_t id = resolver.Resolve("localhost", 443,
        [](const TqDnsResolveResult&) {});
    if (id == 0) {
        resolver.Stop();
        return 61;
    }

    const auto start = std::chrono::steady_clock::now();
    (void)resolver.RunOnce(-1);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    resolver.Cancel(id);
    resolver.Stop();

    if (elapsed.count() >= 1000) {
        return 62;
    }
    return 0;
}

int TestInfiniteTimeoutUsesAresTimeoutForPendingQuery() {
    const pid_t child = ::fork();
    if (child < 0) {
        return 70;
    }
    if (child == 0) {
        _exit(ChildRunOnceInfiniteTimeoutWithPendingQuery());
    }

    int status = 0;
    for (int i = 0; i < 100; ++i) {
        const pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == child) {
            if (!WIFEXITED(status)) {
                return 71;
            }
            return WEXITSTATUS(status);
        }
        if (result < 0) {
            return 72;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    (void)::kill(child, SIGKILL);
    (void)::waitpid(child, &status, 0);
    return 73;
}

int TestInfiniteTimeoutWaitSelectionUsesAresTimeout() {
    TqAresDnsResolver resolver;
    if (!resolver.Start()) {
        return 80;
    }

    const uint64_t id = resolver.Resolve("tcpquic-wait-selection.invalid", 443,
        [](const TqDnsResolveResult&) {});
    if (id == 0) {
        resolver.Stop();
        return 81;
    }

    const int waitMs = resolver.TestOnlyNextWaitMs(-1);
    resolver.Cancel(id);
    resolver.Stop();

    if (waitMs < 0) {
        return 82;
    }
    return 0;
}

} // namespace

int main() {
    int result = TestStartAndLocalhostResolve();
    if (result != 0) {
        return result;
    }
    result = TestCancelSuppressesCallback();
    if (result != 0) {
        return result;
    }
    result = TestStopIsIdempotentAndResolveAfterStopFails();
    if (result != 0) {
        return result;
    }
    result = TestInfiniteTimeoutUsesAresTimeoutForPendingQuery();
    if (result != 0) {
        return result;
    }
    return TestInfiniteTimeoutWaitSelectionUsesAresTimeout();
}
