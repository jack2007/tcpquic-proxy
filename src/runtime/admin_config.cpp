#include "admin_config.h"

#include <sstream>

namespace {

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

void AppendString(std::ostringstream& out, const char* name, const std::string& value) {
    out << '"' << name << "\":\"" << JsonEscape(value) << '"';
}

void AppendBool(std::ostringstream& out, const char* name, bool value) {
    out << '"' << name << "\":" << (value ? "true" : "false");
}

void AppendStringArray(std::ostringstream& out, const char* name, const std::vector<std::string>& values) {
    out << '"' << name << "\":[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out << ',';
        out << '"' << JsonEscape(values[i]) << '"';
    }
    out << ']';
}

void AppendPortForwards(std::ostringstream& out, const std::vector<TqPortForwardConfig>& forwards) {
    out << "\"port_forwards\":[";
    for (size_t i = 0; i < forwards.size(); ++i) {
        if (i != 0) out << ',';
        out << '{';
        AppendString(out, "listen", forwards[i].Listen);
        out << ',';
        AppendString(out, "target_host", forwards[i].TargetHost);
        out << ",\"target_port\":" << forwards[i].TargetPort;
        out << '}';
    }
    out << ']';
}

void AppendQuicPaths(std::ostringstream& out, const std::vector<TqQuicPathConfig>& paths) {
    out << "\"paths\":[";
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i != 0) out << ',';
        out << '{';
        AppendString(out, "name", paths[i].Name);
        out << ',';
        AppendString(out, "local", paths[i].LocalAddress);
        out << ',';
        AppendString(out, "proto_peer", paths[i].Peer);
        out << ",\"proto_connections\":" << paths[i].Connections;
        out << '}';
    }
    out << ']';
}

const char* RoleName(TqMode mode) {
    switch (mode) {
    case TqMode::Client: return "client";
    case TqMode::Server: return "server";
    }
    return "client";
}

const char* QuicProfileName(TqQuicProfile profile) {
    switch (profile) {
    case TqQuicProfile::MaxThroughput: return "max-throughput";
    case TqQuicProfile::LowLatency: return "low-latency";
    }
    return "max-throughput";
}

const char* TuningModeName(TqTuningMode mode) {
    switch (mode) {
    case TqTuningMode::Auto: return "auto";
    case TqTuningMode::Lan: return "lan";
    case TqTuningMode::Wan: return "wan";
    }
    return "wan";
}

void AppendProxyAuth(std::ostringstream& out, const std::vector<TqProxyAuthUser>& users) {
    out << "\"proxy_auth\":[";
    for (size_t i = 0; i < users.size(); ++i) {
        if (i != 0) out << ',';
        out << '{';
        AppendString(out, "username", users[i].Username);
        out << ',';
        AppendString(out, "password", "***");
        out << '}';
    }
    out << ']';
}

void AppendPeerPublicConfig(std::ostringstream& out, const TqPeerConfig& peer) {
    out << '{';
    AppendString(out, "id", peer.PeerId);
    out << ',';
    AppendString(out, "proto_peer", peer.QuicPeer);
    out << ',';
    AppendString(out, "socks_listen", peer.SocksListen);
    out << ',';
    AppendString(out, "http_listen", peer.HttpListen);
    out << ',';
    AppendPortForwards(out, peer.PortForwards);
    out << ',';
    AppendQuicPaths(out, peer.QuicPaths);
    out << ",\"proto_connections\":" << peer.QuicConnections;
    out << ',';
    AppendString(out, "compress", peer.Compress);
    out << ',';
    AppendBool(out, "enabled", peer.Enabled);
    out << '}';
}

void AppendPeers(std::ostringstream& out, const std::vector<TqPeerConfig>& peers) {
    out << "\"peers\":[";
    for (size_t i = 0; i < peers.size(); ++i) {
        if (i != 0) out << ',';
        AppendPeerPublicConfig(out, peers[i]);
    }
    out << ']';
}

void AppendTls(std::ostringstream& out, const TqConfig& cfg, bool redact) {
    out << "\"tls\":{";
    AppendString(out, "cert", cfg.QuicCert);
    out << ',';
    AppendString(out, "key", redact && !cfg.QuicKey.empty() ? "***" : cfg.QuicKey);
    out << ',';
    AppendString(out, "ca", cfg.QuicCa);
    out << '}';
}

void AppendQuic(std::ostringstream& out, const TqConfig& cfg) {
    out << "\"quic\":{";
    AppendString(out, "listen", cfg.QuicListen);
    out << ',';
    AppendString(out, "proto_peer", cfg.QuicPeer);
    out << ',';
    AppendQuicPaths(out, cfg.QuicPaths);
    out << ",\"proto_connections\":" << cfg.QuicConnections;
    out << ",\"connection_stream_count\":" << cfg.QuicConnectionStreamCount;
    out << ",\"keep_alive_interval_ms\":" << cfg.QuicKeepAliveIntervalMs;
    out << ',';
    AppendString(out, "profile", QuicProfileName(cfg.QuicProfile));
    out << ',';
    AppendBool(out, "disable_1rtt_encryption", cfg.QuicDisable1RttEncryption);
    out << '}';
}

} // namespace

std::string TqStructuredErrorJson(const std::string& code, const std::string& message) {
    std::ostringstream out;
    out << "{\"error\":{";
    AppendString(out, "code", code);
    out << ',';
    AppendString(out, "message", message);
    out << "}}";
    return out.str();
}

std::string TqPeerPublicConfigJson(const TqConfig& cfg, const TqPeerConfig& peer) {
    (void)cfg;
    std::ostringstream out;
    AppendPeerPublicConfig(out, peer);
    return out.str();
}

std::string TqClientPublicConfigJson(const TqConfig& cfg) {
    std::ostringstream out;
    out << '{';
    AppendString(out, "role", "client");
    out << ',';
    AppendPeers(out, cfg.Router.Peers);
    out << ',';
    AppendProxyAuth(out, cfg.Router.ProxyAuth);
    out << '}';
    return out.str();
}

std::string TqDiagnosticsJson(const TqConfig& cfg) {
    std::ostringstream out;
    out << '{';
    AppendString(out, "role", RoleName(cfg.Mode));
    out << ',';
    AppendString(out, "tuning_mode", TuningModeName(cfg.TuningMode));
    out << ",\"max_memory_mb\":" << cfg.MaxMemoryMb;
    out << ",\"trace\":" << (cfg.Trace ? "true" : "false");
    out << ",\"trace_interval_sec\":" << cfg.TraceIntervalSec;
    out << ",\"diag_stats\":" << (cfg.DiagStats ? "true" : "false");
    out << ",\"diag_stats_interval_sec\":" << cfg.DiagStatsIntervalSec;
    out << '}';
    return out.str();
}

std::string TqRuntimeConfigJson(const TqConfig& cfg, bool redact) {
    std::ostringstream out;
    out << '{';
    AppendString(out, "role", RoleName(cfg.Mode));
    out << ',';
    AppendString(out, "config_path", cfg.ConfigPath);
    out << ',';
    AppendString(out, "client_config_path", cfg.ClientConfigPath);
    out << ",\"admin\":{";
    AppendString(out, "listen", cfg.AdminListen);
    out << ',';
    AppendString(out, "token_file", cfg.AdminTokenFile);
    out << ",\"threads\":" << cfg.AdminThreads << '}';
    out << ',';
    AppendString(out, "socks_listen", cfg.SocksListen);
    out << ',';
    AppendString(out, "http_listen", cfg.HttpListen);
    out << ',';
    AppendQuic(out, cfg);
    out << ',';
    AppendTls(out, cfg, redact);
    out << ',';
    AppendPortForwards(out, cfg.PortForwards);
    out << ",\"router\":{";
    out << "\"version\":" << cfg.Router.Version << ',';
    AppendProxyAuth(out, cfg.Router.ProxyAuth);
    out << ',';
    AppendPeers(out, cfg.Router.Peers);
    out << '}';
    out << ',';
    AppendString(out, "compress", cfg.Compress);
    out << ",\"compress_level\":" << cfg.CompressLevel;
    out << ',';
    AppendString(out, "tuning_mode", TuningModeName(cfg.TuningMode));
    out << '}';
    return out.str();
}

std::string TqServerRuntimeConfigJson(
    const TqConfig& cfg,
    const std::vector<std::string>& resolvedListens,
    bool redact) {
    std::ostringstream out;
    out << '{';
    AppendString(out, "role", "server");
    out << ',';
    AppendString(out, "config_path", cfg.ConfigPath);
    out << ',';
    AppendString(out, "listen", cfg.QuicListen);
    out << ',';
    AppendStringArray(out, "resolved_listens", resolvedListens);
    out << ',';
    AppendStringArray(out, "allow_targets", cfg.AllowTargets);
    out << ',';
    AppendStringArray(out, "deny_targets", cfg.DenyTargets);
    out << ',';
    AppendQuic(out, cfg);
    out << ',';
    AppendTls(out, cfg, redact);
    out << ',';
    AppendString(out, "tuning_mode", TuningModeName(cfg.TuningMode));
    out << '}';
    return out.str();
}
