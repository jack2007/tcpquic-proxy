#include "config.h"

#include "acl.h"
#include "proxy_auth.h"
#include "tuning.h"

#include <spdlog/spdlog.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>

namespace {

constexpr uint32_t TqMaxQuicConnectionStreamCount = 65535;
constexpr uint32_t TqMinQuicKeepAliveIntervalMs = 1000;
constexpr uint32_t TqMaxQuicKeepAliveIntervalMs = 15000;

const char* NextArg(int& i, int argc, char** argv, const char* flag, std::string& err) {
    if (i + 1 >= argc) {
        err = std::string("missing value for ") + flag;
        return nullptr;
    }
    ++i;
    return argv[i];
}

bool GetOptionValue(const char* arg, const char* prefix, const char*& value) {
    const size_t len = std::strlen(prefix);
    if (std::strncmp(arg, prefix, len) != 0) {
        return false;
    }
    if (arg[len] == '=') {
        value = arg + len + 1;
        return true;
    }
    if (arg[len] == '\0') {
        value = nullptr;
        return true;
    }
    return false;
}

bool ParseUint32(const char* s, uint32_t& out) {
    char* end = nullptr;
    const unsigned long v = std::strtoul(s, &end, 10);
    if (end == s || *end != '\0' || v > UINT32_MAX) {
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

bool ParseUint32InRange(const char* s, uint32_t minValue, uint32_t maxValue, uint32_t& out) {
    if (!ParseUint32(s, out)) {
        return false;
    }
    return out >= minValue && out <= maxValue;
}

bool ParseQuicConnectionStreamCountValue(const char* s, uint32_t& out) {
    return ParseUint32InRange(s, 1, TqMaxQuicConnectionStreamCount, out);
}

bool ParseInt(const char* s, int& out) {
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0') {
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

void SplitCommaList(const std::string& value, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        if (comma == std::string::npos) {
            comma = value.size();
        }
        std::string item = value.substr(start, comma - start);
        const size_t begin = item.find_first_not_of(" \t");
        const size_t end = item.find_last_not_of(" \t");
        if (begin != std::string::npos) {
            out.push_back(item.substr(begin, end - begin + 1));
        }
        start = comma + 1;
    }
}

bool RequireNonEmpty(const std::string& value, const char* flag, std::string& err) {
    if (value.empty()) {
        err = std::string("missing required option: ") + flag;
        return false;
    }
    return true;
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
        port = port * 10 + static_cast<uint32_t>(value[i] - '0');
        if (port > 65535) {
            return false;
        }
    }
    return port != 0;
}

bool IsValidCompress(const std::string& value) {
    return value.empty() || value == "auto" || value == "zstd" || value == "off";
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

class JsonParser {
public:
    JsonParser(const std::string& text, std::string& err) : Text(text), Err(err) {}

    bool ParseRouter(TqRouterConfig& router) {
        router = TqRouterConfig{};
        if (!Consume('{')) {
            return Error("client config must be a JSON object");
        }
        if (Consume('}')) {
            return Finish(router);
        }
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) {
                return Error("malformed client config object");
            }
            if (key == "version") {
                if (!ParseUint32(router.Version)) {
                    return Error("invalid version");
                }
            } else if (key == "peers") {
                if (!ParsePeers(router.Peers)) {
                    return false;
                }
            } else if (key == "proxy_auth") {
                if (!ParseProxyAuth(router.ProxyAuth)) {
                    return false;
                }
            } else if (!SkipValue()) {
                return false;
            }
        } while (Consume(','));

        if (!Consume('}')) {
            return Error("malformed client config object");
        }
        return Finish(router);
    }

    bool ParseRuntimeConfig(TqConfig& cfg) {
        if (!Consume('{')) {
            return Error("config must be a JSON object");
        }
        if (Consume('}')) {
            return FinishRuntimeConfig();
        }
        bool speedTestSpecified = cfg.SpeedTestMode != TqSpeedTestMode::None;
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) {
                return Error("malformed config object");
            }
            if (!ParseRuntimeConfigField(key, cfg, speedTestSpecified)) {
                return false;
            }
        } while (Consume(','));

        if (!Consume('}')) {
            return Error("malformed config object");
        }
        return FinishRuntimeConfig();
    }

private:
    bool Finish(TqRouterConfig& router) {
        SkipWs();
        if (Pos != Text.size()) {
            return Error("unexpected trailing content in client config");
        }
        return TqValidateRouterConfig(router, Err);
    }

    bool FinishRuntimeConfig() {
        SkipWs();
        if (Pos != Text.size()) {
            return Error("unexpected trailing content in config");
        }
        return true;
    }

    bool ParseRuntimeConfigField(const std::string& key, TqConfig& cfg, bool& speedTestSpecified) {
        if (key == "tls") return ParseTlsConfig(cfg);
        if (key == "admin") return ParseAdminConfig(cfg);
        if (key == "proto") return ParseProtoConfig(cfg);
        if (key == "server") return ParseServerConfig(cfg);
        if (key == "relay") return ParseRelayConfig(cfg);
        if (key == "tuning") return ParseTuningConfig(cfg);
        if (key == "compression") return ParseCompressionConfig(cfg);
        if (key == "trace") return ParseTraceConfig(cfg);
        if (key == "client") return ParseClientConfig(cfg, speedTestSpecified);
        if (key == "peers") return ParseRuntimePeers(cfg.Router.Peers);
        return Error(("unknown config key: " + key).c_str());
    }

    bool ParseTlsConfig(TqConfig& cfg) {
        if (!Consume('{')) return Error("tls must be an object");
        if (Consume('}')) return true;
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed tls object");
            if (key == "cert") {
                if (!ParseString(cfg.QuicCert)) return Error("invalid tls.cert");
            } else if (key == "key") {
                if (!ParseString(cfg.QuicKey)) return Error("invalid tls.key");
            } else if (key == "ca") {
                if (!ParseString(cfg.QuicCa)) return Error("invalid tls.ca");
            } else {
                return Error(("unknown tls key: " + key).c_str());
            }
        } while (Consume(','));
        return Consume('}') || Error("malformed tls object");
    }

    bool ParseAdminConfig(TqConfig& cfg) {
        if (!Consume('{')) return Error("admin must be an object");
        if (Consume('}')) return true;
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed admin object");
            if (key == "listen") {
                if (!ParseString(cfg.AdminListen)) return Error("invalid admin.listen");
            } else {
                return Error(("unknown admin key: " + key).c_str());
            }
        } while (Consume(','));
        return Consume('}') || Error("malformed admin object");
    }

    bool ParseProtoConfig(TqConfig& cfg) {
        if (!Consume('{')) return Error("proto must be an object");
        if (Consume('}')) return true;
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed proto object");
            if (key == "profile") {
                std::string value;
                if (!ParseString(value)) return Error("invalid proto.profile");
                if (value == "max-throughput") cfg.QuicProfile = TqQuicProfile::MaxThroughput;
                else if (value == "low-latency") cfg.QuicProfile = TqQuicProfile::LowLatency;
                else return Error("invalid proto.profile");
            } else if (key == "disable_1rtt_encryption") {
                if (!ParseBool(cfg.QuicDisable1RttEncryption)) return Error("invalid proto.disable_1rtt_encryption");
            } else if (key == "connections") {
                if (!ParseUint32(cfg.QuicConnections) || cfg.QuicConnections > 128) return Error("invalid proto.connections");
                if (cfg.QuicConnections == 0) cfg.QuicConnections = 1;
            } else if (key == "connection_stream_count") {
                if (!ParseUint32(cfg.QuicConnectionStreamCount) ||
                    cfg.QuicConnectionStreamCount == 0 ||
                    cfg.QuicConnectionStreamCount > TqMaxQuicConnectionStreamCount) {
                    return Error("invalid proto.connection_stream_count");
                }
            } else if (key == "reconnect_interval_ms") {
                if (!ParseReconnectInterval(cfg.QuicReconnectIntervalMs)) return Error("invalid proto.reconnect_interval_ms");
            } else if (key == "keepalive_ms") {
                if (!ParseUint32InRange(
                        TqMinQuicKeepAliveIntervalMs,
                        TqMaxQuicKeepAliveIntervalMs,
                        cfg.QuicKeepAliveIntervalMs)) {
                    return Error("invalid proto.keepalive_ms");
                }
            } else if (key == "fcw") {
                if (!ParseUint32(cfg.TuningOverrideQuicFcw)) return Error("invalid proto.fcw");
            } else if (key == "srw") {
                if (!ParseUint32(cfg.TuningOverrideQuicSrw)) return Error("invalid proto.srw");
            } else if (key == "iw") {
                if (!ParseUint32(cfg.TuningOverrideQuicIw)) return Error("invalid proto.iw");
            } else if (key == "initrtt_ms") {
                if (!ParseUint32(cfg.TuningOverrideQuicInitRttMs)) return Error("invalid proto.initrtt_ms");
            } else {
                return Error(("unknown proto key: " + key).c_str());
            }
        } while (Consume(','));
        return Consume('}') || Error("malformed proto object");
    }

    bool ParseServerConfig(TqConfig& cfg) {
        if (!Consume('{')) return Error("server must be an object");
        if (Consume('}')) return true;
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed server object");
            if (key == "proto_listen") {
                if (!ParseString(cfg.QuicListen)) return Error("invalid server.proto_listen");
            } else if (key == "allow_targets") {
                if (!ParseStringList(cfg.AllowTargets)) return Error("invalid server.allow_targets");
            } else if (key == "deny_targets") {
                if (!ParseStringList(cfg.DenyTargets)) return Error("invalid server.deny_targets");
            } else {
                return Error(("unknown server key: " + key).c_str());
            }
        } while (Consume(','));
        return Consume('}') || Error("malformed server object");
    }

    bool ParseRelayConfig(TqConfig& cfg) {
        if (!Consume('{')) return Error("relay must be an object");
        if (Consume('}')) return true;
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed relay object");
            if (key == "io_size") {
                if (!ParseUint32(cfg.TuningOverrideRelayIoSize)) return Error("invalid relay.io_size");
            } else if (key == "inflight_bytes") {
                if (!ParseNonZeroUint32(
                        cfg.TuningOverrideRelayInflightBytes,
                        "relay.inflight_bytes")) return false;
            } else if (key == "initial_quic_read_ahead") {
                if (!ParseNonZeroUint32(
                        cfg.TuningOverrideInitialQuicReadAheadBytes,
                        "relay.initial_quic_read_ahead")) return false;
            } else if (key == "linux") {
                if (!ParseLinuxRelayConfig(cfg)) return false;
            } else {
                return Error(("unknown relay key: " + key).c_str());
            }
        } while (Consume(','));
        return Consume('}') || Error("malformed relay object");
    }

    bool ParseLinuxRelayConfig(TqConfig& cfg) {
        if (!Consume('{')) return Error("relay.linux must be an object");
        if (Consume('}')) return true;
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed relay.linux object");
            if (key == "read_chunk_size") {
                if (!ParseNonZeroUint32(cfg.TuningOverrideLinuxRelayReadChunkSize, "relay.linux.read_chunk_size")) return false;
            } else if (key == "worker_slots") {
                uint32_t ignored = 0;
                if (!ParseNonZeroUint32(ignored, "relay.linux.worker_slots")) return false;
                spdlog::warn("relay.linux.worker_slots is deprecated and ignored");
            } else if (key == "tcp_write_max_bytes") {
                if (!ParseNonZeroUint32(cfg.TuningOverrideLinuxRelayTcpWriteMaxBytes, "relay.linux.tcp_write_max_bytes")) return false;
            } else if (key == "tcp_write_burst_bytes") {
                if (!ParseNonZeroUint32(cfg.TuningOverrideLinuxRelayTcpWriteBurstBytes, "relay.linux.tcp_write_burst_bytes")) return false;
            } else {
                return Error(("unknown relay.linux key: " + key).c_str());
            }
        } while (Consume(','));
        return Consume('}') || Error("malformed relay.linux object");
    }

    bool ParseTuningConfig(TqConfig& cfg) {
        if (!Consume('{')) return Error("tuning must be an object");
        if (Consume('}')) return true;
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed tuning object");
            if (key == "mode") {
                std::string value;
                if (!ParseString(value)) return Error("invalid tuning.mode");
                cfg.TuningMode = TqParseTuningMode(value.c_str());
                if (value != "auto" && value != "lan" && value != "wan" && value != "custom") return Error("invalid tuning.mode");
            } else if (key == "target_bandwidth_mbps") {
                if (!ParseUint32(cfg.TargetBandwidthMbps)) return Error("invalid tuning.target_bandwidth_mbps");
            } else if (key == "target_rtt_ms") {
                if (!ParseUint32(cfg.TargetRttMs)) return Error("invalid tuning.target_rtt_ms");
            } else {
                return Error(("unknown tuning key: " + key).c_str());
            }
        } while (Consume(','));
        return Consume('}') || Error("malformed tuning object");
    }

    bool ParseCompressionConfig(TqConfig& cfg) {
        if (!Consume('{')) return Error("compression must be an object");
        if (Consume('}')) return true;
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed compression object");
            if (key == "mode") {
                if (!ParseString(cfg.Compress)) return Error("invalid compression.mode");
                if (!IsValidCompress(cfg.Compress)) return Error("invalid compression.mode");
            } else if (key == "level") {
                if (!ParseInt(cfg.CompressLevel)) return Error("invalid compression.level");
            } else {
                return Error(("unknown compression key: " + key).c_str());
            }
        } while (Consume(','));
        return Consume('}') || Error("malformed compression object");
    }

    bool ParseTraceConfig(TqConfig& cfg) {
        if (!Consume('{')) return Error("trace must be an object");
        if (Consume('}')) return true;
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed trace object");
            if (key == "enabled") {
                if (!ParseBool(cfg.Trace)) return Error("invalid trace.enabled");
            } else if (key == "interval_sec") {
                if (!ParseUint32(cfg.TraceIntervalSec)) return Error("invalid trace.interval_sec");
                cfg.Trace = true;
            } else if (key == "connect_on_start") {
                if (!ParseBool(cfg.TraceConnectOnStart)) return Error("invalid trace.connect_on_start");
                if (cfg.TraceConnectOnStart) cfg.Trace = true;
            } else {
                return Error(("unknown trace key: " + key).c_str());
            }
        } while (Consume(','));
        return Consume('}') || Error("malformed trace object");
    }

    bool ParseClientConfig(TqConfig& cfg, bool& speedTestSpecified) {
        if (!Consume('{')) return Error("client must be an object");
        if (Consume('}')) return true;
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) return Error("malformed client object");
            if (key == "warmup_mb") {
                if (!ParseUint32(cfg.WarmupMb)) return Error("invalid client.warmup_mb");
            } else if (key == "warmup_target") {
                if (!ParseString(cfg.WarmupTarget)) return Error("invalid client.warmup_target");
            } else if (key == "warmup_path") {
                if (!ParseString(cfg.WarmupPath)) return Error("invalid client.warmup_path");
            } else if (key == "download_test") {
                if (!ParseSpeedTest(TqSpeedTestMode::Download, cfg, speedTestSpecified, "client.download_test")) return false;
            } else if (key == "download_sink_test") {
                if (!ParseSpeedTest(TqSpeedTestMode::DownloadSink, cfg, speedTestSpecified, "client.download_sink_test")) return false;
            } else if (key == "upload_test") {
                if (!ParseSpeedTest(TqSpeedTestMode::Upload, cfg, speedTestSpecified, "client.upload_test")) return false;
            } else if (key == "handshake_threads") {
                if (!ParseUint32(cfg.HandshakeThreads)) return Error("invalid client.handshake_threads");
            } else {
                return Error(("unknown client key: " + key).c_str());
            }
        } while (Consume(','));
        return Consume('}') || Error("malformed client object");
    }

    bool ParseSpeedTest(TqSpeedTestMode mode, TqConfig& cfg, bool& speedTestSpecified, const char* key) {
        if (speedTestSpecified) {
            return Error("speed-test options are mutually exclusive");
        }
        if (!ParseUint32InRange(1, 86400, cfg.SpeedTestDurationSec)) {
            return Error((std::string("invalid ") + key).c_str());
        }
        cfg.SpeedTestMode = mode;
        speedTestSpecified = true;
        return true;
    }

    bool ParseNonZeroUint32(uint32_t& out, const char* key) {
        if (!ParseUint32(out) || out == 0) {
            return Error((std::string("invalid ") + key).c_str());
        }
        return true;
    }

    bool ParseReconnectInterval(uint32_t& out) {
        return ParseUint32(out) && out >= 1000 && out <= 60000;
    }

    bool ParseRuntimePeers(std::vector<TqPeerConfig>& peers) {
        peers.clear();
        if (!Consume('[')) {
            return Error("peers must be an array");
        }
        if (Consume(']')) {
            return true;
        }
        do {
            TqPeerConfig peer;
            if (!ParseRuntimePeer(peer)) {
                return false;
            }
            peers.push_back(peer);
        } while (Consume(','));
        return Consume(']') || Error("malformed peers array");
    }

    bool ParseRuntimePeer(TqPeerConfig& peer) {
        if (!Consume('{')) {
            return Error("peer must be an object");
        }
        if (Consume('}')) {
            return true;
        }
        bool protoConnectionsSpecified = false;
        bool protoReconnectIntervalSpecified = false;
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) {
                return Error("malformed peer object");
            }
            if (key == "id") {
                if (!ParseString(peer.PeerId)) return Error("invalid peer.id");
            } else if (key == "proto_peer") {
                if (!ParseString(peer.QuicPeer)) return Error("invalid peer.proto_peer");
            } else if (key == "socks_listen") {
                if (!ParseString(peer.SocksListen)) return Error("invalid peer.socks_listen");
            } else if (key == "http_listen") {
                if (!ParseString(peer.HttpListen)) return Error("invalid peer.http_listen");
            } else if (key == "proto_connections") {
                protoConnectionsSpecified = true;
                if (!ParseUint32(peer.QuicConnections)) return Error("invalid peer.proto_connections");
            } else if (key == "proto_reconnect_interval_ms") {
                protoReconnectIntervalSpecified = true;
                if (!ParseUint32(peer.QuicReconnectIntervalMs)) return Error("invalid peer.proto_reconnect_interval_ms");
            } else if (key == "compress") {
                if (!ParseString(peer.Compress)) return Error("invalid peer.compress");
            } else if (key == "enabled") {
                if (!ParseBool(peer.Enabled)) return Error("invalid peer.enabled");
            } else {
                return Error(("unknown peer key: " + key).c_str());
            }
        } while (Consume(','));
        if (protoConnectionsSpecified && peer.QuicConnections == 0) {
            return Error("peer.proto_connections out of range");
        }
        if (protoReconnectIntervalSpecified &&
            (peer.QuicReconnectIntervalMs < 1000 || peer.QuicReconnectIntervalMs > 60000)) {
            return Error("peer.proto_reconnect_interval_ms out of range");
        }
        return Consume('}') || Error("malformed peer object");
    }

    bool ParsePeers(std::vector<TqPeerConfig>& peers) {
        peers.clear();
        if (!Consume('[')) {
            return Error("peers must be an array");
        }
        if (Consume(']')) {
            return true;
        }
        do {
            TqPeerConfig peer;
            if (!ParsePeer(peer)) {
                return false;
            }
            peers.push_back(peer);
        } while (Consume(','));
        return Consume(']') || Error("malformed peers array");
    }

    bool ParsePeer(TqPeerConfig& peer) {
        if (!Consume('{')) {
            return Error("peer must be an object");
        }
        if (Consume('}')) {
            return true;
        }
        bool quicConnectionsSpecified = false;
        bool quicReconnectIntervalSpecified = false;
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) {
                return Error("malformed peer object");
            }
            if (key == "peer_id") {
                if (!ParseString(peer.PeerId)) return Error("invalid peer_id");
            } else if (key == "quic_peer") {
                if (!ParseString(peer.QuicPeer)) return Error("invalid quic_peer");
            } else if (key == "socks_listen") {
                if (!ParseString(peer.SocksListen)) return Error("invalid socks_listen");
            } else if (key == "http_listen") {
                if (!ParseString(peer.HttpListen)) return Error("invalid http_listen");
            } else if (key == "quic_connections") {
                quicConnectionsSpecified = true;
                if (!ParseUint32(peer.QuicConnections)) return Error("invalid quic_connections");
            } else if (key == "quic_reconnect_interval_ms") {
                quicReconnectIntervalSpecified = true;
                if (!ParseUint32(peer.QuicReconnectIntervalMs)) return Error("invalid quic_reconnect_interval_ms");
            } else if (key == "compress") {
                if (!ParseString(peer.Compress)) return Error("invalid compress");
            } else if (key == "enabled") {
                if (!ParseBool(peer.Enabled)) return Error("invalid enabled");
            } else if (!SkipValue()) {
                return false;
            }
        } while (Consume(','));
        if (quicConnectionsSpecified && peer.QuicConnections == 0) {
            return Error("quic_connections out of range");
        }
        if (quicReconnectIntervalSpecified && peer.QuicReconnectIntervalMs == 0) {
            return Error("quic_reconnect_interval_ms out of range");
        }
        return Consume('}') || Error("malformed peer object");
    }

    bool ParseProxyAuth(std::vector<TqProxyAuthUser>& users) {
        users.clear();
        if (!Consume('[')) {
            return Error("proxy_auth must be an array");
        }
        if (Consume(']')) {
            return true;
        }
        do {
            TqProxyAuthUser user;
            if (!ParseProxyAuthUser(user)) {
                return false;
            }
            users.push_back(std::move(user));
        } while (Consume(','));
        return Consume(']') || Error("malformed proxy_auth array");
    }

    bool ParseProxyAuthUser(TqProxyAuthUser& user) {
        if (!Consume('{')) {
            return Error("proxy_auth entry must be an object");
        }
        if (Consume('}')) {
            return true;
        }
        do {
            std::string key;
            if (!ParseString(key) || !Consume(':')) {
                return Error("malformed proxy_auth object");
            }
            if (key == "username") {
                if (!ParseString(user.Username)) return Error("invalid proxy_auth.username");
            } else if (key == "password") {
                if (!ParseString(user.Password)) return Error("invalid proxy_auth.password");
            } else {
                return Error(("unknown proxy_auth key: " + key).c_str());
            }
        } while (Consume(','));
        return Consume('}') || Error("malformed proxy_auth object");
    }

    bool ParseString(std::string& out) {
        SkipWs();
        if (Pos >= Text.size() || Text[Pos] != '"') {
            return false;
        }
        ++Pos;
        out.clear();
        while (Pos < Text.size()) {
            char ch = Text[Pos++];
            if (ch == '"') {
                return true;
            }
            if (ch == '\\') {
                if (Pos >= Text.size()) {
                    return false;
                }
                char escaped = Text[Pos++];
                switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(escaped);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    if (Pos + 4 > Text.size()) {
                        return false;
                    }
                    uint32_t value = 0;
                    for (int i = 0; i < 4; ++i) {
                        int hex = HexValue(Text[Pos++]);
                        if (hex < 0) {
                            return false;
                        }
                        value = (value << 4) | static_cast<uint32_t>(hex);
                    }
                    AppendUtf8(value, out);
                    break;
                }
                default:
                    return false;
                }
            } else {
                if (static_cast<unsigned char>(ch) < 0x20) {
                    return false;
                }
                out.push_back(ch);
            }
        }
        return false;
    }

    bool ParseUint32(uint32_t& out) {
        SkipWs();
        if (Pos >= Text.size() || !std::isdigit(static_cast<unsigned char>(Text[Pos]))) {
            return false;
        }
        if (Text[Pos] == '0' && Pos + 1 < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Pos + 1]))) {
            return false;
        }
        uint64_t value = 0;
        while (Pos < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Pos]))) {
            value = value * 10 + static_cast<uint32_t>(Text[Pos] - '0');
            if (value > std::numeric_limits<uint32_t>::max()) {
                return false;
            }
            ++Pos;
        }
        out = static_cast<uint32_t>(value);
        return true;
    }

    bool ParseUint32InRange(uint32_t minValue, uint32_t maxValue, uint32_t& out) {
        if (!ParseUint32(out)) {
            return false;
        }
        return out >= minValue && out <= maxValue;
    }

    bool ParseInt(int& out) {
        SkipWs();
        if (Pos >= Text.size()) {
            return false;
        }
        bool negative = false;
        if (Text[Pos] == '-') {
            negative = true;
            ++Pos;
        }
        if (Pos >= Text.size() || !std::isdigit(static_cast<unsigned char>(Text[Pos]))) {
            return false;
        }
        if (Text[Pos] == '0' && Pos + 1 < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Pos + 1]))) {
            return false;
        }
        uint64_t value = 0;
        while (Pos < Text.size() && std::isdigit(static_cast<unsigned char>(Text[Pos]))) {
            value = value * 10 + static_cast<uint32_t>(Text[Pos] - '0');
            const uint64_t limit = negative
                ? static_cast<uint64_t>(std::numeric_limits<int>::max()) + 1ull
                : static_cast<uint64_t>(std::numeric_limits<int>::max());
            if (value > limit) {
                return false;
            }
            ++Pos;
        }
        if (negative && value == static_cast<uint64_t>(std::numeric_limits<int>::max()) + 1ull) {
            out = std::numeric_limits<int>::min();
        } else {
            out = negative ? -static_cast<int>(value) : static_cast<int>(value);
        }
        return true;
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

    bool ParseStringList(std::vector<std::string>& out) {
        SkipWs();
        if (Pos < Text.size() && Text[Pos] == '"') {
            std::string value;
            if (!ParseString(value)) {
                return false;
            }
            SplitCommaList(value, out);
            return true;
        }
        if (!Consume('[')) {
            return false;
        }
        out.clear();
        if (Consume(']')) {
            return true;
        }
        do {
            std::string item;
            if (!ParseString(item)) {
                return false;
            }
            out.push_back(item);
        } while (Consume(','));
        return Consume(']');
    }

    bool SkipValue() {
        SkipWs();
        if (Pos >= Text.size()) {
            return Error("unexpected end of client config");
        }
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
        bool ignoredBool = false;
        if (ParseBool(ignoredBool)) {
            return true;
        }
        uint32_t ignoredUint = 0;
        if (ParseUint32(ignoredUint)) {
            return true;
        }
        return Error("unsupported value in client config");
    }

    bool Consume(char expected) {
        SkipWs();
        if (Pos < Text.size() && Text[Pos] == expected) {
            ++Pos;
            return true;
        }
        return false;
    }

    void SkipWs() {
        while (Pos < Text.size() && std::isspace(static_cast<unsigned char>(Text[Pos]))) {
            ++Pos;
        }
    }

    bool Error(const char* message) {
        Err = message;
        return false;
    }

    const std::string& Text;
    std::string& Err;
    size_t Pos{0};
};

bool LoadRuntimeConfigFile(const std::string& path, TqConfig& cfg, std::string& err) {
    std::ifstream file(path);
    if (!file) {
        err = "failed to open config: " + path;
        return false;
    }
    std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    JsonParser parser(body, err);
    return parser.ParseRuntimeConfig(cfg);
}

} // namespace

void TqPrintUsage(FILE* out) {
    std::fprintf(out,
        "Usage: tcpquic-proxy client|server [options]\n"
        "\n"
        "Client and Server:\n"
        "  --config <path>              Runtime JSON config file\n"
        "                              (preferred; see docs/config_guide.md)\n"
        "  --cert <path>                TLS certificate PEM path\n"
        "  --key <path>                 TLS private key PEM path\n"
        "  --ca <path>                  CA certificate PEM path\n"
        "  --admin-listen <addr>        Admin HTTP listen address for /health and /metrics\n"
        "  --compress <mode>            auto|zstd|off (default off)\n"
        "  --compress-level <n>         Compression level (default 1)\n"
        "\n"
        "Client specific:\n"
        "  --socks-listen <addr>        SOCKS5 listen address (default 127.0.0.1:1080)\n"
        "  --http-listen <addr>         HTTP CONNECT listen address (default 127.0.0.1:8080)\n"
        "  --client-config <path>       Legacy router client config JSON\n"
        "  --peer <addr>                Legacy single-peer address\n"
        "  --connections <n>            Connection count (default 1)\n"
        "  --connection-stream-count <n>\n"
        "                              Max bidirectional streams per connection (default 1024)\n"
        "  --reconnect-interval-ms <n>\n"
        "                              Client reconnect interval in ms (default 3000, 1000..60000)\n"
        "  --keepalive-ms <n>          Keepalive interval in ms (default 5000, 1000..15000)\n"
        "  --handshake-threads <n>      SOCKS/HTTP handshake workers (default 8, 0=auto)\n"
        "  --download-test <sec>        Built-in end-to-end download speed test\n"
        "  --upload-test <sec>          Built-in end-to-end upload speed test\n"
        "\n"
        "Server specific:\n"
        "  --listen <addr>              Listen address\n"
        "  --allow-targets <list>       Allowed CIDR list, comma-separated (default 0.0.0.0/0)\n"
        "  --deny-targets <list>        Denied CIDR list, comma-separated\n"
        "\n"
        "Protocol and relay tuning:\n"
        "  --profile <mode>             max-throughput|low-latency (default max-throughput)\n"
        "  --enable-encrypt            Enable packet encryption\n"
        "  --fcw <bytes>                Override connection flow window\n"
        "  --srw <bytes>                Override stream recv window\n"
        "  --iw <packets>               Override initial window packets\n"
        "  --initrtt-ms <n>             Override initial RTT\n"
        "  --tuning <mode>              auto|lan|wan|custom (default wan)\n"
        "  --target-bandwidth-mbps <n>  Target bandwidth for auto/custom BDP\n"
        "  --target-rtt-ms <n>          Target RTT for auto/custom BDP\n"
        "  --max-memory-mb <n>          Cap relay pool memory across tunnels\n"
        "  --relay-io-size <bytes>      Override relay IO size\n"
        "  --relay-inflight-bytes <n>   Override relay ideal in-flight bytes\n"
        "  --initial-quic-read-ahead <bytes>\n"
        "                              Initial/minimum read-ahead bytes (default 1048576)\n"
        "  --linux-relay-read-chunk-size <bytes>\n"
        "                              Linux relay TCP read chunk size\n"
        "  --linux-relay-tcp-write-max-bytes <bytes>\n"
        "                              Cap each Linux relay TCP sendmsg\n"
        "  --linux-relay-tcp-write-burst-bytes <bytes>\n"
        "                              Cap bytes per Linux relay TCP write flush\n"
        "\n"
        "Diagnostics:\n"
        "  --trace                      Event + periodic debug trace (spdlog file log)\n"
        "  --trace-interval <sec>       Periodic stats interval when --trace (default 10)\n"
        "  --trace-connect-on-start     Client: connect at startup (debug)\n");
}

void TqFinalizeConfig(TqConfig& cfg) {
    TqComputeTuning(cfg, cfg.Tuning);
}

bool TqParseArgs(int argc, char** argv, TqConfig& cfg, std::string& err) {
    cfg = TqConfig{};

    if (argc < 2) {
        err = "missing mode (client|server)";
        return false;
    }

    if (std::strcmp(argv[1], "client") == 0) {
        cfg.Mode = TqMode::Client;
    } else if (std::strcmp(argv[1], "server") == 0) {
        cfg.Mode = TqMode::Server;
    } else if (argv[1][0] == '-') {
        err = "missing mode (client|server)";
        return false;
    } else {
        err = std::string("unknown mode: ") + argv[1];
        return false;
    }

    for (int i = 2; i < argc; ++i) {
        const char* arg = argv[i];
        const char* value = nullptr;
        if (!GetOptionValue(arg, "--config", value)) {
            continue;
        }
        if (value == nullptr) {
            value = NextArg(i, argc, argv, "--config", err);
            if (value == nullptr) {
                return false;
            }
        }
        if (!cfg.ConfigPath.empty()) {
            err = "--config specified more than once";
            return false;
        }
        cfg.ConfigPath = value;
        if (!LoadRuntimeConfigFile(cfg.ConfigPath, cfg, err)) {
            return false;
        }
    }

    bool quicPeerSpecified = false;
    bool speedTestSpecified = false;
    for (int i = 2; i < argc; ++i) {
        const char* arg = argv[i];
        const char* value = nullptr;

        if (std::strcmp(arg, "--compress-min-size") == 0 ||
            GetOptionValue(arg, "--compress-min-size", value)) {
            err = "unsupported option: --compress-min-size";
            return false;
        }

        if (GetOptionValue(arg, "--config", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--config", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.ConfigPath = value;
        } else if (GetOptionValue(arg, "--socks-listen", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--socks-listen", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.SocksListen = value;
        } else if (GetOptionValue(arg, "--http-listen", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--http-listen", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.HttpListen = value;
        } else if (GetOptionValue(arg, "--peer", value)) {
            quicPeerSpecified = true;
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--peer", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.QuicPeer = value;
        } else if (GetOptionValue(arg, "--client-config", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--client-config", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.ClientConfigPath = value;
        } else if (GetOptionValue(arg, "--admin-listen", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--admin-listen", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.AdminListen = value;
        } else if (GetOptionValue(arg, "--listen", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--listen", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.QuicListen = value;
        } else if (GetOptionValue(arg, "--cert", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--cert", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.QuicCert = value;
        } else if (GetOptionValue(arg, "--key", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--key", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.QuicKey = value;
        } else if (GetOptionValue(arg, "--ca", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--ca", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.QuicCa = value;
        } else if (GetOptionValue(arg, "--connections", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--connections", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.QuicConnections)) {
                err = "invalid value for --connections";
                return false;
            }
            if (cfg.QuicConnections > 128) {
                err = "--connections must be <= 128";
                return false;
            }
            if (cfg.QuicConnections == 0) {
                cfg.QuicConnections = 1;
            }
        } else if (GetOptionValue(arg, "--connection-stream-count", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--connection-stream-count", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseQuicConnectionStreamCountValue(value, cfg.QuicConnectionStreamCount)) {
                err = "invalid value for --connection-stream-count (must be 1..65535)";
                return false;
            }
        } else if (GetOptionValue(arg, "--reconnect-interval-ms", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--reconnect-interval-ms", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.QuicReconnectIntervalMs) ||
                cfg.QuicReconnectIntervalMs < 1000 ||
                cfg.QuicReconnectIntervalMs > 60000) {
                err = "invalid value for --reconnect-interval-ms (must be 1000..60000)";
                return false;
            }
        } else if (GetOptionValue(arg, "--keepalive-ms", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--keepalive-ms", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32InRange(
                    value,
                    TqMinQuicKeepAliveIntervalMs,
                    TqMaxQuicKeepAliveIntervalMs,
                    cfg.QuicKeepAliveIntervalMs)) {
                err = "invalid value for --keepalive-ms (must be 1000..15000)";
                return false;
            }
        } else if (GetOptionValue(arg, "--warmup-mb", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--warmup-mb", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.WarmupMb)) {
                err = "invalid value for --warmup-mb";
                return false;
            }
        } else if (GetOptionValue(arg, "--warmup-target", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--warmup-target", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.WarmupTarget = value;
        } else if (GetOptionValue(arg, "--warmup-path", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--warmup-path", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.WarmupPath = value;
        } else if (GetOptionValue(arg, "--download-test", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--download-test", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (speedTestSpecified) {
                err = "--download-test and --upload-test are mutually exclusive";
                return false;
            }
            if (!ParseUint32InRange(value, 1, 86400, cfg.SpeedTestDurationSec)) {
                err = "invalid value for --download-test (must be 1..86400)";
                return false;
            }
            cfg.SpeedTestMode = TqSpeedTestMode::Download;
            speedTestSpecified = true;
        } else if (GetOptionValue(arg, "--download-sink-test", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--download-sink-test", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (speedTestSpecified) {
                err = "speed-test options are mutually exclusive";
                return false;
            }
            if (!ParseUint32InRange(value, 1, 86400, cfg.SpeedTestDurationSec)) {
                err = "invalid value for --download-sink-test (must be 1..86400)";
                return false;
            }
            cfg.SpeedTestMode = TqSpeedTestMode::DownloadSink;
            speedTestSpecified = true;
        } else if (GetOptionValue(arg, "--upload-test", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--upload-test", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (speedTestSpecified) {
                err = "--download-test and --upload-test are mutually exclusive";
                return false;
            }
            if (!ParseUint32InRange(value, 1, 86400, cfg.SpeedTestDurationSec)) {
                err = "invalid value for --upload-test (must be 1..86400)";
                return false;
            }
            cfg.SpeedTestMode = TqSpeedTestMode::Upload;
            speedTestSpecified = true;
        } else if (GetOptionValue(arg, "--profile", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--profile", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (std::strcmp(value, "max-throughput") == 0) {
                cfg.QuicProfile = TqQuicProfile::MaxThroughput;
            } else if (std::strcmp(value, "low-latency") == 0) {
                cfg.QuicProfile = TqQuicProfile::LowLatency;
            } else {
                err = "invalid value for --profile (max-throughput|low-latency)";
                return false;
            }
        } else if (std::strcmp(arg, "--enable-encrypt") == 0) {
            cfg.QuicDisable1RttEncryption = false;
        } else if (GetOptionValue(arg, "--handshake-threads", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--handshake-threads", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.HandshakeThreads)) {
                err = "invalid value for --handshake-threads";
                return false;
            }
        } else if (GetOptionValue(arg, "--compress", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--compress", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.Compress = value;
            if (!IsValidCompress(cfg.Compress)) {
                err = "invalid compress";
                return false;
            }
        } else if (GetOptionValue(arg, "--compress-level", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--compress-level", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseInt(value, cfg.CompressLevel)) {
                err = "invalid value for --compress-level";
                return false;
            }
        } else if (GetOptionValue(arg, "--allow-targets", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--allow-targets", err);
                if (value == nullptr) {
                    return false;
                }
            }
            SplitCommaList(value, cfg.AllowTargets);
        } else if (GetOptionValue(arg, "--deny-targets", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--deny-targets", err);
                if (value == nullptr) {
                    return false;
                }
            }
            SplitCommaList(value, cfg.DenyTargets);
        } else if (GetOptionValue(arg, "--tuning", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--tuning", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.TuningMode = TqParseTuningMode(value);
            if (std::strcmp(value, "auto") != 0 && std::strcmp(value, "lan") != 0 &&
                std::strcmp(value, "wan") != 0 && std::strcmp(value, "custom") != 0) {
                err = "invalid value for --tuning (auto|lan|wan|custom)";
                return false;
            }
        } else if (GetOptionValue(arg, "--target-bandwidth-mbps", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--target-bandwidth-mbps", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TargetBandwidthMbps)) {
                err = "invalid value for --target-bandwidth-mbps";
                return false;
            }
        } else if (GetOptionValue(arg, "--target-rtt-ms", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--target-rtt-ms", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TargetRttMs)) {
                err = "invalid value for --target-rtt-ms";
                return false;
            }
        } else if (GetOptionValue(arg, "--max-memory-mb", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--max-memory-mb", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.MaxMemoryMb) || cfg.MaxMemoryMb == 0) {
                err = "invalid value for --max-memory-mb";
                return false;
            }
        } else if (GetOptionValue(arg, "--relay-io-size", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--relay-io-size", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideRelayIoSize)) {
                err = "invalid value for --relay-io-size";
                return false;
            }
        } else if (GetOptionValue(arg, "--relay-inflight-bytes", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--relay-inflight-bytes", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideRelayInflightBytes) ||
                cfg.TuningOverrideRelayInflightBytes == 0) {
                err = "invalid value for --relay-inflight-bytes";
                return false;
            }
        } else if (GetOptionValue(arg, "--initial-quic-read-ahead", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--initial-quic-read-ahead", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideInitialQuicReadAheadBytes) ||
                cfg.TuningOverrideInitialQuicReadAheadBytes == 0) {
                err = "invalid value for --initial-quic-read-ahead";
                return false;
            }
        } else if (GetOptionValue(arg, "--linux-relay-read-chunk-size", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--linux-relay-read-chunk-size", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideLinuxRelayReadChunkSize) ||
                cfg.TuningOverrideLinuxRelayReadChunkSize == 0) {
                err = "invalid value for --linux-relay-read-chunk-size";
                return false;
            }
        } else if (GetOptionValue(arg, "--linux-relay-tcp-write-max-bytes", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--linux-relay-tcp-write-max-bytes", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideLinuxRelayTcpWriteMaxBytes) ||
                cfg.TuningOverrideLinuxRelayTcpWriteMaxBytes == 0) {
                err = "invalid value for --linux-relay-tcp-write-max-bytes";
                return false;
            }
        } else if (GetOptionValue(arg, "--linux-relay-tcp-write-burst-bytes", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--linux-relay-tcp-write-burst-bytes", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideLinuxRelayTcpWriteBurstBytes) ||
                cfg.TuningOverrideLinuxRelayTcpWriteBurstBytes == 0) {
                err = "invalid value for --linux-relay-tcp-write-burst-bytes";
                return false;
            }
        } else if (GetOptionValue(arg, "--fcw", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--fcw", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideQuicFcw)) {
                err = "invalid value for --fcw";
                return false;
            }
        } else if (GetOptionValue(arg, "--srw", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--srw", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideQuicSrw)) {
                err = "invalid value for --srw";
                return false;
            }
        } else if (GetOptionValue(arg, "--iw", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--iw", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideQuicIw)) {
                err = "invalid value for --iw";
                return false;
            }
        } else if (GetOptionValue(arg, "--initrtt-ms", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--initrtt-ms", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideQuicInitRttMs)) {
                err = "invalid value for --initrtt-ms";
                return false;
            }
        } else if (std::strcmp(arg, "--trace") == 0) {
            cfg.Trace = true;
        } else if (GetOptionValue(arg, "--trace-interval", value)) {
            cfg.Trace = true;
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--trace-interval", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TraceIntervalSec)) {
                err = "invalid value for --trace-interval";
                return false;
            }
        } else if (std::strcmp(arg, "--trace-connect-on-start") == 0) {
            cfg.Trace = true;
            cfg.TraceConnectOnStart = true;
        } else {
            err = std::string("unknown option: ") + arg;
            return false;
        }
    }

    if (!cfg.ClientConfigPath.empty() && cfg.Mode != TqMode::Client) {
        err = "--client-config is valid only in client mode";
        return false;
    }
    if (!cfg.ClientConfigPath.empty() && (!cfg.QuicPeer.empty() || quicPeerSpecified)) {
        err = "--client-config and --peer are mutually exclusive";
        return false;
    }
    if (cfg.SpeedTestMode != TqSpeedTestMode::None) {
        if (cfg.Mode != TqMode::Client) {
            err = "speed-test options are valid only in client mode";
            return false;
        }
        if (!cfg.ClientConfigPath.empty()) {
            err = "speed-test options cannot be used with --client-config";
            return false;
        }
        if (cfg.WarmupMb > 0) {
            err = "speed-test options cannot be used with --warmup-mb";
            return false;
        }
    }
    if (!cfg.ClientConfigPath.empty()) {
        if (!TqLoadClientConfig(cfg.ClientConfigPath, cfg.Router, err)) {
            return false;
        }
    }
    if (!cfg.Router.Peers.empty()) {
        for (auto& peer : cfg.Router.Peers) {
            if (peer.QuicReconnectIntervalMs == 0) {
                peer.QuicReconnectIntervalMs = cfg.QuicReconnectIntervalMs;
            }
        }
        if (!TqValidateRouterConfig(cfg.Router, err)) {
            return false;
        }
    }

    if (cfg.Mode == TqMode::Client) {
        if (cfg.Router.Peers.empty() && !RequireNonEmpty(cfg.QuicPeer, "--peer", err)) {
            return false;
        }
        if (!RequireNonEmpty(cfg.QuicCert, "--cert", err)) {
            return false;
        }
        if (!RequireNonEmpty(cfg.QuicKey, "--key", err)) {
            return false;
        }
        if (!RequireNonEmpty(cfg.QuicCa, "--ca", err)) {
            return false;
        }
        if (cfg.WarmupMb > 0 && !RequireNonEmpty(cfg.WarmupTarget, "--warmup-target", err)) {
            return false;
        }
        if (cfg.SpeedTestMode != TqSpeedTestMode::None && !cfg.Router.Peers.empty()) {
            size_t enabledPeers = 0;
            for (const auto& peer : cfg.Router.Peers) {
                if (peer.Enabled) {
                    ++enabledPeers;
                }
            }
            if (enabledPeers != 1) {
                err = "speed-test options require exactly one enabled peer";
                return false;
            }
        }
    } else {
        if (!RequireNonEmpty(cfg.QuicListen, "--listen", err)) {
            return false;
        }
        if (!RequireNonEmpty(cfg.QuicCert, "--cert", err)) {
            return false;
        }
        if (!RequireNonEmpty(cfg.QuicKey, "--key", err)) {
            return false;
        }
        if (!RequireNonEmpty(cfg.QuicCa, "--ca", err)) {
            return false;
        }
        if (cfg.AllowTargets.empty()) {
            cfg.AllowTargets.push_back("0.0.0.0/0");
        }
        if (!TqValidateCidrList(cfg.AllowTargets, err)) {
            return false;
        }
        if (!cfg.DenyTargets.empty() && !TqValidateCidrList(cfg.DenyTargets, err)) {
            return false;
        }
    }

    return true;
}

bool TqLoadClientConfig(const std::string& path, TqRouterConfig& router, std::string& err) {
    std::ifstream file(path);
    if (!file) {
        err = "failed to open client config: " + path;
        return false;
    }
    std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    JsonParser parser(body, err);
    return parser.ParseRouter(router);
}

bool TqValidateRouterConfig(const TqRouterConfig& router, std::string& err) {
    if (router.Version != 1) {
        err = "client config version must be 1";
        return false;
    }
    if (!TqValidateProxyAuthUsers(router.ProxyAuth, err)) {
        return false;
    }
    std::set<std::string> peerIds;
    std::set<std::string> listens;
    for (const auto& peer : router.Peers) {
        if (peer.PeerId.empty()) { err = "peer_id is required"; return false; }
        if (!peerIds.insert(peer.PeerId).second) { err = "duplicate peer_id: " + peer.PeerId; return false; }
        if (!IsHostPort(peer.QuicPeer)) { err = "invalid quic_peer for " + peer.PeerId; return false; }
        if (peer.SocksListen.empty()) { err = "socks_listen is required for " + peer.PeerId; return false; }
        if (!IsHostPort(peer.SocksListen)) { err = "invalid socks_listen for " + peer.PeerId; return false; }
        if (!listens.insert(peer.SocksListen).second) { err = "duplicate listen: " + peer.SocksListen; return false; }
        if (!peer.HttpListen.empty() && !IsHostPort(peer.HttpListen)) { err = "invalid http_listen for " + peer.PeerId; return false; }
        if (!peer.HttpListen.empty() && !listens.insert(peer.HttpListen).second) { err = "duplicate listen: " + peer.HttpListen; return false; }
        if (peer.QuicConnections > 128) { err = "quic_connections out of range for " + peer.PeerId; return false; }
        if (peer.QuicReconnectIntervalMs != 0 &&
            (peer.QuicReconnectIntervalMs < 1000 || peer.QuicReconnectIntervalMs > 60000)) {
            err = "quic_reconnect_interval_ms out of range for " + peer.PeerId;
            return false;
        }
        if (!IsValidCompress(peer.Compress)) { err = "invalid compress for " + peer.PeerId; return false; }
    }
    return true;
}
