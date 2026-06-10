#include "exe_path.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

#include <filesystem>

namespace fs = std::filesystem;

bool TqGetExecutableDirectory(std::string& out) {
    out.clear();

#if defined(_WIN32)
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }
    try {
        out = fs::path(buffer).parent_path().string();
        return !out.empty();
    } catch (...) {
        return false;
    }
#else
    char buffer[PATH_MAX]{};
    const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len <= 0) {
        return false;
    }
    buffer[len] = '\0';
    try {
        out = fs::path(buffer).parent_path().string();
        return !out.empty();
    } catch (...) {
        return false;
    }
#endif
}
