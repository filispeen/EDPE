#include "log.h"

#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <string>
#include <windows.h>

namespace {
std::mutex s_logMutex;

struct DedupeState {
    std::wstring lastMessage;
    int repeatCount = 0;
    LONGLONG lastRecordOffset = 0;
    LONGLONG expectedFileSize = 0;
};

DedupeState s_dedupe;

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
    if (!message) return;

    const auto& info = getProcessInfo();

    std::lock_guard<std::mutex> lock(s_logMutex);

    const bool isRepeat = (s_dedupe.repeatCount > 0 && s_dedupe.lastMessage == message);
    if (isRepeat && s_dedupe.repeatCount >= 11) {
        return;
    }

    wchar_t formatted[2048];
    if (isRepeat) {
        s_dedupe.repeatCount++;
        if (s_dedupe.repeatCount <= 10) {
            _snwprintf_s(formatted, sizeof(formatted) / sizeof(wchar_t), _TRUNCATE,
                L"[%ls:%lu] (X%d) %ls", info.exeName, info.pid, s_dedupe.repeatCount, message);
        } else {
            _snwprintf_s(formatted, sizeof(formatted) / sizeof(wchar_t), _TRUNCATE,
                L"[%ls:%lu] (X10+) %ls", info.exeName, info.pid, message);
        }
    } else {
        s_dedupe.lastMessage = message;
        s_dedupe.repeatCount = 1;
        _snwprintf_s(formatted, sizeof(formatted) / sizeof(wchar_t), _TRUNCATE,
            L"[%ls:%lu] %ls", info.exeName, info.pid, message);
    }

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
    const DWORD lineBytes = static_cast<DWORD>(bytes + 1);

    const HANDLE file = CreateFileW(info.logPath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER currentSize{};
    if (!GetFileSizeEx(file, &currentSize)) {
        CloseHandle(file);
        return;
    }

    if (isRepeat) {
        if (currentSize.QuadPart == s_dedupe.expectedFileSize &&
            s_dedupe.lastRecordOffset >= 0 &&
            s_dedupe.lastRecordOffset <= currentSize.QuadPart) {

            LARGE_INTEGER seekPos;
            seekPos.QuadPart = s_dedupe.lastRecordOffset;
            if (SetFilePointerEx(file, seekPos, nullptr, FILE_BEGIN)) {
                DWORD written = 0;
                if (WriteFile(file, line, lineBytes, &written, nullptr)) {
                    SetEndOfFile(file);
                    LARGE_INTEGER newSize{};
                    if (GetFileSizeEx(file, &newSize)) {
                        s_dedupe.expectedFileSize = newSize.QuadPart;
                    } else {
                        s_dedupe.expectedFileSize = s_dedupe.lastRecordOffset + written;
                    }
                    CloseHandle(file);
                    return;
                }
            }
        }

        s_dedupe.repeatCount = 1;
        _snwprintf_s(formatted, sizeof(formatted) / sizeof(wchar_t), _TRUNCATE,
            L"[%ls:%lu] %ls", info.exeName, info.pid, message);
        const int fallbackBytes = WideCharToMultiByte(CP_UTF8, 0, formatted, -1, line,
            static_cast<int>(sizeof(line) - 2), nullptr, nullptr);
        if (fallbackBytes) {
            line[fallbackBytes - 1] = '\r';
            line[fallbackBytes] = '\n';
            const DWORD fallbackLineBytes = static_cast<DWORD>(fallbackBytes + 1);

            LARGE_INTEGER seekEnd;
            seekEnd.QuadPart = 0;
            LARGE_INTEGER actualEnd{};
            if (SetFilePointerEx(file, seekEnd, &actualEnd, FILE_END)) {
                s_dedupe.lastRecordOffset = actualEnd.QuadPart;
                DWORD written = 0;
                if (WriteFile(file, line, fallbackLineBytes, &written, nullptr)) {
                    SetEndOfFile(file);
                    s_dedupe.expectedFileSize = s_dedupe.lastRecordOffset + written;
                }
            }
        }
        CloseHandle(file);
        return;
    }

    LARGE_INTEGER seekEnd;
    seekEnd.QuadPart = 0;
    LARGE_INTEGER actualEnd{};
    if (SetFilePointerEx(file, seekEnd, &actualEnd, FILE_END)) {
        s_dedupe.lastRecordOffset = actualEnd.QuadPart;
        DWORD written = 0;
        if (WriteFile(file, line, lineBytes, &written, nullptr)) {
            SetEndOfFile(file);
            s_dedupe.expectedFileSize = s_dedupe.lastRecordOffset + written;
        }
    }

    CloseHandle(file);
}
