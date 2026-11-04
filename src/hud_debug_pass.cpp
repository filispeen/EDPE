#include "hud_debug_pass.h"
#include "hud_debug_shader.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>

namespace edpe {

namespace {

struct HudDebugConstantBufferData {
    float resolution[4];       // x: width, y: height, z: 1/width, w: 1/height
    float outputResolution[4]; // x: outputWidth, y: outputHeight, z: 1/outputWidth, w: 1/outputHeight
    float debugParams[4];      // x: opacity, y: threshold, z: mode, w: isHdr
};

} // namespace

bool HudDebugPass::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
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
    if (FAILED(D3DCompile(kHudDebugShader, sizeof(kHudDebugShader) - 1, nullptr, nullptr,
            nullptr, "vs_main", "vs_5_0", 0, 0, &vsBlob, &errBlob))) {
        return false;
    }
    if (FAILED(D3DCompile(kHudDebugShader, sizeof(kHudDebugShader) - 1, nullptr, nullptr,
            nullptr, "ps_main", "ps_5_0", 0, 0, &psBlob, &errBlob))) {
        return false;
    }

    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(),
            vsBlob->GetBufferSize(), nullptr, &vertexShader_)) ||
        FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(),
            psBlob->GetBufferSize(), nullptr, &pixelShader_))) {
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

    D3D11_BUFFER_DESC bufDesc{};
    bufDesc.ByteWidth = sizeof(HudDebugConstantBufferData);
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&bufDesc, nullptr, &constantBuffer_))) {
        return false;
    }

    // 1x1 black fallback textures and SRVs
    D3D11_TEXTURE2D_DESC dummyDesc{};
    dummyDesc.Width = dummyDesc.Height = 1;
    dummyDesc.MipLevels = dummyDesc.ArraySize = 1;
    dummyDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dummyDesc.SampleDesc.Count = 1;
    dummyDesc.Usage = D3D11_USAGE_DEFAULT;
    dummyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const uint8_t black[4] = {0, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA initData{&black, sizeof(black), 0};
    if (FAILED(device->CreateTexture2D(&dummyDesc, &initData, &dummyTexture_)) ||
        FAILED(device->CreateShaderResourceView(dummyTexture_.Get(), nullptr, &dummySrv_))) {
        return false;
    }

    dummyDesc.Format = DXGI_FORMAT_R8_UNORM;
    const uint8_t zero = 0;
    D3D11_SUBRESOURCE_DATA maskInit{&zero, sizeof(zero), 0};
    if (FAILED(device->CreateTexture2D(&dummyDesc, &maskInit, &dummyMaskTexture_)) ||
        FAILED(device->CreateShaderResourceView(dummyMaskTexture_.Get(), nullptr, &dummyMaskSrv_))) {
        return false;
    }

    device_ = device;
    return true;
}

void HudDebugPass::shutdown() {
    state_.Reset();
    vertexShader_.Reset();
    pixelShader_.Reset();
    linearSampler_.Reset();
    pointSampler_.Reset();
    constantBuffer_.Reset();
    dummyTexture_.Reset();
    dummySrv_.Reset();
    dummyMaskTexture_.Reset();
    dummyMaskSrv_.Reset();
    context_.Reset();
    device_ = nullptr;
}

bool HudDebugPass::render(ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* currentFrameSrv,
    ID3D11ShaderResourceView* hudMaskSrv,
    ID3D11ShaderResourceView* lowResWorldSrv,
    ID3D11ShaderResourceView* gameBackbufferSrv,
    ID3D11RenderTargetView* targetRtv,
    UINT outputWidth, UINT outputHeight,
    UINT renderWidth, UINT renderHeight,
    float opacity,
    float threshold,
    int mode,
    bool isHdr) {
    if (!device_ || !context_ || !targetRtv || outputWidth == 0 || outputHeight == 0) {
        return false;
    }

    const UINT inWidth = renderWidth ? renderWidth : outputWidth;
    const UINT inHeight = renderHeight ? renderHeight : outputHeight;

    lastFrameInfo_.outputW = outputWidth;
    lastFrameInfo_.outputH = outputHeight;
    lastFrameInfo_.renderW = inWidth;
    lastFrameInfo_.renderH = inHeight;
    lastFrameInfo_.mode = mode;

    HudDebugConstantBufferData cbData{};
    cbData.resolution[0] = static_cast<float>(inWidth);
    cbData.resolution[1] = static_cast<float>(inHeight);
    cbData.resolution[2] = 1.0f / static_cast<float>(inWidth);
    cbData.resolution[3] = 1.0f / static_cast<float>(inHeight);
    cbData.outputResolution[0] = static_cast<float>(outputWidth);
    cbData.outputResolution[1] = static_cast<float>(outputHeight);
    cbData.outputResolution[2] = 1.0f / static_cast<float>(outputWidth);
    cbData.outputResolution[3] = 1.0f / static_cast<float>(outputHeight);
    cbData.debugParams[0] = opacity;
    cbData.debugParams[1] = threshold;
    cbData.debugParams[2] = static_cast<float>(mode);
    cbData.debugParams[3] = isHdr ? 1.0f : 0.0f;

    context_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &cbData, 0, 0);

    Microsoft::WRL::ComPtr<ID3DDeviceContextState> prior;
    if (state_) {
        context_->SwapDeviceContextState(state_.Get(), &prior);
    }

    ID3D11RenderTargetView* rtv = targetRtv;
    ID3D11ShaderResourceView* srvs[4] = {
        currentFrameSrv ? currentFrameSrv : dummySrv_.Get(),
        hudMaskSrv ? hudMaskSrv : dummyMaskSrv_.Get(),
        lowResWorldSrv ? lowResWorldSrv : dummySrv_.Get(),
        gameBackbufferSrv ? gameBackbufferSrv : (currentFrameSrv ? currentFrameSrv : dummySrv_.Get())
    };
    ID3D11SamplerState* samplers[2] = { linearSampler_.Get(), pointSampler_.Get() };
    ID3D11Buffer* cb = constantBuffer_.Get();

    const D3D11_VIEWPORT viewport{
        0.0f, 0.0f,
        static_cast<float>(outputWidth), static_cast<float>(outputHeight),
        0.0f, 1.0f
    };

    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 4, srvs);
    context_->PSSetSamplers(0, 2, samplers);
    context_->PSSetConstantBuffers(0, 1, &cb);

    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrvs[4] = { nullptr, nullptr, nullptr, nullptr };
    context_->PSSetShaderResources(0, 4, nullSrvs);
    ID3D11RenderTargetView* nullRtv = nullptr;
    context_->OMSetRenderTargets(1, &nullRtv, nullptr);

    if (state_) {
        context_->SwapDeviceContextState(prior.Get(), nullptr);
    }

    return true;
}

} // namespace edpe
