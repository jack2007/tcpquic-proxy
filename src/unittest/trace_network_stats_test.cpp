#include "trace.h"

#include "msquic.hpp"

#include <string>

const MsQuicApi* MsQuic = nullptr;

int main() {
    const std::string line = TqFormatTraceNetworkStatsLine(TqTraceNetworkStats{
        1234,
        5678,
        9012,
        70000,
        64000,
        10000000});

    if (line.find("net_stats:") == std::string::npos) {
        return 1;
    }
    if (line.find("bbr_bw_bytes_per_sec=10000000") == std::string::npos) {
        return 2;
    }
    if (line.find("bbr_bw_mbps=80.00") == std::string::npos) {
        return 3;
    }
    if (line.find("bytes_in_flight=1234") == std::string::npos ||
        line.find("posted=5678") == std::string::npos ||
        line.find("ideal=9012") == std::string::npos ||
        line.find("srtt=70000us") == std::string::npos ||
        line.find("cwnd=64000") == std::string::npos) {
        return 4;
    }

    return 0;
}
