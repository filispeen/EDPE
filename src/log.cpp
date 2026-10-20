#include "log.h"

#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <windows.h>

namespace {
HANDLE beginLogSession(const wchar_t* path) {
    const DWORD pid = GetCurrentProcessId();
    wchar_t mutex_name[64], marker_name[64];
    swprintf_s(mutex_name, L"Local\\EDPE_log_mutex_%lu", pid);
    swprintf_s(marker_name, L"Local\\EDPE_log_session_%lu", pid);
    const HANDLE mutex = CreateMutexW(nullptr, FALSE, mutex_name);
    if (!mutex) return nullptr;
    const DWORD wait = WaitForSingleObject(mutex, INFINITE);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        return nullptr;
    }
    HANDLE marker = CreateEventW(nullptr, TRUE, TRUE, marker_name);
    if (marker && GetLastError() != ERROR_ALREADY_EXISTS) {
        const HANDLE file = CreateFileW(path, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            CloseHandle(marker);
            marker = nullptr;
        } else {
            CloseHandle(file);
        }
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return marker; // Retained until process exit, including across proxy DLL unloads.
}

struct ProcessInfo {
    wchar_t exeName[MAX_PATH];
    DWORD pid;
    wchar_t logPath[MAX_PATH];
    bool valid;

    ProcessInfo() : pid(GetCurrentProcessId()), valid(false) {
        exeName[0] = L'\0';
        logPath[0] = L'\0';
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (length > 0 && length < MAX_PATH) {
            wchar_t* name = wcsrchr(path, L'\\');
            if (name) {
                wcscpy_s(exeName, name + 1);
                const size_t prefix = name + 1 - path;
                if (prefix + sizeof(L"edpe.log") / sizeof(wchar_t) <= MAX_PATH) {
                    wcscpy_s(name + 1, MAX_PATH - prefix, L"edpe.log");
                    wcscpy_s(logPath, path);
                    valid = true;
                }
            } else {
                wcscpy_s(exeName, path);
            }
        }
        if (exeName[0] == L'\0') {
            wcscpy_s(exeName, L"unknown");
        }
    }
};

const ProcessInfo& getProcessInfo() {
    static const ProcessInfo s_info;
    return s_info;
}
} // namespace

void EdpeLog(const wchar_t* message) {
    const auto& info = getProcessInfo();

    wchar_t formatted[2048];
    _snwprintf_s(formatted, sizeof(formatted) / sizeof(wchar_t), _TRUNCATE,
        L"[%ls:%lu] %ls", info.exeName, info.pid, message);

    OutputDebugStringW(formatted);
    OutputDebugStringW(L"\n");

    if (!info.valid) return;

    static const HANDLE session = beginLogSession(info.logPath);
    (void)session;

    char line[4096];
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, formatted, -1, line,
        static_cast<int>(sizeof(line) - 2), nullptr, nullptr);
    if (!bytes) return;
    line[bytes - 1] = '\r';
    line[bytes] = '\n';

    const HANDLE file = CreateFileW(info.logPath, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, line, bytes + 1, &written, nullptr);
    CloseHandle(file);
}
