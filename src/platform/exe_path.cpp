#include "exe_path.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <unistd.h>
#elif defined(__linux__)
#include <limits.h>
#include <unistd.h>
#endif

#include <cstring>
#include <string>

namespace {

#if defined(__APPLE__)
constexpr size_t kExecutablePathMax = PROC_PIDPATHINFO_MAXSIZE;
#elif defined(_WIN32)
constexpr size_t kExecutablePathMax = MAX_PATH;
#elif defined(__linux__)
constexpr size_t kExecutablePathMax = PATH_MAX;
#else
constexpr size_t kExecutablePathMax = 4096;
#endif

bool ParentDirectoryFromPath(const char* executablePath, char* out, size_t outSize) {
    if (executablePath == nullptr || executablePath[0] == '\0' || out == nullptr || outSize == 0) {
        return false;
    }

    const char* slash = std::strrchr(executablePath, '/');
#if defined(_WIN32)
    const char* backslash = std::strrchr(executablePath, '\\');
    if (backslash != nullptr && (slash == nullptr || backslash > slash)) {
        slash = backslash;
    }
#endif
    if (slash == nullptr || slash == executablePath) {
        return false;
    }

    const size_t length = static_cast<size_t>(slash - executablePath);
    if (length + 1 > outSize) {
        return false;
    }
    std::memcpy(out, executablePath, length);
    out[length] = '\0';
    return true;
}

bool GetExecutablePath(char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) {
        return false;
    }

#if defined(_WIN32)
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }

    const int required =
        WideCharToMultiByte(CP_UTF8, 0, buffer, -1, out, static_cast<int>(outSize), nullptr, nullptr);
    return required > 0;
#elif defined(__APPLE__)
    const int len = proc_pidpath(getpid(), out, outSize);
    if (len <= 0) {
        return false;
    }
    if (static_cast<size_t>(len) >= outSize) {
        return false;
    }
    out[len] = '\0';
    return true;
#elif defined(__linux__)
    const ssize_t len = readlink("/proc/self/exe", out, outSize - 1);
    if (len <= 0) {
        return false;
    }
    out[len] = '\0';
    return true;
#else
    return false;
#endif
}

} // namespace

bool TqGetExecutableDirectory(char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) {
        return false;
    }

    char executablePath[kExecutablePathMax]{};
    if (!GetExecutablePath(executablePath, sizeof(executablePath))) {
        return false;
    }
    return ParentDirectoryFromPath(executablePath, out, outSize);
}

bool TqGetExecutableDirectory(std::string& out) {
    out.clear();
    char buffer[kExecutablePathMax]{};
    if (!TqGetExecutableDirectory(buffer, sizeof(buffer))) {
        return false;
    }
    out.assign(buffer);
    return !out.empty();
}
