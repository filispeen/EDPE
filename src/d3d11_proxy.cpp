#include "d3d11_proxy.h"
#include "crash_logger.h"
#include "context_census.h"
#include "config.h"
#include "input_hook.h"
#include "log.h"
#include "shader_probe.h"
#include "system_dll.h"
#include "vtable_hook.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>
#include <mutex>
#include <set>
#include <tuple>
#include <windows.h>
#include <tlhelp32.h>
#include <dxgi.h>
#include <vector>

namespace {
using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*,
    SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*,
    SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using CreateSamplerStateFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,
    const D3D11_SAMPLER_DESC*, ID3D11SamplerState**);
std::atomic<CreateVertexShaderFn> original_create_vertex_shader{nullptr};
std::atomic<CreatePixelShaderFn> original_create_pixel_shader{nullptr};
using CreateDeferredContextFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);
std::atomic<CreateSamplerStateFn> original_create_sampler_state{nullptr};
std::atomic<CreateDeferredContextFn> original_create_deferred_context{nullptr};
std::atomic<void**> hooked_device_table{nullptr};
std::atomic<bool> retain_vertex_bytecode{false};
std::atomic<bool> s_smooth_motion_detected{false};

HRESULT STDMETHODCALLTYPE observedCreateVertexShader(ID3D11Device* device,
    const void* bytecode, SIZE_T size, ID3D11ClassLinkage* linkage,
    ID3D11VertexShader** shader) {
    const HRESULT result = original_create_vertex_shader.load(std::memory_order_acquire)(
        device, bytecode, size, linkage, shader);
    if (SUCCEEDED(result) && shader && *shader && bytecode && size && size <= 65536) {
        if (retain_vertex_bytecode.load(std::memory_order_relaxed))
            (*shader)->SetPrivateData(kEdpeVertexBytecodeGuid, static_cast<UINT>(size), bytecode);
        const auto hash = EdpeShaderHash(bytecode, size);
        (*shader)->SetPrivateData(kEdpeShaderHashGuid, sizeof(hash), &hash);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE observedCreatePixelShader(ID3D11Device* device,
    const void* bytecode, SIZE_T size, ID3D11ClassLinkage* linkage,
    ID3D11PixelShader** shader) {
    const HRESULT result = original_create_pixel_shader.load(std::memory_order_acquire)(
        device, bytecode, size, linkage, shader);
    if (SUCCEEDED(result) && shader && *shader && bytecode && size && size <= 65536) {
        const auto hash = EdpeShaderHash(bytecode, size);
        (*shader)->SetPrivateData(kEdpeShaderHashGuid, sizeof(hash), &hash);
        (*shader)->SetPrivateData(kEdpePixelBytecodeGuid, static_cast<UINT>(size), bytecode);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE observedCreateSamplerState(ID3D11Device* device,
    const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** sampler) {
    if (!desc) {
        return original_create_sampler_state.load(std::memory_order_acquire)(device, desc, sampler);
    }

    static std::set<std::tuple<UINT, int, int, int>> s_seen_samplers;
    static std::mutex s_sampler_mutex;
    {
        const auto key = std::make_tuple(
            static_cast<UINT>(desc->Filter),
            static_cast<int>(std::lround(desc->MipLODBias * 100.0f)),
            static_cast<int>(std::lround(desc->MinLOD * 100.0f)),
            static_cast<int>(std::lround(std::min(desc->MaxLOD, 1000.0f) * 100.0f)));
        std::lock_guard<std::mutex> lock(s_sampler_mutex);
        if (s_seen_samplers.size() < 128 && s_seen_samplers.insert(key).second) {
            wchar_t msg[160];
            swprintf_s(msg, L"EDPE: CreateSamplerState filter=%u mipBias=%.2f minLOD=%.2f maxLOD=%.2f",
                desc->Filter, desc->MipLODBias, desc->MinLOD, desc->MaxLOD);
            EdpeLog(msg);
        }
    }

    D3D11_SAMPLER_DESC mod_desc = *desc;
    const bool is_comparison = D3D11_DECODE_IS_COMPARISON_FILTER(desc->Filter);
    const bool is_linear_mip = (D3D11_DECODE_MIP_FILTER(desc->Filter) == D3D11_FILTER_TYPE_LINEAR) ||
                               D3D11_DECODE_IS_ANISOTROPIC_FILTER(desc->Filter);
    const bool has_mipmaps = desc->MaxLOD > 0.5f;

    if (!is_comparison && is_linear_mip && has_mipmaps) {
        auto& cfg = edpe::Config::instance();
        const float ratio = edpe::Config::getTargetSsaaMultiplier(cfg.upscalerBackend, cfg.upscaleMode);
        if (ratio > 0.0f && ratio < 1.0f) {
            const float bias = std::log2(ratio) - 1.0f;
            mod_desc.MipLODBias = std::clamp(desc->MipLODBias + bias, -15.99f, 15.99f);
        }
    }

    return original_create_sampler_state.load(std::memory_order_acquire)(device, &mod_desc, sampler);
}

HRESULT STDMETHODCALLTYPE observedCreateDeferredContext(ID3D11Device* device,
    UINT ContextFlags, ID3D11DeviceContext** ppDeferredContext) {
    auto forward = original_create_deferred_context.load(std::memory_order_acquire);
    const HRESULT result = forward ? forward(device, ContextFlags, ppDeferredContext) : E_FAIL;
    if (SUCCEEDED(result) && ppDeferredContext && *ppDeferredContext) {
        ContextCensusHookDeferredContext(*ppDeferredContext);
    }
    return result;
}

std::mutex s_device_track_mutex;
// Non-owning identity pointers (never AddRef/Release during DLL_PROCESS_DETACH).
ID3D11Device* s_primary_device = nullptr;
ID3D11DeviceContext* s_primary_context = nullptr;
LUID s_primary_luid{};
bool s_has_primary_device = false;
std::atomic<unsigned> s_device_create_call_count{0};

HMODULE resolveRealSystemD3D11() {
    static HMODULE s_system_d3d11 = nullptr;
    if (s_system_d3d11) return s_system_d3d11;
    HMODULE raw = LoadSystemDll(L"d3d11.dll");
    HMODULE chosen = chooseSystemD3D11(raw);
    if (chosen && chosen != raw) {
        wchar_t rawPath[MAX_PATH]{};
        wchar_t chosenPath[MAX_PATH]{};
        GetModuleFileNameW(raw, rawPath, MAX_PATH);
        GetModuleFileNameW(chosen, chosenPath, MAX_PATH);
        wchar_t msg[512];
        swprintf_s(msg, L"EDPE: bypassed redirected d3d11 module (%ls -> %ls)", rawPath, chosenPath);
        EdpeLog(msg);
    }
    s_system_d3d11 = chosen ? chosen : raw;
    return s_system_d3d11;
}

void logDeviceCreationCall(const wchar_t* apiName, IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType,
    UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount, UINT sdkVersion) {
    const unsigned callNum = ++s_device_create_call_count;
    std::wstring adapterDesc = L"null";
    LUID adpLuid{};
    if (adapter) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            adapterDesc = desc.Description;
            adpLuid = desc.AdapterLuid;
        } else {
            adapterDesc = L"valid-adapter-no-desc";
        }
    }
    std::wstring flStr;
    if (featureLevels && featureLevelCount > 0) {
        for (UINT i = 0; i < featureLevelCount; ++i) {
            wchar_t hex[16];
            swprintf_s(hex, L"0x%X ", static_cast<unsigned>(featureLevels[i]));
            flStr += hex;
        }
    } else {
        flStr = L"[default]";
    }

    void* stack[8]{};
    USHORT frames = CaptureStackBackTrace(1, 8, stack, nullptr);
    std::wstring stackStr;
    for (USHORT i = 0; i < frames; ++i) {
        wchar_t frameHex[24];
        swprintf_s(frameHex, L"%p ", stack[i]);
        stackStr += frameHex;
    }

    wchar_t msg[512];
    swprintf_s(msg,
        L"EDPE: [DeviceCreate #%u] %ls: adapter=%p (\"%ls\" luid=%08x:%08x) driverType=%d flags=0x%X sdk=0x%X featureLevels=[%ls] thread=%u stack=[%ls]",
        callNum, apiName, adapter, adapterDesc.c_str(), adpLuid.HighPart, adpLuid.LowPart,
        static_cast<int>(driverType), flags, sdkVersion, flStr.c_str(), GetCurrentThreadId(), stackStr.c_str());
    EdpeLog(msg);
}

bool armShaderProbe() {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    wchar_t* name = wcsrchr(path, L'\\');
    if (!name) return false;
    const size_t prefix = name + 1 - path;
    if (prefix + sizeof(L"edpe_shader_probe.once") / sizeof(wchar_t) > MAX_PATH) return false;
    wcscpy_s(name + 1, MAX_PATH - prefix, L"edpe_shader_probe.once");
    return DeleteFileW(path) != 0;
}
}

ID3D11Device* GetPrimaryD3D11Device() {
    std::lock_guard lock(s_device_track_mutex);
    return s_primary_device;
}

void SetPrimaryD3D11Device(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device) return;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> immContext;
    if (context) {
        immContext = context;
    } else {
        device->GetImmediateContext(&immContext);
    }
    std::lock_guard lock(s_device_track_mutex);
    s_primary_device = device;
    if (immContext) {
        s_primary_context = immContext.Get();
    }
    s_has_primary_device = true;
}

void ResetPrimaryD3D11Device() {
    std::lock_guard lock(s_device_track_mutex);
    s_primary_device = nullptr;
    s_primary_context = nullptr;
    s_primary_luid = {};
    s_has_primary_device = false;
}

void hookShaderCreation(ID3D11Device* device) {
    if (!device) return;
    void** table = *reinterpret_cast<void***>(device);
    if (!retain_vertex_bytecode.load(std::memory_order_relaxed) && armShaderProbe()) {
        retain_vertex_bytecode.store(true, std::memory_order_relaxed);
    }
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&observedCreateVertexShader), &self)) {
        EdpeLog(L"EDPE: shader signatures unavailable (proxy lifetime cannot be pinned)");
        return;
    }
    constexpr size_t slot = 12;
    constexpr size_t pixel_slot = 15;
    auto forward = reinterpret_cast<CreateVertexShaderFn>(table[slot]);
    if (!forward || forward == &observedCreateVertexShader) {
        constexpr size_t deferred_slot = 27;
        auto deferred_forward = reinterpret_cast<CreateDeferredContextFn>(table[deferred_slot]);
        if (deferred_forward && deferred_forward != &observedCreateDeferredContext) {
            original_create_deferred_context.store(deferred_forward, std::memory_order_release);
            DWORD old_prot = 0;
            if (VirtualProtect(table + deferred_slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
                InterlockedCompareExchangePointer(
                    reinterpret_cast<PVOID volatile*>(table + deferred_slot),
                    reinterpret_cast<void*>(&observedCreateDeferredContext),
                    reinterpret_cast<void*>(deferred_forward));
                DWORD ignored = 0;
                VirtualProtect(table + deferred_slot, sizeof(void*), old_prot, &ignored);
            }
        }
        return;
    }
    original_create_vertex_shader.store(forward, std::memory_order_release);
    DWORD old_protection = 0;
    if (!VirtualProtect(table + slot, sizeof(void*), PAGE_READWRITE, &old_protection)) return;
    void* replaced = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(table + slot),
        reinterpret_cast<void*>(&observedCreateVertexShader),
        reinterpret_cast<void*>(forward));
    DWORD ignored = 0;
    VirtualProtect(table + slot, sizeof(void*), old_protection, &ignored);
    if (replaced == reinterpret_cast<void*>(forward)) {
        hooked_device_table.store(table, std::memory_order_release);
        bool pixel_hooked = false;
        auto pixel_forward = reinterpret_cast<CreatePixelShaderFn>(table[pixel_slot]);
        if (pixel_forward && pixel_forward != &observedCreatePixelShader) {
            original_create_pixel_shader.store(pixel_forward, std::memory_order_release);
            if (VirtualProtect(table + pixel_slot, sizeof(void*), PAGE_READWRITE, &old_protection)) {
                replaced = InterlockedCompareExchangePointer(
                    reinterpret_cast<PVOID volatile*>(table + pixel_slot),
                    reinterpret_cast<void*>(&observedCreatePixelShader),
                    reinterpret_cast<void*>(pixel_forward));
                VirtualProtect(table + pixel_slot, sizeof(void*), old_protection, &ignored);
                pixel_hooked = replaced == reinterpret_cast<void*>(pixel_forward);
            }
        }
        constexpr size_t sampler_slot = 23;
        bool sampler_hooked = false;
        auto sampler_forward = reinterpret_cast<CreateSamplerStateFn>(table[sampler_slot]);
        if (sampler_forward && sampler_forward != &observedCreateSamplerState) {
            original_create_sampler_state.store(sampler_forward, std::memory_order_release);
            if (VirtualProtect(table + sampler_slot, sizeof(void*), PAGE_READWRITE, &old_protection)) {
                replaced = InterlockedCompareExchangePointer(
                    reinterpret_cast<PVOID volatile*>(table + sampler_slot),
                    reinterpret_cast<void*>(&observedCreateSamplerState),
                    reinterpret_cast<void*>(sampler_forward));
                VirtualProtect(table + sampler_slot, sizeof(void*), old_protection, &ignored);
                sampler_hooked = replaced == reinterpret_cast<void*>(sampler_forward);
            }
        }
        constexpr size_t deferred_slot = 27;
        bool deferred_hooked = false;
        auto deferred_forward = reinterpret_cast<CreateDeferredContextFn>(table[deferred_slot]);
        if (deferred_forward && deferred_forward != &observedCreateDeferredContext) {
            original_create_deferred_context.store(deferred_forward, std::memory_order_release);
            if (VirtualProtect(table + deferred_slot, sizeof(void*), PAGE_READWRITE, &old_protection)) {
                replaced = InterlockedCompareExchangePointer(
                    reinterpret_cast<PVOID volatile*>(table + deferred_slot),
                    reinterpret_cast<void*>(&observedCreateDeferredContext),
                    reinterpret_cast<void*>(deferred_forward));
                VirtualProtect(table + deferred_slot, sizeof(void*), old_protection, &ignored);
                deferred_hooked = (replaced == reinterpret_cast<void*>(deferred_forward));
            }
        }
        EdpeLog(deferred_hooked
            ? L"EDPE: deferred context hook available"
            : L"EDPE: deferred context hook unavailable");

        EdpeLog(pixel_hooked
            ? L"EDPE: vertex/pixel shader signatures available"
            : L"EDPE: vertex shader signatures available; pixel shader hook unavailable");
        EdpeLog(sampler_hooked
            ? L"EDPE: sampler state hook available"
            : L"EDPE: sampler state hook unavailable");
        if (retain_vertex_bytecode.load(std::memory_order_relaxed))
            EdpeLog(L"EDPE: one-run vertex shader bytecode probe armed");
    } else EdpeLog(L"EDPE: vertex shader bytecode probe unavailable (slot changed)");
}

void processDeviceCreated(ID3D11Device* device, ID3D11DeviceContext* context, UINT flags, D3D_FEATURE_LEVEL featureLevel) {
    if (!device) return;

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> immContext;
    if (context) {
        immContext = context;
    } else {
        device->GetImmediateContext(&immContext);
    }

    if (ContextCensusIsDeviceRegistered(device)) {
        hookShaderCreation(device);
        if (immContext) ContextCensusHookContext(immContext.Get());
        return;
    }

    bool isPrimary = false;
    ID3D11Device* primaryPtr = nullptr;
    {
        std::lock_guard lock(s_device_track_mutex);
        if (!s_has_primary_device) {
            s_primary_device = device;
            s_primary_context = immContext.Get();
            s_has_primary_device = true;
            isPrimary = true;
            primaryPtr = device;
        } else {
            primaryPtr = s_primary_device;
            isPrimary = (device == primaryPtr);
        }
    }

    std::wstring adapterName = L"Unknown";
    LUID adapterLuid{};
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgiDevice.GetAddressOf()))) && dxgiDevice) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> dxgiAdapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&dxgiAdapter)) && dxgiAdapter) {
            DXGI_ADAPTER_DESC aDesc{};
            if (SUCCEEDED(dxgiAdapter->GetDesc(&aDesc))) {
                adapterName = aDesc.Description;
                adapterLuid = aDesc.AdapterLuid;
            }
        }
    }

    const D3D_FEATURE_LEVEL fl = (featureLevel != 0) ? featureLevel : device->GetFeatureLevel();
    const UINT devFlags = (flags != 0) ? flags : device->GetCreationFlags();

    if (isPrimary) {
        {
            std::lock_guard lock(s_device_track_mutex);
            s_primary_luid = adapterLuid;
        }
        wchar_t message[384];
        swprintf_s(message,
            L"EDPE: D3D11 device created device=%p context=%p adapter=\"%ls\" luid=%08x:%08x featureLevel=0x%04X flags=0x%X thread=%u",
            device, immContext.Get(), adapterName.c_str(),
            adapterLuid.HighPart, adapterLuid.LowPart,
            static_cast<unsigned>(fl), devFlags, GetCurrentThreadId());
        EdpeLog(message);
        EdpeLog(L"EDPE: registered primary D3D11 device");
    } else {
        static std::atomic<uint32_t> s_extra_device_logs{0};
        const uint32_t extra = s_extra_device_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (extra <= 4) {
            HMODULE selfMod = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&processDeviceCreated), &selfMod);
            void* stack[8]{};
            const USHORT frames = CaptureStackBackTrace(1, 8, stack, nullptr);
            std::wstring stackStr;
            std::wstring callerMod = L"unknown";
            for (USHORT i = 0; i < frames; ++i) {
                wchar_t frameHex[24];
                swprintf_s(frameHex, L"%p ", stack[i]);
                stackStr += frameHex;
                if (callerMod == L"unknown" && stack[i]) {
                    HMODULE mod = nullptr;
                    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(stack[i]), &mod) && mod && mod != selfMod) {
                        wchar_t modPath[MAX_PATH]{};
                        if (GetModuleFileNameW(mod, modPath, MAX_PATH) > 0) {
                            const wchar_t* base = wcsrchr(modPath, L'\\');
                            callerMod = base ? (base + 1) : modPath;
                        }
                    }
                }
            }
            wchar_t extraMsg[512];
            swprintf_s(extraMsg,
                L"EDPE: secondary D3D11 device #%u registered (device=%p, context=%p, primary=%p, adapter=\"%ls\" luid=%08x:%08x, featureLevel=0x%04X, flags=0x%X, caller=%ls, stack=[%ls])",
                extra, device, immContext.Get(), primaryPtr, adapterName.c_str(),
                adapterLuid.HighPart, adapterLuid.LowPart,
                static_cast<unsigned>(fl), devFlags, callerMod.c_str(), stackStr.c_str());
            EdpeLog(extraMsg);
            if (_wcsicmp(callerMod.c_str(), L"NvPresent64.dll") == 0) {
                if (!s_smooth_motion_detected.exchange(true, std::memory_order_acq_rel)) {
                    EdpeLog(L"EDPE: WARNING: NVIDIA Smooth Motion (NvPresent64.dll) secondary device detected; anchoring temporal pass on scene device and bypassing cross-device KeyedMutex bridge");
                }
            }
        } else if (GetModuleHandleW(L"NvPresent64.dll") != nullptr) {
            if (!s_smooth_motion_detected.exchange(true, std::memory_order_acq_rel)) {
                EdpeLog(L"EDPE: WARNING: NVIDIA Smooth Motion (NvPresent64.dll) secondary device detected; anchoring temporal pass on scene device and bypassing cross-device KeyedMutex bridge");
            }
        }
    }

    hookShaderCreation(device);
    ContextCensusRegisterDevice(device, immContext.Get());
    if (immContext) {
        ContextCensusHookContext(immContext.Get());
    }
}

namespace edpe {
bool isSmoothMotionDetected() {
    return s_smooth_motion_detected.load(std::memory_order_relaxed) ||
           GetModuleHandleW(L"NvPresent64.dll") != nullptr;
}
void installD3D11Hooks() {
    if (!edpe::isRunningInEliteProcess()) return;
    static std::atomic<bool> s_installed{false};
    if (s_installed.exchange(true)) return;

    HMODULE gameExe = GetModuleHandleW(nullptr);
    if (!gameExe) return;

    HMODULE selfMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&installD3D11Hooks), &selfMod);

    static void* s_orig_d3d11 = nullptr;
    if (patchIatEntry(gameExe, "d3d11.dll", "D3D11CreateDevice",
            reinterpret_cast<void*>(&EdpeD3D11CreateDevice), &s_orig_d3d11)) {
        EdpeLog(L"EDPE: D3D11CreateDevice IAT hook installed in main executable");
    }
    static void* s_orig_d3d11_sc = nullptr;
    if (patchIatEntry(gameExe, "d3d11.dll", "D3D11CreateDeviceAndSwapChain",
            reinterpret_cast<void*>(&EdpeD3D11CreateDeviceAndSwapChain), &s_orig_d3d11_sc)) {
        EdpeLog(L"EDPE: D3D11CreateDeviceAndSwapChain IAT hook installed in main executable");
    }

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me)) {
            do {
                if (!me.hModule || me.hModule == gameExe || me.hModule == selfMod) continue;
                if (_wcsicmp(me.szModule, L"d3d11.dll") == 0 ||
                    _wcsicmp(me.szModule, L"dxgi.dll") == 0 ||
                    _wcsicmp(me.szModule, L"ntdll.dll") == 0 ||
                    _wcsicmp(me.szModule, L"kernel32.dll") == 0 ||
                    _wcsicmp(me.szModule, L"kernelbase.dll") == 0) {
                    continue;
                }
                void* origDev = nullptr;
                if (patchIatEntry(me.hModule, "d3d11.dll", "D3D11CreateDevice",
                        reinterpret_cast<void*>(&EdpeD3D11CreateDevice), &origDev)) {
                    wchar_t msg[256];
                    swprintf_s(msg, L"EDPE: D3D11CreateDevice IAT hook installed in module %ls", me.szModule);
                    EdpeLog(msg);
                }
                void* origSc = nullptr;
                if (patchIatEntry(me.hModule, "d3d11.dll", "D3D11CreateDeviceAndSwapChain",
                        reinterpret_cast<void*>(&EdpeD3D11CreateDeviceAndSwapChain), &origSc)) {
                    wchar_t msg[256];
                    swprintf_s(msg, L"EDPE: D3D11CreateDeviceAndSwapChain IAT hook installed in module %ls", me.szModule);
                    EdpeLog(msg);
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }
}
}

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
    syncConfigOnce();
    const HMODULE system_d3d11 = resolveRealSystemD3D11();
    if (!system_d3d11) return HRESULT_FROM_WIN32(GetLastError());

    static const auto create_device = reinterpret_cast<decltype(&D3D11CreateDevice)>(
        GetProcAddress(system_d3d11, "D3D11CreateDevice"));
    if (!create_device) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    logDeviceCreationCall(L"D3D11CreateDevice", adapter, driver_type, flags,
        feature_levels, feature_level_count, sdk_version);

    const HRESULT result = create_device(adapter, driver_type, software, flags,
        feature_levels, feature_level_count, sdk_version, device, feature_level, context);
    if (SUCCEEDED(result) && device && *device) {
        edpe::setLastD3D11HResult(result);
        edpe::updateCrashGpuInfoFromDevice(*device);
        void** methods = *reinterpret_cast<void***>(*device);
        wchar_t message[160];
        swprintf_s(message, L"EDPE: D3D11 device created device=%p vtable=%p dsvMethod=%p",
            *device, methods, methods[10]);
        EdpeLog(message);
        hookShaderCreation(*device);
        processDeviceCreated(*device, (context && *context) ? *context : nullptr, flags,
            (feature_level && *feature_level) ? *feature_level : static_cast<D3D_FEATURE_LEVEL>(0));
    } else {
        edpe::setLastD3D11HResult(result);
        EdpeLog(SUCCEEDED(result) ? L"EDPE: D3D11 device created without output"
                                  : L"EDPE: D3D11 device creation failed");
    }
    return result;
}

extern "C" HRESULT WINAPI EdpeD3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
    IDXGISwapChain** ppSwapChain,
    ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext) {
    syncConfigOnce();
    const HMODULE system_d3d11 = resolveRealSystemD3D11();
    if (!system_d3d11) return HRESULT_FROM_WIN32(GetLastError());

    static const auto create_device_and_swap_chain = reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(
        GetProcAddress(system_d3d11, "D3D11CreateDeviceAndSwapChain"));
    if (!create_device_and_swap_chain) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    logDeviceCreationCall(L"D3D11CreateDeviceAndSwapChain", pAdapter, DriverType, Flags,
        pFeatureLevels, FeatureLevels, SDKVersion);

    const HRESULT result = create_device_and_swap_chain(pAdapter, DriverType, Software, Flags,
        pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext);
    if (SUCCEEDED(result)) {
        if (ppDevice && *ppDevice) {
            hookShaderCreation(*ppDevice);
            processDeviceCreated(*ppDevice, (ppImmediateContext && *ppImmediateContext) ? *ppImmediateContext : nullptr,
                Flags, (pFeatureLevel && *pFeatureLevel) ? *pFeatureLevel : static_cast<D3D_FEATURE_LEVEL>(0));
        }
        if (ppSwapChain && *ppSwapChain) {
            ObserveDxgiSwapChain(*ppSwapChain);
            wchar_t scMsg[192];
            swprintf_s(scMsg, L"EDPE: swapchain created swapchain=%p device=%p",
                static_cast<void*>(*ppSwapChain), static_cast<void*>(ppDevice ? *ppDevice : nullptr));
            EdpeLog(scMsg);
        }
    } else {
        EdpeLog(L"EDPE: D3D11CreateDeviceAndSwapChain failed");
    }
    return result;
}
