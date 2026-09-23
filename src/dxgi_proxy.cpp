#include "log.h"
#include "system_dll.h"

#include <dxgi1_3.h>
#include <windows.h>

void ObserveDxgiFactory(REFIID iid, void* factory);

extern "C" HRESULT WINAPI EdpeCreateDXGIFactory1(REFIID iid, void** factory) {
    static const HMODULE system_dxgi = LoadSystemDll(L"dxgi.dll");
    if (!system_dxgi) return HRESULT_FROM_WIN32(GetLastError());

    static const auto create_factory = reinterpret_cast<decltype(&CreateDXGIFactory1)>(
        GetProcAddress(system_dxgi, "CreateDXGIFactory1"));
    if (!create_factory) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    const HRESULT result = create_factory(iid, factory);
    if (SUCCEEDED(result) && factory && *factory) ObserveDxgiFactory(iid, *factory);
    EdpeLog(SUCCEEDED(result) ? L"EDPE: DXGI factory created"
                              : L"EDPE: DXGI factory creation failed");
    return result;
}

extern "C" HRESULT WINAPI EdpeCreateDXGIFactory2(UINT flags, REFIID iid, void** factory) {
    static const HMODULE system_dxgi = LoadSystemDll(L"dxgi.dll");
    if (!system_dxgi) return HRESULT_FROM_WIN32(GetLastError());

    static const auto create_factory = reinterpret_cast<decltype(&CreateDXGIFactory2)>(
        GetProcAddress(system_dxgi, "CreateDXGIFactory2"));
    if (!create_factory) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    const HRESULT result = create_factory(flags, iid, factory);
    if (SUCCEEDED(result) && factory && *factory) ObserveDxgiFactory(iid, *factory);
    EdpeLog(SUCCEEDED(result) ? L"EDPE: DXGI factory created"
                              : L"EDPE: DXGI factory creation failed");
    return result;
}
