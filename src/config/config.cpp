#include "config.h"

#include "acl.h"
#include "tuning.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>

namespace {

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
            } else if (!SkipValue()) {
                return false;
            }
        } while (Consume(','));

        if (!Consume('}')) {
            return Error("malformed client config object");
        }
        return Finish(router);
    }

private:
    bool Finish(TqRouterConfig& router) {
        SkipWs();
        if (Pos != Text.size()) {
            return Error("unexpected trailing content in client config");
        }
        return TqValidateRouterConfig(router, Err);
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

} // namespace

void TqPrintUsage(FILE* out) {
    std::fprintf(out,
        "Usage: tcpquic-proxy client|server [options]\n"
        "\n"
        "Options:\n"
        "  --socks-listen <addr>      SOCKS5 listen address (default 127.0.0.1:1080)\n"
        "  --http-listen <addr>       HTTP CONNECT listen address (default 127.0.0.1:8080)\n"
        "  --client-config <path>     Router client config JSON (client)\n"
        "  --admin-listen <addr>      Router admin listen address\n"
        "  --quic-peer <addr>         QUIC peer address (client, required)\n"
        "  --quic-listen <addr>       QUIC listen address (server, required)\n"
        "  --quic-cert <path>         TLS certificate PEM (required)\n"
        "  --quic-key <path>          TLS private key PEM (required)\n"
        "  --quic-ca <path>           CA certificate PEM (required)\n"
        "  --quic-connections <n>     QUIC connection count (default 1)\n"
        "  --quic-reconnect-interval-ms <n> Client reconnect interval in ms (1000..60000, default 3000)\n"
        "  --warmup-mb <n>            Client startup download warmup per QUIC conn (default 0)\n"
        "  --warmup-target <host:port> Warmup HTTP target (required when --warmup-mb > 0)\n"
        "  --warmup-path <path>       Warmup HTTP GET path (default /)\n"
        "  --download-test <sec>       Client: built-in end-to-end download speed test\n"
        "  --upload-test <sec>         Client: built-in end-to-end upload speed test\n"
        "  --quic-profile <mode>        max-throughput|low-latency (default max-throughput)\n"
        "  --handshake-threads <n>    SOCKS/HTTP handshake workers (default 8, 0=auto)\n"
        "  --compress <mode>          auto|zstd|off (default auto)\n"
        "  --compress-level <n>       Compression level (default 1)\n"
        "  --tuning <mode>            auto|lan|wan|custom (default wan)\n"
        "  --target-bandwidth-mbps <n> Target bandwidth for auto/custom BDP\n"
        "  --target-rtt-ms <n>        Target RTT for auto/custom BDP\n"
        "  --max-memory-mb <n>        Cap relay pool memory across tunnels\n"
        "  --relay-io-size <bytes>    Override relay IO size (custom)\n"
        "  --relay-inflight-bytes <n> Override relay ideal in-flight bytes\n"
        "  --linux-relay-read-chunk-size <bytes> Override Linux relay TCP read chunk size\n"
        "  --linux-relay-worker-slots <n> Override Linux relay worker buffer slots per tunnel\n"
        "  --linux-relay-tcp-write-max-bytes <bytes> Cap each Linux relay TCP sendmsg\n"
        "  --linux-relay-tcp-write-burst-bytes <bytes> Cap bytes per Linux relay TCP write flush\n"
        "  --quic-fcw <bytes>         Override QUIC connection flow window\n"
        "  --quic-srw <bytes>         Override QUIC stream recv window\n"
        "  --quic-iw <packets>        Override QUIC initial window packets\n"
        "  --quic-initrtt-ms <n>      Override QUIC initial RTT\n"
        "  --allow-targets <list>     Allowed CIDR list, comma-separated (server, required)\n"
        "  --deny-targets <list>      Denied CIDR list, comma-separated\n"
        "  --trace                     Event + periodic debug trace (spdlog file log)\n"
        "  --trace-interval <sec>      Periodic stats interval when --trace (default 10)\n"
        "  --trace-connect-on-start    Client: connect QUIC at startup (debug)\n");
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

        if (GetOptionValue(arg, "--socks-listen", value)) {
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
        } else if (GetOptionValue(arg, "--quic-peer", value)) {
            quicPeerSpecified = true;
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--quic-peer", err);
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
        } else if (GetOptionValue(arg, "--quic-listen", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--quic-listen", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.QuicListen = value;
        } else if (GetOptionValue(arg, "--quic-cert", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--quic-cert", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.QuicCert = value;
        } else if (GetOptionValue(arg, "--quic-key", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--quic-key", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.QuicKey = value;
        } else if (GetOptionValue(arg, "--quic-ca", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--quic-ca", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.QuicCa = value;
        } else if (GetOptionValue(arg, "--quic-connections", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--quic-connections", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.QuicConnections)) {
                err = "invalid value for --quic-connections";
                return false;
            }
            if (cfg.QuicConnections > 128) {
                err = "--quic-connections must be <= 128";
                return false;
            }
            if (cfg.QuicConnections == 0) {
                cfg.QuicConnections = 1;
            }
        } else if (GetOptionValue(arg, "--quic-reconnect-interval-ms", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--quic-reconnect-interval-ms", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.QuicReconnectIntervalMs) ||
                cfg.QuicReconnectIntervalMs < 1000 ||
                cfg.QuicReconnectIntervalMs > 60000) {
                err = "invalid value for --quic-reconnect-interval-ms (must be 1000..60000)";
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
            if (cfg.SpeedTestMode != TqSpeedTestMode::None || speedTestSpecified) {
                err = "--download-test and --upload-test are mutually exclusive";
                return false;
            }
            if (!ParseUint32InRange(value, 1, 86400, cfg.SpeedTestDurationSec)) {
                err = "invalid value for --download-test (must be 1..86400)";
                return false;
            }
            cfg.SpeedTestMode = TqSpeedTestMode::Download;
            speedTestSpecified = true;
        } else if (GetOptionValue(arg, "--upload-test", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--upload-test", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (cfg.SpeedTestMode != TqSpeedTestMode::None || speedTestSpecified) {
                err = "--download-test and --upload-test are mutually exclusive";
                return false;
            }
            if (!ParseUint32InRange(value, 1, 86400, cfg.SpeedTestDurationSec)) {
                err = "invalid value for --upload-test (must be 1..86400)";
                return false;
            }
            cfg.SpeedTestMode = TqSpeedTestMode::Upload;
            speedTestSpecified = true;
        } else if (GetOptionValue(arg, "--quic-profile", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--quic-profile", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (std::strcmp(value, "max-throughput") == 0) {
                cfg.QuicProfile = TqQuicProfile::MaxThroughput;
            } else if (std::strcmp(value, "low-latency") == 0) {
                cfg.QuicProfile = TqQuicProfile::LowLatency;
            } else {
                err = "invalid value for --quic-profile (max-throughput|low-latency)";
                return false;
            }
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
            if (!ParseUint32(value, cfg.MaxMemoryMb)) {
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
            if (!ParseUint32(value, cfg.TuningOverrideRelayInflightBytes)) {
                err = "invalid value for --relay-inflight-bytes";
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
        } else if (GetOptionValue(arg, "--linux-relay-worker-slots", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--linux-relay-worker-slots", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideLinuxRelayWorkerSlots) ||
                cfg.TuningOverrideLinuxRelayWorkerSlots == 0) {
                err = "invalid value for --linux-relay-worker-slots";
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
        } else if (GetOptionValue(arg, "--quic-fcw", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--quic-fcw", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideQuicFcw)) {
                err = "invalid value for --quic-fcw";
                return false;
            }
        } else if (GetOptionValue(arg, "--quic-srw", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--quic-srw", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideQuicSrw)) {
                err = "invalid value for --quic-srw";
                return false;
            }
        } else if (GetOptionValue(arg, "--quic-iw", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--quic-iw", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideQuicIw)) {
                err = "invalid value for --quic-iw";
                return false;
            }
        } else if (GetOptionValue(arg, "--quic-initrtt-ms", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--quic-initrtt-ms", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideQuicInitRttMs)) {
                err = "invalid value for --quic-initrtt-ms";
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
    if (!cfg.ClientConfigPath.empty() && quicPeerSpecified) {
        err = "--client-config and --quic-peer are mutually exclusive";
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
        for (auto& peer : cfg.Router.Peers) {
            if (peer.QuicReconnectIntervalMs == 0) {
                peer.QuicReconnectIntervalMs = cfg.QuicReconnectIntervalMs;
            }
        }
    }

    if (cfg.Mode == TqMode::Client) {
        if (cfg.ClientConfigPath.empty() && !RequireNonEmpty(cfg.QuicPeer, "--quic-peer", err)) {
            return false;
        }
        if (!RequireNonEmpty(cfg.QuicCert, "--quic-cert", err)) {
            return false;
        }
        if (!RequireNonEmpty(cfg.QuicKey, "--quic-key", err)) {
            return false;
        }
        if (!RequireNonEmpty(cfg.QuicCa, "--quic-ca", err)) {
            return false;
        }
        if (cfg.WarmupMb > 0 && !RequireNonEmpty(cfg.WarmupTarget, "--warmup-target", err)) {
            return false;
        }
    } else {
        if (!RequireNonEmpty(cfg.QuicListen, "--quic-listen", err)) {
            return false;
        }
        if (!RequireNonEmpty(cfg.QuicCert, "--quic-cert", err)) {
            return false;
        }
        if (!RequireNonEmpty(cfg.QuicKey, "--quic-key", err)) {
            return false;
        }
        if (!RequireNonEmpty(cfg.QuicCa, "--quic-ca", err)) {
            return false;
        }
        if (cfg.AllowTargets.empty()) {
            err = "missing required option: --allow-targets";
            return false;
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
