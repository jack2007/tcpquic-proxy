#include "admin_config.h"

#include <cctype>
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

void AppendRecommendedQuicPaths(std::ostringstream& out, const std::vector<TqQuicPathConfig>& paths) {
    out << "\"paths\":[";
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i != 0) out << ',';
        out << '{';
        AppendString(out, "name", paths[i].Name);
        out << ',';
        AppendString(out, "local", paths[i].LocalAddress);
        out << ',';
        AppendString(out, "peer", paths[i].Peer);
        out << ",\"connections\":" << paths[i].Connections;
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

const char* SpeedTestModeName(TqSpeedTestMode mode) {
    switch (mode) {
    case TqSpeedTestMode::None: return "none";
    case TqSpeedTestMode::Download: return "download";
    case TqSpeedTestMode::DownloadSink: return "download-sink";
    case TqSpeedTestMode::Upload: return "upload";
    }
    return "none";
}

const char* TraceLevelName(TqConfig::TraceLevel level) {
    switch (level) {
    case TqConfig::TraceLevel::Info: return "info";
    case TqConfig::TraceLevel::Debug: return "debug";
    }
    return "info";
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

void AppendClientProto(std::ostringstream& out, const TqConfig& cfg) {
    out << "\"proto\":{";
    AppendString(out, "peer", cfg.QuicPeer);
    out << ',';
    AppendRecommendedQuicPaths(out, cfg.QuicPaths);
    out << ",\"connections\":" << cfg.QuicConnections;
    out << ",\"connection_stream_count\":" << cfg.QuicConnectionStreamCount;
    out << ",\"keep_alive_interval_ms\":" << cfg.QuicKeepAliveIntervalMs;
    out << ',';
    AppendString(out, "profile", QuicProfileName(cfg.QuicProfile));
    out << ',';
    AppendBool(out, "disable_1rtt_encryption", cfg.QuicDisable1RttEncryption);
    out << '}';
}

class DiagnosticsPatchParser {
public:
    DiagnosticsPatchParser(const std::string& text, TqConfig& cfg, std::string& err) :
        Text(text),
        Config(cfg),
        Err(err) {}

    bool Parse() {
        if (!Consume('{')) return Error("diagnostics patch must be an object");
        if (Consume('}')) return Finish();
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed diagnostics patch object");
            if (key == "trace") {
                if (!ParseBool(Config.Trace)) return Error("invalid trace");
            } else if (key == "trace_interval_sec") {
                if (!ParseUint32(Config.TraceIntervalSec)) return Error("invalid trace_interval_sec");
                if (Config.TraceIntervalSec < 1 || Config.TraceIntervalSec > 86400) {
                    return Error("trace_interval_sec out of range");
                }
            } else if (key == "trace_level") {
                std::string level;
                if (!ParseString(level)) return Error("invalid trace_level");
                if (level == "info") {
                    Config.TraceLogLevel = TqConfig::TraceLevel::Info;
                } else if (level == "debug") {
                    Config.TraceLogLevel = TqConfig::TraceLevel::Debug;
                } else {
                    return Error("trace_level out of range");
                }
            } else if (key == "diag_stats") {
                if (!ParseBool(Config.DiagStats)) return Error("invalid diag_stats");
            } else if (key == "diag_stats_interval_sec") {
                if (!ParseUint32(Config.DiagStatsIntervalSec)) return Error("invalid diag_stats_interval_sec");
                if (Config.DiagStatsIntervalSec < 1 || Config.DiagStatsIntervalSec > 86400) {
                    return Error("diag_stats_interval_sec out of range");
                }
            } else {
                return Error("unknown diagnostics field: " + key);
            }
        } while (Consume(','));
        if (!Consume('}')) return Error("malformed diagnostics patch object");
        return Finish();
    }

private:
    const std::string& Text;
    TqConfig& Config;
    std::string& Err;
    size_t Pos{0};

    bool Error(const std::string& err) {
        Err = err;
        return false;
    }

    void SkipWs() {
        while (Pos < Text.size() && std::isspace(static_cast<unsigned char>(Text[Pos]))) {
            ++Pos;
        }
    }

    bool Consume(char ch) {
        SkipWs();
        if (Pos >= Text.size() || Text[Pos] != ch) {
            return false;
        }
        ++Pos;
        return true;
    }

    bool Finish() {
        SkipWs();
        if (Pos != Text.size()) return Error("unexpected trailing content in diagnostics patch");
        Err.clear();
        return true;
    }

    bool ParseString(std::string& out) {
        SkipWs();
        if (Pos >= Text.size() || Text[Pos] != '"') return false;
        ++Pos;
        out.clear();
        while (Pos < Text.size()) {
            const char ch = Text[Pos++];
            if (ch == '"') {
                return true;
            }
            if (ch == '\\') {
                if (Pos >= Text.size()) return false;
                const char escaped = Text[Pos++];
                switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: return false;
                }
            } else {
                if (static_cast<unsigned char>(ch) < 0x20) return false;
                out.push_back(ch);
            }
        }
        return false;
    }

    bool ParseBool(bool& out) {
        SkipWs();
        if (Text.compare(Pos, 4, "true") == 0) {
            Pos += 4;
            out = true;
            return true;
        }
        if (Text.compare(Pos, 5, "false") == 0) {
            Pos += 5;
            out = false;
            return true;
        }
        return false;
    }

    bool ParseUint32(uint32_t& out) {
        SkipWs();
        if (Pos >= Text.size() || !std::isdigit(static_cast<unsigned char>(Text[Pos]))) {
            return false;
        }
        if (Text[Pos] == '0' && Pos + 1 < Text.size() &&
            std::isdigit(static_cast<unsigned char>(Text[Pos + 1]))) {
            return false;
        }
        uint64_t value = 0;
        while (Pos < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Pos]))) {
            value = value * 10 + static_cast<uint64_t>(Text[Pos] - '0');
            if (value > 0xffffffffull) {
                return false;
            }
            ++Pos;
        }
        out = static_cast<uint32_t>(value);
        return true;
    }
};

class RuntimeConfigPatchParser {
public:
    RuntimeConfigPatchParser(const std::string& text, TqConfig& cfg, std::string& err, bool& unsupported) :
        Text(text),
        Config(cfg),
        Err(err),
        Unsupported(unsupported) {
        Unsupported = false;
    }

    bool Parse() {
        if (!Consume('{')) return Error("runtime config patch must be an object");
        if (Consume('}')) return Finish();
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed runtime config patch object");
            if (key == "compression") {
                if (!ParseCompressionObject()) return false;
            } else if (key == "tuning") {
                if (!ParseTuningObject()) return false;
            } else if (key == "admin" || key == "tls" || key == "proto" || key == "relay" || key == "listen") {
                return UnsupportedField(key == "admin" ? "admin.listen" : key);
            } else {
                return Error("unknown runtime config field: " + key);
            }
        } while (Consume(','));
        if (!Consume('}')) return Error("malformed runtime config patch object");
        return Finish();
    }

private:
    const std::string& Text;
    TqConfig& Config;
    std::string& Err;
    bool& Unsupported;
    size_t Pos{0};

    bool Error(const std::string& err) {
        Err = err;
        Unsupported = false;
        return false;
    }

    bool UnsupportedField(const std::string& field) {
        Err = "runtime field " + field + " requires process restart";
        Unsupported = true;
        return false;
    }

    void SkipWs() {
        while (Pos < Text.size() && std::isspace(static_cast<unsigned char>(Text[Pos]))) {
            ++Pos;
        }
    }

    bool Consume(char ch) {
        SkipWs();
        if (Pos >= Text.size() || Text[Pos] != ch) {
            return false;
        }
        ++Pos;
        return true;
    }

    bool Finish() {
        SkipWs();
        if (Pos != Text.size()) return Error("unexpected trailing content in runtime config patch");
        Err.clear();
        Unsupported = false;
        return true;
    }

    bool ParseString(std::string& out) {
        SkipWs();
        if (Pos >= Text.size() || Text[Pos] != '"') return false;
        ++Pos;
        out.clear();
        while (Pos < Text.size()) {
            const char ch = Text[Pos++];
            if (ch == '"') return true;
            if (ch == '\\') {
                if (Pos >= Text.size()) return false;
                const char escaped = Text[Pos++];
                switch (escaped) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
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
        if (Text[Pos] == '0' && Pos + 1 < Text.size() &&
            std::isdigit(static_cast<unsigned char>(Text[Pos + 1]))) {
            return false;
        }
        uint64_t value = 0;
        while (Pos < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Pos]))) {
            value = value * 10 + static_cast<uint64_t>(Text[Pos] - '0');
            if (value > 0xffffffffull) return false;
            ++Pos;
        }
        out = static_cast<uint32_t>(value);
        return true;
    }

    bool ParseInt(int& out) {
        uint32_t value = 0;
        if (!ParseUint32(value)) return false;
        if (value > static_cast<uint32_t>(INT32_MAX)) return false;
        out = static_cast<int>(value);
        return true;
    }

    bool ParseCompressionObject() {
        if (!Consume('{')) return Error("compression must be an object");
        if (Consume('}')) return true;
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed compression object");
            if (key == "mode") {
                std::string mode;
                if (!ParseString(mode)) return Error("invalid compression.mode");
                if (mode != "auto" && mode != "zstd" && mode != "off") {
                    return Error("invalid compression.mode");
                }
                Config.Compress = mode;
            } else if (key == "level") {
                int level = 0;
                if (!ParseInt(level)) return Error("invalid compression.level");
                if (level < 1 || level > 22) return Error("compression.level out of range");
                Config.CompressLevel = level;
            } else {
                return Error("unknown compression field: " + key);
            }
        } while (Consume(','));
        return Consume('}') || Error("malformed compression object");
    }

    bool ParseTuningObject() {
        if (!Consume('{')) return Error("tuning must be an object");
        if (Consume('}')) return true;
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed tuning object");
            if (key == "max_memory_mb") {
                if (!ParseUint32(Config.MaxMemoryMb)) return Error("invalid tuning.max_memory_mb");
            } else {
                return Error("unknown tuning field: " + key);
            }
        } while (Consume(','));
        return Consume('}') || Error("malformed tuning object");
    }
};

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
    AppendString(out, "socks_listen", cfg.SocksListen);
    out << ',';
    AppendString(out, "http_listen", cfg.HttpListen);
    out << ',';
    AppendClientProto(out, cfg);
    out << ',';
    AppendProxyAuth(out, cfg.Router.ProxyAuth);
    out << ",\"speed_test\":{";
    AppendString(out, "mode", SpeedTestModeName(cfg.SpeedTestMode));
    out << ",\"duration_sec\":" << cfg.SpeedTestDurationSec << '}';
    out << ",\"handshake_threads\":" << cfg.HandshakeThreads;
    out << ",\"compression\":{";
    AppendString(out, "mode", cfg.Compress);
    out << ",\"level\":" << cfg.CompressLevel << '}';
    out << ',';
    AppendPeers(out, cfg.Router.Peers);
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
    out << ',';
    AppendString(out, "trace_level", TraceLevelName(cfg.TraceLogLevel));
    out << ",\"diag_stats\":" << (cfg.DiagStats ? "true" : "false");
    out << ",\"diag_stats_interval_sec\":" << cfg.DiagStatsIntervalSec;
    out << '}';
    return out.str();
}

bool TqApplyDiagnosticsPatch(const std::string& body, TqConfig& cfg, std::string& err) {
    DiagnosticsPatchParser parser(body, cfg, err);
    return parser.Parse();
}

bool TqApplyRuntimeConfigPatch(const std::string& body, TqConfig& cfg, std::string& err, bool& unsupported) {
    RuntimeConfigPatchParser parser(body, cfg, err, unsupported);
    return parser.Parse();
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
    out << ",\"max_memory_mb\":" << cfg.MaxMemoryMb;
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
