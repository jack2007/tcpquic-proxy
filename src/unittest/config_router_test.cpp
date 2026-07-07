#include "config.h"
#include "quic_address.h"
#include "relay_metrics.h"
#include "server_metrics.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::string WriteTempConfig(const std::string& body) {
    static unsigned counter = 0;
    std::ostringstream name;
    name << "tcpquic-client-config-" << std::rand() << "-" << counter++ << ".json";
    std::filesystem::path path = std::filesystem::temp_directory_path() / name.str();
    std::ofstream f(path);
    if (!f) abort();
    f << body;
    f.close();
    if (!f) abort();
    return path.string();
}

static std::string ReadTextFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string TempConfigPath(const std::string& name) {
    static unsigned counter = 0;
    std::ostringstream pathName;
    pathName << name << "-" << std::rand() << "-" << counter++ << ".json";
    return (std::filesystem::temp_directory_path() / pathName.str()).string();
}

static bool Parse(int argc, char** argv, TqConfig& cfg, std::string& err) {
    err.clear();
    bool ok = TqParseArgs(argc, argv, cfg, err);
    if (ok) TqFinalizeConfig(cfg);
    return ok;
}

static bool ParseRuntimeConfig(const std::string& body, TqConfig& cfg, std::string& err) {
    std::string file = WriteTempConfig(body);
    const char* args[] = {"tcpquic-proxy", "client", "--config", file.c_str()};
    return Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err);
}

static const char* ExpectedClientNamePrefix() {
#if defined(_WIN32)
    return "win-";
#elif defined(__APPLE__)
    return "macos-";
#else
    return "linux-";
#endif
}

static bool Load(const std::string& body, TqRouterConfig& router, std::string& err) {
    std::string file = WriteTempConfig(body);
    err.clear();
    return TqLoadClientConfig(file, router, err);
}

TqRelayMetricsSnapshot TqSnapshotRelayMetrics() {
    return {};
}

std::string TqRelayMetricsFieldsJson(const TqRelayMetricsSnapshot&) {
    return "{\"linux_relay_backend\":\"test\"}";
}

static std::string CaptureUsage() {
    FILE* file = std::tmpfile();
    if (file == nullptr) abort();
    TqPrintUsage(file);
    if (std::fflush(file) != 0) abort();
    if (std::fseek(file, 0, SEEK_END) != 0) abort();
    const long size = std::ftell(file);
    if (size < 0) abort();
    if (std::fseek(file, 0, SEEK_SET) != 0) abort();
    std::string text(static_cast<size_t>(size), '\0');
    if (size > 0 && std::fread(&text[0], 1, static_cast<size_t>(size), file) != static_cast<size_t>(size)) {
        abort();
    }
    std::fclose(file);
    return text;
}

int main() {
    const std::string removedAdminFlag = std::string("--admin-allow-unauthenticated-") + "legacy";
    const std::string removedAdminKey = std::string("allow_") + "unauthenticated_" + "legacy";
    {
        const std::string usage = CaptureUsage();
        if (usage.find("Client and Server:") == std::string::npos) return 110;
        if (usage.find("Client specific:") == std::string::npos) return 111;
        if (usage.find("Server specific:") == std::string::npos) return 112;
        if (usage.find("Protocol and relay tuning:") == std::string::npos) return 113;
        if (usage.find("Diagnostics:") == std::string::npos) return 114;
        if (usage.find("-h, --help, --usage") == std::string::npos) return 180;
        if (usage.find("--config <path>") == std::string::npos) return 115;
        if (usage.find("--allow-targets <list>") == std::string::npos) return 116;
        if (usage.find("Client TLS: --ca is required") == std::string::npos) return 175;
        if (usage.find("Server TLS: --cert and --key are required") == std::string::npos) return 176;
        if (usage.find("default 0.0.0.0/0") == std::string::npos) return 150;
        if (usage.find("--download-test <sec>") == std::string::npos) return 117;
        if (usage.find("--connection-stream-count <n>") == std::string::npos) return 118;
        if (usage.find("--peer <addr>") == std::string::npos) return 129;
        if (usage.find("--client-name <name>") == std::string::npos) return 401;
        if (usage.find("--listen <addr>") == std::string::npos) return 130;
        if (usage.find("--reconnect-interval-ms") != std::string::npos) return 131;
        if (usage.find("--keepalive-ms <n>") == std::string::npos) return 154;
        if (usage.find("default 5000, 1000..15000") == std::string::npos) return 155;
        if (usage.find("--diag-stats                 Low-overhead periodic stderr stats") == std::string::npos) return 169;
        if (usage.find("--trace-connect-on-start") != std::string::npos) return 174;
        if (usage.find("--compress <mode>            auto|zstd|off (default off)") == std::string::npos) return 133;
        if (usage.find("--quic-") != std::string::npos) return 134;
        if (usage.find("QUIC") != std::string::npos) return 135;
        if (usage.find("--warmup-") != std::string::npos) return 136;
        if (usage.find("--download-sink-test") != std::string::npos) return 137;
        if (usage.find("--enable-encrypt") == std::string::npos) return 138;
        if (usage.find("--disable-1rtt-encryption") != std::string::npos) return 139;
        if (usage.find("--relay-inflight-bytes") != std::string::npos) return 144;
        if (usage.find("--initial-quic-read-ahead") != std::string::npos) return 147;
        if (usage.find("--target-bandwidth-mbps") != std::string::npos) return 148;
        if (usage.find("--target-rtt-ms") != std::string::npos) return 156;
        if (usage.find("--fcw") != std::string::npos) return 157;
        if (usage.find("--srw") != std::string::npos) return 158;
        if (usage.find("custom") != std::string::npos) return 159;
        if (usage.find("--iw <packets>") == std::string::npos) return 160;
        if (usage.find("--initrtt-ms <n>") == std::string::npos) return 161;
        if (usage.find("--relay-io-size <bytes>      Override relay IO size") == std::string::npos) return 145;
        if (usage.find("--relay-read-chunk-size <bytes>") == std::string::npos) return 146;
        if (usage.find("--linux-relay-read-chunk-size <bytes>") == std::string::npos) return 192;
        if (usage.find("--max-memory-mb <n>") == std::string::npos) return 149;
        if (usage.find("--forward <local=target>") == std::string::npos) return 187;
        if (usage.find("--admin-token-file <path>") == std::string::npos) return 188;
        if (usage.find("--admin-threads <n>") == std::string::npos) return 189;
        if (usage.find(removedAdminFlag) != std::string::npos) return 190;
    }
    {
        const char* helpOptions[] = {"-h", "--help", "--usage"};
        for (const char* helpOption : helpOptions) {
            const char* args[] = {"tcpquic-proxy", helpOption};
            TqConfig cfg;
            std::string err;
            if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 181;
            if (!cfg.ShowUsage) return 182;
            if (!err.empty()) return 183;
        }
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--help"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 184;
        if (!cfg.ShowUsage) return 185;
        if (!err.empty()) return 186;
    }
    {
        std::string file = WriteTempConfig(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001","http_listen":"127.0.0.1:18001","quic_connections":4,"compress":"auto","enabled":true}]})json");
        const char* args[] = {"tcpquic-proxy", "client", "--client-config", file.c_str(), "--admin-listen", "127.0.0.1:19091", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 1;
        if (cfg.ClientConfigPath != file) return 2;
        if (cfg.AdminListen != "127.0.0.1:19091") return 3;
        if (cfg.Router.Peers.size() != 1) return 4;
        if (cfg.Router.Peers[0].PeerId != "agent-b") return 5;
        if (cfg.Router.Peers[0].QuicPeer != "127.0.0.1:14444") return 6;
        if (cfg.Router.Peers[0].SocksListen != "127.0.0.1:11001") return 7;
        if (cfg.Router.Peers[0].HttpListen != "127.0.0.1:18001") return 8;
        if (cfg.Router.Peers[0].QuicConnections != 4) return 9;
        if (!cfg.Router.Peers[0].Enabled) return 10;
        if (cfg.QuicCa != "ca.crt") return 177;
        if (!cfg.QuicCert.empty()) return 178;
        if (!cfg.QuicKey.empty()) return 179;
        if (cfg.AdminThreads != 2) return 191;
    }
    {
        std::string file = WriteTempConfig(R"json({"version":1,"peers":[{"peer_id":"agent-b","client_name":"office-a","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001"}]})json");
        const char* args[] = {"tcpquic-proxy", "client", "--client-config", file.c_str(), "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 402;
        if (err.find("client_name") == std::string::npos) return 403;
    }
    {
        std::string file = WriteTempConfig(R"json({
            "tls":{"ca":"ca.crt"},
            "client":{"client_name":"office-a"},
            "peers":[{"id":"agent-b","proto_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001"}]
        })json");
        const char* args[] = {"tcpquic-proxy", "client", "--config", file.c_str()};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 404;
        if (cfg.ClientName != "office-a") return 409;
        if (cfg.Router.Peers.size() != 1) return 410;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 412;
        if (cfg.ClientName.rfind(ExpectedClientNamePrefix(), 0) != 0) return 413;
        if (cfg.ClientName.size() <= std::string(ExpectedClientNamePrefix()).size()) return 414;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--ca", "ca.crt", "--client-name", "edge-a"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 405;
        if (cfg.ClientName != "edge-a") return 406;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--ca", "ca.crt", "--client-name", "bad name"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 407;
        if (err.find("client-name") == std::string::npos) return 408;
    }
    {
        const char* args[] = {
            "tcpquic-proxy", "client",
            "--peer", "127.0.0.1:14444",
            "--ca", "ca.crt",
            removedAdminFlag.c_str()};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 193;
        if (err.find("unknown argument") == std::string::npos &&
            err.find(removedAdminFlag.substr(2)) == std::string::npos) return 194;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--ca", "ca.crt", "--admin-threads", "0"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 197;
        if (err.find("admin-threads") == std::string::npos) return 198;
    }
    {
        TqConfig cfg;
        std::string err;
        const std::string json = std::string(R"json({
            "tls":{"ca":"ca.crt"},
            "admin":{"listen":"127.0.0.1:19091",")json") +
            removedAdminKey + R"json(":true},
            "peers":[{"id":"agent-b","proto_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001"}]
        })json";
        if (ParseRuntimeConfig(json, cfg, err)) {
            return 199;
        }
        if (err.find("unknown admin key") == std::string::npos &&
            err.find(removedAdminKey) == std::string::npos) return 200;
    }
    {
        TqConfig cfg;
        std::string err;
        if (!ParseRuntimeConfig(R"json({
            "tls":{"ca":"ca.crt"},
            "admin":{"listen":"127.0.0.1:19091","token_file":"/tmp/tq-admin.json","threads":3},
            "peers":[{"id":"agent-b","proto_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001"}]
        })json", cfg, err)) {
            std::fprintf(stderr, "runtime admin config parse failed: %s\n", err.c_str());
            return 201;
        }
        if (cfg.AdminListen != "127.0.0.1:19091") return 202;
        if (cfg.AdminTokenFile != "/tmp/tq-admin.json") return 203;
        if (cfg.AdminThreads != 3) return 204;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (!Load(
                R"json({"version":1,"proxy_auth":[{"username":"alice","password":"secret-a"},{"username":"bob","password":"secret-b"}],"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001","http_listen":"127.0.0.1:18001"}]})json",
                router,
                err)) return 120;
        if (router.ProxyAuth.size() != 2) return 121;
        if (router.ProxyAuth[0].Username != "alice") return 122;
        if (router.ProxyAuth[0].Password != "secret-a") return 123;
        if (router.ProxyAuth[1].Username != "bob") return 124;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"proxy_auth":[{"username":"alice","password":""}],"peers":[]})json", router, err)) return 125;
        if (err.find("non-empty") == std::string::npos) return 126;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(
                R"json({"version":1,"proxy_auth":[{"username":"alice","password":"a"},{"username":"alice","password":"b"}],"peers":[]})json",
                router,
                err)) return 127;
        if (err.find("duplicate proxy_auth username") == std::string::npos) return 128;
    }
    {
        std::string file = WriteTempConfig(R"json({"version":1,"peers":[]})json");
        const char* args[] = {"tcpquic-proxy", "client", "--client-config", file.c_str(), "--peer", "127.0.0.1:14444"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 11;
        if (err.find("mutually exclusive") == std::string::npos) return 12;
    }
    {
        std::string file = WriteTempConfig(R"json({"version":1,"peers":[]})json");
        const char* args[] = {"tcpquic-proxy", "client", "--client-config", file.c_str(), "--peer", "", "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 42;
        if (err.find("mutually exclusive") == std::string::npos) return 43;
    }
    {
        std::string file = WriteTempConfig(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001"},{"peer_id":"agent-b","quic_peer":"127.0.0.1:14445","socks_listen":"127.0.0.1:11002"}]})json");
        TqRouterConfig router;
        std::string err;
        if (TqLoadClientConfig(file, router, err)) return 13;
        if (err.find("duplicate peer_id") == std::string::npos) return 14;
    }
    {
        std::string file = WriteTempConfig(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001"},{"peer_id":"agent-c","quic_peer":"127.0.0.1:14445","socks_listen":"127.0.0.1:11001"}]})json");
        TqRouterConfig router;
        std::string err;
        if (TqLoadClientConfig(file, router, err)) return 15;
        if (err.find("duplicate listen") == std::string::npos) return 16;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (!Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:14444","port_forwards":[{"listen":"127.0.0.1:15432","target":"db.example.com:5432"},{"listen":"[::1]:18080","target":"10.0.0.15:8080"}]}]})json", router, err)) return 200;
        if (router.Peers.size() != 1) return 201;
        if (router.Peers[0].PortForwards.size() != 2) return 202;
        if (router.Peers[0].PortForwards[0].Listen != "127.0.0.1:15432") return 203;
        if (router.Peers[0].PortForwards[0].TargetHost != "db.example.com") return 204;
        if (router.Peers[0].PortForwards[0].TargetPort != 5432) return 205;
        if (router.Peers[0].PortForwards[1].Listen != "[::1]:18080") return 206;
        if (router.Peers[0].PortForwards[1].TargetHost != "10.0.0.15") return 207;
        if (router.Peers[0].PortForwards[1].TargetPort != 8080) return 208;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:14444","socks_listen":"","http_listen":"","port_forwards":[]}]})json", router, err)) return 209;
        if (err.find("at least one ingress") == std::string::npos) return 210;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:15432","port_forwards":[{"listen":"127.0.0.1:15432","target":"db.example.com:5432"}]}]})json", router, err)) return 211;
        if (err.find("duplicate listen") == std::string::npos) return 212;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":2,"peers":[]})json", router, err)) return 17;
        if (err.find("version must be 1") == std::string::npos) return 18;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[})json", router, err)) return 19;
        if (err.empty()) return 20;
    }
    {
        std::string file = WriteTempConfig(R"json({"version":1,"peers":[]})json");
        const char* args[] = {"tcpquic-proxy", "server", "--client-config", file.c_str()};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 21;
        if (err.find("valid only in client mode") == std::string::npos) return 22;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:notaport","socks_listen":"127.0.0.1:11001"}]})json", router, err)) return 23;
        if (err.find("invalid quic_peer") == std::string::npos) return 24;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:99999","socks_listen":"127.0.0.1:11001"}]})json", router, err)) return 25;
        if (err.find("invalid quic_peer") == std::string::npos) return 26;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"2001:db8::1:443","socks_listen":"127.0.0.1:11001"}]})json", router, err)) return 27;
        if (err.find("invalid quic_peer") == std::string::npos) return 28;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:14444","socks_listen":""}]})json", router, err)) return 29;
        if (err.find("at least one ingress") == std::string::npos) return 30;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:notaport"}]})json", router, err)) return 49;
        if (err.find("invalid socks_listen") == std::string::npos) return 50;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001","http_listen":"127.0.0.1:99999"}]})json", router, err)) return 51;
        if (err.find("invalid http_listen") == std::string::npos) return 52;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001","quic_connections":129}]})json", router, err)) return 31;
        if (err.find("quic_connections out of range") == std::string::npos) return 32;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (!Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"36.1.1.10:443,59.1.1.10:443","socks_listen":"127.0.0.1:11001","quic_connections":8}]})json", router, err)) return 300;
        if (router.Peers.size() != 1) return 301;
        if (router.Peers[0].QuicPeer != "36.1.1.10:443,59.1.1.10:443") return 302;
        if (router.Peers[0].QuicConnections != 8) return 303;
    }
    {
        std::vector<TqEndpoint> endpoints;
        std::string err;
        if (!TqParseEndpointList("36.1.1.10:443,59.1.1.10:443", endpoints, err)) return 329;
        if (endpoints.size() != 2) return 330;
        if (endpoints[0].Host != "36.1.1.10" || endpoints[0].Port != 443) return 331;
        if (endpoints[1].Host != "59.1.1.10" || endpoints[1].Port != 443) return 332;
    }
    {
        std::vector<TqEndpoint> endpoints;
        std::string err;
        if (!TqParseEndpointList("[2001:db8::1]:443,[2001:db8::2]:443", endpoints, err)) return 333;
        if (endpoints.size() != 2) return 334;
        if (endpoints[0].Host != "2001:db8::1" || endpoints[0].Port != 443) return 335;
        if (endpoints[1].Host != "2001:db8::2" || endpoints[1].Port != 443) return 336;
        if (TqFormatEndpoint(endpoints[0]) != "[2001:db8::1]:443") return 337;
    }
    {
        std::vector<TqEndpoint> endpoints;
        std::string err;
        if (TqParseEndpointList("2001:db8::1:443", endpoints, err)) return 338;
        if (err.find("invalid endpoint") == std::string::npos) return 339;
    }
    {
        std::vector<TqResolvedListen> listens;
        std::string err;
        if (!TqResolveServerListenList("36.1.1.10:443,59.1.1.10:443", listens, err)) return 340;
        if (listens.size() != 2) return 341;
        if (listens[0].Text != "36.1.1.10:443") return 342;
        if (listens[1].Text != "59.1.1.10:443") return 343;
        const std::vector<TqResolvedListen> binds = TqBuildServerListenerBindList(listens);
        if (binds.size() != 1) return 1200;
        if (QuicAddrGetFamily(&binds[0].Address) != QUIC_ADDRESS_FAMILY_INET) return 1201;
        if (QuicAddrGetPort(&binds[0].Address) != 443) return 1202;
        QUIC_ADDR ipv4Wildcard{};
        if (!TqMakeQuicAddr(TqEndpoint{"0.0.0.0", 443}, ipv4Wildcard)) return 1213;
        if (!QuicAddrCompare(&binds[0].Address, &ipv4Wildcard)) return 1214;
        QUIC_ADDR cmcc{};
        QUIC_ADDR ctcc{};
        QUIC_ADDR other{};
        QUIC_ADDR wrongPort{};
        if (!TqMakeQuicAddr(TqEndpoint{"36.1.1.10", 443}, cmcc)) return 1203;
        if (!TqMakeQuicAddr(TqEndpoint{"59.1.1.10", 443}, ctcc)) return 1204;
        if (!TqMakeQuicAddr(TqEndpoint{"10.0.0.99", 443}, other)) return 1205;
        if (!TqMakeQuicAddr(TqEndpoint{"36.1.1.10", 8443}, wrongPort)) return 1206;
        if (!TqServerListenAllowsLocalAddress(listens, &cmcc)) return 1207;
        if (!TqServerListenAllowsLocalAddress(listens, &ctcc)) return 1208;
        if (TqServerListenAllowsLocalAddress(listens, &other)) return 1209;
        if (TqServerListenAllowsLocalAddress(listens, &wrongPort)) return 1210;
        if (TqServerListenAllowsLocalAddress(listens, nullptr)) return 1215;
    }
    {
        std::vector<TqResolvedListen> listens;
        std::string err;
        if (!TqResolveServerListenList("36.1.1.10:443,59.1.1.10:8443", listens, err)) return 1211;
        const std::vector<TqResolvedListen> binds = TqBuildServerListenerBindList(listens);
        if (binds.size() != 2) return 1212;
    }
    {
        std::vector<TqResolvedListen> listens;
        std::string err;
        if (!TqResolveServerListenList("[2001:db8::1]:443,[2001:db8::2]:443", listens, err)) return 1216;
        const std::vector<TqResolvedListen> binds = TqBuildServerListenerBindList(listens);
        if (binds.size() != 1) return 1217;
        QUIC_ADDR ipv6Wildcard{};
        if (!TqMakeQuicAddr(TqEndpoint{"::", 443}, ipv6Wildcard)) return 1218;
        if (!QuicAddrCompare(&binds[0].Address, &ipv6Wildcard)) return 1219;
        QUIC_ADDR allowed{};
        QUIC_ADDR denied{};
        if (!TqMakeQuicAddr(TqEndpoint{"2001:db8::1", 443}, allowed)) return 1220;
        if (!TqMakeQuicAddr(TqEndpoint{"2001:db8::3", 443}, denied)) return 1221;
        if (!TqServerListenAllowsLocalAddress(listens, &allowed)) return 1222;
        if (TqServerListenAllowsLocalAddress(listens, &denied)) return 1223;
    }
    {
        std::vector<TqResolvedListen> listens;
        std::string err;
        if (!TqResolveServerListenList("36.1.1.10:443,[2001:db8::1]:443", listens, err)) return 1224;
        const std::vector<TqResolvedListen> binds = TqBuildServerListenerBindList(listens);
        if (binds.size() != 2) return 1225;
    }
    {
        std::vector<TqResolvedListen> listens;
        std::string err;
        if (!TqResolveServerListenList("*:443", listens, err)) return 1226;
        QUIC_ADDR ipv4AnyAllowed{};
        QUIC_ADDR ipv6AnyAllowed{};
        QUIC_ADDR wrongPort{};
        if (!TqMakeQuicAddr(TqEndpoint{"10.0.0.99", 443}, ipv4AnyAllowed)) return 1227;
        if (!TqMakeQuicAddr(TqEndpoint{"2001:db8::99", 443}, ipv6AnyAllowed)) return 1228;
        if (!TqMakeQuicAddr(TqEndpoint{"10.0.0.99", 8443}, wrongPort)) return 1229;
        if (!TqServerListenAllowsLocalAddress(listens, &ipv4AnyAllowed)) return 1230;
        if (!TqServerListenAllowsLocalAddress(listens, &ipv6AnyAllowed)) return 1231;
        if (TqServerListenAllowsLocalAddress(listens, &wrongPort)) return 1232;
    }
    {
        std::vector<TqResolvedListen> listens;
        std::string err;
        if (!TqResolveServerListenList("[0:0:0:0:0:0:0:1]:443,[::1]:443", listens, err)) return 344;
        if (listens.size() != 1) return 345;
        if (listens[0].Text != "[::1]:443") return 346;
    }
    {
        std::vector<TqResolvedListen> listens;
        std::string err;
        if (!TqResolveServerListenList("*:443", listens, err)) return 349;
        if (listens.size() != 1) return 350;
        if (QuicAddrGetFamily(&listens[0].Address) != QUIC_ADDRESS_FAMILY_UNSPEC) return 351;
        if (QuicAddrGetPort(&listens[0].Address) != 443) return 352;
        if (listens[0].Text != "*:443") return 353;
    }
    {
        TqServerMetrics metrics;
        metrics.Listen = "0.0.0.0:443";
        metrics.ResolvedListens = {"10.0.0.2:443", "10.0.0.3:443"};
        const std::string body = TqServerMetricsJson(metrics, 11);
        if (body.find("\"listen\":\"0.0.0.0:443\"") == std::string::npos) return 347;
        if (body.find("\"resolved_listens\":[\"10.0.0.2:443\",\"10.0.0.3:443\"]") == std::string::npos) return 348;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (!Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","socks_listen":"127.0.0.1:11001","paths":[{"name":"cmcc","local":"10.0.0.2","peer":"36.1.1.10:443","connections":4},{"name":"ctcc","local":"10.0.0.3","peer":"59.1.1.10:443","connections":4}]}]})json", router, err)) return 304;
        if (router.Peers.size() != 1) return 305;
        if (router.Peers[0].QuicPaths.size() != 2) return 306;
        if (router.Peers[0].QuicPaths[0].Name != "cmcc") return 307;
        if (router.Peers[0].QuicPaths[0].LocalAddress != "10.0.0.2") return 308;
        if (router.Peers[0].QuicPaths[0].Peer != "36.1.1.10:443") return 309;
        if (router.Peers[0].QuicPaths[0].Connections != 4) return 310;
        if (router.Peers[0].QuicPaths[1].Name != "ctcc") return 311;
        if (router.Peers[0].QuicPaths[1].LocalAddress != "10.0.0.3") return 312;
        if (router.Peers[0].QuicPaths[1].Peer != "59.1.1.10:443") return 313;
        if (router.Peers[0].QuicPaths[1].Connections != 4) return 314;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","socks_listen":"127.0.0.1:11001","paths":[{"name":"cmcc","local":"10.0.0.2","peer":"36.1.1.10:443","connections":0}]}]})json", router, err)) return 315;
        if (err.find("path connections out of range") == std::string::npos) return 316;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","socks_listen":"127.0.0.1:11001","paths":[{"name":"cmcc","local":"10.0.0.2","peer":"36.1.1.10:443","connections":4},{"name":"cmcc","local":"10.0.0.3","peer":"59.1.1.10:443","connections":4}]}]})json", router, err)) return 317;
        if (err.find("duplicate path name") == std::string::npos) return 318;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","socks_listen":"127.0.0.1:11001","paths":[{"name":"cmcc","peer":"36.1.1.10:443","connections":4}]}]})json", router, err)) return 325;
        if (err.find("path name, local, peer and connections are required") == std::string::npos) return 326;
    }
    {
        const char* locals[] = {"10.0.0.2:0", "bad local", "   ", "localhost", "not_an_ip", "999.999.999.999"};
        for (size_t i = 0; i < sizeof(locals) / sizeof(locals[0]); ++i) {
            std::string body =
                R"json({"version":1,"peers":[{"peer_id":"agent-b","socks_listen":"127.0.0.1:11001","paths":[{"name":"cmcc","local":")json";
            body += locals[i];
            body += R"json(","peer":"36.1.1.10:443","connections":4}]}]})json";
            TqRouterConfig router;
            std::string err;
            if (Load(body, router, err)) return 327;
            if (err.find("invalid path local") == std::string::npos) return 328;
        }
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001","quic_connections":0}]})json", router, err)) return 44;
        if (err.find("quic_connections") == std::string::npos) return 45;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001","compress":"gzip"}]})json", router, err)) return 33;
        if (err.find("invalid compress") == std::string::npos) return 34;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (!Load(R"json({"version":1,"peers":[{"peer_id":"auto","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001","compress":"auto"},{"peer_id":"zstd","quic_peer":"127.0.0.1:14445","socks_listen":"127.0.0.1:11002","compress":"zstd"},{"peer_id":"off","quic_peer":"127.0.0.1:14447","socks_listen":"127.0.0.1:11004","compress":"off"},{"peer_id":"default","quic_peer":"127.0.0.1:14448","socks_listen":"127.0.0.1:11005"}]})json", router, err)) return 35;
        if (router.Peers.size() != 4) return 36;
        if (router.Peers[0].Compress != "auto") return 37;
        if (router.Peers[1].Compress != "zstd") return 38;
        if (router.Peers[2].Compress != "off") return 40;
        if (!router.Peers[3].Compress.empty()) return 41;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"lz4","quic_peer":"127.0.0.1:14446","socks_listen":"127.0.0.1:11003","compress":"lz4"}]})json", router, err)) return 39;
        if (err.find("invalid compress") == std::string::npos) return 73;
    }
    {
        char arg0[] = "tcpquic-proxy";
        char arg1[] = "client";
        char arg2[] = "--peer";
        char arg3[] = "127.0.0.1:4433";
        char arg4[] = "--compress";
        char arg5[] = "lz4";
        char arg6[] = "--cert";
        char arg7[] = "a.crt";
        char arg8[] = "--key";
        char arg9[] = "a.key";
        char arg10[] = "--ca";
        char arg11[] = "ca.crt";
        char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11};
        TqConfig cfg;
        std::string err;
        if (Parse(12, argv, cfg, err)) return 74;
        if (err.find("invalid compress") == std::string::npos) return 75;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (!Load(R"json({"version":1,"peers":[{"peer_id":"\u0061gent-b","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001"}]})json", router, err)) return 46;
        if (router.Peers.size() != 1) return 47;
        if (router.Peers[0].PeerId != "agent-b") return 48;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":01,"peers":[]})json", router, err)) return 53;
        if (err.empty()) return 54;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001","quic_connections":004}]})json", router, err)) return 55;
        if (err.empty()) return 56;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 57;
        if (cfg.QuicConnectionStreamCount != 1024) return 119;
        if (cfg.Compress != "off") return 138;
    }
    {
        const char* args[] = {
            "tcpquic-proxy",
            "client",
            "--peer",
            "127.0.0.1:14444",
            "--forward",
            "127.0.0.1:15432=db.example.com:5432",
            "--forward=[::1]:18080=10.0.0.15:8080",
            "--cert",
            "a.crt",
            "--key",
            "a.key",
            "--ca",
            "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 188;
        if (cfg.PortForwards.size() != 2) return 189;
        if (cfg.PortForwards[0].Listen != "127.0.0.1:15432") return 190;
        if (cfg.PortForwards[0].TargetHost != "db.example.com") return 191;
        if (cfg.PortForwards[0].TargetPort != 5432) return 192;
        if (cfg.PortForwards[1].Listen != "[::1]:18080") return 193;
        if (cfg.PortForwards[1].TargetHost != "10.0.0.15") return 194;
        if (cfg.PortForwards[1].TargetPort != 8080) return 195;
    }
    {
        const char* values[] = {
            "=target",
            "listen=",
            "127.0.0.1:0=target:1",
            "127.0.0.1:99999=target:1",
            "127.0.0.1:15432=2001:db8::1:443",
        };
        for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
            const char* args[] = {
                "tcpquic-proxy",
                "client",
                "--peer",
                "127.0.0.1:14444",
                "--forward",
                values[i],
                "--cert",
                "a.crt",
                "--key",
                "a.key",
                "--ca",
                "ca.crt"};
            TqConfig cfg;
            std::string err;
            if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 220;
            if (err.find("--forward") == std::string::npos) return 221;
        }
    }
    {
        const std::string longHost(256, 'a');
        const std::string forward = "127.0.0.1:15432=" + longHost + ":5432";
        const char* args[] = {
            "tcpquic-proxy",
            "client",
            "--peer",
            "127.0.0.1:14444",
            "--forward",
            forward.c_str(),
            "--cert",
            "a.crt",
            "--key",
            "a.key",
            "--ca",
            "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 275;
        if (err.find("--forward") == std::string::npos) return 276;
    }
    {
        const char* args[] = {
            "tcpquic-proxy",
            "client",
            "--peer",
            "127.0.0.1:14444",
            "--forward",
            "127.0.0.1:15432",
            "--cert",
            "a.crt",
            "--key",
            "a.key",
            "--ca",
            "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 196;
        if (err.find("--forward") == std::string::npos) return 197;
    }
    {
        const char* args[] = {
            "tcpquic-proxy",
            "server",
            "--listen",
            "0.0.0.0:4433",
            "--forward",
            "127.0.0.1:15432=db.example.com:5432",
            "--cert",
            "a.crt",
            "--key",
            "a.key"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 198;
        if (err.find("--forward") == std::string::npos) return 199;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--connection-stream-count", "2048", "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 120;
        if (cfg.QuicConnectionStreamCount != 2048) return 121;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--connection-stream-count", "0", "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 122;
        if (err.find("--connection-stream-count") == std::string::npos) return 123;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--connection-stream-count", "70000", "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 124;
        if (err.find("--connection-stream-count") == std::string::npos) return 125;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--reconnect-interval-ms", "1000", "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 59;
        if (err.find("--reconnect-interval-ms") == std::string::npos) return 60;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 156;
        if (cfg.QuicKeepAliveIntervalMs != 5000) return 157;
        if (!cfg.Trace) return 205;
        if (cfg.TraceIntervalSec != 30) return 206;
    }
    {
        const char* args[] = {
            "tcpquic-proxy", "client", "--peer", "127.0.0.1:14444",
            "--diag-stats", "--diag-stats-interval", "5",
            "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 170;
        if (!cfg.DiagStats) return 171;
        if (!cfg.Trace) return 172;
        if (cfg.DiagStatsIntervalSec != 5) return 173;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--keepalive-ms", "1000", "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 158;
        if (cfg.QuicKeepAliveIntervalMs != 1000) return 159;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--keepalive-ms=15000", "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 160;
        if (cfg.QuicKeepAliveIntervalMs != 15000) return 161;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--keepalive-ms", "999", "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 162;
        if (err.find("--keepalive-ms") == std::string::npos) return 163;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--keepalive-ms", "15001", "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 164;
        if (err.find("--keepalive-ms") == std::string::npos) return 165;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--download-sink-test", "30", "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 76;
        if (cfg.SpeedTestMode != TqSpeedTestMode::DownloadSink) return 77;
        if (cfg.SpeedTestDurationSec != 30) return 78;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444", "--download-test", "30", "--download-sink-test", "30", "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 79;
        if (err.find("mutually exclusive") == std::string::npos) return 80;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--peer", "127.0.0.1:14444"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 180;
        if (err.find("--ca") == std::string::npos) return 181;
    }
    {
        const char* args[] = {
            "tcpquic-proxy",
            "client",
            "--ca",
            "ca.crt",
            "--admin-listen",
            "127.0.0.1:19091"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) {
            std::fprintf(stderr, "admin-only client parse failed: %s\n", err.c_str());
            return 325;
        }
        if (!cfg.Router.Peers.empty()) return 326;
        if (!cfg.QuicPeer.empty()) return 327;
        if (cfg.QuicCa != "ca.crt") return 328;
        if (cfg.AdminListen != "127.0.0.1:19091") return 329;
    }
    {
        const std::string missing = TempConfigPath("tcpquic-missing-client-config");
        std::filesystem::remove(missing);
        TqRouterConfig router;
        std::string err;
        if (!TqLoadClientConfig(missing, router, err)) {
            std::fprintf(stderr, "missing client config should be accepted: %s\n", err.c_str());
            return 332;
        }
        if (!router.Peers.empty()) return 333;
    }
    {
        const std::string empty = WriteTempConfig("");
        TqRouterConfig router;
        std::string err;
        if (!TqLoadClientConfig(empty, router, err)) {
            std::fprintf(stderr, "empty client config should be accepted: %s\n", err.c_str());
            return 334;
        }
        if (!router.Peers.empty()) return 335;
    }
    {
        const char* args[] = {
            "/tmp/tcpquic-proxy-test-bin",
            "client",
            "--ca",
            "ca.crt",
            "--admin-listen",
            "127.0.0.1:19091"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) {
            std::fprintf(stderr, "default client config path parse failed: %s\n", err.c_str());
            return 336;
        }
        if (cfg.ClientConfigPath.empty()) return 337;
        if (cfg.ClientConfigPath.find("tcpquic-proxy-test-bin") == std::string::npos) return 338;
        if (cfg.ClientConfigPath.find("client-config-") == std::string::npos) return 339;
    }
    {
        const std::string file = TempConfigPath("tcpquic-missing-client-runtime-config");
        std::filesystem::remove(file);
        const char* args[] = {
            "tcpquic-proxy",
            "client",
            "--config",
            file.c_str(),
            "--peer",
            "127.0.0.1:14444",
            "--ca",
            "ca.crt",
            "--socks-listen",
            "127.0.0.1:11080"
        };
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) {
            std::fprintf(stderr, "missing client runtime config parse failed: %s\n", err.c_str());
            return 361;
        }
        if (cfg.ConfigPath != file) return 365;
        if (!cfg.ClientConfigPath.empty()) return 366;
        if (!std::filesystem::exists(file)) return 362;
        nlohmann::json json = nlohmann::json::parse(ReadTextFile(file));
        if (!json.contains("client")) return 363;
        if (json["client"].value("client_name", "").rfind(ExpectedClientNamePrefix(), 0) != 0) return 364;
        if (!json.contains("peers") || json["peers"].size() != 1) return 367;
        if (json["peers"][0].value("id", "") != "primary") return 368;
        if (json["peers"][0].value("proto_peer", "") != "127.0.0.1:14444") return 369;
        if (json["peers"][0].value("socks_listen", "") != "127.0.0.1:11080") return 370;
        std::filesystem::remove(file);
    }
    {
        const std::string file = WriteTempConfig(R"json({"version":1,"peers":[{"peer_id":"persisted","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11080","enabled":false}]})json");
        const char* args[] = {
            "tcpquic-proxy",
            "client",
            "--ca",
            "ca.crt",
            "--config",
            file.c_str()};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) {
            std::fprintf(stderr, "router-style --config should be accepted for client: %s\n", err.c_str());
            return 340;
        }
        if (cfg.Router.Peers.size() != 1) return 341;
        if (cfg.Router.Peers[0].PeerId != "persisted") return 342;
        if (!cfg.ClientConfigPath.empty()) return 347;
        if (cfg.ClientName.rfind(ExpectedClientNamePrefix(), 0) != 0) return 343;
        if (cfg.ClientName.size() <= std::string(ExpectedClientNamePrefix()).size()) return 344;
        nlohmann::json upgraded = nlohmann::json::parse(ReadTextFile(file));
        if (!upgraded.contains("client")) return 345;
        if (upgraded["client"].value("client_name", "").rfind(ExpectedClientNamePrefix(), 0) != 0) return 346;
        if (upgraded["peers"][0].contains("proto_connections")) return 348;
        TqConfig reparsed;
        std::string reparseErr;
        const char* reparseArgs[] = {"tcpquic-proxy", "client", "--config", file.c_str()};
        if (!Parse((int)(sizeof(reparseArgs) / sizeof(reparseArgs[0])), const_cast<char**>(reparseArgs), reparsed, reparseErr)) return 349;
    }
    {
        const char* args[] = {
            "tcpquic-proxy",
            "client",
            "--ca",
            "ca.crt",
            "--socks-listen",
            "127.0.0.1:11080"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 330;
        if (err.find("--peer") == std::string::npos) return 331;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"bad","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001","quic_reconnect_interval_ms":5000}]})json", router, err)) return 71;
        if (err.find("quic_reconnect_interval_ms") == std::string::npos) return 72;
    }
    {
        std::string file = WriteTempConfig(R"json({
            "tls":{"cert":"client.crt","key":"client.key","ca":"ca.crt"},
            "client":{"warmup_mb":1},
            "peers":[{"id":"primary","proto_peer":"127.0.0.1:4433","socks_listen":"127.0.0.1:11080"}]
        })json");
        const char* args[] = {"tcpquic-proxy", "client", "--config", file.c_str()};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 156;
        if (err.find("warmup") == std::string::npos) return 157;
    }
    {
        std::string file = WriteTempConfig(R"json({
            "tls":{"ca":"ca.crt"},
            "peers":[{"id":"primary","proto_peer":"127.0.0.1:4433","socks_listen":"127.0.0.1:11080"}]
        })json");
        const char* args[] = {"tcpquic-proxy", "client", "--config", file.c_str()};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 182;
        if (cfg.QuicCa != "ca.crt") return 183;
        if (!cfg.QuicCert.empty()) return 184;
        if (!cfg.QuicKey.empty()) return 185;
    }
    {
        TqConfig cfg;
        std::string err;
        if (!ParseRuntimeConfig(R"json({
            "tls":{"ca":"ca.crt"},
            "peers":[{
                "id":"primary",
                "proto_peer":"127.0.0.1:4433",
                "port_forwards":[
                    {"listen":"127.0.0.1:15432","target":"db.example.com:5432"},
                    {"listen":"[::1]:18080","target":"10.0.0.15:8080"}
                ]
            }]
        })json", cfg, err)) return 222;
        if (cfg.Router.Peers.size() != 1) return 223;
        if (cfg.Router.Peers[0].PeerId != "primary") return 224;
        if (cfg.Router.Peers[0].QuicPeer != "127.0.0.1:4433") return 225;
        if (cfg.Router.Peers[0].PortForwards.size() != 2) return 226;
        if (cfg.Router.Peers[0].PortForwards[0].Listen != "127.0.0.1:15432") return 227;
        if (cfg.Router.Peers[0].PortForwards[0].TargetHost != "db.example.com") return 228;
        if (cfg.Router.Peers[0].PortForwards[0].TargetPort != 5432) return 229;
        if (cfg.Router.Peers[0].PortForwards[1].Listen != "[::1]:18080") return 230;
        if (cfg.Router.Peers[0].PortForwards[1].TargetHost != "10.0.0.15") return 231;
        if (cfg.Router.Peers[0].PortForwards[1].TargetPort != 8080) return 232;
    }
    {
        struct Case {
            std::string portForwards;
            const char* expectedError;
        };
        const std::string longHost(256, 'a');
        const Case cases[] = {
            {R"json([{"target":"db.example.com:5432"}])json", "port_forward listen and target are required"},
            {R"json([{"listen":"127.0.0.1:15432"}])json", "port_forward listen and target are required"},
            {R"json([{"listen":"127.0.0.1:15432","target":"db.example.com:5432","extra":true}])json", "unknown port_forward key"},
            {R"json({"listen":"127.0.0.1:15432","target":"db.example.com:5432"})json", "port_forwards must be an array"},
            {R"json([{"listen":"127.0.0.1:15432","target":"db.example.com:99999"}])json", "invalid port_forward.target"},
            {R"json([{"listen":"127.0.0.1:15432","target":")json" + longHost + R"json(:5432"}])json", "invalid port_forward.target"},
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            std::string body =
                R"json({"tls":{"ca":"ca.crt"},"peers":[{"id":"primary","proto_peer":"127.0.0.1:4433","port_forwards":)json";
            body += cases[i].portForwards;
            body += R"json(}]})json";
            TqConfig cfg;
            std::string err;
            if (ParseRuntimeConfig(body, cfg, err)) return 233;
            if (err.find(cases[i].expectedError) == std::string::npos) return 234;
        }
    }
    {
        TqConfig cfg;
        std::string err;
        if (!ParseRuntimeConfig(R"json({
            "tls":{"ca":"ca.crt"},
            "peers":[{
                "id":"primary",
                "socks_listen":"127.0.0.1:11080",
                "paths":[
                    {"name":"cmcc","local":"10.0.0.2","peer":"36.1.1.10:443","connections":4},
                    {"name":"ctcc","local":"10.0.0.3","peer":"59.1.1.10:443","connections":4}
                ]
            }]
        })json", cfg, err)) return 319;
        if (cfg.Router.Peers.size() != 1) return 320;
        if (!cfg.Router.Peers[0].QuicPeer.empty()) return 321;
        if (cfg.Router.Peers[0].QuicPaths.size() != 2) return 322;
        if (cfg.Router.Peers[0].QuicPaths[0].Name != "cmcc") return 323;
        if (cfg.Router.Peers[0].QuicPaths[1].Peer != "59.1.1.10:443") return 324;
    }
    {
        std::string file = WriteTempConfig(R"json({
            "tls":{"cert":"server.crt","key":"server.key"},
            "server":{
                "proto_listen":"0.0.0.0:4433",
                "allow_targets":["127.0.0.1/32","10.0.0.0/8"],
                "deny_targets":"192.168.0.0/16"
            },
            "compression":{"mode":"off"},
            "proto":{"disable_1rtt_encryption":true,"initrtt_ms":1},
            "tuning":{"mode":"wan"},
            "relay":{"linux":{"read_chunk_size":131072},"common":{"read_chunk_size":262144,"tcp_write_max_bytes":1048576,"tcp_write_burst_bytes":2097152,"event_queue_capacity":8192,"worker_count":2}}
        })json");
        const char* args[] = {"tcpquic-proxy", "server", "--config", file.c_str()};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 81;
        if (cfg.QuicListen != "0.0.0.0:4433") return 82;
        if (cfg.QuicCert != "server.crt") return 83;
        if (cfg.AllowTargets.size() != 2) return 84;
        if (cfg.DenyTargets.size() != 1) return 85;
        if (cfg.Compress != "off") return 86;
        if (!cfg.QuicDisable1RttEncryption) return 87;
        if (cfg.Tuning.RelayReadChunkSize != 262144) return 201;
        if (cfg.Tuning.LinuxRelayReadChunkSize != 262144) return 202;
        if (cfg.Tuning.RelayTcpWriteMaxBytes != 1048576) return 203;
        if (cfg.Tuning.RelayTcpWriteBurstBytes != 2097152) return 204;
        if (cfg.Tuning.RelayEventQueueCapacity != 8192) return 205;
        if (cfg.Tuning.RelayWorkerCount != 2) return 206;
        if (cfg.Tuning.InitialRttMs != 1) return 90;
        if (!cfg.QuicCa.empty()) return 186;
    }
    {
        const std::string file = TempConfigPath("tcpquic-server-encryption-policy-config");
        std::filesystem::remove(file);
        TqConfig cfg;
        std::string err;
        const char* args[] = {
            "tcpquic-proxy",
            "server",
            "--config",
            file.c_str(),
            "--listen",
            "0.0.0.0:4433",
            "--cert",
            "server.crt",
            "--key",
            "server.key"};
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 146;
        const auto root = nlohmann::json::parse(ReadTextFile(file));
        if (root["proto"].value("encryption_policy", "") != "client-choice") return 147;
        if (root["proto"].contains("disable_1rtt_encryption")) return 148;
        TqConfig reparsed;
        std::string reparseErr;
        const char* reparseArgs[] = {"tcpquic-proxy", "server", "--config", file.c_str()};
        if (!Parse((int)(sizeof(reparseArgs) / sizeof(reparseArgs[0])), const_cast<char**>(reparseArgs), reparsed, reparseErr)) return 371;
        std::filesystem::remove(file);
    }
    {
        const std::string file = WriteTempConfig(R"json({
            "tls":{"cert":"server.crt","key":"server.key"},
            "server":{"proto_listen":"0.0.0.0:4433"},
            "proto":{"disable_1rtt_encryption":false}
        })json");
        TqConfig cfg;
        std::string err;
        const char* args[] = {"tcpquic-proxy", "server", "--config", file.c_str()};
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 149;
        if (!cfg.QuicDisable1RttEncryption) return 150;
        std::filesystem::remove(file);
    }
    {
        const std::string file = TempConfigPath("tcpquic-missing-server-runtime-config");
        std::filesystem::remove(file);
        const char* args[] = {
            "tcpquic-proxy",
            "server",
            "--config",
            file.c_str(),
            "--listen",
            "0.0.0.0:4433",
            "--cert",
            "server.crt",
            "--key",
            "server.key",
            "--allow-targets",
            "127.0.0.1/32",
            "--admin-listen",
            "127.0.0.1:19092",
            "--admin-threads",
            "3",
            "--relay-read-chunk-size",
            "262144",
            "--compress",
            "zstd",
            "--compress-level",
            "2",
            "--trace-interval",
            "7"
        };
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) {
            std::fprintf(stderr, "missing server --config should generate config: %s\n", err.c_str());
            return 340;
        }
        if (cfg.ConfigPath != file) return 341;
        if (!std::filesystem::exists(file)) return 342;

        const std::string body = ReadTextFile(file);
        nlohmann::json root = nlohmann::json::parse(body);
        if (root["tls"]["cert"] != "server.crt") return 343;
        if (root["tls"]["key"] != "server.key") return 344;
        if (root["server"]["proto_listen"] != "0.0.0.0:4433") return 345;
        if (root["server"]["allow_targets"] != nlohmann::json::array({"127.0.0.1/32"})) return 346;
        if (root["admin"]["listen"] != "127.0.0.1:19092") return 347;
        if (root["admin"]["threads"] != 3) return 348;
        if (root["compression"]["mode"] != "zstd") return 349;
        if (root["compression"]["level"] != 2) return 350;
        if (root["relay"]["common"]["read_chunk_size"] != 262144) return 364;
        if (root["trace"]["interval_sec"] != 7) return 351;
        if (root.contains("peers")) return 352;
        std::filesystem::remove(file);
    }
    {
        const std::string file = TempConfigPath("tcpquic-missing-server-runtime-config-required");
        std::filesystem::remove(file);
        const char* args[] = {
            "tcpquic-proxy",
            "server",
            "--config",
            file.c_str(),
            "--listen",
            "0.0.0.0:4433"
        };
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 353;
        if (err.find("--cert") == std::string::npos) return 354;
        if (std::filesystem::exists(file)) return 355;
    }
    {
        const char* args[] = {
            "/tmp/tcpquic-proxy-test-bin",
            "server",
            "--listen",
            "0.0.0.0:4433",
            "--cert",
            "server.crt",
            "--key",
            "server.key"
        };
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) {
            std::fprintf(stderr, "default server config path parse failed: %s\n", err.c_str());
            return 356;
        }
        if (cfg.ConfigPath.empty()) return 357;
        if (cfg.ConfigPath.find("tcpquic-proxy-test-bin") == std::string::npos) return 358;
        if (cfg.ConfigPath.find("server-config-") == std::string::npos) return 359;
        if (!std::filesystem::exists(cfg.ConfigPath)) return 360;
        std::filesystem::remove(cfg.ConfigPath);
    }
    {
        const std::string file = WriteTempConfig(R"json({"tls":{"cert":"server.crt"}})json");
        const std::string before = ReadTextFile(file);
        const char* args[] = {"tcpquic-proxy", "server", "--config", file.c_str()};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 361;
        if (err.find("--listen") == std::string::npos) return 362;
        if (ReadTextFile(file) != before) return 363;
        std::filesystem::remove(file);
    }
    {
        const char* args[] = {"tcpquic-proxy", "server", "--listen", "0.0.0.0:4433", "--allow-targets", "127.0.0.1/32", "--cert", "a.crt", "--key", "a.key"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 140;
        if (!cfg.QuicDisable1RttEncryption) return 141;
    }
    {
        const char* args[] = {"tcpquic-proxy", "server", "--listen", "0.0.0.0:4433", "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 151;
        if (cfg.AllowTargets.size() != 1) return 152;
        if (cfg.AllowTargets[0] != "0.0.0.0/0") return 153;
    }
    {
        TqConfig cfg;
        std::string err;
        const char* args[] = {
            "tcpquic-proxy",
            "server",
            "--listen",
            "0.0.0.0:4433",
            "--allow-targets",
            "127.0.0.1/32",
            "--enable-encrypt",
            "--cert",
            "a.crt",
            "--key",
            "a.key"};
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 142;
        if (err.find("--enable-encrypt is client-only") == std::string::npos) return 143;
    }
    {
        TqConfig cfg;
        std::string err;
        const char* args[] = {
            "tcpquic-proxy",
            "client",
            "--peer",
            "127.0.0.1:14444",
            "--socks-listen",
            "127.0.0.1:11080",
            "--enable-encrypt",
            "--ca",
            "ca.crt"};
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 144;
        if (cfg.QuicDisable1RttEncryption) return 145;
    }
    {
        const char* removed[][2] = {
            {"--target-bandwidth-mbps", "10000"},
            {"--target-rtt-ms", "100"},
            {"--relay-inflight-bytes", "1048576"},
            {"--initial-quic-read-ahead", "2097152"},
            {"--fcw", "1073741824"},
            {"--srw", "1073741824"},
            {"--tuning", "custom"},
        };
        for (const auto& item : removed) {
            const char* args[] = {
                "tcpquic-proxy",
                "client",
                "--peer",
                "127.0.0.1:4433",
                item[0],
                item[1],
                "--cert",
                "a.crt",
                "--key",
                "a.key",
                "--ca",
                "ca.crt"};
            TqConfig cfg;
            std::string err;
            if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 147;
            if (err.empty()) return 148;
        }
    }
    {
        const char* args[] = {
            "tcpquic-proxy",
            "server",
            "--listen",
            "0.0.0.0:4433",
            "--allow-targets",
            "127.0.0.1/32",
            "--max-memory-mb",
            "4096",
            "--cert",
            "a.crt",
            "--key",
            "a.key",
            "--ca",
            "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 151;
        if (cfg.MaxMemoryMb != 4096) return 152;
    }
    {
        std::string file = WriteTempConfig(R"json({
            "tls":{"cert":"server.crt","key":"server.key","ca":"ca.crt"},
            "server":{
                "proto_listen":"0.0.0.0:4433",
                "allow_targets":["127.0.0.1/32"]
            },
            "tuning":{"max_memory_mb":4096}
        })json");
        const char* args[] = {"tcpquic-proxy", "server", "--config", file.c_str()};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 152;
        if (err.find("unknown tuning key: max_memory_mb") == std::string::npos) return 153;
    }
    {
        const char* configs[] = {
            R"json({"tls":{"cert":"server.crt","key":"server.key","ca":"ca.crt"},"server":{"proto_listen":"0.0.0.0:4433"},"proto":{"fcw":1073741824}})json",
            R"json({"tls":{"cert":"server.crt","key":"server.key","ca":"ca.crt"},"server":{"proto_listen":"0.0.0.0:4433"},"proto":{"srw":1073741824}})json",
            R"json({"tls":{"cert":"server.crt","key":"server.key","ca":"ca.crt"},"server":{"proto_listen":"0.0.0.0:4433"},"relay":{"inflight_bytes":1048576}})json",
            R"json({"tls":{"cert":"server.crt","key":"server.key","ca":"ca.crt"},"server":{"proto_listen":"0.0.0.0:4433"},"relay":{"initial_quic_read_ahead":2097152}})json",
            R"json({"tls":{"cert":"server.crt","key":"server.key","ca":"ca.crt"},"server":{"proto_listen":"0.0.0.0:4433"},"tuning":{"target_bandwidth_mbps":10000}})json",
            R"json({"tls":{"cert":"server.crt","key":"server.key","ca":"ca.crt"},"server":{"proto_listen":"0.0.0.0:4433"},"tuning":{"target_rtt_ms":100}})json",
            R"json({"tls":{"cert":"server.crt","key":"server.key","ca":"ca.crt"},"server":{"proto_listen":"0.0.0.0:4433"},"tuning":{"mode":"custom"}})json",
        };
        for (const char* body : configs) {
            std::string file = WriteTempConfig(body);
            const char* args[] = {"tcpquic-proxy", "server", "--config", file.c_str()};
            TqConfig cfg;
            std::string err;
            if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 154;
            if (err.empty()) return 155;
        }
    }
    {
        std::string file = WriteTempConfig(R"json({
            "tls":{"cert":"client.crt","key":"client.key","ca":"ca.crt"},
            "proto":{"profile":"low-latency","connections":8,"connection_stream_count":2048,"keepalive_ms":15000},
            "client":{"download_test":30,"handshake_threads":4},
            "trace":{"enabled":true,"interval_sec":2},
            "peers":[{
                "id":"primary",
                "proto_peer":"127.0.0.1:4433",
                "socks_listen":"127.0.0.1:11080",
                "http_listen":"127.0.0.1:18080",
                "proto_connections":8,
                "enabled":true
            }]
        })json");
        const char* args[] = {"tcpquic-proxy", "client", "--config", file.c_str()};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 91;
        if (cfg.Router.Peers.size() != 1) return 92;
        if (cfg.Router.Peers[0].SocksListen != "127.0.0.1:11080") return 93;
        if (cfg.Router.Peers[0].QuicPeer != "127.0.0.1:4433") return 94;
        if (cfg.Router.Peers[0].QuicConnections != 8) return 95;
        if (cfg.QuicConnectionStreamCount != 2048) return 96;
        if (cfg.QuicKeepAliveIntervalMs != 15000) return 166;
        if (cfg.SpeedTestMode != TqSpeedTestMode::Download) return 97;
        if (cfg.SpeedTestDurationSec != 30) return 98;
        if (cfg.QuicProfile != TqQuicProfile::LowLatency) return 99;
        if (cfg.HandshakeThreads != 4) return 100;
        if (!cfg.Trace || cfg.TraceIntervalSec != 2) return 101;
    }
    {
        std::string file = WriteTempConfig(R"json({
            "tls":{"cert":"client.crt","key":"client.key","ca":"ca.crt"},
            "trace":{"enabled":false},
            "peers":[{
                "id":"primary",
                "proto_peer":"127.0.0.1:4433",
                "socks_listen":"127.0.0.1:11080"
            }]
        })json");
        const char* args[] = {"tcpquic-proxy", "client", "--config", file.c_str()};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 207;
        if (cfg.Trace) return 208;
        if (cfg.TraceIntervalSec != 30) return 209;
    }
    {
        const char* args[] = {
            "tcpquic-proxy",
            "client",
            "--peer",
            "127.0.0.1:4433",
            "--trace-connect-on-start",
            "--cert",
            "a.crt",
            "--key",
            "a.key",
            "--ca",
            "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 175;
        if (err.find("unknown option: --trace-connect-on-start") == std::string::npos) return 176;
    }
    {
        std::string file = WriteTempConfig(R"json({
            "tls":{"cert":"client.crt","key":"client.key","ca":"ca.crt"},
            "trace":{"connect_on_start":true},
            "peers":[{"id":"primary","proto_peer":"127.0.0.1:4433","socks_listen":"127.0.0.1:11080"}]
        })json");
        const char* args[] = {"tcpquic-proxy", "client", "--config", file.c_str()};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 177;
        if (err.find("unknown trace key: connect_on_start") == std::string::npos) return 178;
    }
    {
        std::string file = WriteTempConfig(R"json({
            "tls":{"cert":"client.crt","key":"client.key","ca":"ca.crt"},
            "proto":{"keepalive_ms":999},
            "peers":[{"id":"primary","proto_peer":"127.0.0.1:4433","socks_listen":"127.0.0.1:11080"}]
        })json");
        const char* args[] = {"tcpquic-proxy", "client", "--config", file.c_str()};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 167;
        if (err.find("proto.keepalive_ms") == std::string::npos) return 168;
    }
    {
        std::string file = WriteTempConfig(R"json({
            "tls":{"cert":"client.crt","key":"client.key","ca":"ca.crt"},
            "peers":[
                {"id":"a","proto_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001"},
                {"id":"b","proto_peer":"127.0.0.1:14445","socks_listen":"127.0.0.1:11002"}
            ],
            "proto":{"reconnect_interval_ms":4000}
        })json");
        const char* args[] = {"tcpquic-proxy", "client", "--config", file.c_str()};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 102;
        if (err.find("reconnect_interval_ms") == std::string::npos) return 103;
    }
    {
        std::string file = WriteTempConfig(R"json({
            "tls":{"cert":"client.crt","key":"client.key","ca":"ca.crt"},
            "peers":[
                {"id":"b","proto_peer":"127.0.0.1:14445","socks_listen":"127.0.0.1:11002","proto_reconnect_interval_ms":5000}
            ]
        })json");
        const char* args[] = {"tcpquic-proxy", "client", "--config", file.c_str()};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 106;
        if (err.find("proto_reconnect_interval_ms") == std::string::npos) return 107;
    }
    {
        std::string file = WriteTempConfig(R"json({
            "tls":{"cert":"client.crt","key":"client.key","ca":"ca.crt"},
            "client":{"download_test":30,"upload_test":30},
            "peers":[{"id":"primary","proto_peer":"127.0.0.1:4433","socks_listen":"127.0.0.1:11080"}]
        })json");
        const char* args[] = {"tcpquic-proxy", "client", "--config", file.c_str()};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 104;
        if (err.find("speed-test options are mutually exclusive") == std::string::npos) return 105;
    }
    {
        std::string file = WriteTempConfig(R"json({
            "quic_peer":"127.0.0.1:4433",
            "quic_cert":"client.crt",
            "quic_key":"client.key",
            "quic_ca":"ca.crt"
        })json");
        const char* args[] = {"tcpquic-proxy", "client", "--config", file.c_str()};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 108;
        if (err.find("unknown config key") == std::string::npos) return 109;
    }
    return 0;
}
