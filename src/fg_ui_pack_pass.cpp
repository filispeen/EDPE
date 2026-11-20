#include "fg_ui_pack_pass.h"

#include "renderdoc_support.h"

#include <d3dcompiler.h>

namespace edpe {

namespace {

struct PackParams {
    uint32_t outputSize[2];
    uint32_t hasMultiply;
    uint32_t _pad;
};

constexpr char kFgUiPackCs[] = R"(
Texture2D<float4> tLdrUi : register(t0);
Texture2D<float4> tMult  : register(t1);
Texture2D<float4> tHdrUi : register(t2);
RWTexture2D<float4> uPackedUi : register(u0);

cbuffer PackParams : register(b0) {
    uint2 outputSize;
    uint  hasMultiply;
    uint  _pad;
};

[numthreads(32, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outputSize.x || id.y >= outputSize.y) return;
    int3 p = int3(id.xy, 0);

    float4 hdrUi = tHdrUi.Load(p);
    float transmittance = saturate(hdrUi.a);
    float3 mult = (hasMultiply != 0) ? tMult.Load(p).rgb : float3(1.0, 1.0, 1.0);

    // Fast path: untouched background pixel -> zero premultiplied UI + zero alpha.
    if (all(hdrUi.rgb <= 1e-5) && transmittance >= 0.999 && all(mult >= 0.999)) {
        uPackedUi[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    float4 ui = tLdrUi.Load(p);
    int2 maxCoord = max(int2(outputSize) - int2(1, 1), int2(0, 0));
    int3 corners[4] = {
        int3(0, 0, 0),
        int3(maxCoord.x, 0, 0),
        int3(0, maxCoord.y, 0),
        int3(maxCoord.x, maxCoord.y, 0)
    };
    float3 blackFloor = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int ci = 0; ci < 4; ++ci) {
        float4 hc = tHdrUi.Load(corners[ci]);
        float4 uc = tLdrUi.Load(corners[ci]);
        if (hc.a >= 0.999 && all(hc.rgb <= 1e-5) && all(uc.rgb < 0.08)) {
            blackFloor = uc.rgb;
            break;
        }
    }

    float3 uiRaw = max(ui.rgb - blackFloor * transmittance, 0.0);
    // Combine linear transmittance and backdrop darkening into scalar premultiplied alpha
    // so C_scene * (1 - uiAlpha) + uiRaw preserves dark cockpit panels.
    float multLuma = saturate(dot(mult, float3(0.2126, 0.7152, 0.0722)));
    float uiAlpha = 1.0 - saturate(transmittance * multLuma);
    uPackedUi[id.xy] = float4(saturate(uiRaw), saturate(uiAlpha));
}
)";

} // namespace

bool FgUiPackPass::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
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
    if (FAILED(device1->CreateDeviceContextState(
            flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &state_))) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> csBlob;
    if (FAILED(D3DCompile(kFgUiPackCs, sizeof(kFgUiPackCs) - 1, nullptr, nullptr, nullptr,
                          "cs_main", "cs_5_0", 0, 0, &csBlob, nullptr)) ||
        FAILED(device->CreateComputeShader(
            csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &compute_))) {
        return false;
    }

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(PackParams);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &constants_))) {
        return false;
    }

    device_ = device;
    return true;
}

void FgUiPackPass::shutdown() {
    outputUav_.Reset();
    outputSrv_.Reset();
    outputTex_.Reset();
    constants_.Reset();
    compute_.Reset();
    state_.Reset();
    context_.Reset();
    device_.Reset();
    width_ = 0;
    height_ = 0;
}

bool FgUiPackPass::ensureOutput(uint32_t width, uint32_t height) {
    if (!device_ || !width || !height) return false;
    if (outputTex_ && width_ == width && height_ == height) return true;

    outputUav_.Reset();
    outputSrv_.Reset();
    outputTex_.Reset();
    width_ = 0;
    height_ = 0;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &outputTex_)) ||
        FAILED(device_->CreateShaderResourceView(outputTex_.Get(), nullptr, &outputSrv_)) ||
        FAILED(device_->CreateUnorderedAccessView(outputTex_.Get(), nullptr, &outputUav_))) {
        outputUav_.Reset();
        outputSrv_.Reset();
        outputTex_.Reset();
        return false;
    }

    SetD3D11DebugName(outputTex_.Get(), "EDPE_FgPackedUi");
    width_ = width;
    height_ = height;
    return true;
}

bool FgUiPackPass::pack(ID3D11ShaderResourceView* hdrUiSrv,
                        ID3D11ShaderResourceView* ldrUiSrv,
                        ID3D11ShaderResourceView* multUiSrv,
                        bool hasMultiply,
                        uint32_t width,
                        uint32_t height,
                        ID3D11Texture2D* externalTargetTex,
                        ID3D11UnorderedAccessView* externalTargetUav) {
    if (!device_ || !context_ || !state_ || !compute_ || !constants_ ||
        !hdrUiSrv || !ldrUiSrv || !width || !height) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> tempUav;
    ID3D11UnorderedAccessView* targetUav = externalTargetUav;
    if (!targetUav) {
        if (externalTargetTex) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            if (FAILED(device_->CreateUnorderedAccessView(externalTargetTex, &uavDesc, &tempUav))) {
                return false;
            }
            targetUav = tempUav.Get();
        } else {
            if (!ensureOutput(width, height)) return false;
            targetUav = outputUav_.Get();
        }
    }

    const PackParams params{{width, height}, (hasMultiply && multUiSrv) ? 1u : 0u, 0u};
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> priorState;
    context_->SwapDeviceContextState(state_.Get(), &priorState);
    {
        ScopedRenderDocEvent rdocEvent(context_.Get(), L"EDPE: FgUiPack");
        context_->UpdateSubresource(constants_.Get(), 0, nullptr, &params, 0, 0);
        ID3D11Buffer* cbs[] = {constants_.Get()};
        ID3D11ShaderResourceView* srvs[] = {ldrUiSrv, multUiSrv, hdrUiSrv};
        ID3D11UnorderedAccessView* uavs[] = {targetUav};
        context_->CSSetShader(compute_.Get(), nullptr, 0);
        context_->CSSetConstantBuffers(0, 1, cbs);
        context_->CSSetShaderResources(0, 3, srvs);
        context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        context_->Dispatch((width + 31) / 32, (height + 7) / 8, 1);

        ID3D11UnorderedAccessView* nullUav[] = {nullptr};
        ID3D11ShaderResourceView* nullSrvs[3]{};
        context_->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
        context_->CSSetShaderResources(0, 3, nullSrvs);
    }
    context_->SwapDeviceContextState(priorState.Get(), nullptr);
    return true;
}

} // namespace edpe
