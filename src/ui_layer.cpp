#include "ui_layer.h"
#include "context_census.h"
#include "dxbc_fanout.h"
#include "shader_probe.h"

#include <algorithm>
#include <cmath>
#include <d3dcompiler.h>
#include <vector>

namespace edpe {

namespace {

inline constexpr char kUiLayerCompositeCsHlsl[] = R"hlsl(
Texture2D<float4>   tWorld : register(t0); // Апскейлений 3D світ (після DLSS/FSR/TAA)
Texture2D<float4>   tUi    : register(t1); // Тонемаплений колір UI (LdrLayer.rgb)
Texture2D<float4>   tMult  : register(t2); // Шар мультиплікативного затемнення (vignette / drop shadow)
Texture2D<float4>   tHdrUi : register(t3); // Лінійний HDR-шар UI (RGB = емісія, A = Transmittance T = prod(1 - alpha))
RWTexture2D<float4> uOut   : register(u0); // Вихідний буфер

cbuffer CompositeParams : register(b0) {
    uint2 outputSize;
    uint  hasMultiply;
    uint  debugView;
};

float3 srgbToLinear(float3 c) {
    c = max(c, 0.0);
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

float3 linearToSrgb(float3 c) {
    c = max(c, 0.0);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return lerp(hi, lo, step(c, 0.0031308));
}

[numthreads(32, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outputSize.x || id.y >= outputSize.y) return;
    int3 p = int3(id.xy, 0);

    float4 hdrUi = tHdrUi.Load(p);
    float transmittance = saturate(hdrUi.a);
    float3 mult = (hasMultiply != 0) ? tMult.Load(p).rgb : float3(1.0, 1.0, 1.0);

    // Якщо піксель не містить ні емісійного UI, ні напівпрозорої підкладки (transmittance == 1),
    // миттєво повертаємо піксель 3D-світу без читання tUi та без перетворень:
    if (debugView == 0 && all(hdrUi.rgb <= 1e-5) && transmittance >= 0.999 && all(mult >= 0.999)) {
        uOut[id.xy] = tWorld.Load(p);
        return;
    }

    float4 ui = tUi.Load(p);
    float4 corner0 = tUi.Load(int3(0, 0, 0));
    float4 hdrCorner0 = tHdrUi.Load(int3(0, 0, 0));
    float3 blackFloor = (hdrCorner0.a >= 0.999 && all(hdrCorner0.rgb <= 1e-5) && all(corner0.rgb < 0.08))
        ? corner0.rgb : float3(0.0, 0.0, 0.0);
    float3 uiRgb = max(ui.rgb - blackFloor * transmittance, 0.0);

    if (debugView != 0) {
        // Режим діагностики: UI поверх темно-синього фону
        float3 dbg = uiRgb + float3(0.0, 0.05, 0.2) * (1.0 - transmittance);
        uOut[id.xy] = float4(saturate(dbg), 1.0);
        return;
    }

    float4 world = tWorld.Load(p);
    float3 cleanMult = all(mult >= 0.999) ? float3(1.0, 1.0, 1.0) : mult;

    // Оскільки коефіцієнт пропускання T = prod(1 - alpha) накопичується у лінійному
    // HDR-просторі до тонемапінгу (Pass 35), ослаблення фону виконується у лінійному
    // світлі перед поверненням у гамма-простір sRGB:
    float3 worldLin = srgbToLinear(world.rgb);
    float3 attenWorld = linearToSrgb(worldLin * (transmittance * cleanMult));
    float3 finalRgb = uiRgb + attenWorld;
    uOut[id.xy] = float4(saturate(finalRgb), world.a);
}
)hlsl";

inline constexpr char kUiLayerCompositePsHlsl[] = R"hlsl(
Texture2D<float4> tWorld : register(t0);
Texture2D<float4> tUi    : register(t1);
Texture2D<float4> tMult  : register(t2);
Texture2D<float4> tHdrUi : register(t3);

cbuffer CompositeParams : register(b0) {
    uint2 outputSize;
    uint  hasMultiply;
    uint  debugView;
};

struct VSOut {
    float4 pos : SV_Position;
};

VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float3 srgbToLinear(float3 c) {
    c = max(c, 0.0);
    float3 lo = c / 12.92;
    float3 hi = pow((c + 0.055) / 1.055, 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

float3 linearToSrgb(float3 c) {
    c = max(c, 0.0);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return lerp(hi, lo, step(c, 0.0031308));
}

float4 ps_main(VSOut pin) : SV_Target {
    int3 p = int3(pin.pos.xy, 0);

    float4 hdrUi = tHdrUi.Load(p);
    float transmittance = saturate(hdrUi.a);
    float3 mult = (hasMultiply != 0) ? tMult.Load(p).rgb : float3(1.0, 1.0, 1.0);

    if (debugView == 0 && all(hdrUi.rgb <= 1e-5) && transmittance >= 0.999 && all(mult >= 0.999)) {
        return tWorld.Load(p);
    }

    float4 ui = tUi.Load(p);
    float4 corner0 = tUi.Load(int3(0, 0, 0));
    float4 hdrCorner0 = tHdrUi.Load(int3(0, 0, 0));
    float3 blackFloor = (hdrCorner0.a >= 0.999 && all(hdrCorner0.rgb <= 1e-5) && all(corner0.rgb < 0.08))
        ? corner0.rgb : float3(0.0, 0.0, 0.0);
    float3 uiRgb = max(ui.rgb - blackFloor * transmittance, 0.0);

    if (debugView != 0) {
        float3 dbg = uiRgb + float3(0.0, 0.05, 0.2) * (1.0 - transmittance);
        return float4(saturate(dbg), 1.0);
    }

    float4 world = tWorld.Load(p);
    float3 cleanMult = all(mult >= 0.999) ? float3(1.0, 1.0, 1.0) : mult;

    float3 worldLin = srgbToLinear(world.rgb);
    float3 attenWorld = linearToSrgb(worldLin * (transmittance * cleanMult));
    float3 finalRgb = uiRgb + attenWorld;
    return float4(saturate(finalRgb), world.a);
}
)hlsl";

inline constexpr char kSupercruiseBarsGsHlsl[] = R"hlsl(
cbuffer StripParams : register(b0) {
    float2 viewport;
    float  halfWidth;
    float  clipW;
};
struct Vertex {
    float4 colour : TEXCOORD1;
    float4 pos    : SV_Position;
};
static const int kPieces = 8;
[maxvertexcount(36)]
void gs_main(line Vertex v[2], inout TriangleStream<Vertex> stream) {
    float4 p0 = v[0].pos, p1 = v[1].pos;
    float4 c0 = v[0].colour, c1 = v[1].colour;
    const bool in0 = p0.w >= clipW;
    const bool in1 = p1.w >= clipW;
    if (!in0 && !in1) return;
    if (!in0) {
        const float t = (clipW - p0.w) / (p1.w - p0.w);
        p0 = lerp(p0, p1, t);
        c0 = lerp(c0, c1, t);
    } else if (!in1) {
        const float t = (clipW - p1.w) / (p0.w - p1.w);
        p1 = lerp(p1, p0, t);
        c1 = lerp(c1, c0, t);
    }
    const float2 pixelsPerNdc = 0.5 * viewport;
    const float2 n0 = p0.xy / p0.w;
    const float2 n1 = p1.xy / p1.w;
    const float2 d = (n1 - n0) * pixelsPerNdc;
    const float len = length(d);
    if (!(len > 0.001)) return;
    const float2 side = float2(-d.y, d.x) / len * halfWidth / pixelsPerNdc;
    float2 centre[kPieces + 1];
    float4 colour[kPieces + 1];
    [unroll] for (int j = 0; j <= kPieces; ++j) {
        const float u = (float)j / (float)kPieces;
        const float wa = (1.0 - u) / p0.w;
        const float wb = u / p1.w;
        centre[j] = lerp(n0, n1, u);
        colour[j] = (wa * c0 + wb * c1) / (wa + wb);
    }
    Vertex o;
    o.pos.zw = float2(0.0, 1.0);
    [unroll] for (int a = 0; a <= kPieces; ++a) {
        o.colour = float4(colour[a].rgb, 0.0); o.pos.xy = centre[a] - side; stream.Append(o);
        o.colour = colour[a];                  o.pos.xy = centre[a];        stream.Append(o);
    }
    stream.RestartStrip();
    [unroll] for (int b = 0; b <= kPieces; ++b) {
        o.colour = colour[b];                  o.pos.xy = centre[b];        stream.Append(o);
        o.colour = float4(colour[b].rgb, 0.0); o.pos.xy = centre[b] + side; stream.Append(o);
    }
}
)hlsl";

} // namespace

UiLayer& UiLayer::instance() {
    static UiLayer s_instance;
    return s_instance;
}

bool UiLayer::initialize(ID3D11Device* device) {
    if (!device) return false;
    if (device_.Get() == device && compositeCs_ && compositePs_ && coverageVs_ && constantBuffer_) {
        return true;
    }
    if (device_ && device_.Get() != device) {
        shutdown();
    }
    device_ = device;

    // Компіляція вбудованого обчислювального шейдера композитингу
    Microsoft::WRL::ComPtr<ID3DBlob> csBlob;
    if (FAILED(D3DCompile(kUiLayerCompositeCsHlsl, sizeof(kUiLayerCompositeCsHlsl) - 1,
            nullptr, nullptr, nullptr, "cs_main", "cs_5_0", 0, 0, &csBlob, nullptr))) {
        return false;
    }
    if (FAILED(device_->CreateComputeShader(
            csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &compositeCs_))) {
        return false;
    }

    // Компіляція вершинного та піксельного шейдерів прямого RTV-композитингу
    Microsoft::WRL::ComPtr<ID3DBlob> compVsBlob, compPsBlob;
    if (FAILED(D3DCompile(kUiLayerCompositePsHlsl, sizeof(kUiLayerCompositePsHlsl) - 1,
            nullptr, nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &compVsBlob, nullptr)) ||
        FAILED(D3DCompile(kUiLayerCompositePsHlsl, sizeof(kUiLayerCompositePsHlsl) - 1,
            nullptr, nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &compPsBlob, nullptr)) ||
        FAILED(device_->CreateVertexShader(
            compVsBlob->GetBufferPointer(), compVsBlob->GetBufferSize(), nullptr, &coverageVs_)) ||
        FAILED(device_->CreatePixelShader(
            compPsBlob->GetBufferPointer(), compPsBlob->GetBufferSize(), nullptr, &compositePs_))) {
        return false;
    }

    // Компіляція Geometry Shader для смуг суперкруїзу (A47A3315 / 869FFF43)
    Microsoft::WRL::ComPtr<ID3DBlob> barsGsBlob;
    if (SUCCEEDED(D3DCompile(kSupercruiseBarsGsHlsl, sizeof(kSupercruiseBarsGsHlsl) - 1,
            nullptr, nullptr, nullptr, "gs_main", "gs_5_0", 0, 0, &barsGsBlob, nullptr))) {
        device_->CreateGeometryShader(
            barsGsBlob->GetBufferPointer(), barsGsBlob->GetBufferSize(), nullptr, &supercruiseBarsGs_);
    }
    {
        D3D11_BUFFER_DESC gsCbd{};
        gsCbd.ByteWidth = 16;
        gsCbd.Usage = D3D11_USAGE_DEFAULT;
        gsCbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        device_->CreateBuffer(&gsCbd, nullptr, &supercruiseBarsCb_);

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        rd.ScissorEnable = FALSE;
        device_->CreateRasterizerState(&rd, &barsRasterNoScissor_);
        rd.ScissorEnable = TRUE;
        device_->CreateRasterizerState(&rd, &barsRasterScissor_);
    }

    // Константний буфер для ремапінгу 3D-голограм (PS b13, 32 байти)
    {
        D3D11_BUFFER_DESC hbd{};
        hbd.ByteWidth = sizeof(HoloRemapParams);
        hbd.Usage = D3D11_USAGE_DEFAULT;
        hbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        device_->CreateBuffer(&hbd, nullptr, &holoRemapCb_);
    }

    // Чорна 1x1 текстура для обнулення Bloom під час Tonemap Re-issue
    {
        D3D11_TEXTURE2D_DESC zDesc{};
        zDesc.Width = 1;
        zDesc.Height = 1;
        zDesc.MipLevels = 1;
        zDesc.ArraySize = 1;
        zDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        zDesc.SampleDesc.Count = 1;
        zDesc.Usage = D3D11_USAGE_IMMUTABLE;
        zDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const uint16_t zeroHalf[4] = {0, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA zInit{zeroHalf, sizeof(zeroHalf), 0};
        if (SUCCEEDED(device_->CreateTexture2D(&zDesc, &zInit, &zeroBloomTex_))) {
            device_->CreateShaderResourceView(zeroBloomTex_.Get(), nullptr, &zeroBloomSrv_);
        }
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

    // Blend-state для Tonemap Re-issue: повний перезапис LdrLayer без змішування
    {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device_->CreateBlendState(&bd, &toneRgbOnlyBlend_))) {
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
    for (auto& s : savedToneSrvs_) s.Reset();
    savedToneExposureCb_.Reset();
    savedToneBlend_.Reset();

    savedHoloPs_.Reset();
    savedHoloCb13_.Reset();
    for (auto& p : holoPatchedPs_) p.Reset();
    for (auto& a : holoPatchAttempted_) a = false;
    holoRemapCb_.Reset();
    isHoloRemapped_ = false;

    savedBarsGs_.Reset();
    savedBarsGsCb0_.Reset();
    savedBarsRs_.Reset();
    supercruiseBarsGs_.Reset();
    supercruiseBarsCb_.Reset();
    barsRasterNoScissor_.Reset();
    barsRasterScissor_.Reset();
    isBarsActive_ = false;

    zeroBloomTex_.Reset();
    zeroBloomSrv_.Reset();

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
    compositePs_.Reset();
    constantBuffer_.Reset();
    compositeCs_.Reset();
    device_.Reset();

    isIntercepting_ = false;
    isTonemapReissuing_ = false;
    hasMultiplyDraws_ = false;
    multLayerCleared_ = false;
    viewportModified_ = false;
    width_ = 0;
    height_ = 0;
}

void UiLayer::resize(ID3D11Device* device, UINT width, UINT height) {
    if (!device || width == 0 || height == 0) return;
    if (!compositeCs_ || !compositePs_ || device_.Get() != device) {
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

    // 2. Створюємо LDR шар для результату тонемапу
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateTexture2D(&desc, nullptr, &ldrLayerTex_);
    device->CreateRenderTargetView(ldrLayerTex_.Get(), nullptr, &ldrLayerRtv_);
    device->CreateShaderResourceView(ldrLayerTex_.Get(), nullptr, &ldrLayerSrv_);

    // 3. Створюємо Mult шар для затемнення бекдропів
    device->CreateTexture2D(&desc, nullptr, &multLayerTex_);
    device->CreateRenderTargetView(multLayerTex_.Get(), nullptr, &multLayerRtv_);
    device->CreateShaderResourceView(multLayerTex_.Get(), nullptr, &multLayerSrv_);

    multLayerCleared_ = false;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> immCtx;
    device->GetImmediateContext(&immCtx);
    if (immCtx) {
        EvaluatingUpscalerGuard guard;
        const float clearTransparent[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const float clearMult[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        if (hdrLayerRtv_) immCtx->ClearRenderTargetView(hdrLayerRtv_.Get(), clearTransparent);
        if (ldrLayerRtv_) immCtx->ClearRenderTargetView(ldrLayerRtv_.Get(), clearTransparent);
        if (multLayerRtv_) {
            immCtx->ClearRenderTargetView(multLayerRtv_.Get(), clearMult);
            multLayerCleared_ = true;
        }
    }
}

void UiLayer::beginFrame(ID3D11DeviceContext* context) {
    if (!context || !hdrLayerRtv_) return;
    EvaluatingUpscalerGuard guard;

    // Очищуємо тільки HdrLayer (колір 0, пропускання = 1.0).
    // LdrLayer повністю перезаписується під час Tonemap Re-issue (BlendEnable=FALSE),
    // а MultLayer очищується лише якщо в попередньому кадрі були мультиплікативні виклики.
    const float clearTransparent[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    context->ClearRenderTargetView(hdrLayerRtv_.Get(), clearTransparent);

    if ((hasMultiplyDraws_ || !multLayerCleared_) && multLayerRtv_) {
        const float clearMult[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        context->ClearRenderTargetView(multLayerRtv_.Get(), clearMult);
        multLayerCleared_ = true;
    }
    hasMultiplyDraws_ = false;
}

bool UiLayer::shouldInterceptDraw(uint64_t vsHash, uint64_t psHash, bool isHdrTarget) {
    if (!isHdrTarget) {
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
    return (vsHash == 0x2CECEC3065EF0D4Aull && psHash == 0xA4D03619D631B186ull) ||
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
                             UINT renderW, UINT renderH, bool isHdr,
                             ID3D11DepthStencilView* overrideDsv,
                             ID3D11DepthStencilState* overrideDsState,
                             UINT overrideStencilRef,
                             ID3D11BlendState* overrideBlend,
                             ID3D11RenderTargetView* knownOrigRtv,
                             ID3D11DepthStencilView* knownOrigDsv,
                             ID3D11BlendState* knownOrigBlend,
                             const FLOAT* knownOrigBlendFactor,
                             UINT knownOrigSampleMask,
                             ID3D11DepthStencilState* knownOrigDsState,
                             UINT knownOrigStencilRef) {
    if (!context || isIntercepting_ || width_ == 0 || height_ == 0) return;
    if (renderW == 0) renderW = width_;
    if (renderH == 0) renderH = height_;
    EvaluatingUpscalerGuard guard;
    isIntercepting_ = true;

    if (knownOrigRtv) {
        savedRtv_ = knownOrigRtv;
        savedDsv_ = knownOrigDsv;
    } else {
        context->OMGetRenderTargets(1, savedRtv_.GetAddressOf(), savedDsv_.GetAddressOf());
    }

    if (knownOrigBlendFactor) {
        savedBlend_ = knownOrigBlend;
        savedBlendFactor_[0] = knownOrigBlendFactor[0];
        savedBlendFactor_[1] = knownOrigBlendFactor[1];
        savedBlendFactor_[2] = knownOrigBlendFactor[2];
        savedBlendFactor_[3] = knownOrigBlendFactor[3];
        savedSampleMask_ = knownOrigSampleMask;
    } else {
        context->OMGetBlendState(savedBlend_.GetAddressOf(), savedBlendFactor_, &savedSampleMask_);
    }

    if (knownOrigDsState || knownOrigRtv) {
        savedDsState_ = knownOrigDsState;
        savedStencilRef_ = knownOrigStencilRef;
    } else {
        context->OMGetDepthStencilState(savedDsState_.GetAddressOf(), &savedStencilRef_);
    }

    // Оновлюємо в'юпорти та scissor-прямокутники лише якщо потрібен масштаб або зсув джиттера:
    const bool needViewportTransform =
        (renderW != width_ || renderH != height_ ||
         std::fabs(jitterX) > 1e-4f || std::fabs(jitterY) > 1e-4f);
    viewportModified_ = needViewportTransform;
    if (needViewportTransform) {
        savedViewportCount_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        context->RSGetViewports(&savedViewportCount_, savedViewports_);
        savedScissorCount_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        context->RSGetScissorRects(&savedScissorCount_, savedScissors_);

        const float ax = static_cast<float>(width_) / static_cast<float>(renderW);
        const float ay = static_cast<float>(height_) / static_cast<float>(renderH);
        const float cancelX = -jitterX * ax;
        const float cancelY = -jitterY * ay;

        if (savedViewportCount_ > 0) {
            D3D11_VIEWPORT layerVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            for (UINT i = 0; i < savedViewportCount_; ++i) {
                layerVps[i].TopLeftX = savedViewports_[i].TopLeftX * ax + cancelX;
                layerVps[i].TopLeftY = savedViewports_[i].TopLeftY * ay + cancelY;
                layerVps[i].Width    = savedViewports_[i].Width * ax;
                layerVps[i].Height   = savedViewports_[i].Height * ay;
                layerVps[i].MinDepth = savedViewports_[i].MinDepth;
                layerVps[i].MaxDepth = savedViewports_[i].MaxDepth;
            }
            context->RSSetViewports(savedViewportCount_, layerVps);
        } else {
            const D3D11_VIEWPORT layerVp{cancelX, cancelY, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
            context->RSSetViewports(1, &layerVp);
        }

        if (savedScissorCount_ > 0) {
            D3D11_RECT layerScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
            const LONG maxW = static_cast<LONG>(width_);
            const LONG maxH = static_cast<LONG>(height_);
            for (UINT i = 0; i < savedScissorCount_; ++i) {
                const double l = static_cast<double>(savedScissors_[i].left) * ax + cancelX;
                const double t = static_cast<double>(savedScissors_[i].top) * ay + cancelY;
                const double r = static_cast<double>(savedScissors_[i].right) * ax + cancelX;
                const double b = static_cast<double>(savedScissors_[i].bottom) * ay + cancelY;
                LONG L = std::clamp(static_cast<LONG>(std::floor(l + 1e-3)), 0L, maxW);
                LONG T = std::clamp(static_cast<LONG>(std::floor(t + 1e-3)), 0L, maxH);
                LONG R = std::clamp(static_cast<LONG>(std::ceil(r - 1e-3)), L, maxW);
                LONG B = std::clamp(static_cast<LONG>(std::ceil(b - 1e-3)), T, maxH);
                layerScissors[i] = {L, T, R, B};
            }
            context->RSSetScissorRects(savedScissorCount_, layerScissors);
        }
    }

    ID3D11RenderTargetView* targetRtv = isHdr ? hdrLayerRtv_.Get() : ldrLayerRtv_.Get();
    ID3D11BlendState* layerBlend = overrideBlend;
    if (!layerBlend) {
        layerBlend = alphaBlend_.Get();
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
    }

    ID3D11DepthStencilView* dsvToBind = overrideDsv;
    if (!dsvToBind && savedDsv_ && renderW == width_ && renderH == height_) {
        dsvToBind = savedDsv_.Get();
    }

    if (overrideDsState) {
        if (overrideDsState != savedDsState_.Get() || overrideStencilRef != savedStencilRef_) {
            context->OMSetDepthStencilState(overrideDsState, overrideStencilRef);
        }
    } else if (!dsvToBind && noDepthState_) {
        context->OMSetDepthStencilState(noDepthState_.Get(), 0);
    }

    const FLOAT blendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (layerBlend) {
        context->OMSetBlendState(layerBlend, blendFactor, 0xFFFFFFFF);
    }

    context->OMSetRenderTargets(1, &targetRtv, dsvToBind);
}

void UiLayer::endIntercept(ID3D11DeviceContext* context) {
    if (!context || !isIntercepting_) return;
    EvaluatingUpscalerGuard guard;
    isIntercepting_ = false;

    if (isBarsActive_) {
        endSupercruiseBars(context);
    }
    if (isHoloRemapped_) {
        endHoloRemap(context);
    }

    if (viewportModified_) {
        if (savedViewportCount_ > 0) {
            context->RSSetViewports(savedViewportCount_, savedViewports_);
        }
        if (savedScissorCount_ > 0) {
            context->RSSetScissorRects(savedScissorCount_, savedScissors_);
        }
        viewportModified_ = false;
    }
    context->OMSetBlendState(savedBlend_.Get(), savedBlendFactor_, savedSampleMask_);
    context->OMSetDepthStencilState(savedDsState_.Get(), savedStencilRef_);
    context->OMSetRenderTargets(1, savedRtv_.GetAddressOf(), savedDsv_.Get());
    savedRtv_.Reset();
    savedDsv_.Reset();
    savedBlend_.Reset();
    savedDsState_.Reset();
}

bool UiLayer::beginHoloRemap(ID3D11DeviceContext* context, uint64_t psHash,
                             UINT renderW, UINT renderH, float jitterX, float jitterY,
                             float sceneCb1Row120Z) {
    if (!context || !device_ || !holoRemapCb_ || isHoloRemapped_ ||
        renderW == 0 || renderH == 0 || width_ == 0 || height_ == 0) {
        return false;
    }
    const int slot = findHoloProgramSlot(psHash);
    if (slot < 0 || slot >= 3) return false;

    EvaluatingUpscalerGuard guard;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> origPs;
    UINT numClasses = 0;
    context->PSGetShader(origPs.GetAddressOf(), nullptr, &numClasses);
    if (!origPs || numClasses != 0) return false;

    if (!holoPatchAttempted_[slot]) {
        holoPatchAttempted_[slot] = true;
        UINT bytes = 0;
        if (SUCCEEDED(origPs->GetPrivateData(kEdpePixelBytecodeGuid, &bytes, nullptr)) &&
            bytes > 0 && bytes <= 65536) {
            std::vector<BYTE> srcBytes(bytes), patchedBytes;
            if (SUCCEEDED(origPs->GetPrivateData(kEdpePixelBytecodeGuid, &bytes, srcBytes.data()))) {
                std::string reason;
                if (patchHoloPixelShader(srcBytes.data(), bytes, psHash, patchedBytes, reason)) {
                    device_->CreatePixelShader(
                        patchedBytes.data(), patchedBytes.size(), nullptr, &holoPatchedPs_[slot]);
                }
            }
        }
    }
    if (!holoPatchedPs_[slot]) return false;

    const float invScaleX = static_cast<float>(renderW) / static_cast<float>(width_);
    const float invScaleY = static_cast<float>(renderH) / static_cast<float>(height_);
    const float kScale = std::sqrt((static_cast<float>(width_) / static_cast<float>(renderW)) *
                                   (static_cast<float>(height_) / static_cast<float>(renderH)));
    const float baseZ = (std::isfinite(sceneCb1Row120Z) && sceneCb1Row120Z > 1.0f) ? sceneCb1Row120Z : 1.0f;

    HoloRemapParams params{};
    params.scaleX = invScaleX;
    params.scaleY = invScaleY;
    params.biasX  = jitterX;
    params.biasY  = jitterY;
    params.relief = kScale * baseZ;

    savedHoloPs_ = origPs;
    context->PSGetConstantBuffers(13, 1, savedHoloCb13_.GetAddressOf());
    context->UpdateSubresource(holoRemapCb_.Get(), 0, nullptr, &params, 0, 0);
    ID3D11Buffer* cb13 = holoRemapCb_.Get();
    context->PSSetConstantBuffers(13, 1, &cb13);
    context->PSSetShader(holoPatchedPs_[slot].Get(), nullptr, 0);
    isHoloRemapped_ = true;
    return true;
}

void UiLayer::endHoloRemap(ID3D11DeviceContext* context) {
    if (!context || !isHoloRemapped_) return;
    EvaluatingUpscalerGuard guard;
    isHoloRemapped_ = false;
    context->PSSetShader(savedHoloPs_.Get(), nullptr, 0);
    ID3D11Buffer* origCb13 = savedHoloCb13_.Get();
    context->PSSetConstantBuffers(13, 1, &origCb13);
    savedHoloPs_.Reset();
    savedHoloCb13_.Reset();
}

bool UiLayer::beginSupercruiseBars(ID3D11DeviceContext* context, UINT renderW, UINT renderH) {
    if (!context || !supercruiseBarsGs_ || !supercruiseBarsCb_ ||
        !barsRasterNoScissor_ || !barsRasterScissor_ || isBarsActive_ ||
        renderW == 0 || renderH == 0 || width_ == 0 || height_ == 0) {
        return false;
    }
    EvaluatingUpscalerGuard guard;
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> curGs;
    UINT numClasses = 0;
    context->GSGetShader(curGs.GetAddressOf(), nullptr, &numClasses);
    if (curGs || numClasses != 0) return false;

    D3D11_VIEWPORT vp{};
    UINT vpCount = 1;
    context->RSGetViewports(&vpCount, &vp);
    const float vpW = (vpCount > 0 && vp.Width > 0.0f) ? vp.Width : static_cast<float>(width_);
    const float vpH = (vpCount > 0 && vp.Height > 0.0f) ? vp.Height : static_cast<float>(height_);
    const float halfWidth = std::clamp(vpW / static_cast<float>(renderW), 1.0f, 16.0f);
    const float stripParams[4] = {vpW, vpH, halfWidth, 0.01f};

    context->GSGetConstantBuffers(0, 1, savedBarsGsCb0_.GetAddressOf());
    context->RSGetState(savedBarsRs_.GetAddressOf());
    bool scissorOn = false;
    if (savedBarsRs_) {
        D3D11_RASTERIZER_DESC rd{};
        savedBarsRs_->GetDesc(&rd);
        scissorOn = (rd.ScissorEnable != FALSE);
    }

    context->UpdateSubresource(supercruiseBarsCb_.Get(), 0, nullptr, stripParams, 0, 0);
    ID3D11Buffer* cb0 = supercruiseBarsCb_.Get();
    context->GSSetConstantBuffers(0, 1, &cb0);
    context->RSSetState(scissorOn ? barsRasterScissor_.Get() : barsRasterNoScissor_.Get());
    context->GSSetShader(supercruiseBarsGs_.Get(), nullptr, 0);
    isBarsActive_ = true;
    return true;
}

void UiLayer::endSupercruiseBars(ID3D11DeviceContext* context) {
    if (!context || !isBarsActive_) return;
    EvaluatingUpscalerGuard guard;
    isBarsActive_ = false;
    context->GSSetShader(savedBarsGs_.Get(), nullptr, 0);
    ID3D11Buffer* origCb0 = savedBarsGsCb0_.Get();
    context->GSSetConstantBuffers(0, 1, &origCb0);
    context->RSSetState(savedBarsRs_.Get());
    savedBarsGs_.Reset();
    savedBarsGsCb0_.Reset();
    savedBarsRs_.Reset();
}

bool UiLayer::beginTonemapReissue(ID3D11DeviceContext* context,
                                  ID3D11ShaderResourceView* gameLut,
                                  ID3D11Buffer* gameExposureCb,
                                  int hdrSlot,
                                  uint64_t psHash) {
    if (!context || !hdrLayerSrv_ || !ldrLayerRtv_ || !toneRgbOnlyBlend_ || isTonemapReissuing_) {
        return false;
    }
    EvaluatingUpscalerGuard guard;
    isTonemapReissuing_ = true;

    // Зберігаємо оригінальний стан шейдерних ресурсів (слоти t0..t4), RTV та в'юпорту гри
    savedToneViewportCount_ = 1;
    context->RSGetViewports(&savedToneViewportCount_, &savedToneViewport_);
    context->OMGetRenderTargets(1, savedToneRtv_.GetAddressOf(), savedToneDsv_.GetAddressOf());
    for (int i = 0; i < 5; ++i) {
        context->PSGetShaderResources(static_cast<UINT>(i), 1, savedToneSrvs_[i].GetAddressOf());
    }
    context->PSGetConstantBuffers(2, 1, savedToneExposureCb_.GetAddressOf());
    context->OMGetBlendState(savedToneBlend_.GetAddressOf(), savedToneBlendFactor_, &savedToneSampleMask_);

    if (gameExposureCb) {
        context->PSSetConstantBuffers(2, 1, &gameExposureCb);
    }

    ID3D11ShaderResourceView* hudHdrSrv = hdrLayerSrv_.Get();
    ID3D11ShaderResourceView* zeroSrv = zeroBloomSrv_.Get();

    if (hdrSlot == 0) {
        // Галактична/Системна карта (9270 / C89D) та Full System Scanner (6DE7):
        // t0 = HDR-сцена, t1 = Bloom (обнулюємо Bloom, щоб не додавати його двічі)
        context->PSSetShaderResources(0, 1, &hudHdrSrv);
        context->PSSetShaderResources(1, 1, &zeroSrv);
    } else {
        // Кокпітний тонемап (E894 / 0D43 / 99C2 / DE65):
        // t0 = 3D LUT, t1 = HDR-сцена (або DoF), t2..t4 = Bloom (обнулюємо слоти Bloom!)
        if (gameLut) {
            context->PSSetShaderResources(0, 1, &gameLut);
        }
        context->PSSetShaderResources(1, 1, &hudHdrSrv);
        const bool isDofTonemap =
            (psHash == 0xDE65D1AC161643F4ull || psHash == 0xDE65BF04523B41F8ull || hdrSlot == 3);
        if (isDofTonemap) {
            // У варіанті з DoF слот t1 читає розмиту сцену, а t3 — чітку HDR-сцену:
            // подаємо HdrLayer в обидва слоти, а слоти Bloom t2 і t4 обнулюємо.
            context->PSSetShaderResources(2, 1, &zeroSrv);
            context->PSSetShaderResources(3, 1, &hudHdrSrv);
            context->PSSetShaderResources(4, 1, &zeroSrv);
        } else {
            ID3D11ShaderResourceView* zeroSlots[3] = {zeroSrv, zeroSrv, zeroSrv};
            context->PSSetShaderResources(2, 3, zeroSlots);
        }
    }

    // Спрямовуємо вивід у LdrLayer
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

    // Відновлюємо оригінальний стан шейдерних ресурсів (t0..t4) та RTV гри
    // (окремий повно-екранний прохід копіювання альфи більше не потрібен, оскільки
    // шейдер композитингу напряму читає HdrLayer.a зі слота t3).
    ID3D11ShaderResourceView* origSrvs[5] = {
        savedToneSrvs_[0].Get(),
        savedToneSrvs_[1].Get(),
        savedToneSrvs_[2].Get(),
        savedToneSrvs_[3].Get(),
        savedToneSrvs_[4].Get()
    };
    context->PSSetShaderResources(0, 5, origSrvs);
    ID3D11Buffer* origExposureCb = savedToneExposureCb_.Get();
    context->PSSetConstantBuffers(2, 1, &origExposureCb);
    context->OMSetBlendState(savedToneBlend_.Get(), savedToneBlendFactor_, savedToneSampleMask_);
    context->RSSetViewports(savedToneViewportCount_, &savedToneViewport_);
    context->OMSetRenderTargets(1, savedToneRtv_.GetAddressOf(), savedToneDsv_.Get());

    savedToneRtv_.Reset();
    savedToneDsv_.Reset();
    for (auto& s : savedToneSrvs_) s.Reset();
    savedToneExposureCb_.Reset();
    savedToneBlend_.Reset();
}

bool UiLayer::composite(ID3D11DeviceContext* context,
                        ID3D11ShaderResourceView* worldReconstruction,
                        ID3D11UnorderedAccessView* backbufferUav,
                        UINT outputWidth, UINT outputHeight,
                        bool debugView) {
    if (!context || !worldReconstruction || !backbufferUav || !compositeCs_ || !hdrLayerSrv_) return false;
    EvaluatingUpscalerGuard guard;

    // Усуваємо D3D11 RTV/UAV hazard: якщо цільова текстура прив'язана як OM RTV,
    // тимчасово відв'язуємо RTV перед прив'язкою CS UAV.
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> prevRtv;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> prevDsv;
    context->OMGetRenderTargets(1, prevRtv.GetAddressOf(), prevDsv.GetAddressOf());
    if (prevRtv || prevDsv) {
        ID3D11RenderTargetView* nullRtv = nullptr;
        context->OMSetRenderTargets(1, &nullRtv, nullptr);
    }

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

    // Біндимо ресурси (t0 = world, t1 = ldrUi, t2 = mult, t3 = hdrUi)
    ID3D11ShaderResourceView* srvs[4] = {
        worldReconstruction,
        ldrLayerSrv_.Get(),
        multLayerSrv_.Get(),
        hdrLayerSrv_.Get()
    };
    context->CSSetShaderResources(0, 4, srvs);
    context->CSSetUnorderedAccessViews(0, 1, &backbufferUav, nullptr);
    context->CSSetConstantBuffers(0, 1, constantBuffer_.GetAddressOf());
    context->CSSetShader(compositeCs_.Get(), nullptr, 0);

    // Запуск обчислювальної сітки 32x8
    const UINT groupsX = (outputWidth + 31) / 32;
    const UINT groupsY = (outputHeight + 7) / 8;
    context->Dispatch(groupsX, groupsY, 1);

    // Розбіндинг
    ID3D11ShaderResourceView* nullSrvs[4] = {nullptr, nullptr, nullptr, nullptr};
    ID3D11UnorderedAccessView* nullUav = nullptr;
    context->CSSetShaderResources(0, 4, nullSrvs);
    context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);

    if (prevRtv || prevDsv) {
        context->OMSetRenderTargets(1, prevRtv.GetAddressOf(), prevDsv.Get());
    }
    return true;
}

bool UiLayer::compositeToRtv(ID3D11DeviceContext* context,
                             ID3D11ShaderResourceView* worldReconstruction,
                             ID3D11RenderTargetView* targetRtv,
                             UINT outputWidth, UINT outputHeight,
                             bool debugView,
                             bool callerRestoresState) {
    if (!context || !worldReconstruction || !targetRtv ||
        !coverageVs_ || !compositePs_ || !hdrLayerSrv_) {
        return false;
    }
    EvaluatingUpscalerGuard guard;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        auto* data = reinterpret_cast<uint32_t*>(mapped.pData);
        data[0] = outputWidth;
        data[1] = outputHeight;
        data[2] = hasMultiplyDraws_ ? 1 : 0;
        data[3] = debugView ? 1 : 0;
        context->Unmap(constantBuffer_.Get(), 0);
    }

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> origRtv;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> origDsv;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> origVs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> origPs;
    Microsoft::WRL::ComPtr<ID3D11GeometryShader> origGs;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> origLayout;
    D3D11_PRIMITIVE_TOPOLOGY origTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    Microsoft::WRL::ComPtr<ID3D11BlendState> origBlend;
    FLOAT origBlendFactor[4]{};
    UINT origSampleMask = 0xFFFFFFFF;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> origDs;
    UINT origStencilRef = 0;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> origRs;
    D3D11_VIEWPORT origVp{};
    UINT origVpCount = 1;
    ID3D11ShaderResourceView* origSrvs[4]{};
    Microsoft::WRL::ComPtr<ID3D11Buffer> origPsCb0;

    if (!callerRestoresState) {
        context->OMGetRenderTargets(1, origRtv.GetAddressOf(), origDsv.GetAddressOf());
        context->VSGetShader(origVs.GetAddressOf(), nullptr, nullptr);
        context->PSGetShader(origPs.GetAddressOf(), nullptr, nullptr);
        context->GSGetShader(origGs.GetAddressOf(), nullptr, nullptr);
        context->IAGetInputLayout(origLayout.GetAddressOf());
        context->IAGetPrimitiveTopology(&origTopo);
        context->OMGetBlendState(origBlend.GetAddressOf(), origBlendFactor, &origSampleMask);
        context->OMGetDepthStencilState(origDs.GetAddressOf(), &origStencilRef);
        context->RSGetState(origRs.GetAddressOf());
        context->RSGetViewports(&origVpCount, &origVp);
        context->PSGetShaderResources(0, 4, origSrvs);
        context->PSGetConstantBuffers(0, 1, origPsCb0.GetAddressOf());
    }

    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(outputWidth), static_cast<float>(outputHeight), 0.0f, 1.0f};
    context->RSSetViewports(1, &vp);
    context->RSSetState(nullptr);
    const FLOAT blendFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    context->OMSetBlendState(toneRgbOnlyBlend_.Get(), blendFactor, 0xFFFFFFFF);
    if (noDepthState_) {
        context->OMSetDepthStencilState(noDepthState_.Get(), 0);
    }
    context->OMSetRenderTargets(1, &targetRtv, nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(coverageVs_.Get(), nullptr, 0);
    context->PSSetShader(compositePs_.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);

    ID3D11ShaderResourceView* srvs[4] = {
        worldReconstruction,
        ldrLayerSrv_.Get(),
        multLayerSrv_.Get(),
        hdrLayerSrv_.Get()
    };
    context->PSSetShaderResources(0, 4, srvs);
    ID3D11Buffer* cb0 = constantBuffer_.Get();
    context->PSSetConstantBuffers(0, 1, &cb0);

    context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrvs[4] = {nullptr, nullptr, nullptr, nullptr};
    context->PSSetShaderResources(0, 4, nullSrvs);
    ID3D11RenderTargetView* nullRtv = nullptr;
    context->OMSetRenderTargets(1, &nullRtv, nullptr);

    if (!callerRestoresState) {
        context->PSSetShaderResources(0, 4, origSrvs);
        for (int i = 0; i < 4; ++i) {
            if (origSrvs[i]) origSrvs[i]->Release();
        }
        ID3D11Buffer* restoreCb0 = origPsCb0.Get();
        context->PSSetConstantBuffers(0, 1, &restoreCb0);
        context->VSSetShader(origVs.Get(), nullptr, 0);
        context->PSSetShader(origPs.Get(), nullptr, 0);
        context->GSSetShader(origGs.Get(), nullptr, 0);
        context->IASetInputLayout(origLayout.Get());
        context->IASetPrimitiveTopology(origTopo);
        context->RSSetState(origRs.Get());
        if (origVpCount > 0) {
            context->RSSetViewports(origVpCount, &origVp);
        }
        context->OMSetBlendState(origBlend.Get(), origBlendFactor, origSampleMask);
        context->OMSetDepthStencilState(origDs.Get(), origStencilRef);
        context->OMSetRenderTargets(1, origRtv.GetAddressOf(), origDsv.Get());
    }

    return true;
}

} // namespace edpe
