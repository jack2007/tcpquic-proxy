#pragma once

#include "msquic.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct TqEndpoint {
    std::string Host;
    uint16_t Port{0};
};

struct TqResolvedListen {
    std::string Text;
    QUIC_ADDR Address{};
};

bool TqParseEndpoint(const std::string& value, TqEndpoint& endpoint);
bool TqParseEndpointList(const std::string& value, std::vector<TqEndpoint>& endpoints, std::string& err);
bool TqMakeQuicAddr(const TqEndpoint& endpoint, QUIC_ADDR& address);
bool TqResolveServerListenList(const std::string& value, std::vector<TqResolvedListen>& listens, std::string& err);
std::string TqFormatEndpoint(const TqEndpoint& endpoint);
