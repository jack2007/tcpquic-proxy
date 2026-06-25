#include "relay_metrics.h"

TqRelayMetricsSnapshot TqSnapshotRelayMetrics() {
    TqRelayMetricsSnapshot metrics{};
    metrics.Backend = "stub";
    return metrics;
}

std::vector<TqRelayActiveSnapshot> TqSnapshotActiveRelays() {
    return {};
}

void TqAppendRelayMetricsJson(std::ostringstream&, const TqRelayMetricsSnapshot&) {
}

void TqAppendJsonString(std::ostringstream&, const char*, const std::string&) {
}
