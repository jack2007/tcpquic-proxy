#pragma once

#include "acl.h"
#include "ares_dns_resolver.h"
#include "control_protocol.h"
#include "platform_socket.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

struct TqServerDialResult {
    bool Done{false};
    TqOpenError Error{TqOpenError::Internal};
    TqSocketHandle Fd{TqInvalidSocket};
};

using TqServerDialComplete = std::function<void(const TqServerDialResult&)>;

struct TqServerDialRequest {
    std::string Host;
    uint16_t Port{0};
    uint64_t TraceTunnelId{0};
    TqServerDialComplete Complete;
};

class TqServerDialReactor {
public:
#ifdef TQ_UNIT_TESTING
    struct TestHooks {
        std::function<uint64_t(const std::string&, uint16_t, TqDnsResolveCallback)> Resolve;
        std::function<void(uint64_t)> CancelResolve;
        std::function<bool(int)> RunDnsOnce;
        std::function<TqSocketHandle(int, int, int)> CreateSocket;
        std::function<bool(TqSocketHandle)> SetNonBlocking;
        std::function<int(TqSocketHandle, const sockaddr*, socklen_t)> Connect;
        std::function<int(TqSocketHandle)> GetLastSocketError;
        std::function<int(TqSocketHandle, int*)> GetSocketError;
    };
#endif

    TqServerDialReactor();
    explicit TqServerDialReactor(TqAcl acl);
#ifdef TQ_UNIT_TESTING
    TqServerDialReactor(TqAcl acl, TestHooks hooks);
#endif
    ~TqServerDialReactor();

    TqServerDialReactor(const TqServerDialReactor&) = delete;
    TqServerDialReactor& operator=(const TqServerDialReactor&) = delete;

    bool Start();
    void Stop();
    uint64_t Submit(TqServerDialRequest request);
    void Cancel(uint64_t token);
    bool RunOnce(int timeoutMs);

private:
    struct Impl;
    std::unique_ptr<Impl> State;
};
