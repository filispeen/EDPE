#pragma once

#include "fg_backend.h"
#include "fg_ui_pack_pass.h"
#include "temporal_math.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

namespace edpe {

// Frame snapshot exported from ContextCensus to FgRuntime on Present().
struct FgSceneSnapshot {
    bool valid = false;
    ID3D11Texture2D* hudlessColorTex = nullptr;
    ID3D11Texture2D* depthTex = nullptr;
    ID3D11ShaderResourceView* depthSrv = nullptr;
    ID3D11Texture2D* motionVectorsTex = nullptr;
    ID3D11ShaderResourceView* uiHdrSrv = nullptr;
    ID3D11ShaderResourceView* uiLdrSrv = nullptr;
    ID3D11ShaderResourceView* uiMultSrv = nullptr;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float motionScaleX = 1.0f;
    float motionScaleY = 1.0f;
    float frameTimeMs = 16.667f;
    uint64_t frameIndex = 0;
    bool reset = false;
    bool colorIsHdr = false;
    bool isPauseOrMenu = false;
    CameraProjection cameraNow{};
    CameraProjection cameraPrevious{};
};

struct FgRuntimeTelemetry {
    FrameGenerationBackend configuredBackend = FrameGenerationBackend::Off;
    FrameGenerationBackend activeBackend = FrameGenerationBackend::Off;
    bool active = false;
    bool pausedForMenu = false;
    bool exclusiveFullscreenBlocked = false;
    bool renderDocBlocked = false;
    bool initFailed = false;
    uint32_t maxFramesToGenerate = 0;
    bool dynamicMfgSupported = false;
    uint32_t generatedFramesThisCycle = 0;
    uint64_t totalGeneratedFrames = 0;
    float renderedFps = 0.0f;
    float presentedFps = 0.0f;
    float fgGpuTimeMs = 0.0f;
    double d3d12CopyMs = 0.0;
    wchar_t statusMessage[256] = L"Off";
};

// Live runtime orchestrator bridging EDPE's D3D11 scene + HUD layers into
// D3D12ProxyPresenter + FrameGenerationManager (Phase 14 FSR FG / Phase 15 DLSS-G).
// Mounts a click-through WS_CHILD Flip-model D3D12 overlay over Elite's client area
// so toggling FG ON/OFF at runtime requires no game restart and fails open on any error.
class FgRuntime {
public:
    static FgRuntime& instance();

    FgRuntime(const FgRuntime&) = delete;
    FgRuntime& operator=(const FgRuntime&) = delete;

    // Applies UI/INI configuration. Changing target backend clears prior init-failure latch.
    void applyConfig(int backendInt,
                     int multiplier,
                     bool dynamicMfg,
                     float dynamicTargetFps,
                     bool reflexBoost);

    // True when ContextCensus must generate depth + camera motion vectors even if SR is Bypass.
    bool isMotionNeeded() const;

    // Called from observedPresent after UiOnPresent(). If *outPresentedViaProxy is true,
    // observedPresent must skip original(gameSwapChain->Present) for this frame.
    void onPresent(IDXGISwapChain* gameSwapChain,
                   UINT syncInterval,
                   UINT flags,
                   bool* outPresentedViaProxy);

    // Called from observedResizeBuffers before native ResizeBuffers executes.
    void onResize(IDXGISwapChain* gameSwapChain);

    // Called from observedRelease when a DXGI swapchain is destroyed.
    void onSwapChainRelease(IUnknown* object);

    void shutdown();

    // Runs lightweight once-per-process capability detection (e.g. DLSS-G adapter check on NVIDIA)
    // without disrupting the active swapchain.
    void ensureCapabilitiesProbed(ID3D11Device* d3d11Device);

    // Launches capability detection on a background thread so Present/UI never stalls.
    void startCapabilitiesProbeAsync(ID3D11Device* d3d11Device);
    bool isCapabilitiesProbeInProgress() const;

    FgRuntimeTelemetry telemetry() const;
    bool isBackendAvailable(FrameGenerationBackend backend) const;
    void setFgDebugView(bool enabled);
    bool fgDebugView() const;
    const wchar_t* backendUnavailableReason(FrameGenerationBackend backend) const;

    void setTelemetryTimingEnabled(bool enabled);
    void setFrameLimitFps(float maxFps);
    bool isReflexFrameLimiterActive() const;
    void onMainSceneRenderStart();

    // Custom window message dispatcher invoked from edpeWndProc on gameHwnd's UI thread
    // so the WS_CHILD overlay HWND is created/resized/destroyed on the owning thread.
    static bool handleWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* outResult);

private:
    FgRuntime();
    ~FgRuntime();

    bool ensureOverlayWindow(HWND parentHwnd, uint32_t width, uint32_t height);
    void destroyOverlayWindow();
    void syncOverlayWindowSize(uint32_t width, uint32_t height);

    bool ensureBridgeResources(ID3D11Device* dev11,
                               ID3D11DeviceContext* ctx11,
                               uint32_t renderW,
                               uint32_t renderH,
                               uint32_t outW,
                               uint32_t outH,
                               DXGI_FORMAT bbFormat);
    bool ensureDepthExtractCs(ID3D11Device* dev11);
    bool extractDepthToShared(ID3D11DeviceContext* ctx,
                              ID3D11ShaderResourceView* depthSrv,
                              uint32_t renderW,
                              uint32_t renderH,
                              uint32_t slot);
    void releaseBridgeResources();
    void shutdownLocked();
    void joinProbeThread();
    const wchar_t* backendUnavailableReasonLocked(FrameGenerationBackend backend) const;

    std::thread probeThread_;
    std::atomic<bool> probeStarted_{false};
    std::atomic<bool> probeInProgress_{false};

    mutable std::recursive_mutex mutex_;
    FrameGenerationBackend configuredBackend_ = FrameGenerationBackend::Off;
    FrameGenerationBackend failedBackend_ = FrameGenerationBackend::Off;
    uint32_t requestedMultiplier_ = 2;
    bool dynamicMfg_ = false;
    float dynamicTargetFps_ = 0.0f;
    bool reflexBoost_ = false;
    bool fgDebugView_ = false;

    IDXGISwapChain* trackedSwapChain_ = nullptr;
    HWND gameHwnd_ = nullptr;
    HWND overlayHwnd_ = nullptr;
    ATOM overlayClassAtom_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> cachedDev11_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> cachedCtx11_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> cachedCtx4_;
    HWND cachedScHwnd_ = nullptr;

    D3D12ProxyPresenter presenter_;
    FrameGenerationManager manager_;
    FgUiPackPass uiPackPass_;

    static constexpr uint32_t kSharedSlotCount = D3D11D3D12Bridge::kAllocatorCount;
    std::array<SharedBridgeTexture, kSharedSlotCount> sharedHudless_{};
    std::array<SharedBridgeTexture, kSharedSlotCount> sharedUi_{};
    std::array<SharedBridgeTexture, kSharedSlotCount> sharedDepth_{};
    std::array<SharedBridgeTexture, kSharedSlotCount> sharedMvec_{};
    std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>, kSharedSlotCount> sharedUiUav_{};
    std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>, kSharedSlotCount> sharedDepthUav_{};
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> depthExtractCs_;

    std::atomic<bool> nvidiaFgActive_{false};
    std::atomic<bool> reflexLimiterActive_{false};
    std::atomic<float> frameLimitFps_{0.0f};

    uint64_t monotonicFrameIndex_ = 0;
    double lastCopyMs_ = 0.0;
    bool capabilitiesProbed_ = false;
    bool uiPackPassInitialized_ = false;
    bool amdDllAvailable_ = false;
    bool dlssgCapAvailable_ = false;
    wchar_t amdUnavailableReason_[256]{};
    wchar_t dlssgUnavailableReason_[256]{};

    FgRuntimeTelemetry telemetry_{};
};

} // namespace edpe
