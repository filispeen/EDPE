#include "reactive_mask_pass.h"
#include "reactive_mask_shader.h"

#include <d3dcompiler.h>

namespace edpe {

namespace {

struct ReactiveParams {
    float emissiveWeight;
    float hudWeight;
    float emissiveThreshold;
    float hudThreshold;
    float hasEmissive;
    float hasHud;
    float renderWidth;
    float renderHeight;
};

} // namespace

bool ReactiveMaskPass::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
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
    if (FAILED(D3DCompile(kReactiveMaskShader, sizeof(kReactiveMaskShader) - 1, nullptr, nullptr,
            nullptr, "vs_main", "vs_5_0", 0, 0, &vertexCode, nullptr)) ||
        FAILED(D3DCompile(kReactiveMaskShader, sizeof(kReactiveMaskShader) - 1, nullptr, nullptr,
            nullptr, "ps_main", "ps_5_0", 0, 0, &pixelCode, nullptr)) ||
        FAILED(device->CreateVertexShader(vertexCode->GetBufferPointer(),
            vertexCode->GetBufferSize(), nullptr, &vertexShader_)) ||
        FAILED(device->CreatePixelShader(pixelCode->GetBufferPointer(),
            pixelCode->GetBufferSize(), nullptr, &pixelShader_))) {
        return false;
    }

    D3D11_BUFFER_DESC bufDesc{};
    bufDesc.ByteWidth = sizeof(ReactiveParams);
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&bufDesc, nullptr, &constantBuffer_))) {
        return false;
    }

    // Compile Debug View Pixel Shader
    Microsoft::WRL::ComPtr<ID3DBlob> debugPsCode;
    if (SUCCEEDED(D3DCompile(kReactiveMaskDebugShader, sizeof(kReactiveMaskDebugShader) - 1, nullptr, nullptr,
            nullptr, "ps_main", "ps_5_0", 0, 0, &debugPsCode, nullptr))) {
        device->CreatePixelShader(debugPsCode->GetBufferPointer(),
            debugPsCode->GetBufferSize(), nullptr, &debugPixelShader_);
    }

    D3D11_BUFFER_DESC dbgBufDesc{};
    dbgBufDesc.ByteWidth = 16; // float4
    dbgBufDesc.Usage = D3D11_USAGE_DEFAULT;
    dbgBufDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    device->CreateBuffer(&dbgBufDesc, nullptr, &debugConstantBuffer_);

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sampDesc, &pointSampler_);

    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    device->CreateSamplerState(&sampDesc, &linearSampler_);

    device_ = device;
    return true;
}

void ReactiveMaskPass::shutdown() {
    outputTexture_.Reset();
    outputRtv_.Reset();
    outputSrv_.Reset();
    constantBuffer_.Reset();
    vertexShader_.Reset();
    pixelShader_.Reset();
    debugPixelShader_.Reset();
    debugConstantBuffer_.Reset();
    pointSampler_.Reset();
    linearSampler_.Reset();
    state_.Reset();
    context_.Reset();
    device_ = nullptr;
    width_ = 0;
    height_ = 0;
}

bool ReactiveMaskPass::ensureOutputTexture(ID3D11Device* device, uint32_t width, uint32_t height) {
    if (outputTexture_ && width_ == width && height_ == height) return true;

    outputTexture_.Reset();
    outputRtv_.Reset();
    outputSrv_.Reset();
    width_ = 0;
    height_ = 0;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(device->CreateTexture2D(&desc, nullptr, &outputTexture_))) return false;
    if (FAILED(device->CreateRenderTargetView(outputTexture_.Get(), nullptr, &outputRtv_))) {
        outputTexture_.Reset();
        return false;
    }
    if (FAILED(device->CreateShaderResourceView(outputTexture_.Get(), nullptr, &outputSrv_))) {
        outputRtv_.Reset();
        outputTexture_.Reset();
        return false;
    }

    width_ = width;
    height_ = height;
    return true;
}

bool ReactiveMaskPass::render(ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* emissiveSrv,
    ID3D11ShaderResourceView* preHudSrv,
    ID3D11ShaderResourceView* postHudSrv,
    uint32_t width, uint32_t height,
    float emissiveWeight, float hudWeight,
    float emissiveThreshold, float hudThreshold) {
    if (!context || !device_ || !context_ || !width || !height) return false;
    if (!ensureOutputTexture(device_, width, height)) return false;

    ReactiveParams params{};
    params.emissiveWeight = emissiveWeight;
    params.hudWeight = hudWeight;
    params.emissiveThreshold = emissiveThreshold;
    params.hudThreshold = hudThreshold;
    params.hasEmissive = emissiveSrv ? 1.0f : 0.0f;
    params.hasHud = (preHudSrv && postHudSrv) ? 1.0f : 0.0f;
    params.renderWidth = static_cast<float>(width);
    params.renderHeight = static_cast<float>(height);

    context_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &params, 0, 0);

    Microsoft::WRL::ComPtr<ID3DDeviceContextState> prior;
    context_->SwapDeviceContextState(state_.Get(), &prior);

    auto* rtv = outputRtv_.Get();
    ID3D11ShaderResourceView* srvs[3] = {emissiveSrv, preHudSrv, postHudSrv};
    ID3D11Buffer* cb = constantBuffer_.Get();

    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};

    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 3, srvs);
    context_->PSSetConstantBuffers(0, 1, &cb);

    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrvs[3] = {nullptr, nullptr, nullptr};
    context_->PSSetShaderResources(0, 3, nullSrvs);
    ID3D11RenderTargetView* nullRtv = nullptr;
    context_->OMSetRenderTargets(1, &nullRtv, nullptr);

    context_->SwapDeviceContextState(prior.Get(), nullptr);
    return true;
}

bool ReactiveMaskPass::renderDebugView(ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* sceneColorSrv,
    ID3D11RenderTargetView* targetRtv,
    uint32_t width, uint32_t height,
    int mode, float boost, bool isHdr) {
    if (!context || !device_ || !context_ || !targetRtv || !outputSrv_ || !width || !height || !debugPixelShader_) {
        return false;
    }

    struct DebugParams {
        float mode;
        float boost;
        float isHdr;
        float pad;
    } params{};
    params.mode = static_cast<float>(mode);
    params.boost = boost;
    params.isHdr = isHdr ? 1.0f : 0.0f;

    context_->UpdateSubresource(debugConstantBuffer_.Get(), 0, nullptr, &params, 0, 0);

    Microsoft::WRL::ComPtr<ID3DDeviceContextState> prior;
    context_->SwapDeviceContextState(state_.Get(), &prior);

    auto* rtv = targetRtv;
    ID3D11ShaderResourceView* srvs[2] = {outputSrv_.Get(), sceneColorSrv};
    ID3D11Buffer* cb = debugConstantBuffer_.Get();
    ID3D11SamplerState* samplers[2] = {pointSampler_.Get(), linearSampler_.Get()};

    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};

    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(debugPixelShader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 2, srvs);
    context_->PSSetSamplers(0, 2, samplers);
    context_->PSSetConstantBuffers(0, 1, &cb);

    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrvs[2] = {nullptr, nullptr};
    context_->PSSetShaderResources(0, 2, nullSrvs);
    ID3D11RenderTargetView* nullRtv = nullptr;
    context_->OMSetRenderTargets(1, &nullRtv, nullptr);

    context_->SwapDeviceContextState(prior.Get(), nullptr);
    return true;
}

void ReactiveMaskPass::clear(ID3D11DeviceContext* context) {
    if (context && outputRtv_) {
        const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        context->ClearRenderTargetView(outputRtv_.Get(), zero);
    }
}

} // namespace edpe
