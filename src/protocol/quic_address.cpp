#include "quic_address.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <unordered_set>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Iphlpapi.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

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

bool TqIsIpv4Wildcard(const TqEndpoint& endpoint) {
    return endpoint.Host == "0.0.0.0";
}

bool TqIsIpv6Wildcard(const TqEndpoint& endpoint) {
    return endpoint.Host == "::";
}

bool TqFormatQuicAddress(const QUIC_ADDR& address, std::string& text) {
    QUIC_ADDR_STR addressText{};
    if (!QuicAddrToString(&address, &addressText)) {
        return false;
    }
    text = addressText.Address;
    return true;
}

std::string TqQuicAddressKey(const QUIC_ADDR& address, const std::string& text) {
    return std::to_string(static_cast<int>(QuicAddrGetFamily(&address))) + "|" + text;
}

bool TqAppendResolvedListen(
    const TqEndpoint& endpoint,
    std::vector<TqResolvedListen>& resolved,
    std::unordered_set<std::string>& seen) {
    QUIC_ADDR address{};
    if (!TqMakeQuicAddr(endpoint, address)) {
        return false;
    }

    std::string text;
    if (!TqFormatQuicAddress(address, text)) {
        return false;
    }

    if (seen.insert(TqQuicAddressKey(address, text)).second) {
        resolved.push_back(TqResolvedListen{text, address});
    }
    return true;
}

#if defined(_WIN32)
bool TqIsUsableAdapterAddress(const IP_ADAPTER_ADDRESSES& adapter) {
    return adapter.OperStatus == IfOperStatusUp;
}

void TqCollectAdapterAddresses(ADDRESS_FAMILY family, std::vector<std::string>& hosts) {
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 15 * 1024;
    std::vector<unsigned char> buffer(size);
    IP_ADAPTER_ADDRESSES* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    ULONG rc = GetAdaptersAddresses(family, flags, nullptr, adapters, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        rc = GetAdaptersAddresses(family, flags, nullptr, adapters, &size);
    }
    if (rc != NO_ERROR) {
        return;
    }

    std::unordered_set<std::string> seen;
    for (IP_ADAPTER_ADDRESSES* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        if (!TqIsUsableAdapterAddress(*adapter)) {
            continue;
        }
        for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress;
             unicast != nullptr;
             unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == nullptr ||
                unicast->Address.lpSockaddr->sa_family != family) {
                continue;
            }

            char text[INET6_ADDRSTRLEN]{};
            if (family == AF_INET) {
                const auto* addr = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                const uint32_t hostOrder = ntohl(addr->sin_addr.s_addr);
                if (addr->sin_addr.s_addr == INADDR_ANY ||
                    (hostOrder & 0xff000000u) == 0x7f000000u ||
                    IN_MULTICAST(hostOrder)) {
                    continue;
                }
                if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) == nullptr) {
                    continue;
                }
            } else if (family == AF_INET6) {
                const auto* addr = reinterpret_cast<const sockaddr_in6*>(unicast->Address.lpSockaddr);
                const in6_addr& ip = addr->sin6_addr;
                if (IN6_IS_ADDR_UNSPECIFIED(&ip) ||
                    IN6_IS_ADDR_LOOPBACK(&ip) ||
                    IN6_IS_ADDR_MULTICAST(&ip) ||
                    IN6_IS_ADDR_LINKLOCAL(&ip)) {
                    continue;
                }
                if (inet_ntop(AF_INET6, &ip, text, sizeof(text)) == nullptr) {
                    continue;
                }
            }

            if (seen.insert(text).second) {
                hosts.push_back(text);
            }
        }
    }
}
#else
void TqCollectIfaddrsAddresses(int family, std::vector<std::string>& hosts) {
    ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) {
        return;
    }

    std::unordered_set<std::string> seen;
    for (ifaddrs* item = ifaddr; item != nullptr; item = item->ifa_next) {
        if (item->ifa_addr == nullptr ||
            item->ifa_addr->sa_family != family ||
            (item->ifa_flags & IFF_UP) == 0 ||
            (item->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }

        char text[INET6_ADDRSTRLEN]{};
        if (family == AF_INET) {
            const auto* addr = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
            const uint32_t hostOrder = ntohl(addr->sin_addr.s_addr);
            if (addr->sin_addr.s_addr == INADDR_ANY ||
                (hostOrder & 0xff000000u) == 0x7f000000u ||
                IN_MULTICAST(hostOrder)) {
                continue;
            }
            if (inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text)) == nullptr) {
                continue;
            }
        } else if (family == AF_INET6) {
            const auto* addr = reinterpret_cast<const sockaddr_in6*>(item->ifa_addr);
            const in6_addr& ip = addr->sin6_addr;
            if (IN6_IS_ADDR_UNSPECIFIED(&ip) ||
                IN6_IS_ADDR_LOOPBACK(&ip) ||
                IN6_IS_ADDR_MULTICAST(&ip) ||
                IN6_IS_ADDR_LINKLOCAL(&ip)) {
                continue;
            }
            if (inet_ntop(AF_INET6, &ip, text, sizeof(text)) == nullptr) {
                continue;
            }
        }

        if (seen.insert(text).second) {
            hosts.push_back(text);
        }
    }

    freeifaddrs(ifaddr);
}
#endif

std::vector<std::string> TqCollectLocalAddresses(QUIC_ADDRESS_FAMILY family) {
    std::vector<std::string> hosts;
#if defined(_WIN32)
    TqCollectAdapterAddresses(
        family == QUIC_ADDRESS_FAMILY_INET ? AF_INET : AF_INET6,
        hosts);
#else
    TqCollectIfaddrsAddresses(
        family == QUIC_ADDRESS_FAMILY_INET ? AF_INET : AF_INET6,
        hosts);
#endif
    std::sort(hosts.begin(), hosts.end());
    return hosts;
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
        if (endpoint.Host == "*") {
            QUIC_ADDR address{};
            if (!TqMakeQuicAddr(endpoint, address)) {
                err = "invalid listen address: " + TqFormatEndpoint(endpoint);
                return false;
            }
            const std::string text = TqFormatEndpoint(endpoint);
            if (seen.insert(TqQuicAddressKey(address, text)).second) {
                resolved.push_back(TqResolvedListen{text, address});
            }
            continue;
        }

        if (TqIsIpv4Wildcard(endpoint) || TqIsIpv6Wildcard(endpoint)) {
            const QUIC_ADDRESS_FAMILY family =
                TqIsIpv4Wildcard(endpoint) ? QUIC_ADDRESS_FAMILY_INET : QUIC_ADDRESS_FAMILY_INET6;
            const std::vector<std::string> hosts = TqCollectLocalAddresses(family);
            if (hosts.empty()) {
                err = TqFormatEndpoint(endpoint) + " expanded to no usable local addresses";
                return false;
            }
            bool expanded = false;
            for (const std::string& host : hosts) {
                expanded = TqAppendResolvedListen(TqEndpoint{host, endpoint.Port}, resolved, seen) || expanded;
            }
            if (!expanded) {
                err = TqFormatEndpoint(endpoint) + " expanded to no usable local addresses";
                return false;
            }
            continue;
        }

        QUIC_ADDR address{};
        if (!TqMakeQuicAddr(endpoint, address)) {
            err = "invalid listen address: " + TqFormatEndpoint(endpoint);
            return false;
        }

        std::string text;
        if (!TqFormatQuicAddress(address, text)) {
            err = "invalid listen address: " + TqFormatEndpoint(endpoint);
            return false;
        }
        if (seen.insert(TqQuicAddressKey(address, text)).second) {
            resolved.push_back(TqResolvedListen{text, address});
        }
    }

    listens = std::move(resolved);
    return true;
}

bool TqBuildClientSlotPaths(const TqConfig& cfg, std::vector<TqClientSlotPath>& slots, std::string& err) {
    err.clear();
    std::vector<TqClientSlotPath> expanded;

    if (!cfg.QuicPaths.empty()) {
        for (const TqQuicPathConfig& path : cfg.QuicPaths) {
            if (path.Connections == 0) {
                err = "path connections must be greater than zero: " + path.Name;
                return false;
            }

            TqEndpoint endpoint;
            if (!TqParseEndpoint(path.Peer, endpoint)) {
                err = "invalid path peer: " + path.Peer;
                return false;
            }

            const std::string peerText = TqFormatEndpoint(endpoint);
            for (uint32_t i = 0; i < path.Connections; ++i) {
                expanded.push_back(TqClientSlotPath{
                    path.Name,
                    path.LocalAddress,
                    endpoint.Host,
                    endpoint.Port,
                    peerText});
            }
        }

        if (expanded.empty()) {
            err = "path-mode produced no connection slots";
            return false;
        }
        slots = std::move(expanded);
        return true;
    }

    if (cfg.QuicConnections == 0) {
        err = "quic connections must be greater than zero";
        return false;
    }

    std::vector<TqEndpoint> peers;
    if (!TqParseEndpointList(cfg.QuicPeer, peers, err)) {
        if (err.empty()) {
            err = "invalid quic peer list";
        }
        return false;
    }
    if (peers.empty()) {
        err = "quic peer list is empty";
        return false;
    }

    expanded.reserve(cfg.QuicConnections);
    for (uint32_t i = 0; i < cfg.QuicConnections; ++i) {
        const TqEndpoint& endpoint = peers[i % peers.size()];
        expanded.push_back(TqClientSlotPath{
            "default",
            "",
            endpoint.Host,
            endpoint.Port,
            TqFormatEndpoint(endpoint)});
    }

    slots = std::move(expanded);
    return true;
}
