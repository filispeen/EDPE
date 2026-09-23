#include "log.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <d3d11_4.h>
#include <new>

namespace {
// ID3D11Device interface lengths and slots are from the Windows SDK 10.0.26100 headers.
constexpr size_t kCreateDepthStencilView = 10;
constexpr size_t kMaxMethods = 69;

struct HookTable {
    void** original;
    void* methods[kMaxMethods];
};

std::atomic<unsigned> depth_view_count{0};

HookTable* tableOf(ID3D11Device* device) {
    auto* methods = *reinterpret_cast<void***>(device);
    return reinterpret_cast<HookTable*>(reinterpret_cast<char*>(methods) - offsetof(HookTable, methods));
}

template <typename Interface>
bool supportsSamePointer(ID3D11Device* device) {
    Interface* candidate = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(Interface), reinterpret_cast<void**>(&candidate)))) return false;
    const bool same = candidate == device;
    candidate->Release();
    return same;
}

size_t deviceMethods(ID3D11Device* device) {
    if (supportsSamePointer<ID3D11Device5>(device)) return 69;
    if (supportsSamePointer<ID3D11Device4>(device)) return 67;
    if (supportsSamePointer<ID3D11Device3>(device)) return 65;
    if (supportsSamePointer<ID3D11Device2>(device)) return 54;
    if (supportsSamePointer<ID3D11Device1>(device)) return 50;
    return 43;
}

using CreateDepthStencilViewFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*,
    ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*, ID3D11DepthStencilView**);

HRESULT STDMETHODCALLTYPE observedCreateDepthStencilView(ID3D11Device* device,
    ID3D11Resource* resource, const D3D11_DEPTH_STENCIL_VIEW_DESC* desc,
    ID3D11DepthStencilView** view) {
    const auto original = reinterpret_cast<CreateDepthStencilViewFn>(
        tableOf(device)->original[kCreateDepthStencilView]);
    const HRESULT result = original(device, resource, desc, view);
    if (FAILED(result) || !view || !*view || !resource) return result;

    const unsigned index = depth_view_count.fetch_add(1, std::memory_order_relaxed);
    if (index >= 128) return result;

    ID3D11Texture2D* texture = nullptr;
    if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
            reinterpret_cast<void**>(&texture)))) return result;
    D3D11_TEXTURE2D_DESC texture_desc{};
    texture->GetDesc(&texture_desc);
    texture->Release();

    D3D11_DEPTH_STENCIL_VIEW_DESC view_desc{};
    (*view)->GetDesc(&view_desc);
    wchar_t message[320];
    swprintf_s(message,
        L"EDPE: DSV #%u view=%p resource=%p %ux%u textureFormat=%u viewFormat=%u bind=0x%X usage=%u samples=%u array=%u mips=%u dimension=%u flags=0x%X",
        index + 1, *view, resource, texture_desc.Width, texture_desc.Height,
        static_cast<unsigned>(texture_desc.Format), static_cast<unsigned>(view_desc.Format),
        texture_desc.BindFlags, static_cast<unsigned>(texture_desc.Usage),
        texture_desc.SampleDesc.Count, texture_desc.ArraySize, texture_desc.MipLevels,
        static_cast<unsigned>(view_desc.ViewDimension), view_desc.Flags);
    EdpeLog(message);
    return result;
}
} // namespace

void ObserveD3D11Device(ID3D11Device* device) {
    auto* table = new (std::nothrow) HookTable{};
    if (!table) {
        EdpeLog(L"EDPE: D3D11 depth-view observation disabled (allocation failed)");
        return;
    }
    table->original = *reinterpret_cast<void***>(device);
    const size_t method_count = deviceMethods(device);
    std::memcpy(table->methods, table->original, method_count * sizeof(void*));
    table->methods[kCreateDepthStencilView] = reinterpret_cast<void*>(&observedCreateDepthStencilView);
    // ponytail: keep one table per device until process exit; avoids racing concurrent Release calls.
    *reinterpret_cast<void***>(device) = table->methods;
    wchar_t message[128];
    swprintf_s(message, L"EDPE: D3D11 depth-view hook installed device=%p methods=%zu", device, method_count);
    EdpeLog(message);
}
