#pragma once

#include "msquic.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct TqConfig;

struct TqEndpoint {
    std::string Host;
    uint16_t Port{0};
};

struct TqResolvedListen {
    std::string Text;
    QUIC_ADDR Address{};
};

struct TqClientSlotPath {
    std::string Name;
    std::string LocalAddress;
    std::string PeerHost;
    uint16_t PeerPort{0};
    std::string PeerText;
};

bool TqParseEndpoint(const std::string& value, TqEndpoint& endpoint);
bool TqParseEndpointList(const std::string& value, std::vector<TqEndpoint>& endpoints, std::string& err);
bool TqMakeQuicAddr(const TqEndpoint& endpoint, QUIC_ADDR& address);
bool TqResolveServerListenList(const std::string& value, std::vector<TqResolvedListen>& listens, std::string& err);
bool TqBuildClientSlotPaths(const TqConfig& cfg, std::vector<TqClientSlotPath>& slots, std::string& err);
std::string TqFormatEndpoint(const TqEndpoint& endpoint);
