#pragma once

#include "d3d12_presenter.h"
#include "temporal_math.h"

#include <cstdint>
#include <d3d12.h>
#include <memory>

namespace edpe {

// Independent Frame Generation backend identity (AGENTS.md & PLAN.md §§5, 17).
// Kept strictly decoupled from UpscalerBackend so cross-vendor SR + FG remains possible.
enum class FrameGenerationBackend : uint32_t {
    Off = 0,
    AmdFsrFg = 1,     // Phase 14 (AMD FidelityFX FSR 3.1 / 4 FG via ffx_api)
    NvidiaDlssG = 2   // Phase 15 (NVIDIA Streamline sl.dlss_g)
};

inline const wchar_t* getFrameGenerationBackendName(FrameGenerationBackend backend) {
    switch (backend) {
    case FrameGenerationBackend::Off: return L"Off";
    case FrameGenerationBackend::AmdFsrFg: return L"AMD FSR Frame Generation";
    case FrameGenerationBackend::NvidiaDlssG: return L"NVIDIA DLSS Frame Generation";
    default: return L"Unknown";
    }
}

// Vendor-neutral Frame Generation inputs (PLAN.md §17 & roadmap-research-overview.md §4).
struct FrameGenerationInputs {
    // Bridged D3D12 resources (from D3D11D3D12Bridge NT handles).
    ID3D12Resource* hudlessColor = nullptr;          // Reconstructed 3D world BEFORE UiCompositePass
    ID3D12Resource* finalBackbuffer = nullptr;       // Post-UI composited frame (fallback/reference)
    ID3D12Resource* packedUiColorAndAlpha = nullptr; // Premultiplied R8G8B8A8_UNORM packed from UiLayer
    ID3D12Resource* uiHdrLayer = nullptr;            // Optional 3-layer path: hdrLayerTex_
    ID3D12Resource* uiLdrLayer = nullptr;            // Optional 3-layer path: ldrLayerTex_
    ID3D12Resource* uiMultLayer = nullptr;           // Optional 3-layer path: multLayerTex_
    ID3D12Resource* depth = nullptr;                 // Shared R32_FLOAT copy of reversed-Z DSV #2
    ID3D12Resource* motionVectors = nullptr;         // Shared R16G16_FLOAT current->prev render pixels

    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;

    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float motionScaleX = 1.0f;
    float motionScaleY = 1.0f;
    float frameTimeMs = 0.0f;
    uint64_t frameIndex = 0; // Must increment by +1 per rendered frame; skips trigger reset
    bool reset = false;
    bool isHdr = false;
    bool isPauseOrMenu = false;

    CameraProjection cameraNow{};
    CameraProjection cameraPrevious{};
    float viewSpaceToMeters = 1.0f;

    bool isValid(const wchar_t** outReason = nullptr) const;
};

// Row-major row-vector (p_clip = p_view * viewToClip) camera matrices and scalars
// derived from EDPE's CameraProjection for FidelityFX FG and NVIDIA Streamline.
struct FgCameraMatrices {
    float viewToClip[16]{};
    float clipToView[16]{};
    float clipToPrevClip[16]{};
    float prevClipToClip[16]{};
    Vec3 position{};
    Vec3 right{};
    Vec3 up{};
    Vec3 forward{};
    float verticalFovRad = 0.0f;
    float aspectRatio = 1.0f;
    float nearPlane = 0.025f;
};

// Computes FG camera matrices & basis vectors from Elite's CameraProjection.
// Note: Elite uses +Z forward, reversed-Z infinite depth (z_ndc = depthB / viewZ).
bool computeFgCameraMatrices(const CameraProjection& now,
                             const CameraProjection& previous,
                             FgCameraMatrices* outMatrices);

// Diagnostics distinguishing simulation/rendered FPS from FG presented FPS (AGENTS.md).
struct FrameGenerationDiagnostics {
    bool didRun = false;
    FrameGenerationBackend backend = FrameGenerationBackend::Off;
    bool enabled = false;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    uint64_t frameIndex = 0;
    uint32_t generatedFramesThisCycle = 0;
    uint64_t totalGeneratedFrames = 0;
    float renderedFps = 0.0f;
    float presentedFps = 0.0f;
    float fgGpuTimeMs = 0.0f;
    bool reset = false;
    bool success = false;
    wchar_t statusMessage[256] = L"Off";
};

class IFrameGenerationBackend {
public:
    virtual ~IFrameGenerationBackend() = default;

    virtual FrameGenerationBackend backendType() const = 0;
    virtual const wchar_t* name() const = 0;

    virtual bool isAvailable() const = 0;
    virtual const wchar_t* unavailableReason() const = 0;

    // Capability-driven limits (never hardcoded by GPU generation).
    virtual uint32_t maxFramesToGenerate() const = 0;
    virtual bool isDynamicMfgSupported() const = 0;

    virtual bool initialize(D3D12ProxyPresenter* presenter) = 0;
    virtual void shutdown() = 0;

    virtual bool evaluate(ID3D12GraphicsCommandList* cmdList,
                          const FrameGenerationInputs& inputs,
                          bool enabled,
                          uint32_t requestedExtraFrames,
                          uint32_t* outGeneratedFrames) = 0;

    virtual void resetHistory() = 0;
};

class NullFrameGenBackend : public IFrameGenerationBackend {
public:
    FrameGenerationBackend backendType() const override { return FrameGenerationBackend::Off; }
    const wchar_t* name() const override { return L"Off"; }
    bool isAvailable() const override { return true; }
    const wchar_t* unavailableReason() const override { return L""; }
    uint32_t maxFramesToGenerate() const override { return 0; }
    bool isDynamicMfgSupported() const override { return false; }
    bool initialize(D3D12ProxyPresenter* /*presenter*/) override { return true; }
    void shutdown() override {}
    bool evaluate(ID3D12GraphicsCommandList* /*cmdList*/,
                  const FrameGenerationInputs& /*inputs*/,
                  bool /*enabled*/,
                  uint32_t /*requestedExtraFrames*/,
                  uint32_t* outGeneratedFrames) override {
        if (outGeneratedFrames) *outGeneratedFrames = 0;
        return true;
    }
    void resetHistory() override {}
};

// Coordinates independent FG backend selection, monotonicity checks, RenderDoc fail-open
// protection, and separate Rendered vs Presented FPS telemetry.
class FrameGenerationManager {
public:
    FrameGenerationManager();

    void registerBackend(std::unique_ptr<IFrameGenerationBackend> backend);
    bool selectBackend(FrameGenerationBackend backend, D3D12ProxyPresenter* presenter);
    void shutdown();

    FrameGenerationBackend selectedBackend() const { return selected_; }
    void setRequestedExtraFrames(uint32_t extraFrames) { requestedExtraFrames_ = extraFrames; }
    uint32_t requestedExtraFrames() const { return requestedExtraFrames_; }

    // Evaluates FG for the current frame; fails open (generatedFrames = 0) on invalid inputs,
    // pause menus, Windows RenderDoc, or backend error.
    bool evaluate(ID3D12GraphicsCommandList* cmdList,
                  const FrameGenerationInputs& inputs,
                  bool userEnabled);

    const IFrameGenerationBackend* activeBackend() const;
    const IFrameGenerationBackend* getBackend(FrameGenerationBackend backend) const;
    const FrameGenerationDiagnostics& diagnostics() const { return diagnostics_; }

private:
    std::unique_ptr<IFrameGenerationBackend> backends_[3];
    FrameGenerationBackend selected_ = FrameGenerationBackend::Off;
    uint32_t requestedExtraFrames_ = 1;
    uint64_t lastFrameIndex_ = 0;
    bool hasLastFrameIndex_ = false;
    uint64_t totalGeneratedFrames_ = 0;
    FrameGenerationDiagnostics diagnostics_{};
};

} // namespace edpe
