#include "log.h"
#include "shader_probe.h"
#include "system_dll.h"

#include <d3d11.h>
#include <atomic>
#include <cwchar>
#include <windows.h>

namespace {
using CreateVertexShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*,
    SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*,
    SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
std::atomic<CreateVertexShaderFn> original_create_vertex_shader{nullptr};
std::atomic<CreatePixelShaderFn> original_create_pixel_shader{nullptr};
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
    return DeleteFileW(path) != 0; // Consume the marker before installing any hook.
}

void hookShaderCreation(ID3D11Device* device) {
    void** table = *reinterpret_cast<void***>(device);
    if (hooked_device_table.load(std::memory_order_acquire)) return;
    retain_vertex_bytecode.store(armShaderProbe(), std::memory_order_relaxed);
    // COM vtable callbacks must stay valid even if the caller frees this proxy.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&observedCreateVertexShader), &self)) {
        EdpeLog(L"EDPE: shader signatures unavailable (proxy lifetime cannot be pinned)");
        return;
    }
    constexpr size_t slot = 12; // ID3D11Device::CreateVertexShader.
    constexpr size_t pixel_slot = 15; // ID3D11Device::CreatePixelShader.
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
        EdpeLog(pixel_hooked
            ? L"EDPE: vertex/pixel shader signatures available"
            : L"EDPE: vertex shader signatures available; pixel shader hook unavailable");
        if (retain_vertex_bytecode.load(std::memory_order_relaxed))
            EdpeLog(L"EDPE: one-run vertex shader bytecode probe armed");
    } else EdpeLog(L"EDPE: vertex shader bytecode probe unavailable (slot changed)");
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
