#pragma once

#include "platform_socket.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct TqDnsResolveResult {
    bool Completed{false};
    bool Success{false};
    int Status{0};
    std::vector<sockaddr_storage> Addresses;
};

using TqDnsResolveCallback = std::function<void(const TqDnsResolveResult&)>;

class TqAresDnsResolver {
public:
    TqAresDnsResolver();
    ~TqAresDnsResolver();

    TqAresDnsResolver(const TqAresDnsResolver&) = delete;
    TqAresDnsResolver& operator=(const TqAresDnsResolver&) = delete;

    bool Start();
    void Stop();
    uint64_t Resolve(const std::string& host, uint16_t port, TqDnsResolveCallback callback);
    void Cancel(uint64_t id);
    bool RunOnce(int timeoutMs);

#ifdef TQ_UNIT_TESTING
    int TestOnlyNextWaitMs(int requestedTimeoutMs);
#endif

private:
    struct Impl;
    Impl* State{nullptr};
};
