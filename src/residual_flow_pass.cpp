#include "residual_flow_pass.h"
#include "residual_flow_shader.h"
#include "renderdoc_support.h"
#include "log.h"

#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>

namespace edpe {

namespace {

bool isWineRuntimeRFlow() {
    static const bool s_wine = []() {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }();
    return s_wine;
}

bool compileRFlowCs(ID3D11Device* device,
                    const char* source,
                    size_t sourceLen,
                    const char* entryPoint,
                    Microsoft::WRL::ComPtr<ID3D11ComputeShader>& outCs) {
    if (!device || !source || !entryPoint) return false;
    const UINT flags = isWineRuntimeRFlow() ? D3DCOMPILE_OPTIMIZATION_LEVEL1 : D3DCOMPILE_OPTIMIZATION_LEVEL3;
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    Microsoft::WRL::ComPtr<ID3DBlob> err;
    const HRESULT hr = D3DCompile(source, sourceLen, nullptr, nullptr, nullptr,
        entryPoint, "cs_5_0", flags, 0, &code, &err);
    if (FAILED(hr) || !code) {
        wchar_t errWide[256]{};
        if (err && err->GetBufferPointer() && err->GetBufferSize() > 0) {
            const char* raw = static_cast<const char*>(err->GetBufferPointer());
            const int rawLen = static_cast<int>(err->GetBufferSize() > 240 ? 240 : err->GetBufferSize());
            MultiByteToWideChar(CP_UTF8, 0, raw, rawLen, errWide, 255);
            for (wchar_t& ch : errWide) {
                if (ch == L'\r' || ch == L'\n') ch = L' ';
            }
        }
        wchar_t entryW[64]{};
        MultiByteToWideChar(CP_UTF8, 0, entryPoint, -1, entryW, 63);
        wchar_t msg[512]{};
        swprintf_s(msg, L"EDPE: ResidualFlowPass D3DCompile(%ls) failed hr=0x%08X%ls%ls",
            entryW, static_cast<unsigned>(hr), errWide[0] ? L" err=" : L"", errWide);
        EdpeLog(msg);
        return false;
    }
    const HRESULT hrCs = device->CreateComputeShader(
        code->GetBufferPointer(), code->GetBufferSize(), nullptr, &outCs);
    if (FAILED(hrCs)) {
        wchar_t entryW[64]{};
        MultiByteToWideChar(CP_UTF8, 0, entryPoint, -1, entryW, 63);
        wchar_t msg[192]{};
        swprintf_s(msg, L"EDPE: ResidualFlowPass CreateComputeShader(%ls) failed hr=0x%08X",
            entryW, static_cast<unsigned>(hrCs));
        EdpeLog(msg);
        return false;
    }
    return true;
}

bool createRwTex2D(ID3D11Device* device,
                   uint32_t width, uint32_t height, DXGI_FORMAT fmt,
                   const char* debugName,
                   Microsoft::WRL::ComPtr<ID3D11Texture2D>& outTex,
                   Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSrv,
                   Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>& outUav) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &outTex)) ||
        FAILED(device->CreateShaderResourceView(outTex.Get(), nullptr, &outSrv)) ||
        FAILED(device->CreateUnorderedAccessView(outTex.Get(), nullptr, &outUav))) {
        outTex.Reset();
        outSrv.Reset();
        outUav.Reset();
        return false;
    }
    if (debugName) {
        SetD3D11DebugName(outTex.Get(), debugName);
    }
    return true;
}

} // namespace

bool ResidualFlowPass::initialize(ID3D11Device* device) {
    if (!device || initialized_ || device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        return false;
    }
    if (!compileRFlowCs(device, kRFlowPyr0Shader, sizeof(kRFlowPyr0Shader) - 1, "cs_rflow_pyr0", pyr0Cs_) ||
        !compileRFlowCs(device, kRFlowPyr1Shader, sizeof(kRFlowPyr1Shader) - 1, "cs_rflow_pyr1", pyr1Cs_) ||
        !compileRFlowCs(device, kRFlowCoarseShader, sizeof(kRFlowCoarseShader) - 1, "cs_rflow_coarse", coarseCs_) ||
        !compileRFlowCs(device, kRFlowRefineShader, sizeof(kRFlowRefineShader) - 1, "cs_rflow_refine", refineCs_) ||
        !compileRFlowCs(device, kRFlowApplyShader, sizeof(kRFlowApplyShader) - 1, "cs_rflow_apply", applyCs_)) {
        shutdown();
        return false;
    }

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = 16u * sizeof(float); // 4 x float4 (covers all 5 shaders)
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &cb_))) {
        shutdown();
        return false;
    }

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sampDesc, &linearSampler_))) {
        shutdown();
        return false;
    }

    device_ = device;
    initialized_ = true;
    return true;
}

void ResidualFlowPass::shutdown() {
    for (int i = 0; i < 2; ++i) {
        lumaQTex_[i].Reset();
        lumaQSrv_[i].Reset();
        lumaQUav_[i].Reset();
        lumaETex_[i].Reset();
        lumaESrv_[i].Reset();
        lumaEUav_[i].Reset();
    }
    coarseTex_.Reset();
    coarseSrv_.Reset();
    coarseUav_.Reset();
    refineTex_.Reset();
    refineSrv_.Reset();
    refineUav_.Reset();
    outMotionTex_.Reset();
    outMotionSrv_.Reset();
    outMotionUav_.Reset();
    linearSampler_.Reset();
    cb_.Reset();
    pyr0Cs_.Reset();
    pyr1Cs_.Reset();
    coarseCs_.Reset();
    refineCs_.Reset();
    applyCs_.Reset();
    device_.Reset();
    width_ = height_ = 0;
    qWidth_ = qHeight_ = 0;
    eWidth_ = eHeight_ = 0;
    curSlot_ = 0;
    hasPrev_ = false;
    initialized_ = false;
}

bool ResidualFlowPass::ensureTargets(uint32_t width, uint32_t height) {
    if (!device_ || width == 0 || height == 0) return false;
    if (outMotionTex_ && width == width_ && height == height_) {
        return true;
    }

    const uint32_t qW = (width + 3u) / 4u;
    const uint32_t qH = (height + 3u) / 4u;
    const uint32_t eW = (qW + 1u) / 2u;
    const uint32_t eH = (qH + 1u) / 2u;

    for (int i = 0; i < 2; ++i) {
        lumaQTex_[i].Reset();
        lumaQSrv_[i].Reset();
        lumaQUav_[i].Reset();
        lumaETex_[i].Reset();
        lumaESrv_[i].Reset();
        lumaEUav_[i].Reset();
    }
    coarseTex_.Reset();
    coarseSrv_.Reset();
    coarseUav_.Reset();
    refineTex_.Reset();
    refineSrv_.Reset();
    refineUav_.Reset();
    outMotionTex_.Reset();
    outMotionSrv_.Reset();
    outMotionUav_.Reset();

    for (int i = 0; i < 2; ++i) {
        if (!createRwTex2D(device_.Get(), qW, qH, DXGI_FORMAT_R16G16B16A16_FLOAT,
                "EDPE_RFlowLumaQ", lumaQTex_[i], lumaQSrv_[i], lumaQUav_[i]) ||
            !createRwTex2D(device_.Get(), eW, eH, DXGI_FORMAT_R16G16B16A16_FLOAT,
                "EDPE_RFlowLumaE", lumaETex_[i], lumaESrv_[i], lumaEUav_[i])) {
            return false;
        }
    }
    if (!createRwTex2D(device_.Get(), eW, eH, DXGI_FORMAT_R16G16B16A16_FLOAT,
            "EDPE_RFlowCoarse", coarseTex_, coarseSrv_, coarseUav_) ||
        !createRwTex2D(device_.Get(), qW, qH, DXGI_FORMAT_R16G16B16A16_FLOAT,
            "EDPE_RFlowRefine", refineTex_, refineSrv_, refineUav_) ||
        !createRwTex2D(device_.Get(), width, height, DXGI_FORMAT_R16G16_FLOAT,
            "EDPE_RFlowOutMotion", outMotionTex_, outMotionSrv_, outMotionUav_)) {
        return false;
    }

    width_ = width;
    height_ = height;
    qWidth_ = qW;
    qHeight_ = qH;
    eWidth_ = eW;
    eHeight_ = eH;
    curSlot_ = 0;
    hasPrev_ = false;
    return true;
}

bool ResidualFlowPass::render(ID3D11DeviceContext* context,
                              ID3D11ShaderResourceView* currColorSrv,
                              ID3D11ShaderResourceView* baseMotionSrv,
                              ID3D11ShaderResourceView* currDepthSrv,
                              ID3D11ShaderResourceView* motionClassSrv,
                              uint32_t width, uint32_t height,
                              float jitterPxX, float jitterPxY,
                              float prevJitterPxX, float prevJitterPxY,
                              bool reset) {
    if (!initialized_ || !context || !currColorSrv || !baseMotionSrv || !currDepthSrv ||
        width == 0 || height == 0 ||
        !std::isfinite(jitterPxX) || !std::isfinite(jitterPxY) ||
        !std::isfinite(prevJitterPxX) || !std::isfinite(prevJitterPxY)) {
        return false;
    }
    if (!ensureTargets(width, height)) {
        return false;
    }

    curSlot_ ^= 1u;
    const uint32_t prevSlot = curSlot_ ^ 1u;
    const bool usePrev = hasPrev_ && !reset;

    ID3D11ShaderResourceView* const emptySrvs[4] = {nullptr, nullptr, nullptr, nullptr};
    ID3D11UnorderedAccessView* const emptyUavs[1] = {nullptr};
    ID3D11Buffer* const cbPtr = cb_.Get();
    ID3D11SamplerState* const smpPtr = linearSampler_.Get();
    ID3D11SamplerState* const nullSmp = nullptr;
    ID3D11Buffer* const nullCb = nullptr;

    const float rW = static_cast<float>(width_);
    const float rH = static_cast<float>(height_);
    const float qW = static_cast<float>(qWidth_);
    const float qH = static_cast<float>(qHeight_);
    const float eW = static_cast<float>(eWidth_);
    const float eH = static_cast<float>(eHeight_);
    const float jDeltaX = prevJitterPxX - jitterPxX;
    const float jDeltaY = prevJitterPxY - jitterPxY;

    // Pass 0: Quarter-res pyramid (4x4 render pixels -> LumaQ)
    {
        const float cbData[16] = {
            rW, rH, 1.0f / rW, 1.0f / rH,
            qW, qH, 1.0f / qW, 1.0f / qH,
            motionClassSrv ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f
        };
        context->UpdateSubresource(cb_.Get(), 0, nullptr, cbData, 0, 0);
        ID3D11ShaderResourceView* srvs[3] = {currColorSrv, currDepthSrv, motionClassSrv};
        ID3D11UnorderedAccessView* uavs[1] = {lumaQUav_[curSlot_].Get()};
        context->CSSetShader(pyr0Cs_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &cbPtr);
        context->CSSetShaderResources(0, 3, srvs);
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        context->Dispatch((qWidth_ + 7u) / 8u, (qHeight_ + 7u) / 8u, 1u);
        context->CSSetUnorderedAccessViews(0, 1, emptyUavs, nullptr);
        context->CSSetShaderResources(0, 3, emptySrvs);
    }

    // Pass 1: Eighth-res pyramid (2x2 quarter blocks -> LumaE)
    {
        const float cbData[16] = {
            qW, qH, 1.0f / qW, 1.0f / qH,
            eW, eH, 1.0f / eW, 1.0f / eH,
            0.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f
        };
        context->UpdateSubresource(cb_.Get(), 0, nullptr, cbData, 0, 0);
        ID3D11ShaderResourceView* srvs[1] = {lumaQSrv_[curSlot_].Get()};
        ID3D11UnorderedAccessView* uavs[1] = {lumaEUav_[curSlot_].Get()};
        context->CSSetShader(pyr1Cs_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &cbPtr);
        context->CSSetShaderResources(0, 1, srvs);
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        context->Dispatch((eWidth_ + 7u) / 8u, (eHeight_ + 7u) / 8u, 1u);
        context->CSSetUnorderedAccessViews(0, 1, emptyUavs, nullptr);
        context->CSSetShaderResources(0, 1, emptySrvs);
    }

    // Pass 2: Coarse search on eighth-res pyramid (only meaningful when usePrev)
    if (usePrev) {
        const float cbData[16] = {
            rW, rH, 1.0f / rW, 1.0f / rH,
            eW, eH, 1.0f / eW, 1.0f / eH,
            0.0f, 0.0f, jDeltaX, jDeltaY,
            usePrev ? 1.0f : 0.0f, reset ? 1.0f : 0.0f, 0.0f, 0.0f
        };
        context->UpdateSubresource(cb_.Get(), 0, nullptr, cbData, 0, 0);
        ID3D11ShaderResourceView* srvs[3] = {lumaESrv_[curSlot_].Get(), lumaESrv_[prevSlot].Get(), baseMotionSrv};
        ID3D11UnorderedAccessView* uavs[1] = {coarseUav_.Get()};
        context->CSSetShader(coarseCs_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &cbPtr);
        context->CSSetSamplers(0, 1, &smpPtr);
        context->CSSetShaderResources(0, 3, srvs);
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        context->Dispatch((eWidth_ + 7u) / 8u, (eHeight_ + 7u) / 8u, 1u);
        context->CSSetUnorderedAccessViews(0, 1, emptyUavs, nullptr);
        context->CSSetShaderResources(0, 3, emptySrvs);
    }

    // Pass 3: Fine search + strict confidence gate on quarter-res pyramid
    if (usePrev) {
        const float cbData[16] = {
            rW, rH, 1.0f / rW, 1.0f / rH,
            qW, qH, 1.0f / qW, 1.0f / qH,
            eW, eH, jDeltaX, jDeltaY,
            usePrev ? 1.0f : 0.0f, reset ? 1.0f : 0.0f, 0.0f, 0.0f
        };
        context->UpdateSubresource(cb_.Get(), 0, nullptr, cbData, 0, 0);
        ID3D11ShaderResourceView* srvs[4] = {
            lumaQSrv_[curSlot_].Get(), lumaQSrv_[prevSlot].Get(), baseMotionSrv, coarseSrv_.Get()
        };
        ID3D11UnorderedAccessView* uavs[1] = {refineUav_.Get()};
        context->CSSetShader(refineCs_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &cbPtr);
        context->CSSetSamplers(0, 1, &smpPtr);
        context->CSSetShaderResources(0, 4, srvs);
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        context->Dispatch((qWidth_ + 7u) / 8u, (qHeight_ + 7u) / 8u, 1u);
        context->CSSetUnorderedAccessViews(0, 1, emptyUavs, nullptr);
        context->CSSetShaderResources(0, 4, emptySrvs);
    }

    // Pass 4: Full-res bilateral + consensus application
    {
        const float cbData[16] = {
            rW, rH, 1.0f / rW, 1.0f / rH,
            qW, qH, 1.0f / qW, 1.0f / qH,
            usePrev ? 1.0f : 0.0f, reset ? 1.0f : 0.0f, motionClassSrv ? 1.0f : 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f
        };
        context->UpdateSubresource(cb_.Get(), 0, nullptr, cbData, 0, 0);
        ID3D11ShaderResourceView* srvs[4] = {
            baseMotionSrv, currDepthSrv, motionClassSrv, usePrev ? refineSrv_.Get() : nullptr
        };
        ID3D11UnorderedAccessView* uavs[1] = {outMotionUav_.Get()};
        context->CSSetShader(applyCs_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &cbPtr);
        context->CSSetShaderResources(0, 4, srvs);
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        context->Dispatch((width_ + 7u) / 8u, (height_ + 7u) / 8u, 1u);
        context->CSSetUnorderedAccessViews(0, 1, emptyUavs, nullptr);
        context->CSSetShaderResources(0, 4, emptySrvs);
    }

    context->CSSetSamplers(0, 1, &nullSmp);
    context->CSSetConstantBuffers(0, 1, &nullCb);
    context->CSSetShader(nullptr, nullptr, 0);

    hasPrev_ = true;
    return true;
}

} // namespace edpe
