#include "crash_logger.h"
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        edpe::installCrashLogger();
        OutputDebugStringW(L"EDPE loaded\n");
#if defined(EDPE_CRASH_TEST)
        volatile int* crashPtr = nullptr;
        *crashPtr = 42;
#endif
    } else if (reason == DLL_PROCESS_DETACH) {
        edpe::uninstallCrashLogger();
    }
    return TRUE;
}
