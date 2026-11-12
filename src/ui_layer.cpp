#include "ui_layer.h"
#include "context_census.h"

#include <d3dcompiler.h>

namespace edpe {

namespace {

inline constexpr char kUiLayerCompositeCsHlsl[] = R"hlsl(
Texture2D<float4>   tWorld : register(t0); // Апскейлений 3D світ (після DLSS/FSR)
Texture2D<float4>   tUi    : register(t1); // RGB = колір UI, A = Transmittance T
Texture2D<float4>   tMult  : register(t2); // Шар затемнення (vignette / drop shadow)
RWTexture2D<float4> uOut   : register(u0); // Вихідний буфер (SwapChain Backbuffer)

cbuffer CompositeParams : register(b0) {
    uint2 outputSize;
    uint  hasMultiply;
    uint  debugView;
};

[numthreads(32, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outputSize.x || id.y >= outputSize.y) return;
    int3 p = int3(id.xy, 0);

    float4 ui = tUi.Load(p);
    float3 mult = (hasMultiply != 0) ? tMult.Load(p).rgb : float3(1.0, 1.0, 1.0);

    // Якщо піксель не містить емісійного UI, зберігаємо світ без змін:
    if (debugView == 0 && all(ui.rgb <= 1e-4) && all(mult >= 0.999)) {
        uOut[id.xy] = tWorld.Load(p);
        return;
    }

    float4 world = tWorld.Load(p);

    if (debugView != 0) {
        // Режим діагностики: UI поверх темно-синього фону
        float3 dbg = ui.rgb + float3(0.0, 0.05, 0.2) * (1.0 - ui.a);
        uOut[id.xy] = float4(saturate(dbg), 1.0);
        return;
    }

    // Точна комбінація кольорів:
    // ui.rgb - це premultiplied емісійне світло інтерфейсу
    // ui.a   - це коефіцієнт пропускання фону T = prod(1 - alpha)
    // mult   - це мультиплікативне затемнення меню/тіней
    // Захист від затемнення фону підкладками без емісії:
    float effectiveTransmittance = all(ui.rgb <= 1e-4) ? 1.0 : ui.a;
    float3 finalRgb = ui.rgb + world.rgb * effectiveTransmittance * mult;
    uOut[id.xy] = float4(saturate(finalRgb), world.a);
}
)hlsl";

inline constexpr char kUiLayerCoverageHlsl[] = R"hlsl(
Texture2D<float4> tHdrLayer : register(t0);

struct VSOut {
    float4 pos : SV_Position;
};

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 ps_main(VSOut pin) : SV_Target {
    return float4(0.0, 0.0, 0.0, tHdrLayer.Load(int3(pin.pos.xy, 0)).a);
}
)hlsl";

} // namespace

UiLayer& UiLayer::instance() {
    static UiLayer s_instance;
    return s_instance;
}

bool UiLayer::initialize(ID3D11Device* device) {
    if (!device) return false;
    if (device_.Get() == device && compositeCs_ && constantBuffer_) {
        return true;
    }
    if (device_ && device_.Get() != device) {
        shutdown();
    }
    device_ = device;

    // Компіляція обчислювального шейдера композитингу
    Microsoft::WRL::ComPtr<ID3DBlob> csBlob;
    if (FAILED(D3DCompileFromFile(L"src/shaders/ui_layer_composite_cs.hlsl", nullptr, nullptr,
            "cs_main", "cs_5_0", 0, 0, &csBlob, nullptr))) {
        if (FAILED(D3DCompile(kUiLayerCompositeCsHlsl, sizeof(kUiLayerCompositeCsHlsl) - 1,
                nullptr, nullptr, nullptr, "cs_main", "cs_5_0", 0, 0, &csBlob, nullptr))) {
            return false;
        }
    }
    if (FAILED(device_->CreateComputeShader(
            csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &compositeCs_))) {
        return false;
    }

    // Компіляція шейдерів переносу альфи (HdrLayer.a -> LdrLayer.a)
    Microsoft::WRL::ComPtr<ID3DBlob> covVsBlob, covPsBlob;
    if (FAILED(D3DCompile(kUiLayerCoverageHlsl, sizeof(kUiLayerCoverageHlsl) - 1,
            nullptr, nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &covVsBlob, nullptr)) ||
        FAILED(D3DCompile(kUiLayerCoverageHlsl, sizeof(kUiLayerCoverageHlsl) - 1,
            nullptr, nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &covPsBlob, nullptr)) ||
        FAILED(device_->CreateVertexShader(
            covVsBlob->GetBufferPointer(), covVsBlob->GetBufferSize(), nullptr, &coverageVs_)) ||
        FAILED(device_->CreatePixelShader(
            covPsBlob->GetBufferPointer(), covPsBlob->GetBufferSize(), nullptr, &coveragePs_))) {
        return false;
    }

    // Створення константного буфера композитингу
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(uint32_t) * 4;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&cbd, nullptr, &constantBuffer_))) {
        return false;
    }

    // Blend-state для Tonemap Re-issue: запис тільки RGB, альфа не змінюється
    {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        if (FAILED(device_->CreateBlendState(&bd, &toneRgbOnlyBlend_))) {
            return false;
        }
    }

    // Blend-state для переносу альфи (transmittance): запис тільки A
    {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
        if (FAILED(device_->CreateBlendState(&bd, &coverageAlphaOnlyBlend_))) {
            return false;
        }
    }

    // Blend-state для перехоплення UI у позаекранні шари:
    {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device_->CreateBlendState(&bd, &alphaBlend_);
    }
    {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device_->CreateBlendState(&bd, &premulBlend_);
    }
    {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device_->CreateBlendState(&bd, &addBlend_);
    }
    {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device_->CreateBlendState(&bd, &scaledAddBlend_);
    }
    {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device_->CreateBlendState(&bd, &opaqueBlend_);
    }
    {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device_->CreateBlendState(&bd, &darkenBlend_);
    }
    {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        device_->CreateBlendState(&bd, &multBlend_);
    }
    {
        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable = FALSE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        dsd.StencilEnable = FALSE;
        device_->CreateDepthStencilState(&dsd, &noDepthState_);
    }

    return true;
}

void UiLayer::shutdown() {
    savedRtv_.Reset();
    savedDsv_.Reset();
    savedBlend_.Reset();
    savedDsState_.Reset();
    savedToneRtv_.Reset();
    savedToneDsv_.Reset();
    savedToneLutSrv_.Reset();
    savedToneHdrSrv_.Reset();
    savedToneExposureCb_.Reset();
    savedToneBlend_.Reset();

    hdrLayerTex_.Reset();
    hdrLayerRtv_.Reset();
    hdrLayerSrv_.Reset();
    ldrLayerTex_.Reset();
    ldrLayerRtv_.Reset();
    ldrLayerSrv_.Reset();
    multLayerTex_.Reset();
    multLayerRtv_.Reset();
    multLayerSrv_.Reset();

    toneRgbOnlyBlend_.Reset();
    coverageAlphaOnlyBlend_.Reset();
    alphaBlend_.Reset();
    premulBlend_.Reset();
    addBlend_.Reset();
    scaledAddBlend_.Reset();
    opaqueBlend_.Reset();
    darkenBlend_.Reset();
    multBlend_.Reset();
    noDepthState_.Reset();
    coverageVs_.Reset();
    coveragePs_.Reset();
    constantBuffer_.Reset();
    compositeCs_.Reset();
    device_.Reset();

    isIntercepting_ = false;
    isTonemapReissuing_ = false;
    hasMultiplyDraws_ = false;
    width_ = 0;
    height_ = 0;
}

void UiLayer::resize(ID3D11Device* device, UINT width, UINT height) {
    if (!device || width == 0 || height == 0) return;
    if (!compositeCs_ || device_.Get() != device) {
        initialize(device);
    }
    if (width == width_ && height == height_ && hdrLayerTex_ && ldrLayerTex_ && multLayerTex_) return;
    width_ = width;
    height_ = height;

    hdrLayerTex_.Reset(); hdrLayerRtv_.Reset(); hdrLayerSrv_.Reset();
    ldrLayerTex_.Reset(); ldrLayerRtv_.Reset(); ldrLayerSrv_.Reset();
    multLayerTex_.Reset(); multLayerRtv_.Reset(); multLayerSrv_.Reset();

    // 1. Створюємо HDR шар для Pass 35 Cockpit HUD
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    device->CreateTexture2D(&desc, nullptr, &hdrLayerTex_);
    device->CreateRenderTargetView(hdrLayerTex_.Get(), nullptr, &hdrLayerRtv_);
    device->CreateShaderResourceView(hdrLayerTex_.Get(), nullptr, &hdrLayerSrv_);

    // 2. Створюємо LDR шар для 2D Меню (Pass 54) та результату тонемапу
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateTexture2D(&desc, nullptr, &ldrLayerTex_);
    device->CreateRenderTargetView(ldrLayerTex_.Get(), nullptr, &ldrLayerRtv_);
    device->CreateShaderResourceView(ldrLayerTex_.Get(), nullptr, &ldrLayerSrv_);

    // 3. Створюємо Mult шар для затемнення бекдропів
    device->CreateTexture2D(&desc, nullptr, &multLayerTex_);
    device->CreateRenderTargetView(multLayerTex_.Get(), nullptr, &multLayerRtv_);
    device->CreateShaderResourceView(multLayerTex_.Get(), nullptr, &multLayerSrv_);
}

void UiLayer::beginFrame(ID3D11DeviceContext* context) {
    if (!context || !hdrLayerRtv_ || !ldrLayerRtv_ || !multLayerRtv_) return;
    EvaluatingUpscalerGuard guard;
    hasMultiplyDraws_ = false;

    // Очищення: колір чорний (0,0,0), альфа (пропускання) = 1.0
    const float clearTransparent[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float clearMult[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    context->ClearRenderTargetView(hdrLayerRtv_.Get(), clearTransparent);
    context->ClearRenderTargetView(ldrLayerRtv_.Get(), clearTransparent);
    context->ClearRenderTargetView(multLayerRtv_.Get(), clearMult);
}

bool UiLayer::shouldInterceptDraw(uint64_t vsHash, uint64_t psHash, bool isHdrTarget) {
    if (!isHdrTarget) {
        // Pass 54 2D UI (Main Menu, ESC Menu, cursor) draws to backbuffer at native resolution.
        // Never intercept LDR draws; let Cobra render them straight to backbuffer.
        return false;
    }

    if (vsHash == 0x81216C77F90DEDD6ull ||
        vsHash == 0x1989E6D3B405FDE0ull ||
        vsHash == 0x925ACEDA0153AA5Bull ||
        vsHash == 0xB7790CBFC6554097ull ||
        vsHash == 0xE508648660A352B2ull ||
        vsHash == 0x5DA53D8B0133341Eull ||
        vsHash == 0xDF3503CD07F9B10Cull ||
        vsHash == 0x5453D19B6D362364ull ||
        vsHash == 0x44DDCDEBAEA137D8ull ||
        vsHash == 0xA2C2D5510BF1926Dull ||
        vsHash == 0x9B34C331902DC1EDull ||
        vsHash == 0x9611A454527F7FEBull ||
        vsHash == 0xB932058F26B76691ull) {
        return true;
    }
    return (vsHash == 0x2CECEC3065EF0D4Aull && psHash == 0xA4D03619D631B186ull) || // Stencil setup only (0x04)
           (vsHash == 0x5559BD94B6852E83ull &&
            (psHash == 0xEA02FAC2BD6C643Cull || psHash == 0xE95634B0F61D218Full)) ||
           (vsHash == 0xB12F7A618E1BDE98ull &&
            (psHash == 0x203DF51758AADC4Dull || psHash == 0x42AC0CACC9CDF72Bull)) ||
           (vsHash == 0xF8D8A92E96419901ull && psHash == 0x16196F69ADE35E77ull) ||
           (vsHash == 0x1989B75BF034ECC3ull && psHash == 0xDB79AE788E049DFDull) ||
           (vsHash == 0xC7FA0C0F5DD49180ull && psHash == 0x6EEF165A350DA30Full) ||
           (vsHash == 0xA47A3315FFF5E2E4ull && psHash == 0x869FFF43E875906Eull) ||
           (vsHash == 0x9BFC7FD232328391ull && psHash == 0xDBF1725726018F52ull) ||
           (vsHash == 0xE904D334BC8B11EAull && psHash == 0x095030F27D2C362Aull) ||
           (vsHash == 0x1012E00B3CB44469ull && psHash == 0x666EF0C4C616F67Eull) ||
           (vsHash == 0xA3E5D3FCBC1165F8ull && psHash == 0x019E81F4EECFB371ull) ||
           (vsHash == 0x1E8D013722FE71B8ull && psHash == 0x3D62E1CE0B0ECDF6ull) ||
           (vsHash == 0x12382D2EA45E9632ull && psHash == 0x855C469156AB997Full) ||
           (vsHash == 0xC31238331D8AC3D4ull &&
            (psHash == 0xBD0FEB3276C8B2D7ull || psHash == 0x539A4858CE3A3477ull || psHash == 0x964221955A9E77E7ull));
}

void UiLayer::beginIntercept(ID3D11DeviceContext* context, float jitterX, float jitterY,
                             UINT renderW, UINT renderH, bool isHdr) {
    if (!context || isIntercepting_ || width_ == 0 || height_ == 0) return;
    if (renderW == 0) renderW = width_;
    if (renderH == 0) renderH = height_;
    EvaluatingUpscalerGuard guard;
    isIntercepting_ = true;

    // Зберігаємо поточний стан в'юпорту та RTV гри
    savedViewportCount_ = 1;
    context->RSGetViewports(&savedViewportCount_, &savedViewport_);
    context->OMGetRenderTargets(1, savedRtv_.GetAddressOf(), savedDsv_.GetAddressOf());
    context->OMGetBlendState(savedBlend_.GetAddressOf(), savedBlendFactor_, &savedSampleMask_);
    context->OMGetDepthStencilState(savedDsState_.GetAddressOf(), &savedStencilRef_);

    // Перераховуємо в'юпорт: масштабуємо на екран і ВІДНІМАЄМО джиттер камери
    D3D11_VIEWPORT layerVp{};
    layerVp.TopLeftX = (savedViewport_.TopLeftX - jitterX) * (static_cast<float>(width_) / renderW);
    layerVp.TopLeftY = (savedViewport_.TopLeftY - jitterY) * (static_cast<float>(height_) / renderH);
    layerVp.Width    = savedViewport_.Width * (static_cast<float>(width_) / renderW);
    layerVp.Height   = savedViewport_.Height * (static_cast<float>(height_) / renderH);
    layerVp.MinDepth = savedViewport_.MinDepth;
    layerVp.MaxDepth = savedViewport_.MaxDepth;
    context->RSSetViewports(1, &layerVp);

    ID3D11RenderTargetView* targetRtv = isHdr ? hdrLayerRtv_.Get() : ldrLayerRtv_.Get();
    ID3D11BlendState* layerBlend = alphaBlend_.Get();
    if (!savedBlend_) {
        layerBlend = opaqueBlend_.Get();
    } else {
        D3D11_BLEND_DESC bd{};
        savedBlend_->GetDesc(&bd);
        const auto& rt0 = bd.RenderTarget[0];
        if (!rt0.BlendEnable) {
            layerBlend = opaqueBlend_.Get();
        } else if (!isHdr && rt0.DestBlend == D3D11_BLEND_SRC_COLOR && multLayerRtv_) {
            targetRtv = multLayerRtv_.Get();
            layerBlend = multBlend_.Get();
            hasMultiplyDraws_ = true;
        } else if (rt0.SrcBlend == D3D11_BLEND_ZERO) {
            layerBlend = darkenBlend_.Get();
        } else if (rt0.SrcBlend == D3D11_BLEND_ONE && rt0.DestBlend == D3D11_BLEND_INV_SRC_ALPHA) {
            layerBlend = premulBlend_.Get();
        } else if ((rt0.SrcBlend == D3D11_BLEND_SRC_ALPHA || rt0.SrcBlend == D3D11_BLEND_SRC_ALPHA_SAT) &&
                   rt0.DestBlend == D3D11_BLEND_ONE) {
            layerBlend = scaledAddBlend_.Get();
        } else if (rt0.DestBlend == D3D11_BLEND_ONE) {
            layerBlend = addBlend_.Get();
        } else {
            layerBlend = alphaBlend_.Get();
        }
    }

    ID3D11DepthStencilView* dsvToBind = nullptr;
    if (savedDsv_ && renderW == width_ && renderH == height_) {
        dsvToBind = savedDsv_.Get();
    } else if (noDepthState_) {
        context->OMSetDepthStencilState(noDepthState_.Get(), 0);
    }

    const FLOAT blendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (layerBlend) {
        context->OMSetBlendState(layerBlend, blendFactor, savedSampleMask_);
    }

    // Підставляємо відповідний RTV нашого шару:
    context->OMSetRenderTargets(1, &targetRtv, dsvToBind);
}

void UiLayer::endIntercept(ID3D11DeviceContext* context) {
    if (!context || !isIntercepting_) return;
    EvaluatingUpscalerGuard guard;
    isIntercepting_ = false;

    // Відновлюємо оригінальні RTV, blend-state та в'юпорт гри
    context->RSSetViewports(savedViewportCount_, &savedViewport_);
    context->OMSetBlendState(savedBlend_.Get(), savedBlendFactor_, savedSampleMask_);
    context->OMSetDepthStencilState(savedDsState_.Get(), savedStencilRef_);
    context->OMSetRenderTargets(1, savedRtv_.GetAddressOf(), savedDsv_.Get());
    savedRtv_.Reset();
    savedDsv_.Reset();
    savedBlend_.Reset();
    savedDsState_.Reset();
}

bool UiLayer::beginTonemapReissue(ID3D11DeviceContext* context,
                                  ID3D11ShaderResourceView* gameLut,
                                  ID3D11Buffer* gameExposureCb) {
    if (!context || !hdrLayerSrv_ || !ldrLayerRtv_ || !toneRgbOnlyBlend_ || isTonemapReissuing_) {
        return false;
    }
    EvaluatingUpscalerGuard guard;
    isTonemapReissuing_ = true;

    // Зберігаємо оригінальний стан шейдерних ресурсів, RTV та в'юпорту гри
    savedToneViewportCount_ = 1;
    context->RSGetViewports(&savedToneViewportCount_, &savedToneViewport_);
    context->OMGetRenderTargets(1, savedToneRtv_.GetAddressOf(), savedToneDsv_.GetAddressOf());
    context->PSGetShaderResources(0, 1, savedToneLutSrv_.GetAddressOf());
    context->PSGetShaderResources(1, 1, savedToneHdrSrv_.GetAddressOf());
    context->PSGetConstantBuffers(2, 1, savedToneExposureCb_.GetAddressOf());
    context->OMGetBlendState(savedToneBlend_.GetAddressOf(), savedToneBlendFactor_, &savedToneSampleMask_);

    if (gameLut) {
        context->PSSetShaderResources(0, 1, &gameLut);
    }
    if (gameExposureCb) {
        context->PSSetConstantBuffers(2, 1, &gameExposureCb);
    }

    // Підставляємо HdrLayer як вхідний HDR буфер на слот t1
    ID3D11ShaderResourceView* hudHdrSrv = hdrLayerSrv_.Get();
    context->PSSetShaderResources(1, 1, &hudHdrSrv);

    // Спрямовуємо вивід у LdrLayer (RGB канали)
    ID3D11RenderTargetView* uiLdrRtv = ldrLayerRtv_.Get();
    context->OMSetRenderTargets(1, &uiLdrRtv, nullptr);

    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    context->RSSetViewports(1, &vp);

    const FLOAT blendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    context->OMSetBlendState(toneRgbOnlyBlend_.Get(), blendFactor, 0xFFFFFFFF);
    return true;
}

void UiLayer::endTonemapReissue(ID3D11DeviceContext* context) {
    if (!context || !isTonemapReissuing_) return;
    EvaluatingUpscalerGuard guard;
    isTonemapReissuing_ = false;

    // Переносимо альфу (transmittance) з HdrLayer.a в LdrLayer.a
    if (coverageVs_ && coveragePs_ && coverageAlphaOnlyBlend_ && hdrLayerSrv_ && ldrLayerRtv_) {
        Microsoft::WRL::ComPtr<ID3D11VertexShader> origVs;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> origPs;
        Microsoft::WRL::ComPtr<ID3D11GeometryShader> origGs;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> origLayout;
        D3D11_PRIMITIVE_TOPOLOGY origTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        context->VSGetShader(origVs.GetAddressOf(), nullptr, nullptr);
        context->PSGetShader(origPs.GetAddressOf(), nullptr, nullptr);
        context->GSGetShader(origGs.GetAddressOf(), nullptr, nullptr);
        context->IAGetInputLayout(origLayout.GetAddressOf());
        context->IAGetPrimitiveTopology(&origTopo);

        ID3D11ShaderResourceView* nullSrv = nullptr;
        context->PSSetShaderResources(1, 1, &nullSrv);

        ID3D11RenderTargetView* uiLdrRtv = ldrLayerRtv_.Get();
        context->OMSetRenderTargets(1, &uiLdrRtv, nullptr);
        const FLOAT blendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        context->OMSetBlendState(coverageAlphaOnlyBlend_.Get(), blendFactor, 0xFFFFFFFF);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(coverageVs_.Get(), nullptr, 0);
        context->PSSetShader(coveragePs_.Get(), nullptr, 0);
        context->GSSetShader(nullptr, nullptr, 0);
        ID3D11ShaderResourceView* hdrSrv = hdrLayerSrv_.Get();
        context->PSSetShaderResources(0, 1, &hdrSrv);

        context->Draw(3, 0);

        context->PSSetShaderResources(0, 1, &nullSrv);
        context->VSSetShader(origVs.Get(), nullptr, 0);
        context->PSSetShader(origPs.Get(), nullptr, 0);
        context->GSSetShader(origGs.Get(), nullptr, 0);
        context->IASetInputLayout(origLayout.Get());
        context->IASetPrimitiveTopology(origTopo);
    }

    // Відновлюємо оригінальний стан шейдерних ресурсів та RTV гри
    ID3D11ShaderResourceView* origLutSrv = savedToneLutSrv_.Get();
    ID3D11ShaderResourceView* origHdrSrv = savedToneHdrSrv_.Get();
    context->PSSetShaderResources(0, 1, &origLutSrv);
    context->PSSetShaderResources(1, 1, &origHdrSrv);
    ID3D11Buffer* origExposureCb = savedToneExposureCb_.Get();
    context->PSSetConstantBuffers(2, 1, &origExposureCb);
    context->OMSetBlendState(savedToneBlend_.Get(), savedToneBlendFactor_, savedToneSampleMask_);
    context->RSSetViewports(savedToneViewportCount_, &savedToneViewport_);
    context->OMSetRenderTargets(1, savedToneRtv_.GetAddressOf(), savedToneDsv_.Get());

    savedToneRtv_.Reset();
    savedToneDsv_.Reset();
    savedToneLutSrv_.Reset();
    savedToneHdrSrv_.Reset();
    savedToneExposureCb_.Reset();
    savedToneBlend_.Reset();
}

bool UiLayer::composite(ID3D11DeviceContext* context,
                        ID3D11ShaderResourceView* worldReconstruction,
                        ID3D11UnorderedAccessView* backbufferUav,
                        UINT outputWidth, UINT outputHeight,
                        bool debugView) {
    if (!context || !worldReconstruction || !backbufferUav || !compositeCs_) return false;
    EvaluatingUpscalerGuard guard;

    // Оновлюємо константи
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        auto* data = reinterpret_cast<uint32_t*>(mapped.pData);
        data[0] = outputWidth;
        data[1] = outputHeight;
        data[2] = hasMultiplyDraws_ ? 1 : 0;
        data[3] = debugView ? 1 : 0;
        context->Unmap(constantBuffer_.Get(), 0);
    }

    // Біндимо ресурси
    ID3D11ShaderResourceView* srvs[3] = {worldReconstruction, ldrLayerSrv_.Get(), multLayerSrv_.Get()};
    context->CSSetShaderResources(0, 3, srvs);
    context->CSSetUnorderedAccessViews(0, 1, &backbufferUav, nullptr);
    context->CSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());
    context->CSSetShader(compositeCs_.Get(), nullptr, 0);

    // Запуск обчислювальної сітки 32x8
    const UINT groupsX = (outputWidth + 31) / 32;
    const UINT groupsY = (outputHeight + 7) / 8;
    context->Dispatch(groupsX, groupsY, 1);

    // Розбіндинг
    ID3D11ShaderResourceView* nullSrvs[3] = {nullptr, nullptr, nullptr};
    ID3D11UnorderedAccessView* nullUav = nullptr;
    context->CSSetShaderResources(0, 3, nullSrvs);
    context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    return true;
}

} // namespace edpe
