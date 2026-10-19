#include "jitter_debug_pass.h"
#include "jitter_debug_shader.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>

namespace edpe {

namespace {

struct JitterDebugConstantBufferData {
    float resolution[4];   // x: width, y: height, z: 1/width, w: 1/height
    float jitterParams[4]; // x: jitterPixelX, y: jitterPixelY, z: sensitivity, w: threshold
    float debugMode[4];    // x: mode, y: resetFlag, z: 0, w: 0
};

} // namespace

bool JitterDebugPass::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
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
    if (FAILED(D3DCompile(kJitterDebugShader, sizeof(kJitterDebugShader) - 1, nullptr, nullptr,
            nullptr, "vs_main", "vs_5_0", 0, 0, &vsBlob, &errBlob))) {
        return false;
    }
    if (FAILED(D3DCompile(kJitterDebugShader, sizeof(kJitterDebugShader) - 1, nullptr, nullptr,
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
    bufDesc.ByteWidth = sizeof(JitterDebugConstantBufferData);
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&bufDesc, nullptr, &constantBuffer_))) {
        return false;
    }

    device_ = device;
    resetRequested_ = true;
    return true;
}

void JitterDebugPass::shutdown() {
    state_.Reset();
    vertexShader_.Reset();
    pixelShader_.Reset();
    linearSampler_.Reset();
    pointSampler_.Reset();
    constantBuffer_.Reset();
    historyTexture_.Reset();
    historySrv_.Reset();
    context_.Reset();
    device_ = nullptr;
    historyWidth_ = 0;
    historyHeight_ = 0;
    historyFormat_ = DXGI_FORMAT_UNKNOWN;
    resetRequested_ = true;
}

bool JitterDebugPass::ensureHistory(uint32_t width, uint32_t height, DXGI_FORMAT format) {
    if (!device_ || width == 0 || height == 0) return false;

    if (historyTexture_) {
        if (historyWidth_ != width || historyHeight_ != height || historyFormat_ != format) {
            historyTexture_.Reset();
            historySrv_.Reset();
            historyWidth_ = 0;
            historyHeight_ = 0;
            historyFormat_ = DXGI_FORMAT_UNKNOWN;
            resetRequested_ = true;
        }
    }

    if (!historyTexture_) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = format;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = 0;
        td.MiscFlags = 0;

        if (FAILED(device_->CreateTexture2D(&td, nullptr, &historyTexture_))) {
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MostDetailedMip = 0;
        srvDesc.Texture2D.MipLevels = 1;

        if (FAILED(device_->CreateShaderResourceView(historyTexture_.Get(), &srvDesc, &historySrv_))) {
            historyTexture_.Reset();
            return false;
        }

        historyWidth_ = width;
        historyHeight_ = height;
        historyFormat_ = format;
        resetRequested_ = true;
    }

    return true;
}

bool JitterDebugPass::render(ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* inputColorSrv,
    ID3D11RenderTargetView* targetRtv,
    UINT width, UINT height,
    float jitterPixelX, float jitterPixelY,
    float sensitivity,
    float threshold,
    int mode,
    ID3D11ShaderResourceView* motionSrv) {
    if (!device_ || !context_ || !inputColorSrv || !targetRtv || width == 0 || height == 0) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Resource> colorRes;
    inputColorSrv->GetResource(&colorRes);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> colorTex;
    DXGI_FORMAT colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (colorRes && SUCCEEDED(colorRes.As(&colorTex))) {
        D3D11_TEXTURE2D_DESC cd{};
        colorTex->GetDesc(&cd);
        colorFormat = cd.Format;
    }

    ensureHistory(width, height, colorFormat);

    JitterDebugConstantBufferData cbData{};
    cbData.resolution[0] = static_cast<float>(width);
    cbData.resolution[1] = static_cast<float>(height);
    cbData.resolution[2] = 1.0f / static_cast<float>(width);
    cbData.resolution[3] = 1.0f / static_cast<float>(height);
    cbData.jitterParams[0] = jitterPixelX;
    cbData.jitterParams[1] = jitterPixelY;
    cbData.jitterParams[2] = sensitivity;
    cbData.jitterParams[3] = threshold;
    cbData.debugMode[0] = static_cast<float>(mode);
    cbData.debugMode[1] = resetRequested_ ? 1.0f : 0.0f;

    context_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &cbData, 0, 0);

    Microsoft::WRL::ComPtr<ID3DDeviceContextState> prior;
    if (state_) {
        context_->SwapDeviceContextState(state_.Get(), &prior);
    }

    ID3D11RenderTargetView* rtv = targetRtv;
    ID3D11ShaderResourceView* srvs[3] = { inputColorSrv, historySrv_.Get(), motionSrv };
    ID3D11SamplerState* samplers[2] = { linearSampler_.Get(), pointSampler_.Get() };
    ID3D11Buffer* cb = constantBuffer_.Get();

    const D3D11_VIEWPORT viewport{
        0.0f, 0.0f,
        static_cast<float>(width), static_cast<float>(height),
        0.0f, 1.0f
    };

    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 3, srvs);
    context_->PSSetSamplers(0, 2, samplers);
    context_->PSSetConstantBuffers(0, 1, &cb);

    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrvs[3] = { nullptr, nullptr, nullptr };
    context_->PSSetShaderResources(0, 3, nullSrvs);
    ID3D11RenderTargetView* nullRtv = nullptr;
    context_->OMSetRenderTargets(1, &nullRtv, nullptr);

    if (state_ && prior) {
        context_->SwapDeviceContextState(prior.Get(), nullptr);
    }

    // Update history texture with current frame color for temporal pulse detection next frame
    if (historyTexture_ && colorTex) {
        context->CopyResource(historyTexture_.Get(), colorTex.Get());
        resetRequested_ = false;
    }

    return true;
}

} // namespace edpe
