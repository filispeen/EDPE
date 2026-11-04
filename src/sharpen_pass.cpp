#include "sharpen_pass.h"
#include "log.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cwchar>

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

static DXGI_FORMAT toConcreteRtvFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return DXGI_FORMAT_R11G11B10_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default:
        return fmt;
    }
}

} // namespace

bool SharpenPass::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context || device_ ||
        device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        return false;
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
    vertexShader_.Reset();
    pixelShader_.Reset();
    constantBuffer_.Reset();
    sourceTexture_.Reset();
    sourceSrv_.Reset();
    stagingTarget_.Reset();
    stagingRtv_.Reset();
    cachedTarget_ = nullptr;
    cachedRtv_.Reset();
    cachedDesc_ = {};
    device_ = nullptr;
    width_ = 0;
    height_ = 0;
    format_ = DXGI_FORMAT_UNKNOWN;
    direct_ = false;
}

bool SharpenPass::ensureResources(const D3D11_TEXTURE2D_DESC& desc, bool directTarget) {
    if (!device_ || desc.Width == 0 || desc.Height == 0) return false;

    const DXGI_FORMAT concreteFormat = toConcreteRtvFormat(desc.Format);

    if (sourceTexture_ && width_ == desc.Width && height_ == desc.Height &&
        format_ == concreteFormat && direct_ == directTarget) {
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
    td.Format = concreteFormat;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &sourceTexture_);
    if (FAILED(hr)) {
        wchar_t msg[192];
        swprintf_s(msg, L"EDPE: sharpen ensureResources CreateTexture2D(source) failed: hr=0x%08X removed=0x%08X",
            static_cast<unsigned>(hr), static_cast<unsigned>(device_->GetDeviceRemovedReason()));
        EdpeLog(msg);
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = concreteFormat;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    hr = device_->CreateShaderResourceView(sourceTexture_.Get(), &srvDesc, &sourceSrv_);
    if (FAILED(hr)) {
        wchar_t msg[192];
        swprintf_s(msg, L"EDPE: sharpen ensureResources CreateShaderResourceView failed: hr=0x%08X removed=0x%08X",
            static_cast<unsigned>(hr), static_cast<unsigned>(device_->GetDeviceRemovedReason()));
        EdpeLog(msg);
        sourceTexture_.Reset();
        return false;
    }

    if (!directTarget) {
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = device_->CreateTexture2D(&td, nullptr, &stagingTarget_);
        if (FAILED(hr)) {
            wchar_t msg[192];
            swprintf_s(msg, L"EDPE: sharpen ensureResources CreateTexture2D(staging) failed: hr=0x%08X removed=0x%08X",
                static_cast<unsigned>(hr), static_cast<unsigned>(device_->GetDeviceRemovedReason()));
            EdpeLog(msg);
            sourceTexture_.Reset();
            sourceSrv_.Reset();
            return false;
        }

        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = concreteFormat;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;
        hr = device_->CreateRenderTargetView(stagingTarget_.Get(), &rtvDesc, &stagingRtv_);
        if (FAILED(hr)) {
            wchar_t msg[192];
            swprintf_s(msg, L"EDPE: sharpen ensureResources CreateRenderTargetView(staging) failed: hr=0x%08X removed=0x%08X",
                static_cast<unsigned>(hr), static_cast<unsigned>(device_->GetDeviceRemovedReason()));
            EdpeLog(msg);
            sourceTexture_.Reset();
            sourceSrv_.Reset();
            stagingTarget_.Reset();
            return false;
        }
    }

    width_ = desc.Width;
    height_ = desc.Height;
    format_ = concreteFormat;
    direct_ = directTarget;
    return true;
}

bool SharpenPass::apply(ID3D11DeviceContext* context, ID3D11Texture2D* target, float sharpness, bool hdr) {
    if (!device_ || !context || !target || sharpness <= 0.0f) {
        if (device_) {
            wchar_t msg[192];
            swprintf_s(msg, L"EDPE: sharpen apply invalid params: dev=%p ctx=%p target=%p sharp=%.2f",
                device_, context, target, sharpness);
            EdpeLog(msg);
        }
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    target->GetDesc(&desc);
    if (desc.SampleDesc.Count > 1) {
        EdpeLog(L"EDPE: sharpen apply failed: MSAA target not supported");
        return false;
    }

    const bool direct = (desc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
    if (!ensureResources(desc, direct)) {
        EdpeLog(L"EDPE: sharpen apply failed: ensureResources returned false");
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> tempRtv;
    ID3D11RenderTargetView* targetRtv = nullptr;
    if (direct) {
        const DXGI_FORMAT concreteFormat = toConcreteRtvFormat(desc.Format);
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtvDesc.Texture2D.MipSlice = 0;
        rtvDesc.Format = concreteFormat;
        const D3D11_RENDER_TARGET_VIEW_DESC* pRtvDesc = (rtvDesc.Format != desc.Format) ? &rtvDesc : nullptr;

        const bool cacheable = (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
        if (cacheable) {
            if (target != cachedTarget_ || !cachedRtv_ ||
                cachedDesc_.Width != desc.Width || cachedDesc_.Height != desc.Height ||
                cachedDesc_.Format != desc.Format) {
                cachedRtv_.Reset();
                cachedTarget_ = target;
                cachedDesc_ = desc;

                HRESULT hr = device_->CreateRenderTargetView(target, pRtvDesc, &cachedRtv_);
                if (FAILED(hr)) {
                    wchar_t msg[256];
                    swprintf_s(msg, L"EDPE: sharpen apply CreateRenderTargetView(target) failed: hr=0x%08X removed=0x%08X format=%u concrete=%u",
                        static_cast<unsigned>(hr), static_cast<unsigned>(device_->GetDeviceRemovedReason()),
                        static_cast<unsigned>(desc.Format), static_cast<unsigned>(concreteFormat));
                    EdpeLog(msg);
                    cachedTarget_ = nullptr;
                    return false;
                }
            }
            targetRtv = cachedRtv_.Get();
        } else {
            cachedRtv_.Reset();
            cachedTarget_ = nullptr;
            HRESULT hr = device_->CreateRenderTargetView(target, pRtvDesc, &tempRtv);
            if (FAILED(hr)) {
                wchar_t msg[256];
                swprintf_s(msg, L"EDPE: sharpen apply CreateRenderTargetView(target) failed: hr=0x%08X removed=0x%08X format=%u concrete=%u",
                    static_cast<unsigned>(hr), static_cast<unsigned>(device_->GetDeviceRemovedReason()),
                    static_cast<unsigned>(desc.Format), static_cast<unsigned>(concreteFormat));
                EdpeLog(msg);
                return false;
            }
            targetRtv = tempRtv.Get();
        }
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

    // Save previous pipeline state manually (without ID3DDeviceContextState)
    UINT origVpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT origVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    context->RSGetViewports(&origVpCount, origVps);

    Microsoft::WRL::ComPtr<ID3D11RasterizerState> origRs;
    context->RSGetState(&origRs);

    Microsoft::WRL::ComPtr<ID3D11BlendState> origBs;
    FLOAT origBlendFactor[4]{};
    UINT origSampleMask = 0xFFFFFFFF;
    context->OMGetBlendState(&origBs, origBlendFactor, &origSampleMask);

    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> origDss;
    UINT origStencilRef = 0;
    context->OMGetDepthStencilState(&origDss, &origStencilRef);

    ID3D11RenderTargetView* origRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* origDsv = nullptr;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, origRtvs, &origDsv);

    Microsoft::WRL::ComPtr<ID3D11InputLayout> origInputLayout;
    context->IAGetInputLayout(&origInputLayout);

    D3D11_PRIMITIVE_TOPOLOGY origTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    context->IAGetPrimitiveTopology(&origTopology);

    Microsoft::WRL::ComPtr<ID3D11VertexShader> origVs;
    context->VSGetShader(&origVs, nullptr, nullptr);

    Microsoft::WRL::ComPtr<ID3D11PixelShader> origPs;
    context->PSGetShader(&origPs, nullptr, nullptr);

    Microsoft::WRL::ComPtr<ID3D11GeometryShader> origGs;
    context->GSGetShader(&origGs, nullptr, nullptr);

    Microsoft::WRL::ComPtr<ID3D11HullShader> origHs;
    context->HSGetShader(&origHs, nullptr, nullptr);

    Microsoft::WRL::ComPtr<ID3D11DomainShader> origDs;
    context->DSGetShader(&origDs, nullptr, nullptr);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> origPsSrv;
    context->PSGetShaderResources(0, 1, &origPsSrv);

    Microsoft::WRL::ComPtr<ID3D11Buffer> origPsCb;
    context->PSGetConstantBuffers(0, 1, &origPsCb);

    // Bind sharpen pipeline state
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0.0f, 1.0f};
    context->RSSetViewports(1, &vp);
    context->RSSetState(nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(nullptr, 0);
    context->OMSetRenderTargets(1, &targetRtv, nullptr);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context->PSSetShader(pixelShader_.Get(), nullptr, 0);
    if (origGs) context->GSSetShader(nullptr, nullptr, 0);
    if (origHs) context->HSSetShader(nullptr, nullptr, 0);
    if (origDs) context->DSSetShader(nullptr, nullptr, 0);

    ID3D11ShaderResourceView* srvs[1] = { sourceSrv_.Get() };
    context->PSSetShaderResources(0, 1, srvs);
    ID3D11Buffer* cbs[1] = { constantBuffer_.Get() };
    context->PSSetConstantBuffers(0, 1, cbs);

    context->Draw(3, 0);

    // Mandatory unbind
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    context->PSSetShaderResources(0, 1, nullSrv);
    ID3D11RenderTargetView* nullRtv[1] = { nullptr };
    context->OMSetRenderTargets(1, nullRtv, nullptr);

    // Restore original pipeline state
    if (origVpCount > 0) {
        context->RSSetViewports(origVpCount, origVps);
    }
    context->RSSetState(origRs.Get());
    context->OMSetBlendState(origBs.Get(), origBlendFactor, origSampleMask);
    context->OMSetDepthStencilState(origDss.Get(), origStencilRef);
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, origRtvs, origDsv);
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        if (origRtvs[i]) origRtvs[i]->Release();
    }
    if (origDsv) origDsv->Release();
    context->IASetInputLayout(origInputLayout.Get());
    context->IASetPrimitiveTopology(origTopology);
    context->VSSetShader(origVs.Get(), nullptr, 0);
    context->PSSetShader(origPs.Get(), nullptr, 0);
    if (origGs) context->GSSetShader(origGs.Get(), nullptr, 0);
    if (origHs) context->HSSetShader(origHs.Get(), nullptr, 0);
    if (origDs) context->DSSetShader(origDs.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvToRestore = origPsSrv.Get();
    context->PSSetShaderResources(0, 1, &srvToRestore);
    ID3D11Buffer* cbToRestore = origPsCb.Get();
    context->PSSetConstantBuffers(0, 1, &cbToRestore);

    if (!direct) {
        context->CopyResource(target, stagingTarget_.Get());
    }
    return true;
}

} // namespace edpe
