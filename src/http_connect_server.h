#pragma once

#include "control_protocol.h"
#include "tcp_tunnel.h"

#include <atomic>
#include <string>
#include <thread>

class TqThreadPool;

int TqHttpStatusForOpenError(TqOpenError error);
bool TqParseHttpConnectRequest(const std::string& request, TunnelRequest& out);

class TqHttpConnectServer {
public:
    TqHttpConnectServer(std::string listenHostPort, TunnelStartFn onTunnel, TqThreadPool* pool);
    ~TqHttpConnectServer();

    bool Start(std::string& err);
    void Stop();

private:
    void Run();

    std::string ListenHostPort;
    TunnelStartFn OnTunnel;
    TqThreadPool* Pool{nullptr};
    std::atomic<bool> Stopping{false};
    std::atomic<int> ListenFd{-1};
    std::thread Worker;
};

void RunHttpConnectServer(const std::string& listenHostPort, TunnelStartFn onTunnel, TqThreadPool* pool);
