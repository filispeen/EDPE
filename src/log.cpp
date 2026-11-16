#include "log.h"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <string>
#include <windows.h>

namespace {
constexpr size_t kLogRingLines = 50;
constexpr size_t kLogRingLineLength = 512;
static char s_logRing[kLogRingLines][kLogRingLineLength];
static std::atomic<uint32_t> s_logRingWriteIdx{0};

struct DedupeState {
    std::wstring lastMessage;
    int repeatCount = 0;
};

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

struct AsyncLogState {
    CRITICAL_SECTION queueCs{};
    CRITICAL_SECTION fileCs{};
    HANDLE wakeEvent = nullptr;
    HANDLE flushedEvent = nullptr;
    HANDLE workerThread = nullptr;
    HANDLE sessionMarker = nullptr;
    HANDLE logFile = INVALID_HANDLE_VALUE;
    std::string* pendingBytes = nullptr;
    std::wstring* pendingDebug = nullptr;
    DedupeState* dedupe = nullptr;
    uint64_t queuedSeq = 0;
    uint64_t flushedSeq = 0;
    bool shuttingDown = false;
};

INIT_ONCE s_stateOnce = INIT_ONCE_STATIC_INIT;
AsyncLogState* s_state = nullptr;

BOOL CALLBACK initLogStateOnce(PINIT_ONCE, PVOID, PVOID*) {
    auto* state = new AsyncLogState();
    InitializeCriticalSectionAndSpinCount(&state->queueCs, 200);
    InitializeCriticalSectionAndSpinCount(&state->fileCs, 200);
    state->wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    state->flushedEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    state->pendingBytes = new std::string();
    state->pendingDebug = new std::wstring();
    state->dedupe = new DedupeState();
    s_state = state;
    return TRUE;
}

AsyncLogState* getLogState() {
    InitOnceExecuteOnce(&s_stateOnce, initLogStateOnce, nullptr, nullptr);
    return s_state;
}

void pushRingEntry(const wchar_t* formatted) {
    char ringBuf[kLogRingLineLength];
    const int ringBytes = WideCharToMultiByte(CP_UTF8, 0, formatted, -1, ringBuf,
        static_cast<int>(sizeof(ringBuf)), nullptr, nullptr);
    if (ringBytes > 0) {
        const uint32_t ringIdx = s_logRingWriteIdx.fetch_add(1, std::memory_order_relaxed);
        memcpy(s_logRing[ringIdx % kLogRingLines], ringBuf, static_cast<size_t>(ringBytes));
    }
}

void enqueueFormattedLocked(AsyncLogState* state, const ProcessInfo& info, const wchar_t* formatted) {
    pushRingEntry(formatted);

    if (IsDebuggerPresent()) {
        state->pendingDebug->append(formatted);
        state->pendingDebug->push_back(L'\n');
    }

    if (!info.valid) return;

    char line[4096];
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, formatted, -1, line,
        static_cast<int>(sizeof(line) - 2), nullptr, nullptr);
    if (!bytes) return;
    line[bytes - 1] = '\r';
    line[bytes] = '\n';
    state->pendingBytes->append(line, static_cast<size_t>(bytes + 1));
    ++state->queuedSeq;
    if (state->flushedEvent) {
        ResetEvent(state->flushedEvent);
    }
}

void flushRepeatSummaryLocked(AsyncLogState* state, const ProcessInfo& info) {
    if (!state->dedupe || state->dedupe->repeatCount <= 1) {
        return;
    }
    wchar_t formatted[2048];
    _snwprintf_s(formatted, sizeof(formatted) / sizeof(wchar_t), _TRUNCATE,
        L"[%ls:%lu] (X%d) %ls", info.exeName, info.pid,
        state->dedupe->repeatCount, state->dedupe->lastMessage.c_str());
    state->dedupe->repeatCount = 0;
    enqueueFormattedLocked(state, info, formatted);
}

void ensureLogHandleOpenLocked(AsyncLogState* state, const ProcessInfo& info) {
    if (!info.valid) return;
    if (!state->sessionMarker) {
        state->sessionMarker = beginLogSession(info.logPath);
    }
    if (state->logFile != INVALID_HANDLE_VALUE) return;

    HANDLE file = CreateFileW(info.logPath, FILE_APPEND_DATA | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        file = CreateFileW(info.logPath, GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER zero{};
            SetFilePointerEx(file, zero, nullptr, FILE_END);
        }
    }
    state->logFile = file;
}

void writeBatchToDisk(AsyncLogState* state, const ProcessInfo& info,
                      const std::string& bytes, const std::wstring& debugText) {
    if (!debugText.empty()) {
        OutputDebugStringW(debugText.c_str());
    }
    if (bytes.empty() || !info.valid) return;

    EnterCriticalSection(&state->fileCs);
    ensureLogHandleOpenLocked(state, info);
    if (state->logFile != INVALID_HANDLE_VALUE) {
        const char* ptr = bytes.data();
        size_t remaining = bytes.size();
        while (remaining > 0) {
            const DWORD chunk = (remaining > 0x7FFFFFFFu) ? 0x7FFFFFFFu : static_cast<DWORD>(remaining);
            DWORD written = 0;
            if (!WriteFile(state->logFile, ptr, chunk, &written, nullptr) || written == 0) {
                CloseHandle(state->logFile);
                state->logFile = INVALID_HANDLE_VALUE;
                ensureLogHandleOpenLocked(state, info);
                if (state->logFile == INVALID_HANDLE_VALUE ||
                    !WriteFile(state->logFile, ptr, chunk, &written, nullptr) || written == 0) {
                    break;
                }
            }
            ptr += written;
            remaining -= written;
        }
    }
    LeaveCriticalSection(&state->fileCs);
}

DWORD WINAPI logWorkerProc(LPVOID param) {
    auto* state = static_cast<AsyncLogState*>(param);
    const auto& info = getProcessInfo();
    std::string batchBytes;
    std::wstring batchDebug;

    for (;;) {
        if (state->wakeEvent) {
            WaitForSingleObject(state->wakeEvent, INFINITE);
        }

        for (;;) {
            batchBytes.clear();
            batchDebug.clear();
            bool shouldExit = false;
            uint64_t targetSeq = 0;

            EnterCriticalSection(&state->queueCs);
            if (!state->pendingBytes->empty()) {
                batchBytes.swap(*state->pendingBytes);
            }
            if (!state->pendingDebug->empty()) {
                batchDebug.swap(*state->pendingDebug);
            }
            targetSeq = state->queuedSeq;
            shouldExit = state->shuttingDown;
            LeaveCriticalSection(&state->queueCs);

            if (!batchBytes.empty() || !batchDebug.empty()) {
                writeBatchToDisk(state, info, batchBytes, batchDebug);
                EnterCriticalSection(&state->queueCs);
                if (targetSeq > state->flushedSeq) {
                    state->flushedSeq = targetSeq;
                }
                const bool caughtUp = state->pendingBytes->empty() &&
                                      state->pendingDebug->empty() &&
                                      state->flushedSeq >= state->queuedSeq;
                shouldExit = state->shuttingDown;
                if (caughtUp && state->flushedEvent) {
                    SetEvent(state->flushedEvent);
                }
                LeaveCriticalSection(&state->queueCs);
                if (!caughtUp) continue;
                if (!shouldExit) break;
            } else {
                EnterCriticalSection(&state->queueCs);
                if (state->flushedSeq < targetSeq) state->flushedSeq = targetSeq;
                const bool stillEmpty = state->pendingBytes->empty() && state->pendingDebug->empty();
                shouldExit = state->shuttingDown;
                if (stillEmpty && state->flushedEvent) {
                    SetEvent(state->flushedEvent);
                }
                LeaveCriticalSection(&state->queueCs);
                if (!stillEmpty) continue;
                if (!shouldExit) break;
            }

            EnterCriticalSection(&state->fileCs);
            if (state->logFile != INVALID_HANDLE_VALUE) {
                CloseHandle(state->logFile);
                state->logFile = INVALID_HANDLE_VALUE;
            }
            LeaveCriticalSection(&state->fileCs);
            return 0;
        }
    }
}

void ensureWorkerStartedLocked(AsyncLogState* state) {
    if (!state->workerThread && !state->shuttingDown) {
        state->workerThread = CreateThread(nullptr, 0, logWorkerProc, state, 0, nullptr);
    }
}
} // namespace

void EdpeLog(const wchar_t* message) {
    if (!message) return;

    const auto& info = getProcessInfo();
    AsyncLogState* state = getLogState();

    wchar_t formatted[2048];
    bool wakeWorker = false;

    EnterCriticalSection(&state->queueCs);
    const bool isRepeat = (state->dedupe->repeatCount > 0 && state->dedupe->lastMessage == message);
    if (isRepeat) {
        ++state->dedupe->repeatCount;
        LeaveCriticalSection(&state->queueCs);
        return;
    }

    flushRepeatSummaryLocked(state, info);
    state->dedupe->lastMessage = message;
    state->dedupe->repeatCount = 1;
    _snwprintf_s(formatted, sizeof(formatted) / sizeof(wchar_t), _TRUNCATE,
        L"[%ls:%lu] %ls", info.exeName, info.pid, message);

    enqueueFormattedLocked(state, info, formatted);
    if (!state->shuttingDown) {
        ensureWorkerStartedLocked(state);
        wakeWorker = (state->workerThread != nullptr);
    }
    LeaveCriticalSection(&state->queueCs);

    if (wakeWorker && state->wakeEvent) {
        SetEvent(state->wakeEvent);
    }
}

void EdpeLogFlush() {
    AsyncLogState* state = getLogState();
    const auto& info = getProcessInfo();

    HANDLE thread = nullptr;
    EnterCriticalSection(&state->queueCs);
    flushRepeatSummaryLocked(state, info);
    thread = state->workerThread;
    const bool hasPending = !state->pendingBytes->empty() ||
                            !state->pendingDebug->empty() ||
                            (state->flushedSeq < state->queuedSeq);
    LeaveCriticalSection(&state->queueCs);

    if (thread && hasPending) {
        if (state->wakeEvent) SetEvent(state->wakeEvent);
        if (state->flushedEvent &&
            WaitForSingleObject(state->flushedEvent, 250) == WAIT_OBJECT_0) {
            return;
        }
    }

    // Fallback direct write when no worker thread exists (or wait timed out during crash)
    std::string batchBytes;
    std::wstring batchDebug;
    if (TryEnterCriticalSection(&state->queueCs)) {
        batchBytes.swap(*state->pendingBytes);
        batchDebug.swap(*state->pendingDebug);
        if (state->flushedEvent) SetEvent(state->flushedEvent);
        LeaveCriticalSection(&state->queueCs);
    }
    if (!batchBytes.empty() || !batchDebug.empty()) {
        writeBatchToDisk(state, info, batchBytes, batchDebug);
    }
}

void EdpeLogShutdown(bool waitForThread) {
    AsyncLogState* state = getLogState();
    const auto& info = getProcessInfo();

    HANDLE thread = nullptr;
    EnterCriticalSection(&state->queueCs);
    flushRepeatSummaryLocked(state, info);
    state->shuttingDown = true;
    thread = state->workerThread;
    state->workerThread = nullptr;
    const bool hasPending = !state->pendingBytes->empty() || !state->pendingDebug->empty();
    LeaveCriticalSection(&state->queueCs);

    if (waitForThread && thread) {
        if (state->wakeEvent) SetEvent(state->wakeEvent);
        if (WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0) {
            CloseHandle(thread);
            return;
        }
        CloseHandle(thread);
    } else if (thread) {
        CloseHandle(thread);
    }

    if (hasPending && TryEnterCriticalSection(&state->queueCs)) {
        std::string batchBytes;
        std::wstring batchDebug;
        batchBytes.swap(*state->pendingBytes);
        batchDebug.swap(*state->pendingDebug);
        LeaveCriticalSection(&state->queueCs);
        writeBatchToDisk(state, info, batchBytes, batchDebug);
    }
    if (TryEnterCriticalSection(&state->fileCs)) {
        if (state->logFile != INVALID_HANDLE_VALUE) {
            CloseHandle(state->logFile);
            state->logFile = INVALID_HANDLE_VALUE;
        }
        LeaveCriticalSection(&state->fileCs);
    }
}

void EdpeLogDumpRecent(HANDLE file) {
    if (!file || file == INVALID_HANDLE_VALUE) return;
    const uint32_t total = s_logRingWriteIdx.load(std::memory_order_relaxed);
    const uint32_t count = (total < kLogRingLines) ? total : kLogRingLines;
    const uint32_t start = (total < kLogRingLines) ? 0 : (total - kLogRingLines);

    static const char header[] = "--- EDPE Internal Log (Recent Entries) ---\r\n";
    DWORD written = 0;
    WriteFile(file, header, static_cast<DWORD>(strlen(header)), &written, nullptr);

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t ringIdx = (start + i) % kLogRingLines;
        const char* msg = s_logRing[ringIdx];
        if (msg[0] != '\0') {
            DWORD len = static_cast<DWORD>(strlen(msg));
            WriteFile(file, msg, len, &written, nullptr);
            WriteFile(file, "\r\n", 2, &written, nullptr);
        }
    }
    WriteFile(file, "\r\n", 2, &written, nullptr);
}
