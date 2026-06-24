#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <cwchar>

namespace tq_crash_dump {

constexpr wchar_t kDumpDirectoryName[] = L"dumps";
constexpr wchar_t kDumpFilePrefix[] = L"tcpquic-proxy";

inline bool IsCrashExceptionCode(DWORD exceptionCode) {
    return exceptionCode == EXCEPTION_ACCESS_VIOLATION ||
        exceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION ||
        exceptionCode == EXCEPTION_STACK_OVERFLOW ||
        exceptionCode == EXCEPTION_ARRAY_BOUNDS_EXCEEDED ||
        exceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO;
}

inline bool MakeDirectoryIfNeeded(const wchar_t* path) {
    if (path == nullptr || path[0] == L'\0') {
        return false;
    }
    if (::CreateDirectoryW(path, nullptr)) {
        return true;
    }
    return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

inline bool BuildDumpPathInDirectory(
    const wchar_t* baseDirectory,
    DWORD processId,
    DWORD exceptionCode,
    wchar_t* out,
    size_t outCount) {
    if (baseDirectory == nullptr || baseDirectory[0] == L'\0' || out == nullptr || outCount == 0) {
        return false;
    }

    wchar_t dumpDirectory[MAX_PATH]{};
    const int dirWritten =
        std::swprintf(dumpDirectory, MAX_PATH, L"%s\\%s", baseDirectory, kDumpDirectoryName);
    if (dirWritten <= 0 || dirWritten >= MAX_PATH || !MakeDirectoryIfNeeded(dumpDirectory)) {
        return false;
    }

    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    const int pathWritten = std::swprintf(
        out,
        outCount,
        L"%s\\%s-%04u%02u%02u-%02u%02u%02u-%lu-0x%08lx.dmp",
        dumpDirectory,
        kDumpFilePrefix,
        now.wYear,
        now.wMonth,
        now.wDay,
        now.wHour,
        now.wMinute,
        now.wSecond,
        static_cast<unsigned long>(processId),
        static_cast<unsigned long>(exceptionCode));
    return pathWritten > 0 && static_cast<size_t>(pathWritten) < outCount;
}

inline bool GetExecutableDirectory(wchar_t* out, size_t outCount) {
    if (out == nullptr || outCount == 0) {
        return false;
    }
    wchar_t exePath[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }
    wchar_t* slash = std::wcsrchr(exePath, L'\\');
    if (slash == nullptr || slash == exePath) {
        return false;
    }
    *slash = L'\0';
    const size_t directoryLength = std::wcslen(exePath);
    if (directoryLength + 1 > outCount) {
        return false;
    }
    return wcscpy_s(out, outCount, exePath) == 0;
}

inline bool BuildCrashDumpPath(DWORD processId, DWORD exceptionCode, wchar_t* out, size_t outCount) {
    wchar_t cwd[MAX_PATH]{};
    const DWORD cwdLength = ::GetCurrentDirectoryW(MAX_PATH, cwd);
    if (cwdLength != 0 && cwdLength < MAX_PATH &&
        BuildDumpPathInDirectory(cwd, processId, exceptionCode, out, outCount)) {
        return true;
    }

    wchar_t exeDirectory[MAX_PATH]{};
    return GetExecutableDirectory(exeDirectory, MAX_PATH) &&
        BuildDumpPathInDirectory(exeDirectory, processId, exceptionCode, out, outCount);
}

inline void BuildSidecarPath(const wchar_t* dumpPath, const wchar_t* extension, wchar_t* out, size_t outCount) {
    if (out == nullptr || outCount == 0) {
        return;
    }
    out[0] = L'\0';
    if (dumpPath == nullptr || dumpPath[0] == L'\0' || extension == nullptr) {
        return;
    }
    const int written = std::swprintf(out, outCount, L"%s.%s", dumpPath, extension);
    if (written <= 0 || static_cast<size_t>(written) >= outCount) {
        out[0] = L'\0';
    }
}

inline void WriteCrashDumpStatus(
    const wchar_t* dumpPath,
    DWORD exceptionCode,
    bool dumpAttempted,
    bool dumpWritten,
    DWORD errorCode) {
    wchar_t statusPath[MAX_PATH]{};
    BuildSidecarPath(dumpPath, L"txt", statusPath, MAX_PATH);
    if (statusPath[0] == L'\0') {
        return;
    }

    HANDLE file = ::CreateFileW(
        statusPath,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    char buffer[1024]{};
    const int length = std::snprintf(
        buffer,
        sizeof(buffer),
        "tcpquic-proxy crash dump status\n"
        "exception=0x%08lx\n"
        "dump_path=%ls\n"
        "dump_attempted=%d\n"
        "dump_written=%d\n"
        "last_error=%lu\n",
        static_cast<unsigned long>(exceptionCode),
        dumpPath == nullptr ? L"" : dumpPath,
        dumpAttempted ? 1 : 0,
        dumpWritten ? 1 : 0,
        static_cast<unsigned long>(errorCode));
    if (length > 0) {
        DWORD written = 0;
        (void)::WriteFile(file, buffer, static_cast<DWORD>(length), &written, nullptr);
    }
    ::CloseHandle(file);
}

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(
    HANDLE,
    DWORD,
    HANDLE,
    MINIDUMP_TYPE,
    const PMINIDUMP_EXCEPTION_INFORMATION,
    const PMINIDUMP_USER_STREAM_INFORMATION,
    const PMINIDUMP_CALLBACK_INFORMATION);

inline bool TryWriteCrashDump(EXCEPTION_POINTERS* exceptionPointers) {
    const DWORD processId = ::GetCurrentProcessId();
    const DWORD exceptionCode =
        exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr
            ? exceptionPointers->ExceptionRecord->ExceptionCode
            : 0;

    wchar_t dumpPath[MAX_PATH]{};
    if (!BuildCrashDumpPath(processId, exceptionCode, dumpPath, MAX_PATH)) {
        return false;
    }

    HMODULE dbghelp = ::LoadLibraryW(L"dbghelp.dll");
    if (dbghelp == nullptr) {
        WriteCrashDumpStatus(dumpPath, exceptionCode, false, false, ::GetLastError());
        return false;
    }

    auto* writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(
        ::GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (writeDump == nullptr) {
        const DWORD error = ::GetLastError();
        ::FreeLibrary(dbghelp);
        WriteCrashDumpStatus(dumpPath, exceptionCode, false, false, error);
        return false;
    }

    bool dumpWritten = false;
    DWORD errorCode = ERROR_SUCCESS;
    HANDLE file = ::CreateFileW(
        dumpPath,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
        exceptionInfo.ThreadId = ::GetCurrentThreadId();
        exceptionInfo.ExceptionPointers = exceptionPointers;
        exceptionInfo.ClientPointers = FALSE;

        const auto dumpType = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithFullMemory |
            MiniDumpWithHandleData |
            MiniDumpWithThreadInfo |
            MiniDumpWithUnloadedModules);
        dumpWritten = writeDump(
            ::GetCurrentProcess(),
            processId,
            file,
            dumpType,
            exceptionPointers == nullptr ? nullptr : &exceptionInfo,
            nullptr,
            nullptr) != FALSE;
        if (!dumpWritten) {
            errorCode = ::GetLastError();
        }
        ::CloseHandle(file);
    } else {
        errorCode = ::GetLastError();
    }

    ::FreeLibrary(dbghelp);
    WriteCrashDumpStatus(dumpPath, exceptionCode, true, dumpWritten, errorCode);
    return dumpWritten;
}

inline LONG CALLBACK VectoredExceptionHandler(EXCEPTION_POINTERS* exceptionPointers) {
    const DWORD exceptionCode =
        exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr
            ? exceptionPointers->ExceptionRecord->ExceptionCode
            : 0;
    if (IsCrashExceptionCode(exceptionCode)) {
        (void)TryWriteCrashDump(exceptionPointers);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

inline LONG WINAPI UnhandledExceptionFilter(EXCEPTION_POINTERS* exceptionPointers) {
    (void)TryWriteCrashDump(exceptionPointers);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace tq_crash_dump

inline void TqInstallCrashDumpHandler() {
    (void)::AddVectoredExceptionHandler(1, tq_crash_dump::VectoredExceptionHandler);
    ::SetUnhandledExceptionFilter(tq_crash_dump::UnhandledExceptionFilter);
}

#else

inline void TqInstallCrashDumpHandler() {}

#endif
