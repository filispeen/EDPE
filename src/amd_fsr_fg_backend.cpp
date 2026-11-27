#include "amd_fsr_fg_backend.h"

#include "guarded_call.h"
#include "log.h"

#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>

#if defined(EDPE_ENABLE_FFX_FG) && EDPE_ENABLE_FFX_FG
#include <ffx_api.h>
#include <ffx_api_loader.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_framegeneration_dx12.h>
#endif

namespace edpe {

namespace {

HMODULE loadSiblingDll(const wchar_t* dllName) {
    if (HMODULE already = GetModuleHandleW(dllName)) return already;

    wchar_t selfPath[MAX_PATH]{};
    HMODULE selfMod = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&loadSiblingDll),
            &selfMod) &&
        selfMod &&
        GetModuleFileNameW(selfMod, selfPath, MAX_PATH) > 0) {
        std::filesystem::path candidate = std::filesystem::path(selfPath).parent_path() / dllName;
        if (HMODULE loaded = LoadLibraryExW(candidate.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
            return loaded;
        }
    }
    return LoadLibraryW(dllName);
}

void logFfxError(const wchar_t* stage, uint32_t rc) {
    wchar_t buf[256]{};
    std::swprintf(buf, 256, L"EDPE AmdFsrFg: %ls failed (rc=%u)", stage, rc);
    EdpeLog(buf);
}

#if defined(EDPE_ENABLE_FFX_FG) && EDPE_ENABLE_FFX_FG
// A provider call that throws is turned into an error code the caller handles (see guarded_call.h).
template <typename Call>
ffxReturnCode_t callFfxGuarded(const wchar_t* stage, std::atomic<bool>* threw, Call&& call) {
    return callGuarded<ffxReturnCode_t>(FFX_API_RETURN_ERROR, threw, std::forward<Call>(call), [&] {
        wchar_t buf[256]{};
        std::swprintf(buf, 256, L"EDPE AmdFsrFg: %ls threw an exception", stage);
        EdpeLog(buf);
    });
}
#endif

} // namespace

struct AmdFsrFgBackend::Impl {
    std::mutex mutex;
    D3D12ProxyPresenter* presenter = nullptr;
    bool initialized = false;
    bool forceResetNext = false;
    bool debugTearLines = false;
    bool debugPacingLines = false;
    bool debugView = false;

    uint32_t activeWidth = 0;
    uint32_t activeHeight = 0;
    DXGI_FORMAT activeBackbufferFmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT activeHudlessFmt = DXGI_FORMAT_UNKNOWN;
    bool activeIsHdr = false;
    bool activeLowResMv = false;
    std::string providerVersion = "unavailable";
    wchar_t unavailableMsg[256] = L"";

#if defined(EDPE_ENABLE_FFX_FG) && EDPE_ENABLE_FFX_FG
    HMODULE loaderDll = nullptr;
    HMODULE providerDll = nullptr;
    ffxFunctions ffx{};
    ffxContext swapChainCtx = nullptr;
    ffxContext fgCtx = nullptr;
    bool swapChainWrapped = false;
    // Set when a provider call threw: the contexts are in an unknown state, so they are never called again
    // (and never destroyed, which would throw again). Frame generation stays off until the backend is
    // initialized again. Atomic because fgDispatchCallback can run on the swap chain's present thread.
    std::atomic<bool> faulted{false};

    bool ensureLoaderLoaded() {
        if (ffx.CreateContext && ffx.DestroyContext && ffx.Configure && ffx.Query && ffx.Dispatch) {
            return true;
        }
        // Load provider DLL explicitly alongside loader so relative/altered search finds it reliably.
        if (!providerDll) {
            providerDll = loadSiblingDll(L"amd_fidelityfx_framegeneration_dx12.dll");
        }
        if (!loaderDll) {
            loaderDll = loadSiblingDll(L"amd_fidelityfx_loader_dx12.dll");
        }
        if (!loaderDll) {
            std::swprintf(unavailableMsg, 256,
                          L"Missing amd_fidelityfx_loader_dx12.dll beside dxgi.dll / executable");
            return false;
        }
        ffxLoadFunctions(&ffx, loaderDll);
        if (!ffx.CreateContext || !ffx.DestroyContext || !ffx.Configure ||
            !ffx.Query || !ffx.Dispatch) {
            std::swprintf(unavailableMsg, 256,
                          L"amd_fidelityfx_loader_dx12.dll missing ffx_api entry points");
            return false;
        }
        if (!providerDll) {
            std::swprintf(unavailableMsg, 256,
                          L"Missing amd_fidelityfx_framegeneration_dx12.dll beside loader");
            return false;
        }
        unavailableMsg[0] = L'\0';
        return true;
    }

    static ffxReturnCode_t fgDispatchCallback(ffxDispatchDescFrameGeneration* params, void* pUserCtx) {
        auto* self = static_cast<Impl*>(pUserCtx);
        if (!self || !params || !self->ffx.Dispatch || !self->fgCtx || self->faulted) {
            return FFX_API_RETURN_ERROR_PARAMETER;
        }
        return callFfxGuarded(L"ffxDispatch(FrameGeneration callback)", &self->faulted,
                              [&] { return self->ffx.Dispatch(&self->fgCtx, &params->header); });
    }

    void destroyFgContextOnly() {
        if (!fgCtx) return;
        if (faulted) {
            fgCtx = nullptr;
            activeWidth = 0;
            activeHeight = 0;
            activeHudlessFmt = DXGI_FORMAT_UNKNOWN;
            return;
        }
        // Disable FG first so FrameInterpolationSwapChain flushes any in-flight GPU work
        // before destroying resources (avoids D3D12 OBJECT_DELETED_WHILE_STILL_IN_USE).
        ffxConfigureDescFrameGeneration cfg{};
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain = presenter ? presenter->swapChain() : nullptr;
        cfg.frameGenerationEnabled = false;
        cfg.presentCallback = nullptr;
        cfg.frameGenerationCallback = nullptr;
        cfg.HUDLessColor = FfxApiResource{};
        callFfxGuarded(L"ffxConfigure(FrameGeneration disable)", &faulted,
                       [&] { return ffx.Configure(&fgCtx, &cfg.header); });

        if (swapChainCtx && !faulted) {
            ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiCfg{};
            uiCfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
            uiCfg.uiResource = FfxApiResource{};
            uiCfg.flags = 0;
            callFfxGuarded(L"ffxConfigure(RegisterUiResourceDX12 clear)", &faulted,
                           [&] { return ffx.Configure(&swapChainCtx, &uiCfg.header); });
        }

        if (!faulted) {
            callFfxGuarded(L"ffxDestroyContext(FrameGeneration)", &faulted,
                           [&] { return ffx.DestroyContext(&fgCtx, nullptr); });
        }
        fgCtx = nullptr;
        activeWidth = 0;
        activeHeight = 0;
        activeHudlessFmt = DXGI_FORMAT_UNKNOWN;
    }

    bool ensureFgContext(const FrameGenerationInputs& inputs) {
        const DXGI_FORMAT bbFmt = presenter->format();
        const DXGI_FORMAT hudlessFmt = inputs.hudlessColor
                                           ? inputs.hudlessColor->GetDesc().Format
                                           : bbFmt;
        const bool lowResMv = (inputs.renderWidth < inputs.outputWidth) ||
                              (inputs.renderHeight < inputs.outputHeight);

        if (fgCtx && activeWidth == inputs.outputWidth && activeHeight == inputs.outputHeight &&
            activeBackbufferFmt == bbFmt && activeHudlessFmt == hudlessFmt &&
            activeIsHdr == inputs.isHdr && activeLowResMv == lowResMv) {
            return true;
        }

        destroyFgContextOnly();

        ffxCreateBackendDX12Desc backendDesc{};
        backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backendDesc.device = presenter->bridge().d3d12Device();

        ffxCreateContextDescFrameGenerationVersion verDesc{};
        verDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
        verDesc.header.pNext = &backendDesc.header;
        verDesc.version = FFX_FRAMEGENERATION_VERSION;

        ffxCreateContextDescFrameGenerationHudless hudlessDesc{};
        hudlessDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
        hudlessDesc.header.pNext = &verDesc.header;
        hudlessDesc.hudlessBackBufferFormat = ffxApiGetSurfaceFormatDX12(hudlessFmt);

        ffxCreateContextDescFrameGeneration fgDesc{};
        fgDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        fgDesc.header.pNext = (hudlessFmt != bbFmt) ? &hudlessDesc.header : &verDesc.header;
        fgDesc.displaySize = {inputs.outputWidth, inputs.outputHeight};
        fgDesc.maxRenderSize = {inputs.outputWidth, inputs.outputHeight};
        fgDesc.backBufferFormat = ffxApiGetSurfaceFormatDX12(bbFmt);
        fgDesc.flags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED |
                       FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
        if (!lowResMv) {
            fgDesc.flags |= FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
        }
        if (inputs.isHdr) {
            fgDesc.flags |= FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;
        }

        if (faulted) return false;
        const ffxReturnCode_t rc = callFfxGuarded(L"ffxCreateContext(FrameGeneration)", &faulted,
                                                  [&] { return ffx.CreateContext(&fgCtx, &fgDesc.header, nullptr); });
        if (rc != FFX_API_RETURN_OK || !fgCtx) {
            logFfxError(L"ffxCreateContext(FrameGeneration)", rc);
            fgCtx = nullptr;
            return false;
        }

        ffxQueryGetProviderVersion verQuery{};
        verQuery.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        const ffxReturnCode_t verRc = callFfxGuarded(L"ffxQuery(ProviderVersion)", &faulted,
                                                     [&] { return ffx.Query(&fgCtx, &verQuery.header); });
        if (verRc == FFX_API_RETURN_OK && verQuery.versionName) {
            providerVersion = verQuery.versionName;
        }
        if (faulted) return false;

        activeWidth = inputs.outputWidth;
        activeHeight = inputs.outputHeight;
        activeBackbufferFmt = bbFmt;
        activeHudlessFmt = hudlessFmt;
        activeIsHdr = inputs.isHdr;
        activeLowResMv = lowResMv;
        return true;
    }
#endif
};

AmdFsrFgBackend::AmdFsrFgBackend() : impl_(std::make_unique<Impl>()) {}

AmdFsrFgBackend::~AmdFsrFgBackend() {
    shutdown();
}

bool AmdFsrFgBackend::isAvailable() const {
#if defined(EDPE_ENABLE_FFX_FG) && EDPE_ENABLE_FFX_FG
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->ensureLoaderLoaded();
#else
    return false;
#endif
}

const wchar_t* AmdFsrFgBackend::unavailableReason() const {
#if defined(EDPE_ENABLE_FFX_FG) && EDPE_ENABLE_FFX_FG
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->ensureLoaderLoaded()) {
        return impl_->unavailableMsg;
    }
    return L"";
#else
    return L"Built without FidelityFX SDK v2.4+ (EDPE_ENABLE_FFX_FG=0)";
#endif
}

bool AmdFsrFgBackend::initialize(D3D12ProxyPresenter* presenter) {
#if defined(EDPE_ENABLE_FFX_FG) && EDPE_ENABLE_FFX_FG
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->initialized && impl_->presenter == presenter) return true;
    // A fault belongs to the contexts that threw. Clear it only once none is left, so a throw on a live context
    // (for example from the present thread) is never discarded by this reset.
    if (!impl_->fgCtx && !impl_->swapChainCtx) impl_->faulted = false;
    if (!presenter || !presenter->isInitialized() || !presenter->swapChain() ||
        !presenter->bridge().commandQueue()) {
        return false;
    }
    if (!impl_->ensureLoaderLoaded()) {
        return false;
    }

    IDXGISwapChain4* rawSc = presenter->detachSwapChainForWrap();
    if (!rawSc) return false;

    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 scVer{};
    scVer.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    scVer.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;

    IDXGISwapChain4* wrappedSc = rawSc;
    ffxCreateContextDescFrameGenerationSwapChainWrapDX12 wrapDesc{};
    wrapDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_WRAP_DX12;
    wrapDesc.header.pNext = &scVer.header;
    wrapDesc.swapchain = &wrappedSc;
    wrapDesc.gameQueue = presenter->bridge().commandQueue();

    const ffxReturnCode_t rc = callFfxGuarded(L"ffxCreateContext(SwapChainWrapDX12)", &impl_->faulted,
                                              [&] { return impl_->ffx.CreateContext(&impl_->swapChainCtx, &wrapDesc.header, nullptr); });
    if (rc != FFX_API_RETURN_OK || !impl_->swapChainCtx || !wrappedSc) {
        logFfxError(L"ffxCreateContext(SwapChainWrapDX12)", rc);
        impl_->swapChainCtx = nullptr;
        // Re-attach raw swapchain if wrap failed before replacing, or recreate plain swapchain.
        if (!presenter->attachWrappedSwapChain(rawSc)) {
            presenter->recreatePlainSwapChain();
        }
        return false;
    }

    if (!presenter->attachWrappedSwapChain(wrappedSc)) {
        callFfxGuarded(L"ffxDestroyContext(SwapChain, attach failed)", &impl_->faulted,
                       [&] { return impl_->ffx.DestroyContext(&impl_->swapChainCtx, nullptr); });
        impl_->swapChainCtx = nullptr;
        presenter->recreatePlainSwapChain();
        return false;
    }

    impl_->presenter = presenter;
    impl_->swapChainWrapped = true;
    impl_->initialized = true;
    impl_->forceResetNext = true;
    return true;
#else
    (void)presenter;
    return false;
#endif
}

void AmdFsrFgBackend::shutdown() {
#if defined(EDPE_ENABLE_FFX_FG) && EDPE_ENABLE_FFX_FG
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) return;

    impl_->destroyFgContextOnly();

    if (impl_->swapChainCtx) {
        if (impl_->presenter) {
            impl_->presenter->bridge().waitGpuIdle();
        }
        if (!impl_->faulted) {
            callFfxGuarded(L"ffxDestroyContext(SwapChain)", &impl_->faulted,
                           [&] { return impl_->ffx.DestroyContext(&impl_->swapChainCtx, nullptr); });
        }
        impl_->swapChainCtx = nullptr;
    }

    if (impl_->swapChainWrapped && impl_->presenter && impl_->presenter->isInitialized()) {
        impl_->presenter->recreatePlainSwapChain();
    }

    impl_->swapChainWrapped = false;
    impl_->presenter = nullptr;
    impl_->initialized = false;
#endif
}

bool AmdFsrFgBackend::evaluate(ID3D12GraphicsCommandList* cmdList,
                               const FrameGenerationInputs& inputs,
                               bool enabled,
                               uint32_t /*requestedExtraFrames*/,
                               uint32_t* outGeneratedFrames) {
    if (outGeneratedFrames) *outGeneratedFrames = 0;
#if defined(EDPE_ENABLE_FFX_FG) && EDPE_ENABLE_FFX_FG
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized || !impl_->presenter || !cmdList || impl_->faulted) return false;
    if (!impl_->ensureFgContext(inputs)) return false;

    // HUDLessColor and RegisterUiResourceDX12 are mutually exclusive UI paths in FSR FG.
    // When HUDLessColor is supplied, the backbuffer already carries composited game UI + ImGui,
    // so let FSR FG extract UI via (backbuffer - HUDLessColor) and avoid double-compositing UI.
    const bool usePackedUi = (!inputs.hudlessColor && inputs.packedUiColorAndAlpha != nullptr);
    ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 uiCfg{};
    uiCfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
    if (usePackedUi) {
        uiCfg.uiResource = ffxApiGetResourceDX12(
            inputs.packedUiColorAndAlpha, FFX_API_RESOURCE_STATE_COMMON);
        uiCfg.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA |
                      FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
    } else {
        uiCfg.uiResource = FfxApiResource{};
        uiCfg.flags = 0;
    }
    const ffxReturnCode_t uiRc = callFfxGuarded(L"ffxConfigure(RegisterUiResourceDX12)", &impl_->faulted,
                                                [&] { return impl_->ffx.Configure(&impl_->swapChainCtx, &uiCfg.header); });
    if (uiRc != FFX_API_RETURN_OK) {
        logFfxError(L"ffxConfigure(RegisterUiResourceDX12)", uiRc);
        return false;
    }

    uint32_t dispatchFlags = 0;
    if (impl_->debugTearLines) dispatchFlags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES;
    if (impl_->debugPacingLines) dispatchFlags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES;
    if (impl_->debugView) dispatchFlags |= FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW;

    // 1. Strict FSR SDK 2.4 order: Configure -> Dispatch(PrepareV2) -> Present/Dispatch(FG).
    ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = impl_->presenter->swapChain();
    cfg.presentCallback = nullptr;
    cfg.presentCallbackUserContext = nullptr;
    cfg.frameGenerationCallback = enabled ? &Impl::fgDispatchCallback : nullptr;
    cfg.frameGenerationCallbackUserContext = enabled ? impl_.get() : nullptr;
    cfg.frameGenerationEnabled = enabled;
    cfg.allowAsyncWorkloads = false;
    cfg.HUDLessColor = inputs.hudlessColor
                           ? ffxApiGetResourceDX12(inputs.hudlessColor, FFX_API_RESOURCE_STATE_COMMON)
                           : FfxApiResource{};
    cfg.flags = dispatchFlags;
    cfg.onlyPresentGenerated = false;
    cfg.generationRect = {0, 0, static_cast<int32_t>(inputs.outputWidth), static_cast<int32_t>(inputs.outputHeight)};
    cfg.frameID = inputs.frameIndex;

    const ffxReturnCode_t cfgRc = callFfxGuarded(L"ffxConfigure(FrameGeneration)", &impl_->faulted,
                                                 [&] { return impl_->ffx.Configure(&impl_->fgCtx, &cfg.header); });
    if (cfgRc != FFX_API_RETURN_OK) {
        logFfxError(L"ffxConfigure(FrameGeneration)", cfgRc);
        return false;
    }

    if (!enabled) {
        return true;
    }

    FgCameraMatrices camMats{};
    if (!computeFgCameraMatrices(inputs.cameraNow, inputs.cameraPrevious, &camMats)) {
        return false;
    }

    // Transition shared inputs from COMMON -> NON_PIXEL_SHADER_RESOURCE for PrepareV2 compute,
    // then back to COMMON before cmdList ends.
    D3D11D3D12Bridge::transitionResource(
        cmdList, inputs.depth,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D11D3D12Bridge::transitionResource(
        cmdList, inputs.motionVectors,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ffxDispatchDescFrameGenerationPrepareV2 prep{};
    prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prep.frameID = inputs.frameIndex;
    prep.flags = dispatchFlags;
    prep.commandList = cmdList;
    prep.renderSize = {inputs.renderWidth, inputs.renderHeight};
    prep.jitterOffset = {inputs.jitterX, inputs.jitterY};
    prep.motionVectorScale = {inputs.motionScaleX, inputs.motionScaleY};
    prep.frameTimeDelta = inputs.frameTimeMs;
    prep.reset = inputs.reset || impl_->forceResetNext;
    // FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE:
    // FidelityFX swaps near/far for reversed-infinite depth (cameraNear=FLT_MAX, cameraFar=nearPlane).
    prep.cameraNear = FLT_MAX;
    prep.cameraFar = camMats.nearPlane;
    prep.cameraFovAngleVertical = camMats.verticalFovRad;
    prep.viewSpaceToMetersFactor = inputs.viewSpaceToMeters;
    prep.depth = ffxApiGetResourceDX12(inputs.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    prep.motionVectors = ffxApiGetResourceDX12(inputs.motionVectors, FFX_API_RESOURCE_STATE_COMPUTE_READ);

    prep.cameraPosition[0] = camMats.position.x;
    prep.cameraPosition[1] = camMats.position.y;
    prep.cameraPosition[2] = camMats.position.z;
    prep.cameraUp[0] = camMats.up.x;
    prep.cameraUp[1] = camMats.up.y;
    prep.cameraUp[2] = camMats.up.z;
    prep.cameraRight[0] = camMats.right.x;
    prep.cameraRight[1] = camMats.right.y;
    prep.cameraRight[2] = camMats.right.z;
    prep.cameraForward[0] = camMats.forward.x;
    prep.cameraForward[1] = camMats.forward.y;
    prep.cameraForward[2] = camMats.forward.z;

    const ffxReturnCode_t prepRc = callFfxGuarded(L"ffxDispatch(PrepareV2)", &impl_->faulted,
                                                  [&] { return impl_->ffx.Dispatch(&impl_->fgCtx, &prep.header); });

    D3D11D3D12Bridge::transitionResource(
        cmdList, inputs.depth,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    D3D11D3D12Bridge::transitionResource(
        cmdList, inputs.motionVectors,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);

    if (prepRc != FFX_API_RETURN_OK) {
        logFfxError(L"ffxDispatch(PrepareV2)", prepRc);
        return false;
    }

    impl_->forceResetNext = false;
    if (outGeneratedFrames) *outGeneratedFrames = 1;
    return true;
#else
    (void)cmdList;
    (void)inputs;
    (void)enabled;
    return false;
#endif
}

void AmdFsrFgBackend::resetHistory() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->forceResetNext = true;
}

void AmdFsrFgBackend::setDebugFlags(bool tearLines, bool pacingLines, bool debugView) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->debugTearLines = tearLines;
    impl_->debugPacingLines = pacingLines;
    impl_->debugView = debugView;
}

void AmdFsrFgBackend::setDebugView(bool enabled) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->debugView = enabled;
}

bool AmdFsrFgBackend::isDebugViewEnabled() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->debugView;
}

bool AmdFsrFgBackend::queryGpuMemoryUsage(uint64_t* outFgTotalBytes,
                                          uint64_t* outSwapChainTotalBytes) const {
    if (outFgTotalBytes) *outFgTotalBytes = 0;
    if (outSwapChainTotalBytes) *outSwapChainTotalBytes = 0;
#if defined(EDPE_ENABLE_FFX_FG) && EDPE_ENABLE_FFX_FG
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->initialized) return false;

    if (outFgTotalBytes && impl_->fgCtx && !impl_->faulted) {
        FfxApiEffectMemoryUsage usage{};
        ffxQueryDescFrameGenerationGetGPUMemoryUsage q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE;
        q.gpuMemoryUsageFrameGeneration = &usage;
        const ffxReturnCode_t queryRc = callFfxGuarded(L"ffxQuery(FrameGeneration memory usage)", &impl_->faulted,
                                                       [&] { return impl_->ffx.Query(&impl_->fgCtx, &q.header); });
        if (queryRc == FFX_API_RETURN_OK) {
            *outFgTotalBytes = usage.totalUsageInBytes;
        }
    }
    if (outSwapChainTotalBytes && impl_->swapChainCtx && !impl_->faulted) {
        FfxApiEffectMemoryUsage usage{};
        ffxQueryFrameGenerationSwapChainGetGPUMemoryUsageDX12 q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_GPU_MEMORY_USAGE_DX12;
        q.gpuMemoryUsageFrameGenerationSwapchain = &usage;
        const ffxReturnCode_t queryRc = callFfxGuarded(L"ffxQuery(SwapChain memory usage)", &impl_->faulted,
                                                       [&] { return impl_->ffx.Query(&impl_->swapChainCtx, &q.header); });
        if (queryRc == FFX_API_RETURN_OK) {
            *outSwapChainTotalBytes = usage.totalUsageInBytes;
        }
    }
    return true;
#else
    return false;
#endif
}

const char* AmdFsrFgBackend::providerVersionString() const {
    return impl_->providerVersion.c_str();
}

} // namespace edpe
