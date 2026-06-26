#include "shutdown.h"

#include "trace.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace {

std::atomic<bool> g_interruptRequested{false};
std::atomic<bool> g_interruptLogged{false};

void LogInterruptReceived() {
    std::fprintf(stderr, "tcpquic-proxy: received interrupt (Ctrl+C), shutting down...\n");
    std::fflush(stderr);
    TqTraceLogLine("event=interrupt_received signal=ctrl+c");
}

#if defined(_WIN32)

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    switch (ctrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        if (g_interruptRequested.exchange(true, std::memory_order_release)) {
            std::fprintf(stderr, "tcpquic-proxy: second interrupt, forcing exit\n");
            std::fflush(stderr);
            std::_Exit(1);
        }
        return TRUE;
    default:
        return FALSE;
    }
}

#else

void SigIntHandler(int) {
    if (g_interruptRequested.exchange(true, std::memory_order_release)) {
        _exit(1);
    }
}

#endif

} // namespace

void TqInstallInterruptHandler() {
#if defined(_WIN32)
    if (!SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE)) {
        std::fprintf(stderr, "tcpquic-proxy: warning: failed to install Ctrl+C handler\n");
        std::fflush(stderr);
    }
#else
    struct sigaction action {};
    action.sa_handler = SigIntHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGINT, &action, nullptr) != 0) {
        std::fprintf(stderr, "tcpquic-proxy: warning: failed to install SIGINT handler\n");
        std::fflush(stderr);
    }
    struct sigaction termAction {};
    termAction.sa_handler = SigIntHandler;
    sigemptyset(&termAction.sa_mask);
    termAction.sa_flags = 0;
    (void)sigaction(SIGTERM, &termAction, nullptr);
#endif
}

void TqWaitForInterrupt() {
    while (!g_interruptRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!g_interruptLogged.exchange(true, std::memory_order_acq_rel)) {
        LogInterruptReceived();
    }
}
