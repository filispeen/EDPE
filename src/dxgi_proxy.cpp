#include "d3d11_proxy.h"
#include "crash_logger.h"
#include "config.h"
#include "log.h"
#include "renderdoc_support.h"
#include "system_dll.h"
#include "input_hook.h"

#include <dxgi1_3.h>
#include <intrin.h>
#include <mutex>
#include <windows.h>

void ObserveDxgiFactory(REFIID iid, void* factory);

namespace {
thread_local int t_dxgiFactoryDepth = 0;

struct ScopedDepth {
    int& depth;
    explicit ScopedDepth(int& d) : depth(d) { ++depth; }
    ~ScopedDepth() { --depth; }
};

template <typename Fn>
Fn resolveDxgiFactoryProc(HMODULE systemDxgi, const char* name, bool bypassHooks) {
    if (bypassHooks) {
        if (void* raw = ResolvePeExportRaw(systemDxgi, name)) {
            return reinterpret_cast<Fn>(raw);
        }
    }
    return reinterpret_cast<Fn>(GetProcAddress(systemDxgi, name));
}
} // namespace

void syncConfigOnce() {
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        edpe::RenderDocBridge::instance().refresh();
        // Preload system DLLs before patching IATs so RenderDoc's LoadLibrary hook
        // does not immediately overwrite EDPE's D3D11 IAT entries.
        LoadSystemDll(L"dxgi.dll");
        LoadSystemDll(L"d3d11.dll");
        if (edpe::isRunningInEliteProcess()) {
            edpe::installInputHooks();
        }
        edpe::installD3D11Hooks();
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
    const bool fromRenderDoc = (t_dxgiFactoryDepth > 0) || IsAddressInRenderDocModule(_ReturnAddress());
    if (t_dxgiFactoryDepth == 0) {
        syncConfigOnce();
        ScopedDepth probeGuard(t_dxgiFactoryDepth);
        EnsureRenderDocFactoryHooked();
    }
    static const HMODULE system_dxgi = LoadSystemDll(L"dxgi.dll");
    if (!system_dxgi) return HRESULT_FROM_WIN32(GetLastError());

    const auto create_factory = resolveDxgiFactoryProc<decltype(&CreateDXGIFactory)>(
        system_dxgi, "CreateDXGIFactory", fromRenderDoc);
    if (!create_factory) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    ScopedDepth guard(t_dxgiFactoryDepth);
    const HRESULT result = create_factory(iid, factory);
    if (fromRenderDoc) return result;

    edpe::setLastD3D11HResult(result);
    if (SUCCEEDED(result) && factory && *factory) ObserveDxgiFactory(iid, *factory);
    EdpeLog(SUCCEEDED(result) ? L"EDPE: DXGI factory created"
                              : L"EDPE: DXGI factory creation failed");
    return result;
}

extern "C" HRESULT WINAPI EdpeCreateDXGIFactory1(REFIID iid, void** factory) {
    const bool fromRenderDoc = (t_dxgiFactoryDepth > 0) || IsAddressInRenderDocModule(_ReturnAddress());
    if (t_dxgiFactoryDepth == 0) {
        syncConfigOnce();
        ScopedDepth probeGuard(t_dxgiFactoryDepth);
        EnsureRenderDocFactoryHooked();
    }
    static const HMODULE system_dxgi = LoadSystemDll(L"dxgi.dll");
    if (!system_dxgi) return HRESULT_FROM_WIN32(GetLastError());

    const auto create_factory = resolveDxgiFactoryProc<decltype(&CreateDXGIFactory1)>(
        system_dxgi, "CreateDXGIFactory1", fromRenderDoc);
    if (!create_factory) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    ScopedDepth guard(t_dxgiFactoryDepth);
    const HRESULT result = create_factory(iid, factory);
    if (fromRenderDoc) return result;

    edpe::setLastD3D11HResult(result);
    if (SUCCEEDED(result) && factory && *factory) ObserveDxgiFactory(iid, *factory);
    EdpeLog(SUCCEEDED(result) ? L"EDPE: DXGI factory created"
                              : L"EDPE: DXGI factory creation failed");
    return result;
}

extern "C" HRESULT WINAPI EdpeCreateDXGIFactory2(UINT flags, REFIID iid, void** factory) {
    const bool fromRenderDoc = (t_dxgiFactoryDepth > 0) || IsAddressInRenderDocModule(_ReturnAddress());
    if (t_dxgiFactoryDepth == 0) {
        syncConfigOnce();
        ScopedDepth probeGuard(t_dxgiFactoryDepth);
        EnsureRenderDocFactoryHooked();
    }
    static const HMODULE system_dxgi = LoadSystemDll(L"dxgi.dll");
    if (!system_dxgi) return HRESULT_FROM_WIN32(GetLastError());

    const auto create_factory = resolveDxgiFactoryProc<decltype(&CreateDXGIFactory2)>(
        system_dxgi, "CreateDXGIFactory2", fromRenderDoc);
    if (!create_factory) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    ScopedDepth guard(t_dxgiFactoryDepth);
    const HRESULT result = create_factory(flags, iid, factory);
    if (fromRenderDoc) return result;

    edpe::setLastD3D11HResult(result);
    if (SUCCEEDED(result) && factory && *factory) ObserveDxgiFactory(iid, *factory);
    EdpeLog(SUCCEEDED(result) ? L"EDPE: DXGI factory created"
                              : L"EDPE: DXGI factory creation failed");
    return result;
}
