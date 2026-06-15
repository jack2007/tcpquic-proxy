#pragma once

#include <cstdint>
#include <string>

struct TqTcpBenchEndpoint {
    std::string Host;
    uint16_t Port{0};
};

bool TqTcpBenchParseEndpoint(
    const std::string& value,
    TqTcpBenchEndpoint& endpoint,
    std::string& err);

std::string TqTcpBenchBuildHttpConnectRequest(const TqTcpBenchEndpoint& endpoint);
bool TqTcpBenchHttpConnectSucceeded(const std::string& response);

int TqTcpSinkBenchMain(int argc, char** argv);
