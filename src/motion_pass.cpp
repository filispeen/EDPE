#include "motion_pass.h"
#include "motion_shader.h"
#include "renderdoc_support.h"

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
    Microsoft::WRL::ComPtr<ID3DBlob> vertex_code, pixel_code, pixel_gpu_code;
    if (FAILED(D3DCompile(kMotionShader, sizeof(kMotionShader) - 1, nullptr, nullptr,
            nullptr, "vs", "vs_5_0", 0, 0, &vertex_code, nullptr)) ||
        FAILED(D3DCompile(kMotionShader, sizeof(kMotionShader) - 1, nullptr, nullptr,
            nullptr, "ps", "ps_5_0", 0, 0, &pixel_code, nullptr)) ||
        FAILED(D3DCompile(kMotionShader, sizeof(kMotionShader) - 1, nullptr, nullptr,
            nullptr, "ps_gpu", "ps_5_0", 0, 0, &pixel_gpu_code, nullptr)) ||
        FAILED(device->CreateVertexShader(vertex_code->GetBufferPointer(),
            vertex_code->GetBufferSize(), nullptr, &vertex_)) ||
        FAILED(device->CreatePixelShader(pixel_code->GetBufferPointer(),
            pixel_code->GetBufferSize(), nullptr, &pixel_)) ||
        FAILED(device->CreatePixelShader(pixel_gpu_code->GetBufferPointer(),
            pixel_gpu_code->GetBufferSize(), nullptr, &pixel_gpu_))) return false;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 36 * sizeof(float);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &constants_))) return false;
    device_ = device;
    return true;
}

bool MotionPass::render(ID3D11ShaderResourceView* depth, const CameraProjection& now,
    const CameraProjection& previous, UINT width, UINT height,
    float jitterX, float jitterY, float prevJitterX, float prevJitterY, bool reset,
    ID3D11ShaderResourceView* prevDepth,
    ID3D11ShaderResourceView* engineVelocity,
    bool preserveContextState) {
    if (!device_ || !depth || !width || !height ||
        !(now.scaleX > 0 && now.scaleY > 0 && now.depthB > 0 &&
          previous.scaleX > 0 && previous.scaleY > 0) ||
        !std::isfinite(now.scaleX) || !std::isfinite(now.scaleY) ||
        !std::isfinite(now.depthB) || !std::isfinite(previous.scaleX) ||
        !std::isfinite(previous.scaleY) ||
        !std::isfinite(jitterX) || !std::isfinite(jitterY) ||
        !std::isfinite(prevJitterX) || !std::isfinite(prevJitterY)) return false;
    for (float value : now.worldFromView) if (!std::isfinite(value)) return false;
    for (float value : previous.worldFromView) if (!std::isfinite(value)) return false;
    const int modeFlags =
        ((prevDepth && !reset) ? 1 : 0) |
        ((engineVelocity && !reset) ? 2 : 0);
    // Precompute relative view-space transform M_rel = R_prev^T * R_now,
    // t_rel = R_prev^T * (t_now - t_prev) once on CPU to halve per-pixel ALU.
    const float dx = now.worldFromView[3] - previous.worldFromView[3];
    const float dy = now.worldFromView[7] - previous.worldFromView[7];
    const float dz = now.worldFromView[11] - previous.worldFromView[11];
    float values[36]{};
    for (unsigned r = 0; r < 3; ++r) {
        const float p0 = previous.worldFromView[0 * 4 + r];
        const float p1 = previous.worldFromView[1 * 4 + r];
        const float p2 = previous.worldFromView[2 * 4 + r];
        for (unsigned c = 0; c < 3; ++c) {
            values[r * 4 + c] =
                p0 * now.worldFromView[0 * 4 + c] +
                p1 * now.worldFromView[1 * 4 + c] +
                p2 * now.worldFromView[2 * 4 + c];
        }
        values[r * 4 + 3] = p0 * dx + p1 * dy + p2 * dz;
    }
    const float wf = static_cast<float>(width);
    const float hf = static_cast<float>(height);
    values[12] = std::sqrt(dx * dx + dy * dy + dz * dz); // prev0.x = translationStep
    values[13] = (now.depthB > 0.0f) ? (now.depthB / 50000.0f) : 1e-6f; // prev0.y = celestialDepthThresh
    values[16] = 2.0f / wf;          // prev1.x
    values[17] = -2.0f / hf;         // prev1.y
    values[18] = 1.0f / now.scaleX;  // prev1.z
    values[19] = 1.0f / now.scaleY;  // prev1.w
    values[20] = previous.scaleX * (0.5f * wf);   // prev2.x
    values[21] = -previous.scaleY * (0.5f * hf);  // prev2.y
    values[22] = 0.5f * wf;                       // prev2.z
    values[23] = 0.5f * hf;                       // prev2.w
    values[24] = now.scaleX;
    values[25] = now.scaleY;
    values[26] = previous.scaleX;
    values[27] = previous.scaleY;
    values[28] = now.depthB;
    values[29] = wf;
    values[30] = hf;
    values[31] = static_cast<float>(modeFlags);
    values[32] = jitterX;
    values[33] = jitterY;
    values[34] = prevJitterX;
    values[35] = prevJitterY;
    return renderInternal(depth,
        (prevDepth && !reset) ? prevDepth : nullptr,
        (engineVelocity && !reset) ? engineVelocity : nullptr,
        values, nullptr, nullptr, width, height, preserveContextState);
}

bool MotionPass::renderGpuCameras(ID3D11ShaderResourceView* depth,
    ID3D11Buffer* now, ID3D11Buffer* previous, UINT width, UINT height,
    float jitterX, float jitterY, float prevJitterX, float prevJitterY, bool reset,
    ID3D11ShaderResourceView* prevDepth,
    ID3D11ShaderResourceView* engineVelocity) {
    if (!device_ || !now || !previous || !width || !height ||
        !std::isfinite(jitterX) || !std::isfinite(jitterY) ||
        !std::isfinite(prevJitterX) || !std::isfinite(prevJitterY)) return false;
    ID3D11Buffer* camera_buffers[] = {now, previous};
    for (auto* buffer : camera_buffers) {
        D3D11_BUFFER_DESC desc{};
        buffer->GetDesc(&desc);
        Microsoft::WRL::ComPtr<ID3D11Device> owner;
        buffer->GetDevice(&owner);
        if (owner.Get() != device_.Get() || desc.ByteWidth != 5376 ||
            !(desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return false;
    }
    const int modeFlags =
        ((prevDepth && !reset) ? 1 : 0) |
        ((engineVelocity && !reset) ? 2 : 0);
    float values[36]{};
    values[29] = static_cast<float>(width);
    values[30] = static_cast<float>(height);
    values[31] = static_cast<float>(modeFlags);
    values[32] = jitterX;
    values[33] = jitterY;
    values[34] = prevJitterX;
    values[35] = prevJitterY;
    return renderInternal(depth,
        (prevDepth && !reset) ? prevDepth : nullptr,
        (engineVelocity && !reset) ? engineVelocity : nullptr,
        values, now, previous, width, height, true);
}

bool MotionPass::renderInternal(ID3D11ShaderResourceView* depth,
    ID3D11ShaderResourceView* prevDepth,
    ID3D11ShaderResourceView* engineVelocity,
    const float (&values)[36], ID3D11Buffer* now, ID3D11Buffer* previous,
    UINT width, UINT height, bool preserveContextState) {
    if (!device_ || !depth || !width || !height) return false;
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
    if (preserveContextState) {
        ID3D11DepthStencilView* bound_dsv = nullptr;
        context_->OMGetRenderTargets(0, nullptr, &bound_dsv);
        if (bound_dsv) { bound_dsv->Release(); return false; }
    }

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
        SetD3D11DebugName(output_texture_.Get(), "EDPE_MotionVectors");
        width_ = width;
        height_ = height;
    }

    context_->UpdateSubresource(constants_.Get(), 0, nullptr, values, 0, 0);

    Microsoft::WRL::ComPtr<ID3DDeviceContextState> prior;
    if (preserveContextState) {
        context_->SwapDeviceContextState(state_.Get(), &prior);
    } else {
        static constexpr float kBlendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        context_->RSSetState(nullptr);
        context_->RSSetScissorRects(0, nullptr);
        context_->OMSetBlendState(nullptr, kBlendFactor, 0xFFFFFFFFu);
        context_->OMSetDepthStencilState(nullptr, 0);
        context_->GSSetShader(nullptr, nullptr, 0);
    }
    auto* rtv = output_rtv_.Get();
    ID3D11ShaderResourceView* srvs[3] = {depth, prevDepth, engineVelocity};
    ID3D11Buffer* buffers[] = {constants_.Get(), now, previous};
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width),
        static_cast<float>(height), 0, 1};
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_.Get(), nullptr, 0);
    context_->PSSetShader(now ? pixel_gpu_.Get() : pixel_.Get(), nullptr, 0);
    context_->PSSetShaderResources(0, 3, srvs);
    context_->PSSetConstantBuffers(0, now ? 3 : 1, buffers);
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* empty_srvs[3] = {nullptr, nullptr, nullptr};
    context_->PSSetShaderResources(0, 3, empty_srvs);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    if (preserveContextState) {
        context_->SwapDeviceContextState(prior.Get(), nullptr);
    }

    return true;
}

void MotionPass::clear(ID3D11DeviceContext* context) {
    if (!context || !output_rtv_) return;
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context->ClearRenderTargetView(output_rtv_.Get(), zero);
}

} // namespace edpe
