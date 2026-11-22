#pragma once

#include "fg_backend.h"

#include <memory>

namespace edpe {

// AMD FidelityFX Frame Generation (FSR 3.1 / 4.0) backend using official ffx_api v2.4+.
// Dynamically loads amd_fidelityfx_loader_dx12.dll (fail-open when DLLs are absent).
class AmdFsrFgBackend : public IFrameGenerationBackend {
public:
    AmdFsrFgBackend();
    ~AmdFsrFgBackend() override;

    FrameGenerationBackend backendType() const override { return FrameGenerationBackend::AmdFsrFg; }
    const wchar_t* name() const override { return L"AMD FSR Frame Generation"; }

    bool isAvailable() const override;
    const wchar_t* unavailableReason() const override;

    uint32_t maxFramesToGenerate() const override { return 1; }
    bool isDynamicMfgSupported() const override { return false; }

    bool initialize(D3D12ProxyPresenter* presenter) override;
    void shutdown() override;

    bool evaluate(ID3D12GraphicsCommandList* cmdList,
                  const FrameGenerationInputs& inputs,
                  bool enabled,
                  uint32_t requestedExtraFrames,
                  uint32_t* outGeneratedFrames) override;

    void resetHistory() override;

    // Optional debug overlay flags (tear lines / pacing lines / motion-flow debug view).
    void setDebugFlags(bool tearLines, bool pacingLines, bool debugView);
    void setDebugView(bool enabled) override;
    bool isDebugViewEnabled() const override;

    // Queries GPU memory footprint reported by FidelityFX FG + Swapchain contexts.
    bool queryGpuMemoryUsage(uint64_t* outFgTotalBytes, uint64_t* outSwapChainTotalBytes) const;

    const char* providerVersionString() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace edpe
