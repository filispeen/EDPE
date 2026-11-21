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

    UINT mv_support = 0;
    UINT depth_support = 0;
    if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_R16G16_FLOAT, &mv_support)) ||
        !(mv_support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) ||
        FAILED(device->CheckFormatSupport(DXGI_FORMAT_R32_FLOAT, &depth_support)) ||
        !(depth_support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) return false;

    Microsoft::WRL::ComPtr<ID3DBlob> cs_code, cs_gpu_code;
    if (FAILED(D3DCompile(kMotionShader, sizeof(kMotionShader) - 1, nullptr, nullptr,
            nullptr, "cs", "cs_5_0", 0, 0, &cs_code, nullptr)) ||
        FAILED(D3DCompile(kMotionShader, sizeof(kMotionShader) - 1, nullptr, nullptr,
            nullptr, "cs_gpu", "cs_5_0", 0, 0, &cs_gpu_code, nullptr)) ||
        FAILED(device->CreateComputeShader(cs_code->GetBufferPointer(),
            cs_code->GetBufferSize(), nullptr, &compute_)) ||
        FAILED(device->CreateComputeShader(cs_gpu_code->GetBufferPointer(),
            cs_gpu_code->GetBufferSize(), nullptr, &compute_gpu_))) return false;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 36 * sizeof(float);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &constants_))) return false;
    device_ = device;
    return true;
}

bool MotionPass::ensureOutputs(UINT width, UINT height, bool needClassOutput) {
    const bool sizeChanged = (width != width_ || height != height_);
    if (!output_texture_ || !depth_output_texture_ || sizeChanged) {
        D3D11_TEXTURE2D_DESC mvDesc{};
        mvDesc.Width = width;
        mvDesc.Height = height;
        mvDesc.MipLevels = mvDesc.ArraySize = 1;
        mvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
        mvDesc.SampleDesc.Count = 1;
        mvDesc.Usage = D3D11_USAGE_DEFAULT;
        mvDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> mvTex;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> mvUav;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mvSrv;
        if (FAILED(device_->CreateTexture2D(&mvDesc, nullptr, &mvTex)) ||
            FAILED(device_->CreateUnorderedAccessView(mvTex.Get(), nullptr, &mvUav)) ||
            FAILED(device_->CreateShaderResourceView(mvTex.Get(), nullptr, &mvSrv))) return false;

        D3D11_TEXTURE2D_DESC depthDesc = mvDesc;
        depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> depthTex;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> depthUav;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> depthSrv;
        if (FAILED(device_->CreateTexture2D(&depthDesc, nullptr, &depthTex)) ||
            FAILED(device_->CreateUnorderedAccessView(depthTex.Get(), nullptr, &depthUav)) ||
            FAILED(device_->CreateShaderResourceView(depthTex.Get(), nullptr, &depthSrv))) return false;

        output_texture_ = mvTex;
        output_uav_ = mvUav;
        output_srv_ = mvSrv;
        SetD3D11DebugName(output_texture_.Get(), "EDPE_MotionVectors");

        depth_output_texture_ = depthTex;
        depth_output_uav_ = depthUav;
        depth_output_srv_ = depthSrv;
        SetD3D11DebugName(depth_output_texture_.Get(), "EDPE_CanonicalDepth");

        class_output_texture_.Reset();
        class_output_uav_.Reset();
        class_output_srv_.Reset();

        width_ = width;
        height_ = height;
    }

    if (needClassOutput && !class_output_texture_) {
        D3D11_TEXTURE2D_DESC classDesc{};
        classDesc.Width = width;
        classDesc.Height = height;
        classDesc.MipLevels = classDesc.ArraySize = 1;
        classDesc.Format = DXGI_FORMAT_R8_UINT;
        classDesc.SampleDesc.Count = 1;
        classDesc.Usage = D3D11_USAGE_DEFAULT;
        classDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> classTex;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> classUav;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> classSrv;
        if (SUCCEEDED(device_->CreateTexture2D(&classDesc, nullptr, &classTex)) &&
            SUCCEEDED(device_->CreateUnorderedAccessView(classTex.Get(), nullptr, &classUav)) &&
            SUCCEEDED(device_->CreateShaderResourceView(classTex.Get(), nullptr, &classSrv))) {
            class_output_texture_ = classTex;
            class_output_uav_ = classUav;
            class_output_srv_ = classSrv;
            SetD3D11DebugName(class_output_texture_.Get(), "EDPE_MotionClass");
        }
    }
    return true;
}

bool MotionPass::render(ID3D11ShaderResourceView* depth, const CameraProjection& now,
    const CameraProjection& previous, UINT width, UINT height,
    float jitterX, float jitterY, float prevJitterX, float prevJitterY, bool reset,
    ID3D11ShaderResourceView* prevDepth,
    bool preserveContextState,
    bool zeroTranslation,
    bool enableClassOutput,
    ID3D11ShaderResourceView* weaponMotion,
    ID3D11ShaderResourceView* sceneStencil,
    ID3D11ShaderResourceView* celestialRecords,
    uint32_t celestialCount,
    bool clampCockpit) {
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
    const bool useWeapon = (weaponMotion != nullptr && !reset);
    const bool useCelestial = (celestialRecords != nullptr && celestialCount > 0u && !reset);
    const int modeFlags =
        ((prevDepth && !reset) ? 1 : 0) |
        (enableClassOutput ? 4 : 0) |
        (reset ? 8 : 0) |
        (useWeapon ? 16 : 0) |
        (useCelestial ? 32 : 0) |
        (clampCockpit ? 64 : 0);
    // Precompute relative view-space transform M_rel = R_prev^T * R_now,
    // t_rel = R_prev^T * (t_now - t_prev) in double precision on CPU to avoid
    // float32 cancellation during translation and halve per-pixel ALU.
    const double dx = zeroTranslation ? 0.0
        : (static_cast<double>(now.worldFromView[3]) - static_cast<double>(previous.worldFromView[3]));
    const double dy = zeroTranslation ? 0.0
        : (static_cast<double>(now.worldFromView[7]) - static_cast<double>(previous.worldFromView[7]));
    const double dz = zeroTranslation ? 0.0
        : (static_cast<double>(now.worldFromView[11]) - static_cast<double>(previous.worldFromView[11]));
    float values[36]{};
    for (unsigned r = 0; r < 3; ++r) {
        const double p0 = static_cast<double>(previous.worldFromView[0 * 4 + r]);
        const double p1 = static_cast<double>(previous.worldFromView[1 * 4 + r]);
        const double p2 = static_cast<double>(previous.worldFromView[2 * 4 + r]);
        for (unsigned c = 0; c < 3; ++c) {
            values[r * 4 + c] = static_cast<float>(
                p0 * static_cast<double>(now.worldFromView[0 * 4 + c]) +
                p1 * static_cast<double>(now.worldFromView[1 * 4 + c]) +
                p2 * static_cast<double>(now.worldFromView[2 * 4 + c]));
        }
        values[r * 4 + 3] = static_cast<float>(p0 * dx + p1 * dy + p2 * dz);
    }
    const float wf = static_cast<float>(width);
    const float hf = static_cast<float>(height);
    values[12] = static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz)); // prev0.x = translationStep
    values[13] = (now.depthB > 0.0f) ? (now.depthB / 50000.0f) : 1e-6f; // prev0.y = celestialDepthThresh
    values[14] = static_cast<float>(useCelestial ? (celestialCount > 16u ? 16u : celestialCount) : 0u); // prev0.z
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
        useWeapon ? weaponMotion : nullptr,
        useWeapon ? sceneStencil : nullptr,
        useCelestial ? celestialRecords : nullptr,
        values, nullptr, nullptr, width, height, preserveContextState, enableClassOutput);
}

bool MotionPass::renderGpuCameras(ID3D11ShaderResourceView* depth,
    ID3D11Buffer* now, ID3D11Buffer* previous, UINT width, UINT height,
    float jitterX, float jitterY, float prevJitterX, float prevJitterY, bool reset,
    ID3D11ShaderResourceView* prevDepth,
    bool enableClassOutput) {
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
        (enableClassOutput ? 4 : 0) |
        (reset ? 8 : 0);
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
        nullptr, nullptr, nullptr,
        values, now, previous, width, height, true, enableClassOutput);
}

bool MotionPass::renderInternal(ID3D11ShaderResourceView* depth,
    ID3D11ShaderResourceView* prevDepth,
    ID3D11ShaderResourceView* weaponMotion,
    ID3D11ShaderResourceView* sceneStencil,
    ID3D11ShaderResourceView* celestialRecords,
    const float (&values)[36], ID3D11Buffer* now, ID3D11Buffer* previous,
    UINT width, UINT height, bool preserveContextState, bool enableClassOutput) {
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

    if (!ensureOutputs(width, height, enableClassOutput)) return false;

    // If weaponMotion is provided without an explicit stencil SRV, create a view
    // on the R32G8X24_TYPELESS scene depth resource directly.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> autoStencilSrv;
    ID3D11ShaderResourceView* activeStencilSrv = sceneStencil;
    if (weaponMotion && !activeStencilSrv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC stDesc{};
        stDesc.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        stDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        stDesc.Texture2D.MipLevels = 1;
        if (SUCCEEDED(device_->CreateShaderResourceView(texture.Get(), &stDesc, &autoStencilSrv))) {
            activeStencilSrv = autoStencilSrv.Get();
        }
    }

    float patched[36];
    for (int i = 0; i < 36; ++i) patched[i] = values[i];
    int flags = static_cast<int>(patched[31] + 0.1f);
    if (enableClassOutput && !class_output_uav_) flags &= ~4;
    if (!weaponMotion || !activeStencilSrv) flags &= ~16;
    if (!celestialRecords) flags &= ~32;
    patched[31] = static_cast<float>(flags);
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, patched, 0, 0);

    Microsoft::WRL::ComPtr<ID3DDeviceContextState> prior;
    if (preserveContextState) {
        context_->SwapDeviceContextState(state_.Get(), &prior);
    }

    ID3D11ShaderResourceView* srvs[5] = {
        depth, prevDepth, weaponMotion, activeStencilSrv, celestialRecords
    };
    ID3D11UnorderedAccessView* uavs[3] = {
        output_uav_.Get(),
        depth_output_uav_.Get(),
        (enableClassOutput && class_output_uav_) ? class_output_uav_.Get() : nullptr
    };
    ID3D11Buffer* buffers[3] = {constants_.Get(), now, previous};

    context_->CSSetShader(now ? compute_gpu_.Get() : compute_.Get(), nullptr, 0);
    context_->CSSetConstantBuffers(0, now ? 3 : 1, buffers);
    context_->CSSetShaderResources(0, 5, srvs);
    context_->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
    context_->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1u);

    ID3D11UnorderedAccessView* empty_uavs[3] = {nullptr, nullptr, nullptr};
    ID3D11ShaderResourceView* empty_srvs[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    context_->CSSetUnorderedAccessViews(0, 3, empty_uavs, nullptr);
    context_->CSSetShaderResources(0, 5, empty_srvs);
    context_->CSSetShader(nullptr, nullptr, 0);

    if (preserveContextState) {
        context_->SwapDeviceContextState(prior.Get(), nullptr);
    }

    return true;
}

void MotionPass::clear(ID3D11DeviceContext* context) {
    if (!context || !output_uav_) return;
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context->ClearUnorderedAccessViewFloat(output_uav_.Get(), zero);
    if (class_output_uav_) {
        const UINT resetCls[4] = {
            static_cast<UINT>(MotionPixelClass::Reset), 0u, 0u, 0u
        };
        context->ClearUnorderedAccessViewUint(class_output_uav_.Get(), resetCls);
    }
}

} // namespace edpe
