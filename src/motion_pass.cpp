#include "motion_pass.h"
#include "motion_shader.h"
#include "renderdoc_support.h"
#include "log.h"

#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>

namespace edpe {

namespace {

bool isWineRuntime() {
    static const bool s_wine = []() {
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
    }();
    return s_wine;
}

UINT motionCompileFlags() {
    // Wine's builtin d3dcompiler_47 (vkd3d-shader) can stall or fail under -O3 on
    // large compute shaders; DXVK + RADV/ACO still optimize the resulting DXBC.
    return isWineRuntime() ? D3DCOMPILE_OPTIMIZATION_LEVEL1 : D3DCOMPILE_OPTIMIZATION_LEVEL3;
}

bool compileMotionComputeShader(ID3D11Device* device,
                                const D3D_SHADER_MACRO* macros,
                                const char* entryPoint,
                                const wchar_t* tag,
                                Microsoft::WRL::ComPtr<ID3D11ComputeShader>& outShader) {
    if (!device || !entryPoint) return false;
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    Microsoft::WRL::ComPtr<ID3DBlob> err;
    const UINT flags = motionCompileFlags();
    const HRESULT hr = D3DCompile(kMotionShader, sizeof(kMotionShader) - 1, nullptr, macros,
        nullptr, entryPoint, "cs_5_0", flags, 0, &code, &err);
    if (FAILED(hr) || !code) {
        wchar_t errWide[256]{};
        if (err && err->GetBufferPointer() && err->GetBufferSize() > 0) {
            const char* raw = static_cast<const char*>(err->GetBufferPointer());
            const int rawLen = static_cast<int>(err->GetBufferSize() > 240 ? 240 : err->GetBufferSize());
            MultiByteToWideChar(CP_UTF8, 0, raw, rawLen, errWide, 255);
            for (wchar_t& ch : errWide) {
                if (ch == L'\r' || ch == L'\n') ch = L' ';
            }
        }
        wchar_t msg[512]{};
        swprintf_s(msg, L"EDPE: MotionPass D3DCompile(%ls) failed hr=0x%08X flags=0x%X%ls%ls",
            tag ? tag : L"cs", static_cast<unsigned>(hr), flags,
            errWide[0] ? L" err=" : L"", errWide);
        EdpeLog(msg);
        return false;
    }
    const HRESULT hrCs = device->CreateComputeShader(
        code->GetBufferPointer(), code->GetBufferSize(), nullptr, &outShader);
    if (FAILED(hrCs)) {
        wchar_t msg[192]{};
        swprintf_s(msg, L"EDPE: MotionPass CreateComputeShader(%ls) failed hr=0x%08X",
            tag ? tag : L"cs", static_cast<unsigned>(hrCs));
        EdpeLog(msg);
        return false;
    }
    return true;
}

} // namespace

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
            __uuidof(ID3D11Device1), nullptr, &state_))) {
        state_.Reset();
    }

    UINT mv_support = 0;
    UINT depth_support = 0;
    if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_R16G16_FLOAT, &mv_support)) ||
        !(mv_support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) ||
        FAILED(device->CheckFormatSupport(DXGI_FORMAT_R32_FLOAT, &depth_support)) ||
        !(depth_support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE)) return false;

    const D3D_SHADER_MACRO macrosBase[] = {
        {"EDPE_MOTION_HAS_WEAPON", "0"},
        {"EDPE_MOTION_HAS_CELESTIAL", "0"},
        {"EDPE_MOTION_GPU_CAMERAS", "0"},
        {nullptr, nullptr}
    };
    if (!compileMotionComputeShader(device, macrosBase, "cs", L"cs_base", compute_)) {
        return false;
    }

    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0.0f;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sampDesc, &point_sampler_))) return false;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 36 * sizeof(float);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &constants_))) return false;
    device_ = device;
    wchar_t initMsg[160]{};
    swprintf_s(initMsg, L"EDPE: MotionPass initialized (wine=%d flags=0x%X ctxState=%d)",
        isWineRuntime() ? 1 : 0, motionCompileFlags(), state_ ? 1 : 0);
    EdpeLog(initMsg);
    return true;
}

bool MotionPass::ensureFullComputeShader() {
    if (compute_full_) return true;
    if (compute_full_attempted_ || !device_) return false;
    compute_full_attempted_ = true;
    const D3D_SHADER_MACRO macrosFull[] = {
        {"EDPE_MOTION_HAS_WEAPON", "1"},
        {"EDPE_MOTION_HAS_CELESTIAL", "1"},
        {"EDPE_MOTION_GPU_CAMERAS", "0"},
        {nullptr, nullptr}
    };
    return compileMotionComputeShader(device_.Get(), macrosFull, "cs", L"cs_full", compute_full_);
}

bool MotionPass::ensureGpuComputeShader() {
    if (compute_gpu_) return true;
    if (compute_gpu_attempted_ || !device_) return false;
    compute_gpu_attempted_ = true;
    const D3D_SHADER_MACRO macrosGpu[] = {
        {"EDPE_MOTION_HAS_WEAPON", "0"},
        {"EDPE_MOTION_HAS_CELESTIAL", "0"},
        {"EDPE_MOTION_GPU_CAMERAS", "1"},
        {nullptr, nullptr}
    };
    return compileMotionComputeShader(device_.Get(), macrosGpu, "cs_gpu", L"cs_gpu", compute_gpu_);
}

bool MotionPass::ensureCockpitTileShader() {
    if (cockpit_tile_cs_) return true;
    if (cockpit_tile_attempted_ || !device_) return false;
    cockpit_tile_attempted_ = true;
    const D3D_SHADER_MACRO macrosTile[] = {
        {"EDPE_COCKPIT_TILE_PASS", "1"},
        {nullptr, nullptr}
    };
    return compileMotionComputeShader(device_.Get(), macrosTile, "cs_cockpit_tiles", L"cs_cockpit_tiles", cockpit_tile_cs_);
}

bool MotionPass::ensureCockpitTileTexture(UINT width, UINT height) {
    if (!device_ || !width || !height) return false;
    const UINT tileW = (width + 15u) / 16u;
    const UINT tileH = (height + 15u) / 16u;
    if (cockpit_tile_tex_ && cockpit_tile_uav_ && cockpit_tile_srv_ &&
        cockpit_tile_w_ == tileW && cockpit_tile_h_ == tileH) {
        return true;
    }

    cockpit_tile_tex_.Reset();
    cockpit_tile_uav_.Reset();
    cockpit_tile_srv_.Reset();
    cockpit_tile_w_ = 0;
    cockpit_tile_h_ = 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = tileW;
    td.Height = tileH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &tex)) ||
        FAILED(device_->CreateUnorderedAccessView(tex.Get(), nullptr, &uav)) ||
        FAILED(device_->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
        return false;
    }

    cockpit_tile_tex_ = tex;
    cockpit_tile_uav_ = uav;
    cockpit_tile_srv_ = srv;
    cockpit_tile_w_ = tileW;
    cockpit_tile_h_ = tileH;
    SetD3D11DebugName(cockpit_tile_tex_.Get(), "EDPE_CockpitMotionTiles");
    return true;
}

bool MotionPass::ensureOutputs(UINT width, UINT height, bool needClassOutput, bool needReactiveOutput) {
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

        reactive_output_texture_.Reset();
        reactive_output_uav_.Reset();
        reactive_output_srv_.Reset();

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

    if (needReactiveOutput && !reactive_output_texture_) {
        D3D11_TEXTURE2D_DESC reactiveDesc{};
        reactiveDesc.Width = width;
        reactiveDesc.Height = height;
        reactiveDesc.MipLevels = reactiveDesc.ArraySize = 1;
        reactiveDesc.Format = DXGI_FORMAT_R8_UNORM;
        reactiveDesc.SampleDesc.Count = 1;
        reactiveDesc.Usage = D3D11_USAGE_DEFAULT;
        reactiveDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

        Microsoft::WRL::ComPtr<ID3D11Texture2D> reactiveTex;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> reactiveUav;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> reactiveSrv;
        if (SUCCEEDED(device_->CreateTexture2D(&reactiveDesc, nullptr, &reactiveTex)) &&
            SUCCEEDED(device_->CreateUnorderedAccessView(reactiveTex.Get(), nullptr, &reactiveUav)) &&
            SUCCEEDED(device_->CreateShaderResourceView(reactiveTex.Get(), nullptr, &reactiveSrv))) {
            reactive_output_texture_ = reactiveTex;
            reactive_output_uav_ = reactiveUav;
            reactive_output_srv_ = reactiveSrv;
            SetD3D11DebugName(reactive_output_texture_.Get(), "EDPE_MotionReactive");
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
    bool clampCockpit,
    bool enableReactiveOutput,
    bool inSrv) {
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
        (clampCockpit ? 64 : 0) |
        (enableReactiveOutput ? 512 : 0) |
        (inSrv ? 1024 : 0);
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
        values, nullptr, nullptr, width, height, preserveContextState, enableClassOutput, enableReactiveOutput);
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
        values, now, previous, width, height, true, enableClassOutput, false);
}

bool MotionPass::renderInternal(ID3D11ShaderResourceView* depth,
    ID3D11ShaderResourceView* prevDepth,
    ID3D11ShaderResourceView* weaponMotion,
    ID3D11ShaderResourceView* sceneStencil,
    ID3D11ShaderResourceView* celestialRecords,
    const float (&values)[36], ID3D11Buffer* now, ID3D11Buffer* previous,
    UINT width, UINT height, bool preserveContextState, bool enableClassOutput, bool enableReactiveOutput) {
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

    if (!ensureOutputs(width, height, enableClassOutput, enableReactiveOutput)) return false;

    float patched[36];
    for (int i = 0; i < 36; ++i) patched[i] = values[i];
    int flags = static_cast<int>(patched[31] + 0.1f);

    // Bind a cached X32_TYPELESS_G8X24_UINT stencil view on the R32G8X24_TYPELESS scene depth
    // for both ship cockpit (0x80) and on-foot weapon/viewmodel (0x10) stencil masking.
    ID3D11ShaderResourceView* activeStencilSrv = sceneStencil;
    if (!activeStencilSrv) {
        for (int i = 0; i < 2; ++i) {
            if (cached_stencil_srv_[i]) {
                Microsoft::WRL::ComPtr<ID3D11Resource> cachedRes;
                cached_stencil_srv_[i]->GetResource(&cachedRes);
                if (cachedRes.Get() == texture.Get()) {
                    activeStencilSrv = cached_stencil_srv_[i].Get();
                    break;
                }
            }
        }
        if (!activeStencilSrv) {
            D3D11_SHADER_RESOURCE_VIEW_DESC stDesc{};
            stDesc.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
            stDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            stDesc.Texture2D.MipLevels = 1;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> createdSrv;
            if (SUCCEEDED(device_->CreateShaderResourceView(texture.Get(), &stDesc, &createdSrv))) {
                const int slot = cached_stencil_next_ & 1;
                cached_stencil_srv_[slot] = createdSrv;
                cached_stencil_next_ = slot ^ 1;
                activeStencilSrv = createdSrv.Get();
            }
        }
    }

    if (enableClassOutput && !class_output_uav_) flags &= ~4;
    if (enableReactiveOutput && !reactive_output_uav_) flags &= ~512;
    if (!weaponMotion || !activeStencilSrv) flags &= ~16;
    if (!celestialRecords) flags &= ~32;
    if (activeStencilSrv) flags |= 128;
    else flags &= ~128;

    const bool useWeapon = ((flags & 16) != 0);
    const bool useCelestial = ((flags & 32) != 0);
    const bool clampCockpit = ((flags & 64) != 0);
    const bool wantFullShader = (!now) && (useWeapon || useCelestial);
    const bool hasFullShader = wantFullShader && ensureFullComputeShader();
    if (wantFullShader && !hasFullShader) {
        flags &= ~(16 | 32);
    }
    const bool effectiveWeapon = ((flags & 16) != 0);
    const bool useCockpitTiles =
        (!now) && effectiveWeapon && clampCockpit && activeStencilSrv &&
        ensureCockpitTileShader() &&
        ensureCockpitTileTexture(width, height);
    if (useCockpitTiles) {
        flags |= 256;
    } else {
        flags &= ~256;
    }

    patched[31] = static_cast<float>(flags);
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, patched, 0, 0);

    if (now && !ensureGpuComputeShader()) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DDeviceContextState> prior;
    const bool swappedState = preserveContextState && (state_ != nullptr);
    if (swappedState) {
        context_->SwapDeviceContextState(state_.Get(), &prior);
    }

    if (useCockpitTiles) {
        ID3D11Buffer* tileCb = constants_.Get();
        ID3D11ShaderResourceView* tileSrvs[2] = {weaponMotion, activeStencilSrv};
        ID3D11UnorderedAccessView* tileUav = cockpit_tile_uav_.Get();
        context_->CSSetShader(cockpit_tile_cs_.Get(), nullptr, 0);
        context_->CSSetConstantBuffers(0, 1, &tileCb);
        context_->CSSetShaderResources(2, 2, tileSrvs);
        context_->CSSetUnorderedAccessViews(0, 1, &tileUav, nullptr);
        context_->Dispatch(cockpit_tile_w_, cockpit_tile_h_, 1u);

        ID3D11UnorderedAccessView* nullTileUav = nullptr;
        ID3D11ShaderResourceView* nullTileSrvs[2] = {nullptr, nullptr};
        context_->CSSetUnorderedAccessViews(0, 1, &nullTileUav, nullptr);
        context_->CSSetShaderResources(2, 2, nullTileSrvs);
    }

    ID3D11ShaderResourceView* srvs[6] = {
        depth, prevDepth, weaponMotion, activeStencilSrv, celestialRecords,
        useCockpitTiles ? cockpit_tile_srv_.Get() : nullptr
    };
    ID3D11UnorderedAccessView* uavs[4] = {
        output_uav_.Get(),
        depth_output_uav_.Get(),
        (enableClassOutput && class_output_uav_) ? class_output_uav_.Get() : nullptr,
        (enableReactiveOutput && reactive_output_uav_) ? reactive_output_uav_.Get() : nullptr
    };
    ID3D11Buffer* buffers[3] = {constants_.Get(), now, previous};
    ID3D11SamplerState* smp = point_sampler_.Get();
    ID3D11ComputeShader* activeCs = now
        ? compute_gpu_.Get()
        : (hasFullShader ? compute_full_.Get() : compute_.Get());
    if (!activeCs) {
        if (swappedState) {
            context_->SwapDeviceContextState(prior.Get(), nullptr);
        }
        return false;
    }

    context_->CSSetShader(activeCs, nullptr, 0);
    context_->CSSetSamplers(0, 1, &smp);
    context_->CSSetConstantBuffers(0, now ? 3 : 1, buffers);
    context_->CSSetShaderResources(0, 6, srvs);
    context_->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
    context_->Dispatch((width + 7u) / 8u, (height + 7u) / 8u, 1u);

    ID3D11UnorderedAccessView* empty_uavs[4] = {nullptr, nullptr, nullptr, nullptr};
    ID3D11ShaderResourceView* empty_srvs[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    ID3D11SamplerState* null_smp = nullptr;
    context_->CSSetUnorderedAccessViews(0, 4, empty_uavs, nullptr);
    context_->CSSetShaderResources(0, 6, empty_srvs);
    context_->CSSetSamplers(0, 1, &null_smp);
    context_->CSSetShader(nullptr, nullptr, 0);

    if (swappedState) {
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
