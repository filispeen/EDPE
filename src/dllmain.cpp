#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        OutputDebugStringW(L"EDPE loaded\n");
    }
    return TRUE;
}
