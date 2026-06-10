#pragma once

#include <cstdint>
#include <vector>

#include "platform_socket.h"

struct TqDialResult {
    TqSocketHandle Fd{TqInvalidSocket};
    bool Refused{false};
    bool TimedOut{false};
};

TqDialResult TqDialTcp(const std::vector<sockaddr_storage>& addrs, int timeoutMs);
void TqTuneTcpForThroughput(TqSocketHandle fd, int bufferBytes = 0);
