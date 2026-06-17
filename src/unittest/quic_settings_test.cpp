#include "quic_session.h"
#include "tuning.h"

int main() {
    TqConfig cfg;
    cfg.TuningMode = TqTuningMode::Wan;
    TqComputeTuning(cfg, cfg.Tuning);

    if (cfg.QuicConnectionStreamCount != 1024) {
        return 10;
    }

    const MsQuicSettings clientSettings = TqMakeMsQuicSettings(cfg, false);
    if (clientSettings.IsSet.StreamMultiReceiveEnabled != TRUE ||
        clientSettings.StreamMultiReceiveEnabled != TRUE) {
        return 1;
    }
    if (clientSettings.IsSet.PeerBidiStreamCount != TRUE ||
        clientSettings.PeerBidiStreamCount != 1024) {
        return 11;
    }
    if (clientSettings.IsSet.NetStatsEventEnabled != TRUE ||
        clientSettings.NetStatsEventEnabled != TRUE) {
        return 14;
    }

    const MsQuicSettings serverSettings = TqMakeMsQuicSettings(cfg, true);
    if (serverSettings.IsSet.StreamMultiReceiveEnabled != TRUE ||
        serverSettings.StreamMultiReceiveEnabled != TRUE) {
        return 2;
    }
    if (serverSettings.IsSet.ServerResumptionLevel != TRUE) {
        return 3;
    }
    if (serverSettings.IsSet.PeerBidiStreamCount != TRUE ||
        serverSettings.PeerBidiStreamCount != 1024) {
        return 12;
    }
    if (serverSettings.IsSet.NetStatsEventEnabled != TRUE ||
        serverSettings.NetStatsEventEnabled != TRUE) {
        return 15;
    }

    cfg.QuicConnectionStreamCount = 2048;
    const MsQuicSettings overriddenSettings = TqMakeMsQuicSettings(cfg, false);
    if (overriddenSettings.IsSet.PeerBidiStreamCount != TRUE ||
        overriddenSettings.PeerBidiStreamCount != 2048) {
        return 13;
    }

    return 0;
}
