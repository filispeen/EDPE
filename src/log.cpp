#include "log.h"

#include <cstddef>
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
}

void EdpeLog(const wchar_t* message) {
    OutputDebugStringW(message);
    OutputDebugStringW(L"\n");

    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return;
    wchar_t* name = wcsrchr(path, L'\\');
    if (!name) return;
    const size_t prefix = name + 1 - path;
    if (prefix + sizeof(L"edpe.log") / sizeof(wchar_t) > MAX_PATH) return;
    wcscpy_s(name + 1, MAX_PATH - prefix, L"edpe.log");
    static const HANDLE session = beginLogSession(path);
    (void)session;

    char line[1024];
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, message, -1, line,
        static_cast<int>(sizeof(line) - 2), nullptr, nullptr);
    if (!bytes) return;
    line[bytes - 1] = '\r';
    line[bytes] = '\n';

    const HANDLE file = CreateFileW(path, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(file, line, bytes + 1, &written, nullptr);
    CloseHandle(file);
}
