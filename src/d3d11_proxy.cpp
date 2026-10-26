#include "d3d11_proxy.h"
#include "context_census.h"
#include "config.h"
#include "input_hook.h"
#include "log.h"
#include "shader_probe.h"
#include "system_dll.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <d3d11.h>
#include <mutex>
#include <set>
#include <tuple>
#include <windows.h>

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

HRESULT STDMETHODCALLTYPE observedCreateVertexShader(ID3D11Device* device,
    const void* bytecode, SIZE_T size, ID3D11ClassLinkage* linkage,
    ID3D11VertexShader** shader) {
    const HRESULT result = original_create_vertex_shader.load(std::memory_order_acquire)(
        device, bytecode, size, linkage, shader);
    if (SUCCEEDED(result) && shader && *shader && bytecode && size && size <= 65536) {
        if (retain_vertex_bytecode.load(std::memory_order_relaxed))
            (*shader)->SetPrivateData(kEdpeVertexBytecodeGuid, static_cast<UINT>(size), bytecode);
        const auto hash = EdpeEdvrShaderHash(bytecode, size);
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
        const auto hash = EdpeEdvrShaderHash(bytecode, size);
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

void hookShaderCreation(ID3D11Device* device) {
    if (!device) return;
    void** table = *reinterpret_cast<void***>(device);
    if (hooked_device_table.load(std::memory_order_acquire)) return;
    retain_vertex_bytecode.store(armShaderProbe(), std::memory_order_relaxed);
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
    if (!forward || forward == &observedCreateVertexShader) return;
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

namespace edpe {
void installD3D11Hooks() {
    static std::atomic<bool> s_installed{false};
    if (s_installed.exchange(true)) return;

    HMODULE gameExe = GetModuleHandleW(nullptr);
    if (!gameExe) return;

    static void* s_orig_d3d11 = nullptr;
    if (patchIatEntry(gameExe, "d3d11.dll", "D3D11CreateDevice",
            reinterpret_cast<void*>(&EdpeD3D11CreateDevice), &s_orig_d3d11)) {
        EdpeLog(L"EDPE: D3D11CreateDevice IAT hook installed in main executable");
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
        hookShaderCreation(*device);
    } else {
        EdpeLog(SUCCEEDED(result) ? L"EDPE: D3D11 device created without output"
                                  : L"EDPE: D3D11 device creation failed");
    }
    return result;
}
