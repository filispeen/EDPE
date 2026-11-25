#include "taa_pass.h"
#include "taa_shader.h"

#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>

namespace edpe {

namespace {

struct TaaConstantBufferData {
    float resolution[4];       // outputWidth, outputHeight, 1/outputWidth, 1/outputHeight
    float renderResolution[4]; // renderWidth, renderHeight, 1/renderWidth, 1/renderHeight
    float params[4];           // jitterX, jitterY, blendWeight, resetFlag
    float jitterHistory[4];    // prevJitterX, prevJitterY, depthB, hasPrevDepth
};

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
        // Fall back gracefully if device context state cannot be created
        state_.Reset();
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    if (FAILED(D3DCompile(kTaaShader, sizeof(kTaaShader) - 1, nullptr, nullptr,
            nullptr, "vs_main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errBlob))) {
        return false;
    }
    if (FAILED(D3DCompile(kTaaShader, sizeof(kTaaShader) - 1, nullptr, nullptr,
            nullptr, "ps_main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errBlob))) {
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
    bufDesc.ByteWidth = sizeof(TaaConstantBufferData);
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&bufDesc, nullptr, &constantBuffer_))) {
        return false;
    }

    device_ = device;
    resetRequested_ = true;
    return true;
}

void TaaPass::shutdown() {
    state_.Reset();
    vertexShader_.Reset();
    pixelShader_.Reset();
    linearSampler_.Reset();
    pointSampler_.Reset();
    constantBuffer_.Reset();
    for (int i = 0; i < 2; ++i) {
        historyTexture_[i].Reset();
        historySrv_[i].Reset();
        historyRtv_[i].Reset();
    }
    historyReadIdx_ = 0;
    cachedDestRtv_.Reset();
    cachedDestTexture_ = nullptr;
    context_.Reset();
    device_ = nullptr;
    historyWidth_ = 0;
    historyHeight_ = 0;
    historyFormat_ = DXGI_FORMAT_UNKNOWN;
    resetRequested_ = true;
}

bool TaaPass::ensureResources(uint32_t width, uint32_t height, DXGI_FORMAT format) {
    if (!device_ || width == 0 || height == 0 || format == DXGI_FORMAT_UNKNOWN) return false;

    if (historyTexture_[0] || historyTexture_[1]) {
        if (historyWidth_ != width || historyHeight_ != height || historyFormat_ != format) {
            for (int i = 0; i < 2; ++i) {
                historyTexture_[i].Reset();
                historySrv_[i].Reset();
                historyRtv_[i].Reset();
            }
            historyReadIdx_ = 0;
            historyWidth_ = 0;
            historyHeight_ = 0;
            historyFormat_ = DXGI_FORMAT_UNKNOWN;
            resetRequested_ = true;
        }
    }

    if (!historyTexture_[0] || !historyTexture_[1]) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = format;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

        for (int i = 0; i < 2; ++i) {
            if (FAILED(device_->CreateTexture2D(&td, nullptr, &historyTexture_[i])) ||
                FAILED(device_->CreateShaderResourceView(historyTexture_[i].Get(), nullptr, &historySrv_[i])) ||
                FAILED(device_->CreateRenderTargetView(historyTexture_[i].Get(), nullptr, &historyRtv_[i]))) {
                for (int j = 0; j < 2; ++j) {
                    historyTexture_[j].Reset();
                    historySrv_[j].Reset();
                    historyRtv_[j].Reset();
                }
                return false;
            }
        }

        historyReadIdx_ = 0;
        historyWidth_ = width;
        historyHeight_ = height;
        historyFormat_ = format;
        resetRequested_ = true;
    }

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
    DXGI_FORMAT /*preferredFormat*/) {
    if (!device_ || !context || !currentFrameSrv || width == 0 || height == 0) {
        return false;
    }

    const uint32_t rW = (renderWidth > 0) ? renderWidth : width;
    const uint32_t rH = (renderHeight > 0) ? renderHeight : height;

    // Always allocate fp16 history buffers for both LDR and HDR so 8-bit UNORM
    // quantization never stalls temporal convergence at high blend weights or bands dark space.
    if (!ensureResources(width, height, kHistoryFormat)) {
        return false;
    }

    const uint32_t readIdx = historyReadIdx_;
    const uint32_t writeIdx = readIdx ^ 1u;
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

    TaaConstantBufferData cb{};
    cb.resolution[0] = static_cast<float>(width);
    cb.resolution[1] = static_cast<float>(height);
    cb.resolution[2] = 1.0f / static_cast<float>(width);
    cb.resolution[3] = 1.0f / static_cast<float>(height);
    cb.renderResolution[0] = static_cast<float>(rW);
    cb.renderResolution[1] = static_cast<float>(rH);
    cb.renderResolution[2] = 1.0f / static_cast<float>(rW);
    cb.renderResolution[3] = 1.0f / static_cast<float>(rH);
    cb.params[0] = jitterX;
    cb.params[1] = jitterY;
    cb.params[2] = 0.90f; // 90% history weight
    cb.params[3] = performReset ? 1.0f : 0.0f;
    cb.jitterHistory[0] = prevJitterX;
    cb.jitterHistory[1] = prevJitterY;
    cb.jitterHistory[2] = (depthB > 0.0f) ? depthB : 0.025f;
    cb.jitterHistory[3] = (!performReset && prevDepthSrv && depthSrv) ? 1.0f : 0.0f;

    context->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &cb, 0, 0);

    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    context->RSSetViewports(1, &vp);
    context->RSSetState(nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(nullptr, 0);
    if (zeroCopyPingPong) {
        ID3D11RenderTargetView* rtv = historyRtv_[writeIdx].Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
    } else {
        // Write fp16 history (SV_Target0) and hardware-converted destination (SV_Target1) in one pass.
        ID3D11RenderTargetView* rtvs[2] = { historyRtv_[writeIdx].Get(), targetRtv };
        context->OMSetRenderTargets(2, rtvs, nullptr);
    }
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context->GSSetShader(nullptr, nullptr, 0);

    ID3D11ShaderResourceView* srvs[5] = {
        currentFrameSrv,
        historySrv_[readIdx].Get(),
        motionVectorsSrv,
        depthSrv,
        (!performReset) ? prevDepthSrv : nullptr
    };
    context->PSSetShaderResources(0, 5, srvs);

    ID3D11SamplerState* samplers[2] = { linearSampler_.Get(), pointSampler_.Get() };
    context->PSSetSamplers(0, 2, samplers);

    ID3D11Buffer* cbs[1] = { constantBuffer_.Get() };
    context->PSSetConstantBuffers(0, 1, cbs);

    context->Draw(3, 0);

    // Unbind resources before next use to prevent pipeline hazards
    ID3D11ShaderResourceView* nullSrvs[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
    context->PSSetShaderResources(0, 5, nullSrvs);
    ID3D11RenderTargetView* nullRtvs[2] = { nullptr, nullptr };
    context->OMSetRenderTargets(2, nullRtvs, nullptr);

    historyReadIdx_ = writeIdx;

    return true;
}

} // namespace edpe
