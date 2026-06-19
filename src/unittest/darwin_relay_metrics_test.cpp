#if defined(__APPLE__)

#include "relay_metrics.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace {

bool Contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

void CheckContains(const std::string& text, const char* needle) {
    if (!Contains(text, needle)) {
        std::fprintf(stderr, "missing JSON fragment: %s\n", needle);
        std::abort();
    }
}

void Check(bool condition, const char* expression) {
    if (!condition) {
        std::fprintf(stderr, "check failed: %s\n", expression);
        std::abort();
    }
}

} // namespace

int main() {
    TqRelayMetricsSnapshot runtimeSnapshot = TqSnapshotRelayMetrics();
    Check(std::string(runtimeSnapshot.Backend) == "worker", "runtimeSnapshot.Backend == worker");

    TqRelayMetricsSnapshot metrics{};
    metrics.Backend = "worker";
    metrics.OutstandingQuicSendBytes = 11;
    metrics.TcpReadBatches = 22;
    metrics.TcpWriteBatches = 33;
    metrics.PendingBytes = 44;
    metrics.CurrentPendingQuicReceiveBytes = 55;
    metrics.PendingTcpWriteQueue = 66;
    metrics.PendingTcpWriteBytes = 77;
    metrics.QuicReceiveViewCount = 88;
    metrics.QuicReceiveViewBytes = 99;
    metrics.DeferredReceiveCompletes = 111;
    metrics.QuicSendBackpressureEvents = 222;

    std::ostringstream out;
    TqAppendRelayMetricsJson(out, metrics);
    const std::string json = out.str();
    CheckContains(json, "\"linux_relay_backend\":\"worker\"");
    CheckContains(json, "\"linux_relay_outstanding_quic_send_bytes\":11");
    CheckContains(json, "\"linux_relay_tcp_read_batches\":22");
    CheckContains(json, "\"linux_relay_tcp_write_batches\":33");
    CheckContains(json, "\"linux_relay_pending_bytes\":44");
    CheckContains(json, "\"linux_relay_current_pending_quic_receive_bytes\":55");
    CheckContains(json, "\"linux_relay_pending_tcp_write_queue\":66");
    CheckContains(json, "\"linux_relay_pending_tcp_write_bytes\":77");
    CheckContains(json, "\"linux_relay_quic_receive_view_count\":88");
    CheckContains(json, "\"linux_relay_quic_receive_view_bytes\":99");
    CheckContains(json, "\"linux_relay_deferred_receive_completes\":111");
    CheckContains(json, "\"linux_relay_quic_send_backpressure_events\":222");
    return 0;
}

#else
int main() { return 0; }
#endif
