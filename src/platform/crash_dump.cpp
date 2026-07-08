#include "crash_dump.h"

#include "exe_path.h"

#include <cstdio>
#include <filesystem>
#include <string>

#ifndef TCPQUIC_HAS_CRASHPAD
#define TCPQUIC_HAS_CRASHPAD 0
#endif

#if TCPQUIC_HAS_CRASHPAD
#include "client/crashpad_client.h"
#include "base/files/file_path.h"

#include <map>
#include <memory>
#include <vector>
#endif

namespace {

#if TCPQUIC_HAS_CRASHPAD

std::filesystem::path TqExecutableDirectoryPath() {
    std::string exeDir;
    if (!TqGetExecutableDirectory(exeDir)) {
        return {};
    }
    return std::filesystem::u8path(exeDir);
}

base::FilePath TqCrashpadFilePath(const std::filesystem::path& path) {
    return base::FilePath(path.native());
}

std::filesystem::path TqCrashpadHandlerPath(const std::filesystem::path& exeDir) {
#if defined(_WIN32)
    return exeDir / "crashpad_handler.exe";
#else
    return exeDir / "crashpad_handler";
#endif
}

bool TqStartCrashpad() {
    const std::filesystem::path exeDir = TqExecutableDirectoryPath();
    if (exeDir.empty()) {
        std::fprintf(stderr, "raypx2: crashpad init failed: executable directory unavailable\n");
        return false;
    }

    const std::filesystem::path handler = TqCrashpadHandlerPath(exeDir);
    const std::filesystem::path database = exeDir / "crashpad";
    const std::filesystem::path metrics = database / "metrics";

    std::error_code ec;
    std::filesystem::create_directories(database, ec);
    if (ec) {
        std::fprintf(stderr,
            "raypx2: crashpad init failed: cannot create database '%s': %s\n",
            database.u8string().c_str(),
            ec.message().c_str());
        return false;
    }
    std::filesystem::create_directories(metrics, ec);
    if (ec) {
        std::fprintf(stderr,
            "raypx2: crashpad init failed: cannot create metrics directory '%s': %s\n",
            metrics.u8string().c_str(),
            ec.message().c_str());
        return false;
    }
    if (!std::filesystem::exists(handler)) {
        std::fprintf(stderr,
            "raypx2: crashpad init failed: handler not found at '%s'\n",
            handler.u8string().c_str());
        return false;
    }

    static std::unique_ptr<crashpad::CrashpadClient> client;
    if (client) {
        return true;
    }

    auto nextClient = std::make_unique<crashpad::CrashpadClient>();
    const std::map<std::string, std::string> annotations{
        {"prod", "raypx2"},
        {"ver", "local"},
    };
    const std::vector<std::string> arguments;

    const bool started = nextClient->StartHandler(
        TqCrashpadFilePath(handler),
        TqCrashpadFilePath(database),
        TqCrashpadFilePath(metrics),
        std::string(),
        annotations,
        arguments,
        true,
        false);
    if (!started) {
        std::fprintf(stderr, "raypx2: crashpad init failed: StartHandler returned false\n");
        return false;
    }

    client = std::move(nextClient);
    std::fprintf(stderr,
        "raypx2: crashpad enabled (database=%s)\n",
        database.u8string().c_str());
    return true;
}

#else

void TqWarnCrashpadDisabledOnce() {
    static bool warned = false;
    if (warned) {
        return;
    }
    warned = true;
    std::fprintf(stderr, "raypx2: crashpad disabled; local crash dumps are not available\n");
}

#endif

} // namespace

void TqInstallCrashDumpHandler() {
#if TCPQUIC_HAS_CRASHPAD
    if (!TqStartCrashpad()) {
        std::fprintf(stderr, "raypx2: crashpad unavailable; no fallback crash handler is installed\n");
    }
#else
    TqWarnCrashpadDisabledOnce();
#endif
}
