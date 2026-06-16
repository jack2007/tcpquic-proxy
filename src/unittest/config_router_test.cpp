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
        if (usage.find("--config <path>") == std::string::npos) return 115;
        if (usage.find("--allow-targets <list>") == std::string::npos) return 116;
        if (usage.find("--download-test <sec>") == std::string::npos) return 117;
    }
    {
        std::string file = WriteTempConfig(R"json({"version":1,"peers":[{"peer_id":"agent-b","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001","http_listen":"127.0.0.1:18001","quic_connections":4,"compress":"auto","enabled":true}]})json");
        const char* args[] = {"tcpquic-proxy", "client", "--client-config", file.c_str(), "--admin-listen", "127.0.0.1:19091", "--quic-cert", "a.crt", "--quic-key", "a.key", "--quic-ca", "ca.crt"};
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
        const char* args[] = {"tcpquic-proxy", "client", "--client-config", file.c_str(), "--quic-peer", "127.0.0.1:14444"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 11;
        if (err.find("mutually exclusive") == std::string::npos) return 12;
    }
    {
        std::string file = WriteTempConfig(R"json({"version":1,"peers":[]})json");
        const char* args[] = {"tcpquic-proxy", "client", "--client-config", file.c_str(), "--quic-peer", "", "--quic-cert", "a.crt", "--quic-key", "a.key", "--quic-ca", "ca.crt"};
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
        if (err.find("socks_listen is required") == std::string::npos) return 30;
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
        char arg2[] = "--quic-peer";
        char arg3[] = "127.0.0.1:4433";
        char arg4[] = "--compress";
        char arg5[] = "lz4";
        char arg6[] = "--quic-cert";
        char arg7[] = "a.crt";
        char arg8[] = "--quic-key";
        char arg9[] = "a.key";
        char arg10[] = "--quic-ca";
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
        const char* args[] = {"tcpquic-proxy", "client", "--quic-peer", "127.0.0.1:14444", "--quic-cert", "a.crt", "--quic-key", "a.key", "--quic-ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 57;
        if (cfg.QuicReconnectIntervalMs != 3000) return 58;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--quic-peer", "127.0.0.1:14444", "--quic-reconnect-interval-ms", "1000", "--quic-cert", "a.crt", "--quic-key", "a.key", "--quic-ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 59;
        if (cfg.QuicReconnectIntervalMs != 1000) return 60;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--quic-peer", "127.0.0.1:14444", "--quic-reconnect-interval-ms", "60000", "--quic-cert", "a.crt", "--quic-key", "a.key", "--quic-ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 61;
        if (cfg.QuicReconnectIntervalMs != 60000) return 62;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--quic-peer", "127.0.0.1:14444", "--quic-reconnect-interval-ms", "999", "--quic-cert", "a.crt", "--quic-key", "a.key", "--quic-ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 63;
        if (err.find("--quic-reconnect-interval-ms") == std::string::npos) return 64;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--quic-peer", "127.0.0.1:14444", "--quic-reconnect-interval-ms", "60001", "--quic-cert", "a.crt", "--quic-key", "a.key", "--quic-ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 65;
        if (err.find("--quic-reconnect-interval-ms") == std::string::npos) return 66;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--quic-peer", "127.0.0.1:14444", "--download-sink-test", "30", "--quic-cert", "a.crt", "--quic-key", "a.key", "--quic-ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 76;
        if (cfg.SpeedTestMode != TqSpeedTestMode::DownloadSink) return 77;
        if (cfg.SpeedTestDurationSec != 30) return 78;
    }
    {
        const char* args[] = {"tcpquic-proxy", "client", "--quic-peer", "127.0.0.1:14444", "--download-test", "30", "--download-sink-test", "30", "--quic-cert", "a.crt", "--quic-key", "a.key", "--quic-ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 79;
        if (err.find("mutually exclusive") == std::string::npos) return 80;
    }
    {
        std::string file = WriteTempConfig(R"json({"version":1,"peers":[{"peer_id":"inherit","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001"},{"peer_id":"override","quic_peer":"127.0.0.1:14445","socks_listen":"127.0.0.1:11002","quic_reconnect_interval_ms":5000}]})json");
        const char* args[] = {"tcpquic-proxy", "client", "--client-config", file.c_str(), "--quic-reconnect-interval-ms", "4000", "--quic-cert", "a.crt", "--quic-key", "a.key", "--quic-ca", "ca.crt"};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 67;
        if (cfg.Router.Peers.size() != 2) return 68;
        if (cfg.Router.Peers[0].QuicReconnectIntervalMs != 4000) return 69;
        if (cfg.Router.Peers[1].QuicReconnectIntervalMs != 5000) return 70;
    }
    {
        TqRouterConfig router;
        std::string err;
        if (Load(R"json({"version":1,"peers":[{"peer_id":"bad","quic_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001","quic_reconnect_interval_ms":0}]})json", router, err)) return 71;
        if (err.find("quic_reconnect_interval_ms") == std::string::npos) return 72;
    }
    {
        std::string file = WriteTempConfig(R"json({
            "tls":{"cert":"server.crt","key":"server.key","ca":"ca.crt"},
            "server":{
                "proto_listen":"0.0.0.0:4433",
                "allow_targets":["127.0.0.1/32","10.0.0.0/8"],
                "deny_targets":"192.168.0.0/16"
            },
            "compression":{"mode":"off"},
            "proto":{"disable_1rtt_encryption":true,"initrtt_ms":1},
            "tuning":{"mode":"custom"},
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
    }
    {
        std::string file = WriteTempConfig(R"json({
            "tls":{"cert":"client.crt","key":"client.key","ca":"ca.crt"},
            "proto":{"profile":"low-latency","connections":8,"reconnect_interval_ms":5000},
            "client":{"download_test":30,"handshake_threads":4},
            "trace":{"enabled":true,"interval_sec":2,"connect_on_start":true},
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
        if (cfg.QuicReconnectIntervalMs != 5000) return 96;
        if (cfg.SpeedTestMode != TqSpeedTestMode::Download) return 97;
        if (cfg.SpeedTestDurationSec != 30) return 98;
        if (cfg.QuicProfile != TqQuicProfile::LowLatency) return 99;
        if (cfg.HandshakeThreads != 4) return 100;
        if (!cfg.Trace || cfg.TraceIntervalSec != 2 || !cfg.TraceConnectOnStart) return 101;
    }
    {
        std::string file = WriteTempConfig(R"json({
            "tls":{"cert":"client.crt","key":"client.key","ca":"ca.crt"},
            "peers":[
                {"id":"a","proto_peer":"127.0.0.1:14444","socks_listen":"127.0.0.1:11001"},
                {"id":"b","proto_peer":"127.0.0.1:14445","socks_listen":"127.0.0.1:11002","proto_reconnect_interval_ms":5000}
            ],
            "proto":{"reconnect_interval_ms":4000}
        })json");
        const char* args[] = {"tcpquic-proxy", "client", "--config", file.c_str()};
        TqConfig cfg;
        std::string err;
        if (!Parse((int)(sizeof(args) / sizeof(args[0])), const_cast<char**>(args), cfg, err)) return 102;
        if (cfg.Router.Peers.size() != 2) return 103;
        if (cfg.Router.Peers[0].QuicReconnectIntervalMs != 4000) return 106;
        if (cfg.Router.Peers[1].QuicReconnectIntervalMs != 5000) return 107;
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
