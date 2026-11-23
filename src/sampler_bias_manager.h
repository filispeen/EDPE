#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace edpe {

/// Tracks eligible D3D11 sampler states created by the game and provides
/// cached MipLODBias-adjusted variants for lock-free runtime remapping in PSSetSamplers.
class SamplerBiasManager {
public:
    using CreateSamplerFn = HRESULT(STDMETHODCALLTYPE*)(
        ID3D11Device*, const D3D11_SAMPLER_DESC*, ID3D11SamplerState**);

    static constexpr uint32_t kMaxTrackedSamplers = 512;

    static SamplerBiasManager& instance();

    static bool isEligibleDesc(const D3D11_SAMPLER_DESC& desc);
    static float computeLiveMipLodBias(
        int backend, int mode, uint32_t renderWidth, uint32_t outputWidth, float userOffset);
    static int quantizeBias(float bias);

    void setOriginalCreateSampler(CreateSamplerFn fn);
    void registerSampler(
        ID3D11Device* device, const D3D11_SAMPLER_DESC& origDesc, ID3D11SamplerState* origSampler);

    void setTargetBias(float bias);
    float getTargetBias() const;
    int getTargetBiasQ() const;

    bool remapSamplers(
        UINT numSamplers, ID3D11SamplerState* const* inSamplers, ID3D11SamplerState** outSamplers);

    uint32_t trackedCount() const;
    uint32_t remappedCallCount() const;
    void resetForTests();

private:
    SamplerBiasManager() = default;
    ~SamplerBiasManager() = default;

    struct Entry {
        ID3D11SamplerState* origSampler = nullptr;
        ID3D11Device* device = nullptr;
        D3D11_SAMPLER_DESC origDesc{};
        std::atomic<int> activeBiasQ{0};
        std::atomic<ID3D11SamplerState*> activeSampler{nullptr};
        std::unordered_map<int, Microsoft::WRL::ComPtr<ID3D11SamplerState>> variants;
    };

    ID3D11SamplerState* getOrCreateBiasedLocked(uint32_t entryIdx, int biasQ);

    mutable std::mutex mutex_;
    std::atomic<CreateSamplerFn> origCreateSampler_{nullptr};
    std::atomic<uint32_t> count_{0};
    std::atomic<int> targetBiasQ_{0};
    std::atomic<float> targetBiasFloat_{0.0f};
    std::atomic<uint32_t> remappedCallCount_{0};

    std::array<std::atomic<ID3D11SamplerState*>, kMaxTrackedSamplers> fastOrigPtrs_{};
    std::array<Entry, kMaxTrackedSamplers> entries_{};
};

} // namespace edpe
