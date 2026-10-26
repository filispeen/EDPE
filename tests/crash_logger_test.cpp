#include "crash_logger.h"
#include "log.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

int runChildCrash() {
    edpe::installCrashLogger();
    edpe::setLastD3D11HResult(DXGI_ERROR_DEVICE_REMOVED);
    edpe::updateCrashUpscalerState(1, 2, false, 1920, 1080, 2560, 1440, 42);
    EdpeLog(L"Test log message 1 before crash");
    EdpeLog(L"Test log message 2 before crash");
    volatile int* nullPtr = nullptr;
    *nullPtr = 42;
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc > 1 && std::strcmp(argv[1], "--child-crash-av") == 0) {
        return runChildCrash();
    }

    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    fs::path exeDir = fs::path(exePath).parent_path();
    fs::path logsDir = exeDir / "logs";
    fs::create_directories(logsDir);

    for (const auto& entry : fs::directory_iterator(logsDir)) {
        if (entry.path().extension() == ".log") {
            fs::remove(entry.path());
        }
    }

    std::wstring cmd = std::wstring(L"\"") + exePath + L"\" --child-crash-av";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    if (!created) {
        std::cerr << "Failed to create child process\n";
        return 1;
    }

    WaitForSingleObject(pi.hProcess, 10000);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (exitCode != static_cast<DWORD>(STATUS_ACCESS_VIOLATION)) {
        std::cerr << "Child process exit code mismatch: 0x" << std::hex << exitCode << "\n";
        return 2;
    }

    std::vector<fs::path> foundLogs;
    for (const auto& entry : fs::directory_iterator(logsDir)) {
        if (entry.path().filename().wstring().rfind(L"crash_", 0) == 0 && entry.path().extension() == ".log") {
            foundLogs.push_back(entry.path());
        }
    }

    if (foundLogs.empty()) {
        std::cerr << "No crash log was created in " << logsDir.string() << "\n";
        return 3;
    }

    std::ifstream logFile(foundLogs[0], std::ios::binary);
    if (!logFile.is_open()) {
        std::cerr << "Failed to open crash log\n";
        return 4;
    }

    std::string content((std::istreambuf_iterator<char>(logFile)), std::istreambuf_iterator<char>());
    logFile.close();

    const char* requiredStrings[] = {
        "EDPE CRASH REPORT",
        "PID:",
        "TID:",
        "Exception Code:    0xC0000005 (EXCEPTION_ACCESS_VIOLATION)",
        "Processor Registers",
        "RAX:",
        "RIP:",
        "Stack Trace",
        "System & Hardware",
        "OS Version:",
        "Graphics State",
        "Last D3D11 HRESULT: 0x887A0005 (DXGI_ERROR_DEVICE_REMOVED)",
        "Upscaler Backend:   NVIDIA DLSS (Active)",
        "Upscaler Mode:      Quality",
        "Resolution:         1920x1080 -> 2560x1440",
        "Frame Number:       42",
        "Loaded Modules",
        "--- EDPE Internal Log (Recent Entries) ---",
        "Test log message 1 before crash",
        "Test log message 2 before crash"
    };

    for (const char* req : requiredStrings) {
        if (content.find(req) == std::string::npos) {
            std::cerr << "Missing expected string in crash log: " << req << "\n";
            return 5;
        }
    }

    for (int i = 1; i <= 25; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "crash_2020-01-01_00-00-%02d.log", i);
        fs::path dummyPath = logsDir / name;
        std::ofstream dummy(dummyPath);
        dummy << "dummy";
    }

    edpe::pruneCrashLogs();

    int remainingCount = 0;
    for (const auto& entry : fs::directory_iterator(logsDir)) {
        if (entry.path().filename().wstring().rfind(L"crash_", 0) == 0 && entry.path().extension() == ".log") {
            remainingCount++;
        }
    }

    if (remainingCount > 20) {
        std::cerr << "Pruning failed, remaining count: " << remainingCount << "\n";
        return 6;
    }

    if (fs::exists(logsDir / "crash_2020-01-01_00-00-01.log")) {
        std::cerr << "Oldest file was not pruned\n";
        return 7;
    }

    if (!fs::exists(logsDir / "crash_2020-01-01_00-00-25.log")) {
        std::cerr << "Newest file was unexpectedly pruned\n";
        return 8;
    }

    for (const auto& entry : fs::directory_iterator(logsDir)) {
        fs::remove(entry.path());
    }

    std::cout << "All crash logger checks passed successfully\n";
    return 0;
}
