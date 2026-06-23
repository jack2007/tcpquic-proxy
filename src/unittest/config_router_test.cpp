#include "config.h"

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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

static bool Load(const std::string& body, TqRouterConfig& router, std::string& err) {
    std::string file = WriteTempConfig(body);
    err.clear();
    return TqLoadClientConfig(file, router, err);
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
        if (usage.find("Linux relay TCP read chunk size") == std::string::npos) return 146;
        if (usage.find("--max-memory-mb <n>") == std::string::npos) return 149;
        if (usage.find("--forward <local=target>") == std::string::npos) return 187;
        if (usage.find("--admin-token-file <path>") == std::string::npos) return 188;
        if (usage.find("--admin-threads <n>") == std::string::npos) return 189;
        if (usage.find("--admin-allow-unauthenticated-legacy") == std::string::npos) return 190;
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
        if (cfg.AdminAllowUnauthenticatedLegacy) return 192;
    }
    {
        const char* args[] = {
            "tcpquic-proxy", "client",
            "--peer", "127.0.0.1:14444",
            "--ca", "ca.crt",
            "--admin-listen", "127.0.0.1:19091",
            "--admin-token-file", "/tmp/tcpquic-admin-token.json",
            "--admin-threads", "4",
            "--admin-allow-unauthenticated-legacy"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 193;
        if (cfg.AdminTokenFile != "/tmp/tcpquic-admin-token.json") return 194;
        if (cfg.AdminThreads != 4) return 195;
        if (!cfg.AdminAllowUnauthenticatedLegacy) return 196;
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
        if (!ParseRuntimeConfig(R"json({
            "tls":{"ca":"ca.crt"},
            "admin":{"listen":"127.0.0.1:19091","token_file":"/tmp/tq-admin.json","threads":3,"allow_unauthenticated_legacy":true},
            "peers":[{"id":"agent-b","proto_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001"}]
        })json", cfg, err)) {
            std::fprintf(stderr, "runtime admin config parse failed: %s\n", err.c_str());
            return 199;
        }
        if (cfg.AdminListen != "127.0.0.1:19091") return 200;
        if (cfg.AdminTokenFile != "/tmp/tq-admin.json") return 201;
        if (cfg.AdminThreads != 3) return 202;
        if (!cfg.AdminAllowUnauthenticatedLegacy) return 203;
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
        if (cfg.Trace) return 172;
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
            "relay":{"linux":{"read_chunk_size":262144,"worker_slots":1024}}
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
        if (cfg.Tuning.LinuxRelayReadChunkSize != 262144) return 88;
        if (cfg.Tuning.InitialRttMs != 1) return 90;
        if (!cfg.QuicCa.empty()) return 186;
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
        const char* args[] = {"tcpquic-proxy", "server", "--listen", "0.0.0.0:4433", "--allow-targets", "127.0.0.1/32", "--enable-encrypt", "--cert", "a.crt", "--key", "a.key", "--ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 142;
        if (cfg.QuicDisable1RttEncryption) return 143;
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
