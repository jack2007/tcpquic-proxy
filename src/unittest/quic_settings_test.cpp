#include "quic_session.h"
#include "tuning.h"

int main() {
    TqConfig cfg;
    cfg.TuningMode = TqTuningMode::Wan;
    TqComputeTuning(cfg, cfg.Tuning);

    const MsQuicSettings clientSettings = TqMakeMsQuicSettings(cfg, false);
    if (clientSettings.IsSet.StreamMultiReceiveEnabled != TRUE ||
        clientSettings.StreamMultiReceiveEnabled != TRUE) {
        return 1;
    }

    const MsQuicSettings serverSettings = TqMakeMsQuicSettings(cfg, true);
    if (serverSettings.IsSet.StreamMultiReceiveEnabled != TRUE ||
        serverSettings.StreamMultiReceiveEnabled != TRUE) {
        return 2;
    }
    if (serverSettings.IsSet.ServerResumptionLevel != TRUE) {
        return 3;
    }

    return 0;
}
