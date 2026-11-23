#pragma once

#include "temporal_inputs.h"
#include "taa_pass.h"
#include "sharpen_pass.h"


#include <atomic>
#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <string>
#include <wrl/client.h>

namespace edpe {

// Diagnostics collected for each evaluation cycle (AGENTS.md diagnostics requirement).
struct UpscalerDiagnostics {
    bool didRun = false;
    UpscalerBackend backend = UpscalerBackend::None;
    UpscaleMode mode = UpscaleMode::Native;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    float frameTimeMs = 0.0f;
    float gpuTimeMs = 0.0f;
    bool reset = false;
    bool success = false;
    wchar_t statusMessage[256] = L"idle";
};

// Abstract backend interface for vendor temporal reconstruction (AGENTS.md & PLAN.md section 4).
class IUpscalerBackend {
public:
    virtual ~IUpscalerBackend() = default;

    virtual UpscalerBackend backendType() const = 0;
    virtual const wchar_t* name() const = 0;

    virtual bool isAvailable() const = 0;
    virtual const wchar_t* unavailableReason() const = 0;

    virtual bool isModeSupported(UpscaleMode mode) const = 0;

    virtual bool initialize(ID3D11Device* device, ID3D11DeviceContext* context) = 0;
    virtual void shutdown() = 0;

    virtual bool evaluate(ID3D11DeviceContext* context,
        const TemporalFrameInputs& inputs,
        ID3D11RenderTargetView* destinationRtv,
        ID3D11Texture2D* destinationTexture) = 0;

    virtual void resetHistory() = 0;

    virtual unsigned recommendedJitterPhaseCount(unsigned configuredBasePhases,
        uint32_t renderWidth, uint32_t renderHeight,
        uint32_t outputWidth, uint32_t outputHeight) const;
};

// Null / Bypass backend: performs original game output without temporal reconstruction.
class NullUpscalerBackend : public IUpscalerBackend {
public:
    UpscalerBackend backendType() const override { return UpscalerBackend::None; }
    const wchar_t* name() const override { return L"None (Bypass)"; }

    bool isAvailable() const override { return true; }
    const wchar_t* unavailableReason() const override { return L""; }

    bool isModeSupported(UpscaleMode mode) const override {
        return mode == UpscaleMode::Native;
    }

    bool initialize(ID3D11Device* /*device*/, ID3D11DeviceContext* /*context*/) override {
        return true;
    }
    void shutdown() override {}

    bool evaluate(ID3D11DeviceContext* /*context*/,
        const TemporalFrameInputs& /*inputs*/,
        ID3D11RenderTargetView* /*destinationRtv*/,
        ID3D11Texture2D* /*destinationTexture*/) override {
        return true; // Bypass leaves original output intact.
    }

    void resetHistory() override {}
};

// NVIDIA DLSS / DLAA backend implementation wrapper.
// Governed by safety constraints in nvidia-dlaa-input-contract.md and ngx_context.h.
class NvidiaUpscalerBackend : public IUpscalerBackend {
public:
    UpscalerBackend backendType() const override { return UpscalerBackend::NvidiaDlss; }
    const wchar_t* name() const override { return L"NVIDIA DLSS / DLAA"; }

    bool isAvailable() const override;
    const wchar_t* unavailableReason() const override;

    bool isModeSupported(UpscaleMode mode) const override;

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;
    void shutdown() override;

    bool evaluate(ID3D11DeviceContext* context,
        const TemporalFrameInputs& inputs,
        ID3D11RenderTargetView* destinationRtv,
        ID3D11Texture2D* destinationTexture) override;

    void resetHistory() override;

private:
    bool initialized_ = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> outputTexture_;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outputUav_;
    ID3D11Device* outputDevice_ = nullptr;
    uint32_t outputWidth_ = 0;
    uint32_t outputHeight_ = 0;
    DXGI_FORMAT outputFormat_ = DXGI_FORMAT_UNKNOWN;

    bool ensureOutputTexture(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format);
};

// AMD FidelityFX FSR backend implementation wrapper.
// Governed by safety constraints in amd-fsr-input-contract.md.
class AmdUpscalerBackend : public IUpscalerBackend {
public:
    AmdUpscalerBackend();
    ~AmdUpscalerBackend() override;

    UpscalerBackend backendType() const override { return UpscalerBackend::AmdFsr; }
    const wchar_t* name() const override { return L"AMD FSR"; }

    bool isAvailable() const override;
    const wchar_t* unavailableReason() const override;

    bool isModeSupported(UpscaleMode mode) const override;

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;
    void shutdown() override;

    bool evaluate(ID3D11DeviceContext* context,
        const TemporalFrameInputs& inputs,
        ID3D11RenderTargetView* destinationRtv,
        ID3D11Texture2D* destinationTexture) override;

    void resetHistory() override;

    unsigned recommendedJitterPhaseCount(unsigned configuredBasePhases,
        uint32_t renderWidth, uint32_t renderHeight,
        uint32_t outputWidth, uint32_t outputHeight) const override;

    static constexpr float kDefaultVelocityFactor = 0.00f;
    void setVelocityFactor(float factor);
    float getVelocityFactor() const;

    // Runtime FSR 3.1.2 / FSR 4 selection (menu). Both run under the same AMD FSR backend; FSR 4 falls back to
    // FSR 3.1.2 on any failure. Selecting FSR 4 again after a failure retries initialization.
    void setFsr4Selected(bool selected);
    // Explicit user retry after FSR 4 failed to initialize or was disabled by failures.
    void retryFsr4();
    bool isFsr4Selected() const;
    // True when FSR 4 is selected and its engine is initialized and not disabled by failures.
    bool isFsr4Running() const;
    // The startup probe succeeded: this GPU and AMD's runtime offer a 4.x provider (selectable in the menu).
    bool isFsr4Capable() const;
    // The provider that actually runs now, e.g. "FSR 4.1.2", "FSR 3.1.2", or "FSR 3.1.2 (FSR 4 unavailable)".
    std::string describeProvider() const;
    const wchar_t* fsr4LastError() const;

private:
    bool tryInitFsr4();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Native Direct3D 11 Temporal Anti-Aliasing (TAA) backend.
// Evaluates 1:1 subpixel Halton (2,3) jitter accumulation, YCoCg variance clipping,
// and camera-depth motion reprojection without external vendor SDK dependencies.
class TaaUpscalerBackend : public IUpscalerBackend {
public:
    UpscalerBackend backendType() const override { return UpscalerBackend::Taa; }
    const wchar_t* name() const override { return L"TAA"; }

    bool isAvailable() const override;
    const wchar_t* unavailableReason() const override;

    bool isModeSupported(UpscaleMode mode) const override {
        return mode == UpscaleMode::Native || mode == UpscaleMode::NativeAA;
    }

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context) override;
    void shutdown() override;

    bool evaluate(ID3D11DeviceContext* context,
        const TemporalFrameInputs& inputs,
        ID3D11RenderTargetView* destinationRtv,
        ID3D11Texture2D* destinationTexture) override;

    void resetHistory() override;

private:
    TaaPass pass_;
};

// Central coordinator for temporal upscaling in EDPE.
// Manages backend selection, mode configuration, fail-open fallback, and diagnostics.
class UpscalerManager {
public:
    static UpscalerManager& instance();

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void shutdown();

    bool setBackend(UpscalerBackend backend);
    UpscalerBackend getBackend() const { return currentBackendType_; }

    bool setMode(UpscaleMode mode);
    UpscaleMode getMode() const { return currentMode_; }

    bool isBackendAvailable(UpscalerBackend backend) const;
    const wchar_t* getBackendUnavailableReason(UpscalerBackend backend) const;

    // FSR 4.1.2 selection inside the AMD FSR backend (menu entry "AMD FSR 4.1.2"). Idempotent.
    void setFsr4Selected(bool selected);
    void retryFsr4() { amdBackend_.retryFsr4(); pendingReset_ = true; }
    bool isFsr4Selected() const { return amdBackend_.isFsr4Selected(); }
    bool isFsr4Running() const { return amdBackend_.isFsr4Running(); }
    // FSR 4 can be offered: the FSR backend is usable, the signed DLLs are present beside the proxy and the
    // startup probe found a 4.x provider.
    bool isFsr4Available() const;
    const wchar_t* getFsr4UnavailableReason() const;
    std::string describeAmdProvider() const { return amdBackend_.describeProvider(); }

    bool isReconstructionActive() const {
        return currentBackendType_ != UpscalerBackend::None && currentMode_ != UpscaleMode::Native;
    }

    bool hasEvaluated() const { return evalCount_ > 0; }

    // Dispatches temporal reconstruction. Fails open on any error.
    bool evaluate(ID3D11DeviceContext* context,
        const TemporalFrameInputs& inputs,
        ID3D11RenderTargetView* destinationRtv,
        ID3D11Texture2D* destinationTexture);

    bool evaluateBackend(UpscalerBackend backendType,
        ID3D11DeviceContext* context,
        const TemporalFrameInputs& inputs,
        ID3D11RenderTargetView* destinationRtv,
        ID3D11Texture2D* destinationTexture);

    bool applySharpenToRtv(ID3D11DeviceContext* context,
        ID3D11ShaderResourceView* sourceSrv,
        ID3D11RenderTargetView* targetRtv,
        uint32_t width, uint32_t height,
        float sharpness, bool hdr,
        bool callerRestoresState = false);

    bool isBackendModeSupported(UpscalerBackend backendType) const;
    void resetBackendHistory(UpscalerBackend backendType);
    unsigned getRecommendedJitterPhaseCount(unsigned configuredBasePhases,
        uint32_t renderWidth, uint32_t renderHeight,
        uint32_t outputWidth, uint32_t outputHeight) const;

    void setFsrVelocityFactor(float factor);
    float getFsrVelocityFactor() const;

    void requestReset() { pendingReset_ = true; }

    void setTelemetryVisibility(bool needTiming, bool needStatusText) {
        telemetryTimingEnabled_.store(needTiming, std::memory_order_relaxed);
        telemetryStatusEnabled_.store(needStatusText, std::memory_order_relaxed);
    }

    const UpscalerDiagnostics& getDiagnostics() const { return diagnostics_; }

    // Last measured GPU time (ms) of the given backend. The active backend reports its live
    // value; other backends report the last time measured by evaluateBackend (comparison capture).
    float getBackendGpuTimeMs(UpscalerBackend backendType) const {
        if (backendType == currentBackendType_) return diagnostics_.gpuTimeMs;
        const auto idx = static_cast<size_t>(backendType);
        return idx < kBackendCount ? backendTiming_[idx].lastMs : 0.0f;
    }

private:
    UpscalerManager();
    ~UpscalerManager() = default;

    IUpscalerBackend* getBackendInstance(UpscalerBackend backend);
    const IUpscalerBackend* getBackendInstance(UpscalerBackend backend) const;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    UpscalerBackend currentBackendType_ = UpscalerBackend::None;
    UpscaleMode currentMode_ = UpscaleMode::Native;
    bool pendingReset_ = false;

    NullUpscalerBackend nullBackend_;
    NvidiaUpscalerBackend nvidiaBackend_;
    AmdUpscalerBackend amdBackend_;
    TaaUpscalerBackend taaBackend_;
    SharpenPass sharpenPass_;

    struct GpuTimingSlot {
        Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
        Microsoft::WRL::ComPtr<ID3D11Query> start;
        Microsoft::WRL::ComPtr<ID3D11Query> end;
        bool inFlight = false;
    };
    static constexpr size_t kTimingRingSize = 4;
    GpuTimingSlot timingRing_[kTimingRingSize]{};
    size_t timingWriteIndex_ = 0;
    float lastGpuTimeMs_ = 0.0f;
    unsigned evalCount_{0};
    std::atomic<bool> telemetryTimingEnabled_{false};
    std::atomic<bool> telemetryStatusEnabled_{false};

    // Per-backend async GPU timing for evaluateBackend (indexed by UpscalerBackend value).
    static constexpr size_t kBackendCount = 4;
    struct BackendTiming {
        GpuTimingSlot slot;
        float lastMs = 0.0f;
    };
    BackendTiming backendTiming_[kBackendCount]{};

    UpscalerDiagnostics diagnostics_{};
};

} // namespace edpe
