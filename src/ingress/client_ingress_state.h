#pragma once

#include "tcp_tunnel.h"
#include "proxy_auth.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class TqClientIngressProto {
    Socks5,
    HttpConnect,
};

enum class TqClientIngressResult {
    NeedRead,
    NeedWrite,
    ReadyToOpen,
    Close,
};

class TqClientIngressState {
public:
    explicit TqClientIngressState(
        TqClientIngressProto proto,
        std::shared_ptr<const TqProxyAuthTable> auth = std::make_shared<TqProxyAuthTable>());

    TqClientIngressResult Feed(const uint8_t* data, size_t size);
    const std::string& PendingWrite() const;
    void MarkWriteComplete(size_t bytes);
    const TunnelRequest& Request() const;
    std::vector<uint8_t> TakeBufferedData();

private:
    TqClientIngressResult FeedSocks5();
    TqClientIngressResult FeedHttpConnect();

    TqClientIngressProto Proto;
    std::shared_ptr<const TqProxyAuthTable> Auth;
    std::vector<uint8_t> ReadBuffer;
    std::string WriteBuffer;
    TunnelRequest ParsedRequest{};
    bool SocksGreetingDone{false};
    bool SocksAuthPending{false};
    bool CloseAfterWrite{false};
    bool Ready{false};
};
