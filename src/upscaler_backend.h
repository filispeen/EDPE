#pragma once

#include "temporal_inputs.h"

#include <cstdint>
#include <d3d11.h>
#include <memory>
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
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

    bool isReconstructionActive() const {
        return currentBackendType_ != UpscalerBackend::None && currentMode_ != UpscaleMode::Native;
    }

    // Dispatches temporal reconstruction. Fails open on any error.
    bool evaluate(ID3D11DeviceContext* context,
        const TemporalFrameInputs& inputs,
        ID3D11RenderTargetView* destinationRtv,
        ID3D11Texture2D* destinationTexture);

    void requestReset() { pendingReset_ = true; }

    const UpscalerDiagnostics& getDiagnostics() const { return diagnostics_; }

private:
    UpscalerManager();
    ~UpscalerManager() = default;

    IUpscalerBackend* getBackendInstance(UpscalerBackend backend);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    UpscalerBackend currentBackendType_ = UpscalerBackend::None;
    UpscaleMode currentMode_ = UpscaleMode::Native;
    bool pendingReset_ = false;

    NullUpscalerBackend nullBackend_;
    NvidiaUpscalerBackend nvidiaBackend_;
    AmdUpscalerBackend amdBackend_;

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

    UpscalerDiagnostics diagnostics_{};
};

} // namespace edpe
