#include "taa_pass.h"
#include "taa_shader.h"
#include "log.h"
#include "renderdoc_support.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace edpe {

namespace {

struct TaaPrepassCB {
    float renderRes[4];
    float jitterParams[4];
    float depthParams[4];
    float camRelRow2[4];
    uint32_t flags[4];
};

struct TaaPatchDivCB {
    float renderRes[4];
    float patchRes[4];
    float flags[4];
};

struct TaaResolveCB {
    float outputRes[4];
    float renderRes[4];
    float jitterAndAlpha[4];
    float gammaAndReset[4];
    float extraParams[4];
};

struct TaaPostRcasCB {
    float resolution[2];
    float sharpness;
    float effectiveHdr;
};

struct TaaBlitCB {
    float outputRes[4];
    uint32_t debugInfo[4];
    float rcasParams[4]; // x: rcasSharpness, y: effectiveHdr, z/w: unused
};

bool isFloatColorFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return true;
    default:
        return false;
    }
}

bool isWineRuntimeTaa() {
    static const bool s_wine = []() {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }();
    return s_wine;
}

bool compileTaaShaderBlob(const char* source, size_t sourceLen,
                          const char* entryPoint, const char* profile,
                          Microsoft::WRL::ComPtr<ID3DBlob>& outBlob) {
    Microsoft::WRL::ComPtr<ID3DBlob> errBlob;
    // Compile with -O1 on Wine/Proton for d3dcompiler_47 (vkd3d-shader) safety, -O3 on native Windows.
    const UINT compileFlags = isWineRuntimeTaa()
        ? D3DCOMPILE_OPTIMIZATION_LEVEL1
        : D3DCOMPILE_OPTIMIZATION_LEVEL3;
    const HRESULT hr = D3DCompile(source, sourceLen, nullptr, nullptr, nullptr,
        entryPoint, profile, compileFlags, 0, &outBlob, &errBlob);
    if (FAILED(hr) || !outBlob) {
        wchar_t errWide[256]{};
        if (errBlob && errBlob->GetBufferPointer() && errBlob->GetBufferSize() > 0) {
            const char* raw = static_cast<const char*>(errBlob->GetBufferPointer());
            const int len = static_cast<int>((std::min<size_t>)(errBlob->GetBufferSize(), 240u));
            MultiByteToWideChar(CP_UTF8, 0, raw, len, errWide, 255);
            for (wchar_t& ch : errWide) {
                if (ch == L'\r' || ch == L'\n') ch = L' ';
            }
        }
        wchar_t msg[512]{};
        swprintf_s(msg, L"EDPE: TaaPass D3DCompile(%hs/%hs) failed hr=0x%08X%ls%ls",
            entryPoint, profile, static_cast<unsigned>(hr),
            errWide[0] ? L" err=" : L"", errWide);
        EdpeLog(msg);
        return false;
    }
    return true;
}

} // namespace

bool TaaPass::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context || device_ ||
        device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0 ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) ||
        FAILED(context->QueryInterface(IID_PPV_ARGS(&context_)))) {
        return false;
    }

    const auto level = device->GetFeatureLevel();
    const UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
        ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    if (FAILED(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
            __uuidof(ID3D11Device1), nullptr, &state_))) {
        state_.Reset();
    }

    Microsoft::WRL::ComPtr<ID3DBlob> preBlob, divBlob, resBlob, rcasBlob, dbgBlob, vsBlob, psBlob;
    if (!compileTaaShaderBlob(kTaaPrepassShader, sizeof(kTaaPrepassShader) - 1, "cs_taa_prepass", "cs_5_0", preBlob) ||
        !compileTaaShaderBlob(kTaaPatchDivShader, sizeof(kTaaPatchDivShader) - 1, "cs_taa_patch_div", "cs_5_0", divBlob) ||
        !compileTaaShaderBlob(kTaaResolveShader, sizeof(kTaaResolveShader) - 1, "cs_taa_resolve", "cs_5_0", resBlob) ||
        !compileTaaShaderBlob(kTaaPostRcasShader, sizeof(kTaaPostRcasShader) - 1, "cs_taa_post_rcas", "cs_5_0", rcasBlob) ||
        !compileTaaShaderBlob(kTaaBlitShader, sizeof(kTaaBlitShader) - 1, "cs_taa_debug", "cs_5_0", dbgBlob) ||
        !compileTaaShaderBlob(kTaaBlitShader, sizeof(kTaaBlitShader) - 1, "vs_taa_blit", "vs_5_0", vsBlob) ||
        !compileTaaShaderBlob(kTaaBlitShader, sizeof(kTaaBlitShader) - 1, "ps_taa_blit", "ps_5_0", psBlob)) {
        return false;
    }

    if (FAILED(device->CreateComputeShader(preBlob->GetBufferPointer(), preBlob->GetBufferSize(), nullptr, &prepassCs_)) ||
        FAILED(device->CreateComputeShader(divBlob->GetBufferPointer(), divBlob->GetBufferSize(), nullptr, &patchDivCs_)) ||
        FAILED(device->CreateComputeShader(resBlob->GetBufferPointer(), resBlob->GetBufferSize(), nullptr, &resolveCs_)) ||
        FAILED(device->CreateComputeShader(rcasBlob->GetBufferPointer(), rcasBlob->GetBufferSize(), nullptr, &postRcasCs_)) ||
        FAILED(device->CreateComputeShader(dbgBlob->GetBufferPointer(), dbgBlob->GetBufferSize(), nullptr, &debugCs_)) ||
        FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &blitVs_)) ||
        FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &blitPs_))) {
        return false;
    }

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0.0f;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sampDesc, &linearSampler_))) {
        return false;
    }

    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(device->CreateSamplerState(&sampDesc, &pointSampler_))) {
        return false;
    }

    auto makeCb = [&](UINT byteWidth, Microsoft::WRL::ComPtr<ID3D11Buffer>& outBuf) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = byteWidth;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        return SUCCEEDED(device->CreateBuffer(&bd, nullptr, &outBuf));
    };

    if (!makeCb(sizeof(TaaPrepassCB), prepassCb_) ||
        !makeCb(sizeof(TaaPatchDivCB), patchDivCb_) ||
        !makeCb(sizeof(TaaResolveCB), resolveCb_) ||
        !makeCb(sizeof(TaaPostRcasCB), postRcasCb_) ||
        !makeCb(sizeof(TaaBlitCB), blitCb_)) {
        return false;
    }

    device_ = device;
    resetRequested_ = true;
    hasPrevDepthCopy_ = false;
    hasPrevPrepass_ = false;
    prepassReadIdx_ = 0;
    framesSinceReset_ = 0;
    return true;
}

void TaaPass::shutdown() {
    state_.Reset();
    prepassCs_.Reset();
    patchDivCs_.Reset();
    resolveCs_.Reset();
    postRcasCs_.Reset();
    debugCs_.Reset();
    blitVs_.Reset();
    blitPs_.Reset();
    linearSampler_.Reset();
    pointSampler_.Reset();
    prepassCb_.Reset();
    patchDivCb_.Reset();
    resolveCb_.Reset();
    postRcasCb_.Reset();
    blitCb_.Reset();

    for (int i = 0; i < 2; ++i) {
        prepassTex_[i].Reset();
        prepassSrv_[i].Reset();
        prepassUav_[i].Reset();
    }
    patchDivTex_.Reset();
    patchDivSrv_.Reset();
    patchDivUav_.Reset();
    prevDepthTex_.Reset();
    prevDepthSrv_.Reset();

    for (int i = 0; i < 2; ++i) {
        historyTexture_[i].Reset();
        historySrv_[i].Reset();
        historyUav_[i].Reset();
        historyAuxTex_[i].Reset();
        historyAuxSrv_[i].Reset();
        historyAuxUav_[i].Reset();
    }
    postTex_.Reset();
    postSrv_.Reset();
    postUav_.Reset();

    displayTexture_ = nullptr;
    displaySrv_ = nullptr;
    historyReadIdx_ = 0;
    prepassReadIdx_ = 0;
    hasPrevPrepass_ = false;
    cachedDestRtv_.Reset();
    cachedDestTexture_ = nullptr;
    context_.Reset();
    device_ = nullptr;
    renderWidth_ = 0;
    renderHeight_ = 0;
    patchWidth_ = 0;
    patchHeight_ = 0;
    prevDepthWidth_ = 0;
    prevDepthHeight_ = 0;
    hasPrevDepthCopy_ = false;
    historyWidth_ = 0;
    historyHeight_ = 0;
    historyFormat_ = DXGI_FORMAT_UNKNOWN;
    resetRequested_ = true;
    framesSinceReset_ = 0;
}

bool TaaPass::ensureRenderResources(uint32_t renderWidth, uint32_t renderHeight) {
    if (!device_ || renderWidth == 0 || renderHeight == 0) return false;
    const uint32_t pW = (renderWidth + 3u) / 4u;
    const uint32_t pH = (renderHeight + 3u) / 4u;
    if (prepassTex_[0] && prepassTex_[1] && patchDivTex_ &&
        renderWidth_ == renderWidth && renderHeight_ == renderHeight &&
        patchWidth_ == pW && patchHeight_ == pH) {
        return true;
    }
    for (int i = 0; i < 2; ++i) {
        prepassTex_[i].Reset();
        prepassSrv_[i].Reset();
        prepassUav_[i].Reset();
    }
    patchDivTex_.Reset();
    patchDivSrv_.Reset();
    patchDivUav_.Reset();
    renderWidth_ = 0;
    renderHeight_ = 0;
    patchWidth_ = 0;
    patchHeight_ = 0;
    prepassReadIdx_ = 0;
    hasPrevPrepass_ = false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = renderWidth;
    td.Height = renderHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    for (int i = 0; i < 2; ++i) {
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &prepassTex_[i])) ||
            FAILED(device_->CreateShaderResourceView(prepassTex_[i].Get(), nullptr, &prepassSrv_[i])) ||
            FAILED(device_->CreateUnorderedAccessView(prepassTex_[i].Get(), nullptr, &prepassUav_[i]))) {
            for (int j = 0; j < 2; ++j) {
                prepassTex_[j].Reset();
                prepassSrv_[j].Reset();
                prepassUav_[j].Reset();
            }
            return false;
        }
        SetD3D11DebugName(prepassTex_[i].Get(), i == 0 ? "EDPE_TaaPrepass0" : "EDPE_TaaPrepass1");
    }

    D3D11_TEXTURE2D_DESC pd{};
    pd.Width = pW;
    pd.Height = pH;
    pd.MipLevels = 1;
    pd.ArraySize = 1;
    pd.Format = DXGI_FORMAT_R16G16_FLOAT;
    pd.SampleDesc.Count = 1;
    pd.Usage = D3D11_USAGE_DEFAULT;
    pd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device_->CreateTexture2D(&pd, nullptr, &patchDivTex_)) ||
        FAILED(device_->CreateShaderResourceView(patchDivTex_.Get(), nullptr, &patchDivSrv_)) ||
        FAILED(device_->CreateUnorderedAccessView(patchDivTex_.Get(), nullptr, &patchDivUav_))) {
        patchDivTex_.Reset();
        patchDivSrv_.Reset();
        patchDivUav_.Reset();
        return false;
    }
    SetD3D11DebugName(patchDivTex_.Get(), "EDPE_TaaPatchDiv");

    renderWidth_ = renderWidth;
    renderHeight_ = renderHeight;
    patchWidth_ = pW;
    patchHeight_ = pH;
    return true;
}

bool TaaPass::ensurePrevDepthTexture(uint32_t renderWidth, uint32_t renderHeight) {
    if (!device_ || renderWidth == 0 || renderHeight == 0) return false;
    if (prevDepthTex_ && prevDepthSrv_ && prevDepthWidth_ == renderWidth && prevDepthHeight_ == renderHeight) {
        return true;
    }
    prevDepthTex_.Reset();
    prevDepthSrv_.Reset();
    prevDepthWidth_ = 0;
    prevDepthHeight_ = 0;
    hasPrevDepthCopy_ = false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = renderWidth;
    td.Height = renderHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(device_->CreateTexture2D(&td, nullptr, &prevDepthTex_)) ||
        FAILED(device_->CreateShaderResourceView(prevDepthTex_.Get(), nullptr, &prevDepthSrv_))) {
        prevDepthTex_.Reset();
        prevDepthSrv_.Reset();
        return false;
    }
    SetD3D11DebugName(prevDepthTex_.Get(), "EDPE_TaaPrevDepth");
    prevDepthWidth_ = renderWidth;
    prevDepthHeight_ = renderHeight;
    return true;
}

bool TaaPass::ensureOutputResources(uint32_t width, uint32_t height, DXGI_FORMAT format) {
    if (!device_ || width == 0 || height == 0 || format == DXGI_FORMAT_UNKNOWN) return false;

    if (historyTexture_[0] || historyTexture_[1]) {
        if (historyWidth_ != width || historyHeight_ != height || historyFormat_ != format) {
            for (int i = 0; i < 2; ++i) {
                historyTexture_[i].Reset();
                historySrv_[i].Reset();
                historyUav_[i].Reset();
                historyAuxTex_[i].Reset();
                historyAuxSrv_[i].Reset();
                historyAuxUav_[i].Reset();
            }
            postTex_.Reset();
            postSrv_.Reset();
            postUav_.Reset();
            displayTexture_ = nullptr;
            displaySrv_ = nullptr;
            historyReadIdx_ = 0;
            historyWidth_ = 0;
            historyHeight_ = 0;
            historyFormat_ = DXGI_FORMAT_UNKNOWN;
            resetRequested_ = true;
            hasPrevDepthCopy_ = false;
            hasPrevPrepass_ = false;
            framesSinceReset_ = 0;
        }
    }

    if (!historyTexture_[0] || !historyTexture_[1] || !historyAuxTex_[0] || !historyAuxTex_[1]) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = format;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;

        for (int i = 0; i < 2; ++i) {
            if (FAILED(device_->CreateTexture2D(&td, nullptr, &historyTexture_[i])) ||
                FAILED(device_->CreateShaderResourceView(historyTexture_[i].Get(), nullptr, &historySrv_[i])) ||
                FAILED(device_->CreateUnorderedAccessView(historyTexture_[i].Get(), nullptr, &historyUav_[i])) ||
                FAILED(device_->CreateTexture2D(&td, nullptr, &historyAuxTex_[i])) ||
                FAILED(device_->CreateShaderResourceView(historyAuxTex_[i].Get(), nullptr, &historyAuxSrv_[i])) ||
                FAILED(device_->CreateUnorderedAccessView(historyAuxTex_[i].Get(), nullptr, &historyAuxUav_[i]))) {
                for (int j = 0; j < 2; ++j) {
                    historyTexture_[j].Reset();
                    historySrv_[j].Reset();
                    historyUav_[j].Reset();
                    historyAuxTex_[j].Reset();
                    historyAuxSrv_[j].Reset();
                    historyAuxUav_[j].Reset();
                }
                return false;
            }
            SetD3D11DebugName(historyTexture_[i].Get(), i == 0 ? "EDPE_TaaHistory0" : "EDPE_TaaHistory1");
            SetD3D11DebugName(historyAuxTex_[i].Get(), i == 0 ? "EDPE_TaaHistoryAux0" : "EDPE_TaaHistoryAux1");
        }

        historyReadIdx_ = 0;
        historyWidth_ = width;
        historyHeight_ = height;
        historyFormat_ = format;
        resetRequested_ = true;
        hasPrevDepthCopy_ = false;
        hasPrevPrepass_ = false;
        framesSinceReset_ = 0;
    }

    return true;
}

bool TaaPass::ensurePostTexture(bool needPost) {
    if (!needPost) return true;
    if (postTex_ && postSrv_ && postUav_) return true;
    if (!device_ || historyWidth_ == 0 || historyHeight_ == 0) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = historyWidth_;
    td.Height = historyHeight_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = kHistoryFormat;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    if (FAILED(device_->CreateTexture2D(&td, nullptr, &postTex_)) ||
        FAILED(device_->CreateShaderResourceView(postTex_.Get(), nullptr, &postSrv_)) ||
        FAILED(device_->CreateUnorderedAccessView(postTex_.Get(), nullptr, &postUav_))) {
        postTex_.Reset();
        postSrv_.Reset();
        postUav_.Reset();
        return false;
    }
    SetD3D11DebugName(postTex_.Get(), "EDPE_TaaPostOut");
    return true;
}

bool TaaPass::render(ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* currentFrameSrv,
    ID3D11ShaderResourceView* motionVectorsSrv,
    ID3D11ShaderResourceView* depthSrv,
    ID3D11RenderTargetView* destinationRtv,
    ID3D11Texture2D* destinationTexture,
    uint32_t width,
    uint32_t height,
    float jitterX,
    float jitterY,
    bool reset,
    uint32_t renderWidth,
    uint32_t renderHeight,
    float prevJitterX,
    float prevJitterY,
    float depthB,
    ID3D11ShaderResourceView* prevDepthSrv,
    DXGI_FORMAT /*preferredFormat*/,
    ID3D11ShaderResourceView* reactiveMaskSrv,
    ID3D11ShaderResourceView* motionClassSrv,
    const TaaSettings* settings,
    bool colorIsHdr,
    const CameraProjection* cameraNow,
    const CameraProjection* cameraPrev) {
    if (!device_ || !context || !currentFrameSrv || width == 0 || height == 0) {
        return false;
    }

    const uint32_t rW = (renderWidth > 0) ? renderWidth : width;
    const uint32_t rH = (renderHeight > 0) ? renderHeight : height;

    if (!ensureRenderResources(rW, rH) ||
        !ensureOutputResources(width, height, kHistoryFormat)) {
        return false;
    }

    bool effectiveHdr = colorIsHdr;
    if (!effectiveHdr) {
        Microsoft::WRL::ComPtr<ID3D11Resource> colorRes;
        currentFrameSrv->GetResource(&colorRes);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> colorTex;
        if (colorRes && SUCCEEDED(colorRes.As(&colorTex))) {
            D3D11_TEXTURE2D_DESC cd{};
            colorTex->GetDesc(&cd);
            if (isFloatColorFormat(cd.Format)) {
                effectiveHdr = true;
            }
        }
    }

    const TaaSettings cfg = settings ? *settings : TaaSettings{};
    const float minAlpha = std::clamp(cfg.minAlpha, 0.01f, 0.25f);
    const float maxAlpha = std::clamp(cfg.maxAlpha, minAlpha, 0.60f);
    const float baseGamma = std::clamp(cfg.baseGamma, 0.50f, 2.50f);
    const float lockMaxGamma = std::clamp(cfg.lockMaxGamma, baseGamma, 3.50f);
    const float rcasSharpness = std::clamp(cfg.rcasSharpness, 0.0f, 1.0f);
    const int debugView = (cfg.debugView >= 0 && cfg.debugView <= 4) ? cfg.debugView : 0;

    const bool zeroCopyPingPong = (destinationTexture == nullptr && destinationRtv == nullptr);
    ID3D11RenderTargetView* targetRtv = destinationRtv;
    if (!zeroCopyPingPong && !targetRtv && destinationTexture) {
        if (cachedDestTexture_ != destinationTexture || !cachedDestRtv_) {
            cachedDestRtv_.Reset();
            cachedDestTexture_ = nullptr;
            if (FAILED(device_->CreateRenderTargetView(destinationTexture, nullptr, &cachedDestRtv_))) {
                return false;
            }
            cachedDestTexture_ = destinationTexture;
        }
        targetRtv = cachedDestRtv_.Get();
    }
    if (!zeroCopyPingPong && !targetRtv) return false;

    const bool performReset = reset || resetRequested_;
    resetRequested_ = false;
    if (performReset) {
        framesSinceReset_ = 0;
        hasPrevDepthCopy_ = false;
        hasPrevPrepass_ = false;
    }

    const uint32_t readIdx = historyReadIdx_;
    const uint32_t writeIdx = readIdx ^ 1u;
    const uint32_t preReadIdx = prepassReadIdx_;
    const uint32_t preWriteIdx = preReadIdx ^ 1u;

    ID3D11ShaderResourceView* activePrevDepthSrv =
        (!performReset)
            ? ((hasPrevDepthCopy_ && prevDepthSrv_) ? prevDepthSrv_.Get() : prevDepthSrv)
            : nullptr;

    // -------------------------------------------------------------------------
    // Pass 1: cs_taa_prepass (renderWidth x renderHeight)
    // -------------------------------------------------------------------------
    {
        TaaPrepassCB pcb{};
        pcb.renderRes[0] = static_cast<float>(rW);
        pcb.renderRes[1] = static_cast<float>(rH);
        pcb.renderRes[2] = 1.0f / static_cast<float>(rW);
        pcb.renderRes[3] = 1.0f / static_cast<float>(rH);
        pcb.jitterParams[0] = jitterX;
        pcb.jitterParams[1] = jitterY;
        pcb.jitterParams[2] = prevJitterX;
        pcb.jitterParams[3] = prevJitterY;
        pcb.depthParams[0] = (depthB > 0.0f) ? depthB : 0.025f;
        pcb.depthParams[1] = 1.0f;
        pcb.depthParams[2] = 1.0f;
        pcb.depthParams[3] = effectiveHdr ? 1.0f : 0.0f;

        bool hasRelCamZ = false;
        if (!performReset && cameraNow && cameraPrev &&
            cameraNow->scaleX > 0.0f && cameraNow->scaleY > 0.0f &&
            std::isfinite(cameraNow->scaleX) && std::isfinite(cameraNow->scaleY)) {
            bool allFinite = true;
            for (float v : cameraNow->worldFromView) if (!std::isfinite(v)) allFinite = false;
            for (float v : cameraPrev->worldFromView) if (!std::isfinite(v)) allFinite = false;
            const double curFwdLen2 =
                static_cast<double>(cameraNow->worldFromView[2]) * cameraNow->worldFromView[2] +
                static_cast<double>(cameraNow->worldFromView[6]) * cameraNow->worldFromView[6] +
                static_cast<double>(cameraNow->worldFromView[10]) * cameraNow->worldFromView[10];
            const double prevFwdLen2 =
                static_cast<double>(cameraPrev->worldFromView[2]) * cameraPrev->worldFromView[2] +
                static_cast<double>(cameraPrev->worldFromView[6]) * cameraPrev->worldFromView[6] +
                static_cast<double>(cameraPrev->worldFromView[10]) * cameraPrev->worldFromView[10];
            if (allFinite && curFwdLen2 > 0.5 && prevFwdLen2 > 0.5) {
                const double dx = static_cast<double>(cameraNow->worldFromView[3]) - static_cast<double>(cameraPrev->worldFromView[3]);
                const double dy = static_cast<double>(cameraNow->worldFromView[7]) - static_cast<double>(cameraPrev->worldFromView[7]);
                const double dz = static_cast<double>(cameraNow->worldFromView[11]) - static_cast<double>(cameraPrev->worldFromView[11]);
                const double p0 = static_cast<double>(cameraPrev->worldFromView[2]);
                const double p1 = static_cast<double>(cameraPrev->worldFromView[6]);
                const double p2 = static_cast<double>(cameraPrev->worldFromView[10]);
                const double r20 = p0 * cameraNow->worldFromView[0] + p1 * cameraNow->worldFromView[4] + p2 * cameraNow->worldFromView[8];
                const double r21 = p0 * cameraNow->worldFromView[1] + p1 * cameraNow->worldFromView[5] + p2 * cameraNow->worldFromView[9];
                const double r22 = p0 * cameraNow->worldFromView[2] + p1 * cameraNow->worldFromView[6] + p2 * cameraNow->worldFromView[10];
                const double tz = p0 * dx + p1 * dy + p2 * dz;
                if (std::isfinite(r20) && std::isfinite(r21) && std::isfinite(r22) && std::isfinite(tz)) {
                    pcb.depthParams[1] = 1.0f / cameraNow->scaleX;
                    pcb.depthParams[2] = 1.0f / cameraNow->scaleY;
                    pcb.camRelRow2[0] = static_cast<float>(r20);
                    pcb.camRelRow2[1] = static_cast<float>(r21);
                    pcb.camRelRow2[2] = static_cast<float>(r22);
                    pcb.camRelRow2[3] = static_cast<float>(tz);
                    hasRelCamZ = true;
                }
            }
        }

        uint32_t pFlags = 0u;
        if (!performReset && activePrevDepthSrv && depthSrv) pFlags |= 1u;
        if (motionClassSrv) pFlags |= 2u;
        if (!performReset && reactiveMaskSrv) pFlags |= 4u;
        if (motionVectorsSrv) pFlags |= 8u;
        if (depthSrv) pFlags |= 16u;
        if (hasRelCamZ) pFlags |= 32u;
        pcb.flags[0] = pFlags;

        context->UpdateSubresource(prepassCb_.Get(), 0, nullptr, &pcb, 0, 0);

        ID3D11Buffer* cb = prepassCb_.Get();
        ID3D11SamplerState* smp = pointSampler_.Get();
        ID3D11ShaderResourceView* srvs[6] = {
            currentFrameSrv,
            depthSrv,
            activePrevDepthSrv,
            motionVectorsSrv,
            motionClassSrv,
            (!performReset) ? reactiveMaskSrv : nullptr
        };
        ID3D11UnorderedAccessView* uav = prepassUav_[preWriteIdx].Get();

        context->CSSetShader(prepassCs_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &cb);
        context->CSSetSamplers(0, 1, &smp);
        context->CSSetShaderResources(0, 6, srvs);
        context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context->Dispatch((rW + 7u) / 8u, (rH + 7u) / 8u, 1u);

        ID3D11UnorderedAccessView* nullUav = nullptr;
        ID3D11ShaderResourceView* nullSrvs[6]{};
        context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        context->CSSetShaderResources(0, 6, nullSrvs);
    }

    // -------------------------------------------------------------------------
    // Pass 1.5: cs_taa_patch_div (half-res unvectored-motion detector)
    // -------------------------------------------------------------------------
    const bool runPatchDiv = !performReset && patchDivCs_ && patchDivUav_ && patchDivSrv_;
    if (runPatchDiv) {
        TaaPatchDivCB dcb{};
        dcb.renderRes[0] = static_cast<float>(rW);
        dcb.renderRes[1] = static_cast<float>(rH);
        dcb.renderRes[2] = 1.0f / static_cast<float>(rW);
        dcb.renderRes[3] = 1.0f / static_cast<float>(rH);
        dcb.patchRes[0] = static_cast<float>(patchWidth_);
        dcb.patchRes[1] = static_cast<float>(patchHeight_);
        dcb.patchRes[2] = jitterX;
        dcb.patchRes[3] = jitterY;
        dcb.flags[0] = effectiveHdr ? 1.0f : 0.0f;
        context->UpdateSubresource(patchDivCb_.Get(), 0, nullptr, &dcb, 0, 0);

        ID3D11Buffer* cb = patchDivCb_.Get();
        ID3D11SamplerState* smp = linearSampler_.Get();
        ID3D11ShaderResourceView* srvs[3] = {
            currentFrameSrv,
            historySrv_[readIdx].Get(),
            prepassSrv_[preWriteIdx].Get()
        };
        ID3D11UnorderedAccessView* uav = patchDivUav_.Get();

        context->CSSetShader(patchDivCs_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &cb);
        context->CSSetSamplers(0, 1, &smp);
        context->CSSetShaderResources(0, 3, srvs);
        context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context->Dispatch((patchWidth_ + 7u) / 8u, (patchHeight_ + 7u) / 8u, 1u);

        ID3D11UnorderedAccessView* nullUav = nullptr;
        ID3D11ShaderResourceView* nullSrvs[3]{};
        context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        context->CSSetShaderResources(0, 3, nullSrvs);
    }

    // -------------------------------------------------------------------------
    // Pass 2: cs_taa_resolve (outputWidth x outputHeight)
    // -------------------------------------------------------------------------
    {
        TaaResolveCB rcb{};
        rcb.outputRes[0] = static_cast<float>(width);
        rcb.outputRes[1] = static_cast<float>(height);
        rcb.outputRes[2] = 1.0f / static_cast<float>(width);
        rcb.outputRes[3] = 1.0f / static_cast<float>(height);
        rcb.renderRes[0] = static_cast<float>(rW);
        rcb.renderRes[1] = static_cast<float>(rH);
        rcb.renderRes[2] = 1.0f / static_cast<float>(rW);
        rcb.renderRes[3] = 1.0f / static_cast<float>(rH);
        rcb.jitterAndAlpha[0] = jitterX;
        rcb.jitterAndAlpha[1] = jitterY;
        rcb.jitterAndAlpha[2] = minAlpha;
        rcb.jitterAndAlpha[3] = maxAlpha;
        rcb.gammaAndReset[0] = baseGamma;
        rcb.gammaAndReset[1] = lockMaxGamma;
        rcb.gammaAndReset[2] = performReset ? 1.0f : 0.0f;
        uint32_t rFlags = 0u;
        if (motionClassSrv) rFlags |= 1u;
        if (!performReset && reactiveMaskSrv) rFlags |= 2u;
        rcb.gammaAndReset[3] = static_cast<float>(rFlags);
        rcb.extraParams[0] = effectiveHdr ? 1.0f : 0.0f;
        rcb.extraParams[1] = (!performReset && hasPrevPrepass_) ? 1.0f : 0.0f;
        rcb.extraParams[2] = runPatchDiv ? 1.0f : 0.0f;

        context->UpdateSubresource(resolveCb_.Get(), 0, nullptr, &rcb, 0, 0);

        ID3D11Buffer* cb = resolveCb_.Get();
        ID3D11SamplerState* smps[2] = { linearSampler_.Get(), pointSampler_.Get() };
        ID3D11ShaderResourceView* srvs[8] = {
            currentFrameSrv,
            historySrv_[readIdx].Get(),
            historyAuxSrv_[readIdx].Get(),
            prepassSrv_[preWriteIdx].Get(),
            motionClassSrv,
            (!performReset) ? reactiveMaskSrv : nullptr,
            (!performReset && hasPrevPrepass_) ? prepassSrv_[preReadIdx].Get() : nullptr,
            runPatchDiv ? patchDivSrv_.Get() : nullptr
        };
        ID3D11UnorderedAccessView* uavs[2] = {
            historyUav_[writeIdx].Get(),
            historyAuxUav_[writeIdx].Get()
        };

        context->CSSetShader(resolveCs_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &cb);
        context->CSSetSamplers(0, 2, smps);
        context->CSSetShaderResources(0, 8, srvs);
        context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        context->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1u);

        ID3D11UnorderedAccessView* nullUavs[2]{};
        ID3D11ShaderResourceView* nullSrvs[8]{};
        ID3D11SamplerState* nullSmps[2]{};
        ID3D11Buffer* nullCb = nullptr;
        context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
        context->CSSetShaderResources(0, 8, nullSrvs);
        context->CSSetSamplers(0, 2, nullSmps);
        context->CSSetConstantBuffers(0, 1, &nullCb);
        context->CSSetShader(nullptr, nullptr, 0);
    }

    historyReadIdx_ = writeIdx;
    prepassReadIdx_ = preWriteIdx;
    hasPrevPrepass_ = true;
    displayTexture_ = historyTexture_[historyReadIdx_].Get();
    displaySrv_ = historySrv_[historyReadIdx_].Get();

    // Update prevDepthTex_ only when caller did not already supply prevDepthSrv (saves full-screen CopyResource).
    if (prevDepthSrv != nullptr) {
        hasPrevDepthCopy_ = false;
    } else if (depthSrv) {
        Microsoft::WRL::ComPtr<ID3D11Resource> depthRes;
        depthSrv->GetResource(&depthRes);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> depthTex;
        if (depthRes && SUCCEEDED(depthRes.As(&depthTex))) {
            D3D11_TEXTURE2D_DESC dd{};
            depthTex->GetDesc(&dd);
            if ((dd.Format == DXGI_FORMAT_R32_FLOAT || dd.Format == DXGI_FORMAT_R32_TYPELESS) &&
                dd.Width == rW && dd.Height == rH && dd.SampleDesc.Count == 1 &&
                ensurePrevDepthTexture(rW, rH)) {
                context->CopyResource(prevDepthTex_.Get(), depthTex.Get());
                hasPrevDepthCopy_ = true;
            } else {
                hasPrevDepthCopy_ = false;
            }
        }
    } else {
        hasPrevDepthCopy_ = false;
    }

    // -------------------------------------------------------------------------
    // Pass 3 (optional, strictly outside history loop): PostRCAS & DebugView
    // When !zeroCopyPingPong, PostRCAS is folded into ps_taa_blit to save a full-screen pass.
    // -------------------------------------------------------------------------
    const bool runRcas =
        zeroCopyPingPong && (debugView == 0) && (rcasSharpness > 1e-4f) && ensurePostTexture(true);
    if (runRcas) {
        TaaPostRcasCB rcasCb{};
        rcasCb.resolution[0] = static_cast<float>(width);
        rcasCb.resolution[1] = static_cast<float>(height);
        rcasCb.sharpness = rcasSharpness;
        rcasCb.effectiveHdr = effectiveHdr ? 1.0f : 0.0f;
        context->UpdateSubresource(postRcasCb_.Get(), 0, nullptr, &rcasCb, 0, 0);

        ID3D11Buffer* cb = postRcasCb_.Get();
        ID3D11ShaderResourceView* srvs[2] = {
            historySrv_[historyReadIdx_].Get(),
            historyAuxSrv_[historyReadIdx_].Get()
        };
        ID3D11UnorderedAccessView* uav = postUav_.Get();

        context->CSSetShader(postRcasCs_.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &cb);
        context->CSSetShaderResources(0, 2, srvs);
        context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1u);

        ID3D11UnorderedAccessView* nullUav = nullptr;
        ID3D11ShaderResourceView* nullSrvs[2]{};
        ID3D11Buffer* nullCb = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        context->CSSetShaderResources(0, 2, nullSrvs);
        context->CSSetConstantBuffers(0, 1, &nullCb);
        context->CSSetShader(nullptr, nullptr, 0);

        displayTexture_ = postTex_.Get();
        displaySrv_ = postSrv_.Get();
    }

    if (zeroCopyPingPong) {
        if (debugView != 0 && ensurePostTexture(true)) {
            TaaBlitCB bcb{};
            bcb.outputRes[0] = static_cast<float>(width);
            bcb.outputRes[1] = static_cast<float>(height);
            bcb.outputRes[2] = 1.0f / static_cast<float>(width);
            bcb.outputRes[3] = 1.0f / static_cast<float>(height);
            bcb.debugInfo[0] = static_cast<uint32_t>(debugView);
            context->UpdateSubresource(blitCb_.Get(), 0, nullptr, &bcb, 0, 0);

            ID3D11Buffer* cb = blitCb_.Get();
            ID3D11SamplerState* smp = pointSampler_.Get();
            ID3D11ShaderResourceView* srvs[3] = {
                historySrv_[historyReadIdx_].Get(),
                historyAuxSrv_[historyReadIdx_].Get(),
                prepassSrv_[prepassReadIdx_].Get()
            };
            ID3D11UnorderedAccessView* uav = postUav_.Get();

            context->CSSetShader(debugCs_.Get(), nullptr, 0);
            context->CSSetConstantBuffers(0, 1, &cb);
            context->CSSetSamplers(0, 1, &smp);
            context->CSSetShaderResources(0, 3, srvs);
            context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            context->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1u);

            ID3D11UnorderedAccessView* nullUav = nullptr;
            ID3D11ShaderResourceView* nullSrvs[3]{};
            ID3D11SamplerState* nullSmp = nullptr;
            ID3D11Buffer* nullCb = nullptr;
            context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
            context->CSSetShaderResources(0, 3, nullSrvs);
            context->CSSetSamplers(0, 1, &nullSmp);
            context->CSSetConstantBuffers(0, 1, &nullCb);
            context->CSSetShader(nullptr, nullptr, 0);

            displayTexture_ = postTex_.Get();
            displaySrv_ = postSrv_.Get();
        }
    } else {
        // Blit final display image (with optional inline PostRCAS or DebugView) to caller's destinationRtv.
        TaaBlitCB bcb{};
        bcb.outputRes[0] = static_cast<float>(width);
        bcb.outputRes[1] = static_cast<float>(height);
        bcb.outputRes[2] = 1.0f / static_cast<float>(width);
        bcb.outputRes[3] = 1.0f / static_cast<float>(height);
        bcb.debugInfo[0] = static_cast<uint32_t>(debugView);
        bcb.rcasParams[0] = (debugView == 0) ? rcasSharpness : 0.0f;
        bcb.rcasParams[1] = effectiveHdr ? 1.0f : 0.0f;
        context->UpdateSubresource(blitCb_.Get(), 0, nullptr, &bcb, 0, 0);

        const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
        context->RSSetViewports(1, &vp);
        context->RSSetState(nullptr);
        context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(nullptr, 0);
        context->OMSetRenderTargets(1, &targetRtv, nullptr);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(blitVs_.Get(), nullptr, 0);
        context->PSSetShader(blitPs_.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);

        ID3D11Buffer* cb = blitCb_.Get();
        ID3D11SamplerState* smp = pointSampler_.Get();
        ID3D11ShaderResourceView* srvs[3] = {
            displaySrv_,
            historyAuxSrv_[historyReadIdx_].Get(),
            prepassSrv_[prepassReadIdx_].Get()
        };
        context->PSSetConstantBuffers(0, 1, &cb);
        context->PSSetSamplers(0, 1, &smp);
        context->PSSetShaderResources(0, 3, srvs);

        context->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrvs[3]{};
        ID3D11RenderTargetView* nullRtv = nullptr;
        context->PSSetShaderResources(0, 3, nullSrvs);
        context->OMSetRenderTargets(1, &nullRtv, nullptr);
    }

    if (framesSinceReset_ < 1000u) {
        ++framesSinceReset_;
    }

    return true;
}

} // namespace edpe
