#pragma once

#include <cstdint>
#include <vector>

#include <sys/socket.h>

struct TqDialResult {
    int Fd{-1};
    bool Refused{false};
    bool TimedOut{false};
};

TqDialResult TqDialTcp(const std::vector<sockaddr_storage>& addrs, int timeoutMs);
void TqTuneTcpForThroughput(int fd, int bufferBytes = 0);
