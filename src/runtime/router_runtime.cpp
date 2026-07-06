#include "router_runtime.h"

#include "admin_config.h"
#include "admin_memory.h"
#include "control_protocol.h"
#include "relay_metrics.h"
#include "tunnel_registry.h"
#include "trace.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace {

constexpr size_t kMaxPortForwardTargetHostLength = 255;
constexpr uint32_t kDefaultPeerDrainGraceSeconds = 10;
constexpr uint32_t kMaxQuicConnections = 128;

bool WriteTextFileAtomically(const std::string& path, const std::string& body, std::string& err);

struct TqPeerPatch {
    bool HasQuicPeer{false};
    bool HasSocksListen{false};
    bool HasHttpListen{false};
    bool HasPortForwards{false};
    bool HasQuicPaths{false};
    bool HasQuicConnections{false};
    bool HasCompress{false};
    bool HasEnabled{false};
    std::string QuicPeer;
    std::string SocksListen;
    std::string HttpListen;
    std::vector<TqPortForwardConfig> PortForwards;
    std::vector<TqQuicPathConfig> QuicPaths;
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

nlohmann::json PortForwardsJsonValue(const std::vector<TqPortForwardConfig>& forwards) {
    nlohmann::json values = nlohmann::json::array();
    for (const auto& forward : forwards) {
        values.push_back({
            {"listen", forward.Listen},
            {"target", PortForwardTargetText(forward)},
        });
    }
    return values;
}

nlohmann::json QuicPathsJsonValue(const std::vector<TqQuicPathConfig>& paths) {
    nlohmann::json values = nlohmann::json::array();
    for (const auto& path : paths) {
        values.push_back({
            {"name", path.Name},
            {"local", path.LocalAddress},
            {"peer", path.Peer},
            {"connections", path.Connections},
        });
    }
    return values;
}

nlohmann::json PeerConfigJsonValue(const TqPeerConfig& peer) {
    nlohmann::json body{
        {"peer_id", peer.PeerId},
        {"quic_peer", peer.QuicPeer},
        {"socks_listen", peer.SocksListen},
        {"http_listen", peer.HttpListen},
        {"port_forwards", PortForwardsJsonValue(peer.PortForwards)},
        {"paths", QuicPathsJsonValue(peer.QuicPaths)},
        {"compress", peer.Compress},
        {"enabled", peer.Enabled},
    };
    if (peer.QuicConnections != 0) {
        body["quic_connections"] = peer.QuicConnections;
    }
    return body;
}

nlohmann::json RuntimePeerConfigJsonValue(const TqPeerConfig& peer) {
    nlohmann::json body{
        {"id", peer.PeerId},
        {"proto_peer", peer.QuicPeer},
        {"socks_listen", peer.SocksListen},
        {"http_listen", peer.HttpListen},
        {"port_forwards", PortForwardsJsonValue(peer.PortForwards)},
        {"paths", QuicPathsJsonValue(peer.QuicPaths)},
        {"compress", peer.Compress},
        {"enabled", peer.Enabled},
    };
    if (peer.QuicConnections != 0) {
        body["proto_connections"] = peer.QuicConnections;
    }
    return body;
}

nlohmann::json PeerMetricsJsonValue(const TqPeerMetrics& peer) {
    return {
        {"peer_id", peer.PeerId},
        {"enabled", peer.Enabled},
        {"quic_peer", peer.QuicPeer},
        {"client_name", peer.ClientName},
        {"socks_listen", peer.SocksListen},
        {"http_listen", peer.HttpListen},
        {"port_forwards", PortForwardsJsonValue(peer.PortForwards)},
        {"paths", QuicPathsJsonValue(peer.QuicPaths)},
        {"state", peer.State},
        {"connection_count", peer.ConnectionCount},
        {"connected_connections", peer.ConnectedConnections},
        {"active_streams", peer.ActiveStreams},
        {"total_streams", peer.TotalStreams},
        {"reconnects", peer.Reconnects},
        {"last_error", peer.LastError},
        {"last_connected_at", peer.LastConnectedAt},
    };
}

std::string PeerMetricsJson(const TqPeerMetrics& peer) {
    return PeerMetricsJsonValue(peer).dump();
}

std::string PeersJson(const std::vector<TqPeerMetrics>& peers) {
    nlohmann::json body{{"peers", nlohmann::json::array()}};
    for (const auto& peer : peers) {
        body["peers"].push_back(PeerMetricsJsonValue(peer));
    }
    return body.dump();
}

std::string RouterConfigJson(const TqRouterConfig& config) {
    nlohmann::json body{{"version", config.Version}, {"peers", nlohmann::json::array()}};
    for (const auto& peer : config.Peers) {
        body["peers"].push_back(PeerConfigJsonValue(peer));
    }
    return body.dump();
}

std::string ClientRuntimeConfigJson(const TqConfig& runtimeConfig, const TqRouterConfig& config) {
    nlohmann::json body;
    body["tls"] = nlohmann::json::object();
    if (!runtimeConfig.QuicCa.empty()) {
        body["tls"]["ca"] = runtimeConfig.QuicCa;
    }
    if (!runtimeConfig.QuicCert.empty()) {
        body["tls"]["cert"] = runtimeConfig.QuicCert;
    }
    if (!runtimeConfig.QuicKey.empty()) {
        body["tls"]["key"] = runtimeConfig.QuicKey;
    }
    if (!runtimeConfig.AdminListen.empty() || !runtimeConfig.AdminTokenFile.empty() || runtimeConfig.AdminThreads != 2) {
        body["admin"] = nlohmann::json::object();
        if (!runtimeConfig.AdminListen.empty()) {
            body["admin"]["listen"] = runtimeConfig.AdminListen;
        }
        if (!runtimeConfig.AdminTokenFile.empty()) {
            body["admin"]["token_file"] = runtimeConfig.AdminTokenFile;
        }
        body["admin"]["threads"] = runtimeConfig.AdminThreads;
    }
    body["client"] = {{"client_name", runtimeConfig.ClientName}};
    body["proto"] = {
        {"profile", runtimeConfig.QuicProfile == TqQuicProfile::LowLatency ? "low-latency" : "max-throughput"},
        {"disable_1rtt_encryption", runtimeConfig.QuicDisable1RttEncryption},
        {"connection_stream_count", runtimeConfig.QuicConnectionStreamCount},
        {"keepalive_ms", runtimeConfig.QuicKeepAliveIntervalMs},
        {"connections", runtimeConfig.QuicConnections},
    };
    body["compression"] = {
        {"mode", runtimeConfig.Compress},
        {"level", runtimeConfig.CompressLevel},
    };
    body["peers"] = nlohmann::json::array();
    for (const auto& peer : config.Peers) {
        body["peers"].push_back(RuntimePeerConfigJsonValue(peer));
    }
    return body.dump(2) + "\n";
}

bool ReadRuntimeJsonFile(const std::string& path, nlohmann::json& root) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    try {
        root = nlohmann::json::parse(std::string(
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()));
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    return root.is_object();
}

nlohmann::json RuntimePeersJsonValue(const TqRouterConfig& config) {
    nlohmann::json peers = nlohmann::json::array();
    for (const auto& peer : config.Peers) {
        peers.push_back(RuntimePeerConfigJsonValue(peer));
    }
    return peers;
}

bool WriteRuntimeConfigPeers(
    const std::string& path,
    const TqConfig& runtimeConfig,
    const TqRouterConfig& config,
    std::string& err) {
    nlohmann::json root;
    if (ReadRuntimeJsonFile(path, root)) {
        root["peers"] = RuntimePeersJsonValue(config);
        return WriteTextFileAtomically(path, root.dump(2) + "\n", err);
    }
    return WriteTextFileAtomically(path, ClientRuntimeConfigJson(runtimeConfig, config), err);
}

bool WriteTextFileAtomically(const std::string& path, const std::string& body, std::string& err) {
    std::error_code ec;
    const std::filesystem::path target(path);
    const std::filesystem::path parent = target.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            err = "failed to create client config directory: " + ec.message();
            return false;
        }
    }

    const std::filesystem::path tmp = target.string() + ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) {
            err = "failed to open client config temp file: " + tmp.string();
            return false;
        }
        file << body;
        file.close();
        if (!file) {
            err = "failed to write client config temp file: " + tmp.string();
            return false;
        }
    }

    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        const std::string renameError = ec.message();
        std::error_code removeEc;
        std::filesystem::remove(tmp, removeEc);
        err = "failed to publish client config file: " + renameError;
        return false;
    }
    return true;
}

nlohmann::json ConnectionJsonValue(const TqConnectionSnapshot& connection) {
    return {
        {"connection_id", connection.ConnectionId},
        {"slot_index", connection.SlotIndex},
        {"generation", connection.Generation},
        {"connected", connection.Connected},
        {"retry_scheduled", connection.RetryScheduled},
        {"state", connection.State},
        {"path", connection.PathName},
        {"local", connection.LocalAddress},
        {"peer", connection.PeerAddress},
        {"active_tunnels", connection.ActiveTunnels},
        {"last_error", connection.LastError},
    };
}

std::string ConnectionJson(const TqConnectionSnapshot& connection) {
    return ConnectionJsonValue(connection).dump();
}

std::string ConnectionsJson(const std::vector<TqConnectionSnapshot>& connections) {
    nlohmann::json body{{"connections", nlohmann::json::array()}};
    for (const auto& connection : connections) {
        body["connections"].push_back(ConnectionJsonValue(connection));
    }
    return body.dump();
}

nlohmann::json TunnelJsonValue(const TqTunnelSnapshot& tunnel) {
    return {
        {"tunnel_id", tunnel.TunnelId},
        {"peer_id", tunnel.PeerId},
        {"connection_id", tunnel.ConnectionId},
        {"state", tunnel.State},
        {"target", tunnel.Target},
        {"role", tunnel.Role},
        {"ingress", tunnel.Ingress},
        {"compress", tunnel.Compress},
        {"created_at", tunnel.CreatedAt},
        {"duration_ms", tunnel.DurationMs},
        {"tcp_read_bytes", tunnel.TcpReadBytes},
        {"tcp_write_bytes", tunnel.TcpWriteBytes},
        {"pending_bytes", tunnel.PendingBytes},
        {"relay_backend", tunnel.RelayBackend},
        {"worker_index", tunnel.WorkerIndex},
        {"last_error", tunnel.LastError},
    };
}

std::string TunnelJson(const TqTunnelSnapshot& tunnel) {
    return TunnelJsonValue(tunnel).dump();
}

std::string TunnelsJson(const std::vector<TqTunnelSnapshot>& tunnels) {
    nlohmann::json body{{"tunnels", nlohmann::json::array()}};
    for (const auto& tunnel : tunnels) {
        body["tunnels"].push_back(TunnelJsonValue(tunnel));
    }
    return body.dump();
}

nlohmann::json RelayMetricsJsonValue(const TqRelayMetricsSnapshot& metrics) {
    return nlohmann::json::parse(TqRelayMetricsFieldsJson(metrics));
}

void MergeObject(nlohmann::json& target, const nlohmann::json& source) {
    for (const auto& item : source.items()) {
        target[item.key()] = item.value();
    }
}

std::string RelayMetricsJson() {
    const auto metrics = TqSnapshotRelayMetrics();
    nlohmann::json body{
        {"backend", metrics.Backend},
        {"active_relays", metrics.ActiveRelays},
        {"pending_bytes", metrics.PendingBytes},
        {"tcp_read_bytes", metrics.TcpReadBytes},
        {"tcp_write_bytes", metrics.TcpWriteBytes},
        {"errors", metrics.Errors},
    };
    MergeObject(body, RelayMetricsJsonValue(metrics));
    return body.dump();
}

std::string ErrorJson(const std::string& err) {
    return nlohmann::json{{"error", err}}.dump();
}

std::string StatusJson(const char* status) {
    return nlohmann::json{{"status", status}}.dump();
}

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

class RouterJsonParser {
public:
    RouterJsonParser(const std::string& text, std::string& err) : Text(text), Err(err) {}

    bool Parse(TqRouterConfig& router) {
        router = TqRouterConfig{};
        nlohmann::json root;
        if (!Load(root, "malformed config object")) return false;
        if (!root.is_object()) return Error("config must be a JSON object");
        for (const auto& item : root.items()) {
            const std::string& key = item.key();
            if (key == "version") {
                if (!ReadUint32(item.value(), router.Version)) return Error("invalid version");
            } else if (key == "peers") {
                if (!ParsePeers(item.value(), router.Peers)) return false;
            }
        }
        return TqValidateRouterConfig(router, Err);
    }

    bool ParsePatch(TqPeerPatch& patch) {
        patch = TqPeerPatch{};
        nlohmann::json root;
        if (!Load(root, "malformed peer patch object")) return false;
        if (!root.is_object()) return Error("peer patch must be an object");
        bool hasLegacyPeerId = false;
        bool hasRecommendedPeerId = false;
        bool hasLegacyQuicPeer = false;
        bool hasRecommendedQuicPeer = false;
        bool hasLegacyQuicConnections = false;
        bool hasRecommendedQuicConnections = false;
        for (const auto& item : root.items()) {
            const std::string& key = item.key();
            if (key == "peer_id") {
                if (hasRecommendedPeerId) return Error("conflicting peer field aliases");
                hasLegacyPeerId = true;
                std::string ignored;
                if (!ReadStringField(item.value(), ignored, "invalid peer_id")) return false;
            } else if (key == "id") {
                if (hasLegacyPeerId) return Error("conflicting peer field aliases");
                hasRecommendedPeerId = true;
                std::string ignored;
                if (!ReadStringField(item.value(), ignored, "invalid id")) return false;
            } else if (key == "quic_peer") {
                if (hasRecommendedQuicPeer) return Error("conflicting peer field aliases");
                hasLegacyQuicPeer = true;
                patch.HasQuicPeer = true;
                if (!ReadStringField(item.value(), patch.QuicPeer, "invalid quic_peer")) return false;
            } else if (key == "client_name") {
                return Error("unknown peer key: client_name");
            } else if (key == "proto_peer") {
                if (hasLegacyQuicPeer) return Error("conflicting peer field aliases");
                hasRecommendedQuicPeer = true;
                patch.HasQuicPeer = true;
                if (!ReadStringField(item.value(), patch.QuicPeer, "invalid proto_peer")) return false;
            } else if (key == "socks_listen") {
                patch.HasSocksListen = true;
                if (!ReadStringField(item.value(), patch.SocksListen, "invalid socks_listen")) return false;
            } else if (key == "http_listen") {
                patch.HasHttpListen = true;
                if (!ReadStringField(item.value(), patch.HttpListen, "invalid http_listen")) return false;
            } else if (key == "port_forwards") {
                patch.HasPortForwards = true;
                if (!ParsePortForwards(item.value(), patch.PortForwards)) return false;
            } else if (key == "paths") {
                patch.HasQuicPaths = true;
                if (!ParseQuicPaths(item.value(), patch.QuicPaths)) return false;
            } else if (key == "quic_connections") {
                if (hasRecommendedQuicConnections) return Error("conflicting peer field aliases");
                hasLegacyQuicConnections = true;
                patch.HasQuicConnections = true;
                if (!ReadUint32(item.value(), patch.QuicConnections)) return Error("invalid quic_connections");
                if (patch.QuicConnections == 0) return Error("quic_connections out of range");
            } else if (key == "proto_connections") {
                if (hasLegacyQuicConnections) return Error("conflicting peer field aliases");
                hasRecommendedQuicConnections = true;
                patch.HasQuicConnections = true;
                if (!ReadUint32(item.value(), patch.QuicConnections)) return Error("invalid proto_connections");
                if (patch.QuicConnections == 0) return Error("proto_connections out of range");
            } else if (key == "compress") {
                patch.HasCompress = true;
                if (!ReadStringField(item.value(), patch.Compress, "invalid compress")) return false;
            } else if (key == "enabled") {
                patch.HasEnabled = true;
                if (!ReadBool(item.value(), patch.Enabled)) return Error("invalid enabled");
            }
        }
        return true;
    }

    bool ParsePeerActionBody(TqPeerActionBody& body) {
        body = TqPeerActionBody{};
        nlohmann::json root;
        if (!Load(root, "malformed peer action body")) return false;
        if (!root.is_object()) return Error("peer action body must be an object");
        for (const auto& item : root.items()) {
            const std::string& key = item.key();
            if (key == "mode") {
                if (!ReadStringField(item.value(), body.Mode, "invalid mode")) return false;
            } else if (key == "grace_seconds") {
                if (!ReadUint32(item.value(), body.GraceSeconds)) return Error("invalid grace_seconds");
            }
        }
        return true;
    }

private:
    bool Load(nlohmann::json& out, const char* err) {
        if (ContainsUnicodeSurrogateEscape()) {
            return Error("unicode surrogate escapes are not supported");
        }
        try {
            out = nlohmann::json::parse(Text);
            return true;
        } catch (const nlohmann::json::exception&) {
            return Error(err);
        }
    }

    bool ContainsUnicodeSurrogateEscape() const {
        for (size_t i = 0; i + 5 < Text.size(); ++i) {
            if (Text[i] != '\\' || Text[i + 1] != 'u') {
                continue;
            }
            uint32_t value = 0;
            bool valid = true;
            for (int j = 0; j < 4; ++j) {
                const int hex = HexValue(Text[i + 2 + static_cast<size_t>(j)]);
                if (hex < 0) {
                    valid = false;
                    break;
                }
                value = (value << 4) | static_cast<uint32_t>(hex);
            }
            if (valid && value >= 0xD800 && value <= 0xDFFF) {
                return true;
            }
        }
        return false;
    }

    bool ReadString(const nlohmann::json& value, std::string& out) {
        if (!value.is_string()) return false;
        out = value.get<std::string>();
        return true;
    }

    bool ReadStringField(const nlohmann::json& value, std::string& out, const std::string& err) {
        if (ReadString(value, out)) return true;
        return Error(err);
    }

    bool ReadBool(const nlohmann::json& value, bool& out) {
        if (!value.is_boolean()) return false;
        out = value.get<bool>();
        return true;
    }

    bool ReadUint32(const nlohmann::json& value, uint32_t& out) {
        uint64_t raw = 0;
        if (value.is_number_unsigned()) {
            raw = value.get<uint64_t>();
        } else if (value.is_number_integer()) {
            const int64_t signedValue = value.get<int64_t>();
            if (signedValue < 0) return false;
            raw = static_cast<uint64_t>(signedValue);
        } else {
            return false;
        }
        if (raw > UINT32_MAX) return false;
        out = static_cast<uint32_t>(raw);
        return true;
    }

    bool RequireObject(const nlohmann::json& value, const char* err) {
        return value.is_object() || Error(err);
    }

    bool RequireArray(const nlohmann::json& value, const char* err) {
        return value.is_array() || Error(err);
    }

    bool ParsePeers(const nlohmann::json& array, std::vector<TqPeerConfig>& peers) {
        peers.clear();
        if (!RequireArray(array, "peers must be an array")) return false;
        for (const auto& value : array) {
            TqPeerConfig peer;
            if (!ParsePeer(value, peer)) return false;
            peers.push_back(std::move(peer));
        }
        return true;
    }

    bool ParsePeer(const nlohmann::json& object, TqPeerConfig& peer) {
        if (!RequireObject(object, "peer must be an object")) return false;
        bool quicConnectionsSpecified = false;
        bool hasLegacyPeerId = false;
        bool hasRecommendedPeerId = false;
        bool hasLegacyQuicPeer = false;
        bool hasRecommendedQuicPeer = false;
        bool hasLegacyQuicConnections = false;
        bool hasRecommendedQuicConnections = false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "peer_id") {
                if (hasRecommendedPeerId) return Error("conflicting peer field aliases");
                hasLegacyPeerId = true;
                if (!ReadStringField(item.value(), peer.PeerId, "invalid peer_id")) return false;
            } else if (key == "id") {
                if (hasLegacyPeerId) return Error("conflicting peer field aliases");
                hasRecommendedPeerId = true;
                if (!ReadStringField(item.value(), peer.PeerId, "invalid id")) return false;
            } else if (key == "quic_peer") {
                if (hasRecommendedQuicPeer) return Error("conflicting peer field aliases");
                hasLegacyQuicPeer = true;
                if (!ReadStringField(item.value(), peer.QuicPeer, "invalid quic_peer")) return false;
            } else if (key == "client_name") {
                return Error("unknown peer key: client_name");
            } else if (key == "proto_peer") {
                if (hasLegacyQuicPeer) return Error("conflicting peer field aliases");
                hasRecommendedQuicPeer = true;
                if (!ReadStringField(item.value(), peer.QuicPeer, "invalid proto_peer")) return false;
            } else if (key == "socks_listen") {
                if (!ReadStringField(item.value(), peer.SocksListen, "invalid socks_listen")) return false;
            } else if (key == "http_listen") {
                if (!ReadStringField(item.value(), peer.HttpListen, "invalid http_listen")) return false;
            } else if (key == "port_forwards") {
                if (!ParsePortForwards(item.value(), peer.PortForwards)) return false;
            } else if (key == "paths") {
                if (!ParseQuicPaths(item.value(), peer.QuicPaths)) return false;
            } else if (key == "quic_connections") {
                if (hasRecommendedQuicConnections) return Error("conflicting peer field aliases");
                hasLegacyQuicConnections = true;
                quicConnectionsSpecified = true;
                if (!ReadUint32(item.value(), peer.QuicConnections)) return Error("invalid quic_connections");
            } else if (key == "proto_connections") {
                if (hasLegacyQuicConnections) return Error("conflicting peer field aliases");
                hasRecommendedQuicConnections = true;
                quicConnectionsSpecified = true;
                if (!ReadUint32(item.value(), peer.QuicConnections)) return Error("invalid proto_connections");
            } else if (key == "quic_reconnect_interval_ms") {
                return Error("unknown peer key: quic_reconnect_interval_ms");
            } else if (key == "compress") {
                if (!ReadStringField(item.value(), peer.Compress, "invalid compress")) return false;
            } else if (key == "enabled") {
                if (!ReadBool(item.value(), peer.Enabled)) return Error("invalid enabled");
            }
        }
        if (quicConnectionsSpecified && peer.QuicConnections == 0) return Error("quic_connections out of range");
        return true;
    }

    bool ParseQuicPaths(const nlohmann::json& array, std::vector<TqQuicPathConfig>& paths) {
        paths.clear();
        if (!RequireArray(array, "paths must be an array")) return false;
        for (const auto& value : array) {
            TqQuicPathConfig path;
            if (!ParseQuicPathObject(value, path)) return false;
            paths.push_back(std::move(path));
        }
        return true;
    }

    bool ParseQuicPathObject(const nlohmann::json& object, TqQuicPathConfig& path) {
        if (!RequireObject(object, "path must be an object")) return false;
        if (object.empty()) return Error("path fields are required");
        bool hasName = false, hasLocal = false, hasPeer = false, hasConnections = false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "name") {
                hasName = true;
                if (!ReadString(item.value(), path.Name)) return Error("invalid path.name");
            } else if (key == "local") {
                hasLocal = true;
                if (!ReadString(item.value(), path.LocalAddress)) return Error("invalid path.local");
            } else if (key == "peer") {
                hasPeer = true;
                if (!ReadString(item.value(), path.Peer)) return Error("invalid path.peer");
            } else if (key == "connections") {
                hasConnections = true;
                if (!ReadUint32(item.value(), path.Connections)) return Error("invalid path.connections");
            } else {
                return Error("unknown path key: " + key);
            }
        }
        if (!hasName || !hasLocal || !hasPeer || !hasConnections) return Error("path name, local, peer and connections are required");
        return true;
    }

    bool ParsePortForwardPort(const std::string& value, size_t portStart, uint16_t& port) {
        if (portStart >= value.size()) return false;
        uint32_t parsedPort = 0;
        for (size_t i = portStart; i < value.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(value[i]))) return false;
            const uint32_t digit = static_cast<uint32_t>(value[i] - '0');
            if (parsedPort > 6553 || (parsedPort == 6553 && digit > 5)) return false;
            parsedPort = parsedPort * 10 + digit;
        }
        if (parsedPort == 0) return false;
        port = static_cast<uint16_t>(parsedPort);
        return true;
    }

    bool ParsePortForwardTargetText(const std::string& target, std::string& host, uint16_t& port) {
        host.clear();
        port = 0;
        size_t portStart = std::string::npos;
        if (!target.empty() && target[0] == '[') {
            const size_t close = target.find(']');
            if (close == std::string::npos || close == 1 || close + 2 > target.size() || target[close + 1] != ':') return false;
            host = target.substr(1, close - 1);
            portStart = close + 2;
        } else {
            const size_t colon = target.find(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= target.size() || target.find(':', colon + 1) != std::string::npos) return false;
            host = target.substr(0, colon);
            portStart = colon + 1;
        }
        if (host.empty() || host.size() > kMaxPortForwardTargetHostLength || !ParsePortForwardPort(target, portStart, port)) {
            host.clear();
            port = 0;
            return false;
        }
        return true;
    }

    bool ParsePortForwards(const nlohmann::json& array, std::vector<TqPortForwardConfig>& forwards) {
        forwards.clear();
        if (!RequireArray(array, "port_forwards must be an array")) return false;
        for (const auto& value : array) {
            TqPortForwardConfig forward;
            if (!ParsePortForwardObject(value, forward)) return false;
            forwards.push_back(std::move(forward));
        }
        return true;
    }

    bool ParsePortForwardObject(const nlohmann::json& object, TqPortForwardConfig& forward) {
        if (!RequireObject(object, "port_forward must be an object")) return false;
        if (object.empty()) return Error("port_forward listen and target are required");
        bool hasListen = false, hasTarget = false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "listen") {
                hasListen = true;
                if (!ReadString(item.value(), forward.Listen) || !IsHostPort(forward.Listen)) return Error("invalid port_forward.listen");
            } else if (key == "target") {
                hasTarget = true;
                std::string target;
                if (!ReadString(item.value(), target) || !ParsePortForwardTargetText(target, forward.TargetHost, forward.TargetPort)) return Error("invalid port_forward.target");
            } else {
                return Error("unknown port_forward key: " + key);
            }
        }
        if (!hasListen || !hasTarget) return Error("port_forward listen and target are required");
        return true;
    }

    bool Error(const std::string& err) {
        Err = err;
        return false;
    }

    const std::string& Text;
    std::string& Err;
};

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
    constexpr const char* ConfigSuffix = "/config";
    constexpr size_t configSuffixLen = std::char_traits<char>::length(ConfigSuffix);
    if (tail.size() > configSuffixLen &&
        tail.compare(tail.size() - configSuffixLen, configSuffixLen, ConfigSuffix) == 0) {
        out.Action = "config";
        tail.resize(tail.size() - configSuffixLen);
        return DecodePathSegment(tail, out.PeerId);
    }
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
    if (patch.HasQuicPaths) peer.QuicPaths = patch.QuicPaths;
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

bool SameQuicPaths(const std::vector<TqQuicPathConfig>& a, const std::vector<TqQuicPathConfig>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].Name != b[i].Name ||
            a[i].LocalAddress != b[i].LocalAddress ||
            a[i].Peer != b[i].Peer ||
            a[i].Connections != b[i].Connections) {
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
        SameQuicPaths(a.QuicPaths, b.QuicPaths) &&
        a.QuicConnections == b.QuicConnections &&
        a.Compress == b.Compress;
}

bool PeerDataPlaneChanged(const TqPeerConfig& a, const TqPeerConfig& b) {
    return a.QuicPeer != b.QuicPeer ||
        a.SocksListen != b.SocksListen ||
        a.HttpListen != b.HttpListen ||
        !SamePortForwards(a.PortForwards, b.PortForwards) ||
        !SameQuicPaths(a.QuicPaths, b.QuicPaths) ||
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

TqConfig DefaultClientRuntimeConfig() {
    TqConfig cfg;
    cfg.Mode = TqMode::Client;
    return cfg;
}

} // namespace

TqRouterRuntime::TqRouterRuntime(TqPeerRuntimeAdapter* adapter)
    : RuntimeConfig(DefaultClientRuntimeConfig()), Adapter(adapter) {}

TqRouterRuntime::TqRouterRuntime(TqPeerRuntimeAdapter* adapter, TqConfig runtimeConfig)
    : RuntimeConfig(std::move(runtimeConfig)), Config(RuntimeConfig.Router), Adapter(adapter) {}

TqRouterRuntime::TqRouterRuntime(bool bridgeValidationMode)
    : RuntimeConfig(DefaultClientRuntimeConfig()), BridgeValidationMode(bridgeValidationMode) {}

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
        metrics.ClientName = RuntimeConfig.ClientName;
        metrics.Enabled = peer.Enabled;
        metrics.QuicPeer = peer.QuicPeer;
        metrics.SocksListen = peer.SocksListen;
        metrics.HttpListen = peer.HttpListen;
        metrics.PortForwards = peer.PortForwards;
        metrics.QuicPaths = peer.QuicPaths;
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
    RuntimeConfig.Router = config;
    if (!PersistConfigLocked(config, err)) {
        return false;
    }
    return true;
}

bool TqRouterRuntime::PersistConfigLocked(const TqRouterConfig& config, std::string& err) const {
    if (RuntimeConfig.Mode != TqMode::Client) {
        return true;
    }
    if (!RuntimeConfig.ConfigPath.empty() &&
        !WriteRuntimeConfigPeers(RuntimeConfig.ConfigPath, RuntimeConfig, config, err)) {
        return false;
    }
    if (!RuntimeConfig.ClientConfigPath.empty() &&
        !WriteTextFileAtomically(RuntimeConfig.ClientConfigPath, RouterConfigJson(config), err)) {
        return false;
    }
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
                if (!live.ClientName.empty()) {
                    peer.ClientName = live.ClientName;
                }
                peer.LastError = live.LastError;
                peer.LastConnectedAt = live.LastConnectedAt;
                peer.State = live.State;
                peer.PortForwards = item.second.PortForwards;
                peer.QuicPaths = item.second.QuicPaths;
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
    RuntimeConfig.Router = Config;
    auto running = RunningPeers.find(peerId);
    if (running != RunningPeers.end()) {
        running->second.QuicConnections = desired;
    }
    auto metrics = Metrics.find(peerId);
    if (metrics != Metrics.end()) {
        metrics->second.ConnectionCount = desired;
    }
    if (!PersistConfigLocked(Config, err)) {
        return false;
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
    RuntimeConfig.Router = Config;
    auto running = RunningPeers.find(peerId);
    if (running != RunningPeers.end()) {
        running->second.QuicConnections = desired;
    }
    auto metrics = Metrics.find(peerId);
    if (metrics != Metrics.end()) {
        metrics->second.ConnectionCount = desired;
    }
    if (!PersistConfigLocked(Config, err)) {
        return false;
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
    return RouterConfigJson(config);
}

std::string TqRouterRuntime::MetricsJson() const {
    const TqRouterMetrics metrics = SnapshotMetrics();
    nlohmann::json body{
        {"role", metrics.Role},
        {"status", metrics.Status},
        {"uptime_seconds", metrics.UptimeSeconds},
        {"peers", nlohmann::json::array()},
    };
    for (const auto& peer : metrics.Peers) {
        body["peers"].push_back(PeerMetricsJsonValue(peer));
    }
    return body.dump();
}

std::string TqRouterRuntime::HealthJson() const {
    const TqRouterMetrics metrics = SnapshotMetrics();
    return nlohmann::json{
        {"role", metrics.Role},
        {"status", metrics.Status},
        {"uptime_seconds", metrics.UptimeSeconds},
    }.dump();
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
    if (req.Method == "GET" && req.Path == "/runtime/config") {
        TqConfig runtimeConfig;
        {
            std::lock_guard<std::mutex> lock(Mutex);
            runtimeConfig = RuntimeConfig;
        }
        return TqJsonResponse(200, TqRuntimeConfigJson(runtimeConfig, false));
    }
    if (req.Method == "PATCH" && req.Path == "/runtime/config") {
        TqConfig runtimeConfig;
        std::string err;
        bool unsupported = false;
        {
            std::lock_guard<std::mutex> lock(Mutex);
            runtimeConfig = RuntimeConfig;
            if (!TqApplyRuntimeConfigPatch(req.Body, runtimeConfig, err, unsupported)) {
                return TqJsonResponse(
                    unsupported ? 503 : 400,
                    unsupported ? TqStructuredErrorJson("not_supported", err) : ErrorJson(err));
            }
            RuntimeConfig = runtimeConfig;
        }
        return TqJsonResponse(200, TqRuntimeConfigJson(runtimeConfig, false));
    }
    if (req.Method == "GET" && req.Path == "/client/config") {
        TqConfig runtimeConfig;
        {
            std::lock_guard<std::mutex> lock(Mutex);
            runtimeConfig = RuntimeConfig;
        }
        return TqJsonResponse(200, TqClientPublicConfigJson(runtimeConfig));
    }
    if (req.Method == "GET" && req.Path == "/diagnostics") {
        TqConfig runtimeConfig;
        {
            std::lock_guard<std::mutex> lock(Mutex);
            runtimeConfig = RuntimeConfig;
        }
        return TqJsonResponse(200, TqDiagnosticsJson(runtimeConfig));
    }
    if (req.Method == "PATCH" && req.Path == "/diagnostics") {
        TqConfig runtimeConfig;
        std::string err;
        {
            std::lock_guard<std::mutex> lock(Mutex);
            runtimeConfig = RuntimeConfig;
            if (!TqApplyDiagnosticsPatch(req.Body, runtimeConfig, err)) {
                return TqJsonResponse(400, ErrorJson(err));
            }
            RuntimeConfig = runtimeConfig;
        }
        if (!TqApplyDiagnosticsRuntime(runtimeConfig)) {
            return TqJsonResponse(
                503,
                TqStructuredErrorJson("not_supported", "failed to apply diagnostics runtime state"));
        }
        return TqJsonResponse(200, TqDiagnosticsJson(runtimeConfig));
    }
    if (req.Method == "GET" && req.Path == "/config") {
        return TqJsonResponse(200, ConfigJson());
    }
    if (req.Method == "GET" && req.Path == "/relay/metrics") {
        return TqJsonResponse(200, RelayMetricsJson());
    }
    if (req.Method == "GET" && req.Path == "/relay/active-relays") {
        return TqJsonResponse(200, TqRelayActiveRelaysJson());
    }
    if (req.Method == "GET" && req.Path.compare(0, 21, "/relay/active-relays/") == 0) {
        const std::string encodedRelayId = req.Path.substr(21);
        std::string relayId;
        if (encodedRelayId.empty() || encodedRelayId.find('/') != std::string::npos ||
            !DecodePathSegment(encodedRelayId, relayId) || relayId.empty()) {
            return TqJsonResponse(404, TqStructuredErrorJson("not_found", "not found"));
        }
        bool found = false;
        bool supported = false;
        const std::string body = TqRelayActiveRelayJson(relayId, found, supported);
        if (!supported) {
            return TqJsonResponse(503, TqStructuredErrorJson(
                "not_supported", "relay active relay detail is not supported by this backend"));
        }
        if (!found) {
            return TqJsonResponse(404, TqStructuredErrorJson("not_found", "not found"));
        }
        return TqJsonResponse(200, body);
    }
    if (req.Method == "GET" && req.Path == "/relay/workers") {
        return TqJsonResponse(200, TqRelayWorkersJson());
    }
    if (req.Method == "GET" && req.Path.compare(0, 15, "/relay/workers/") == 0) {
        const std::string encodedWorkerId = req.Path.substr(15);
        std::string workerId;
        if (encodedWorkerId.empty() || encodedWorkerId.find('/') != std::string::npos ||
            !DecodePathSegment(encodedWorkerId, workerId) || workerId.empty()) {
            return TqJsonResponse(404, TqStructuredErrorJson("not_found", "not found"));
        }
        bool found = false;
        bool supported = false;
        const std::string body = TqRelayWorkerDetailJson(workerId, found, supported);
        if (!supported) {
            return TqJsonResponse(503, TqStructuredErrorJson(
                "not_supported", "relay worker detail is not supported by this backend"));
        }
        if (!found) {
            return TqJsonResponse(404, TqStructuredErrorJson("not_found", "not found"));
        }
        return TqJsonResponse(200, body);
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
                return TqJsonResponse(202, StatusJson("aborting"));
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
            return TqJsonResponse(202, StatusJson("aborting"));
        }
        if (tunnelPath.Action == "drain") {
            if (!TqDrainTunnelById(tunnelPath.TunnelId)) {
                return TqJsonResponse(404, ErrorJson("not found"));
            }
            return TqJsonResponse(202, StatusJson("draining"));
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
            return TqJsonResponse(202, StatusJson("reconnecting"));
        }
        if (connectionPath.Action == "abort-tunnels") {
            if (!AbortConnectionTunnels(connectionPath.PeerId, connectionPath.ConnectionId, err)) {
                return TqJsonResponse(err == "not found" ? 404 : 400, ErrorJson(err));
            }
            return TqJsonResponse(202, StatusJson("aborting"));
        }
        return TqJsonResponse(404, ErrorJson("not found"));
    }

    TqPeerAdminPath peerPath;
    if (ParsePeerAdminPath(req.Path, peerPath)) {
        if (!peerPath.Collection && req.Method == "GET" && peerPath.Action == "config") {
            TqConfig runtimeConfig;
            TqPeerConfig peerConfig;
            {
                std::lock_guard<std::mutex> lock(Mutex);
                auto peer = FindPeerConfig(RuntimeConfig.Router.Peers, peerPath.PeerId);
                if (peer == RuntimeConfig.Router.Peers.end()) {
                    return TqJsonResponse(404, ErrorJson("not found"));
                }
                runtimeConfig = RuntimeConfig;
                peerConfig = *peer;
            }
            return TqJsonResponse(200, TqPeerPublicConfigJson(runtimeConfig, peerConfig));
        }
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
            return TqJsonResponse(202, StatusJson("draining"));
        }
        if (peerPath.Action == "abort-tunnels") {
            if (!AbortPeerTunnels(peerPath.PeerId, err)) {
                return TqJsonResponse(err == "not found" ? 404 : 400, ErrorJson(err));
            }
            return TqJsonResponse(202, StatusJson("aborting"));
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
