#include "d3d11_proxy.h"
#include "config.h"
#include "log.h"
#include "system_dll.h"
#include "input_hook.h"

#include <dxgi1_3.h>
#include <mutex>
#include <windows.h>

void ObserveDxgiFactory(REFIID iid, void* factory);

void syncConfigOnce() {
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        edpe::installD3D11Hooks();
        if (edpe::isRunningInEliteProcess()) {
            edpe::installInputHooks();
        }
        auto& cfg = edpe::Config::instance();
        cfg.load();
        if (cfg.syncEliteGraphicsConfig()) {
            wchar_t msg[160]{};
            swprintf_s(msg, L"EDPE: synced Elite graphics config (SSAAMultiplier=%.6f)",
                edpe::Config::getTargetSsaaMultiplier(cfg.upscalerBackend, cfg.upscaleMode));
            EdpeLog(msg);
        }
    });
}

extern "C" HRESULT WINAPI EdpeCreateDXGIFactory(REFIID iid, void** factory) {
    syncConfigOnce();
    static const HMODULE system_dxgi = LoadSystemDll(L"dxgi.dll");
    if (!system_dxgi) return HRESULT_FROM_WIN32(GetLastError());

    static const auto create_factory = reinterpret_cast<decltype(&CreateDXGIFactory)>(
        GetProcAddress(system_dxgi, "CreateDXGIFactory"));
    if (!create_factory) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    const HRESULT result = create_factory(iid, factory);
    if (SUCCEEDED(result) && factory && *factory) ObserveDxgiFactory(iid, *factory);
    EdpeLog(SUCCEEDED(result) ? L"EDPE: DXGI factory created"
                              : L"EDPE: DXGI factory creation failed");
    return result;
}

extern "C" HRESULT WINAPI EdpeCreateDXGIFactory1(REFIID iid, void** factory) {
    syncConfigOnce();
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
    syncConfigOnce();
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
