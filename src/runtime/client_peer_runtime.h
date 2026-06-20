#pragma once

#include "config.h"

#include <string>

inline TqPeerConfig TqMakePrimaryPeerConfig(const TqConfig& cfg) {
    TqPeerConfig peer;
    peer.PeerId = "primary";
    peer.Enabled = true;
    peer.QuicPeer = cfg.QuicPeer;
    peer.SocksListen = cfg.SocksListen;
    peer.HttpListen = cfg.HttpListen;
    peer.QuicConnections = cfg.QuicConnections;
    peer.Compress = cfg.Compress;
    return peer;
}

inline TqConfig TqMakePeerRuntimeConfig(const TqConfig& baseConfig, const TqPeerConfig& peer) {
    TqConfig cfg = baseConfig;
    cfg.ClientConfigPath.clear();
    cfg.AdminListen.clear();
    cfg.QuicPeer = peer.QuicPeer;
    cfg.SocksListen = peer.SocksListen;
    cfg.HttpListen = peer.HttpListen;
    cfg.QuicConnections = peer.QuicConnections == 0 ? baseConfig.QuicConnections : peer.QuicConnections;
    cfg.Compress = peer.Compress.empty() ? baseConfig.Compress : peer.Compress;
    return cfg;
}
