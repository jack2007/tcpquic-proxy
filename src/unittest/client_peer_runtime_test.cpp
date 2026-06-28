#include "client_peer_runtime.h"

#include <string>

static int TestPrimaryPeerConfigUsesCliFields() {
    TqConfig cfg;
    cfg.QuicPeer = "10.0.0.1:4433";
    cfg.SocksListen = "127.0.0.1:1080";
    cfg.HttpListen = "127.0.0.1:18080";
    cfg.PortForwards.push_back(TqPortForwardConfig{"127.0.0.1:15432", "db.internal", 5432});
    cfg.QuicPaths.push_back(TqQuicPathConfig{"cmcc", "10.10.1.2", "36.1.1.10:443", 2});
    cfg.QuicPaths.push_back(TqQuicPathConfig{"ctcc", "10.20.1.2", "59.1.1.10:443", 1});
    cfg.QuicConnections = 4;
    cfg.Compress = "off";

    const TqPeerConfig peer = TqMakePrimaryPeerConfig(cfg);
    if (peer.PeerId != "primary") return 10;
    if (peer.QuicPeer != cfg.QuicPeer) return 11;
    if (peer.SocksListen != cfg.SocksListen) return 12;
    if (peer.HttpListen != cfg.HttpListen) return 13;
    if (peer.QuicConnections != cfg.QuicConnections) return 14;
    if (peer.Compress != cfg.Compress) return 15;
    if (!peer.Enabled) return 16;
    if (peer.PortForwards.size() != 1) return 17;
    if (peer.PortForwards[0].Listen != "127.0.0.1:15432") return 18;
    if (peer.QuicPaths.size() != 2) return 19;
    if (peer.QuicPaths[0].Name != "cmcc") return 101;
    if (peer.QuicPaths[1].Peer != "59.1.1.10:443") return 102;
    return 0;
}

static int TestPeerConfigOverlayUsesPeerOverrides() {
    TqConfig base;
    base.QuicConnections = 2;
    base.Compress = "zstd";

    TqPeerConfig peer;
    peer.PeerId = "agent-a";
    peer.QuicPeer = "10.0.0.2:4433";
    peer.SocksListen = "127.0.0.1:11001";
    peer.HttpListen = "127.0.0.1:18081";
    peer.PortForwards.push_back(TqPortForwardConfig{"127.0.0.1:15433", "cache.internal", 6379});
    peer.QuicPaths.push_back(TqQuicPathConfig{"cmcc", "10.10.1.2", "36.1.1.10:443", 2});
    peer.QuicPaths.push_back(TqQuicPathConfig{"ctcc", "10.20.1.2", "59.1.1.10:443", 1});
    peer.QuicConnections = 8;
    peer.Compress = "off";

    const TqConfig out = TqMakePeerRuntimeConfig(base, peer);
    if (out.QuicPeer != peer.QuicPeer) return 20;
    if (out.SocksListen != peer.SocksListen) return 21;
    if (out.HttpListen != peer.HttpListen) return 22;
    if (out.QuicConnections != peer.QuicConnections) return 23;
    if (out.Compress != peer.Compress) return 24;
    if (!out.ClientConfigPath.empty()) return 25;
    if (!out.AdminListen.empty()) return 26;
    if (out.PortForwards.size() != 1) return 27;
    if (out.PortForwards[0].TargetHost != "cache.internal") return 28;
    if (out.PortForwards[0].TargetPort != 6379) return 29;
    if (out.QuicPaths.size() != 2) return 103;
    if (out.QuicPaths[0].LocalAddress != "10.10.1.2") return 104;
    if (out.QuicPaths[1].Connections != 1) return 105;
    return 0;
}

static int TestPeerConfigOverlayUsesBaseDefaults() {
    TqConfig base;
    base.QuicConnections = 3;
    base.Compress = "zstd";

    TqPeerConfig peer;
    peer.PeerId = "agent-b";
    peer.QuicPeer = "10.0.0.3:4433";
    peer.SocksListen = "127.0.0.1:11002";

    const TqConfig out = TqMakePeerRuntimeConfig(base, peer);
    if (out.QuicConnections != base.QuicConnections) return 30;
    if (out.Compress != base.Compress) return 31;
    return 0;
}

int main() {
    if (int rc = TestPrimaryPeerConfigUsesCliFields()) return rc;
    if (int rc = TestPeerConfigOverlayUsesPeerOverrides()) return rc;
    if (int rc = TestPeerConfigOverlayUsesBaseDefaults()) return rc;
    return 0;
}
