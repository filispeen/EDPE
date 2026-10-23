#include "d3d11_proxy.h"
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        edpe::installD3D11Hooks();
        OutputDebugStringW(L"EDPE loaded\n");
    }
    return TRUE;
}
