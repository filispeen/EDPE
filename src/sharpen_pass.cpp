#include "sharpen_pass.h"
#include "log.h"

#include <d3dcompiler.h>
#include <algorithm>

namespace edpe {

namespace {

struct SharpenConstants {
    float params[4];
};

inline constexpr char kSharpenShader[] = R"(
cbuffer SharpenConstants : register(b0) {
    float4 params;
};

Texture2D<float4> src : register(t0);

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOutput vs_main(uint id : SV_VertexID) {
    VSOutput o;
    float2 corner = float2((id << 1) & 2, id & 2);
    o.pos = float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = corner;
    return o;
}

float3 enc(float3 c) {
    c = max(c, 0.0);
    return params.w > 0.5 ? c / (1.0 + c) : saturate(c);
}

float3 dec(float3 c) {
    c = saturate(c);
    return params.w > 0.5 ? c / max(1.0 - c, 1e-4) : c;
}

float3 fetch(int2 p, int2 hi) {
    return enc(src.Load(int3(clamp(p, int2(0, 0), hi), 0)).rgb);
}

float4 ps_main(VSOutput i) : SV_Target {
    int2 hi = int2((int)params.y - 1, (int)params.z - 1);
    int2 p = int2(i.pos.xy);

    float3 a = fetch(p + int2(-1, -1), hi);
    float3 b = fetch(p + int2( 0, -1), hi);
    float3 c = fetch(p + int2( 1, -1), hi);
    float3 d = fetch(p + int2(-1,  0), hi);
    float4 center = src.Load(int3(clamp(p, int2(0, 0), hi), 0));
    float3 e = enc(center.rgb);
    float3 f = fetch(p + int2( 1,  0), hi);
    float3 g = fetch(p + int2(-1,  1), hi);
    float3 h = fetch(p + int2( 0,  1), hi);
    float3 k = fetch(p + int2( 1,  1), hi);

    float3 mn = min(min(min(d, e), min(f, b)), h);
    float3 mn2 = min(min(a, c), min(g, k));
    mn += min(mn, mn2);

    float3 mx = max(max(max(d, e), max(f, b)), h);
    float3 mx2 = max(max(a, c), max(g, k));
    mx += max(mx, mx2);

    float3 amp = saturate(min(mn, 2.0 - mx) / max(mx, 1e-4));
    amp = sqrt(amp);

    float peak = -0.2 * saturate(params.x);
    float3 w = amp * peak;
    float3 rcpW = 1.0 / (1.0 + 4.0 * w);

    float3 result = dec(max(((b + d + f + h) * w + e) * rcpW, 0.0));
    return float4(result, center.a);
}
)";

} // namespace

bool SharpenPass::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
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

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    if (FAILED(D3DCompile(kSharpenShader, sizeof(kSharpenShader) - 1, nullptr, nullptr,
            nullptr, "vs_main", "vs_5_0", 0, 0, &vsBlob, &errBlob))) {
        EdpeLog(L"EDPE: sharpen vertex shader compile failed");
        return false;
    }
    if (FAILED(D3DCompile(kSharpenShader, sizeof(kSharpenShader) - 1, nullptr, nullptr,
            nullptr, "ps_main", "ps_5_0", 0, 0, &psBlob, &errBlob))) {
        EdpeLog(L"EDPE: sharpen pixel shader compile failed");
        return false;
    }

    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), nullptr, &vertexShader_)) ||
        FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(),
            psBlob->GetBufferSize(), nullptr, &pixelShader_))) {
        return false;
    }

    D3D11_BUFFER_DESC bufDesc{};
    bufDesc.ByteWidth = sizeof(SharpenConstants);
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&bufDesc, nullptr, &constantBuffer_))) {
        return false;
    }

    device_ = device;
    return true;
}

void SharpenPass::shutdown() {
    state_.Reset();
    vertexShader_.Reset();
    pixelShader_.Reset();
    constantBuffer_.Reset();
    sourceTexture_.Reset();
    sourceSrv_.Reset();
    stagingTarget_.Reset();
    stagingRtv_.Reset();
    context_.Reset();
    device_ = nullptr;
    width_ = 0;
    height_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;
    direct_ = false;
}

bool SharpenPass::ensureResources(const D3D11_TEXTURE2D_DESC& desc, bool directTarget) {
    if (!device_ || desc.Width == 0 || desc.Height == 0) return false;

    if (sourceTexture_ && width_ == desc.Width && height_ == desc.Height &&
        format_ == desc.Format && direct_ == directTarget) {
        return true;
    }

    sourceTexture_.Reset();
    sourceSrv_.Reset();
    stagingTarget_.Reset();
    stagingRtv_.Reset();
    width_ = 0;
    height_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = desc.Width;
    td.Height = desc.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = desc.Format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(device_->CreateTexture2D(&td, nullptr, &sourceTexture_)) ||
        FAILED(device_->CreateShaderResourceView(sourceTexture_.Get(), nullptr, &sourceSrv_))) {
        sourceTexture_.Reset();
        sourceSrv_.Reset();
        return false;
    }

    if (!directTarget) {
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &stagingTarget_)) ||
            FAILED(device_->CreateRenderTargetView(stagingTarget_.Get(), nullptr, &stagingRtv_))) {
            sourceTexture_.Reset();
            sourceSrv_.Reset();
            stagingTarget_.Reset();
            stagingRtv_.Reset();
            return false;
        }
    }

    width_ = desc.Width;
    height_ = desc.Height;
    format_ = desc.Format;
    direct_ = directTarget;
    return true;
}

bool SharpenPass::apply(ID3D11DeviceContext* context, ID3D11Texture2D* target, float sharpness, bool hdr) {
    if (!device_ || !context || !target || sharpness <= 0.0f) return false;

    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);
    if (desc.SampleDesc.Count > 1) return false;

    const bool direct = (desc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
    if (!ensureResources(desc, direct)) return false;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> localRtv;
    ID3D11RenderTargetView* targetRtv = nullptr;
    if (direct) {
        if (FAILED(device_->CreateRenderTargetView(target, nullptr, &localRtv))) return false;
        targetRtv = localRtv.Get();
    } else {
        targetRtv = stagingRtv_.Get();
    }

    context->CopyResource(sourceTexture_.Get(), target);

    SharpenConstants cb{};
    cb.params[0] = std::clamp(sharpness, 0.0f, 1.0f);
    cb.params[1] = static_cast<float>(desc.Width);
    cb.params[2] = static_cast<float>(desc.Height);
    cb.params[3] = hdr ? 1.0f : 0.0f;
    context->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &cb, 0, 0);

    Microsoft::WRL::ComPtr<ID3DDeviceContextState> prior;
    if (state_ && context_) {
        context_->SwapDeviceContextState(state_.Get(), &prior);
    }

    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0.0f, 1.0f};
    context->RSSetViewports(1, &vp);
    context->OMSetRenderTargets(1, &targetRtv, nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context->PSSetShader(pixelShader_.Get(), nullptr, 0);

    ID3D11ShaderResourceView* srvs[1] = { sourceSrv_.Get() };
    context->PSSetShaderResources(0, 1, srvs);
    ID3D11Buffer* cbs[1] = { constantBuffer_.Get() };
    context->PSSetConstantBuffers(0, 1, cbs);

    context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    context->PSSetShaderResources(0, 1, nullSrv);
    ID3D11RenderTargetView* nullRtv = nullptr;
    context->OMSetRenderTargets(1, &nullRtv, nullptr);

    if (state_ && prior && context_) {
        context_->SwapDeviceContextState(prior.Get(), nullptr);
    }

    if (!direct) {
        context->CopyResource(target, stagingTarget_.Get());
    }
    return true;
}

} // namespace edpe
