#include "motion_pass.h"
#include "motion_shader.h"

#include <d3dcompiler.h>
#include <cmath>

namespace edpe {

bool MotionPass::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
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

    UINT format_support = 0;
    if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_R16G16_FLOAT, &format_support)) ||
        !(format_support & D3D11_FORMAT_SUPPORT_RENDER_TARGET) ||
        !(format_support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) return false;
    Microsoft::WRL::ComPtr<ID3DBlob> vertex_code, pixel_code;
    if (FAILED(D3DCompile(kMotionShader, sizeof(kMotionShader) - 1, nullptr, nullptr,
            nullptr, "vs", "vs_5_0", 0, 0, &vertex_code, nullptr)) ||
        FAILED(D3DCompile(kMotionShader, sizeof(kMotionShader) - 1, nullptr, nullptr,
            nullptr, "ps", "ps_5_0", 0, 0, &pixel_code, nullptr)) ||
        FAILED(device->CreateVertexShader(vertex_code->GetBufferPointer(),
            vertex_code->GetBufferSize(), nullptr, &vertex_)) ||
        FAILED(device->CreatePixelShader(pixel_code->GetBufferPointer(),
            pixel_code->GetBufferSize(), nullptr, &pixel_))) return false;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 32 * sizeof(float);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &constants_))) return false;
    device_ = device;
    return true;
}

bool MotionPass::render(ID3D11ShaderResourceView* depth, const CameraProjection& now,
    const CameraProjection& previous, UINT width, UINT height) {
    if (!device_ || !depth || !width || !height ||
        !(now.scaleX > 0 && now.scaleY > 0 && now.depthB > 0 &&
          previous.scaleX > 0 && previous.scaleY > 0) ||
        !std::isfinite(now.scaleX) || !std::isfinite(now.scaleY) ||
        !std::isfinite(now.depthB) || !std::isfinite(previous.scaleX) ||
        !std::isfinite(previous.scaleY)) return false;
    for (float value : now.worldFromView) if (!std::isfinite(value)) return false;
    for (float value : previous.worldFromView) if (!std::isfinite(value)) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    depth->GetDesc(&srv_desc);
    if (srv_desc.Format != DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ||
        srv_desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) return false;
    Microsoft::WRL::ComPtr<ID3D11Resource> resource;
    depth->GetResource(&resource);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) return false;
    Microsoft::WRL::ComPtr<ID3D11Device> depth_device;
    texture->GetDevice(&depth_device);
    if (depth_device.Get() != device_.Get()) return false;
    D3D11_TEXTURE2D_DESC depth_desc{};
    texture->GetDesc(&depth_desc);
    if (depth_desc.Width != width || depth_desc.Height != height ||
        depth_desc.SampleDesc.Count != 1 || depth_desc.MipLevels != 1 ||
        depth_desc.ArraySize != 1 ||
        depth_desc.Format != DXGI_FORMAT_R32G8X24_TYPELESS) return false;
    ID3D11DepthStencilView* bound_dsv = nullptr;
    context_->OMGetRenderTargets(0, nullptr, &bound_dsv);
    if (bound_dsv) { bound_dsv->Release(); return false; }

    if (!output_texture_ || width != width_ || height != height_) {
        D3D11_TEXTURE2D_DESC output_desc{};
        output_desc.Width = width;
        output_desc.Height = height;
        output_desc.MipLevels = output_desc.ArraySize = 1;
        output_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
        output_desc.SampleDesc.Count = 1;
        output_desc.Usage = D3D11_USAGE_DEFAULT;
        output_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> output;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        if (FAILED(device_->CreateTexture2D(&output_desc, nullptr, &output)) ||
            FAILED(device_->CreateRenderTargetView(output.Get(), nullptr, &rtv)) ||
            FAILED(device_->CreateShaderResourceView(output.Get(), nullptr, &srv))) return false;
        output_texture_ = output;
        output_rtv_ = rtv;
        output_srv_ = srv;
        width_ = width;
        height_ = height;
    }

    float values[32]{};
    for (unsigned i = 0; i < 12; ++i) {
        values[i] = now.worldFromView[i];
        values[12 + i] = previous.worldFromView[i];
    }
    values[24] = now.scaleX;
    values[25] = now.scaleY;
    values[26] = previous.scaleX;
    values[27] = previous.scaleY;
    values[28] = now.depthB;
    values[29] = static_cast<float>(width);
    values[30] = static_cast<float>(height);
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, values, 0, 0);

    Microsoft::WRL::ComPtr<ID3DDeviceContextState> prior;
    context_->SwapDeviceContextState(state_.Get(), &prior);
    auto* rtv = output_rtv_.Get();
    auto* srv = depth;
    auto* cb = constants_.Get();
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width),
        static_cast<float>(height), 0, 1};
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.Get(), nullptr, 0);
    context_->PSSetShader(pixel_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 1, &srv);
    context_->PSSetConstantBuffers(0, 1, &cb);
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* empty_srv = nullptr;
    context_->PSSetShaderResources(0, 1, &empty_srv);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    context_->SwapDeviceContextState(prior.Get(), nullptr);
    return true;
}

} // namespace edpe
