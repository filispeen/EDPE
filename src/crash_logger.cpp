#include "crash_logger.h"
#include "log.h"
#include "config.h"
#include "system_dll.h"

#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <dxgi.h>
#include <d3d11.h>
#include <atomic>
#include <csignal>
#include <exception>
#include <cstdio>
#include <cwchar>

namespace edpe {

constexpr DWORD kStatusHeapCorruption = 0xC0000374;
constexpr DWORD kStatusStackBufferOverrun = 0xC0000409;
constexpr DWORD kStatusFatalAppExit = 0xC0000420;
constexpr DWORD kStatusInvalidParameter = 0xC000000D;
constexpr DWORD kCppException = 0xE06D7363;

static LPTOP_LEVEL_EXCEPTION_FILTER s_prevUnhandledFilter = nullptr;
static PVOID s_vehHandle = nullptr;
static std::terminate_handler s_prevTerminate = nullptr;
static void (*s_prevSigAbrt)(int) = nullptr;
static _invalid_parameter_handler s_prevInvalidParam = nullptr;
static _purecall_handler s_prevPurecall = nullptr;

static std::atomic<bool> s_crashLogged{false};

static wchar_t s_logsDirectory[MAX_PATH] = {0};
static wchar_t s_cachedWindowsVersion[128] = {0};
static wchar_t s_cachedGpuName[128] = {0};
static wchar_t s_cachedDriverVersion[64] = {0};

static std::atomic<HRESULT> s_lastD3D11HResult{S_OK};
static std::atomic<int> s_upscalerBackend{0};
static std::atomic<int> s_upscaleMode{0};
static std::atomic<bool> s_isFallback{false};
static std::atomic<uint32_t> s_renderWidth{0};
static std::atomic<uint32_t> s_renderHeight{0};
static std::atomic<uint32_t> s_outputWidth{0};
static std::atomic<uint32_t> s_outputHeight{0};
static std::atomic<uint64_t> s_frameNumber{0};

static const char* getExceptionName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_BREAKPOINT: return "EXCEPTION_BREAKPOINT";
        case EXCEPTION_SINGLE_STEP: return "EXCEPTION_SINGLE_STEP";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_FLT_DENORMAL_OPERAND: return "EXCEPTION_FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INEXACT_RESULT: return "EXCEPTION_FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_INVALID_OPERATION: return "EXCEPTION_FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW: return "EXCEPTION_FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK: return "EXCEPTION_FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW: return "EXCEPTION_FLT_UNDERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW: return "EXCEPTION_INT_OVERFLOW";
        case EXCEPTION_PRIV_INSTRUCTION: return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION: return "EXCEPTION_INVALID_DISPOSITION";
        case EXCEPTION_GUARD_PAGE: return "EXCEPTION_GUARD_PAGE";
        case EXCEPTION_INVALID_HANDLE: return "EXCEPTION_INVALID_HANDLE";
        case kStatusHeapCorruption: return "STATUS_HEAP_CORRUPTION";
        case kStatusStackBufferOverrun: return "STATUS_STACK_BUFFER_OVERRUN";
        case kStatusFatalAppExit: return "STATUS_FATAL_APP_EXIT";
        case kStatusInvalidParameter: return "STATUS_INVALID_PARAMETER";
        case kCppException: return "CPP_EXCEPTION";
        default: return "UNKNOWN_EXCEPTION";
    }
}

static const char* getHResultName(HRESULT hr) {
    switch (hr) {
        case S_OK: return "S_OK";
        case S_FALSE: return "S_FALSE";
        case E_FAIL: return "E_FAIL";
        case E_INVALIDARG: return "E_INVALIDARG";
        case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
        case E_NOINTERFACE: return "E_NOINTERFACE";
        case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
        case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
        case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
        case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
        case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
        default: return "UNKNOWN";
    }
}

static const char* getBackendName(int backend) {
    switch (backend) {
        case 1: return "NVIDIA DLSS";
        case 2: return "AMD FSR";
        case 3: return "TAA";
        default: return "None (Bypass)";
    }
}

static const char* getModeName(int mode) {
    switch (mode) {
        case 1: return "NativeAA";
        case 2: return "Quality";
        case 3: return "Balanced";
        case 4: return "Performance";
        case 5: return "UltraPerformance";
        default: return "Native";
    }
}

static void initLogsDirectory() {
    if (s_logsDirectory[0] != L'\0') {
        return;
    }
    HMODULE hModule = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&installCrashLogger), &hModule) && hModule) {
        wchar_t modulePath[MAX_PATH] = {0};
        DWORD len = GetModuleFileNameW(hModule, modulePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            wchar_t* lastSlash = wcsrchr(modulePath, L'\\');
            if (lastSlash) {
                *lastSlash = L'\0';
                swprintf_s(s_logsDirectory, L"%ls\\logs", modulePath);
                CreateDirectoryW(s_logsDirectory, nullptr);
                return;
            }
        }
    }
    wchar_t exePath[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        wchar_t* lastSlash = wcsrchr(exePath, L'\\');
        if (lastSlash) {
            *lastSlash = L'\0';
            swprintf_s(s_logsDirectory, L"%ls\\logs", exePath);
            CreateDirectoryW(s_logsDirectory, nullptr);
        }
    }
}

void pruneCrashLogs() {
    initLogsDirectory();
    if (s_logsDirectory[0] == L'\0') {
        return;
    }

    wchar_t pattern[MAX_PATH];
    swprintf_s(pattern, L"%ls\\crash_*.log", s_logsDirectory);

    static wchar_t fileList[256][MAX_PATH];
    int count = 0;

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                if (count < 256) {
                    wcscpy_s(fileList[count], fd.cFileName);
                    count++;
                }
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    for (int i = 0; i < count - 1; ++i) {
        for (int j = 0; j < count - 1 - i; ++j) {
            if (wcscmp(fileList[j], fileList[j + 1]) > 0) {
                wchar_t tmp[MAX_PATH];
                wcscpy_s(tmp, fileList[j]);
                wcscpy_s(fileList[j], fileList[j + 1]);
                wcscpy_s(fileList[j + 1], tmp);
            }
        }
    }

    if (count > 20) {
        int toDelete = count - 20;
        for (int i = 0; i < toDelete; ++i) {
            wchar_t filePath[MAX_PATH];
            swprintf_s(filePath, L"%ls\\%ls", s_logsDirectory, fileList[i]);
            DeleteFileW(filePath);
        }
    }
}

static void cacheSystemInformation() {
    typedef LONG(NTAPI* PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        auto pfn = reinterpret_cast<PFN_RtlGetVersion>(GetProcAddress(hNtdll, "RtlGetVersion"));
        if (pfn) {
            RTL_OSVERSIONINFOW vi{};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (pfn(&vi) == 0) {
                swprintf_s(s_cachedWindowsVersion, L"Windows %lu.%lu (Build %lu)",
                           vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
            }
        }
    }
    if (s_cachedWindowsVersion[0] == L'\0') {
        wcscpy_s(s_cachedWindowsVersion, L"Windows (Unknown)");
    }

    HMODULE hDxgi = LoadSystemDll(L"dxgi.dll");
    if (hDxgi) {
        auto pfnCreateDXGIFactory1 = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(
            GetProcAddress(hDxgi, "CreateDXGIFactory1"));
        if (pfnCreateDXGIFactory1) {
            IDXGIFactory1* factory = nullptr;
            if (SUCCEEDED(pfnCreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
                IDXGIAdapter* adapter = nullptr;
                if (SUCCEEDED(factory->EnumAdapters(0, &adapter)) && adapter) {
                    DXGI_ADAPTER_DESC desc{};
                    if (SUCCEEDED(adapter->GetDesc(&desc))) {
                        wcscpy_s(s_cachedGpuName, desc.Description);
                    }
                    LARGE_INTEGER drvVer{};
                    if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &drvVer))) {
                        swprintf_s(s_cachedDriverVersion, L"%u.%u.%u.%u",
                                   HIWORD(drvVer.HighPart),
                                   LOWORD(drvVer.HighPart),
                                   HIWORD(drvVer.LowPart),
                                   LOWORD(drvVer.LowPart));
                    }
                    adapter->Release();
                }
                factory->Release();
            }
        }
        FreeLibrary(hDxgi);
    }
    if (s_cachedGpuName[0] == L'\0') {
        wcscpy_s(s_cachedGpuName, L"Unknown GPU");
    }
    if (s_cachedDriverVersion[0] == L'\0') {
        wcscpy_s(s_cachedDriverVersion, L"Unknown Driver Version");
    }
}

void updateCrashGpuInfo(IDXGISwapChain* swapChain) {
    if (!swapChain) return;
    ID3D11Device* device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device))) && device) {
        updateCrashGpuInfoFromDevice(device);
        device->Release();
    }
}

void updateCrashGpuInfoFromDevice(ID3D11Device* device) {
    if (!device) return;
    IDXGIDevice* dxgiDevice = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) && dxgiDevice) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter) {
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(adapter->GetDesc(&desc))) {
                wcscpy_s(s_cachedGpuName, desc.Description);
            }
            LARGE_INTEGER drvVer{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &drvVer))) {
                swprintf_s(s_cachedDriverVersion, L"%u.%u.%u.%u",
                           HIWORD(drvVer.HighPart),
                           LOWORD(drvVer.HighPart),
                           HIWORD(drvVer.LowPart),
                           LOWORD(drvVer.LowPart));
            }
            adapter->Release();
        }
        dxgiDevice->Release();
    }
}

static void writeRaw(HANDLE file, const char* str) {
    if (!file || file == INVALID_HANDLE_VALUE || !str) return;
    DWORD len = static_cast<DWORD>(strlen(str));
    DWORD written = 0;
    WriteFile(file, str, len, &written, nullptr);
}

static void writeFormat(HANDLE file, const char* fmt, ...) {
    static char buf[4096];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    if (len > 0) {
        DWORD written = 0;
        WriteFile(file, buf, static_cast<DWORD>(len), &written, nullptr);
    }
}

static void writeCrashDump(EXCEPTION_POINTERS* ep) {
    bool expected = false;
    if (!s_crashLogged.compare_exchange_strong(expected, true)) {
        return;
    }

    initLogsDirectory();

    SYSTEMTIME st;
    GetLocalTime(&st);

    wchar_t crashFilePath[MAX_PATH];
    swprintf_s(crashFilePath, L"%ls\\crash_%04u-%02u-%02u_%02u-%02u-%02u.log",
               s_logsDirectory,
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);

    HANDLE file = CreateFileW(crashFilePath, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    const DWORD pid = GetCurrentProcessId();
    const DWORD tid = GetCurrentThreadId();

    writeRaw(file, "================================================================================\r\n");
    writeRaw(file, "                              EDPE CRASH REPORT\r\n");
    writeRaw(file, "================================================================================\r\n\r\n");

    writeFormat(file, "Date/Time:    %04u-%02u-%02u %02u:%02u:%02u\r\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    writeFormat(file, "EDPE Version: %s\r\n", edpe::kVersion);
    writeFormat(file, "PID:          %lu (0x%X)\r\n", pid, pid);
    writeFormat(file, "TID:          %lu (0x%X)\r\n\r\n", tid, tid);

    PEXCEPTION_RECORD rec = ep ? ep->ExceptionRecord : nullptr;
    PCONTEXT ctx = ep ? ep->ContextRecord : nullptr;

    const DWORD excCode = rec ? rec->ExceptionCode : 0;
    const void* excAddr = rec ? rec->ExceptionAddress : nullptr;

    HMODULE excMod = nullptr;
    wchar_t excModName[MAX_PATH] = L"unknown";
    DWORD64 excModBase = 0;
    if (excAddr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      reinterpret_cast<LPCWSTR>(excAddr), &excMod) && excMod) {
        excModBase = reinterpret_cast<DWORD64>(excMod);
        wchar_t modPath[MAX_PATH] = {0};
        if (GetModuleFileNameW(excMod, modPath, MAX_PATH)) {
            wchar_t* p = wcsrchr(modPath, L'\\');
            wcscpy_s(excModName, p ? (p + 1) : modPath);
        }
    }
    const DWORD64 excOffset = excAddr ? (reinterpret_cast<DWORD64>(excAddr) - excModBase) : 0;

    writeRaw(file, "--- Exception Details ---\r\n");
    writeFormat(file, "Exception Code:    0x%08X (%s)\r\n", excCode, getExceptionName(excCode));
    writeFormat(file, "Exception Address: 0x%p (%ls+0x%llX)\r\n", excAddr, excModName, excOffset);

    if (rec && (excCode == EXCEPTION_ACCESS_VIOLATION || excCode == EXCEPTION_IN_PAGE_ERROR) && rec->NumberParameters >= 2) {
        const ULONG_PTR accessType = rec->ExceptionInformation[0];
        const ULONG_PTR targetAddr = rec->ExceptionInformation[1];
        const char* op = (accessType == 0) ? "Read" : (accessType == 1) ? "Write" : (accessType == 8) ? "Execute" : "Access";
        writeFormat(file, "Access Violation:  %s at 0x%016llX\r\n", op, static_cast<unsigned long long>(targetAddr));
    }
    writeRaw(file, "\r\n");

    if (ctx) {
        writeRaw(file, "--- Processor Registers ---\r\n");
#if defined(_M_X64)
        writeFormat(file, "RAX: 0x%016llX  RBX: 0x%016llX  RCX: 0x%016llX  RDX: 0x%016llX\r\n",
                    ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
        writeFormat(file, "RSI: 0x%016llX  RDI: 0x%016llX  RBP: 0x%016llX  RSP: 0x%016llX\r\n",
                    ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp);
        writeFormat(file, "R8:  0x%016llX  R9:  0x%016llX  R10: 0x%016llX  R11: 0x%016llX\r\n",
                    ctx->R8, ctx->R9, ctx->R10, ctx->R11);
        writeFormat(file, "R12: 0x%016llX  R13: 0x%016llX  R14: 0x%016llX  R15: 0x%016llX\r\n",
                    ctx->R12, ctx->R13, ctx->R14, ctx->R15);
        writeFormat(file, "RIP: 0x%016llX  EFLAGS: 0x%08X\r\n", ctx->Rip, ctx->EFlags);
        writeFormat(file, "CS: 0x%04X  DS: 0x%04X  ES: 0x%04X  FS: 0x%04X  GS: 0x%04X  SS: 0x%04X\r\n",
                    ctx->SegCs, ctx->SegDs, ctx->SegEs, ctx->SegFs, ctx->SegGs, ctx->SegSs);
#endif
        writeRaw(file, "\r\n");
    }

    writeRaw(file, "--- Stack Trace ---\r\n");
    HANDLE hProcess = GetCurrentProcess();
    HANDLE hThread = GetCurrentThread();

    CONTEXT ctxCopy{};
    if (ctx) {
        ctxCopy = *ctx;
    } else {
        RtlCaptureContext(&ctxCopy);
    }

    STACKFRAME64 frame{};
#if defined(_M_X64)
    DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = ctxCopy.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctxCopy.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctxCopy.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
#endif

    static char symBuffer[sizeof(SYMBOL_INFO) + 256];
    PSYMBOL_INFO pSym = reinterpret_cast<PSYMBOL_INFO>(symBuffer);
    pSym->SizeOfStruct = sizeof(SYMBOL_INFO);
    pSym->MaxNameLen = 255;

    int frameIndex = 0;
    while (frameIndex < 64) {
        if (!StackWalk64(machineType, hProcess, hThread, &frame, &ctxCopy, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) {
            break;
        }

        const DWORD64 addr = frame.AddrPC.Offset;
        HMODULE hMod = nullptr;
        wchar_t frameModName[MAX_PATH] = L"unknown";
        DWORD64 frameModBase = 0;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(addr), &hMod) && hMod) {
            frameModBase = reinterpret_cast<DWORD64>(hMod);
            wchar_t modPath[MAX_PATH] = {0};
            if (GetModuleFileNameW(hMod, modPath, MAX_PATH)) {
                wchar_t* p = wcsrchr(modPath, L'\\');
                wcscpy_s(frameModName, p ? (p + 1) : modPath);
            }
        }
        const DWORD64 offsetInMod = addr - frameModBase;

        DWORD64 displacement = 0;
        bool hasSymbol = SymFromAddr(hProcess, addr, &displacement, pSym) == TRUE;

        IMAGEHLP_LINE64 lineInfo{};
        lineInfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD lineDisp = 0;
        bool hasLine = SymGetLineFromAddr64(hProcess, addr, &lineDisp, &lineInfo) == TRUE;

        if (hasSymbol && hasLine) {
            writeFormat(file, "#%02d 0x%016llX %ls+0x%llX (%s+0x%llX) [%s:%lu]\r\n",
                        frameIndex, addr, frameModName, offsetInMod, pSym->Name, displacement,
                        lineInfo.FileName, lineInfo.LineNumber);
        } else if (hasSymbol) {
            writeFormat(file, "#%02d 0x%016llX %ls+0x%llX (%s+0x%llX)\r\n",
                        frameIndex, addr, frameModName, offsetInMod, pSym->Name, displacement);
        } else {
            writeFormat(file, "#%02d 0x%016llX %ls+0x%llX\r\n",
                        frameIndex, addr, frameModName, offsetInMod);
        }
        frameIndex++;
    }

    if (frameIndex == 0) {
        void* backtraceAddrs[64]{};
        USHORT captured = CaptureStackBackTrace(0, 64, backtraceAddrs, nullptr);
        for (USHORT i = 0; i < captured; ++i) {
            const DWORD64 addr = reinterpret_cast<DWORD64>(backtraceAddrs[i]);
            HMODULE hMod = nullptr;
            wchar_t frameModName[MAX_PATH] = L"unknown";
            DWORD64 frameModBase = 0;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(addr), &hMod) && hMod) {
                frameModBase = reinterpret_cast<DWORD64>(hMod);
                wchar_t modPath[MAX_PATH] = {0};
                if (GetModuleFileNameW(hMod, modPath, MAX_PATH)) {
                    wchar_t* p = wcsrchr(modPath, L'\\');
                    wcscpy_s(frameModName, p ? (p + 1) : modPath);
                }
            }
            const DWORD64 offsetInMod = addr - frameModBase;
            writeFormat(file, "#%02d 0x%016llX %ls+0x%llX\r\n", i, addr, frameModName, offsetInMod);
        }
    }
    writeRaw(file, "\r\n");

    writeRaw(file, "--- System & Hardware ---\r\n");
    writeFormat(file, "OS Version:     %ls\r\n", s_cachedWindowsVersion);
    writeFormat(file, "GPU:            %ls\r\n", s_cachedGpuName);
    writeFormat(file, "Driver Version: %ls\r\n\r\n", s_cachedDriverVersion);

    const HRESULT hr = s_lastD3D11HResult.load(std::memory_order_relaxed);
    const int backend = s_upscalerBackend.load(std::memory_order_relaxed);
    const int mode = s_upscaleMode.load(std::memory_order_relaxed);
    const bool isFb = s_isFallback.load(std::memory_order_relaxed);
    const uint32_t rW = s_renderWidth.load(std::memory_order_relaxed);
    const uint32_t rH = s_renderHeight.load(std::memory_order_relaxed);
    const uint32_t oW = s_outputWidth.load(std::memory_order_relaxed);
    const uint32_t oH = s_outputHeight.load(std::memory_order_relaxed);
    const uint64_t frameNum = s_frameNumber.load(std::memory_order_relaxed);

    writeRaw(file, "--- Graphics State ---\r\n");
    writeFormat(file, "Last D3D11 HRESULT: 0x%08X (%s)\r\n", hr, getHResultName(hr));
    writeFormat(file, "Upscaler Backend:   %s (%s)\r\n", getBackendName(backend), isFb ? "Fallback" : "Active");
    writeFormat(file, "Upscaler Mode:      %s\r\n", getModeName(mode));
    writeFormat(file, "Resolution:         %ux%u -> %ux%u\r\n", rW, rH, oW, oH);
    writeFormat(file, "Frame Number:       %llu\r\n\r\n", frameNum);

    writeRaw(file, "--- Loaded Modules ---\r\n");
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(hSnap, &me)) {
            do {
                const DWORD64 modBase = reinterpret_cast<DWORD64>(me.modBaseAddr);
                const DWORD64 modEnd = modBase + me.modBaseSize;
                writeFormat(file, "0x%016llX - 0x%016llX  %-28ls  %ls\r\n",
                            modBase, modEnd, me.szModule, me.szExePath);
            } while (Module32NextW(hSnap, &me));
        }
        CloseHandle(hSnap);
    }
    writeRaw(file, "\r\n");

    EdpeLogDumpRecent(file);

    CloseHandle(file);

    pruneCrashLogs();
}

static LONG WINAPI edpeUnhandledExceptionFilter(EXCEPTION_POINTERS* ep) {
    writeCrashDump(ep);
    if (s_prevUnhandledFilter) {
        return s_prevUnhandledFilter(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI edpeVectoredExceptionHandler(PEXCEPTION_POINTERS ep) {
    if (!ep || !ep->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_STACK_OVERFLOW:
        case kStatusHeapCorruption:
        case kStatusStackBufferOverrun:
            writeCrashDump(ep);
            break;
        default:
            break;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void edpeTerminateHandler() {
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = kCppException;
    rec.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
    EXCEPTION_POINTERS ep{&rec, &ctx};
    writeCrashDump(&ep);
    if (s_prevTerminate) {
        s_prevTerminate();
    } else {
        std::abort();
    }
}

static void edpeSignalHandler(int sig) {
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = kStatusFatalAppExit;
    rec.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
    EXCEPTION_POINTERS ep{&rec, &ctx};
    writeCrashDump(&ep);
    if (s_prevSigAbrt && s_prevSigAbrt != SIG_DFL && s_prevSigAbrt != SIG_IGN) {
        s_prevSigAbrt(sig);
    } else {
        signal(SIGABRT, SIG_DFL);
        raise(SIGABRT);
    }
}

static void edpeInvalidParameterHandler(const wchar_t* expression, const wchar_t* function,
                                       const wchar_t* file, unsigned int line, uintptr_t pReserved) {
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = kStatusInvalidParameter;
    rec.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
    EXCEPTION_POINTERS ep{&rec, &ctx};
    writeCrashDump(&ep);
    if (s_prevInvalidParam) {
        s_prevInvalidParam(expression, function, file, line, pReserved);
    }
}

static void edpePurecallHandler() {
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = 0xC0000025;
    rec.ExceptionAddress = reinterpret_cast<PVOID>(ctx.Rip);
    EXCEPTION_POINTERS ep{&rec, &ctx};
    writeCrashDump(&ep);
    if (s_prevPurecall) {
        s_prevPurecall();
    } else {
        std::abort();
    }
}

void installCrashLogger() {
    static std::atomic<bool> s_installed{false};
    bool expected = false;
    if (!s_installed.compare_exchange_strong(expected, true)) {
        return;
    }

    ULONG stackGuarantee = 64 * 1024;
    SetThreadStackGuarantee(&stackGuarantee);

    initLogsDirectory();
    cacheSystemInformation();

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);

    s_prevUnhandledFilter = SetUnhandledExceptionFilter(edpeUnhandledExceptionFilter);
    s_vehHandle = AddVectoredExceptionHandler(1, edpeVectoredExceptionHandler);
    s_prevTerminate = std::set_terminate(edpeTerminateHandler);
    s_prevSigAbrt = signal(SIGABRT, edpeSignalHandler);
    s_prevInvalidParam = _set_invalid_parameter_handler(edpeInvalidParameterHandler);
    s_prevPurecall = _set_purecall_handler(edpePurecallHandler);
}

void uninstallCrashLogger() {
    if (s_prevUnhandledFilter) {
        SetUnhandledExceptionFilter(s_prevUnhandledFilter);
        s_prevUnhandledFilter = nullptr;
    }
    if (s_vehHandle) {
        RemoveVectoredExceptionHandler(s_vehHandle);
        s_vehHandle = nullptr;
    }
    if (s_prevTerminate) {
        std::set_terminate(s_prevTerminate);
        s_prevTerminate = nullptr;
    }
    if (s_prevSigAbrt) {
        signal(SIGABRT, s_prevSigAbrt);
        s_prevSigAbrt = nullptr;
    }
    if (s_prevInvalidParam) {
        _set_invalid_parameter_handler(s_prevInvalidParam);
        s_prevInvalidParam = nullptr;
    }
    if (s_prevPurecall) {
        _set_purecall_handler(s_prevPurecall);
        s_prevPurecall = nullptr;
    }
    SymCleanup(GetCurrentProcess());
}

void setLastD3D11HResult(HRESULT hr) {
    s_lastD3D11HResult.store(hr, std::memory_order_relaxed);
}

void updateCrashUpscalerState(int backend, int mode, bool isFallback, uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH, uint64_t frameNumber) {
    s_upscalerBackend.store(backend, std::memory_order_relaxed);
    s_upscaleMode.store(mode, std::memory_order_relaxed);
    s_isFallback.store(isFallback, std::memory_order_relaxed);
    s_renderWidth.store(renderW, std::memory_order_relaxed);
    s_renderHeight.store(renderH, std::memory_order_relaxed);
    s_outputWidth.store(outW, std::memory_order_relaxed);
    s_outputHeight.store(outH, std::memory_order_relaxed);
    s_frameNumber.store(frameNumber, std::memory_order_relaxed);
}

void setCrashFrameNumber(uint64_t frameNumber) {
    s_frameNumber.store(frameNumber, std::memory_order_relaxed);
}

void writeCrashDumpForTesting(EXCEPTION_POINTERS* ep) {
    writeCrashDump(ep);
}

}
