#include "sampler_bias_manager.h"
#include "config.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace edpe {

SamplerBiasManager& SamplerBiasManager::instance() {
    // Heap-allocate intentionally so shutdown never releases D3D11 COM objects under LoaderLock.
    static auto* s_instance = new SamplerBiasManager();
    return *s_instance;
}

bool SamplerBiasManager::isEligibleDesc(const D3D11_SAMPLER_DESC& desc) {
    const bool isComparison = D3D11_DECODE_IS_COMPARISON_FILTER(desc.Filter);
    const bool isLinearMip = (D3D11_DECODE_MIP_FILTER(desc.Filter) == D3D11_FILTER_TYPE_LINEAR) ||
                             D3D11_DECODE_IS_ANISOTROPIC_FILTER(desc.Filter);
    const bool hasMipmaps = desc.MaxLOD > 0.5f;
    return !isComparison && isLinearMip && hasMipmaps;
}

float SamplerBiasManager::computeLiveMipLodBias(
    int backend, int mode, uint32_t renderWidth, uint32_t outputWidth, float userOffset) {
    if (backend == 0) return 0.0f;

    const float ratio = (renderWidth > 0 && outputWidth > 0)
        ? (static_cast<float>(renderWidth) / static_cast<float>(outputWidth))
        : Config::getTargetSsaaMultiplier(backend, mode);
    const float baseBias = (ratio > 0.0f && ratio < 1.0f) ? std::log2(ratio) : 0.0f;
    const float cleanOffset = std::isfinite(userOffset) ? std::clamp(userOffset, -2.0f, 2.0f) : 0.0f;
    return std::clamp(baseBias + cleanOffset, -3.0f, 2.0f);
}

int SamplerBiasManager::quantizeBias(float bias) {
    const float clean = std::isfinite(bias) ? std::clamp(bias, -3.0f, 2.0f) : 0.0f;
    return static_cast<int>(std::lround(clean * 100.0f));
}

void SamplerBiasManager::setOriginalCreateSampler(CreateSamplerFn fn) {
    origCreateSampler_.store(fn, std::memory_order_release);
}

void SamplerBiasManager::registerSampler(
    ID3D11Device* device, const D3D11_SAMPLER_DESC& origDesc, ID3D11SamplerState* origSampler) {
    if (!device || !origSampler || !isEligibleDesc(origDesc)) return;

    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t n = count_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; ++i) {
        if (entries_[i].origSampler == origSampler) {
            if (entries_[i].device != device ||
                std::memcmp(&entries_[i].origDesc, &origDesc, sizeof(D3D11_SAMPLER_DESC)) != 0) {
                entries_[i].activeBiasQ.store(0, std::memory_order_release);
                entries_[i].activeSampler.store(nullptr, std::memory_order_release);
                entries_[i].variants.clear();
                entries_[i].device = device;
                entries_[i].origDesc = origDesc;
            }
            return;
        }
    }

    if (n >= kMaxTrackedSamplers) return;

    Entry& e = entries_[n];
    e.origSampler = origSampler;
    e.device = device;
    e.origDesc = origDesc;
    e.activeBiasQ.store(0, std::memory_order_relaxed);
    e.activeSampler.store(nullptr, std::memory_order_relaxed);
    e.variants.clear();
    fastOrigPtrs_[n].store(origSampler, std::memory_order_relaxed);
    count_.store(n + 1, std::memory_order_release);
}

void SamplerBiasManager::setTargetBias(float bias) {
    const float clean = std::isfinite(bias) ? std::clamp(bias, -3.0f, 2.0f) : 0.0f;
    const int q = quantizeBias(clean);
    targetBiasFloat_.store(static_cast<float>(q) * 0.01f, std::memory_order_relaxed);
    targetBiasQ_.store(q, std::memory_order_release);
}

float SamplerBiasManager::getTargetBias() const {
    return targetBiasFloat_.load(std::memory_order_relaxed);
}

int SamplerBiasManager::getTargetBiasQ() const {
    return targetBiasQ_.load(std::memory_order_acquire);
}

ID3D11SamplerState* SamplerBiasManager::getOrCreateBiasedLocked(uint32_t entryIdx, int biasQ) {
    if (entryIdx >= kMaxTrackedSamplers || biasQ == 0) return nullptr;

    std::lock_guard<std::mutex> lock(mutex_);
    if (entryIdx >= count_.load(std::memory_order_relaxed)) return nullptr;

    Entry& entry = entries_[entryIdx];
    if (entry.activeBiasQ.load(std::memory_order_relaxed) == biasQ) {
        if (ID3D11SamplerState* cur = entry.activeSampler.load(std::memory_order_relaxed)) {
            return cur;
        }
    }

    auto it = entry.variants.find(biasQ);
    if (it != entry.variants.end() && it->second) {
        ID3D11SamplerState* cached = it->second.Get();
        entry.activeSampler.store(cached, std::memory_order_release);
        entry.activeBiasQ.store(biasQ, std::memory_order_release);
        return cached;
    }

    const CreateSamplerFn createFn = origCreateSampler_.load(std::memory_order_acquire);
    if (!entry.device || !createFn) return nullptr;

    D3D11_SAMPLER_DESC biasedDesc = entry.origDesc;
    biasedDesc.MipLODBias = std::clamp(
        entry.origDesc.MipLODBias + static_cast<float>(biasQ) * 0.01f, -15.99f, 15.99f);

    ID3D11SamplerState* createdRaw = nullptr;
    if (FAILED(createFn(entry.device, &biasedDesc, &createdRaw)) || !createdRaw) {
        return nullptr;
    }

    Microsoft::WRL::ComPtr<ID3D11SamplerState> holder;
    holder.Attach(createdRaw);
    entry.variants.emplace(biasQ, holder);
    entry.activeSampler.store(holder.Get(), std::memory_order_release);
    entry.activeBiasQ.store(biasQ, std::memory_order_release);
    return holder.Get();
}

bool SamplerBiasManager::remapSamplers(
    UINT numSamplers, ID3D11SamplerState* const* inSamplers, ID3D11SamplerState** outSamplers) {
    const int q = targetBiasQ_.load(std::memory_order_acquire);
    if (q == 0 || !inSamplers || !outSamplers || numSamplers == 0) {
        return false;
    }
    const uint32_t n = std::min<uint32_t>(count_.load(std::memory_order_acquire), kMaxTrackedSamplers);
    if (n == 0) {
        return false;
    }

    bool replaced = false;
    for (UINT i = 0; i < numSamplers; ++i) {
        ID3D11SamplerState* const s = inSamplers[i];
        outSamplers[i] = s;
        if (!s) continue;

        for (uint32_t j = 0; j < n; ++j) {
            if (fastOrigPtrs_[j].load(std::memory_order_relaxed) != s) continue;

            Entry& entry = entries_[j];
            ID3D11SamplerState* biased = nullptr;
            if (entry.activeBiasQ.load(std::memory_order_acquire) == q) {
                biased = entry.activeSampler.load(std::memory_order_acquire);
            }
            if (!biased) {
                biased = getOrCreateBiasedLocked(j, q);
            }
            if (biased && biased != s) {
                outSamplers[i] = biased;
                replaced = true;
            }
            break;
        }
    }

    if (replaced) {
        remappedCallCount_.fetch_add(1, std::memory_order_relaxed);
    }
    return replaced;
}

uint32_t SamplerBiasManager::trackedCount() const {
    return count_.load(std::memory_order_acquire);
}

uint32_t SamplerBiasManager::remappedCallCount() const {
    return remappedCallCount_.load(std::memory_order_acquire);
}

void SamplerBiasManager::resetForTests() {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t n = std::min<uint32_t>(count_.load(std::memory_order_relaxed), kMaxTrackedSamplers);
    for (uint32_t i = 0; i < n; ++i) {
        entries_[i].activeBiasQ.store(0, std::memory_order_relaxed);
        entries_[i].activeSampler.store(nullptr, std::memory_order_relaxed);
        entries_[i].variants.clear();
        entries_[i].origSampler = nullptr;
        entries_[i].device = nullptr;
        entries_[i].origDesc = {};
        fastOrigPtrs_[i].store(nullptr, std::memory_order_relaxed);
    }
    count_.store(0, std::memory_order_release);
    targetBiasQ_.store(0, std::memory_order_release);
    targetBiasFloat_.store(0.0f, std::memory_order_relaxed);
    remappedCallCount_.store(0, std::memory_order_relaxed);
    origCreateSampler_.store(nullptr, std::memory_order_release);
}

} // namespace edpe
