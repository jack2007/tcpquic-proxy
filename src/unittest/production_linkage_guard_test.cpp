#include <fstream>
#include <sstream>
#include <string>

int main() {
    std::ifstream cmake("src/CMakeLists.txt");
    if (!cmake) return 1;
    std::ostringstream buffer;
    buffer << cmake.rdbuf();
    const std::string text = buffer.str();

    std::ifstream rootCmake("CMakeLists.txt");
    if (!rootCmake) return 18;
    std::ostringstream rootCmakeBuffer;
    rootCmakeBuffer << rootCmake.rdbuf();
    const std::string rootCmakeText = rootCmakeBuffer.str();
    if (rootCmakeText.find("CMAKE_BUILD_TYPE RelWithDebInfo") == std::string::npos) {
        return 19;
    }

    const size_t proxySources = text.find("set(TCPQUIC_PROXY_SOURCES");
    const size_t proxyTarget = text.find("add_tcpquic_executable(tcpquic-proxy");
    if (proxySources == std::string::npos || proxyTarget == std::string::npos) return 2;
    if (text.find("set_target_properties(tcpquic-proxy PROPERTIES OUTPUT_NAME raypx2)") ==
        std::string::npos) {
        return 13;
    }
    if (text.find("platform/crash_dump.cpp") == std::string::npos) {
        return 14;
    }

    const std::string productionBlock = text.substr(proxySources, proxyTarget - proxySources);
    if (productionBlock.find("relay_blocking_demo.cpp") != std::string::npos) return 3;
    if (productionBlock.find("tcp_write_queue.cpp") != std::string::npos) return 4;
    if (productionBlock.find("relay_buffer.cpp") == std::string::npos) return 5;
    if (productionBlock.find("relay_alloc.cpp") == std::string::npos) return 6;
    if (productionBlock.find("relay_buffer_pool.cpp") != std::string::npos) return 7;
#if defined(__linux__)
    if (productionBlock.find("linux_relay_worker.cpp") == std::string::npos) return 8;
#elif defined(_WIN32)
    if (productionBlock.find("windows_relay_worker.cpp") == std::string::npos) return 8;
#endif

    std::ifstream windowsRelayWorker("src/tunnel/windows_relay_worker.cpp");
    if (!windowsRelayWorker) return 9;
    std::ostringstream workerBuffer;
    workerBuffer << windowsRelayWorker.rdbuf();
    const std::string workerText = workerBuffer.str();
    if (workerText.find("EnqueueEvent(") != std::string::npos) return 10;
    if (workerText.find("DrainEvents(") != std::string::npos) return 11;
    if (workerText.find("ProcessRelayTask(") != std::string::npos) return 12;

    std::ifstream crashDump("src/platform/crash_dump.h");
    if (!crashDump) return 15;
    std::ostringstream crashDumpBuffer;
    crashDumpBuffer << crashDump.rdbuf();
    const std::string crashDumpText = crashDumpBuffer.str();
    if (crashDumpText.find("MiniDumpWriteDump") != std::string::npos) return 16;
    if (crashDumpText.find("dbghelp") != std::string::npos) return 17;

    std::ifstream crashDumpImpl("src/platform/crash_dump.cpp");
    if (!crashDumpImpl) return 20;
    std::ostringstream crashDumpImplBuffer;
    crashDumpImplBuffer << crashDumpImpl.rdbuf();
    const std::string crashDumpImplText = crashDumpImplBuffer.str();
    if (crashDumpImplText.find("MiniDumpWriteDump") != std::string::npos) return 21;
    if (crashDumpImplText.find("dbghelp") != std::string::npos) return 22;

    return 0;
}
