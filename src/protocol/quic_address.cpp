#include "quic_address.h"

#include <cctype>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace {

bool TqParsePort(const std::string& value, uint16_t& port) {
    if (value.empty()) {
        return false;
    }

    unsigned long parsed = 0;
    for (unsigned char ch : value) {
        if (!std::isdigit(ch)) {
            return false;
        }
        parsed = parsed * 10 + static_cast<unsigned long>(ch - '0');
        if (parsed > std::numeric_limits<uint16_t>::max()) {
            return false;
        }
    }

    if (parsed == 0) {
        return false;
    }
    port = static_cast<uint16_t>(parsed);
    return true;
}

std::string TqTrimListItem(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t')) {
        --end;
    }

    return value.substr(begin, end - begin);
}

}

bool TqParseEndpoint(const std::string& value, TqEndpoint& endpoint) {
    if (value.empty()) {
        return false;
    }

    std::string host;
    std::string portText;
    if (value[0] == '[') {
        const size_t close = value.find(']');
        if (close == std::string::npos ||
            close + 1 >= value.size() ||
            value[close + 1] != ':') {
            return false;
        }
        host = value.substr(1, close - 1);
        portText = value.substr(close + 2);
    } else {
        const size_t colon = value.find(':');
        if (colon == std::string::npos ||
            value.find(':', colon + 1) != std::string::npos ||
            colon == 0 ||
            colon + 1 >= value.size()) {
            return false;
        }
        host = value.substr(0, colon);
        portText = value.substr(colon + 1);
    }

    uint16_t port = 0;
    if (host.empty() || !TqParsePort(portText, port)) {
        return false;
    }

    endpoint.Host = host;
    endpoint.Port = port;
    return true;
}

bool TqParseEndpointList(const std::string& value, std::vector<TqEndpoint>& endpoints, std::string& err) {
    err.clear();
    std::vector<TqEndpoint> parsed;

    size_t begin = 0;
    while (begin <= value.size()) {
        const size_t comma = value.find(',', begin);
        const size_t end = comma == std::string::npos ? value.size() : comma;
        const std::string item = TqTrimListItem(value.substr(begin, end - begin));

        TqEndpoint endpoint;
        if (!TqParseEndpoint(item, endpoint)) {
            err = "invalid endpoint: " + item;
            return false;
        }
        parsed.push_back(std::move(endpoint));

        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }

    endpoints = std::move(parsed);
    return true;
}

std::string TqFormatEndpoint(const TqEndpoint& endpoint) {
    if (endpoint.Host.find(':') != std::string::npos) {
        return "[" + endpoint.Host + "]:" + std::to_string(endpoint.Port);
    }
    return endpoint.Host + ":" + std::to_string(endpoint.Port);
}

bool TqMakeQuicAddr(const TqEndpoint& endpoint, QUIC_ADDR& address) {
    if (endpoint.Host == "*") {
        std::memset(&address, 0, sizeof(address));
        QuicAddrSetFamily(&address, QUIC_ADDRESS_FAMILY_UNSPEC);
        QuicAddrSetPort(&address, endpoint.Port);
        return true;
    }
    return QuicAddrFromString(endpoint.Host.c_str(), endpoint.Port, &address);
}

bool TqResolveServerListenList(const std::string& value, std::vector<TqResolvedListen>& listens, std::string& err) {
    err.clear();

    std::vector<TqEndpoint> endpoints;
    if (!TqParseEndpointList(value, endpoints, err)) {
        return false;
    }

    std::unordered_set<std::string> seen;
    std::vector<TqResolvedListen> resolved;
    for (const TqEndpoint& endpoint : endpoints) {
        const std::string text = TqFormatEndpoint(endpoint);
        if (!seen.insert(text).second) {
            err = "duplicate listen address: " + text;
            return false;
        }

        QUIC_ADDR address{};
        if (!TqMakeQuicAddr(endpoint, address)) {
            err = "invalid listen address: " + text;
            return false;
        }
        resolved.push_back(TqResolvedListen{text, address});
    }

    listens = std::move(resolved);
    return true;
}
