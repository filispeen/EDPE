#include "fg_integration.h"

#include "amd_fsr_fg_backend.h"
#include "context_census.h"
#include "log.h"
#include "nvidia_sl_fg_backend.h"
#include "renderdoc_support.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace edpe {

namespace {

constexpr UINT kWmEdpeFgCreateChild = WM_APP + 0x4E1;
constexpr UINT kWmEdpeFgResizeChild = WM_APP + 0x4E2;
constexpr UINT kWmEdpeFgDestroyChild = WM_APP + 0x4E3;

struct CreateChildParams {
    HWND parent = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    HINSTANCE hInstance = nullptr;
    const wchar_t* className = nullptr;
    HWND outHwnd = nullptr;
};

constexpr const char kDepthExtractCsHlsl[] = R"hlsl(
Texture2D<float4> srcDepth : register(t0);
RWTexture2D<float> dstDepth : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint w, h;
    dstDepth.GetDimensions(w, h);
    if (tid.x < w && tid.y < h) {
        dstDepth[tid.xy] = srcDepth.Load(int3(tid.xy, 0)).r;
    }
}
)hlsl";

LRESULT CALLBACK edpeOverlayChildWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST:
        // Keep the child overlay completely click-through so game & ImGui inputs stay on parent.
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

FrameGenerationBackend intToFgBackend(int v) {
    switch (v) {
    case 1: return FrameGenerationBackend::AmdFsrFg;
    case 2: return FrameGenerationBackend::NvidiaDlssG;
    default: return FrameGenerationBackend::Off;
    }
}

DXGI_FORMAT canonicalNonSrgbFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    default:
        return fmt;
    }
}

bool areCopyCompatible(DXGI_FORMAT a, DXGI_FORMAT b) {
    return canonicalNonSrgbFormat(a) == canonicalNonSrgbFormat(b);
}

struct EvalHookCtx {
    FrameGenerationManager* manager = nullptr;
    FrameGenerationInputs inputs{};
    bool enabled = false;
    bool evalOk = false;
};

bool prePresentFgCallback(ID3D12GraphicsCommandList* cmdList, void* userCtx) {
    auto* ctx = static_cast<EvalHookCtx*>(userCtx);
    if (!ctx || !ctx->manager) return false;
    ctx->evalOk = ctx->manager->evaluate(cmdList, ctx->inputs, ctx->enabled);
    return ctx->evalOk;
}

} // namespace

FgRuntime& FgRuntime::instance() {
    static FgRuntime s_rt;
    return s_rt;
}

FgRuntime::FgRuntime() {
    auto amd = std::make_unique<AmdFsrFgBackend>();
    amdDllAvailable_ = amd->isAvailable();
    if (!amdDllAvailable_) {
        std::wcsncpy(amdUnavailableReason_, amd->unavailableReason(), 255);
    }
    manager_.registerBackend(std::move(amd));

    auto sl = std::make_unique<NvidiaStreamlineFgBackend>();
    dlssgCapAvailable_ = sl->isAvailable();
    if (!dlssgCapAvailable_) {
        std::wcsncpy(dlssgUnavailableReason_, sl->unavailableReason(), 255);
    }
    manager_.registerBackend(std::move(sl));
}

FgRuntime::~FgRuntime() {
    shutdown();
}

void FgRuntime::applyConfig(int backendInt,
                            int multiplier,
                            bool dynamicMfg,
                            float dynamicTargetFps,
                            bool reflexBoost) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const FrameGenerationBackend nextBackend = intToFgBackend(backendInt);
    if (nextBackend != configuredBackend_) {
        // Clear prior init-failure latch when user switches backend selection.
        failedBackend_ = FrameGenerationBackend::Off;
        telemetry_.initFailed = false;
    }
    configuredBackend_ = nextBackend;
    requestedMultiplier_ = static_cast<uint32_t>((std::max)(2, (std::min)(multiplier, 4)));
    dynamicMfg_ = dynamicMfg;
    dynamicTargetFps_ = dynamicTargetFps;
    reflexBoost_ = reflexBoost;
    telemetry_.configuredBackend = configuredBackend_;

    if (auto* b = manager_.getBackend(FrameGenerationBackend::NvidiaDlssG)) {
        auto* sl = static_cast<NvidiaStreamlineFgBackend*>(const_cast<IFrameGenerationBackend*>(b));
        sl->setLowLatencyBoost(reflexBoost_);
        sl->setDynamicMfgMode(dynamicMfg_, dynamicTargetFps_);
        sl->setDebugView(fgDebugView_);
    }
    if (auto* b = manager_.getBackend(FrameGenerationBackend::AmdFsrFg)) {
        auto* amd = static_cast<AmdFsrFgBackend*>(const_cast<IFrameGenerationBackend*>(b));
        amd->setDebugFlags(false, false, fgDebugView_);
    }
}

void FgRuntime::setFgDebugView(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    fgDebugView_ = enabled;
    if (auto* b = manager_.getBackend(FrameGenerationBackend::AmdFsrFg)) {
        auto* amd = static_cast<AmdFsrFgBackend*>(const_cast<IFrameGenerationBackend*>(b));
        amd->setDebugFlags(false, false, fgDebugView_);
    }
    if (auto* b = manager_.getBackend(FrameGenerationBackend::NvidiaDlssG)) {
        auto* sl = static_cast<NvidiaStreamlineFgBackend*>(const_cast<IFrameGenerationBackend*>(b));
        sl->setDebugView(fgDebugView_);
    }
}

bool FgRuntime::fgDebugView() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return fgDebugView_;
}

bool FgRuntime::isMotionNeeded() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return configuredBackend_ != FrameGenerationBackend::Off &&
           configuredBackend_ != failedBackend_ &&
           !D3D12ProxyPresenter::isCrossApiPresentBlockedByRenderDoc();
}

bool FgRuntime::handleWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* outResult) {
    if (msg == kWmEdpeFgCreateChild) {
        auto* p = reinterpret_cast<CreateChildParams*>(lp);
        if (p && p->parent == hwnd && p->className && p->width > 0 && p->height > 0) {
            p->outHwnd = CreateWindowExW(
                WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY | WS_EX_NOACTIVATE,
                p->className,
                L"EDPE_FgOverlay",
                WS_CHILD | WS_VISIBLE | WS_DISABLED,
                0, 0, static_cast<int>(p->width), static_cast<int>(p->height),
                p->parent, nullptr, p->hInstance, nullptr);
            if (p->outHwnd) {
                SetWindowPos(p->outHwnd, HWND_TOP, 0, 0,
                             static_cast<int>(p->width), static_cast<int>(p->height),
                             SWP_NOACTIVATE | SWP_SHOWWINDOW);
            }
        }
        if (outResult) *outResult = p && p->outHwnd ? 1 : 0;
        return true;
    }
    if (msg == kWmEdpeFgResizeChild) {
        HWND child = reinterpret_cast<HWND>(wp);
        const UINT w = LOWORD(lp);
        const UINT h = HIWORD(lp);
        if (child && IsWindow(child) && w > 0 && h > 0) {
            SetWindowPos(child, HWND_TOP, 0, 0, static_cast<int>(w), static_cast<int>(h),
                         SWP_NOACTIVATE | SWP_NOMOVE | SWP_SHOWWINDOW);
        }
        if (outResult) *outResult = 1;
        return true;
    }
    if (msg == kWmEdpeFgDestroyChild) {
        HWND child = reinterpret_cast<HWND>(lp);
        if (child && IsWindow(child)) {
            DestroyWindow(child);
        }
        if (outResult) *outResult = 1;
        return true;
    }
    return false;
}

bool FgRuntime::ensureOverlayWindow(HWND parentHwnd, uint32_t width, uint32_t height) {
    if (!parentHwnd || !IsWindow(parentHwnd) || width == 0 || height == 0) return false;
    if (overlayHwnd_ && IsWindow(overlayHwnd_) && gameHwnd_ == parentHwnd) {
        syncOverlayWindowSize(width, height);
        return true;
    }
    destroyOverlayWindow();

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static constexpr const wchar_t* kOverlayClass = L"EDPE_FgChildOverlayClass";
    if (!overlayClassAtom_) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = &edpeOverlayChildWndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kOverlayClass;
        overlayClassAtom_ = RegisterClassExW(&wc);
        if (!overlayClassAtom_ && GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            overlayClassAtom_ = 1;
        }
    }

    CreateChildParams params{};
    params.parent = parentHwnd;
    params.width = width;
    params.height = height;
    params.hInstance = hInst;
    params.className = kOverlayClass;

    const DWORD ownerTid = GetWindowThreadProcessId(parentHwnd, nullptr);
    if (ownerTid == GetCurrentThreadId()) {
        handleWindowMessage(parentHwnd, kWmEdpeFgCreateChild, 0,
                            reinterpret_cast<LPARAM>(&params), nullptr);
    } else {
        DWORD_PTR res = 0;
        SendMessageTimeoutW(parentHwnd, kWmEdpeFgCreateChild, 0,
                            reinterpret_cast<LPARAM>(&params),
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &res);
        // Fallback if parent's WndProc hasn't been hooked or is a test HWND without message pump.
        if (!params.outHwnd) {
            params.outHwnd = CreateWindowExW(
                WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY | WS_EX_NOACTIVATE,
                kOverlayClass,
                L"EDPE_FgOverlay",
                WS_CHILD | WS_VISIBLE | WS_DISABLED,
                0, 0, static_cast<int>(width), static_cast<int>(height),
                parentHwnd, nullptr, hInst, nullptr);
        }
    }

    overlayHwnd_ = params.outHwnd;
    gameHwnd_ = parentHwnd;
    return overlayHwnd_ != nullptr;
}

void FgRuntime::syncOverlayWindowSize(uint32_t width, uint32_t height) {
    if (!overlayHwnd_ || !IsWindow(overlayHwnd_) || !gameHwnd_) return;
    RECT rc{};
    if (GetClientRect(overlayHwnd_, &rc)) {
        const uint32_t curW = static_cast<uint32_t>((std::max)(0L, rc.right - rc.left));
        const uint32_t curH = static_cast<uint32_t>((std::max)(0L, rc.bottom - rc.top));
        if (curW == width && curH == height) return;
    }

    const DWORD ownerTid = GetWindowThreadProcessId(overlayHwnd_, nullptr);
    if (ownerTid == GetCurrentThreadId()) {
        SetWindowPos(overlayHwnd_, HWND_TOP, 0, 0,
                     static_cast<int>(width), static_cast<int>(height),
                     SWP_NOACTIVATE | SWP_NOMOVE | SWP_SHOWWINDOW);
    } else {
        DWORD_PTR res = 0;
        SendMessageTimeoutW(gameHwnd_, kWmEdpeFgResizeChild,
                            reinterpret_cast<WPARAM>(overlayHwnd_),
                            MAKELPARAM(width, height),
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &res);
    }
}

void FgRuntime::destroyOverlayWindow() {
    if (!overlayHwnd_) return;
    HWND child = overlayHwnd_;
    HWND parent = gameHwnd_;
    overlayHwnd_ = nullptr;
    if (IsWindow(child)) {
        const DWORD ownerTid = GetWindowThreadProcessId(child, nullptr);
        if (ownerTid == GetCurrentThreadId()) {
            DestroyWindow(child);
        } else if (parent && IsWindow(parent)) {
            DWORD_PTR res = 0;
            SendMessageTimeoutW(parent, kWmEdpeFgDestroyChild, 0,
                                reinterpret_cast<LPARAM>(child),
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &res);
        }
    }
}

bool FgRuntime::ensureDepthExtractCs(ID3D11Device* dev11) {
    if (depthExtractCs_) return true;
    if (!dev11) return false;
    Microsoft::WRL::ComPtr<ID3DBlob> csBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errBlob;
    if (FAILED(D3DCompile(
            kDepthExtractCsHlsl, sizeof(kDepthExtractCsHlsl) - 1,
            "FgDepthExtract.hlsl", nullptr, nullptr, "main", "cs_5_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &csBlob, &errBlob))) {
        return false;
    }
    return SUCCEEDED(dev11->CreateComputeShader(
        csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &depthExtractCs_));
}

bool FgRuntime::ensureBridgeResources(ID3D11Device* dev11,
                                      ID3D11DeviceContext* ctx11,
                                      uint32_t renderW,
                                      uint32_t renderH,
                                      uint32_t outW,
                                      uint32_t outH,
                                      DXGI_FORMAT bbFormat) {
    if (!dev11 || !ctx11 || !presenter_.isInitialized() || !renderW || !renderH || !outW || !outH) {
        return false;
    }
    auto& bridge = presenter_.bridge();
    const DXGI_FORMAT cleanOutFmt = canonicalNonSrgbFormat(bbFormat);

    // A size change (for example a new upscaler render size) frees shared textures that the previous frame's
    // frame generation work on the D3D12 queue may still read. Wait for that queue once before the first one is
    // freed; freeing them early made the GPU page fault and hung the frame generation device.
    bool waitedForGpu = false;
    auto waitBeforeFree = [&](bool exists) {
        if (exists && !waitedForGpu) {
            bridge.waitGpuIdle();
            waitedForGpu = true;
        }
    };

    if (!sharedHudless_.isValid() || sharedHudless_.width != outW ||
        sharedHudless_.height != outH || sharedHudless_.format != cleanOutFmt) {
        waitBeforeFree(sharedHudless_.isValid());
        sharedHudless_.reset();
        if (!bridge.createSharedTexture(
                outW, outH, cleanOutFmt,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                sharedHudless_)) {
            return false;
        }
        SetD3D11DebugName(sharedHudless_.d3d11.Get(), "EDPE_FgSharedHudless_D3D11");
        SetD3D12DebugName(sharedHudless_.d3d12.Get(), "EDPE_FgSharedHudless_D3D12");
    }

    if (!sharedUi_.isValid() || sharedUi_.width != outW || sharedUi_.height != outH) {
        waitBeforeFree(sharedUi_.isValid());
        sharedUiUav_.Reset();
        sharedUi_.reset();
        if (!bridge.createSharedTexture(
                outW, outH, DXGI_FORMAT_R8G8B8A8_UNORM,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                sharedUi_)) {
            return false;
        }
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(dev11->CreateUnorderedAccessView(sharedUi_.d3d11.Get(), &uavDesc, &sharedUiUav_))) {
            return false;
        }
        SetD3D11DebugName(sharedUi_.d3d11.Get(), "EDPE_FgSharedUi_D3D11");
        SetD3D12DebugName(sharedUi_.d3d12.Get(), "EDPE_FgSharedUi_D3D12");
    }

    if (!sharedDepth_.isValid() || sharedDepth_.width != renderW || sharedDepth_.height != renderH) {
        waitBeforeFree(sharedDepth_.isValid());
        sharedDepthUav_.Reset();
        sharedDepth_.reset();
        if (!bridge.createSharedTexture(
                renderW, renderH, DXGI_FORMAT_R32_FLOAT,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                sharedDepth_)) {
            return false;
        }
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        if (FAILED(dev11->CreateUnorderedAccessView(sharedDepth_.d3d11.Get(), &uavDesc, &sharedDepthUav_))) {
            return false;
        }
        SetD3D11DebugName(sharedDepth_.d3d11.Get(), "EDPE_FgSharedDepth_D3D11");
        SetD3D12DebugName(sharedDepth_.d3d12.Get(), "EDPE_FgSharedDepth_D3D12");
    }

    if (!sharedMvec_.isValid() || sharedMvec_.width != renderW || sharedMvec_.height != renderH) {
        waitBeforeFree(sharedMvec_.isValid());
        sharedMvec_.reset();
        if (!bridge.createSharedTexture(
                renderW, renderH, DXGI_FORMAT_R16G16_FLOAT,
                D3D11_BIND_SHADER_RESOURCE,
                sharedMvec_)) {
            return false;
        }
        SetD3D11DebugName(sharedMvec_.d3d11.Get(), "EDPE_FgSharedMvec_D3D11");
        SetD3D12DebugName(sharedMvec_.d3d12.Get(), "EDPE_FgSharedMvec_D3D12");
    }

    if (!uiPackPassInitialized_) {
        uiPackPassInitialized_ = uiPackPass_.initialize(dev11, ctx11);
    }
    return ensureDepthExtractCs(dev11) && uiPackPassInitialized_;
}

bool FgRuntime::extractDepthToShared(ID3D11DeviceContext* ctx,
                                     ID3D11ShaderResourceView* depthSrv,
                                     uint32_t renderW,
                                     uint32_t renderH) {
    if (!ctx || !depthSrv || !depthExtractCs_ || !sharedDepthUav_) return false;
    ctx->CSSetShader(depthExtractCs_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[1] = {depthSrv};
    ID3D11UnorderedAccessView* uavs[1] = {sharedDepthUav_.Get()};
    ctx->CSSetShaderResources(0, 1, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    ctx->Dispatch((renderW + 7u) / 8u, (renderH + 7u) / 8u, 1u);

    ID3D11ShaderResourceView* nullSrvs[1] = {nullptr};
    ID3D11UnorderedAccessView* nullUavs[1] = {nullptr};
    ctx->CSSetShaderResources(0, 1, nullSrvs);
    ctx->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);
    return true;
}

void FgRuntime::releaseBridgeResources() {
    sharedUiUav_.Reset();
    sharedDepthUav_.Reset();
    sharedHudless_.reset();
    sharedUi_.reset();
    sharedDepth_.reset();
    sharedMvec_.reset();
    depthExtractCs_.Reset();
    uiPackPass_.shutdown();
    uiPackPassInitialized_ = false;
}

void FgRuntime::shutdownLocked() {
    presenter_.setPrePresentCallback(nullptr, nullptr);
    if (presenter_.isInitialized()) {
        presenter_.bridge().waitGpuIdle();
    }
    manager_.shutdown();
    releaseBridgeResources();
    presenter_.shutdown();
    destroyOverlayWindow();
    trackedSwapChain_ = nullptr;
    gameHwnd_ = nullptr;
    monotonicFrameIndex_ = 0;

    telemetry_.active = false;
    telemetry_.activeBackend = FrameGenerationBackend::Off;
    telemetry_.generatedFramesThisCycle = 0;
    telemetry_.renderedFps = 0.0f;
    telemetry_.presentedFps = 0.0f;
    telemetry_.fgGpuTimeMs = 0.0f;
}

void FgRuntime::shutdown() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    shutdownLocked();
}

void FgRuntime::restartForUpscalerChange() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!presenter_.isInitialized()) return;
    EdpeLog(L"EDPE FG: frame generation off for the upscaler change; it starts again on the next frame");
    shutdownLocked();
}

// When the frame generation D3D12 device is removed (GPU hang or reset), turn frame generation off and keep it off
// until it is selected again. Without this, every frame reopened shared textures on the dead device and the game
// eventually stopped responding.
bool FgRuntime::handleDeviceLostLocked() {
    if (!presenter_.isInitialized()) return false;
    ID3D12Device* device = presenter_.bridge().d3d12Device();
    const HRESULT reason = device ? device->GetDeviceRemovedReason() : S_OK;
    if (SUCCEEDED(reason)) return false;
    wchar_t message[256];
    swprintf_s(message,
        L"EDPE FG: frame generation D3D12 device lost (reason 0x%08X); frame generation is off until it is selected again",
        static_cast<unsigned>(reason));
    EdpeLog(message);
    failedBackend_ = configuredBackend_;
    telemetry_.initFailed = true;
    std::wcsncpy(telemetry_.statusMessage,
                 L"Off: the frame generation GPU device was lost. Select frame generation again to retry.", 255);
    shutdownLocked();
    return true;
}

void FgRuntime::onResize(IDXGISwapChain* gameSwapChain) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!trackedSwapChain_ || trackedSwapChain_ == gameSwapChain) {
        // Turn off & release FG + proxy swapchain before DXGI ResizeBuffers per SL/FSR docs.
        shutdownLocked();
    }
}

void FgRuntime::onSwapChainRelease(IUnknown* object) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (trackedSwapChain_ && static_cast<IUnknown*>(trackedSwapChain_) == object) {
        shutdownLocked();
    }
}

void FgRuntime::ensureCapabilitiesProbed(ID3D11Device* d3d11Device) {
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (capabilitiesProbed_ || !d3d11Device) return;
        // Mark probed immediately under lock so concurrent/re-entrant calls cannot double-probe.
        capabilitiesProbed_ = true;
    }

    if (D3D12ProxyPresenter::isCrossApiPresentBlockedByRenderDoc()) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        amdDllAvailable_ = false;
        dlssgCapAvailable_ = false;
        std::wcsncpy(amdUnavailableReason_, L"Blocked while Windows renderdoc.dll is active.", 255);
        std::wcsncpy(dlssgUnavailableReason_, L"Blocked while Windows renderdoc.dll is active.", 255);
        return;
    }

    bool localAmdAvail = false;
    wchar_t localAmdReason[256]{};
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (auto* b = manager_.getBackend(FrameGenerationBackend::AmdFsrFg)) {
            localAmdAvail = b->isAvailable();
            if (!localAmdAvail) {
                std::wcsncpy(localAmdReason, b->unavailableReason(), 255);
            }
        }
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDev))) &&
        SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
        adapter->GetDesc(&desc);
    }

    bool localDlssgAvail = false;
    wchar_t localDlssgReason[256]{};
    if (desc.VendorId != 0x10DE) {
        std::wcsncpy(localDlssgReason,
                     L"Requires an NVIDIA GeForce RTX 40+ / 50+ GPU.", 255);
    } else {
#if defined(EDPE_ENABLE_STREAMLINE) && EDPE_ENABLE_STREAMLINE
        // Perform LUID-only Streamline capability probe WITHOUT holding mutex_ and without
        // creating a temporary D3D12 device or calling slSetD3DDevice (Streamline DLSS-G §3.0).
        StreamlineStatusSnapshot snap{};
        localDlssgAvail = NvidiaStreamlineFgBackend::probeCapabilitiesForLuid(
            desc.AdapterLuid, &snap);
        if (!localDlssgAvail && snap.unavailableReason[0] != L'\0') {
            std::wcsncpy(localDlssgReason, snap.unavailableReason, 255);
        }
#else
        std::wcsncpy(localDlssgReason,
                     L"Built without NVIDIA Streamline SDK headers.", 255);
#endif
    }

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    amdDllAvailable_ = localAmdAvail;
    if (!amdDllAvailable_ && localAmdReason[0]) {
        std::wcsncpy(amdUnavailableReason_, localAmdReason, 255);
    }
    dlssgCapAvailable_ = localDlssgAvail;
    if (!dlssgCapAvailable_ && localDlssgReason[0]) {
        std::wcsncpy(dlssgUnavailableReason_, localDlssgReason, 255);
    }
}

void FgRuntime::onPresent(IDXGISwapChain* gameSwapChain,
                          UINT syncInterval,
                          UINT flags,
                          bool* outPresentedViaProxy) {
    if (outPresentedViaProxy) *outPresentedViaProxy = false;
    if (!gameSwapChain || (flags & DXGI_PRESENT_TEST)) return;

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    telemetry_.configuredBackend = configuredBackend_;
    telemetry_.renderDocBlocked = D3D12ProxyPresenter::isCrossApiPresentBlockedByRenderDoc();
    telemetry_.exclusiveFullscreenBlocked = false;

    if (configuredBackend_ == FrameGenerationBackend::Off || telemetry_.renderDocBlocked) {
        if (presenter_.isInitialized()) {
            shutdownLocked();
        }
        if (telemetry_.renderDocBlocked && configuredBackend_ != FrameGenerationBackend::Off) {
            std::wcsncpy(telemetry_.statusMessage,
                         L"Disabled (Windows renderdoc.dll in-process)", 255);
        } else {
            std::wcsncpy(telemetry_.statusMessage, L"Off", 255);
        }
        return;
    }

    if (configuredBackend_ == failedBackend_) {
        return;
    }

    BOOL isFullscreen = FALSE;
    if (SUCCEEDED(gameSwapChain->GetFullscreenState(&isFullscreen, nullptr)) && isFullscreen) {
        if (presenter_.isInitialized()) {
            shutdownLocked();
        }
        telemetry_.exclusiveFullscreenBlocked = true;
        std::wcsncpy(telemetry_.statusMessage,
                     L"Paused: requires Borderless / Windowed (Exclusive Fullscreen active)",
                     255);
        return;
    }

    DXGI_SWAP_CHAIN_DESC scDesc{};
    if (FAILED(gameSwapChain->GetDesc(&scDesc)) || !scDesc.OutputWindow) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> dev11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> ctx4;
    if (FAILED(gameSwapChain->GetDevice(IID_PPV_ARGS(&dev11))) || !dev11) {
        return;
    }
    dev11->GetImmediateContext(&ctx11);
    if (!ctx11 || FAILED(ctx11.As(&ctx4))) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> gameBackbuffer;
    if (FAILED(gameSwapChain->GetBuffer(0, IID_PPV_ARGS(&gameBackbuffer))) || !gameBackbuffer) {
        return;
    }
    D3D11_TEXTURE2D_DESC bbDesc{};
    gameBackbuffer->GetDesc(&bbDesc);
    const DXGI_FORMAT cleanFormat = canonicalNonSrgbFormat(bbDesc.Format);

    if (presenter_.isInitialized() &&
        (trackedSwapChain_ != gameSwapChain ||
         gameHwnd_ != scDesc.OutputWindow ||
         presenter_.width() != bbDesc.Width ||
         presenter_.height() != bbDesc.Height ||
         presenter_.format() != cleanFormat)) {
        shutdownLocked();
    }

    if (!presenter_.isInitialized()) {
        if (!ensureOverlayWindow(scDesc.OutputWindow, bbDesc.Width, bbDesc.Height)) {
            failedBackend_ = configuredBackend_;
            telemetry_.initFailed = true;
            std::wcsncpy(telemetry_.statusMessage, L"Failed to create child overlay HWND", 255);
            return;
        }
        if (!presenter_.initialize(dev11.Get(), overlayHwnd_, bbDesc.Width, bbDesc.Height,
                                   cleanFormat, 3, 0)) {
            destroyOverlayWindow();
            failedBackend_ = configuredBackend_;
            telemetry_.initFailed = true;
            std::wcsncpy(telemetry_.statusMessage, L"D3D12ProxyPresenter init failed", 255);
            return;
        }
        trackedSwapChain_ = gameSwapChain;
    }

    if (handleDeviceLostLocked()) {
        return;
    }

    if (manager_.selectedBackend() != configuredBackend_) {
        if (auto* b = manager_.getBackend(FrameGenerationBackend::NvidiaDlssG)) {
            auto* sl = static_cast<NvidiaStreamlineFgBackend*>(const_cast<IFrameGenerationBackend*>(b));
            sl->setLowLatencyBoost(reflexBoost_);
            sl->setDynamicMfgMode(dynamicMfg_, dynamicTargetFps_);
            sl->setDebugView(fgDebugView_);
        }
        if (auto* b = manager_.getBackend(FrameGenerationBackend::AmdFsrFg)) {
            auto* amd = static_cast<AmdFsrFgBackend*>(const_cast<IFrameGenerationBackend*>(b));
            amd->setDebugFlags(false, false, fgDebugView_);
        }
        if (!manager_.selectBackend(configuredBackend_, &presenter_)) {
            const wchar_t* reason = backendUnavailableReasonLocked(configuredBackend_);
            if (configuredBackend_ == FrameGenerationBackend::AmdFsrFg) {
                amdDllAvailable_ = false;
                if (reason && reason[0]) std::wcsncpy(amdUnavailableReason_, reason, 255);
            } else if (configuredBackend_ == FrameGenerationBackend::NvidiaDlssG) {
                dlssgCapAvailable_ = false;
                if (reason && reason[0]) std::wcsncpy(dlssgUnavailableReason_, reason, 255);
            }
            failedBackend_ = configuredBackend_;
            telemetry_.initFailed = true;
            std::wcsncpy(telemetry_.statusMessage,
                         (reason && reason[0]) ? reason : L"FG backend init failed", 255);
            shutdownLocked();
            return;
        }
    }

    syncOverlayWindowSize(bbDesc.Width, bbDesc.Height);

    FgSceneSnapshot snap{};
    const bool hasSnap = ContextCensusGetFgSnapshot(&snap) && snap.valid;

    const uint32_t renderW = (hasSnap && snap.renderWidth > 0) ? snap.renderWidth : bbDesc.Width;
    const uint32_t renderH = (hasSnap && snap.renderHeight > 0) ? snap.renderHeight : bbDesc.Height;

    if (!ensureBridgeResources(dev11.Get(), ctx11.Get(), renderW, renderH, bbDesc.Width, bbDesc.Height, cleanFormat)) {
        return;
    }

    ++monotonicFrameIndex_;
    EvalHookCtx evalCtx{};
    evalCtx.manager = &manager_;

    const bool canGenerateThisFrame =
        hasSnap && !snap.isPauseOrMenu && snap.depthSrv && snap.motionVectorsTex;

    FrameGenerationInputs& in = evalCtx.inputs;
    in.renderWidth = renderW;
    in.renderHeight = renderH;
    in.outputWidth = bbDesc.Width;
    in.outputHeight = bbDesc.Height;
    in.frameTimeMs = (hasSnap && snap.frameTimeMs > 0.0f) ? snap.frameTimeMs : 16.6667f;
    in.frameIndex = monotonicFrameIndex_;
    in.reset = !hasSnap || snap.reset || (monotonicFrameIndex_ == 1);
    in.isHdr = hasSnap ? snap.colorIsHdr : false;
    in.isPauseOrMenu = !canGenerateThisFrame;

    if (canGenerateThisFrame) {
        // Ensure D3D12 (PrepareV2 + FG Present) from the previous frame has finished reading
        // shared bridge textures before D3D11 overwrites them for the current frame.
        presenter_.waitForLastPresentOnD3D11(ctx4.Get());

        D3D11_TEXTURE2D_DESC mvDesc{};
        snap.motionVectorsTex->GetDesc(&mvDesc);
        if (mvDesc.Width == renderW && mvDesc.Height == renderH &&
            mvDesc.Format == DXGI_FORMAT_R16G16_FLOAT) {
            ctx11->CopyResource(sharedMvec_.d3d11.Get(), snap.motionVectorsTex);
            in.motionVectors = sharedMvec_.d3d12.Get();
        }
        if (extractDepthToShared(ctx11.Get(), snap.depthSrv, renderW, renderH)) {
            in.depth = sharedDepth_.d3d12.Get();
        }
        if (snap.hudlessColorTex) {
            D3D11_TEXTURE2D_DESC hlDesc{};
            snap.hudlessColorTex->GetDesc(&hlDesc);
            if (hlDesc.Width == bbDesc.Width && hlDesc.Height == bbDesc.Height &&
                areCopyCompatible(hlDesc.Format, cleanFormat)) {
                ctx11->CopyResource(sharedHudless_.d3d11.Get(), snap.hudlessColorTex);
                in.hudlessColor = sharedHudless_.d3d12.Get();
            }
        }
        if (snap.uiHdrSrv || snap.uiLdrSrv || snap.uiMultSrv) {
            const float clearZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            ctx11->ClearUnorderedAccessViewFloat(sharedUiUav_.Get(), clearZero);
            if (uiPackPass_.pack(snap.uiHdrSrv,
                                  snap.uiLdrSrv,
                                  snap.uiMultSrv,
                                  snap.uiMultSrv != nullptr,
                                  bbDesc.Width,
                                  bbDesc.Height,
                                  sharedUi_.d3d11.Get(),
                                  sharedUiUav_.Get())) {
                in.packedUiColorAndAlpha = sharedUi_.d3d12.Get();
            }
        }
        in.finalBackbuffer = presenter_.currentBackBuffer();
        in.jitterX = snap.jitterX;
        in.jitterY = snap.jitterY;
        in.motionScaleX = snap.motionScaleX;
        in.motionScaleY = snap.motionScaleY;
        in.cameraNow = snap.cameraNow;
        in.cameraPrevious = snap.cameraPrevious;
        in.isPauseOrMenu = !(in.depth != nullptr && in.motionVectors != nullptr);
        evalCtx.enabled = true;
    } else {
        in.cameraNow.worldFromView[0] = 1.0f;
        in.cameraNow.worldFromView[5] = 1.0f;
        in.cameraNow.worldFromView[10] = 1.0f;
        in.cameraNow.scaleX = 1.0f;
        in.cameraNow.scaleY = static_cast<float>(bbDesc.Width) / static_cast<float>((std::max)(1u, bbDesc.Height));
        in.cameraNow.depthB = 0.025f;
        in.cameraPrevious = in.cameraNow;
        in.isPauseOrMenu = true;
        evalCtx.enabled = true;
    }

    const uint32_t requestedExtra =
        (dynamicMfg_ && manager_.activeBackend() && manager_.activeBackend()->isDynamicMfgSupported())
            ? 0xFFFFFFFFu
            : (requestedMultiplier_ > 1 ? requestedMultiplier_ - 1u : 1u);
    manager_.setRequestedExtraFrames(requestedExtra);

    presenter_.setPrePresentCallback(&prePresentFgCallback, &evalCtx);
    HRESULT presentHr = E_FAIL;
    double copyMs = 0.0;
    const bool presentedOk = presenter_.presentFromD3D11Texture(
        ctx4.Get(), gameBackbuffer.Get(), syncInterval, flags, &presentHr, &copyMs);
    presenter_.setPrePresentCallback(nullptr, nullptr);

    lastCopyMs_ = copyMs;

    const auto& mgrDiag = manager_.diagnostics();
    telemetry_.active = presentedOk;
    telemetry_.activeBackend = manager_.selectedBackend();
    telemetry_.pausedForMenu = in.isPauseOrMenu;
    telemetry_.generatedFramesThisCycle = mgrDiag.generatedFramesThisCycle;
    telemetry_.totalGeneratedFrames = mgrDiag.totalGeneratedFrames;
    telemetry_.renderedFps = mgrDiag.renderedFps;
    telemetry_.presentedFps = mgrDiag.presentedFps;
    telemetry_.fgGpuTimeMs = mgrDiag.fgGpuTimeMs;
    telemetry_.d3d12CopyMs = lastCopyMs_;
    if (const auto* activeB = manager_.activeBackend()) {
        telemetry_.maxFramesToGenerate = activeB->maxFramesToGenerate();
        telemetry_.dynamicMfgSupported = activeB->isDynamicMfgSupported();
    } else {
        telemetry_.maxFramesToGenerate = 0;
        telemetry_.dynamicMfgSupported = false;
    }
    std::wcsncpy(telemetry_.statusMessage, mgrDiag.statusMessage, 255);

    if (presentedOk && SUCCEEDED(presentHr)) {
        if (outPresentedViaProxy) *outPresentedViaProxy = true;
    } else {
        // Fail open back to native D3D11 swapchain if proxy presentation ever fails.
        std::wstring msg = L"[EDPE][FG] Proxy present failed; falling back to native Present.";
        EdpeLog(msg.c_str());
        shutdownLocked();
        if (outPresentedViaProxy) *outPresentedViaProxy = false;
    }
}

FgRuntimeTelemetry FgRuntime::telemetry() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return telemetry_;
}

bool FgRuntime::isBackendAvailable(FrameGenerationBackend backend) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (D3D12ProxyPresenter::isCrossApiPresentBlockedByRenderDoc()) return false;
    if (backend == FrameGenerationBackend::Off) return true;
    if (backend == FrameGenerationBackend::AmdFsrFg) return amdDllAvailable_;
    if (backend == FrameGenerationBackend::NvidiaDlssG) return dlssgCapAvailable_;
    return false;
}

const wchar_t* FgRuntime::backendUnavailableReason(FrameGenerationBackend backend) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return backendUnavailableReasonLocked(backend);
}

const wchar_t* FgRuntime::backendUnavailableReasonLocked(FrameGenerationBackend backend) const {
    if (D3D12ProxyPresenter::isCrossApiPresentBlockedByRenderDoc()) {
        return L"Blocked while Windows renderdoc.dll is in-process.";
    }
    if (backend == FrameGenerationBackend::AmdFsrFg) {
        if (const auto* b = manager_.getBackend(FrameGenerationBackend::AmdFsrFg)) {
            const wchar_t* r = b->unavailableReason();
            if (r && r[0]) return r;
        }
        if (amdUnavailableReason_[0]) return amdUnavailableReason_;
    } else if (backend == FrameGenerationBackend::NvidiaDlssG) {
        if (const auto* b = manager_.getBackend(FrameGenerationBackend::NvidiaDlssG)) {
            const wchar_t* r = b->unavailableReason();
            if (r && r[0]) return r;
        }
        if (dlssgUnavailableReason_[0]) return dlssgUnavailableReason_;
    }
    return L"";
}

} // namespace edpe
