#include "motion_class_debug_pass.h"
#include "motion_class_debug_shader.h"

#include <d3dcompiler.h>
#include <cstring>

namespace edpe {

bool MotionClassDebugPass::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context || device_ ||
        device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0 ||
        context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;

    Microsoft::WRL::ComPtr<ID3D11Device1> device1;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))) ||
        FAILED(context->QueryInterface(IID_PPV_ARGS(&context_)))) return false;

    const auto level = device->GetFeatureLevel();
    const UINT flags = device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED
        ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    if (FAILED(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION,
            __uuidof(ID3D11Device1), nullptr, &state_))) return false;

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, csBlob;
    if (FAILED(D3DCompile(kMotionClassDebugShader, sizeof(kMotionClassDebugShader) - 1,
            nullptr, nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vsBlob, nullptr)) ||
        FAILED(D3DCompile(kMotionClassDebugShader, sizeof(kMotionClassDebugShader) - 1,
            nullptr, nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &psBlob, nullptr)) ||
        FAILED(D3DCompile(kMotionClassHistogramShader, sizeof(kMotionClassHistogramShader) - 1,
            nullptr, nullptr, nullptr, "cs_main", "cs_5_0", 0, 0, &csBlob, nullptr)) ||
        FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
            nullptr, &vertexShader_)) ||
        FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
            nullptr, &pixelShader_)) ||
        FAILED(device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(),
            nullptr, &histShader_))) {
        shutdown();
        return false;
    }

    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device->CreateSamplerState(&samp, &linearSampler_))) {
        shutdown();
        return false;
    }

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = 16;
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &constants_)) ||
        FAILED(device->CreateBuffer(&cbDesc, nullptr, &histConstants_))) {
        shutdown();
        return false;
    }

    D3D11_BUFFER_DESC histDesc{};
    histDesc.ByteWidth = 8 * sizeof(uint32_t);
    histDesc.Usage = D3D11_USAGE_DEFAULT;
    histDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    histDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    histDesc.StructureByteStride = sizeof(uint32_t);
    if (FAILED(device->CreateBuffer(&histDesc, nullptr, &histBuffer_))) {
        shutdown();
        return false;
    }
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = 8;
    if (FAILED(device->CreateUnorderedAccessView(histBuffer_.Get(), &uavDesc, &histUav_))) {
        shutdown();
        return false;
    }

    D3D11_BUFFER_DESC stageDesc{};
    stageDesc.ByteWidth = 8 * sizeof(uint32_t);
    stageDesc.Usage = D3D11_USAGE_STAGING;
    stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; i < 2; ++i) {
        if (FAILED(device->CreateBuffer(&stageDesc, nullptr, &histStaging_[i]))) {
            shutdown();
            return false;
        }
    }

    device_ = device;
    return true;
}

void MotionClassDebugPass::shutdown() {
    histStaging_[0].Reset();
    histStaging_[1].Reset();
    histUav_.Reset();
    histBuffer_.Reset();
    histConstants_.Reset();
    constants_.Reset();
    linearSampler_.Reset();
    histShader_.Reset();
    pixelShader_.Reset();
    vertexShader_.Reset();
    state_.Reset();
    context_.Reset();
    device_.Reset();
    histStagingFilled_[0] = histStagingFilled_[1] = false;
    lastStats_ = {};
}

bool MotionClassDebugPass::render(ID3D11DeviceContext* context,
                                  ID3D11ShaderResourceView* inputColorSrv,
                                  ID3D11ShaderResourceView* classMapSrv,
                                  ID3D11RenderTargetView* targetRtv,
                                  UINT outputWidth, UINT outputHeight,
                                  UINT classWidth, UINT classHeight,
                                  float opacity, bool isHdr) {
    if (!device_ || !context || !inputColorSrv || !classMapSrv || !targetRtv ||
        !outputWidth || !outputHeight || !classWidth || !classHeight) return false;

    Microsoft::WRL::ComPtr<ID3DDeviceContextState> prior;
    context_->SwapDeviceContextState(state_.Get(), &prior);

    // 1. Update async histogram (non-blocking DO_NOT_WAIT readback from previous slot).
    const uint32_t readSlot = histSlot_ ^ 1u;
    if (histStagingFilled_[readSlot]) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(histStaging_[readSlot].Get(), 0, D3D11_MAP_READ,
                D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) {
            uint32_t bins[8]{};
            std::memcpy(bins, mapped.pData, sizeof(bins));
            context_->Unmap(histStaging_[readSlot].Get(), 0);
            uint32_t total = 0;
            for (int i = 0; i < 8; ++i) {
                lastStats_.counts[i] = bins[i];
                total += bins[i];
            }
            lastStats_.totalSamples = total;
            lastStats_.valid = (total > 0);
        }
    }

    const UINT zero[4] = {0u, 0u, 0u, 0u};
    context_->ClearUnorderedAccessViewUint(histUav_.Get(), zero);
    const UINT stride = 4u;
    const uint32_t histCb[4] = {classWidth, classHeight, stride, 0u};
    context_->UpdateSubresource(histConstants_.Get(), 0, nullptr, histCb, 0, 0);
    ID3D11Buffer* hcb = histConstants_.Get();
    ID3D11UnorderedAccessView* huav = histUav_.Get();
    context_->CSSetShader(histShader_.Get(), nullptr, 0);
    context_->CSSetConstantBuffers(0, 1, &hcb);
    context_->CSSetShaderResources(0, 1, &classMapSrv);
    context_->CSSetUnorderedAccessViews(0, 1, &huav, nullptr);
    const UINT gx = ((classWidth + stride - 1u) / stride + 7u) / 8u;
    const UINT gy = ((classHeight + stride - 1u) / stride + 7u) / 8u;
    context_->Dispatch(gx, gy, 1u);
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context_->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    context_->CSSetShaderResources(0, 1, &nullSrv);
    context_->CSSetShader(nullptr, nullptr, 0);
    context_->CopyResource(histStaging_[histSlot_].Get(), histBuffer_.Get());
    histStagingFilled_[histSlot_] = true;
    histSlot_ ^= 1u;

    // 2. Render color-coded OutClass overlay onto targetRtv.
    const float cb[4] = {
        static_cast<float>(classWidth),
        static_cast<float>(classHeight),
        opacity,
        isHdr ? 1.0f : 0.0f
    };
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, cb, 0, 0);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(outputWidth),
        static_cast<float>(outputHeight), 0.0f, 1.0f};
    ID3D11ShaderResourceView* srvs[2] = {inputColorSrv, classMapSrv};
    ID3D11SamplerState* samp = linearSampler_.Get();
    ID3D11Buffer* psCb = constants_.Get();
    context_->OMSetRenderTargets(1, &targetRtv, nullptr);
    context_->RSSetViewports(1, &vp);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, &psCb);
    context_->PSSetShaderResources(0, 2, srvs);
    context_->PSSetSamplers(0, 1, &samp);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* emptySrvs[2] = {nullptr, nullptr};
    context_->PSSetShaderResources(0, 2, emptySrvs);
    context_->OMSetRenderTargets(0, nullptr, nullptr);

    context_->SwapDeviceContextState(prior.Get(), nullptr);
    return true;
}

} // namespace edpe
