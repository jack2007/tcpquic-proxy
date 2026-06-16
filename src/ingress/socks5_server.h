#pragma once

#include "control_protocol.h"
#include "platform_socket.h"
#include "proxy_auth.h"
#include "tcp_tunnel.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class TqThreadPool;

static constexpr uint8_t TQ_SOCKS5_REP_SUCCEEDED = 0x00;
static constexpr uint8_t TQ_SOCKS5_REP_GENERAL_FAILURE = 0x01;
static constexpr uint8_t TQ_SOCKS5_REP_CONNECTION_NOT_ALLOWED = 0x02;
static constexpr uint8_t TQ_SOCKS5_REP_NETWORK_UNREACHABLE = 0x03;
static constexpr uint8_t TQ_SOCKS5_REP_HOST_UNREACHABLE = 0x04;
static constexpr uint8_t TQ_SOCKS5_REP_CONNECTION_REFUSED = 0x05;
static constexpr uint8_t TQ_SOCKS5_REP_TTL_EXPIRED = 0x06;
static constexpr uint8_t TQ_SOCKS5_REP_COMMAND_NOT_SUPPORTED = 0x07;
static constexpr uint8_t TQ_SOCKS5_REP_ADDRESS_TYPE_NOT_SUPPORTED = 0x08;

uint8_t TqSocks5RepForOpenError(TqOpenError error);
bool TqParseSocks5ConnectRequest(const std::vector<uint8_t>& request, TunnelRequest& out);

class TqSocks5Server {
public:
    TqSocks5Server(
        std::string listenHostPort,
        TunnelStartFn onTunnel,
        TqThreadPool* pool,
        std::shared_ptr<const TqProxyAuthTable> auth = std::make_shared<TqProxyAuthTable>());
    ~TqSocks5Server();

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

void RunSocks5Server(
    const std::string& listenHostPort,
    TunnelStartFn onTunnel,
    TqThreadPool* pool,
    std::shared_ptr<const TqProxyAuthTable> auth = std::make_shared<TqProxyAuthTable>());
