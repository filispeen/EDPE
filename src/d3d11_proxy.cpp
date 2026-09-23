#include "log.h"
#include "system_dll.h"

#include <d3d11.h>
#include <cwchar>
#include <windows.h>

extern "C" HRESULT WINAPI EdpeD3D11CreateDevice(
    IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL* feature_levels,
    UINT feature_level_count,
    UINT sdk_version,
    ID3D11Device** device,
    D3D_FEATURE_LEVEL* feature_level,
    ID3D11DeviceContext** context) {
    static const HMODULE system_d3d11 = LoadSystemDll(L"d3d11.dll");
    if (!system_d3d11) return HRESULT_FROM_WIN32(GetLastError());

    static const auto create_device = reinterpret_cast<decltype(&D3D11CreateDevice)>(
        GetProcAddress(system_d3d11, "D3D11CreateDevice"));
    if (!create_device) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    const HRESULT result = create_device(adapter, driver_type, software, flags,
        feature_levels, feature_level_count, sdk_version, device, feature_level, context);
    if (SUCCEEDED(result) && device && *device) {
        void** methods = *reinterpret_cast<void***>(*device);
        wchar_t message[160];
        swprintf_s(message, L"EDPE: D3D11 device created device=%p vtable=%p dsvMethod=%p",
            *device, methods, methods[10]);
        EdpeLog(message);
    } else {
        EdpeLog(SUCCEEDED(result) ? L"EDPE: D3D11 device created without output"
                                  : L"EDPE: D3D11 device creation failed");
    }
    return result;
}
