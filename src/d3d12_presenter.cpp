#include "d3d12_presenter.h"

#include "log.h"
#include "renderdoc_support.h"
#include "system_dll.h"

#include <cstdio>

namespace edpe {

namespace {

using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT Flags, REFIID riid, void** ppFactory);

LRESULT CALLBACK dummyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void logPresenterFailure(const wchar_t* stage, HRESULT hr) {
    wchar_t buf[256]{};
    std::swprintf(buf, 256, L"EDPE D3D12Presenter: %ls failed (hr=0x%08X)",
                  stage, static_cast<unsigned>(hr));
    EdpeLog(buf);
}

} // namespace

HiddenDummyWindow::~HiddenDummyWindow() {
    destroy();
}

bool HiddenDummyWindow::create() {
    destroy();
    hInstance_ = GetModuleHandleW(nullptr);
    wchar_t className[64]{};
    std::swprintf(className, 64, L"EDPE_DummyD3D11Wnd_%p", static_cast<void*>(this));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = dummyWndProc;
    wc.hInstance = hInstance_;
    wc.lpszClassName = className;
    classAtom_ = RegisterClassExW(&wc);
    if (!classAtom_) return false;

    // WS_POPUP | WS_DISABLED + SW_HIDE prevents Wine/Wayland/X11 and Windows focus stealing.
    const LPCWSTR atomName = reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(classAtom_));
    hwnd_ = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        atomName,
        L"EDPE Dummy",
        WS_POPUP | WS_DISABLED,
        0, 0, 1, 1,
        nullptr, nullptr, hInstance_, nullptr);
    if (!hwnd_) {
        UnregisterClassW(atomName, hInstance_);
        classAtom_ = 0;
        return false;
    }
    ShowWindow(hwnd_, SW_HIDE);
    return true;
}

void HiddenDummyWindow::destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (classAtom_ && hInstance_) {
        UnregisterClassW(reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(classAtom_)), hInstance_);
        classAtom_ = 0;
    }
    hInstance_ = nullptr;
}

D3D12ProxyPresenter::~D3D12ProxyPresenter() {
    shutdown();
}

bool D3D12ProxyPresenter::isCrossApiPresentBlockedByRenderDoc() {
    // Windows renderdoc.dll cannot track mid-frame cross-API OpenSharedHandle updates;
    // Linux/Proton Vulkan capture (librenderdoc.so) attaches below DXVK/vkd3d-proton.
    return GetModuleHandleW(L"renderdoc.dll") != nullptr;
}

bool D3D12ProxyPresenter::initialize(ID3D11Device* d3d11Device,
                                     HWND targetHwnd,
                                     uint32_t width,
                                     uint32_t height,
                                     DXGI_FORMAT format,
                                     uint32_t bufferCount,
                                     UINT flags) {
    std::unique_lock lock(presentResizeMutex_);
    shutdown();

    if (!d3d11Device || !targetHwnd || !IsWindow(targetHwnd) || !width || !height) {
        return false;
    }

    if (!bridge_.initialize(d3d11Device)) {
        return false;
    }

    // Resolve system DXGI directly (System32 on Windows, DXVK/vkd3d-proton under Wine)
    // so creating the D3D12 swapchain never recurses into EDPE's own dxgi.dll exports.
    HMODULE sysDxgi = LoadSystemDll(L"dxgi.dll");
    if (!sysDxgi) {
        logPresenterFailure(L"LoadSystemDll(dxgi.dll)", HRESULT_FROM_WIN32(GetLastError()));
        bridge_.shutdown();
        return false;
    }

    auto createFactory2 = reinterpret_cast<PFN_CreateDXGIFactory2>(
        GetProcAddress(sysDxgi, "CreateDXGIFactory2"));
    if (!createFactory2) {
        logPresenterFailure(L"GetProcAddress(CreateDXGIFactory2)", HRESULT_FROM_WIN32(GetLastError()));
        bridge_.shutdown();
        return false;
    }

    HRESULT hr = createFactory2(0, IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) {
        logPresenterFailure(L"CreateDXGIFactory2", hr);
        bridge_.shutdown();
        return false;
    }

    BOOL allowTearing = FALSE;
    Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory_.As(&factory5))) {
        if (FAILED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing)))) {
            allowTearing = FALSE;
        }
    }
    tearingSupported_ = (allowTearing == TRUE);

    const DXGI_SWAP_CHAIN_DESC1 scDesc = sanitizeFlipSwapChainDesc(
        width, height, format, bufferCount, flags, tearingSupported_);

    Microsoft::WRL::ComPtr<IDXGISwapChain1> sc1;
    hr = factory_->CreateSwapChainForHwnd(
        bridge_.commandQueue(),
        targetHwnd,
        &scDesc,
        nullptr,
        nullptr,
        &sc1);
    if (FAILED(hr)) {
        logPresenterFailure(L"CreateSwapChainForHwnd(D3D12)", hr);
        factory_.Reset();
        bridge_.shutdown();
        return false;
    }

    // Keep Alt+Enter handling owned by the game/D3D11 window path rather than DXGI auto-fullscreen.
    factory_->MakeWindowAssociation(targetHwnd, DXGI_MWA_NO_ALT_ENTER);

    hr = sc1.As(&swapChain_);
    if (FAILED(hr)) {
        logPresenterFailure(L"As(IDXGISwapChain4)", hr);
        sc1.Reset();
        factory_.Reset();
        bridge_.shutdown();
        return false;
    }

    targetHwnd_ = targetHwnd;
    width_ = scDesc.Width;
    height_ = scDesc.Height;
    format_ = scDesc.Format;
    bufferCount_ = scDesc.BufferCount;
    swapChainFlags_ = scDesc.Flags;

    slotPresentFence_.fill(0);
    if (!acquireBackBuffers() || !ensureSharedInput(width_, height_, format_) ||
        !ensureTimestampQueries()) {
        releaseBackBuffers();
        for (auto& s : sharedInputs_) {
            s.reset();
        }
        if (mappedTimestamps_ && queryReadback_) {
            const D3D12_RANGE emptyRange{0, 0};
            queryReadback_->Unmap(0, &emptyRange);
            mappedTimestamps_ = nullptr;
        }
        queryReadback_.Reset();
        queryHeap_.Reset();
        swapChain_.Reset();
        factory_.Reset();
        bridge_.shutdown();
        return false;
    }

    swapChain_->SetColorSpace1(colorSpace_);
    initialized_ = true;
    return true;
}

void D3D12ProxyPresenter::shutdown() {
    // Caller may hold presentResizeMutex_ (initialize) or call externally; guard safely.
    bridge_.waitGpuIdle();
    releaseBackBuffers();
    for (auto& s : sharedInputs_) {
        s.reset();
    }
    slotPresentFence_.fill(0);
    if (mappedTimestamps_ && queryReadback_) {
        const D3D12_RANGE emptyRange{0, 0};
        queryReadback_->Unmap(0, &emptyRange);
        mappedTimestamps_ = nullptr;
    }
    queryReadback_.Reset();
    queryHeap_.Reset();
    slotHasTimestamp_.fill(false);
    swapChain_.Reset();
    factory_.Reset();
    bridge_.shutdown();
    targetHwnd_ = nullptr;
    width_ = 0;
    height_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;
    bufferCount_ = 0;
    swapChainFlags_ = 0;
    tearingSupported_ = false;
    lastPresentDoneFence_ = 0;
    initialized_ = false;
}

bool D3D12ProxyPresenter::acquireBackBuffers() {
    releaseBackBuffers();
    if (!swapChain_ || !bufferCount_) return false;
    backBuffers_.resize(bufferCount_);
    for (uint32_t i = 0; i < bufferCount_; ++i) {
        const HRESULT hr = swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i]));
        if (FAILED(hr)) {
            logPresenterFailure(L"IDXGISwapChain4::GetBuffer", hr);
            releaseBackBuffers();
            return false;
        }
        char name[64]{};
        std::snprintf(name, sizeof(name), "EDPE_D3D12SwapBuffer_%u", i);
        SetD3D12DebugName(backBuffers_[i].Get(), name);
    }
    return true;
}

void D3D12ProxyPresenter::releaseBackBuffers() {
    backBuffers_.clear();
}

bool D3D12ProxyPresenter::ensureSharedInput(uint32_t width, uint32_t height, DXGI_FORMAT format) {
    bool allValid = true;
    for (const auto& s : sharedInputs_) {
        if (!s.isValid() || s.width != width || s.height != height || s.format != format) {
            allValid = false;
            break;
        }
    }
    if (allValid) {
        return true;
    }
    for (auto& s : sharedInputs_) {
        s.reset();
    }
    for (uint32_t i = 0; i < D3D11D3D12Bridge::kAllocatorCount; ++i) {
        if (!bridge_.createSharedTexture(
                width, height, format,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                sharedInputs_[i])) {
            for (auto& s : sharedInputs_) {
                s.reset();
            }
            return false;
        }
        char n11[64]{};
        char n12[64]{};
        std::snprintf(n11, sizeof(n11), "EDPE_PresenterSharedIn_%u_D3D11", i);
        std::snprintf(n12, sizeof(n12), "EDPE_PresenterSharedIn_%u_D3D12", i);
        SetD3D11DebugName(sharedInputs_[i].d3d11.Get(), n11);
        SetD3D12DebugName(sharedInputs_[i].d3d12.Get(), n12);
    }
    return true;
}

bool D3D12ProxyPresenter::ensureTimestampQueries() {
    if (queryHeap_ && queryReadback_ && mappedTimestamps_) return true;
    auto* dev = bridge_.d3d12Device();
    if (!dev) return false;

    if (mappedTimestamps_ && queryReadback_) {
        const D3D12_RANGE emptyRange{0, 0};
        queryReadback_->Unmap(0, &emptyRange);
        mappedTimestamps_ = nullptr;
    }
    queryReadback_.Reset();
    queryHeap_.Reset();

    constexpr uint32_t kSlots = D3D11D3D12Bridge::kAllocatorCount;
    D3D12_QUERY_HEAP_DESC qh{};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = kSlots * 2;
    if (FAILED(dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&queryHeap_)))) {
        return false;
    }

    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width = sizeof(uint64_t) * kSlots * 2;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(
            &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&queryReadback_)))) {
        queryHeap_.Reset();
        return false;
    }

    const D3D12_RANGE fullReadRange{0, sizeof(uint64_t) * kSlots * 2};
    void* mapped = nullptr;
    if (FAILED(queryReadback_->Map(0, &fullReadRange, &mapped)) || !mapped) {
        queryReadback_.Reset();
        queryHeap_.Reset();
        return false;
    }
    mappedTimestamps_ = static_cast<uint64_t*>(mapped);
    slotHasTimestamp_.fill(false);
    return true;
}

bool D3D12ProxyPresenter::resizeBuffers(uint32_t bufferCount,
                                        uint32_t width,
                                        uint32_t height,
                                        DXGI_FORMAT format,
                                        UINT flags) {
    std::unique_lock lock(presentResizeMutex_);
    if (!initialized_ || !swapChain_ || !width || !height) return false;

    bridge_.waitGpuIdle();
    lastPresentDoneFence_ = 0;
    slotPresentFence_.fill(0);
    releaseBackBuffers();
    for (auto& s : sharedInputs_) {
        s.reset();
    }
    slotHasTimestamp_.fill(false);

    const DXGI_SWAP_CHAIN_DESC1 nextDesc = sanitizeFlipSwapChainDesc(
        width, height,
        format != DXGI_FORMAT_UNKNOWN ? format : format_,
        bufferCount > 0 ? bufferCount : bufferCount_,
        flags,
        tearingSupported_);

    const HRESULT hr = swapChain_->ResizeBuffers(
        nextDesc.BufferCount,
        nextDesc.Width,
        nextDesc.Height,
        nextDesc.Format,
        nextDesc.Flags);
    if (FAILED(hr)) {
        logPresenterFailure(L"IDXGISwapChain4::ResizeBuffers", hr);
        return false;
    }

    width_ = nextDesc.Width;
    height_ = nextDesc.Height;
    format_ = nextDesc.Format;
    bufferCount_ = nextDesc.BufferCount;
    swapChainFlags_ = nextDesc.Flags;

    if (!acquireBackBuffers() || !ensureSharedInput(width_, height_, format_)) {
        return false;
    }

    swapChain_->SetColorSpace1(colorSpace_);
    return true;
}

bool D3D12ProxyPresenter::setColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) {
    std::unique_lock lock(presentResizeMutex_);
    colorSpace_ = colorSpace;
    if (!initialized_ || !swapChain_) return false;
    return SUCCEEDED(swapChain_->SetColorSpace1(colorSpace_));
}

ID3D12Resource* D3D12ProxyPresenter::currentBackBuffer() const {
    if (!initialized_ || !swapChain_) return nullptr;
    const UINT idx = swapChain_->GetCurrentBackBufferIndex();
    return (idx < backBuffers_.size()) ? backBuffers_[idx].Get() : nullptr;
}

bool D3D12ProxyPresenter::waitForLastPresentOnD3D11(ID3D11DeviceContext4* ctx4) const {
    if (!initialized_ || !ctx4 || lastPresentDoneFence_ == 0) return true;
    return bridge_.waitOnD3D11(ctx4, lastPresentDoneFence_);
}

void D3D12ProxyPresenter::waitForSharedSlotOnD3D11(ID3D11DeviceContext4* ctx4, uint32_t slot) const {
    if (!initialized_ || !ctx4 || slot >= D3D11D3D12Bridge::kAllocatorCount) return;
    const uint64_t fence = slotPresentFence_[slot];
    if (fence != 0) {
        bridge_.waitOnD3D11(ctx4, fence);
    }
}

IDXGISwapChain4* D3D12ProxyPresenter::detachSwapChainForWrap() {
    bridge_.waitGpuIdle();
    releaseBackBuffers();
    return swapChain_.Detach();
}

bool D3D12ProxyPresenter::attachWrappedSwapChain(IDXGISwapChain4* wrappedSwapChain) {
    bridge_.waitGpuIdle();
    releaseBackBuffers();
    if (wrappedSwapChain) {
        swapChain_.Attach(wrappedSwapChain);
    }
    if (!acquireBackBuffers()) {
        return false;
    }
    if (swapChain_) {
        swapChain_->SetColorSpace1(colorSpace_);
    }
    return true;
}

bool D3D12ProxyPresenter::recreatePlainSwapChain() {
    std::unique_lock lock(presentResizeMutex_);
    if (!initialized_ || !factory_ || !targetHwnd_ || !bridge_.commandQueue()) return false;
    bridge_.waitGpuIdle();
    releaseBackBuffers();
    swapChain_.Reset();

    const DXGI_SWAP_CHAIN_DESC1 scDesc = sanitizeFlipSwapChainDesc(
        width_, height_, format_, bufferCount_, swapChainFlags_, tearingSupported_);
    Microsoft::WRL::ComPtr<IDXGISwapChain1> sc1;
    const HRESULT hr = factory_->CreateSwapChainForHwnd(
        bridge_.commandQueue(), targetHwnd_, &scDesc, nullptr, nullptr, &sc1);
    if (FAILED(hr) || FAILED(sc1.As(&swapChain_))) {
        return false;
    }
    factory_->MakeWindowAssociation(targetHwnd_, DXGI_MWA_NO_ALT_ENTER);
    if (!acquireBackBuffers()) return false;
    swapChain_->SetColorSpace1(colorSpace_);
    return true;
}

bool D3D12ProxyPresenter::presentFromD3D11Texture(ID3D11DeviceContext4* ctx4,
                                                  ID3D11Texture2D* srcD3D11,
                                                  UINT syncInterval,
                                                  UINT flags,
                                                  HRESULT* outPresentHr,
                                                  double* outGpuCopyMs,
                                                  bool callerWaitedForSlot) {
    if (outPresentHr) *outPresentHr = E_FAIL;
    if (outGpuCopyMs) *outGpuCopyMs = 0.0;

    std::shared_lock lock(presentResizeMutex_);
    const uint32_t expectedSlot = bridge_.nextAllocatorSlot();
    if (!initialized_ || !ctx4 || !srcD3D11 || !swapChain_ ||
        !sharedInputs_[expectedSlot].isValid()) {
        return false;
    }

    const UINT bbIdx = swapChain_->GetCurrentBackBufferIndex();
    if (bbIdx >= backBuffers_.size() || !backBuffers_[bbIdx]) return false;
    ID3D12Resource* backBuffer = backBuffers_[bbIdx].Get();

    // 1. Wait on GPU for prior D3D12 Present/FG reads on this ring slot before overwriting it.
    if (!callerWaitedForSlot && slotPresentFence_[expectedSlot] != 0) {
        bridge_.waitOnD3D11(ctx4, slotPresentFence_[expectedSlot]);
    }
    ctx4->CopyResource(sharedInputs_[expectedSlot].d3d11.Get(), srcD3D11);

    // 2. Open D3D12 command list and read prior timestamp for this allocator ring slot non-blockingly.
    uint32_t slot = 0;
    auto* cmdList = bridge_.beginCommandList(slot);
    if (!cmdList) return false;

    const bool recordTiming = telemetryTimingEnabled_.load(std::memory_order_relaxed);
    if (outGpuCopyMs && slotHasTimestamp_[slot] && mappedTimestamps_ && bridge_.timestampFrequency() > 0) {
        const uint64_t t0 = mappedTimestamps_[slot * 2];
        const uint64_t t1 = mappedTimestamps_[slot * 2 + 1];
        if (t1 >= t0) {
            *outGpuCopyMs = static_cast<double>(t1 - t0) /
                            (static_cast<double>(bridge_.timestampFrequency()) / 1000.0);
        }
    }

    {
        ScopedD3D12RenderDocEvent ev(cmdList, L"EDPE: D3D12ProxyPresentBlit");
        if (recordTiming && queryHeap_ && queryReadback_) {
            cmdList->EndQuery(queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
        }

        if (prePresentCb_) {
            prePresentCb_(cmdList, prePresentUserCtx_);
        }

        ID3D12Resource* sharedD3D12 = sharedInputs_[slot].d3d12.Get();
        D3D11D3D12Bridge::transitionResources2(
            cmdList,
            sharedD3D12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE,
            backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);

        cmdList->CopyResource(backBuffer, sharedD3D12);

        D3D11D3D12Bridge::transitionResources2(
            cmdList,
            backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT,
            sharedD3D12, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);

        if (recordTiming && queryHeap_ && queryReadback_) {
            cmdList->EndQuery(queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
            cmdList->ResolveQueryData(
                queryHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                slot * 2, 2,
                queryReadback_.Get(), sizeof(uint64_t) * slot * 2);
            slotHasTimestamp_[slot] = true;
        } else {
            slotHasTimestamp_[slot] = false;
        }
    }

    if (bridge_.submitAfterD3D11Signal(ctx4, slot, true) == 0) {
        return false;
    }

    UINT presentFlags = flags;
    if (syncInterval == 0 && tearingSupported_ && !(presentFlags & DXGI_PRESENT_TEST)) {
        presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
    } else if (!tearingSupported_) {
        presentFlags &= ~DXGI_PRESENT_ALLOW_TEARING;
    }

    if (prePresentMarkerCb_) {
        prePresentMarkerCb_(presentMarkerUserCtx_);
    }
    const HRESULT presentHr = swapChain_->Present(syncInterval, presentFlags);
    if (postPresentMarkerCb_) {
        postPresentMarkerCb_(presentMarkerUserCtx_);
    }
    const uint64_t postPresentFence = bridge_.signalD3D12Queue();
    if (postPresentFence != 0) {
        slotPresentFence_[slot] = postPresentFence;
        lastPresentDoneFence_ = postPresentFence;
    }
    if (outPresentHr) *outPresentHr = presentHr;
    if (FAILED(presentHr)) {
        logPresenterFailure(L"IDXGISwapChain4::Present", presentHr);
        return false;
    }
    return true;
}

} // namespace edpe
