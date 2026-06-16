#pragma once

#include "control_protocol.h"
#include "platform_socket.h"
#include "proxy_auth.h"
#include "tcp_tunnel.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

class TqThreadPool;

int TqHttpStatusForOpenError(TqOpenError error);
bool TqParseHttpConnectRequest(const std::string& request, TunnelRequest& out);

class TqHttpConnectServer {
public:
    TqHttpConnectServer(
        std::string listenHostPort,
        TunnelStartFn onTunnel,
        TqThreadPool* pool,
        std::shared_ptr<const TqProxyAuthTable> auth = std::make_shared<TqProxyAuthTable>());
    ~TqHttpConnectServer();

    bool Start(std::string& err);
    void Stop();

private:
    void Run();

    std::string ListenHostPort;
    TunnelStartFn OnTunnel;
    TqThreadPool* Pool{nullptr};
    std::shared_ptr<const TqProxyAuthTable> Auth;
    std::atomic<bool> Stopping{false};
    std::atomic<TqSocketHandle> ListenFd{TqInvalidSocket};
    std::thread Worker;
};

void RunHttpConnectServer(
    const std::string& listenHostPort,
    TunnelStartFn onTunnel,
    TqThreadPool* pool,
    std::shared_ptr<const TqProxyAuthTable> auth = std::make_shared<TqProxyAuthTable>());
