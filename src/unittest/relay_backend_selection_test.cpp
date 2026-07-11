#include "relay.h"
#include "linux_relay_worker.h"
#include "tuning.h"

#include <cassert>

int main() {
#if defined(__linux__)
    TqRelayHandle handle{};
    assert(handle.Backend == TqRelayBackendType::None);
    assert(handle.LinuxWorker == nullptr);
    assert(handle.LinuxRelayId == 0);
    assert(handle.LinuxWorkerIndex == 0);

    TqLinuxRelayWorkerConfig config{};
    config.WorkerIndex = 7;
    TqLinuxRelayWorker worker(config);
    handle.LinuxWorkerIndex = worker.WorkerIndex();
    assert(handle.LinuxWorkerIndex == 7);

    static_assert(static_cast<int>(TqRelayBackendType::None) == 0, "None backend must stay stable");
    static_assert(static_cast<int>(TqRelayBackendType::LinuxWorker) == 1, "Linux worker is the only production backend");

    assert(TqRelayLinuxFastPathEnabled(&handle) == false);
#endif
    return 0;
}
