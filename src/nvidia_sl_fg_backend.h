#pragma once

#include "fg_backend.h"

#include <cstdint>
#include <memory>

namespace edpe {

// Capability & runtime diagnostics snapshot from NVIDIA Streamline (Phase 15).
struct StreamlineStatusSnapshot {
    bool interposerFound = false;
    bool signatureVerified = false;
    bool slInitialized = false;
    bool reflexSupported = false;
    bool pclSupported = false;
    bool dlssgSupported = false;
    bool vsyncSupported = false;
    bool dynamicMfgSupported = false;
    uint32_t maxFramesToGenerate = 0;
    uint32_t lastPresentedFrames = 0;
    uint32_t dlssgStatusFlags = 0; // sl::DLSSGStatus bitmask
    int32_t lastDlssgSupportCode = -1;
    int32_t lastReflexSupportCode = -1;
    int32_t lastSlResult = 0;
    uint64_t estimatedVramBytes = 0;
    char slVersion[32]{};
    char dlssgVersion[32]{};
    char reflexVersion[32]{};
    wchar_t unavailableReason[256]{};
};

// NVIDIA DLSS Frame Generation backend via Streamline SDK v2.14+ (Phase 15).
// Uses ManualHooking (eUseManualHooking | eUseFrameBasedResourceTagging) and dynamic
// LoadLibrary("sl.interposer.dll") after embedded NVIDIA signature verification so
// EDPE's dxgi.dll proxy hooks are never bypassed or conflicted.
// Automatically couples mandatory sl.reflex + sl.pcl frame tokens with sl.dlss_g.
class NvidiaStreamlineFgBackend : public IFrameGenerationBackend {
public:
    NvidiaStreamlineFgBackend();
    ~NvidiaStreamlineFgBackend() override;

    FrameGenerationBackend backendType() const override { return FrameGenerationBackend::NvidiaDlssG; }
    const wchar_t* name() const override { return L"NVIDIA DLSS Frame Generation"; }

    bool isAvailable() const override;
    const wchar_t* unavailableReason() const override;

    // Capability-driven limits queried from slDLSSGGetState (never hardcoded).
    uint32_t maxFramesToGenerate() const override;
    bool isDynamicMfgSupported() const override;

    bool initialize(D3D12ProxyPresenter* presenter) override;
    void shutdown() override;

    bool evaluate(ID3D12GraphicsCommandList* cmdList,
                  const FrameGenerationInputs& inputs,
                  bool enabled,
                  uint32_t requestedExtraFrames,
                  uint32_t* outGeneratedFrames) override;

    void resetHistory() override;

    // Reflex Boost (eLowLatency vs eLowLatencyWithBoost) and Dynamic MFG controls.
    void setLowLatencyBoost(bool boost);
    bool isLowLatencyBoost() const;

    void setDynamicMfgMode(bool enableDynamic, float targetFps = 0.0f);
    bool isDynamicMfgEnabled() const;

    // Invokes slReflexSleep for the current frame token when Reflex is active.
    bool reflexSleepCurrentFrame();

    StreamlineStatusSnapshot statusSnapshot() const;

    // Transiently probes Streamline initialization, security signature, Reflex/PCL,
    // and DLSS-G capability on the given adapter without replacing any swapchain.
    static bool probeCapabilities(ID3D12Device* d3d12Device,
                                  StreamlineStatusSnapshot* outSnapshot);

    static const char* dlssgStatusToString(uint32_t statusMask);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace edpe
