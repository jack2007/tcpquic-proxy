#include "client_peer_runtime.h"
#include "client_ingress_reactor.h"
#include "speed_test.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

bool TqAttachServerSpeedControlStream(
    TqServerSpeedTestController&,
    MsQuicConnection*,
    MsQuicStream*,
    std::vector<uint8_t>,
    std::function<void()>) {
    return false;
}

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

static int TestConnectionStateCallbackDoesNotSynchronouslyOpenListeners() {
    TqConfig cfg{};
    cfg.Mode = TqMode::Client;
    cfg.QuicPeer = "127.0.0.1:4433";
    cfg.SocksListen = "127.0.0.1:0";
    cfg.HttpListen.clear();
    cfg.PortForwards.clear();
    cfg.QuicConnections = 1;

    TqClientIngressReactor ingress;
    if (!ingress.Start()) return 1701;

    auto runtime = std::make_shared<TqClientPeerRuntime>("peer-a", cfg, &ingress);
    std::string err;
    if (!runtime->EnableAcceptingAndApplyCurrentConnectionState(err, false)) {
        ingress.Stop();
        return 1702;
    }

    std::atomic<bool> blockerStarted{false};
    std::atomic<bool> releaseBlocker{false};
    if (!ingress.EnqueueDelayed(std::chrono::milliseconds(0), [&blockerStarted, &releaseBlocker]() {
            blockerStarted.store(true, std::memory_order_release);
            for (int i = 0; i < 500 && !releaseBlocker.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        })) {
        ingress.Stop();
        return 1705;
    }

    for (int i = 0; i < 50 && !blockerStarted.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!blockerStarted.load(std::memory_order_acquire)) {
        releaseBlocker.store(true, std::memory_order_release);
        ingress.Stop();
        return 1706;
    }

    const auto start = std::chrono::steady_clock::now();
    runtime->ScheduleConnectionStateApplyForTest(1);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed > std::chrono::milliseconds(100)) {
        releaseBlocker.store(true, std::memory_order_release);
        ingress.Stop();
        return 1703;
    }

    if (ingress.PeerCountForTest() != 0) {
        releaseBlocker.store(true, std::memory_order_release);
        runtime->StopAccepting();
        ingress.Stop();
        return 1707;
    }

    releaseBlocker.store(true, std::memory_order_release);
    for (int i = 0; i < 50 && ingress.PeerCountForTest() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (ingress.PeerCountForTest() != 1) {
        ingress.Stop();
        return 1704;
    }

    runtime->StopAccepting();
    ingress.Stop();
    return 0;
}

static int TestStopAcceptingDoesNotHangBehindPendingListenerApply() {
    TqConfig cfg{};
    cfg.Mode = TqMode::Client;
    cfg.QuicPeer = "127.0.0.1:4433";
    cfg.SocksListen = "127.0.0.1:0";
    cfg.HttpListen.clear();
    cfg.PortForwards.clear();
    cfg.QuicConnections = 1;

    TqClientIngressReactor ingress;
    if (!ingress.Start()) return 1801;

    auto runtime = std::make_shared<TqClientPeerRuntime>("peer-b", cfg, &ingress);
    std::string err;
    if (!runtime->EnableAcceptingAndApplyCurrentConnectionState(err, false)) {
        ingress.Stop();
        return 1802;
    }
    runtime->ScheduleConnectionStateApplyForTest(1);
    for (int i = 0; i < 50 && ingress.PeerCountForTest() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (ingress.PeerCountForTest() != 1) {
        ingress.Stop();
        return 1807;
    }

    std::atomic<bool> blockerStarted{false};
    std::atomic<bool> releaseBlocker{false};
    if (!ingress.EnqueueDelayed(std::chrono::milliseconds(0), [&blockerStarted, &releaseBlocker]() {
            blockerStarted.store(true, std::memory_order_release);
            for (int i = 0; i < 500 && !releaseBlocker.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        })) {
        ingress.Stop();
        return 1803;
    }

    for (int i = 0; i < 50 && !blockerStarted.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!blockerStarted.load(std::memory_order_acquire)) {
        releaseBlocker.store(true, std::memory_order_release);
        ingress.Stop();
        return 1804;
    }

    runtime->ScheduleConnectionStateApplyForTest(1);

    auto stopFinished = std::make_shared<std::atomic<bool>>(false);
    std::thread stopper([runtime, stopFinished]() {
        runtime->StopAccepting();
        stopFinished->store(true, std::memory_order_release);
    });

    releaseBlocker.store(true, std::memory_order_release);
    for (int i = 0; i < 50 && !stopFinished->load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!stopFinished->load(std::memory_order_acquire)) {
        stopper.detach();
        return 1805;
    }
    stopper.join();

    for (int i = 0; i < 50 && ingress.PeerCountForTest() != 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (ingress.PeerCountForTest() != 0) {
        runtime->StopAll();
        ingress.Stop();
        return 1806;
    }

    runtime->StopAll();
    ingress.Stop();
    return 0;
}

static int TestConcurrentAsyncOpenAndSyncApplyShareInFlightOpen() {
    TqConfig cfg{};
    cfg.Mode = TqMode::Client;
    cfg.QuicPeer = "127.0.0.1:4433";
    cfg.SocksListen = "127.0.0.1:0";
    cfg.HttpListen.clear();
    cfg.PortForwards.clear();
    cfg.QuicConnections = 1;

    TqClientIngressReactor ingress;
    if (!ingress.Start()) return 1901;

    auto runtime = std::make_shared<TqClientPeerRuntime>("peer-c", cfg, &ingress);
    std::string err;
    if (!runtime->EnableAcceptingAndApplyCurrentConnectionState(err, false)) {
        ingress.Stop();
        return 1902;
    }

    std::atomic<bool> hookStarted{false};
    std::atomic<bool> releaseHook{false};
    runtime->SetBeforeOpenIngressForTest([&hookStarted, &releaseHook]() {
        hookStarted.store(true, std::memory_order_release);
        for (int i = 0; i < 500 && !releaseHook.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    runtime->ScheduleConnectionStateApplyForTest(1);
    for (int i = 0; i < 50 && !hookStarted.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!hookStarted.load(std::memory_order_acquire)) {
        releaseHook.store(true, std::memory_order_release);
        runtime->SetBeforeOpenIngressForTest(nullptr);
        ingress.Stop();
        return 1903;
    }

    auto syncFinished = std::make_shared<std::atomic<bool>>(false);
    auto syncResult = std::make_shared<std::atomic<bool>>(false);
    std::thread syncApply([runtime, syncFinished, syncResult]() {
        std::string syncErr;
        syncResult->store(
            runtime->ApplyConnectionStateForTest(1, syncErr, false),
            std::memory_order_release);
        syncFinished->store(true, std::memory_order_release);
    });

    for (int i = 0; i < 5 && !syncFinished->load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (syncFinished->load(std::memory_order_acquire)) {
        releaseHook.store(true, std::memory_order_release);
        syncApply.join();
        runtime->SetBeforeOpenIngressForTest(nullptr);
        runtime->StopAccepting();
        ingress.Stop();
        return 1904;
    }
    if (ingress.PeerCountForTest() != 0) {
        releaseHook.store(true, std::memory_order_release);
        syncApply.join();
        runtime->SetBeforeOpenIngressForTest(nullptr);
        runtime->StopAccepting();
        ingress.Stop();
        return 1905;
    }

    releaseHook.store(true, std::memory_order_release);
    for (int i = 0; i < 50 && !syncFinished->load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!syncFinished->load(std::memory_order_acquire)) {
        syncApply.detach();
        runtime->SetBeforeOpenIngressForTest(nullptr);
        runtime->StopAccepting();
        ingress.Stop();
        return 1906;
    }
    syncApply.join();
    runtime->SetBeforeOpenIngressForTest(nullptr);

    if (!syncResult->load(std::memory_order_acquire)) {
        runtime->StopAccepting();
        ingress.Stop();
        return 1907;
    }
    if (ingress.PeerCountForTest() != 1) {
        runtime->StopAccepting();
        ingress.Stop();
        return 1908;
    }

    runtime->StopAccepting();
    ingress.Stop();
    return 0;
}

static int TestSnapshotPeerMetricsIncludesStreamTotals() {
    TqConfig cfg{};
    cfg.Mode = TqMode::Client;
    cfg.QuicPeer = "127.0.0.1:4433";
    cfg.SocksListen = "127.0.0.1:0";
    cfg.QuicConnections = 1;

    TqClientIngressReactor ingress;
    auto runtime = std::make_shared<TqClientPeerRuntime>("peer-streams", cfg, &ingress);
    runtime->IncrementTotalStreamsForTest(3);

    const TqPeerMetrics metrics = runtime->SnapshotPeerMetrics();
    if (metrics.PeerId != "peer-streams") return 2101;
    if (metrics.TotalStreams != 3) return 2102;
    if (metrics.ActiveStreams != 0) return 2103;
    return 0;
}

int main() {
    if (int rc = TestPrimaryPeerConfigUsesCliFields()) return rc;
    if (int rc = TestPeerConfigOverlayUsesPeerOverrides()) return rc;
    if (int rc = TestPeerConfigOverlayUsesBaseDefaults()) return rc;
    if (int rc = TestConnectionStateCallbackDoesNotSynchronouslyOpenListeners()) return rc;
    if (int rc = TestStopAcceptingDoesNotHangBehindPendingListenerApply()) return rc;
    if (int rc = TestConcurrentAsyncOpenAndSyncApplyShareInFlightOpen()) return rc;
    if (int rc = TestSnapshotPeerMetricsIncludesStreamTotals()) return rc;
    return 0;
}
