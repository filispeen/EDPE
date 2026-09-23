#include "log.h"
#include "ui.h"
#include "context_census.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <new>
#include <windows.h>

namespace {
// COM vtable slots and interface lengths come from the Windows SDK DXGI headers.
constexpr size_t kCreateSwapChain = 10;
constexpr size_t kPresent = 8;
constexpr size_t kRelease = 2;
constexpr size_t kResizeBuffers = 13;
constexpr size_t kMaxMethods = 41;

struct HookTable {
    void** original;
    void* methods[kMaxMethods];
};

std::atomic<unsigned long long> present_count{0};

HookTable* tableOf(void* object) {
    auto* methods = *reinterpret_cast<void***>(object);
    return reinterpret_cast<HookTable*>(reinterpret_cast<char*>(methods) - offsetof(HookTable, methods));
}

template <typename Interface>
bool supportsSamePointer(IUnknown* object) {
    Interface* candidate = nullptr;
    if (FAILED(object->QueryInterface(__uuidof(Interface), reinterpret_cast<void**>(&candidate)))) return false;
    const bool same = candidate == object;
    candidate->Release();
    return same;
}

size_t factoryMethods(IUnknown* factory) {
    if (supportsSamePointer<IDXGIFactory7>(factory)) return 32;
    if (supportsSamePointer<IDXGIFactory6>(factory)) return 30;
    if (supportsSamePointer<IDXGIFactory5>(factory)) return 29;
    if (supportsSamePointer<IDXGIFactory4>(factory)) return 28;
    if (supportsSamePointer<IDXGIFactory3>(factory)) return 26;
    if (supportsSamePointer<IDXGIFactory2>(factory)) return 25;
    return 14;
}

size_t swapChainMethods(IUnknown* swap_chain) {
    if (supportsSamePointer<IDXGISwapChain4>(swap_chain)) return 41;
    if (supportsSamePointer<IDXGISwapChain3>(swap_chain)) return 40;
    if (supportsSamePointer<IDXGISwapChain2>(swap_chain)) return 36;
    if (supportsSamePointer<IDXGISwapChain1>(swap_chain)) return 29;
    return 18;
}

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

ULONG STDMETHODCALLTYPE observedRelease(IUnknown* object) {
    auto* table = tableOf(object);
    UiOnRelease(object);
    *reinterpret_cast<void***>(object) = table->original;
    const auto original = reinterpret_cast<ReleaseFn>(table->original[kRelease]);
    const ULONG remaining = original(object);
    if (remaining) *reinterpret_cast<void***>(object) = table->methods;
    else {
        ContextCensusOnSwapChainRelease(object);
        delete table;
    }
    return remaining;
}

HRESULT STDMETHODCALLTYPE observedResizeBuffers(IDXGISwapChain* swap_chain, UINT buffer_count,
    UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    UiOnResize(swap_chain);
    const auto original = reinterpret_cast<ResizeBuffersFn>(tableOf(swap_chain)->original[kResizeBuffers]);
    return original(swap_chain, buffer_count, width, height, format, flags);
}

bool hookObject(IUnknown* object, size_t count, size_t method, void* replacement) {
    auto* table = new (std::nothrow) HookTable{};
    if (!table) return false;
    table->original = *reinterpret_cast<void***>(object);
    std::memcpy(table->methods, table->original, count * sizeof(void*));
    table->methods[kRelease] = reinterpret_cast<void*>(&observedRelease);
    table->methods[method] = replacement;
    *reinterpret_cast<void***>(object) = table->methods;
    return true;
}

void observeSwapChain(IDXGISwapChain* swap_chain);

HRESULT STDMETHODCALLTYPE observedCreateSwapChain(IDXGIFactory* factory, IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swap_chain) {
    const auto original = reinterpret_cast<CreateSwapChainFn>(tableOf(factory)->original[kCreateSwapChain]);
    const HRESULT result = original(factory, device, desc, swap_chain);
    if (SUCCEEDED(result) && swap_chain && *swap_chain) observeSwapChain(*swap_chain);
    return result;
}

void logFirstPresent(IDXGISwapChain* swap_chain) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap_chain->GetDesc(&desc))) return;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (SUCCEEDED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
        device->GetImmediateContext(&context);
    }
    wchar_t message[256];
    void** device_methods = device ? *reinterpret_cast<void***>(device) : nullptr;
    swprintf_s(message, L"EDPE: Present swapchain=%p %ux%u format=%u device=%p context=%p vtable=%p dsvMethod=%p",
        swap_chain, desc.BufferDesc.Width, desc.BufferDesc.Height,
        static_cast<unsigned>(desc.BufferDesc.Format), device, context,
        device_methods, device_methods ? device_methods[10] : nullptr);
    EdpeLog(message);
    if (context) {
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        context->OMGetRenderTargets(1, &rtv, &dsv);
        D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
        if (rtv) rtv->GetDesc(&rtv_desc);
        D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
        D3D11_TEXTURE2D_DESC depth_desc{};
        if (dsv) {
            dsv->GetDesc(&dsv_desc);
            ID3D11Resource* resource = nullptr;
            dsv->GetResource(&resource);
            if (resource) {
                ID3D11Texture2D* texture = nullptr;
                if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),
                        reinterpret_cast<void**>(&texture)))) {
                    texture->GetDesc(&depth_desc);
                    texture->Release();
                }
                resource->Release();
            }
        }
        swprintf_s(message,
            L"EDPE: Present bindings rtv=%p rtvFormat=%u dsv=%p viewFormat=%u textureFormat=%u depth=%ux%u bind=0x%X samples=%u",
            rtv, static_cast<unsigned>(rtv_desc.Format), dsv,
            static_cast<unsigned>(dsv_desc.Format), static_cast<unsigned>(depth_desc.Format),
            depth_desc.Width, depth_desc.Height, depth_desc.BindFlags, depth_desc.SampleDesc.Count);
        EdpeLog(message);
        if (dsv) dsv->Release();
        if (rtv) rtv->Release();
    }
    if (context) context->Release();
    if (device) device->Release();
}

void logContextDispatch(IDXGISwapChain* swap_chain, unsigned long long frame) {
    ID3D11Device* device = nullptr;
    if (FAILED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) return;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (context) {
        void** methods = *reinterpret_cast<void***>(context);
        wchar_t message[256];
        swprintf_s(message,
            L"EDPE: context dispatch frame=%llu context=%p vtable=%p slot12=%p slot33=%p slot53=%p",
            frame, context, methods, methods[12], methods[33], methods[53]);
        EdpeLog(message);
        context->Release();
    }
    device->Release();
}

HRESULT STDMETHODCALLTYPE observedPresent(IDXGISwapChain* swap_chain, UINT sync_interval, UINT flags) {
    const auto frame = present_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frame == 1) logFirstPresent(swap_chain);
    if (frame <= 8 || (frame <= 32768 && frame % 1024 == 0)) logContextDispatch(swap_chain, frame);
    ContextCensusOnPresent(swap_chain, frame, flags);
    UiOnPresent(swap_chain, flags);
    ContextCensusAfterOverlay(swap_chain, flags);
    const auto original = reinterpret_cast<PresentFn>(tableOf(swap_chain)->original[kPresent]);
    return original(swap_chain, sync_interval, flags);
}

void observeSwapChain(IDXGISwapChain* swap_chain) {
    if (!hookObject(swap_chain, swapChainMethods(swap_chain), kPresent,
            reinterpret_cast<void*>(&observedPresent))) {
        OutputDebugStringW(L"EDPE: swapchain observation disabled (allocation failed)\n");
    } else {
        tableOf(swap_chain)->methods[kResizeBuffers] = reinterpret_cast<void*>(&observedResizeBuffers);
    }
}
} // namespace

void ObserveDxgiFactory(REFIID iid, void* factory) {
    if (iid != __uuidof(IDXGIFactory) && iid != __uuidof(IDXGIFactory1) &&
        iid != __uuidof(IDXGIFactory2) && iid != __uuidof(IDXGIFactory3) &&
        iid != __uuidof(IDXGIFactory4) && iid != __uuidof(IDXGIFactory5) &&
        iid != __uuidof(IDXGIFactory6) && iid != __uuidof(IDXGIFactory7)) return;
    auto* object = static_cast<IDXGIFactory1*>(factory);
    if (!hookObject(object, factoryMethods(object), kCreateSwapChain,
            reinterpret_cast<void*>(&observedCreateSwapChain))) {
        OutputDebugStringW(L"EDPE: factory observation disabled (allocation failed)\n");
    }
}

extern "C" unsigned long long WINAPI EdpeObservedPresentCount() {
    return present_count.load(std::memory_order_relaxed);
}

extern "C" BOOL WINAPI EdpeMenuVisible() { return UiMenuVisible(); }
