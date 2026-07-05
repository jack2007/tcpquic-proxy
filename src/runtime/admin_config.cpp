#include "admin_config.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <sstream>

namespace {

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

class DiagnosticsPatchParser {
public:
    DiagnosticsPatchParser(const std::string& text, TqConfig& cfg, std::string& err) :
        Text(text),
        Config(cfg),
        Err(err) {}

    bool Parse() {
        nlohmann::json root;
        if (!Load(root, "malformed diagnostics patch object")) return false;
        if (!root.is_object()) return Error("diagnostics patch must be an object");
        for (const auto& item : root.items()) {
            const std::string& key = item.key();
            if (key == "trace") {
                if (!ReadBool(item.value(), Config.Trace)) return Error("invalid trace");
            } else if (key == "trace_interval_sec") {
                if (!ReadUint32(item.value(), Config.TraceIntervalSec)) return Error("invalid trace_interval_sec");
                if (Config.TraceIntervalSec < 1 || Config.TraceIntervalSec > 86400) return Error("trace_interval_sec out of range");
            } else if (key == "trace_level") {
                std::string level;
                if (!ReadString(item.value(), level)) return Error("invalid trace_level");
                if (level == "info") Config.TraceLogLevel = TqConfig::TraceLevel::Info;
                else if (level == "debug") Config.TraceLogLevel = TqConfig::TraceLevel::Debug;
                else return Error("trace_level out of range");
            } else if (key == "diag_stats") {
                if (!ReadBool(item.value(), Config.DiagStats)) return Error("invalid diag_stats");
            } else if (key == "diag_stats_interval_sec") {
                if (!ReadUint32(item.value(), Config.DiagStatsIntervalSec)) return Error("invalid diag_stats_interval_sec");
                if (Config.DiagStatsIntervalSec < 1 || Config.DiagStatsIntervalSec > 86400) return Error("diag_stats_interval_sec out of range");
            } else {
                return Error("unknown diagnostics field: " + key);
            }
        }
        Err.clear();
        return true;
    }

private:
    const std::string& Text;
    TqConfig& Config;
    std::string& Err;

    bool Load(nlohmann::json& out, const char* err) {
        try {
            out = nlohmann::json::parse(Text);
            return true;
        } catch (const nlohmann::json::exception&) {
            return Error(err);
        }
    }

    bool ReadString(const nlohmann::json& value, std::string& out) {
        if (!value.is_string()) return false;
        out = value.get<std::string>();
        return true;
    }

    bool ReadBool(const nlohmann::json& value, bool& out) {
        if (!value.is_boolean()) return false;
        out = value.get<bool>();
        return true;
    }

    bool ReadUint32(const nlohmann::json& value, uint32_t& out) {
        uint64_t raw = 0;
        if (value.is_number_unsigned()) raw = value.get<uint64_t>();
        else if (value.is_number_integer()) {
            const int64_t signedValue = value.get<int64_t>();
            if (signedValue < 0) return false;
            raw = static_cast<uint64_t>(signedValue);
        } else return false;
        if (raw > UINT32_MAX) return false;
        out = static_cast<uint32_t>(raw);
        return true;
    }

    bool Error(const std::string& err) {
        Err = err;
        return false;
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
        nlohmann::json root;
        if (!Load(root, "malformed runtime config patch object")) return false;
        if (!root.is_object()) return Error("runtime config patch must be an object");
        for (const auto& item : root.items()) {
            const std::string& key = item.key();
            if (key == "compression") {
                if (!ParseCompressionObject(item.value())) return false;
            } else if (key == "tuning") {
                if (!ParseTuningObject(item.value())) return false;
            } else if (key == "admin" || key == "tls" || key == "proto" || key == "relay" || key == "listen") {
                return UnsupportedField(key == "admin" ? "admin.listen" : key);
            } else {
                return Error("unknown runtime config field: " + key);
            }
        }
        Err.clear();
        Unsupported = false;
        return true;
    }

private:
    const std::string& Text;
    TqConfig& Config;
    std::string& Err;
    bool& Unsupported;

    bool Load(nlohmann::json& out, const char* err) {
        try {
            out = nlohmann::json::parse(Text);
            return true;
        } catch (const nlohmann::json::exception&) {
            return Error(err);
        }
    }

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

    bool ReadString(const nlohmann::json& value, std::string& out) {
        if (!value.is_string()) return false;
        out = value.get<std::string>();
        return true;
    }

    bool ReadUint32(const nlohmann::json& value, uint32_t& out) {
        uint64_t raw = 0;
        if (value.is_number_unsigned()) raw = value.get<uint64_t>();
        else if (value.is_number_integer()) {
            const int64_t signedValue = value.get<int64_t>();
            if (signedValue < 0) return false;
            raw = static_cast<uint64_t>(signedValue);
        } else return false;
        if (raw > UINT32_MAX) return false;
        out = static_cast<uint32_t>(raw);
        return true;
    }

    bool ReadInt(const nlohmann::json& value, int& out) {
        if (!value.is_number_integer()) return false;
        const int64_t raw = value.get<int64_t>();
        if (raw < INT32_MIN || raw > INT32_MAX) return false;
        out = static_cast<int>(raw);
        return true;
    }

    bool ParseCompressionObject(const nlohmann::json& object) {
        if (!object.is_object()) return Error("compression must be an object");
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "mode") {
                std::string mode;
                if (!ReadString(item.value(), mode)) return Error("invalid compression.mode");
                if (mode != "auto" && mode != "zstd" && mode != "off") return Error("invalid compression.mode");
                Config.Compress = mode;
            } else if (key == "level") {
                int level = 0;
                if (!ReadInt(item.value(), level)) return Error("invalid compression.level");
                if (level < 1 || level > 22) return Error("compression.level out of range");
                Config.CompressLevel = level;
            } else {
                return Error("unknown compression field: " + key);
            }
        }
        return true;
    }

    bool ParseTuningObject(const nlohmann::json& object) {
        if (!object.is_object()) return Error("tuning must be an object");
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "max_memory_mb") {
                if (!ReadUint32(item.value(), Config.MaxMemoryMb)) return Error("invalid tuning.max_memory_mb");
            } else {
                return Error("unknown tuning field: " + key);
            }
        }
        return true;
    }
};

nlohmann::json PortForwardsJsonValue(const std::vector<TqPortForwardConfig>& forwards) {
    nlohmann::json values = nlohmann::json::array();
    for (const auto& forward : forwards) {
        values.push_back({
            {"listen", forward.Listen},
            {"target_host", forward.TargetHost},
            {"target_port", forward.TargetPort},
        });
    }
    return values;
}

nlohmann::json QuicPathsJsonValue(const std::vector<TqQuicPathConfig>& paths, bool recommendedNames) {
    nlohmann::json values = nlohmann::json::array();
    for (const auto& path : paths) {
        nlohmann::json item{
            {"name", path.Name},
            {"local", path.LocalAddress},
        };
        if (recommendedNames) {
            item["peer"] = path.Peer;
            item["connections"] = path.Connections;
        } else {
            item["proto_peer"] = path.Peer;
            item["proto_connections"] = path.Connections;
        }
        values.push_back(std::move(item));
    }
    return values;
}

nlohmann::json ProxyAuthJsonValue(const std::vector<TqProxyAuthUser>& users) {
    nlohmann::json values = nlohmann::json::array();
    for (const auto& user : users) {
        values.push_back({
            {"username", user.Username},
            {"password", "***"},
        });
    }
    return values;
}

nlohmann::json PeerPublicConfigJsonValue(const TqPeerConfig& peer) {
    return {
        {"id", peer.PeerId},
        {"proto_peer", peer.QuicPeer},
        {"socks_listen", peer.SocksListen},
        {"http_listen", peer.HttpListen},
        {"port_forwards", PortForwardsJsonValue(peer.PortForwards)},
        {"paths", QuicPathsJsonValue(peer.QuicPaths, false)},
        {"proto_connections", peer.QuicConnections},
        {"compress", peer.Compress},
        {"enabled", peer.Enabled},
    };
}

nlohmann::json PeersJsonValue(const std::vector<TqPeerConfig>& peers) {
    nlohmann::json values = nlohmann::json::array();
    for (const auto& peer : peers) {
        values.push_back(PeerPublicConfigJsonValue(peer));
    }
    return values;
}

nlohmann::json TlsJsonValue(const TqConfig& cfg, bool redact) {
    return {
        {"cert", cfg.QuicCert},
        {"key", redact && !cfg.QuicKey.empty() ? "***" : cfg.QuicKey},
        {"ca", cfg.QuicCa},
    };
}

nlohmann::json QuicJsonValue(const TqConfig& cfg) {
    return {
        {"listen", cfg.QuicListen},
        {"proto_peer", cfg.QuicPeer},
        {"paths", QuicPathsJsonValue(cfg.QuicPaths, false)},
        {"proto_connections", cfg.QuicConnections},
        {"connection_stream_count", cfg.QuicConnectionStreamCount},
        {"keep_alive_interval_ms", cfg.QuicKeepAliveIntervalMs},
        {"profile", QuicProfileName(cfg.QuicProfile)},
        {"disable_1rtt_encryption", cfg.QuicDisable1RttEncryption},
    };
}

nlohmann::json ClientProtoJsonValue(const TqConfig& cfg) {
    return {
        {"peer", cfg.QuicPeer},
        {"paths", QuicPathsJsonValue(cfg.QuicPaths, true)},
        {"connections", cfg.QuicConnections},
        {"connection_stream_count", cfg.QuicConnectionStreamCount},
        {"keep_alive_interval_ms", cfg.QuicKeepAliveIntervalMs},
        {"profile", QuicProfileName(cfg.QuicProfile)},
        {"disable_1rtt_encryption", cfg.QuicDisable1RttEncryption},
    };
}

} // namespace

std::string TqStructuredErrorJson(const std::string& code, const std::string& message) {
    return nlohmann::json{{"error", {{"code", code}, {"message", message}}}}.dump();
}

std::string TqPeerPublicConfigJson(const TqConfig& cfg, const TqPeerConfig& peer) {
    (void)cfg;
    return PeerPublicConfigJsonValue(peer).dump();
}

std::string TqClientPublicConfigJson(const TqConfig& cfg) {
    nlohmann::json body{
        {"role", "client"},
        {"client_name", cfg.ClientName},
        {"socks_listen", cfg.SocksListen},
        {"http_listen", cfg.HttpListen},
        {"proto", ClientProtoJsonValue(cfg)},
        {"proxy_auth", ProxyAuthJsonValue(cfg.Router.ProxyAuth)},
        {"speed_test", {{"mode", SpeedTestModeName(cfg.SpeedTestMode)}, {"duration_sec", cfg.SpeedTestDurationSec}}},
        {"handshake_threads", cfg.HandshakeThreads},
        {"compression", {{"mode", cfg.Compress}, {"level", cfg.CompressLevel}}},
        {"peers", PeersJsonValue(cfg.Router.Peers)},
    };
    return body.dump();
}

std::string TqDiagnosticsJson(const TqConfig& cfg) {
    nlohmann::json body{
        {"role", RoleName(cfg.Mode)},
        {"tuning_mode", TuningModeName(cfg.TuningMode)},
        {"max_memory_mb", cfg.MaxMemoryMb},
        {"trace", cfg.Trace},
        {"trace_interval_sec", cfg.TraceIntervalSec},
        {"trace_level", TraceLevelName(cfg.TraceLogLevel)},
        {"diag_stats", cfg.DiagStats},
        {"diag_stats_interval_sec", cfg.DiagStatsIntervalSec},
    };
    return body.dump();
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
    nlohmann::json body{
        {"role", RoleName(cfg.Mode)},
        {"client_name", cfg.ClientName},
        {"config_path", cfg.ConfigPath},
        {"client_config_path", cfg.ClientConfigPath},
        {"admin", {{"listen", cfg.AdminListen}, {"token_file", cfg.AdminTokenFile}, {"threads", cfg.AdminThreads}}},
        {"socks_listen", cfg.SocksListen},
        {"http_listen", cfg.HttpListen},
        {"quic", QuicJsonValue(cfg)},
        {"tls", TlsJsonValue(cfg, redact)},
        {"port_forwards", PortForwardsJsonValue(cfg.PortForwards)},
        {"router", {{"version", cfg.Router.Version}, {"proxy_auth", ProxyAuthJsonValue(cfg.Router.ProxyAuth)}, {"peers", PeersJsonValue(cfg.Router.Peers)}}},
        {"compress", cfg.Compress},
        {"compress_level", cfg.CompressLevel},
        {"tuning_mode", TuningModeName(cfg.TuningMode)},
        {"max_memory_mb", cfg.MaxMemoryMb},
    };
    return body.dump();
}

std::string TqServerRuntimeConfigJson(
    const TqConfig& cfg,
    const std::vector<std::string>& resolvedListens,
    bool redact) {
    nlohmann::json body{
        {"role", "server"},
        {"config_path", cfg.ConfigPath},
        {"listen", cfg.QuicListen},
        {"resolved_listens", resolvedListens},
        {"allow_targets", cfg.AllowTargets},
        {"deny_targets", cfg.DenyTargets},
        {"quic", QuicJsonValue(cfg)},
        {"tls", TlsJsonValue(cfg, redact)},
        {"tuning_mode", TuningModeName(cfg.TuningMode)},
    };
    return body.dump();
}
