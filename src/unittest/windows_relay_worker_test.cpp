#include "platform_socket.h"
#include "windows_relay_worker.h"

#include <cassert>

int main() {
#if defined(_WIN32)
    TqSocketStartup startup;
    assert(startup.Ok());

    TqWindowsRelayWorker worker;
    assert(worker.Start());
    worker.Stop();
    worker.Stop();
#endif
    return 0;
}
