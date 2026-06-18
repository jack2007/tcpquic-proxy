#include "ares_dns_resolver.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>
#if !defined(_WIN32)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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

int TestMultipleResolverLifecycle() {
    TqAresDnsResolver first;
    TqAresDnsResolver second;
    TqAresDnsResolver third;

    if (!first.Start()) {
        return 50;
    }
    if (!second.Start()) {
        first.Stop();
        return 51;
    }
    first.Stop();
    if (!third.Start()) {
        second.Stop();
        return 52;
    }

    bool callbackRan = false;
    TqDnsResolveResult observed{};
    const uint64_t id = second.Resolve("localhost", 443,
        [&](const TqDnsResolveResult& result) {
            callbackRan = true;
            observed = result;
        });
    if (id == 0) {
        second.Stop();
        third.Stop();
        return 53;
    }
    if (!RunUntilCallback(second, callbackRan, 5000)) {
        second.Stop();
        third.Stop();
        return 54;
    }
    if (!observed.Completed || !observed.Success) {
        second.Stop();
        third.Stop();
        return 55;
    }

    second.Stop();
    third.Stop();

    if (!first.Start()) {
        return 56;
    }
    first.Stop();
    return 0;
}

int TestConcurrentResolverLifecycle() {
    constexpr int kResolverCount = 4;
    std::vector<int> results(kResolverCount, 0);
    std::vector<std::thread> threads;
    threads.reserve(kResolverCount);

    for (int i = 0; i < kResolverCount; ++i) {
        threads.emplace_back([&, i]() {
            TqAresDnsResolver resolver;
            if (!resolver.Start()) {
                results[static_cast<size_t>(i)] = 60;
                return;
            }
            bool callbackRan = false;
            const uint64_t id = resolver.Resolve("localhost", 443,
                [&](const TqDnsResolveResult&) {
                    callbackRan = true;
                });
            if (id == 0) {
                resolver.Stop();
                results[static_cast<size_t>(i)] = 61;
                return;
            }
            resolver.Cancel(id);
            (void)resolver.RunOnce(0);
            if (callbackRan) {
                resolver.Stop();
                results[static_cast<size_t>(i)] = 62;
                return;
            }
            resolver.Stop();
        });
    }

    for (std::thread& thread : threads) {
        thread.join();
    }
    for (int result : results) {
        if (result != 0) {
            return result;
        }
    }
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
#if defined(_WIN32)
    return 0;
#else
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
#endif
}

int TestNoPendingTimeoutSelectionIsStable() {
    TqAresDnsResolver resolver;
    if (!resolver.Start()) {
        return 80;
    }

    const int infiniteWaitMs = resolver.TestOnlyNextWaitMs(-1);
    const int boundedWaitMs = resolver.TestOnlyNextWaitMs(25);
    resolver.Stop();

    if (infiniteWaitMs != -1) {
        return 81;
    }
    if (boundedWaitMs != 25) {
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
    result = TestMultipleResolverLifecycle();
    if (result != 0) {
        return result;
    }
    result = TestConcurrentResolverLifecycle();
    if (result != 0) {
        return result;
    }
    result = TestInfiniteTimeoutUsesAresTimeoutForPendingQuery();
    if (result != 0) {
        return result;
    }
    return TestNoPendingTimeoutSelectionIsStable();
}
