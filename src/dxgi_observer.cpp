#include "d3d11_proxy.h"
#include "config.h"
#include "crash_logger.h"
#include "log.h"
#include "system_dll.h"
#include "ui.h"
#include "context_census.h"
#include "fg_integration.h"
#include "frame_limiter.h"
#include "perf_overlay.h"
#include "vtable_hook.h"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <mutex>
#include <new>
#include <windows.h>

namespace {
constexpr size_t kCreateSwapChain = 10;
constexpr size_t kCreateSwapChainForHwnd = 15;
constexpr size_t kPresent = 8;
constexpr size_t kAddRef = 1;
constexpr size_t kRelease = 2;
constexpr size_t kResizeBuffers = 13;
constexpr size_t kMaxMethods = 41;

struct HookTable {
    void** original;
    void* rtti[2];
    void* methods[kMaxMethods];
    bool isSwapChain = false;
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
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
    const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

ULONG STDMETHODCALLTYPE observedRelease(IUnknown* object) {
    auto* table = tableOf(object);
    const bool isSwapChain = table->isSwapChain;
    *reinterpret_cast<void***>(object) = table->original;
    const auto original = reinterpret_cast<ReleaseFn>(table->original[kRelease]);
    const ULONG remaining = original(object);
    if (remaining) *reinterpret_cast<void***>(object) = table->methods;
    else {
        if (isSwapChain) {
            UiOnRelease(object);
            edpe::FgRuntime::instance().onSwapChainRelease(object);
            ContextCensusOnSwapChainRelease(object);
        }
        delete table;
    }
    return remaining;
}

HRESULT STDMETHODCALLTYPE observedResizeBuffers(IDXGISwapChain* swap_chain, UINT buffer_count,
    UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    UiOnResize(swap_chain);
    edpe::FgRuntime::instance().onResize(swap_chain);
    edpe::FrameLimiter::instance().reset();
    ContextCensusOnResize(swap_chain);
    const auto original = reinterpret_cast<ResizeBuffersFn>(tableOf(swap_chain)->original[kResizeBuffers]);
    return original(swap_chain, buffer_count, width, height, format, flags);
}

bool hookObject(IUnknown* object, size_t count, size_t method, void* replacement) {
    auto* table = new (std::nothrow) HookTable{};
    if (!table) return false;
    table->original = *reinterpret_cast<void***>(object);
    edpe::safeReadPtr(table->original - 2, &table->rtti[0]);
    edpe::safeReadPtr(table->original - 1, &table->rtti[1]);
    std::memcpy(table->methods, table->original, count * sizeof(void*));
    table->methods[kRelease] = reinterpret_cast<void*>(&observedRelease);
    table->methods[method] = replacement;
    *reinterpret_cast<void***>(object) = table->methods;
    return true;
}

void observeSwapChain(IDXGISwapChain* swap_chain);

ID3D11Device* hookDeviceIfD3D11(IUnknown* device) {
    if (!device) return nullptr;
    ID3D11Device* d3d_device = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(&d3d_device))) && d3d_device) {
        processDeviceCreated(d3d_device, nullptr, 0, static_cast<D3D_FEATURE_LEVEL>(0));
        d3d_device->Release();
    }
    return d3d_device;
}

HRESULT STDMETHODCALLTYPE observedCreateSwapChain(IDXGIFactory* factory, IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swap_chain) {
    ID3D11Device* d3d_dev = hookDeviceIfD3D11(device);
    const auto original = reinterpret_cast<CreateSwapChainFn>(tableOf(factory)->original[kCreateSwapChain]);
    const HRESULT result = original(factory, device, desc, swap_chain);
    if (SUCCEEDED(result) && swap_chain && *swap_chain) {
        wchar_t scMsg[192];
        swprintf_s(scMsg, L"EDPE: swapchain created swapchain=%p device=%p",
            static_cast<void*>(*swap_chain),
            d3d_dev ? static_cast<void*>(d3d_dev) : static_cast<void*>(device));
        EdpeLog(scMsg);
        observeSwapChain(*swap_chain);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE observedCreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    IDXGIOutput* restrict_to_output, IDXGISwapChain1** swap_chain) {
    ID3D11Device* d3d_dev = hookDeviceIfD3D11(device);
    const auto original = reinterpret_cast<CreateSwapChainForHwndFn>(tableOf(factory)->original[kCreateSwapChainForHwnd]);
    const HRESULT result = original(factory, device, hwnd, desc, fullscreen_desc, restrict_to_output, swap_chain);
    if (SUCCEEDED(result) && swap_chain && *swap_chain) {
        wchar_t scMsg[192];
        swprintf_s(scMsg, L"EDPE: swapchain created swapchain=%p device=%p",
            static_cast<void*>(*swap_chain),
            d3d_dev ? static_cast<void*>(d3d_dev) : static_cast<void*>(device));
        EdpeLog(scMsg);
        observeSwapChain(*swap_chain);
    }
    return result;
}

void logFirstPresent(IDXGISwapChain* swap_chain) {
    edpe::updateCrashGpuInfo(swap_chain);
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swap_chain->GetDesc(&desc))) return;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    if (SUCCEEDED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device)))) {
        hookShaderCreation(device);
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
    static IDXGISwapChain* s_logged_swapchains[8]{};
    static std::atomic<size_t> s_logged_swapchain_count{0};
    bool already_logged_sc = false;
    const size_t sc_count = s_logged_swapchain_count.load(std::memory_order_acquire);
    for (size_t i = 0; i < sc_count; ++i) {
        if (s_logged_swapchains[i] == swap_chain) {
            already_logged_sc = true;
            break;
        }
    }
    if (!already_logged_sc && sc_count < 8) {
        const size_t idx = s_logged_swapchain_count.fetch_add(1, std::memory_order_acq_rel);
        if (idx < 8) {
            s_logged_swapchains[idx] = swap_chain;
            ID3D11Device* presentDevice = nullptr;
            if (SUCCEEDED(swap_chain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&presentDevice))) && presentDevice) {
                processDeviceCreated(presentDevice, nullptr, 0, static_cast<D3D_FEATURE_LEVEL>(0));
                ContextCensusLogPresentDevice(swap_chain, presentDevice);
                presentDevice->Release();
            }
            logFirstPresent(swap_chain);
        }
    }
    const auto original = reinterpret_cast<PresentFn>(tableOf(swap_chain)->original[kPresent]);
    if (!ContextCensusSelectSwapChain(swap_chain, flags)) {
        const HRESULT result = original(swap_chain, sync_interval, flags);
        edpe::setLastD3D11HResult(result);
        return result;
    }
    if (edpe::isRunningInEliteProcess() && (flags & DXGI_PRESENT_TEST)) {
        const HRESULT result = original(swap_chain, sync_interval, flags);
        edpe::setLastD3D11HResult(result);
        return result;
    }
    const auto frame = present_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frame <= 8 || (edpe::Config::instance().devMode && frame <= 32768 && frame % 1024 == 0)) {
        logContextDispatch(swap_chain, frame);
    }
    edpe::PerfTracker::instance().onPresentStart(swap_chain);
    ContextCensusOnPresent(swap_chain, frame, flags);
    UiOnPresent(swap_chain, flags);
    ContextCensusAfterOverlay(swap_chain, flags);
    bool presentedViaFgProxy = false;
    edpe::FgRuntime::instance().setFrameLimitFps(edpe::FrameLimiter::instance().targetFps());
    edpe::FgRuntime::instance().onPresent(swap_chain, sync_interval, flags, &presentedViaFgProxy, UiMenuVisible());
    const HRESULT result = presentedViaFgProxy ? S_OK : original(swap_chain, sync_interval, flags);
    edpe::setLastD3D11HResult(result);
    ContextCensusAfterPresent(swap_chain, flags);
    if (!(presentedViaFgProxy && edpe::FgRuntime::instance().isReflexFrameLimiterActive())) {
        edpe::FrameLimiter::instance().waitForNextFrame();
    }
    edpe::PerfTracker::instance().onPresentEnd();
    return result;
}

void observeSwapChain(IDXGISwapChain* swap_chain) {
    if (!swap_chain) return;
    void** current_vtable = *reinterpret_cast<void***>(swap_chain);
    if (current_vtable && current_vtable[kPresent] == reinterpret_cast<void*>(&observedPresent)) {
        return;
    }
    if (!hookObject(swap_chain, swapChainMethods(swap_chain), kPresent,
            reinterpret_cast<void*>(&observedPresent))) {
        OutputDebugStringW(L"EDPE: swapchain observation disabled (allocation failed)\n");
    } else {
        auto* tbl = tableOf(swap_chain);
        tbl->isSwapChain = true;
        tbl->methods[kResizeBuffers] = reinterpret_cast<void*>(&observedResizeBuffers);
    }
}
} // namespace

void ObserveDxgiSwapChain(IDXGISwapChain* swap_chain) {
    observeSwapChain(swap_chain);
}

namespace {
std::atomic<CreateSwapChainFn> s_origWrappedCreateSwapChain{nullptr};
std::atomic<CreateSwapChainForHwndFn> s_origWrappedCreateSwapChainForHwnd{nullptr};
thread_local bool t_inRenderDocFactoryProbe = false;

HRESULT STDMETHODCALLTYPE observedWrappedCreateSwapChain(IDXGIFactory* factory, IUnknown* device,
    DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swap_chain) {
    hookDeviceIfD3D11(device);
    const auto original = s_origWrappedCreateSwapChain.load(std::memory_order_acquire);
    const HRESULT result = original ? original(factory, device, desc, swap_chain) : E_FAIL;
    if (SUCCEEDED(result) && swap_chain && *swap_chain) {
        wchar_t scMsg[192];
        swprintf_s(scMsg, L"EDPE: RenderDoc wrapped swapchain created swapchain=%p device=%p",
            static_cast<void*>(*swap_chain), static_cast<void*>(device));
        EdpeLog(scMsg);
        observeSwapChain(*swap_chain);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE observedWrappedCreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND hwnd,
    const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc,
    IDXGIOutput* restrict_to_output, IDXGISwapChain1** swap_chain) {
    hookDeviceIfD3D11(device);
    const auto original = s_origWrappedCreateSwapChainForHwnd.load(std::memory_order_acquire);
    const HRESULT result = original
        ? original(factory, device, hwnd, desc, fullscreen_desc, restrict_to_output, swap_chain)
        : E_FAIL;
    if (SUCCEEDED(result) && swap_chain && *swap_chain) {
        wchar_t scMsg[192];
        swprintf_s(scMsg, L"EDPE: RenderDoc wrapped swapchain created swapchain=%p device=%p",
            static_cast<void*>(*swap_chain), static_cast<void*>(device));
        EdpeLog(scMsg);
        observeSwapChain(*swap_chain);
    }
    return result;
}

bool patchWrappedFactorySlot(void** table, size_t slot, void* replacement, void** outOriginal) {
    if (!table || !replacement || !outOriginal) return false;
    void* current = table[slot];
    if (!current || current == replacement) return true;
    *outOriginal = current;
    DWORD oldProt = 0;
    if (!VirtualProtect(table + slot, sizeof(void*), PAGE_READWRITE, &oldProt)) return false;
    void* prev = InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(table + slot), replacement, current);
    DWORD ignored = 0;
    VirtualProtect(table + slot, sizeof(void*), oldProt, &ignored);
    return prev == current;
}
} // namespace

void EnsureRenderDocFactoryHooked() {
    if (t_inRenderDocFactoryProbe || !GetModuleHandleW(L"renderdoc.dll")) return;
    static std::once_flag s_once;
    std::call_once(s_once, [] {
        struct ProbeGuard {
            bool& flag;
            explicit ProbeGuard(bool& f) : flag(f) { flag = true; }
            ~ProbeGuard() { flag = false; }
        } guard(t_inRenderDocFactoryProbe);
        HMODULE system_dxgi = nullptr;
        wchar_t sysDxgiPath[MAX_PATH]{};
        const UINT sysLen = GetSystemDirectoryW(sysDxgiPath, MAX_PATH);
        if (sysLen > 0 && sysLen + 10 < MAX_PATH) {
            wcscpy_s(sysDxgiPath + sysLen, MAX_PATH - sysLen, L"\\dxgi.dll");
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, sysDxgiPath, &system_dxgi);
        }
        if (!system_dxgi) {
            system_dxgi = LoadSystemDll(L"dxgi.dll");
        }
        if (!system_dxgi) return;
        const auto create_factory = reinterpret_cast<decltype(&CreateDXGIFactory1)>(
            GetProcAddress(system_dxgi, "CreateDXGIFactory1"));
        if (!create_factory) return;

        IDXGIFactory1* probeFactory = nullptr;
        if (FAILED(create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&probeFactory))) ||
            !probeFactory) {
            return;
        }
        void** vt = *reinterpret_cast<void***>(probeFactory);
        if (vt && IsAddressInRenderDocModule(vt[kCreateSwapChain])) {
            HMODULE self = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(&observedWrappedCreateSwapChain), &self)) {
                void* origCs = nullptr;
                const bool okCs = patchWrappedFactorySlot(
                    vt, kCreateSwapChain, reinterpret_cast<void*>(&observedWrappedCreateSwapChain), &origCs);
                if (okCs && origCs) {
                    s_origWrappedCreateSwapChain.store(
                        reinterpret_cast<CreateSwapChainFn>(origCs), std::memory_order_release);
                }
                if (supportsSamePointer<IDXGIFactory2>(probeFactory)) {
                    void* origCsHwnd = nullptr;
                    const bool okHwnd = patchWrappedFactorySlot(
                        vt, kCreateSwapChainForHwnd,
                        reinterpret_cast<void*>(&observedWrappedCreateSwapChainForHwnd), &origCsHwnd);
                    if (okHwnd && origCsHwnd) {
                        s_origWrappedCreateSwapChainForHwnd.store(
                            reinterpret_cast<CreateSwapChainForHwndFn>(origCsHwnd), std::memory_order_release);
                    }
                }
                EdpeLog(okCs ? L"EDPE: RenderDoc WrappedIDXGIFactory CreateSwapChain hook armed"
                             : L"EDPE: RenderDoc WrappedIDXGIFactory hook failed");
            }
        }
        probeFactory->Release();
    });
}

void ObserveDxgiFactory(REFIID iid, void* factory) {
    if (!factory) return;
    if (iid != __uuidof(IDXGIFactory) && iid != __uuidof(IDXGIFactory1) &&
        iid != __uuidof(IDXGIFactory2) && iid != __uuidof(IDXGIFactory3) &&
        iid != __uuidof(IDXGIFactory4) && iid != __uuidof(IDXGIFactory5) &&
        iid != __uuidof(IDXGIFactory6) && iid != __uuidof(IDXGIFactory7)) return;
    auto* object = static_cast<IDXGIFactory1*>(factory);
    void** current_vtable = *reinterpret_cast<void***>(object);
    if (current_vtable && current_vtable[kCreateSwapChain] == reinterpret_cast<void*>(&observedCreateSwapChain)) {
        return;
    }
    const size_t method_count = factoryMethods(object);
    if (!hookObject(object, method_count, kCreateSwapChain,
            reinterpret_cast<void*>(&observedCreateSwapChain))) {
        OutputDebugStringW(L"EDPE: factory observation disabled (allocation failed)\n");
    } else if (method_count >= 25) {
        tableOf(object)->methods[kCreateSwapChainForHwnd] =
            reinterpret_cast<void*>(&observedCreateSwapChainForHwnd);
    }
}

extern "C" unsigned long long WINAPI EdpeObservedPresentCount() {
    return present_count.load(std::memory_order_relaxed);
}

extern "C" BOOL WINAPI EdpeMenuVisible() { return UiMenuVisible(); }
