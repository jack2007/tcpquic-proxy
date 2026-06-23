#include "router_runtime.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>
#include <unordered_set>

namespace {

constexpr size_t kMaxPortForwardTargetHostLength = 255;

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (char ch : value) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                static const char Hex[] = "0123456789abcdef";
                const unsigned char v = static_cast<unsigned char>(ch);
                out += "\\u00";
                out.push_back(Hex[v >> 4]);
                out.push_back(Hex[v & 0x0f]);
            } else {
                out.push_back(ch);
            }
            break;
        }
    }
    return out;
}

void AppendJsonString(std::ostringstream& out, const char* name, const std::string& value) {
    out << '"' << name << "\":\"" << JsonEscape(value) << '"';
}

bool IsHostPort(const std::string& value) {
    size_t portStart = std::string::npos;
    if (!value.empty() && value[0] == '[') {
        const size_t close = value.find(']');
        if (close == std::string::npos || close == 1 || close + 2 > value.size() || value[close + 1] != ':') {
            return false;
        }
        portStart = close + 2;
    } else {
        const size_t colon = value.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size() || value.find(':', colon + 1) != std::string::npos) {
            return false;
        }
        portStart = colon + 1;
    }

    uint32_t port = 0;
    for (size_t i = portStart; i < value.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(value[i]))) {
            return false;
        }
        const uint32_t digit = static_cast<uint32_t>(value[i] - '0');
        if (port > 6553 || (port == 6553 && digit > 5)) {
            return false;
        }
        port = port * 10 + digit;
    }
    return port != 0;
}

std::string PortForwardTargetText(const TqPortForwardConfig& forward) {
    std::ostringstream out;
    if (forward.TargetHost.find(':') != std::string::npos) {
        out << '[' << forward.TargetHost << ']';
    } else {
        out << forward.TargetHost;
    }
    out << ':' << forward.TargetPort;
    return out.str();
}

void AppendPortForwardsJson(std::ostringstream& out, const std::vector<TqPortForwardConfig>& forwards) {
    out << ",\"port_forwards\":[";
    for (size_t i = 0; i < forwards.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << '{';
        AppendJsonString(out, "listen", forwards[i].Listen);
        out << ',';
        AppendJsonString(out, "target", PortForwardTargetText(forwards[i]));
        out << '}';
    }
    out << ']';
}

void AppendPeerConfigJson(std::ostringstream& out, const TqPeerConfig& peer) {
    out << '{';
    AppendJsonString(out, "peer_id", peer.PeerId);
    out << ',';
    AppendJsonString(out, "quic_peer", peer.QuicPeer);
    out << ',';
    AppendJsonString(out, "socks_listen", peer.SocksListen);
    out << ',';
    AppendJsonString(out, "http_listen", peer.HttpListen);
    AppendPortForwardsJson(out, peer.PortForwards);
    if (peer.QuicConnections != 0) {
        out << ",\"quic_connections\":" << peer.QuicConnections;
    }
    out << ',';
    AppendJsonString(out, "compress", peer.Compress);
    out << ",\"enabled\":" << (peer.Enabled ? "true" : "false");
    out << '}';
}

void AppendPeerMetricsJson(std::ostringstream& out, const TqPeerMetrics& peer) {
    out << '{';
    AppendJsonString(out, "peer_id", peer.PeerId);
    out << ",\"enabled\":" << (peer.Enabled ? "true" : "false") << ',';
    AppendJsonString(out, "quic_peer", peer.QuicPeer);
    out << ',';
    AppendJsonString(out, "socks_listen", peer.SocksListen);
    out << ',';
    AppendJsonString(out, "http_listen", peer.HttpListen);
    AppendPortForwardsJson(out, peer.PortForwards);
    out << ',';
    AppendJsonString(out, "state", peer.State);
    out << ",\"connection_count\":" << peer.ConnectionCount;
    out << ",\"connected_connections\":" << peer.ConnectedConnections;
    out << ",\"active_streams\":" << peer.ActiveStreams;
    out << ",\"total_streams\":" << peer.TotalStreams;
    out << ",\"reconnects\":" << peer.Reconnects;
    out << ',';
    AppendJsonString(out, "last_error", peer.LastError);
    out << ',';
    AppendJsonString(out, "last_connected_at", peer.LastConnectedAt);
    out << '}';
}

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

void AppendUtf8(uint32_t value, std::string& out) {
    if (value <= 0x7F) {
        out.push_back(static_cast<char>(value));
    } else if (value <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (value >> 6)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (value >> 12)));
        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
    }
}

class RouterJsonParser {
public:
    RouterJsonParser(const std::string& text, std::string& err) : Text(text), Err(err) {}

    bool Parse(TqRouterConfig& router) {
        router = TqRouterConfig{};
        if (!Consume('{')) return Error("config must be a JSON object");
        if (Consume('}')) return Finish(router);
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed config object");
            if (key == "version") {
                if (!ParseUint32(router.Version)) return Error("invalid version");
            } else if (key == "peers") {
                if (!ParsePeers(router.Peers)) return false;
            } else if (!SkipValue()) {
                return false;
            }
        } while (Consume(','));
        if (!Consume('}')) return Error("malformed config object");
        return Finish(router);
    }

private:
    bool Finish(TqRouterConfig& router) {
        SkipWs();
        if (Pos != Text.size()) return Error("unexpected trailing content in config");
        return TqValidateRouterConfig(router, Err);
    }

    bool ParsePeers(std::vector<TqPeerConfig>& peers) {
        peers.clear();
        if (!Consume('[')) return Error("peers must be an array");
        if (Consume(']')) return true;
        do {
            TqPeerConfig peer;
            if (!ParsePeer(peer)) return false;
            peers.push_back(peer);
        } while (Consume(','));
        return Consume(']') || Error("malformed peers array");
    }

    bool ParsePeer(TqPeerConfig& peer) {
        if (!Consume('{')) return Error("peer must be an object");
        if (Consume('}')) return true;
        bool quicConnectionsSpecified = false;
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed peer object");
            if (key == "peer_id") {
                if (!ParseStringField(peer.PeerId, "invalid peer_id")) return false;
            } else if (key == "quic_peer") {
                if (!ParseStringField(peer.QuicPeer, "invalid quic_peer")) return false;
            } else if (key == "socks_listen") {
                if (!ParseStringField(peer.SocksListen, "invalid socks_listen")) return false;
            } else if (key == "http_listen") {
                if (!ParseStringField(peer.HttpListen, "invalid http_listen")) return false;
            } else if (key == "port_forwards") {
                if (!ParsePortForwards(peer.PortForwards)) return false;
            } else if (key == "quic_connections") {
                quicConnectionsSpecified = true;
                if (!ParseUint32(peer.QuicConnections)) return Error("invalid quic_connections");
            } else if (key == "quic_reconnect_interval_ms") {
                return Error("unknown peer key: quic_reconnect_interval_ms");
            } else if (key == "compress") {
                if (!ParseStringField(peer.Compress, "invalid compress")) return false;
            } else if (key == "enabled") {
                if (!ParseBool(peer.Enabled)) return Error("invalid enabled");
            } else if (!SkipValue()) {
                return false;
            }
        } while (Consume(','));
        if (quicConnectionsSpecified && peer.QuicConnections == 0) return Error("quic_connections out of range");
        return Consume('}') || Error("malformed peer object");
    }

    bool ParsePortForwardPort(const std::string& value, size_t portStart, uint16_t& port) {
        if (portStart >= value.size()) {
            return false;
        }
        uint32_t parsedPort = 0;
        for (size_t i = portStart; i < value.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(value[i]))) {
                return false;
            }
            const uint32_t digit = static_cast<uint32_t>(value[i] - '0');
            if (parsedPort > 6553 || (parsedPort == 6553 && digit > 5)) {
                return false;
            }
            parsedPort = parsedPort * 10 + digit;
        }
        if (parsedPort == 0) {
            return false;
        }
        port = static_cast<uint16_t>(parsedPort);
        return true;
    }

    bool ParsePortForwardTargetText(const std::string& target, std::string& host, uint16_t& port) {
        host.clear();
        port = 0;

        size_t portStart = std::string::npos;
        if (!target.empty() && target[0] == '[') {
            const size_t close = target.find(']');
            if (close == std::string::npos || close == 1 || close + 2 > target.size() || target[close + 1] != ':') {
                return false;
            }
            host = target.substr(1, close - 1);
            portStart = close + 2;
        } else {
            const size_t colon = target.find(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= target.size() ||
                target.find(':', colon + 1) != std::string::npos) {
                return false;
            }
            host = target.substr(0, colon);
            portStart = colon + 1;
        }

        if (host.empty() || host.size() > kMaxPortForwardTargetHostLength ||
            !ParsePortForwardPort(target, portStart, port)) {
            host.clear();
            port = 0;
            return false;
        }
        return true;
    }

    bool ParsePortForwards(std::vector<TqPortForwardConfig>& forwards) {
        forwards.clear();
        if (!Consume('[')) return Error("port_forwards must be an array");
        if (Consume(']')) return true;
        do {
            TqPortForwardConfig forward;
            if (!ParsePortForwardObject(forward)) return false;
            forwards.push_back(forward);
        } while (Consume(','));
        return Consume(']') || Error("malformed port_forwards array");
    }

    bool ParsePortForwardObject(TqPortForwardConfig& forward) {
        if (!Consume('{')) return Error("port_forward must be an object");
        bool hasListen = false;
        bool hasTarget = false;
        if (Consume('}')) return Error("port_forward listen and target are required");
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed port_forward object");
            if (key == "listen") {
                hasListen = true;
                if (!ParseString(forward.Listen)) return Error("invalid port_forward.listen");
                if (!IsHostPort(forward.Listen)) return Error("invalid port_forward.listen");
            } else if (key == "target") {
                hasTarget = true;
                std::string target;
                if (!ParseString(target)) return Error("invalid port_forward.target");
                if (!ParsePortForwardTargetText(target, forward.TargetHost, forward.TargetPort)) {
                    return Error("invalid port_forward.target");
                }
            } else {
                return Error("unknown port_forward key: " + key);
            }
        } while (Consume(','));
        if (!Consume('}')) return Error("malformed port_forward object");
        if (!hasListen || !hasTarget) return Error("port_forward listen and target are required");
        return true;
    }

    bool ParseStringField(std::string& out, const std::string& err) {
        if (ParseString(out)) return true;
        return !Err.empty() ? false : Error(err);
    }

    bool ParseString(std::string& out) {
        SkipWs();
        if (Pos >= Text.size() || Text[Pos] != '"') return false;
        ++Pos;
        out.clear();
        while (Pos < Text.size()) {
            char ch = Text[Pos++];
            if (ch == '"') return true;
            if (ch == '\\') {
                if (Pos >= Text.size()) return false;
                char escaped = Text[Pos++];
                switch (escaped) {
                case '"': case '\\': case '/': out.push_back(escaped); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (Pos + 4 > Text.size()) return false;
                    uint32_t value = 0;
                    for (int i = 0; i < 4; ++i) {
                        int hex = HexValue(Text[Pos++]);
                        if (hex < 0) return false;
                        value = (value << 4) | static_cast<uint32_t>(hex);
                    }
                    if (value >= 0xD800 && value <= 0xDFFF) return Error("unicode surrogate escapes are not supported");
                    AppendUtf8(value, out);
                    break;
                }
                default: return false;
                }
            } else {
                if (static_cast<unsigned char>(ch) < 0x20) return false;
                out.push_back(ch);
            }
        }
        return false;
    }

    bool ParseUint32(uint32_t& out) {
        SkipWs();
        if (Pos >= Text.size() || !std::isdigit(static_cast<unsigned char>(Text[Pos]))) return false;
        if (Text[Pos] == '0' && Pos + 1 < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Pos + 1]))) return false;
        uint64_t value = 0;
        while (Pos < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Pos]))) {
            value = (value * 10) + static_cast<uint32_t>(Text[Pos] - '0');
            if (value > UINT32_MAX) return false;
            ++Pos;
        }
        out = static_cast<uint32_t>(value);
        return true;
    }

    bool ParseBool(bool& out) {
        SkipWs();
        if (Text.compare(Pos, 4, "true") == 0) { Pos += 4; out = true; return true; }
        if (Text.compare(Pos, 5, "false") == 0) { Pos += 5; out = false; return true; }
        return false;
    }

    bool SkipValue() {
        SkipWs();
        if (Pos >= Text.size()) return Error("unexpected end of config");
        if (Text[Pos] == '"') {
            std::string ignored;
            return ParseString(ignored) || Error("invalid string value");
        }
        if (Text[Pos] == '{') {
            ++Pos;
            if (Consume('}')) return true;
            do {
                std::string ignored;
                if (!ParseString(ignored) || !Consume(':') || !SkipValue()) return Error("malformed object value");
            } while (Consume(','));
            return Consume('}') || Error("malformed object value");
        }
        if (Text[Pos] == '[') {
            ++Pos;
            if (Consume(']')) return true;
            do {
                if (!SkipValue()) return false;
            } while (Consume(','));
            return Consume(']') || Error("malformed array value");
        }
        if (std::isdigit(static_cast<unsigned char>(Text[Pos]))) {
            uint32_t ignored = 0;
            return ParseUint32(ignored) || Error("invalid number value");
        }
        if (Text.compare(Pos, 4, "true") == 0) { Pos += 4; return true; }
        if (Text.compare(Pos, 5, "false") == 0) { Pos += 5; return true; }
        if (Text.compare(Pos, 4, "null") == 0) { Pos += 4; return true; }
        return Error("invalid value");
    }

    bool Consume(char expected) {
        SkipWs();
        if (Pos >= Text.size() || Text[Pos] != expected) return false;
        ++Pos;
        return true;
    }

    void SkipWs() {
        while (Pos < Text.size() && std::isspace(static_cast<unsigned char>(Text[Pos]))) ++Pos;
    }

    bool Error(const std::string& err) {
        Err = err;
        return false;
    }

    const std::string& Text;
    std::string& Err;
    size_t Pos{0};
};

std::string ErrorJson(const std::string& err) {
    std::ostringstream out;
    out << "{\"error\":\"" << JsonEscape(err) << "\"}";
    return out.str();
}

bool SetPeerEnabled(TqRouterConfig& config, const std::string& peerId, bool enabled) {
    for (auto& peer : config.Peers) {
        if (peer.PeerId == peerId) {
            peer.Enabled = enabled;
            return true;
        }
    }
    return false;
}

const TqPeerConfig* FindSingleEnabledPeer(const TqRouterConfig& config, size_t& enabledPeers) {
    const TqPeerConfig* enabledPeer = nullptr;
    enabledPeers = 0;
    for (const auto& peer : config.Peers) {
        if (peer.Enabled) {
            ++enabledPeers;
            enabledPeer = &peer;
        }
    }
    return enabledPeers == 1 ? enabledPeer : nullptr;
}

bool SamePortForwards(const std::vector<TqPortForwardConfig>& a, const std::vector<TqPortForwardConfig>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].Listen != b[i].Listen ||
            a[i].TargetHost != b[i].TargetHost ||
            a[i].TargetPort != b[i].TargetPort) {
            return false;
        }
    }
    return true;
}

bool SameBridgeActivePeer(const TqPeerConfig& a, const TqPeerConfig& b) {
    return a.PeerId == b.PeerId &&
        a.QuicPeer == b.QuicPeer &&
        a.SocksListen == b.SocksListen &&
        a.HttpListen == b.HttpListen &&
        SamePortForwards(a.PortForwards, b.PortForwards) &&
        a.QuicConnections == b.QuicConnections &&
        a.Compress == b.Compress;
}

bool PeerDataPlaneChanged(const TqPeerConfig& a, const TqPeerConfig& b) {
    return a.QuicPeer != b.QuicPeer ||
        a.SocksListen != b.SocksListen ||
        a.HttpListen != b.HttpListen ||
        !SamePortForwards(a.PortForwards, b.PortForwards) ||
        a.QuicConnections != b.QuicConnections ||
        a.Compress != b.Compress;
}

} // namespace

TqRouterRuntime::TqRouterRuntime(TqPeerRuntimeAdapter* adapter) : Adapter(adapter) {}

TqRouterRuntime::TqRouterRuntime(bool bridgeValidationMode) : BridgeValidationMode(bridgeValidationMode) {}

bool TqRouterRuntime::ApplyConfig(const TqRouterConfig& config, std::string& err) {
    if (!TqValidateRouterConfig(config, err)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(Mutex);
    if (BridgeValidationMode) {
        size_t enabledPeers = 0;
        const TqPeerConfig* enabledPeer = FindSingleEnabledPeer(config, enabledPeers);
        if (enabledPeers > 1) {
            err = "multiple enabled peers require the Task 5 router adapter";
            return false;
        }
        if (!BridgeStartupCaptured) {
            if (enabledPeer != nullptr) {
                BridgeActivePeer = *enabledPeer;
                BridgeStartupCaptured = true;
            }
        } else if (enabledPeer == nullptr || !SameBridgeActivePeer(*enabledPeer, BridgeActivePeer)) {
            err = "bridge mode cannot change the single active peer after startup";
            return false;
        }
    }

    constexpr uint32_t DrainGraceSeconds = 10;
    std::unordered_set<std::string> present;
    for (const auto& peer : config.Peers) {
        present.insert(peer.PeerId);
        auto runningIt = RunningPeers.find(peer.PeerId);
        const TqPeerConfig* runningPeer = runningIt == RunningPeers.end() ? nullptr : &runningIt->second;
        const bool newPeer = runningPeer == nullptr;
        const bool changedPeer = runningPeer != nullptr && PeerDataPlaneChanged(*runningPeer, peer);

        std::string adapterErr;
        bool startOk = true;
        if (Adapter != nullptr) {
            if (runningPeer != nullptr && (!peer.Enabled || changedPeer)) {
                const std::string runningPeerId = runningPeer->PeerId;
                Adapter->StopAccepting(runningPeerId);
                Adapter->AbortPeerTunnels(runningPeerId);
                Adapter->DrainPeer(runningPeerId, DrainGraceSeconds);
                RunningPeers.erase(runningPeerId);
                runningPeer = nullptr;
            }
            if (peer.Enabled && (newPeer || changedPeer)) {
                startOk = Adapter->StartPeer(peer, adapterErr);
                if (startOk) {
                    RunningPeers[peer.PeerId] = peer;
                }
            }
        } else if (peer.Enabled) {
            RunningPeers[peer.PeerId] = peer;
        } else {
            RunningPeers.erase(peer.PeerId);
        }

        auto& metrics = Metrics[peer.PeerId];
        metrics.PeerId = peer.PeerId;
        metrics.Enabled = peer.Enabled;
        metrics.QuicPeer = peer.QuicPeer;
        metrics.SocksListen = peer.SocksListen;
        metrics.HttpListen = peer.HttpListen;
        metrics.PortForwards = peer.PortForwards;
        metrics.LastError = adapterErr;
        if (!peer.Enabled) {
            metrics.State = "disabled";
        } else if (!startOk) {
            metrics.State = "down";
        } else {
            metrics.State = "connecting";
        }
    }

    for (auto& item : Metrics) {
        if (present.find(item.first) == present.end()) {
            auto runningIt = RunningPeers.find(item.first);
            if (runningIt != RunningPeers.end() && Adapter != nullptr) {
                const std::string runningPeerId = runningIt->second.PeerId;
                Adapter->StopAccepting(runningPeerId);
                Adapter->AbortPeerTunnels(runningPeerId);
                Adapter->DrainPeer(runningPeerId, DrainGraceSeconds);
            }
            RunningPeers.erase(item.first);
            item.second.State = "draining";
            item.second.Enabled = false;
        }
    }

    Config = config;
    return true;
}

TqRouterConfig TqRouterRuntime::SnapshotConfig() const {
    std::lock_guard<std::mutex> lock(Mutex);
    return Config;
}

TqRouterMetrics TqRouterRuntime::SnapshotMetrics() const {
    std::lock_guard<std::mutex> lock(Mutex);
    TqRouterMetrics snapshot;
    snapshot.UptimeSeconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - Started).count());
    snapshot.Peers.reserve(Metrics.size());
    for (const auto& item : Metrics) {
        TqPeerMetrics peer = item.second;
        if (Adapter != nullptr && RunningPeers.find(item.first) != RunningPeers.end()) {
            TqPeerMetrics live;
            if (Adapter->SnapshotPeerMetrics(item.first, live)) {
                peer.ConnectionCount = live.ConnectionCount;
                peer.ConnectedConnections = live.ConnectedConnections;
                peer.ActiveStreams = live.ActiveStreams;
                peer.TotalStreams = live.TotalStreams;
                peer.Reconnects = live.Reconnects;
                peer.LastError = live.LastError;
                peer.LastConnectedAt = live.LastConnectedAt;
                peer.State = live.State;
                peer.PortForwards = item.second.PortForwards;
            }
        }
        snapshot.Peers.push_back(peer);
    }
    std::sort(snapshot.Peers.begin(), snapshot.Peers.end(), [](const TqPeerMetrics& a, const TqPeerMetrics& b) {
        return a.PeerId < b.PeerId;
    });
    return snapshot;
}

std::string TqRouterRuntime::ConfigJson() const {
    const TqRouterConfig config = SnapshotConfig();
    std::ostringstream out;
    out << "{\"version\":" << config.Version << ",\"peers\":[";
    for (size_t i = 0; i < config.Peers.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        AppendPeerConfigJson(out, config.Peers[i]);
    }
    out << "]}";
    return out.str();
}

std::string TqRouterRuntime::MetricsJson() const {
    const TqRouterMetrics metrics = SnapshotMetrics();
    std::ostringstream out;
    out << '{';
    AppendJsonString(out, "role", metrics.Role);
    out << ',';
    AppendJsonString(out, "status", metrics.Status);
    out << ",\"uptime_seconds\":" << metrics.UptimeSeconds << ",\"peers\":[";
    for (size_t i = 0; i < metrics.Peers.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        AppendPeerMetricsJson(out, metrics.Peers[i]);
    }
    out << "]}";
    return out.str();
}

std::string TqRouterRuntime::HealthJson() const {
    const TqRouterMetrics metrics = SnapshotMetrics();
    std::ostringstream out;
    out << '{';
    AppendJsonString(out, "role", metrics.Role);
    out << ',';
    AppendJsonString(out, "status", metrics.Status);
    out << ",\"uptime_seconds\":" << metrics.UptimeSeconds;
    out << '}';
    return out.str();
}

std::string TqRouterRuntime::HandleAdmin(const TqHttpRequest& req) {
    if (req.Method == "GET" && req.Path == "/health") {
        return TqJsonResponse(200, HealthJson());
    }
    if (req.Method == "GET" && req.Path == "/metrics") {
        return TqJsonResponse(200, MetricsJson());
    }
    if (req.Method == "GET" && req.Path == "/config") {
        return TqJsonResponse(200, ConfigJson());
    }
    if (req.Method == "PUT" && req.Path == "/config") {
        TqRouterConfig config;
        std::string err;
        RouterJsonParser parser(req.Body, err);
        if (!parser.Parse(config) || !ApplyConfig(config, err)) {
            return TqJsonResponse(400, ErrorJson(err));
        }
        return TqJsonResponse(200, ConfigJson());
    }

    constexpr const char* PeerPrefix = "/peers/";
    constexpr const char* EnableSuffix = "/enable";
    constexpr const char* DisableSuffix = "/disable";
    if (req.Method == "POST" && req.Path.compare(0, std::char_traits<char>::length(PeerPrefix), PeerPrefix) == 0) {
        const bool enable = req.Path.size() > std::char_traits<char>::length(EnableSuffix) &&
            req.Path.compare(req.Path.size() - std::char_traits<char>::length(EnableSuffix), std::char_traits<char>::length(EnableSuffix), EnableSuffix) == 0;
        const bool disable = req.Path.size() > std::char_traits<char>::length(DisableSuffix) &&
            req.Path.compare(req.Path.size() - std::char_traits<char>::length(DisableSuffix), std::char_traits<char>::length(DisableSuffix), DisableSuffix) == 0;
        if (enable || disable) {
            const size_t idStart = std::char_traits<char>::length(PeerPrefix);
            const size_t idEnd = req.Path.size() - std::char_traits<char>::length(enable ? EnableSuffix : DisableSuffix);
            TqRouterConfig config = SnapshotConfig();
            if (!SetPeerEnabled(config, req.Path.substr(idStart, idEnd - idStart), enable)) {
                return TqJsonResponse(404, ErrorJson("not found"));
            }
            std::string err;
            if (!ApplyConfig(config, err)) {
                return TqJsonResponse(400, ErrorJson(err));
            }
            return TqJsonResponse(200, ConfigJson());
        }
    }

    return TqJsonResponse(404, ErrorJson("not found"));
}

bool TqValidateSinglePeerStartupBridge(const TqRouterConfig& config, std::string& err) {
    size_t enabledPeers = 0;
    for (const auto& peer : config.Peers) {
        if (peer.Enabled) {
            ++enabledPeers;
        }
    }
    if (enabledPeers > 1) {
        err = "multiple enabled peers require the Task 5 router adapter";
        return false;
    }
    err.clear();
    return true;
}
