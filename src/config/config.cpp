#include "config.h"

#include "acl.h"
#include "control_protocol.h"
#include "proxy_auth.h"
#include "tuning.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr uint32_t TqMaxQuicConnectionStreamCount = 65535;
constexpr uint32_t TqMinQuicKeepAliveIntervalMs = 1000;
constexpr uint32_t TqMaxQuicKeepAliveIntervalMs = 15000;
constexpr size_t kMaxPortForwardTargetHostLength = 255;

uint64_t CurrentPid() {
#if defined(_WIN32)
    return static_cast<uint64_t>(_getpid());
#else
    return static_cast<uint64_t>(::getpid());
#endif
}

std::string BaseNameFromArgv0(const char* argv0) {
    if (argv0 == nullptr || argv0[0] == '\0') {
        return "tcpquic-proxy";
    }
    std::string path(argv0);
    const size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
        path = path.substr(pos + 1);
    }
    return path.empty() ? "tcpquic-proxy" : path;
}

std::filesystem::path DefaultRuntimeBaseDir(const char* argv0) {
    const std::string runtimeName = BaseNameFromArgv0(argv0);
    std::filesystem::path base;
#if defined(_WIN32)
    char localAppData[MAX_PATH]{};
    const DWORD envLen = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
    base = envLen > 0 && envLen < MAX_PATH
        ? std::filesystem::path(localAppData) / runtimeName
        : std::filesystem::temp_directory_path() / runtimeName;
#else
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (runtimeDir != nullptr && runtimeDir[0] != '\0') {
        base = std::filesystem::path(runtimeDir) / runtimeName;
    } else {
        base = std::filesystem::temp_directory_path() /
            (runtimeName + "-" + std::to_string(static_cast<unsigned long>(::getuid())));
    }
#endif
    return base;
}

std::string DefaultRoleConfigPath(const char* argv0, TqMode mode) {
    const char* role = mode == TqMode::Client ? "client" : "server";
    return (DefaultRuntimeBaseDir(argv0) /
        (std::string(role) + "-config-" + std::to_string(CurrentPid()) + ".json")).string();
}

bool IsWhitespaceOnly(const std::string& value) {
    for (unsigned char ch : value) {
        if (!std::isspace(ch)) {
            return false;
        }
    }
    return true;
}

const char* DefaultClientNameOsPrefix() {
#if defined(_WIN32)
    return "win";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

bool IsClientNameChar(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '.' || ch == '_' || ch == '-' || ch == ':';
}

std::string ReadHostName() {
#if defined(_WIN32)
    char buf[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = static_cast<DWORD>(sizeof(buf));
    if (GetComputerNameA(buf, &size) && size > 0) {
        return std::string(buf, size);
    }
#else
    char buf[256]{};
    if (::gethostname(buf, sizeof(buf) - 1) == 0 && buf[0] != '\0') {
        buf[sizeof(buf) - 1] = '\0';
        return buf;
    }
#endif
    return "unknown";
}

std::string SanitizeClientNameHost(std::string host, size_t maxLen) {
    std::string out;
    out.reserve(host.size());
    for (unsigned char ch : host) {
        out.push_back(IsClientNameChar(ch) ? static_cast<char>(ch) : '-');
        if (out.size() == maxLen) {
            break;
        }
    }
    if (out.empty()) {
        out = "unknown";
    }
    if (out.size() > maxLen) {
        out.resize(maxLen);
    }
    return out;
}

std::string DefaultClientName() {
    const std::string prefix = DefaultClientNameOsPrefix();
    const size_t hostMaxLen = TQ_CLIENT_HELLO_MAX_NAME_LEN - prefix.size() - 1;
    return prefix + "-" + SanitizeClientNameHost(ReadHostName(), hostMaxLen);
}

void EnsureDefaultClientName(TqConfig& cfg) {
    if (cfg.Mode == TqMode::Client && cfg.ClientName.empty()) {
        cfg.ClientName = DefaultClientName();
    }
}

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

bool IsHelpOption(const char* arg) {
    return std::strcmp(arg, "-h") == 0 ||
           std::strcmp(arg, "--help") == 0 ||
           std::strcmp(arg, "--usage") == 0;
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
        const uint32_t digit = static_cast<uint32_t>(value[i] - '0');
        if (port > 6553 || (port == 6553 && digit > 5)) {
            return false;
        }
        port = port * 10 + digit;
    }
    return port != 0;
}

bool IsHostPortList(const std::string& value) {
    std::vector<std::string> endpoints;
    SplitCommaList(value, endpoints);
    if (endpoints.empty()) {
        return false;
    }
    for (const auto& endpoint : endpoints) {
        if (!IsHostPort(endpoint)) {
            return false;
        }
    }
    return true;
}

bool IsValidQuicPathLocalAddress(const std::string& value) {
    if (value.empty()) {
        return false;
    }

    uint32_t octet = 0;
    uint32_t octets = 0;
    bool haveDigit = false;
    for (char ch : value) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            const uint32_t digit = static_cast<uint32_t>(ch - '0');
            if (octet > 25 || (octet == 25 && digit > 5)) {
                return false;
            }
            octet = octet * 10 + digit;
            haveDigit = true;
        } else if (ch == '.') {
            if (!haveDigit || octets >= 3) {
                return false;
            }
            ++octets;
            octet = 0;
            haveDigit = false;
        } else {
            return false;
        }
    }
    return haveDigit && octets == 3;
}

bool SplitHostPortValue(const std::string& value, std::string& host, uint16_t& port) {
    host.clear();
    port = 0;

    size_t portStart = std::string::npos;
    if (!value.empty() && value[0] == '[') {
        const size_t close = value.find(']');
        if (close == std::string::npos || close == 1 || close + 2 > value.size() || value[close + 1] != ':') {
            return false;
        }
        host = value.substr(1, close - 1);
        portStart = close + 2;
    } else {
        const size_t colon = value.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= value.size() || value.find(':', colon + 1) != std::string::npos) {
            return false;
        }
        host = value.substr(0, colon);
        portStart = colon + 1;
    }

    uint32_t parsedPort = 0;
    for (size_t i = portStart; i < value.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(value[i]))) {
            host.clear();
            return false;
        }
        const uint32_t digit = static_cast<uint32_t>(value[i] - '0');
        if (parsedPort > 6553 || (parsedPort == 6553 && digit > 5)) {
            host.clear();
            return false;
        }
        parsedPort = parsedPort * 10 + digit;
    }
    if (host.empty() || host.size() > kMaxPortForwardTargetHostLength || parsedPort == 0) {
        host.clear();
        return false;
    }
    port = static_cast<uint16_t>(parsedPort);
    return true;
}

bool ParsePortForwardValue(const std::string& value, TqPortForwardConfig& out) {
    const size_t equals = value.find('=');
    if (equals == std::string::npos || equals == 0 || equals + 1 >= value.size()) {
        return false;
    }

    TqPortForwardConfig parsed;
    parsed.Listen = value.substr(0, equals);
    if (!IsHostPort(parsed.Listen)) {
        return false;
    }
    if (!SplitHostPortValue(value.substr(equals + 1), parsed.TargetHost, parsed.TargetPort)) {
        return false;
    }
    out = std::move(parsed);
    return true;
}

bool IsValidPortForwardTarget(const TqPortForwardConfig& forward) {
    return !forward.TargetHost.empty() &&
           forward.TargetHost.size() <= kMaxPortForwardTargetHostLength &&
           forward.TargetPort != 0;
}

bool IsValidCompress(const std::string& value) {
    return value.empty() || value == "auto" || value == "zstd" || value == "off";
}

class JsonParser {
public:
    JsonParser(const std::string& text, std::string& err) : Text(text), Err(err) {}

    bool ParseRouter(TqRouterConfig& router) {
        router = TqRouterConfig{};
        nlohmann::json root;
        if (!Load(root, "malformed client config object")) {
            return false;
        }
        if (!root.is_object()) {
            return Error("client config must be a JSON object");
        }
        for (const auto& item : root.items()) {
            const std::string& key = item.key();
            const auto& value = item.value();
            if (key == "version") {
                if (!ReadUint32(value, router.Version)) return Error("invalid version");
            } else if (key == "peers") {
                if (!ParsePeers(value, router.Peers, false)) return false;
            } else if (key == "proxy_auth") {
                if (!ParseProxyAuth(value, router.ProxyAuth)) return false;
            }
        }
        return TqValidateRouterConfig(router, Err);
    }

    bool ParseRuntimeConfig(TqConfig& cfg) {
        nlohmann::json root;
        if (!Load(root, "malformed config object")) {
            return false;
        }
        if (!root.is_object()) {
            return Error("config must be a JSON object");
        }
        if (root.contains("version")) {
            return Error("unknown config key: version");
        }
        bool speedTestSpecified = cfg.SpeedTestMode != TqSpeedTestMode::None;
        for (const auto& item : root.items()) {
            if (!ParseRuntimeConfigField(item.key(), item.value(), cfg, speedTestSpecified)) {
                return false;
            }
        }
        return true;
    }

private:
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
        if (value.is_number_unsigned()) {
            raw = value.get<uint64_t>();
        } else if (value.is_number_integer()) {
            const int64_t signedValue = value.get<int64_t>();
            if (signedValue < 0) return false;
            raw = static_cast<uint64_t>(signedValue);
        } else {
            return false;
        }
        if (raw > std::numeric_limits<uint32_t>::max()) return false;
        out = static_cast<uint32_t>(raw);
        return true;
    }

    bool ReadUint32InRange(const nlohmann::json& value, uint32_t minValue, uint32_t maxValue, uint32_t& out) {
        if (!ReadUint32(value, out)) return false;
        return out >= minValue && out <= maxValue;
    }

    bool ReadInt(const nlohmann::json& value, int& out) {
        if (!value.is_number_integer()) return false;
        const int64_t raw = value.get<int64_t>();
        if (raw < std::numeric_limits<int>::min() || raw > std::numeric_limits<int>::max()) return false;
        out = static_cast<int>(raw);
        return true;
    }

    bool RequireObject(const nlohmann::json& value, const char* err) {
        return value.is_object() || Error(err);
    }

    bool RequireArray(const nlohmann::json& value, const char* err) {
        return value.is_array() || Error(err);
    }

    bool ParseRuntimeConfigField(const std::string& key, const nlohmann::json& value, TqConfig& cfg, bool& speedTestSpecified) {
        if (key == "tls") return ParseTlsConfig(value, cfg);
        if (key == "admin") return ParseAdminConfig(value, cfg);
        if (key == "proto") return ParseProtoConfig(value, cfg);
        if (key == "server") return ParseServerConfig(value, cfg);
        if (key == "relay") return ParseRelayConfig(value, cfg);
        if (key == "tuning") return ParseTuningConfig(value, cfg);
        if (key == "compression") return ParseCompressionConfig(value, cfg);
        if (key == "trace") return ParseTraceConfig(value, cfg);
        if (key == "client") return ParseClientConfig(value, cfg, speedTestSpecified);
        if (key == "peers") return ParsePeers(value, cfg.Router.Peers, true);
        return Error("unknown config key: " + key);
    }

    bool ParseTlsConfig(const nlohmann::json& object, TqConfig& cfg) {
        if (!RequireObject(object, "tls must be an object")) return false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "cert") {
                if (!ReadString(item.value(), cfg.QuicCert)) return Error("invalid tls.cert");
            } else if (key == "key") {
                if (!ReadString(item.value(), cfg.QuicKey)) return Error("invalid tls.key");
            } else if (key == "ca") {
                if (!ReadString(item.value(), cfg.QuicCa)) return Error("invalid tls.ca");
            } else {
                return Error("unknown tls key: " + key);
            }
        }
        return true;
    }

    bool ParseAdminConfig(const nlohmann::json& object, TqConfig& cfg) {
        if (!RequireObject(object, "admin must be an object")) return false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "listen") {
                if (!ReadString(item.value(), cfg.AdminListen)) return Error("invalid admin.listen");
            } else if (key == "token_file") {
                if (!ReadString(item.value(), cfg.AdminTokenFile)) return Error("invalid admin.token_file");
            } else if (key == "threads") {
                if (!ReadUint32InRange(item.value(), 1, 32, cfg.AdminThreads)) return Error("invalid admin.threads");
            } else {
                return Error("unknown admin key: " + key);
            }
        }
        return true;
    }

    bool ParseProtoConfig(const nlohmann::json& object, TqConfig& cfg) {
        if (!RequireObject(object, "proto must be an object")) return false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "profile") {
                std::string profile;
                if (!ReadString(item.value(), profile)) return Error("invalid proto.profile");
                if (profile == "max-throughput") cfg.QuicProfile = TqQuicProfile::MaxThroughput;
                else if (profile == "low-latency") cfg.QuicProfile = TqQuicProfile::LowLatency;
                else return Error("invalid proto.profile");
            } else if (key == "encryption_policy") {
                std::string policy;
                if (!ReadString(item.value(), policy) || policy != "client-choice") return Error("invalid proto.encryption_policy");
                if (cfg.Mode != TqMode::Server) return Error("proto.encryption_policy is server-only");
            } else if (key == "disable_1rtt_encryption") {
                bool disable1RttEncryption = false;
                if (!ReadBool(item.value(), disable1RttEncryption)) return Error("invalid proto.disable_1rtt_encryption");
                if (cfg.Mode != TqMode::Server) {
                    cfg.QuicDisable1RttEncryption = disable1RttEncryption;
                }
            } else if (key == "connections") {
                if (!ReadUint32(item.value(), cfg.QuicConnections) || cfg.QuicConnections > 128) return Error("invalid proto.connections");
                if (cfg.QuicConnections == 0) cfg.QuicConnections = 1;
            } else if (key == "connection_stream_count") {
                if (!ReadUint32(item.value(), cfg.QuicConnectionStreamCount) || cfg.QuicConnectionStreamCount == 0 || cfg.QuicConnectionStreamCount > TqMaxQuicConnectionStreamCount) {
                    return Error("invalid proto.connection_stream_count");
                }
            } else if (key == "keepalive_ms") {
                if (!ReadUint32InRange(item.value(), TqMinQuicKeepAliveIntervalMs, TqMaxQuicKeepAliveIntervalMs, cfg.QuicKeepAliveIntervalMs)) return Error("invalid proto.keepalive_ms");
            } else if (key == "iw") {
                if (!ReadUint32(item.value(), cfg.TuningOverrideQuicIw)) return Error("invalid proto.iw");
            } else if (key == "initrtt_ms") {
                if (!ReadUint32(item.value(), cfg.TuningOverrideQuicInitRttMs)) return Error("invalid proto.initrtt_ms");
            } else {
                return Error("unknown proto key: " + key);
            }
        }
        return true;
    }

    bool ParseServerConfig(const nlohmann::json& object, TqConfig& cfg) {
        if (!RequireObject(object, "server must be an object")) return false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "proto_listen") {
                if (!ReadString(item.value(), cfg.QuicListen)) return Error("invalid server.proto_listen");
            } else if (key == "allow_targets") {
                if (!ParseStringList(item.value(), cfg.AllowTargets)) return Error("invalid server.allow_targets");
            } else if (key == "deny_targets") {
                if (!ParseStringList(item.value(), cfg.DenyTargets)) return Error("invalid server.deny_targets");
            } else {
                return Error("unknown server key: " + key);
            }
        }
        return true;
    }

    bool ParseRelayConfig(const nlohmann::json& object, TqConfig& cfg) {
        if (!RequireObject(object, "relay must be an object")) return false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "io_size") {
                if (!ReadUint32(item.value(), cfg.TuningOverrideRelayIoSize)) return Error("invalid relay.io_size");
            } else if (key == "common") {
                if (!ParseCommonRelayConfig(item.value(), cfg)) return false;
            } else if (key == "linux") {
                if (!ParseLinuxRelayConfig(item.value(), cfg)) return false;
            } else if (key == "windows") {
                if (!ParseWindowsRelayConfig(item.value(), cfg)) return false;
            } else {
                return Error("unknown relay key: " + key);
            }
        }
        return true;
    }

    bool ParseCommonRelayConfig(const nlohmann::json& object, TqConfig& cfg) {
        if (!RequireObject(object, "relay.common must be an object")) return false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "read_chunk_size") {
                if (!ReadNonZeroUint32(item.value(), cfg.TuningOverrideRelayReadChunkSize)) return Error("invalid relay.common.read_chunk_size");
            } else if (key == "tcp_write_max_bytes") {
                if (!ReadNonZeroUint32(item.value(), cfg.TuningOverrideRelayTcpWriteMaxBytes)) return Error("invalid relay.common.tcp_write_max_bytes");
            } else if (key == "tcp_write_burst_bytes") {
                if (!ReadNonZeroUint32(item.value(), cfg.TuningOverrideRelayTcpWriteBurstBytes)) return Error("invalid relay.common.tcp_write_burst_bytes");
            } else if (key == "event_queue_capacity") {
                if (!ReadUint32InRange(
                        item.value(),
                        TqLinuxRelayEventQueueCapacityMin,
                        TqLinuxRelayEventQueueCapacityMax,
                        cfg.TuningOverrideRelayEventQueueCapacity)) {
                    return Error("invalid relay.common.event_queue_capacity");
                }
            } else if (key == "worker_count") {
                if (!ReadUint32InRange(
                        item.value(),
                        TqRelayWorkerCountMin,
                        TqRelayWorkerCountMax,
                        cfg.TuningOverrideRelayWorkerCount)) {
                    return Error("invalid relay.common.worker_count");
                }
            } else {
                return Error("unknown relay.common key: " + key);
            }
        }
        return true;
    }

    bool ParseLinuxRelayConfig(const nlohmann::json& object, TqConfig& cfg) {
        if (!RequireObject(object, "relay.linux must be an object")) return false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "read_chunk_size") {
                if (!ReadNonZeroUint32(item.value(), cfg.TuningOverrideLinuxRelayReadChunkSize)) return Error("invalid relay.linux.read_chunk_size");
            } else if (key == "worker_slots") {
                uint32_t ignored = 0;
                if (!ReadNonZeroUint32(item.value(), ignored)) return Error("invalid relay.linux.worker_slots");
                spdlog::warn("relay.linux.worker_slots is deprecated and ignored");
            } else if (key == "tcp_write_max_bytes") {
                if (!ReadNonZeroUint32(item.value(), cfg.TuningOverrideLinuxRelayTcpWriteMaxBytes)) return Error("invalid relay.linux.tcp_write_max_bytes");
            } else if (key == "tcp_write_burst_bytes") {
                if (!ReadNonZeroUint32(item.value(), cfg.TuningOverrideLinuxRelayTcpWriteBurstBytes)) return Error("invalid relay.linux.tcp_write_burst_bytes");
            } else if (key == "event_queue_capacity") {
                if (!ReadUint32InRange(
                        item.value(),
                        TqLinuxRelayEventQueueCapacityMin,
                        TqLinuxRelayEventQueueCapacityMax,
                        cfg.TuningOverrideLinuxRelayEventQueueCapacity)) {
                    return Error("invalid relay.linux.event_queue_capacity");
                }
            } else if (key == "worker_count") {
                if (!ReadNonZeroUint32(item.value(), cfg.TuningOverrideLinuxRelayWorkerCount)) {
                    return Error("invalid relay.linux.worker_count");
                }
            } else {
                return Error("unknown relay.linux key: " + key);
            }
        }
        return true;
    }

    bool ParseWindowsRelayConfig(const nlohmann::json& object, TqConfig& cfg) {
        if (!RequireObject(object, "relay.windows must be an object")) return false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "worker_count") {
                if (!ReadNonZeroUint32(item.value(), cfg.TuningOverrideWindowsRelayWorkerCount)) {
                    return Error("invalid relay.windows.worker_count");
                }
            } else {
                return Error("unknown relay.windows key: " + key);
            }
        }
        return true;
    }

    bool ParseTuningConfig(const nlohmann::json& object, TqConfig& cfg) {
        if (!RequireObject(object, "tuning must be an object")) return false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "mode") {
                std::string value;
                if (!ReadString(item.value(), value)) return Error("invalid tuning.mode");
                cfg.TuningMode = TqParseTuningMode(value.c_str());
                if (value != "auto" && value != "lan" && value != "wan") return Error("invalid tuning.mode");
            } else {
                return Error("unknown tuning key: " + key);
            }
        }
        return true;
    }

    bool ParseCompressionConfig(const nlohmann::json& object, TqConfig& cfg) {
        if (!RequireObject(object, "compression must be an object")) return false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "mode") {
                if (!ReadString(item.value(), cfg.Compress) || !IsValidCompress(cfg.Compress)) return Error("invalid compression.mode");
            } else if (key == "level") {
                if (!ReadInt(item.value(), cfg.CompressLevel)) return Error("invalid compression.level");
            } else {
                return Error("unknown compression key: " + key);
            }
        }
        return true;
    }

    bool ParseTraceConfig(const nlohmann::json& object, TqConfig& cfg) {
        if (!RequireObject(object, "trace must be an object")) return false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "enabled") {
                if (!ReadBool(item.value(), cfg.Trace)) return Error("invalid trace.enabled");
            } else if (key == "interval_sec") {
                if (!ReadUint32(item.value(), cfg.TraceIntervalSec)) return Error("invalid trace.interval_sec");
                cfg.Trace = true;
            } else if (key == "level") {
                std::string level;
                if (!ReadString(item.value(), level)) return Error("invalid trace.level");
                if (level == "info") cfg.TraceLogLevel = TqConfig::TraceLevel::Info;
                else if (level == "debug") cfg.TraceLogLevel = TqConfig::TraceLevel::Debug;
                else return Error("invalid trace.level (expected info or debug)");
            } else {
                return Error("unknown trace key: " + key);
            }
        }
        return true;
    }

    bool ParseClientConfig(const nlohmann::json& object, TqConfig& cfg, bool& speedTestSpecified) {
        if (!RequireObject(object, "client must be an object")) return false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "download_test") {
                if (!ParseSpeedTest(item.value(), TqSpeedTestMode::Download, cfg, speedTestSpecified, "client.download_test")) return false;
            } else if (key == "download_sink_test") {
                if (!ParseSpeedTest(item.value(), TqSpeedTestMode::DownloadSink, cfg, speedTestSpecified, "client.download_sink_test")) return false;
            } else if (key == "upload_test") {
                if (!ParseSpeedTest(item.value(), TqSpeedTestMode::Upload, cfg, speedTestSpecified, "client.upload_test")) return false;
            } else if (key == "handshake_threads") {
                if (!ReadUint32(item.value(), cfg.HandshakeThreads)) return Error("invalid client.handshake_threads");
            } else if (key == "client_name") {
                if (!ReadString(item.value(), cfg.ClientName) ||
                    (!cfg.ClientName.empty() && !TqIsValidClientName(cfg.ClientName))) {
                    return Error("invalid client.client_name");
                }
            } else {
                return Error("unknown client key: " + key);
            }
        }
        return true;
    }

    bool ParseSpeedTest(const nlohmann::json& value, TqSpeedTestMode mode, TqConfig& cfg, bool& speedTestSpecified, const char* key) {
        if (speedTestSpecified) return Error("speed-test options are mutually exclusive");
        if (!ReadUint32InRange(value, 1, 86400, cfg.SpeedTestDurationSec)) return Error(std::string("invalid ") + key);
        cfg.SpeedTestMode = mode;
        speedTestSpecified = true;
        return true;
    }

    bool ReadNonZeroUint32(const nlohmann::json& value, uint32_t& out) {
        return ReadUint32(value, out) && out != 0;
    }

    bool ParsePeers(const nlohmann::json& array, std::vector<TqPeerConfig>& peers, bool runtimeShape) {
        peers.clear();
        if (!RequireArray(array, "peers must be an array")) return false;
        for (const auto& value : array) {
            TqPeerConfig peer;
            if (runtimeShape) {
                if (!ParseRuntimePeer(value, peer)) return false;
            } else if (!ParsePeer(value, peer)) {
                return false;
            }
            peers.push_back(std::move(peer));
        }
        return true;
    }

    bool ParseRuntimePeer(const nlohmann::json& object, TqPeerConfig& peer) {
        if (!RequireObject(object, "peer must be an object")) return false;
        bool protoConnectionsSpecified = false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "id") {
                if (!ReadString(item.value(), peer.PeerId)) return Error("invalid peer.id");
            } else if (key == "client_name") {
                return Error("unknown peer key: client_name");
            } else if (key == "proto_peer") {
                if (!ReadString(item.value(), peer.QuicPeer)) return Error("invalid peer.proto_peer");
            } else if (key == "socks_listen") {
                if (!ReadString(item.value(), peer.SocksListen)) return Error("invalid peer.socks_listen");
            } else if (key == "http_listen") {
                if (!ReadString(item.value(), peer.HttpListen)) return Error("invalid peer.http_listen");
            } else if (key == "port_forwards") {
                if (!ParsePortForwards(item.value(), peer.PortForwards)) return false;
            } else if (key == "paths") {
                if (!ParseQuicPaths(item.value(), peer.QuicPaths)) return false;
            } else if (key == "proto_connections") {
                protoConnectionsSpecified = true;
                if (!ReadUint32(item.value(), peer.QuicConnections)) return Error("invalid peer.proto_connections");
            } else if (key == "compress") {
                if (!ReadString(item.value(), peer.Compress)) return Error("invalid peer.compress");
            } else if (key == "enabled") {
                if (!ReadBool(item.value(), peer.Enabled)) return Error("invalid peer.enabled");
            } else {
                return Error("unknown peer key: " + key);
            }
        }
        if (protoConnectionsSpecified && peer.QuicConnections == 0) return Error("peer.proto_connections out of range");
        return true;
    }

    bool ParsePeer(const nlohmann::json& object, TqPeerConfig& peer) {
        if (!RequireObject(object, "peer must be an object")) return false;
        bool quicConnectionsSpecified = false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "peer_id") {
                if (!ReadString(item.value(), peer.PeerId)) return Error("invalid peer_id");
            } else if (key == "client_name") {
                return Error("unknown peer key: client_name");
            } else if (key == "quic_peer") {
                if (!ReadString(item.value(), peer.QuicPeer)) return Error("invalid quic_peer");
            } else if (key == "socks_listen") {
                if (!ReadString(item.value(), peer.SocksListen)) return Error("invalid socks_listen");
            } else if (key == "http_listen") {
                if (!ReadString(item.value(), peer.HttpListen)) return Error("invalid http_listen");
            } else if (key == "port_forwards") {
                if (!ParsePortForwards(item.value(), peer.PortForwards)) return false;
            } else if (key == "paths") {
                if (!ParseQuicPaths(item.value(), peer.QuicPaths)) return false;
            } else if (key == "quic_connections") {
                quicConnectionsSpecified = true;
                if (!ReadUint32(item.value(), peer.QuicConnections)) return Error("invalid quic_connections");
            } else if (key == "quic_reconnect_interval_ms") {
                return Error("unknown peer key: quic_reconnect_interval_ms");
            } else if (key == "compress") {
                if (!ReadString(item.value(), peer.Compress)) return Error("invalid compress");
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
                if (!ReadString(item.value(), target) || !SplitHostPortValue(target, forward.TargetHost, forward.TargetPort)) return Error("invalid port_forward.target");
            } else {
                return Error("unknown port_forward key: " + key);
            }
        }
        if (!hasListen || !hasTarget) return Error("port_forward listen and target are required");
        return true;
    }

    bool ParseProxyAuth(const nlohmann::json& array, std::vector<TqProxyAuthUser>& users) {
        users.clear();
        if (!RequireArray(array, "proxy_auth must be an array")) return false;
        for (const auto& value : array) {
            TqProxyAuthUser user;
            if (!ParseProxyAuthUser(value, user)) return false;
            users.push_back(std::move(user));
        }
        return true;
    }

    bool ParseProxyAuthUser(const nlohmann::json& object, TqProxyAuthUser& user) {
        if (!RequireObject(object, "proxy_auth entry must be an object")) return false;
        for (const auto& item : object.items()) {
            const std::string& key = item.key();
            if (key == "username") {
                if (!ReadString(item.value(), user.Username)) return Error("invalid proxy_auth.username");
            } else if (key == "password") {
                if (!ReadString(item.value(), user.Password)) return Error("invalid proxy_auth.password");
            } else {
                return Error("unknown proxy_auth key: " + key);
            }
        }
        return true;
    }

    bool ParseStringList(const nlohmann::json& value, std::vector<std::string>& out) {
        out.clear();
        if (value.is_string()) {
            SplitCommaList(value.get<std::string>(), out);
            return true;
        }
        if (!value.is_array()) return false;
        for (const auto& item : value) {
            if (!item.is_string()) return false;
            out.push_back(item.get<std::string>());
        }
        return true;
    }

    bool Error(const char* message) {
        Err = message;
        return false;
    }

    bool Error(const std::string& message) {
        Err = message;
        return false;
    }

    const std::string& Text;
    std::string& Err;
};

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

const char* TraceLevelName(TqConfig::TraceLevel level) {
    switch (level) {
    case TqConfig::TraceLevel::Info: return "info";
    case TqConfig::TraceLevel::Debug: return "debug";
    }
    return "info";
}

nlohmann::json StringArrayJson(const std::vector<std::string>& values) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& value : values) {
        out.push_back(value);
    }
    return out;
}

std::string PortForwardTargetText(const TqPortForwardConfig& forward) {
    const bool ipv6 = forward.TargetHost.find(':') != std::string::npos &&
        !(forward.TargetHost.size() >= 2 && forward.TargetHost.front() == '[' && forward.TargetHost.back() == ']');
    std::string host = ipv6 ? "[" + forward.TargetHost + "]" : forward.TargetHost;
    return host + ":" + std::to_string(forward.TargetPort);
}

nlohmann::json PortForwardsJson(const std::vector<TqPortForwardConfig>& forwards) {
    nlohmann::json values = nlohmann::json::array();
    for (const auto& forward : forwards) {
        values.push_back({
            {"listen", forward.Listen},
            {"target", PortForwardTargetText(forward)},
        });
    }
    return values;
}

nlohmann::json QuicPathsJson(const std::vector<TqQuicPathConfig>& paths) {
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

nlohmann::json RuntimePeerJson(const TqPeerConfig& peer) {
    nlohmann::json value = {
        {"id", peer.PeerId},
        {"proto_peer", peer.QuicPeer},
        {"socks_listen", peer.SocksListen},
        {"http_listen", peer.HttpListen},
        {"port_forwards", PortForwardsJson(peer.PortForwards)},
        {"paths", QuicPathsJson(peer.QuicPaths)},
        {"compress", peer.Compress},
        {"enabled", peer.Enabled},
    };
    if (peer.QuicConnections != 0) {
        value["proto_connections"] = peer.QuicConnections;
    }
    return value;
}

TqPeerConfig PrimaryPeerFromConfig(const TqConfig& cfg) {
    TqPeerConfig peer;
    peer.PeerId = "primary";
    peer.Enabled = true;
    peer.QuicPeer = cfg.QuicPeer;
    peer.QuicPaths = cfg.QuicPaths;
    peer.SocksListen = cfg.SocksListen;
    peer.HttpListen = cfg.HttpListen;
    peer.PortForwards = cfg.PortForwards;
    peer.QuicConnections = cfg.QuicConnections;
    peer.Compress = cfg.Compress;
    return peer;
}

nlohmann::json RuntimePeersJson(const TqConfig& cfg) {
    nlohmann::json values = nlohmann::json::array();
    const std::vector<TqPeerConfig>* peers = &cfg.Router.Peers;
    std::vector<TqPeerConfig> singlePeer;
    if (peers->empty() && (!cfg.QuicPeer.empty() || !cfg.QuicPaths.empty())) {
        singlePeer.push_back(PrimaryPeerFromConfig(cfg));
        peers = &singlePeer;
    }
    for (const auto& peer : *peers) {
        values.push_back(RuntimePeerJson(peer));
    }
    return values;
}

std::string RuntimeConfigJson(const TqConfig& cfg) {
    nlohmann::json root;
    root["tls"] = nlohmann::json::object();
    if (!cfg.QuicCert.empty()) root["tls"]["cert"] = cfg.QuicCert;
    if (!cfg.QuicKey.empty()) root["tls"]["key"] = cfg.QuicKey;
    if (!cfg.QuicCa.empty()) root["tls"]["ca"] = cfg.QuicCa;

    if (!cfg.AdminListen.empty() || !cfg.AdminTokenFile.empty() || cfg.AdminThreads != 2) {
        root["admin"] = nlohmann::json::object();
        if (!cfg.AdminListen.empty()) root["admin"]["listen"] = cfg.AdminListen;
        if (!cfg.AdminTokenFile.empty()) root["admin"]["token_file"] = cfg.AdminTokenFile;
        root["admin"]["threads"] = cfg.AdminThreads;
    }

    nlohmann::json proto{
        {"profile", QuicProfileName(cfg.QuicProfile)},
        {"connection_stream_count", cfg.QuicConnectionStreamCount},
        {"keepalive_ms", cfg.QuicKeepAliveIntervalMs},
    };
    if (cfg.Mode == TqMode::Client) {
        proto["disable_1rtt_encryption"] = cfg.QuicDisable1RttEncryption;
        proto["connections"] = cfg.QuicConnections;
    } else {
        proto["encryption_policy"] = "client-choice";
    }
    if (cfg.TuningOverrideQuicIw != 0) proto["iw"] = cfg.TuningOverrideQuicIw;
    if (cfg.TuningOverrideQuicInitRttMs != 0) proto["initrtt_ms"] = cfg.TuningOverrideQuicInitRttMs;
    root["proto"] = std::move(proto);

    root["compression"] = {
        {"mode", cfg.Compress},
        {"level", cfg.CompressLevel},
    };
    root["tuning"] = {{"mode", TuningModeName(cfg.TuningMode)}};

    nlohmann::json relay = nlohmann::json::object();
    if (cfg.TuningOverrideRelayIoSize != 0) relay["io_size"] = cfg.TuningOverrideRelayIoSize;
    nlohmann::json commonRelay = nlohmann::json::object();
    if (cfg.TuningOverrideRelayReadChunkSize != 0) commonRelay["read_chunk_size"] = cfg.TuningOverrideRelayReadChunkSize;
    if (cfg.TuningOverrideRelayTcpWriteMaxBytes != 0) commonRelay["tcp_write_max_bytes"] = cfg.TuningOverrideRelayTcpWriteMaxBytes;
    if (cfg.TuningOverrideRelayTcpWriteBurstBytes != 0) commonRelay["tcp_write_burst_bytes"] = cfg.TuningOverrideRelayTcpWriteBurstBytes;
    if (cfg.TuningOverrideRelayEventQueueCapacity != 0) commonRelay["event_queue_capacity"] = cfg.TuningOverrideRelayEventQueueCapacity;
    if (cfg.TuningOverrideRelayWorkerCount != 0) commonRelay["worker_count"] = cfg.TuningOverrideRelayWorkerCount;
    if (!commonRelay.empty()) relay["common"] = std::move(commonRelay);
    nlohmann::json linuxRelay = nlohmann::json::object();
    if (cfg.TuningOverrideLinuxRelayReadChunkSize != 0) linuxRelay["read_chunk_size"] = cfg.TuningOverrideLinuxRelayReadChunkSize;
    if (cfg.TuningOverrideLinuxRelayTcpWriteMaxBytes != 0) linuxRelay["tcp_write_max_bytes"] = cfg.TuningOverrideLinuxRelayTcpWriteMaxBytes;
    if (cfg.TuningOverrideLinuxRelayTcpWriteBurstBytes != 0) linuxRelay["tcp_write_burst_bytes"] = cfg.TuningOverrideLinuxRelayTcpWriteBurstBytes;
    if (cfg.TuningOverrideLinuxRelayEventQueueCapacity != 0) linuxRelay["event_queue_capacity"] = cfg.TuningOverrideLinuxRelayEventQueueCapacity;
    if (cfg.TuningOverrideLinuxRelayWorkerCount != 0) linuxRelay["worker_count"] = cfg.TuningOverrideLinuxRelayWorkerCount;
    if (!linuxRelay.empty()) relay["linux"] = std::move(linuxRelay);
    nlohmann::json windowsRelay = nlohmann::json::object();
    if (cfg.TuningOverrideWindowsRelayWorkerCount != 0) windowsRelay["worker_count"] = cfg.TuningOverrideWindowsRelayWorkerCount;
    if (!windowsRelay.empty()) relay["windows"] = std::move(windowsRelay);
    if (!relay.empty()) root["relay"] = std::move(relay);

    root["trace"] = {
        {"enabled", cfg.Trace},
        {"interval_sec", cfg.TraceIntervalSec},
        {"level", TraceLevelName(cfg.TraceLogLevel)},
    };

    if (cfg.Mode == TqMode::Client) {
        root["client"] = {
            {"client_name", cfg.ClientName},
        };
        nlohmann::json peers = RuntimePeersJson(cfg);
        if (!peers.empty()) {
            root["peers"] = std::move(peers);
        }
    } else {
        root["server"] = {
            {"proto_listen", cfg.QuicListen},
            {"allow_targets", StringArrayJson(cfg.AllowTargets)},
            {"deny_targets", StringArrayJson(cfg.DenyTargets)},
        };
    }

    return root.dump(2) + "\n";
}

bool WriteTextFileAtomically(const std::string& path, const std::string& body, std::string& err) {
    std::error_code ec;
    const std::filesystem::path target(path);
    const std::filesystem::path parent = target.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            err = "failed to create config directory: " + ec.message();
            return false;
        }
    }

    const std::filesystem::path tmp = target.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            err = "failed to open config temp file: " + tmp.string();
            return false;
        }
        out << body;
        out.close();
        if (!out) {
            err = "failed to write config temp file: " + tmp.string();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }

    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        err = "failed to publish config file: " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool LoadRuntimeConfigFile(const std::string& path, TqConfig& cfg, std::string& err, bool* legacyRouterConfig) {
    if (legacyRouterConfig != nullptr) {
        *legacyRouterConfig = false;
    }
    std::ifstream file(path);
    if (!file) {
        err = "failed to open config: " + path;
        return false;
    }
    std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    JsonParser parser(body, err);
    if (parser.ParseRuntimeConfig(cfg)) {
        return true;
    }
    const std::string runtimeErr = err;
    if (cfg.Mode == TqMode::Client && runtimeErr == "unknown config key: version") {
        TqRouterConfig router;
        std::string routerErr;
        JsonParser routerParser(body, routerErr);
        if (routerParser.ParseRouter(router)) {
            cfg.Router = std::move(router);
            if (legacyRouterConfig != nullptr) {
                *legacyRouterConfig = true;
            }
            err.clear();
            return true;
        }
    }
    err = runtimeErr;
    return false;
}

} // namespace

void TqPrintUsage(FILE* out) {
    std::fprintf(out,
        "Usage: tcpquic-proxy client|server [options]\n"
        "\n"
        "Client and Server:\n"
        "  -h, --help, --usage         Show this help and exit\n"
        "  --config <path>              Runtime JSON config file\n"
        "                              (preferred; see docs/config_guide_cn.md)\n"
        "  --cert <path>                TLS certificate PEM path\n"
        "  --key <path>                 TLS private key PEM path\n"
        "  --ca <path>                  CA certificate PEM path\n"
        "                              Client TLS: --ca is required; --cert/--key are ignored\n"
        "                              Server TLS: --cert and --key are required; --ca is optional\n"
        "  --admin-listen <addr>        Admin HTTP listen address for /health and /metrics\n"
        "  --admin-token-file <path>    Admin bearer token JSON file path\n"
        "  --admin-threads <n>          Admin HTTP worker threads (default 2, 1..32)\n"
        "  --compress <mode>            auto|zstd|off (default off)\n"
        "  --compress-level <n>         Compression level (default 1)\n"
        "\n"
        "Client specific:\n"
        "  --socks-listen <addr>        SOCKS5 listen address (default 127.0.0.1:1080)\n"
        "  --http-listen <addr>         HTTP CONNECT listen address (default 127.0.0.1:8080)\n"
        "  --forward <local=target>    Local port forward, repeatable\n"
        "  --client-config <path>       Legacy router client config JSON\n"
        "  --peer <addr>                Legacy single-peer address\n"
        "  --client-name <name>         Display name sent to server console\n"
        "  --connections <n>            Connection count (default 1)\n"
        "  --connection-stream-count <n>\n"
        "                              Max bidirectional streams per connection (default 1024)\n"
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
        "  --iw <packets>               Override initial window packets\n"
        "  --initrtt-ms <n>             Override initial RTT\n"
        "  --tuning <mode>              auto|lan|wan (default wan)\n"
        "  --max-memory-mb <n>          Cap relay pool memory across tunnels\n"
        "  --relay-io-size <bytes>      Override relay IO size\n"
        "  --relay-read-chunk-size <bytes>\n"
        "                              Shared Linux/Darwin relay TCP read chunk size\n"
        "  --relay-tcp-write-max-bytes <bytes>\n"
        "                              Cap each shared relay TCP sendmsg\n"
        "  --relay-tcp-write-burst-bytes <bytes>\n"
        "                              Cap bytes per shared relay TCP write flush\n"
        "  --relay-event-queue-capacity <events>\n"
        "                              Shared relay event queue capacity (default 4096, 1024..1048576)\n"
        "  --relay-worker-count <n>\n"
        "                              Shared Linux/Darwin relay worker count (default auto-detect, 1..8)\n"
        "  --linux-relay-read-chunk-size <bytes>\n"
        "                              Legacy alias for --relay-read-chunk-size\n"
        "  --linux-relay-tcp-write-max-bytes <bytes>\n"
        "                              Legacy alias for --relay-tcp-write-max-bytes\n"
        "  --linux-relay-tcp-write-burst-bytes <bytes>\n"
        "                              Legacy alias for --relay-tcp-write-burst-bytes\n"
        "  --linux-relay-event-queue-capacity <events>\n"
        "                              Legacy alias for --relay-event-queue-capacity\n"
        "  --linux-relay-worker-count <n>\n"
        "                              Legacy alias for --relay-worker-count\n"
        "  --windows-relay-worker-count <n>\n"
        "                              Windows relay worker count (default auto-detect, 1..8)\n"
        "\n"
        "Diagnostics:\n"
        "  --trace                      Event + periodic trace (enabled by default)\n"
        "  --trace-interval <sec>       Periodic stats interval for trace log (default 30)\n"
        "  --trace-level <info|debug>   Trace file log level (default info)\n"
        "  --diag-stats                 Low-overhead periodic stderr stats\n"
        "  --diag-stats-interval <sec>  Periodic stderr stats interval (default 5)\n");
}

void TqFinalizeConfig(TqConfig& cfg) {
    EnsureDefaultClientName(cfg);
    TqComputeTuning(cfg, cfg.Tuning);
}

bool TqParseArgs(int argc, char** argv, TqConfig& cfg, std::string& err) {
    cfg = TqConfig{};

    for (int i = 1; i < argc; ++i) {
        if (IsHelpOption(argv[i])) {
            cfg.ShowUsage = true;
            err.clear();
            return true;
        }
    }

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

    bool configSpecified = false;
    bool configFileMissing = false;
    bool legacyRouterConfigLoadedFromConfig = false;
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
        configSpecified = true;
        cfg.ConfigPath = value;
        if (!std::filesystem::exists(cfg.ConfigPath)) {
            configFileMissing = true;
        } else if (!LoadRuntimeConfigFile(cfg.ConfigPath, cfg, err, &legacyRouterConfigLoadedFromConfig)) {
            return false;
        }
    }

    bool quicPeerSpecified = false;
    bool clientConfigSpecified = false;
    bool socksListenSpecified = false;
    bool httpListenSpecified = false;
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
            socksListenSpecified = true;
        } else if (GetOptionValue(arg, "--http-listen", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--http-listen", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.HttpListen = value;
            httpListenSpecified = true;
        } else if (GetOptionValue(arg, "--peer", value)) {
            quicPeerSpecified = true;
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--peer", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.QuicPeer = value;
        } else if (GetOptionValue(arg, "--client-name", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--client-name", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.ClientName = value;
            if (!TqIsValidClientName(cfg.ClientName)) {
                err = "invalid value for --client-name";
                return false;
            }
        } else if (GetOptionValue(arg, "--forward", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--forward", err);
                if (value == nullptr) {
                    return false;
                }
            }
            TqPortForwardConfig forward;
            if (!ParsePortForwardValue(value, forward)) {
                err = "invalid value for --forward";
                return false;
            }
            cfg.PortForwards.push_back(std::move(forward));
        } else if (GetOptionValue(arg, "--client-config", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--client-config", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.ClientConfigPath = value;
            clientConfigSpecified = true;
        } else if (GetOptionValue(arg, "--admin-listen", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--admin-listen", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.AdminListen = value;
        } else if (GetOptionValue(arg, "--admin-token-file", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--admin-token-file", err);
                if (value == nullptr) {
                    return false;
                }
            }
            cfg.AdminTokenFile = value;
        } else if (GetOptionValue(arg, "--admin-threads", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--admin-threads", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32InRange(value, 1, 32, cfg.AdminThreads)) {
                err = "invalid value for --admin-threads (must be 1..32)";
                return false;
            }
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
            if (cfg.Mode == TqMode::Server) {
                err = "--enable-encrypt is client-only";
                return false;
            }
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
                std::strcmp(value, "wan") != 0) {
                err = "invalid value for --tuning (auto|lan|wan)";
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
        } else if (GetOptionValue(arg, "--relay-read-chunk-size", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--relay-read-chunk-size", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideRelayReadChunkSize) ||
                cfg.TuningOverrideRelayReadChunkSize == 0) {
                err = "invalid value for --relay-read-chunk-size";
                return false;
            }
        } else if (GetOptionValue(arg, "--relay-tcp-write-max-bytes", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--relay-tcp-write-max-bytes", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideRelayTcpWriteMaxBytes) ||
                cfg.TuningOverrideRelayTcpWriteMaxBytes == 0) {
                err = "invalid value for --relay-tcp-write-max-bytes";
                return false;
            }
        } else if (GetOptionValue(arg, "--relay-tcp-write-burst-bytes", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--relay-tcp-write-burst-bytes", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.TuningOverrideRelayTcpWriteBurstBytes) ||
                cfg.TuningOverrideRelayTcpWriteBurstBytes == 0) {
                err = "invalid value for --relay-tcp-write-burst-bytes";
                return false;
            }
        } else if (GetOptionValue(arg, "--relay-event-queue-capacity", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--relay-event-queue-capacity", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32InRange(
                    value,
                    TqLinuxRelayEventQueueCapacityMin,
                    TqLinuxRelayEventQueueCapacityMax,
                    cfg.TuningOverrideRelayEventQueueCapacity)) {
                err = "invalid value for --relay-event-queue-capacity";
                return false;
            }
        } else if (GetOptionValue(arg, "--relay-worker-count", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--relay-worker-count", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32InRange(
                    value,
                    TqRelayWorkerCountMin,
                    TqRelayWorkerCountMax,
                    cfg.TuningOverrideRelayWorkerCount) ||
                cfg.TuningOverrideRelayWorkerCount == 0) {
                err = "invalid value for --relay-worker-count";
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
        } else if (GetOptionValue(arg, "--linux-relay-event-queue-capacity", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--linux-relay-event-queue-capacity", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32InRange(
                    value,
                    TqLinuxRelayEventQueueCapacityMin,
                    TqLinuxRelayEventQueueCapacityMax,
                    cfg.TuningOverrideLinuxRelayEventQueueCapacity)) {
                err = "invalid value for --linux-relay-event-queue-capacity";
                return false;
            }
        } else if (GetOptionValue(arg, "--linux-relay-worker-count", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--linux-relay-worker-count", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32InRange(
                    value,
                    TqRelayWorkerCountMin,
                    TqRelayWorkerCountMax,
                    cfg.TuningOverrideLinuxRelayWorkerCount) ||
                cfg.TuningOverrideLinuxRelayWorkerCount == 0) {
                err = "invalid value for --linux-relay-worker-count";
                return false;
            }
        } else if (GetOptionValue(arg, "--windows-relay-worker-count", value)) {
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--windows-relay-worker-count", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32InRange(
                    value,
                    TqRelayWorkerCountMin,
                    TqRelayWorkerCountMax,
                    cfg.TuningOverrideWindowsRelayWorkerCount) ||
                cfg.TuningOverrideWindowsRelayWorkerCount == 0) {
                err = "invalid value for --windows-relay-worker-count";
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
        } else if (GetOptionValue(arg, "--trace-level", value)) {
            cfg.Trace = true;
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--trace-level", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (std::strcmp(value, "info") == 0) {
                cfg.TraceLogLevel = TqConfig::TraceLevel::Info;
            } else if (std::strcmp(value, "debug") == 0) {
                cfg.TraceLogLevel = TqConfig::TraceLevel::Debug;
            } else {
                err = "invalid value for --trace-level (expected info or debug)";
                return false;
            }
        } else if (std::strcmp(arg, "--diag-stats") == 0) {
            cfg.DiagStats = true;
        } else if (GetOptionValue(arg, "--diag-stats-interval", value)) {
            cfg.DiagStats = true;
            if (value == nullptr) {
                value = NextArg(i, argc, argv, "--diag-stats-interval", err);
                if (value == nullptr) {
                    return false;
                }
            }
            if (!ParseUint32(value, cfg.DiagStatsIntervalSec) || cfg.DiagStatsIntervalSec == 0) {
                err = "invalid value for --diag-stats-interval";
                return false;
            }
        } else {
            err = std::string("unknown option: ") + arg;
            return false;
        }
    }

    if (clientConfigSpecified && cfg.Mode != TqMode::Client) {
        err = "--client-config is valid only in client mode";
        return false;
    }
    if (!cfg.PortForwards.empty() && cfg.Mode != TqMode::Client) {
        err = "--forward is valid only in client mode";
        return false;
    }
    if (clientConfigSpecified && (!cfg.QuicPeer.empty() || quicPeerSpecified)) {
        err = "--client-config and --peer are mutually exclusive";
        return false;
    }
    if (cfg.SpeedTestMode != TqSpeedTestMode::None) {
        if (cfg.Mode != TqMode::Client) {
            err = "speed-test options are valid only in client mode";
            return false;
        }
        if (clientConfigSpecified) {
            err = "speed-test options cannot be used with --client-config";
            return false;
        }
    }
    if (cfg.Mode == TqMode::Client && !configSpecified && !clientConfigSpecified && cfg.ClientConfigPath.empty()) {
        cfg.ClientConfigPath = DefaultRoleConfigPath(argc > 0 ? argv[0] : nullptr, TqMode::Client);
    }
    if (clientConfigSpecified) {
        if (!TqLoadClientConfig(cfg.ClientConfigPath, cfg.Router, err)) {
            return false;
        }
    }
    if (!cfg.Router.Peers.empty()) {
        if (!TqValidateRouterConfig(cfg.Router, err)) {
            return false;
        }
    }

    if (cfg.Mode == TqMode::Client) {
        if (cfg.Router.Peers.empty()) {
            const bool hasLegacyIngress =
                socksListenSpecified || httpListenSpecified || !cfg.PortForwards.empty();
            const bool hasLegacySinglePeer =
                quicPeerSpecified || !cfg.QuicPeer.empty() || hasLegacyIngress ||
                cfg.SpeedTestMode != TqSpeedTestMode::None;
            if (hasLegacySinglePeer && !RequireNonEmpty(cfg.QuicPeer, "--peer", err)) {
                return false;
            }
            if (hasLegacySinglePeer) {
                std::set<std::string> listens;
                if (cfg.SocksListen.empty() && cfg.HttpListen.empty() && cfg.PortForwards.empty()) {
                    err = "at least one ingress is required";
                    return false;
                }
                if (!cfg.SocksListen.empty()) {
                    if (!IsHostPort(cfg.SocksListen)) {
                        err = "invalid socks listen";
                        return false;
                    }
                    if (!listens.insert(cfg.SocksListen).second) {
                        err = "duplicate listen: " + cfg.SocksListen;
                        return false;
                    }
                }
                if (!cfg.HttpListen.empty()) {
                    if (!IsHostPort(cfg.HttpListen)) {
                        err = "invalid http listen";
                        return false;
                    }
                    if (!listens.insert(cfg.HttpListen).second) {
                        err = "duplicate listen: " + cfg.HttpListen;
                        return false;
                    }
                }
                for (const auto& forward : cfg.PortForwards) {
                    if (!IsHostPort(forward.Listen)) {
                        err = "invalid port_forward.listen";
                        return false;
                    }
                    if (!IsValidPortForwardTarget(forward)) {
                        err = "invalid port_forward.target";
                        return false;
                    }
                    if (!listens.insert(forward.Listen).second) {
                        err = "duplicate listen: " + forward.Listen;
                        return false;
                    }
                }
            }
        }
        if (!RequireNonEmpty(cfg.QuicCa, "--ca", err)) {
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
        if (configSpecified && (configFileMissing || legacyRouterConfigLoadedFromConfig)) {
            EnsureDefaultClientName(cfg);
            if (!WriteTextFileAtomically(cfg.ConfigPath, RuntimeConfigJson(cfg), err)) {
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
        if (cfg.AllowTargets.empty()) {
            cfg.AllowTargets.push_back("0.0.0.0/0");
        }
        if (!TqValidateCidrList(cfg.AllowTargets, err)) {
            return false;
        }
        if (!cfg.DenyTargets.empty() && !TqValidateCidrList(cfg.DenyTargets, err)) {
            return false;
        }
        if (cfg.ConfigPath.empty()) {
            cfg.ConfigPath = DefaultRoleConfigPath(argc > 0 ? argv[0] : nullptr, TqMode::Server);
        }
        if (!configSpecified || configFileMissing) {
            if (!WriteTextFileAtomically(cfg.ConfigPath, RuntimeConfigJson(cfg), err)) {
                return false;
            }
        }
    }

    return true;
}

bool TqLoadClientConfig(const std::string& path, TqRouterConfig& router, std::string& err) {
    std::ifstream file(path);
    if (!file) {
        if (!std::filesystem::exists(path)) {
            router = TqRouterConfig{};
            return true;
        }
        err = "failed to open client config: " + path;
        return false;
    }
    std::string body((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (body.empty() || IsWhitespaceOnly(body)) {
        router = TqRouterConfig{};
        return true;
    }
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
        if (peer.QuicPaths.empty()) {
            if (!IsHostPortList(peer.QuicPeer)) { err = "invalid quic_peer for " + peer.PeerId; return false; }
        } else {
            std::set<std::string> pathNames;
            uint32_t pathConnections = 0;
            for (const auto& path : peer.QuicPaths) {
                if (path.Name.empty()) { err = "path name is required for " + peer.PeerId; return false; }
                if (!pathNames.insert(path.Name).second) { err = "duplicate path name: " + path.Name; return false; }
                if (!IsValidQuicPathLocalAddress(path.LocalAddress)) {
                    err = "invalid path local for " + peer.PeerId;
                    return false;
                }
                if (!IsHostPort(path.Peer)) { err = "invalid path peer for " + peer.PeerId; return false; }
                if (path.Connections == 0 || path.Connections > 128 || pathConnections > 128 - path.Connections) {
                    err = "path connections out of range for " + peer.PeerId;
                    return false;
                }
                pathConnections += path.Connections;
            }
        }
        if (peer.SocksListen.empty() && peer.HttpListen.empty() && peer.PortForwards.empty()) {
            err = "at least one ingress is required for " + peer.PeerId;
            return false;
        }
        if (!peer.SocksListen.empty() && !IsHostPort(peer.SocksListen)) { err = "invalid socks_listen for " + peer.PeerId; return false; }
        if (!peer.SocksListen.empty() && !listens.insert(peer.SocksListen).second) { err = "duplicate listen: " + peer.SocksListen; return false; }
        if (!peer.HttpListen.empty() && !IsHostPort(peer.HttpListen)) { err = "invalid http_listen for " + peer.PeerId; return false; }
        if (!peer.HttpListen.empty() && !listens.insert(peer.HttpListen).second) { err = "duplicate listen: " + peer.HttpListen; return false; }
        for (const auto& forward : peer.PortForwards) {
            if (!IsHostPort(forward.Listen)) { err = "invalid port_forward.listen for " + peer.PeerId; return false; }
            if (!IsValidPortForwardTarget(forward)) { err = "invalid port_forward.target for " + peer.PeerId; return false; }
            if (!listens.insert(forward.Listen).second) { err = "duplicate listen: " + forward.Listen; return false; }
        }
        if (peer.QuicConnections > 128) { err = "quic_connections out of range for " + peer.PeerId; return false; }
        if (!IsValidCompress(peer.Compress)) { err = "invalid compress for " + peer.PeerId; return false; }
    }
    return true;
}
