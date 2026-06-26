#include "router_runtime.h"

#include "admin_memory.h"
#include "relay_metrics.h"
#include "tunnel_registry.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <sstream>
#include <unordered_set>

namespace {

constexpr size_t kMaxPortForwardTargetHostLength = 255;
constexpr uint32_t kDefaultPeerDrainGraceSeconds = 10;
constexpr uint32_t kMaxQuicConnections = 128;

struct TqPeerPatch {
    bool HasQuicPeer{false};
    bool HasSocksListen{false};
    bool HasHttpListen{false};
    bool HasPortForwards{false};
    bool HasQuicConnections{false};
    bool HasCompress{false};
    bool HasEnabled{false};
    std::string QuicPeer;
    std::string SocksListen;
    std::string HttpListen;
    std::vector<TqPortForwardConfig> PortForwards;
    uint32_t QuicConnections{0};
    std::string Compress;
    bool Enabled{false};
};

struct TqPeerAdminPath {
    bool Collection{false};
    std::string PeerId;
    std::string Action;
};

struct TqConnectionAdminPath {
    std::string PeerId;
    bool Collection{false};
    std::string ConnectionId;
    std::string Action;
};

struct TqTunnelAdminPath {
    bool Collection{false};
    std::string TunnelId;
    std::string Action;
};

struct TqPeerActionBody {
    std::string Mode{"reject-if-active"};
    uint32_t GraceSeconds{kDefaultPeerDrainGraceSeconds};
};

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

std::string PeerMetricsJson(const TqPeerMetrics& peer) {
    std::ostringstream out;
    AppendPeerMetricsJson(out, peer);
    return out.str();
}

std::string PeersJson(const std::vector<TqPeerMetrics>& peers) {
    std::ostringstream out;
    out << "{\"peers\":[";
    for (size_t i = 0; i < peers.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        AppendPeerMetricsJson(out, peers[i]);
    }
    out << "]}";
    return out.str();
}

void AppendConnectionJson(std::ostringstream& out, const TqConnectionSnapshot& connection) {
    out << '{';
    AppendJsonString(out, "connection_id", connection.ConnectionId);
    out << ",\"slot_index\":" << connection.SlotIndex;
    out << ",\"generation\":" << connection.Generation;
    out << ",\"connected\":" << (connection.Connected ? "true" : "false");
    out << ",\"retry_scheduled\":" << (connection.RetryScheduled ? "true" : "false");
    out << ',';
    AppendJsonString(out, "state", connection.State);
    out << ",\"active_tunnels\":" << connection.ActiveTunnels;
    out << ',';
    AppendJsonString(out, "last_error", connection.LastError);
    out << '}';
}

std::string ConnectionJson(const TqConnectionSnapshot& connection) {
    std::ostringstream out;
    AppendConnectionJson(out, connection);
    return out.str();
}

std::string ConnectionsJson(const std::vector<TqConnectionSnapshot>& connections) {
    std::ostringstream out;
    out << "{\"connections\":[";
    for (size_t i = 0; i < connections.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        AppendConnectionJson(out, connections[i]);
    }
    out << "]}";
    return out.str();
}

void AppendTunnelJson(std::ostringstream& out, const TqTunnelSnapshot& tunnel) {
    out << '{';
    AppendJsonString(out, "tunnel_id", tunnel.TunnelId);
    out << ',';
    AppendJsonString(out, "peer_id", tunnel.PeerId);
    out << ',';
    AppendJsonString(out, "connection_id", tunnel.ConnectionId);
    out << ',';
    AppendJsonString(out, "state", tunnel.State);
    out << ',';
    AppendJsonString(out, "target", tunnel.Target);
    out << ',';
    AppendJsonString(out, "role", tunnel.Role);
    out << ',';
    AppendJsonString(out, "ingress", tunnel.Ingress);
    out << ',';
    AppendJsonString(out, "compress", tunnel.Compress);
    out << ',';
    AppendJsonString(out, "created_at", tunnel.CreatedAt);
    out << ",\"duration_ms\":" << tunnel.DurationMs;
    out << ",\"tcp_read_bytes\":" << tunnel.TcpReadBytes;
    out << ",\"tcp_write_bytes\":" << tunnel.TcpWriteBytes;
    out << ",\"pending_bytes\":" << tunnel.PendingBytes;
    out << ',';
    AppendJsonString(out, "relay_backend", tunnel.RelayBackend);
    out << ",\"worker_index\":" << tunnel.WorkerIndex;
    out << ',';
    AppendJsonString(out, "last_error", tunnel.LastError);
    out << '}';
}

std::string TunnelJson(const TqTunnelSnapshot& tunnel) {
    std::ostringstream out;
    AppendTunnelJson(out, tunnel);
    return out.str();
}

std::string TunnelsJson(const std::vector<TqTunnelSnapshot>& tunnels) {
    std::ostringstream out;
    out << "{\"tunnels\":[";
    for (size_t i = 0; i < tunnels.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        AppendTunnelJson(out, tunnels[i]);
    }
    out << "]}";
    return out.str();
}

std::string RelayMetricsJson() {
    std::ostringstream out;
    out << '{';
    const auto metrics = TqSnapshotRelayMetrics();
    AppendJsonString(out, "backend", metrics.Backend);
    out << ",\"active_relays\":" << metrics.ActiveRelays;
    out << ",\"pending_bytes\":" << metrics.PendingBytes;
    out << ",\"tcp_read_bytes\":" << metrics.TcpReadBytes;
    out << ",\"tcp_write_bytes\":" << metrics.TcpWriteBytes;
    out << ",\"errors\":" << metrics.Errors;
    TqAppendRelayMetricsJson(out, metrics);
    out << '}';
    return out.str();
}

std::string RelayWorkersJson() {
    const auto metrics = TqSnapshotRelayMetrics();
    std::ostringstream out;
    out << "{\"workers\":[{\"worker_id\":\"aggregate\",";
    AppendJsonString(out, "backend", metrics.Backend);
    out << ",\"active_relays\":" << metrics.ActiveRelays;
    out << ",\"pending_bytes\":" << metrics.PendingBytes;
    out << ",\"tcp_read_bytes\":" << metrics.TcpReadBytes;
    out << ",\"tcp_write_bytes\":" << metrics.TcpWriteBytes;
    out << "}]}";
    return out.str();
}

std::string RelayWorkerJson(const std::string& workerId) {
    const auto metrics = TqSnapshotRelayMetrics();
    std::ostringstream out;
    out << '{';
    AppendJsonString(out, "worker_id", workerId);
    out << ',';
    AppendJsonString(out, "backend", metrics.Backend);
    out << ",\"active_relays\":" << metrics.ActiveRelays;
    out << ",\"pending_bytes\":" << metrics.PendingBytes;
    out << ",\"tcp_read_bytes\":" << metrics.TcpReadBytes;
    out << ",\"tcp_write_bytes\":" << metrics.TcpWriteBytes;
    out << '}';
    return out.str();
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

    bool ParsePatch(TqPeerPatch& patch) {
        patch = TqPeerPatch{};
        if (!Consume('{')) return Error("peer patch must be an object");
        if (Consume('}')) return FinishPatch();
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed peer patch object");
            if (key == "peer_id") {
                std::string ignored;
                if (!ParseStringField(ignored, "invalid peer_id")) return false;
            } else if (key == "quic_peer") {
                patch.HasQuicPeer = true;
                if (!ParseStringField(patch.QuicPeer, "invalid quic_peer")) return false;
            } else if (key == "socks_listen") {
                patch.HasSocksListen = true;
                if (!ParseStringField(patch.SocksListen, "invalid socks_listen")) return false;
            } else if (key == "http_listen") {
                patch.HasHttpListen = true;
                if (!ParseStringField(patch.HttpListen, "invalid http_listen")) return false;
            } else if (key == "port_forwards") {
                patch.HasPortForwards = true;
                if (!ParsePortForwards(patch.PortForwards)) return false;
            } else if (key == "quic_connections") {
                patch.HasQuicConnections = true;
                if (!ParseUint32(patch.QuicConnections)) return Error("invalid quic_connections");
                if (patch.QuicConnections == 0) return Error("quic_connections out of range");
            } else if (key == "compress") {
                patch.HasCompress = true;
                if (!ParseStringField(patch.Compress, "invalid compress")) return false;
            } else if (key == "enabled") {
                patch.HasEnabled = true;
                if (!ParseBool(patch.Enabled)) return Error("invalid enabled");
            } else if (!SkipValue()) {
                return false;
            }
        } while (Consume(','));
        if (!Consume('}')) return Error("malformed peer patch object");
        return FinishPatch();
    }

    bool ParsePeerActionBody(TqPeerActionBody& body) {
        body = TqPeerActionBody{};
        if (!Consume('{')) return Error("peer action body must be an object");
        if (Consume('}')) return FinishPatch();
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed peer action body");
            if (key == "mode") {
                if (!ParseStringField(body.Mode, "invalid mode")) return false;
            } else if (key == "grace_seconds") {
                if (!ParseUint32(body.GraceSeconds)) return Error("invalid grace_seconds");
            } else if (!SkipValue()) {
                return false;
            }
        } while (Consume(','));
        if (!Consume('}')) return Error("malformed peer action body");
        return FinishPatch();
    }

private:
    bool Finish(TqRouterConfig& router) {
        SkipWs();
        if (Pos != Text.size()) return Error("unexpected trailing content in config");
        return TqValidateRouterConfig(router, Err);
    }

    bool FinishPatch() {
        SkipWs();
        if (Pos != Text.size()) return Error("unexpected trailing content in peer patch");
        return true;
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

bool DecodePathSegment(const std::string& encoded, std::string& decoded) {
    decoded.clear();
    decoded.reserve(encoded.size());
    for (size_t i = 0; i < encoded.size(); ++i) {
        const char ch = encoded[i];
        if (ch == '/') {
            return false;
        }
        if (ch != '%') {
            decoded.push_back(ch);
            continue;
        }
        if (i + 2 >= encoded.size()) {
            return false;
        }
        const int hi = HexValue(encoded[i + 1]);
        const int lo = HexValue(encoded[i + 2]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        decoded.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return !decoded.empty();
}

bool ParsePeerAdminPath(const std::string& path, TqPeerAdminPath& out) {
    out = TqPeerAdminPath{};
    constexpr const char* PeerPrefix = "/peers";
    if (path == PeerPrefix) {
        out.Collection = true;
        return true;
    }
    constexpr size_t peerPrefixLen = std::char_traits<char>::length(PeerPrefix);
    if (path.compare(0, peerPrefixLen + 1, "/peers/") != 0) {
        return false;
    }

    std::string tail = path.substr(peerPrefixLen + 1);
    const struct LegacyAction {
        const char* Suffix;
        const char* Action;
    } legacyActions[] = {
        {"/enable", "enable"},
        {"/disable", "disable"},
    };
    for (const auto& legacy : legacyActions) {
        const size_t suffixLen = std::char_traits<char>::length(legacy.Suffix);
        if (tail.size() > suffixLen && tail.compare(tail.size() - suffixLen, suffixLen, legacy.Suffix) == 0) {
            out.Action = legacy.Action;
            tail.resize(tail.size() - suffixLen);
            return DecodePathSegment(tail, out.PeerId);
        }
    }

    const size_t actionPos = tail.find(':');
    if (actionPos != std::string::npos) {
        out.Action = tail.substr(actionPos + 1);
        tail.resize(actionPos);
    }
    return DecodePathSegment(tail, out.PeerId);
}

bool ParseConnectionAdminPath(const std::string& path, TqConnectionAdminPath& out) {
    out = TqConnectionAdminPath{};
    constexpr const char* PeerPrefix = "/peers/";
    constexpr size_t peerPrefixLen = std::char_traits<char>::length(PeerPrefix);
    if (path.compare(0, peerPrefixLen, PeerPrefix) != 0) {
        return false;
    }

    const size_t connectionMarker = path.find("/connections", peerPrefixLen);
    if (connectionMarker == std::string::npos) {
        return false;
    }
    if (path.compare(connectionMarker, 12, "/connections") != 0) {
        return false;
    }

    const std::string encodedPeer = path.substr(peerPrefixLen, connectionMarker - peerPrefixLen);
    if (!DecodePathSegment(encodedPeer, out.PeerId)) {
        return false;
    }

    const size_t afterConnections = connectionMarker + 12;
    if (afterConnections == path.size()) {
        out.Collection = true;
        return true;
    }
    if (afterConnections >= path.size() || path[afterConnections] != '/') {
        return false;
    }

    std::string tail = path.substr(afterConnections + 1);
    if (tail.empty() || tail.find('/') != std::string::npos) {
        return false;
    }
    const size_t actionPos = tail.find(':');
    if (actionPos != std::string::npos) {
        out.Action = tail.substr(actionPos + 1);
        tail.resize(actionPos);
    }
    return DecodePathSegment(tail, out.ConnectionId);
}

bool ParseTunnelAdminPath(const std::string& path, TqTunnelAdminPath& out) {
    out = TqTunnelAdminPath{};
    constexpr const char* TunnelPrefix = "/tunnels";
    constexpr size_t tunnelPrefixLen = std::char_traits<char>::length(TunnelPrefix);
    if (path == TunnelPrefix) {
        out.Collection = true;
        return true;
    }
    if (path.compare(0, tunnelPrefixLen + 1, "/tunnels/") != 0) {
        return false;
    }
    std::string tail = path.substr(tunnelPrefixLen + 1);
    if (tail.empty() || tail.find('/') != std::string::npos) {
        return false;
    }
    const size_t actionPos = tail.find(':');
    if (actionPos != std::string::npos) {
        out.Action = tail.substr(actionPos + 1);
        tail.resize(actionPos);
    }
    return DecodePathSegment(tail, out.TunnelId);
}

bool ParsePeerConfigBody(const std::string& body, TqPeerConfig& peer, std::string& err) {
    TqRouterConfig wrapper;
    const std::string wrapped = std::string("{\"version\":1,\"peers\":[") + body + "]}";
    RouterJsonParser parser(wrapped, err);
    if (!parser.Parse(wrapper)) {
        return false;
    }
    if (wrapper.Peers.size() != 1) {
        err = "peer body must contain one peer object";
        return false;
    }
    peer = wrapper.Peers[0];
    return true;
}

bool ParsePeerPatchBody(const std::string& body, TqPeerPatch& patch, std::string& err) {
    RouterJsonParser parser(body, err);
    return parser.ParsePatch(patch);
}

std::vector<TqPeerConfig>::iterator FindPeerConfig(std::vector<TqPeerConfig>& peers, const std::string& peerId) {
    return std::find_if(peers.begin(), peers.end(), [&peerId](const TqPeerConfig& peer) {
        return peer.PeerId == peerId;
    });
}

void ApplyPeerPatch(TqPeerConfig& peer, const TqPeerPatch& patch) {
    if (patch.HasQuicPeer) peer.QuicPeer = patch.QuicPeer;
    if (patch.HasSocksListen) peer.SocksListen = patch.SocksListen;
    if (patch.HasHttpListen) peer.HttpListen = patch.HttpListen;
    if (patch.HasPortForwards) peer.PortForwards = patch.PortForwards;
    if (patch.HasQuicConnections) peer.QuicConnections = patch.QuicConnections;
    if (patch.HasCompress) peer.Compress = patch.Compress;
    if (patch.HasEnabled) peer.Enabled = patch.Enabled;
}

bool DeleteModeAllowsEnabledPeer(const std::string& mode) {
    return mode == "drain" || mode == "abort";
}

bool ParsePeerActionBodyText(const std::string& text, TqPeerActionBody& body, std::string& err) {
    body = TqPeerActionBody{};
    if (text.empty()) {
        return true;
    }
    RouterJsonParser parser(text, err);
    return parser.ParsePeerActionBody(body);
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

bool UpdatePeerConnectionCount(TqRouterConfig& config, const std::string& peerId, uint32_t desired) {
    auto it = FindPeerConfig(config.Peers, peerId);
    if (it == config.Peers.end()) {
        return false;
    }
    it->QuicConnections = desired;
    return true;
}

} // namespace

TqRouterRuntime::TqRouterRuntime(TqPeerRuntimeAdapter* adapter) : Adapter(adapter) {}

TqRouterRuntime::TqRouterRuntime(bool bridgeValidationMode) : BridgeValidationMode(bridgeValidationMode) {}

bool TqRouterRuntime::ApplyConfig(const TqRouterConfig& config, std::string& err) {
    std::lock_guard<std::mutex> lock(Mutex);
    return ApplyConfigLocked(config, err);
}

bool TqRouterRuntime::ApplyConfigLocked(const TqRouterConfig& config, std::string& err) {
    if (!TqValidateRouterConfig(config, err)) {
        return false;
    }

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

std::vector<TqPeerMetrics> TqRouterRuntime::ListPeers() const {
    return SnapshotMetrics().Peers;
}

bool TqRouterRuntime::GetPeer(const std::string& peerId, TqPeerMetrics& out) const {
    const auto peers = ListPeers();
    auto it = std::find_if(peers.begin(), peers.end(), [&peerId](const TqPeerMetrics& peer) {
        return peer.PeerId == peerId;
    });
    if (it == peers.end()) {
        return false;
    }
    out = *it;
    return true;
}

bool TqRouterRuntime::CreatePeer(const TqPeerConfig& peer, std::string& err) {
    std::lock_guard<std::mutex> lock(Mutex);
    TqRouterConfig config = Config;
    if (FindPeerConfig(config.Peers, peer.PeerId) != config.Peers.end()) {
        err = "peer already exists";
        return false;
    }
    config.Peers.push_back(peer);
    return ApplyConfigLocked(config, err);
}

bool TqRouterRuntime::ReplacePeer(const std::string& peerId, const TqPeerConfig& peer, std::string& err) {
    if (peer.PeerId != peerId) {
        err = "peer_id does not match path";
        return false;
    }
    std::lock_guard<std::mutex> lock(Mutex);
    TqRouterConfig config = Config;
    auto it = FindPeerConfig(config.Peers, peerId);
    if (it == config.Peers.end()) {
        err = "not found";
        return false;
    }
    *it = peer;
    return ApplyConfigLocked(config, err);
}

bool TqRouterRuntime::PatchPeer(const std::string& peerId, const std::string& body, std::string& err) {
    TqPeerPatch patch;
    if (!ParsePeerPatchBody(body, patch, err)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(Mutex);
    TqRouterConfig config = Config;
    auto it = FindPeerConfig(config.Peers, peerId);
    if (it == config.Peers.end()) {
        err = "not found";
        return false;
    }
    ApplyPeerPatch(*it, patch);
    return ApplyConfigLocked(config, err);
}

bool TqRouterRuntime::DeletePeer(const std::string& peerId, const std::string& mode, std::string& err) {
    std::lock_guard<std::mutex> lock(Mutex);
    TqRouterConfig config = Config;
    auto it = FindPeerConfig(config.Peers, peerId);
    if (it == config.Peers.end()) {
        err = "not found";
        return false;
    }
    if (it->Enabled && !DeleteModeAllowsEnabledPeer(mode)) {
        err = "delete enabled peer requires mode drain or abort";
        return false;
    }
    config.Peers.erase(it);
    return ApplyConfigLocked(config, err);
}

bool TqRouterRuntime::EnablePeer(const std::string& peerId, std::string& err) {
    std::lock_guard<std::mutex> lock(Mutex);
    TqRouterConfig config = Config;
    if (!SetPeerEnabled(config, peerId, true)) {
        err = "not found";
        return false;
    }
    return ApplyConfigLocked(config, err);
}

bool TqRouterRuntime::DisablePeer(const std::string& peerId, std::string& err) {
    std::lock_guard<std::mutex> lock(Mutex);
    TqRouterConfig config = Config;
    if (!SetPeerEnabled(config, peerId, false)) {
        err = "not found";
        return false;
    }
    return ApplyConfigLocked(config, err);
}

bool TqRouterRuntime::DrainPeer(const std::string& peerId, uint32_t graceSeconds, std::string& err) {
    TqPeerMetrics peer;
    if (!GetPeer(peerId, peer)) {
        err = "not found";
        return false;
    }
    if (Adapter != nullptr) {
        Adapter->DrainPeer(peerId, graceSeconds == 0 ? kDefaultPeerDrainGraceSeconds : graceSeconds);
    }
    return true;
}

bool TqRouterRuntime::AbortPeerTunnels(const std::string& peerId, std::string& err) {
    TqPeerMetrics peer;
    if (!GetPeer(peerId, peer)) {
        err = "not found";
        return false;
    }
    if (Adapter != nullptr) {
        Adapter->AbortPeerTunnels(peerId);
    }
    return true;
}

std::vector<TqConnectionSnapshot> TqRouterRuntime::ListConnections(const std::string& peerId) const {
    TqPeerMetrics peer;
    if (!GetPeer(peerId, peer)) {
        return {};
    }
    if (Adapter == nullptr) {
        return {};
    }
    auto connections = Adapter->SnapshotConnections(peerId);
    std::sort(connections.begin(), connections.end(), [](const auto& a, const auto& b) {
        return a.SlotIndex < b.SlotIndex;
    });
    return connections;
}

bool TqRouterRuntime::GetConnection(
    const std::string& peerId,
    const std::string& connectionId,
    TqConnectionSnapshot& out) const {
    const auto connections = ListConnections(peerId);
    auto it = std::find_if(connections.begin(), connections.end(), [&connectionId](const TqConnectionSnapshot& item) {
        return item.ConnectionId == connectionId;
    });
    if (it == connections.end()) {
        return false;
    }
    out = *it;
    return true;
}

bool TqRouterRuntime::AddConnection(const std::string& peerId, std::string& err) {
    std::lock_guard<std::mutex> controlLock(ConnectionControlMutex);
    uint32_t current = 0;
    {
        std::lock_guard<std::mutex> lock(Mutex);
        auto it = FindPeerConfig(Config.Peers, peerId);
        if (it == Config.Peers.end()) {
            err = "not found";
            return false;
        }
        current = it->QuicConnections;
    }
    if (Adapter != nullptr) {
        const auto liveConnections = Adapter->SnapshotConnections(peerId);
        if (!liveConnections.empty() || current == 0) {
            current = static_cast<uint32_t>(liveConnections.size());
        }
    }
    if (current >= kMaxQuicConnections) {
        err = "quic_connections out of range";
        return false;
    }

    const uint32_t desired = current + 1;
    if (Adapter != nullptr && !Adapter->SetDesiredConnectionCount(peerId, desired, err)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(Mutex);
    if (!UpdatePeerConnectionCount(Config, peerId, desired)) {
        err = "not found";
        return false;
    }
    auto running = RunningPeers.find(peerId);
    if (running != RunningPeers.end()) {
        running->second.QuicConnections = desired;
    }
    auto metrics = Metrics.find(peerId);
    if (metrics != Metrics.end()) {
        metrics->second.ConnectionCount = desired;
    }
    return true;
}

bool TqRouterRuntime::DeleteConnection(
    const std::string& peerId,
    const std::string& connectionId,
    std::string& err) {
    std::lock_guard<std::mutex> controlLock(ConnectionControlMutex);
    uint32_t current = 0;
    {
        std::lock_guard<std::mutex> lock(Mutex);
        auto it = FindPeerConfig(Config.Peers, peerId);
        if (it == Config.Peers.end()) {
            err = "not found";
            return false;
        }
        current = it->QuicConnections;
    }
    if (Adapter != nullptr) {
        const auto liveConnections = Adapter->SnapshotConnections(peerId);
        if (!liveConnections.empty() || current == 0) {
            current = static_cast<uint32_t>(liveConnections.size());
        }
    }
    if (current <= 1) {
        err = "cannot delete the last connection";
        return false;
    }

    const uint32_t desired = current - 1;
    if (Adapter != nullptr && !Adapter->StopHighestConnection(peerId, connectionId, err)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(Mutex);
    if (!UpdatePeerConnectionCount(Config, peerId, desired)) {
        err = "not found";
        return false;
    }
    auto running = RunningPeers.find(peerId);
    if (running != RunningPeers.end()) {
        running->second.QuicConnections = desired;
    }
    auto metrics = Metrics.find(peerId);
    if (metrics != Metrics.end()) {
        metrics->second.ConnectionCount = desired;
    }
    return true;
}

bool TqRouterRuntime::ReconnectConnection(
    const std::string& peerId,
    const std::string& connectionId,
    std::string& err) {
    TqPeerMetrics peer;
    if (!GetPeer(peerId, peer)) {
        err = "not found";
        return false;
    }
    if (Adapter == nullptr) {
        err = "connection control is not available";
        return false;
    }
    return Adapter->ReconnectConnection(peerId, connectionId, err);
}

bool TqRouterRuntime::AbortConnectionTunnels(
    const std::string& peerId,
    const std::string& connectionId,
    std::string& err) {
    TqPeerMetrics peer;
    if (!GetPeer(peerId, peer)) {
        err = "not found";
        return false;
    }
    if (Adapter == nullptr) {
        err = "connection control is not available";
        return false;
    }
    return Adapter->AbortConnectionTunnels(peerId, connectionId, err);
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
    std::string response;
    if (TqHandleMemoryAdmin(req, response)) {
        return response;
    }

    if (req.Method == "GET" && req.Path == "/health") {
        return TqJsonResponse(200, HealthJson());
    }
    if (req.Method == "GET" && req.Path == "/metrics") {
        return TqJsonResponse(200, MetricsJson());
    }
    if (req.Method == "GET" && req.Path == "/config") {
        return TqJsonResponse(200, ConfigJson());
    }
    if (req.Method == "GET" && req.Path == "/relay/metrics") {
        return TqJsonResponse(200, RelayMetricsJson());
    }
    if (req.Method == "GET" && req.Path == "/relay/workers") {
        return TqJsonResponse(200, RelayWorkersJson());
    }
    if (req.Method == "GET" && req.Path.compare(0, 15, "/relay/workers/") == 0) {
        std::string workerId;
        if (!DecodePathSegment(req.Path.substr(15), workerId) || workerId != "aggregate") {
            return TqJsonResponse(404, ErrorJson("not found"));
        }
        return TqJsonResponse(200, RelayWorkerJson(workerId));
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

    TqTunnelAdminPath tunnelPath;
    if (ParseTunnelAdminPath(req.Path, tunnelPath)) {
        if (tunnelPath.Collection) {
            if (req.Method == "GET") {
                return TqJsonResponse(200, TunnelsJson(TqSnapshotTunnels()));
            }
            return TqJsonResponse(404, ErrorJson("not found"));
        }
        if (tunnelPath.Action.empty()) {
            if (req.Method == "GET") {
                TqTunnelSnapshot tunnel;
                if (!TqGetTunnelSnapshot(tunnelPath.TunnelId, tunnel)) {
                    return TqJsonResponse(404, ErrorJson("not found"));
                }
                return TqJsonResponse(200, TunnelJson(tunnel));
            }
            if (req.Method == "DELETE") {
                if (!TqAbortTunnelById(tunnelPath.TunnelId)) {
                    return TqJsonResponse(404, ErrorJson("not found"));
                }
                return TqJsonResponse(202, "{\"status\":\"aborting\"}");
            }
            return TqJsonResponse(404, ErrorJson("not found"));
        }
        if (req.Method != "POST") {
            return TqJsonResponse(404, ErrorJson("not found"));
        }
        if (tunnelPath.Action == "abort") {
            if (!TqAbortTunnelById(tunnelPath.TunnelId)) {
                return TqJsonResponse(404, ErrorJson("not found"));
            }
            return TqJsonResponse(202, "{\"status\":\"aborting\"}");
        }
        if (tunnelPath.Action == "drain") {
            if (!TqDrainTunnelById(tunnelPath.TunnelId)) {
                return TqJsonResponse(404, ErrorJson("not found"));
            }
            return TqJsonResponse(202, "{\"status\":\"draining\"}");
        }
        return TqJsonResponse(404, ErrorJson("not found"));
    }

    TqConnectionAdminPath connectionPath;
    if (ParseConnectionAdminPath(req.Path, connectionPath)) {
        if (connectionPath.Collection) {
            if (req.Method == "GET") {
                TqPeerMetrics peer;
                if (!GetPeer(connectionPath.PeerId, peer)) {
                    return TqJsonResponse(404, ErrorJson("not found"));
                }
                return TqJsonResponse(200, ConnectionsJson(ListConnections(connectionPath.PeerId)));
            }
            if (req.Method == "POST") {
                std::string err;
                if (!AddConnection(connectionPath.PeerId, err)) {
                    return TqJsonResponse(err == "not found" ? 404 : 400, ErrorJson(err));
                }
                return TqJsonResponse(201, ConnectionsJson(ListConnections(connectionPath.PeerId)));
            }
            return TqJsonResponse(404, ErrorJson("not found"));
        }

        if (connectionPath.Action.empty()) {
            if (req.Method == "GET") {
                TqConnectionSnapshot connection;
                if (!GetConnection(connectionPath.PeerId, connectionPath.ConnectionId, connection)) {
                    return TqJsonResponse(404, ErrorJson("not found"));
                }
                return TqJsonResponse(200, ConnectionJson(connection));
            }
            if (req.Method == "DELETE") {
                std::string err;
                if (!DeleteConnection(connectionPath.PeerId, connectionPath.ConnectionId, err)) {
                    if (err == "not found") {
                        return TqJsonResponse(404, ErrorJson(err));
                    }
                    if (err.find("highest") != std::string::npos || err.find("last connection") != std::string::npos) {
                        return TqJsonResponse(409, ErrorJson(err));
                    }
                    return TqJsonResponse(400, ErrorJson(err));
                }
                return TqJsonResponse(200, ConnectionsJson(ListConnections(connectionPath.PeerId)));
            }
            return TqJsonResponse(404, ErrorJson("not found"));
        }

        if (req.Method != "POST") {
            return TqJsonResponse(404, ErrorJson("not found"));
        }
        std::string err;
        if (connectionPath.Action == "reconnect") {
            if (!ReconnectConnection(connectionPath.PeerId, connectionPath.ConnectionId, err)) {
                return TqJsonResponse(err == "not found" ? 404 : 400, ErrorJson(err));
            }
            return TqJsonResponse(202, "{\"status\":\"reconnecting\"}");
        }
        if (connectionPath.Action == "abort-tunnels") {
            if (!AbortConnectionTunnels(connectionPath.PeerId, connectionPath.ConnectionId, err)) {
                return TqJsonResponse(err == "not found" ? 404 : 400, ErrorJson(err));
            }
            return TqJsonResponse(202, "{\"status\":\"aborting\"}");
        }
        return TqJsonResponse(404, ErrorJson("not found"));
    }

    TqPeerAdminPath peerPath;
    if (ParsePeerAdminPath(req.Path, peerPath)) {
        if (peerPath.Collection) {
            if (req.Method == "GET") {
                return TqJsonResponse(200, PeersJson(ListPeers()));
            }
            if (req.Method == "POST") {
                TqPeerConfig peer;
                std::string err;
                if (!ParsePeerConfigBody(req.Body, peer, err)) {
                    return TqJsonResponse(400, ErrorJson(err));
                }
                if (!CreatePeer(peer, err)) {
                    return TqJsonResponse(err.find("already exists") != std::string::npos ? 409 : 400, ErrorJson(err));
                }
                TqPeerMetrics created;
                return TqJsonResponse(GetPeer(peer.PeerId, created) ? 201 : 200,
                    GetPeer(peer.PeerId, created) ? PeerMetricsJson(created) : ConfigJson());
            }
            return TqJsonResponse(404, ErrorJson("not found"));
        }

        if (peerPath.Action.empty()) {
            if (req.Method == "GET") {
                TqPeerMetrics peer;
                if (!GetPeer(peerPath.PeerId, peer)) {
                    return TqJsonResponse(404, ErrorJson("not found"));
                }
                return TqJsonResponse(200, PeerMetricsJson(peer));
            }
            if (req.Method == "PUT") {
                TqPeerConfig peer;
                std::string err;
                if (!ParsePeerConfigBody(req.Body, peer, err)) {
                    return TqJsonResponse(400, ErrorJson(err));
                }
                if (!ReplacePeer(peerPath.PeerId, peer, err)) {
                    return TqJsonResponse(err == "not found" ? 404 : 400, ErrorJson(err));
                }
                TqPeerMetrics replaced;
                return TqJsonResponse(GetPeer(peerPath.PeerId, replaced) ? 200 : 404,
                    GetPeer(peerPath.PeerId, replaced) ? PeerMetricsJson(replaced) : ErrorJson("not found"));
            }
            if (req.Method == "PATCH") {
                std::string err;
                if (!PatchPeer(peerPath.PeerId, req.Body, err)) {
                    return TqJsonResponse(err == "not found" ? 404 : 400, ErrorJson(err));
                }
                TqPeerMetrics patched;
                return TqJsonResponse(GetPeer(peerPath.PeerId, patched) ? 200 : 404,
                    GetPeer(peerPath.PeerId, patched) ? PeerMetricsJson(patched) : ErrorJson("not found"));
            }
            if (req.Method == "DELETE") {
                std::string err;
                TqPeerActionBody body;
                if (!ParsePeerActionBodyText(req.Body, body, err)) {
                    return TqJsonResponse(400, ErrorJson(err));
                }
                if (!DeletePeer(peerPath.PeerId, body.Mode, err)) {
                    if (err == "not found") {
                        return TqJsonResponse(404, ErrorJson(err));
                    }
                    return TqJsonResponse(err.find("requires mode") != std::string::npos ? 409 : 400, ErrorJson(err));
                }
                return TqJsonResponse(200, PeersJson(ListPeers()));
            }
            return TqJsonResponse(404, ErrorJson("not found"));
        }

        if (req.Method != "POST") {
            return TqJsonResponse(404, ErrorJson("not found"));
        }
        std::string err;
        if (peerPath.Action == "enable") {
            if (!EnablePeer(peerPath.PeerId, err)) {
                return TqJsonResponse(err == "not found" ? 404 : 400, ErrorJson(err));
            }
            TqPeerMetrics peer;
            return TqJsonResponse(GetPeer(peerPath.PeerId, peer) ? 200 : 404,
                GetPeer(peerPath.PeerId, peer) ? PeerMetricsJson(peer) : ErrorJson("not found"));
        }
        if (peerPath.Action == "disable") {
            if (!DisablePeer(peerPath.PeerId, err)) {
                return TqJsonResponse(err == "not found" ? 404 : 400, ErrorJson(err));
            }
            TqPeerMetrics peer;
            return TqJsonResponse(GetPeer(peerPath.PeerId, peer) ? 200 : 404,
                GetPeer(peerPath.PeerId, peer) ? PeerMetricsJson(peer) : ErrorJson("not found"));
        }
        if (peerPath.Action == "drain") {
            TqPeerActionBody body;
            if (!ParsePeerActionBodyText(req.Body, body, err)) {
                return TqJsonResponse(400, ErrorJson(err));
            }
            if (!DrainPeer(peerPath.PeerId, body.GraceSeconds, err)) {
                return TqJsonResponse(err == "not found" ? 404 : 400, ErrorJson(err));
            }
            return TqJsonResponse(202, "{\"status\":\"draining\"}");
        }
        if (peerPath.Action == "abort-tunnels") {
            if (!AbortPeerTunnels(peerPath.PeerId, err)) {
                return TqJsonResponse(err == "not found" ? 404 : 400, ErrorJson(err));
            }
            return TqJsonResponse(202, "{\"status\":\"aborting\"}");
        }
        return TqJsonResponse(404, ErrorJson("not found"));
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
