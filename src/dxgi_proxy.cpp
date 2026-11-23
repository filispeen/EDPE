#include "d3d11_proxy.h"
#include "crash_logger.h"
#include "config.h"
#include "frame_limiter.h"
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

HMODULE resolveBackendDxgi() {
    static const HMODULE s_backendDxgi = []() -> HMODULE {
        const auto& chainPath = edpe::Config::instance().chainDxgiDll;
        if (!chainPath.empty()) {
            std::wstring resolvedPath;
            HMODULE mod = LoadChainedOrSystemDll(L"dxgi.dll", chainPath.c_str(), &resolvedPath);
            if (mod && !resolvedPath.empty()) {
                std::wstring msg = L"EDPE: chained dxgi.dll -> " + resolvedPath;
                EdpeLog(msg.c_str());
                return mod;
            }
            std::wstring warn = L"EDPE: failed to chain dxgi.dll (" + chainPath + L"); falling back to system dxgi.dll";
            EdpeLog(warn.c_str());
            return mod;
        }
        return LoadSystemDll(L"dxgi.dll");
    }();
    return s_backendDxgi;
}
} // namespace

void syncConfigOnce() {
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        auto& cfg = edpe::Config::instance();
        cfg.load();
        edpe::FrameLimiter::instance().configure(cfg.gameFpsLimitEnabled, cfg.gameFpsLimit);
        edpe::RenderDocBridge::instance().refresh();
        // Preload backend/system DLLs before patching IATs so RenderDoc's LoadLibrary hook
        // does not immediately overwrite EDPE's D3D11 IAT entries.
        resolveBackendDxgi();
        LoadSystemDll(L"d3d11.dll");
        if (edpe::isRunningInEliteProcess()) {
            edpe::installInputHooks();
        }
        edpe::installD3D11Hooks();
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
    const HMODULE system_dxgi = resolveBackendDxgi();
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
    const HMODULE system_dxgi = resolveBackendDxgi();
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
    const HMODULE system_dxgi = resolveBackendDxgi();
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
