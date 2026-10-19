#include "ui_composite_pass.h"
#include "ui_composite_shader.h"

#include <d3dcompiler.h>

namespace edpe {

namespace {

struct CompositeParams {
    float renderSize[2];
    float renderTexel[2];
    float outputSize[2];
    float outputTexel[2];
    float flags[4]; // x: isHdr (1.0f = HDR, 0.0f = LDR)
};

} // namespace

bool UiCompositePass::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
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
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vertexCode, pixelCode;
    if (FAILED(D3DCompile(kUiCompositeShader, sizeof(kUiCompositeShader) - 1, nullptr, nullptr,
            nullptr, "vs_main", "vs_5_0", 0, 0, &vertexCode, nullptr)) ||
        FAILED(D3DCompile(kUiCompositeShader, sizeof(kUiCompositeShader) - 1, nullptr, nullptr,
            nullptr, "ps_main", "ps_5_0", 0, 0, &pixelCode, nullptr)) ||
        FAILED(device->CreateVertexShader(vertexCode->GetBufferPointer(),
            vertexCode->GetBufferSize(), nullptr, &vertexShader_)) ||
        FAILED(device->CreatePixelShader(pixelCode->GetBufferPointer(),
            pixelCode->GetBufferSize(), nullptr, &pixelShader_))) {
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

    D3D11_BUFFER_DESC bufDesc{};
    bufDesc.ByteWidth = sizeof(CompositeParams);
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&bufDesc, nullptr, &constantBuffer_))) {
        return false;
    }

    device_ = device;
    return true;
}

bool UiCompositePass::composite(ID3D11DeviceContext* /*context*/,
    ID3D11ShaderResourceView* dlssReconstruction,
    ID3D11ShaderResourceView* gameBackbufferWithUi,
    ID3D11ShaderResourceView* lowResWorld,
    ID3D11RenderTargetView* targetBackbufferRtv,
    UINT outputWidth, UINT outputHeight,
    UINT renderWidth, UINT renderHeight,
    bool isHdr) {
    if (!device_ || !context_ || !dlssReconstruction || !gameBackbufferWithUi ||
        !lowResWorld || !targetBackbufferRtv || !outputWidth || !outputHeight ||
        !renderWidth || !renderHeight) {
        return false;
    }

    CompositeParams params{};
    params.renderSize[0] = static_cast<float>(renderWidth);
    params.renderSize[1] = static_cast<float>(renderHeight);
    params.renderTexel[0] = 1.0f / static_cast<float>(renderWidth);
    params.renderTexel[1] = 1.0f / static_cast<float>(renderHeight);
    params.outputSize[0] = static_cast<float>(outputWidth);
    params.outputSize[1] = static_cast<float>(outputHeight);
    params.outputTexel[0] = 1.0f / static_cast<float>(outputWidth);
    params.outputTexel[1] = 1.0f / static_cast<float>(outputHeight);
    params.flags[0] = isHdr ? 1.0f : 0.0f;

    context_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &params, 0, 0);

    Microsoft::WRL::ComPtr<ID3DDeviceContextState> prior;
    context_->SwapDeviceContextState(state_.Get(), &prior);

    auto* rtv = targetBackbufferRtv;
    ID3D11ShaderResourceView* srvs[3] = {dlssReconstruction, gameBackbufferWithUi, lowResWorld};
    ID3D11Buffer* cb = constantBuffer_.Get();
    auto* sampler = linearSampler_.Get();

    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(outputWidth),
        static_cast<float>(outputHeight), 0.0f, 1.0f};

    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 3, srvs);
    context_->PSSetSamplers(0, 1, &sampler);
    context_->PSSetConstantBuffers(0, 1, &cb);

    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrvs[3] = {nullptr, nullptr, nullptr};
    context_->PSSetShaderResources(0, 3, nullSrvs);
    ID3D11RenderTargetView* nullRtv = nullptr;
    context_->OMSetRenderTargets(1, &nullRtv, nullptr);

    context_->SwapDeviceContextState(prior.Get(), nullptr);
    return true;
}

} // namespace edpe
