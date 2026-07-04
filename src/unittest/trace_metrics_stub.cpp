#include "relay_metrics.h"

TqRelayMetricsSnapshot TqSnapshotRelayMetrics() {
    TqRelayMetricsSnapshot metrics{};
    metrics.Backend = "stub";
    return metrics;
}

std::vector<TqRelayActiveSnapshot> TqSnapshotActiveRelays() {
    return {};
}

std::string TqRelayMetricsFieldsJson(const TqRelayMetricsSnapshot&) {
    return "{}";
}
