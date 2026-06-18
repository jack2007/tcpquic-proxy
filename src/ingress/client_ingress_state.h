#pragma once

#include "tcp_tunnel.h"

#include <cstddef>
#include <cstdint>
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
    explicit TqClientIngressState(TqClientIngressProto proto);

    TqClientIngressResult Feed(const uint8_t* data, size_t size);
    const std::string& PendingWrite() const;
    void MarkWriteComplete(size_t bytes);
    const TunnelRequest& Request() const;
    std::vector<uint8_t> TakeBufferedData();

private:
    TqClientIngressResult FeedSocks5();
    TqClientIngressResult FeedHttpConnect();

    TqClientIngressProto Proto;
    std::vector<uint8_t> ReadBuffer;
    std::string WriteBuffer;
    TunnelRequest ParsedRequest{};
    bool SocksGreetingDone{false};
    bool Ready{false};
};
