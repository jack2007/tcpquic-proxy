#include "crash_dump.h"

#include <chrono>
#include <cstdio>
#include <thread>

int main() {
    TqInstallCrashDumpHandler();
    std::fprintf(stderr, "probe: crashpad installed, waiting before crash\n");
    std::fflush(stderr);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::fprintf(stderr, "probe: about to crash\n");
    std::fflush(stderr);
    volatile int* p = nullptr;
    *p = 42;
    return 0;
}
