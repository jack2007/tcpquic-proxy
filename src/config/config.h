#pragma once

#include "tuning.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

enum class TqMode { Client, Server };

enum class TqSpeedTestMode {
    None,
    Download,
    DownloadSink,
    Upload,
};

enum class TqQuicProfile { MaxThroughput, LowLatency };

struct TqPeerConfig {
    std::string PeerId;
    std::string QuicPeer;
    std::string SocksListen;
    std::string HttpListen;
    uint32_t QuicConnections{0};
    std::string Compress;
    bool Enabled{true};
};

struct TqProxyAuthUser {
    std::string Username;
    std::string Password;
};

struct TqRouterConfig {
    uint32_t Version{1};
    std::vector<TqProxyAuthUser> ProxyAuth;
    std::vector<TqPeerConfig> Peers;
};

struct TqConfig {
    TqMode Mode{};
    std::string SocksListen = "127.0.0.1:1080";
    std::string HttpListen = "127.0.0.1:8080";
    std::string QuicPeer;
    std::string ConfigPath;
    std::string ClientConfigPath;
    std::string AdminListen;
    TqRouterConfig Router{};
    std::string QuicListen;
    std::string QuicCert;
    std::string QuicKey;
    std::string QuicCa;
    uint32_t QuicConnections = 1;
    uint32_t QuicConnectionStreamCount = 1024;
    uint32_t QuicKeepAliveIntervalMs{5000};
    TqSpeedTestMode SpeedTestMode{TqSpeedTestMode::None};
    uint32_t SpeedTestDurationSec{0};
    TqQuicProfile QuicProfile{TqQuicProfile::MaxThroughput};
    bool QuicDisable1RttEncryption{true};
    uint32_t HandshakeThreads = 8;
    std::string Compress = "off";
    int CompressLevel = 1;
    std::vector<std::string> AllowTargets;
    std::vector<std::string> DenyTargets;

    TqTuningMode TuningMode{TqTuningMode::Wan};
    uint32_t TargetBandwidthMbps{0};
    uint32_t TargetRttMs{0};
    uint32_t MaxMemoryMb{0};
    uint32_t TuningOverrideRelayIoSize{0};
    uint32_t TuningOverrideRelayInflightBytes{0};
    uint32_t TuningOverrideLinuxRelayReadChunkSize{0};
    uint32_t TuningOverrideLinuxRelayTcpWriteMaxBytes{0};
    uint32_t TuningOverrideLinuxRelayTcpWriteBurstBytes{0};
    uint32_t TuningOverrideInitialQuicReadAheadBytes{0};
    uint32_t TuningOverrideQuicFcw{0};
    uint32_t TuningOverrideQuicSrw{0};
    uint32_t TuningOverrideQuicIw{0};
    uint32_t TuningOverrideQuicInitRttMs{0};
    TqTuningConfig Tuning{};

    bool Trace{false};
    uint32_t TraceIntervalSec{10};
    bool TraceConnectOnStart{false};
    bool DiagStats{false};
    uint32_t DiagStatsIntervalSec{5};
};

void TqPrintUsage(FILE* out);
bool TqParseArgs(int argc, char** argv, TqConfig& cfg, std::string& err);
bool TqLoadClientConfig(const std::string& path, TqRouterConfig& router, std::string& err);
bool TqValidateRouterConfig(const TqRouterConfig& router, std::string& err);
void TqFinalizeConfig(TqConfig& cfg);
