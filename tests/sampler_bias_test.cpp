#include "sampler_bias_manager.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cfloat>
#include <cmath>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace {

HRESULT STDMETHODCALLTYPE rawCreateSampler(
    ID3D11Device* dev, const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** out) {
    return dev ? dev->CreateSamplerState(desc, out) : E_INVALIDARG;
}

D3D11_SAMPLER_DESC makeDefaultSampler(D3D11_FILTER filter, float maxLod, float bias = 0.0f) {
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = filter;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MipLODBias = bias;
    sd.MaxAnisotropy = (filter == D3D11_FILTER_ANISOTROPIC) ? 8u : 1u;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0.0f;
    sd.MaxLOD = maxLod;
    return sd;
}

bool testEligibilityAndMath() {
    using SBM = edpe::SamplerBiasManager;

    if (!SBM::isEligibleDesc(makeDefaultSampler(D3D11_FILTER_ANISOTROPIC, 12.0f))) return false;
    if (!SBM::isEligibleDesc(makeDefaultSampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, FLT_MAX))) return false;
    if (SBM::isEligibleDesc(makeDefaultSampler(D3D11_FILTER_MIN_MAG_MIP_POINT, 12.0f))) return false;
    if (SBM::isEligibleDesc(makeDefaultSampler(D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT, 12.0f))) return false;
    if (SBM::isEligibleDesc(makeDefaultSampler(D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR, 12.0f))) return false;
    if (SBM::isEligibleDesc(makeDefaultSampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, 0.0f))) return false;

    if (SBM::computeLiveMipLodBias(0, 4, 1280, 2560, -0.5f) != 0.0f) return false;
    if (std::fabs(SBM::computeLiveMipLodBias(1, 4, 1280, 2560, 0.0f) - (-1.0f)) > 1e-4f) return false;
    if (std::fabs(SBM::computeLiveMipLodBias(1, 4, 1280, 2560, 0.25f) - (-0.75f)) > 1e-4f) return false;
    const float expectedQuality = std::log2(1707.0f / 2560.0f);
    if (std::fabs(SBM::computeLiveMipLodBias(1, 2, 1707, 2560, 0.0f) - expectedQuality) > 1e-4f) return false;
    if (SBM::computeLiveMipLodBias(1, 1, 2560, 2560, 0.0f) != 0.0f) return false;
    if (SBM::computeLiveMipLodBias(1, 5, 640, 2560, -2.0f) != -3.0f) return false;
    // Native TAA (backend=3) clamps MipLODBias >= -0.25f to prevent grazing-angle mip flicker
    if (SBM::computeLiveMipLodBias(3, 1, 2560, 2560, 0.0f) != 0.0f) return false;
    if (std::fabs(SBM::computeLiveMipLodBias(3, 1, 2560, 2560, -1.0f) - (-0.25f)) > 1e-4f) return false;
    if (std::fabs(SBM::computeLiveMipLodBias(3, 1, 2560, 2560, 0.50f) - 0.50f) > 1e-4f) return false;
    return true;
}

bool testLiveRemapping() {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    }
    if (FAILED(hr) || !dev) {
        std::printf("  -> FAIL: D3D11CreateDevice failed\n");
        return false;
    }

    auto& mgr = edpe::SamplerBiasManager::instance();
    mgr.resetForTests();
    mgr.setOriginalCreateSampler(&rawCreateSampler);

    const auto anisoDesc = makeDefaultSampler(D3D11_FILTER_ANISOTROPIC, 12.0f, 0.25f);
    const auto pointDesc = makeDefaultSampler(D3D11_FILTER_MIN_MAG_MIP_POINT, 12.0f, 0.0f);
    ComPtr<ID3D11SamplerState> origAniso, origPoint;
    if (FAILED(dev->CreateSamplerState(&anisoDesc, &origAniso)) ||
        FAILED(dev->CreateSamplerState(&pointDesc, &origPoint))) {
        mgr.resetForTests();
        return false;
    }

    mgr.registerSampler(dev.Get(), anisoDesc, origAniso.Get());
    mgr.registerSampler(dev.Get(), pointDesc, origPoint.Get());
    if (mgr.trackedCount() != 1u) {
        std::printf("  -> FAIL: expected trackedCount=1 got %u\n", mgr.trackedCount());
        mgr.resetForTests();
        return false;
    }

    ID3D11SamplerState* in[3] = { origAniso.Get(), origPoint.Get(), nullptr };
    ID3D11SamplerState* out[3] = {};

    mgr.setTargetBias(0.0f);
    if (mgr.remapSamplers(3, in, out)) {
        std::printf("  -> FAIL: bias 0.0f should not remap\n");
        mgr.resetForTests();
        return false;
    }

    mgr.setTargetBias(-1.0f);
    if (!mgr.remapSamplers(3, in, out) || out[0] == origAniso.Get() || out[1] != origPoint.Get() || out[2] != nullptr) {
        std::printf("  -> FAIL: remap at -1.0 failed\n");
        mgr.resetForTests();
        return false;
    }
    ComPtr<ID3D11SamplerState> cachedMinusOne = out[0];
    D3D11_SAMPLER_DESC gotDesc{};
    cachedMinusOne->GetDesc(&gotDesc);
    if (std::fabs(gotDesc.MipLODBias - (-0.75f)) > 1e-3f) {
        std::printf("  -> FAIL: expected MipLODBias -0.75, got %.3f\n", gotDesc.MipLODBias);
        mgr.resetForTests();
        return false;
    }

    mgr.setTargetBias(-0.58f);
    if (!mgr.remapSamplers(3, in, out) || out[0] == cachedMinusOne.Get()) {
        std::printf("  -> FAIL: remap at -0.58 failed\n");
        mgr.resetForTests();
        return false;
    }
    out[0]->GetDesc(&gotDesc);
    if (std::fabs(gotDesc.MipLODBias - (-0.33f)) > 1e-3f) {
        std::printf("  -> FAIL: expected MipLODBias -0.33, got %.3f\n", gotDesc.MipLODBias);
        mgr.resetForTests();
        return false;
    }

    // Switching back to -1.0f must reuse exact cached sampler pointer.
    mgr.setTargetBias(-1.0f);
    if (!mgr.remapSamplers(3, in, out) || out[0] != cachedMinusOne.Get()) {
        std::printf("  -> FAIL: expected cached sampler reuse for -1.0\n");
        mgr.resetForTests();
        return false;
    }

    // Re-registering same pointer with same desc preserves cached pointer; changing desc invalidates.
    mgr.registerSampler(dev.Get(), anisoDesc, origAniso.Get());
    if (!mgr.remapSamplers(3, in, out) || out[0] != cachedMinusOne.Get()) {
        std::printf("  -> FAIL: duplicate registration unexpectedly invalidated cache\n");
        mgr.resetForTests();
        return false;
    }

    const auto changedDesc = makeDefaultSampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, 8.0f, 0.0f);
    mgr.registerSampler(dev.Get(), changedDesc, origAniso.Get());
    if (!mgr.remapSamplers(3, in, out) || out[0] == cachedMinusOne.Get()) {
        std::printf("  -> FAIL: changed desc did not invalidate stale variant\n");
        mgr.resetForTests();
        return false;
    }
    out[0]->GetDesc(&gotDesc);
    if (gotDesc.Filter != D3D11_FILTER_MIN_MAG_MIP_LINEAR || std::fabs(gotDesc.MipLODBias - (-1.0f)) > 1e-3f) {
        std::printf("  -> FAIL: changed desc variant properties incorrect\n");
        mgr.resetForTests();
        return false;
    }

    // Native TAA: ForceAniso16x upgrades MIN_MAG_MIP_LINEAR -> 16x ANISOTROPIC even at bias=0.0f,
    // and NativeTaaBiasGuard lifts negative game-authored MipLODBias to >= 0.0f (or >= -0.25f with offset).
    mgr.resetForTests();
    mgr.setOriginalCreateSampler(&rawCreateSampler);
    const auto linearMipDesc = makeDefaultSampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, 12.0f, -0.75f);
    ComPtr<ID3D11SamplerState> origLinearNeg;
    if (FAILED(dev->CreateSamplerState(&linearMipDesc, &origLinearNeg))) {
        mgr.resetForTests();
        return false;
    }
    mgr.registerSampler(dev.Get(), linearMipDesc, origLinearNeg.Get());
    mgr.registerSampler(dev.Get(), pointDesc, origPoint.Get());
    ID3D11SamplerState* taaIn[2] = { origLinearNeg.Get(), origPoint.Get() };
    ID3D11SamplerState* taaOut[2] = {};
    mgr.setTargetBias(0.0f);
    mgr.setForceAniso16x(true);
    mgr.setNativeTaaBiasGuard(true);
    if (!mgr.remapSamplers(2, taaIn, taaOut) || taaOut[0] == origLinearNeg.Get() || taaOut[1] != origPoint.Get()) {
        std::printf("  -> FAIL: TAA ForceAniso16x / NativeTaaBiasGuard remap failed\n");
        mgr.resetForTests();
        return false;
    }
    taaOut[0]->GetDesc(&gotDesc);
    if (gotDesc.Filter != D3D11_FILTER_ANISOTROPIC || gotDesc.MaxAnisotropy != 16u ||
        std::fabs(gotDesc.MipLODBias - 0.0f) > 1e-3f) {
        std::printf("  -> FAIL: expected 16x AF and MipLODBias=0.0, got filter=%u aniso=%u bias=%.3f\n",
                    gotDesc.Filter, gotDesc.MaxAnisotropy, gotDesc.MipLODBias);
        mgr.resetForTests();
        return false;
    }
    mgr.setTargetBias(-0.25f);
    if (!mgr.remapSamplers(2, taaIn, taaOut)) {
        std::printf("  -> FAIL: TAA -0.25 bias remap failed\n");
        mgr.resetForTests();
        return false;
    }
    taaOut[0]->GetDesc(&gotDesc);
    if (gotDesc.Filter != D3D11_FILTER_ANISOTROPIC || gotDesc.MaxAnisotropy != 16u ||
        std::fabs(gotDesc.MipLODBias - (-0.25f)) > 1e-3f) {
        std::printf("  -> FAIL: expected 16x AF and MipLODBias=-0.25, got bias=%.3f\n", gotDesc.MipLODBias);
        mgr.resetForTests();
        return false;
    }

    std::printf("  Live sampler remapping OK (tracked=%u, calls=%u)\n",
                mgr.trackedCount(), mgr.remappedCallCount());
    mgr.resetForTests();
    return true;
}

} // namespace

int main() {
    std::printf("[SamplerBiasTest]\n");
    bool ok = true;
    ok &= testEligibilityAndMath();
    ok &= testLiveRemapping();
    std::printf("[SamplerBiasTest] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
