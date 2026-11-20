#include "upscaler_backend.h"
#include "context_census.h"
#include "log.h"
#include "ngx_context.h"

std::atomic<bool> g_is_evaluating_upscaler{false};
std::atomic<int> g_evaluating_upscaler_depth{0};
std::recursive_mutex g_scene_ctx_mutex;

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <vector>

#if defined(EDPE_ENABLE_FSR)
#include <d3dcompiler.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#endif

namespace edpe {

// -------------------------------------------------------------------------
// NvidiaUpscalerBackend
// -------------------------------------------------------------------------

bool NvidiaUpscalerBackend::isAvailable() const {
    return NgxContext::is_dlss_available() || NgxContext::is_dlaa_available();
}

const wchar_t* NvidiaUpscalerBackend::unavailableReason() const {
    if (isAvailable()) return L"";
    return NgxContext::get_status_message();
}

bool NvidiaUpscalerBackend::isModeSupported(UpscaleMode mode) const {
    switch (mode) {
    case UpscaleMode::NativeAA:
        return NgxContext::is_dlaa_available();
    case UpscaleMode::Quality:
    case UpscaleMode::Balanced:
    case UpscaleMode::Performance:
    case UpscaleMode::UltraPerformance:
        return NgxContext::is_dlss_available();
    default:
        return false;
    }
}

bool NvidiaUpscalerBackend::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    initialized_ = NgxContext::initialize(device, context);
    return initialized_;
}

namespace {
DXGI_FORMAT sanitizeConcreteUavFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return format;
    }
}

/// Returns true if the upscaler can evaluate its UAV output straight into destinationTexture.
bool canWriteDirectOutputUav(const D3D11_TEXTURE2D_DESC& desc,
                             uint32_t expectedWidth,
                             uint32_t expectedHeight,
                             ID3D11Texture2D* destinationTexture,
                             ID3D11Texture2D* inputColor,
                             DXGI_FORMAT sanitizedFormat) {
    return destinationTexture &&
           destinationTexture != inputColor &&
           desc.Width == expectedWidth &&
           desc.Height == expectedHeight &&
           desc.SampleDesc.Count == 1 &&
           desc.Usage == D3D11_USAGE_DEFAULT &&
           (desc.BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE)) ==
               (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE) &&
           desc.Format == sanitizedFormat;
}
} // namespace

void NvidiaUpscalerBackend::shutdown() {
    outputTexture_.Reset();
    outputUav_.Reset();
    outputDevice_ = nullptr;
    outputWidth_ = 0;
    outputHeight_ = 0;
    outputFormat_ = DXGI_FORMAT_UNKNOWN;
    if (initialized_) {
        NgxContext::shutdown();
        initialized_ = false;
    }
}

bool NvidiaUpscalerBackend::ensureOutputTexture(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    const DXGI_FORMAT uavFormat = sanitizeConcreteUavFormat(format);
    if (outputTexture_ && outputUav_ && outputDevice_ == device &&
        outputWidth_ == width && outputHeight_ == height && outputFormat_ == uavFormat) {
        return true;
    }
    outputTexture_.Reset();
    outputUav_.Reset();
    outputDevice_ = nullptr;
    outputWidth_ = 0;
    outputHeight_ = 0;
    outputFormat_ = DXGI_FORMAT_UNKNOWN;

    if (!device || !width || !height) return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = uavFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(device->CreateTexture2D(&desc, nullptr, &outputTexture_))) {
        EdpeLog(L"EDPE: failed to create DLAA intermediate output texture");
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = uavFormat;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;

    if (FAILED(device->CreateUnorderedAccessView(outputTexture_.Get(), &uavDesc, &outputUav_))) {
        EdpeLog(L"EDPE: failed to create DLAA intermediate output UAV");
        outputTexture_.Reset();
        return false;
    }

    outputDevice_ = device;
    outputWidth_ = width;
    outputHeight_ = height;
    outputFormat_ = uavFormat;
    return true;
}

bool NvidiaUpscalerBackend::evaluate(ID3D11DeviceContext* context,
    const TemporalFrameInputs& inputs,
    ID3D11RenderTargetView* destinationRtv,
    ID3D11Texture2D* destinationTexture) {
    (void)destinationRtv;
    if (!isAvailable() || !context) return false;

    // Translate EDPE temporal conventions to NVIDIA NGX expectations.
    NvidiaDlssInputParams params{};
    const wchar_t* reason = nullptr;
    if (!translateToNvidia(inputs, &params, &reason)) {
        wchar_t message[256];
        swprintf_s(message, L"EDPE: NVIDIA input translation failed: %ls", reason ? reason : L"unknown");
        EdpeLog(message);
        return false;
    }

    if (!inputs.color) return false;

    D3D11_TEXTURE2D_DESC outDesc{};
    if (destinationTexture) {
        destinationTexture->GetDesc(&outDesc);
    } else {
        inputs.color->GetDesc(&outDesc);
    }

    const DXGI_FORMAT uavFormat = sanitizeConcreteUavFormat(outDesc.Format);
    const bool directDest = canWriteDirectOutputUav(
        outDesc, inputs.outputWidth, inputs.outputHeight, destinationTexture, inputs.color, uavFormat);

    if (!directDest) {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        if (!ensureOutputTexture(device.Get(), inputs.outputWidth, inputs.outputHeight, outDesc.Format)) {
            return false;
        }
    }

    ID3D11Texture2D* evalOut = directDest ? destinationTexture : outputTexture_.Get();
    ID3D11Resource* colorIn = inputs.color;
    ID3D11Resource* depthIn = inputs.depth;
    ID3D11Resource* motionIn = inputs.motionVectors;

    const UpscaleMode mode = UpscalerManager::instance().getMode();
    bool evalOk = NgxContext::evaluateDlss(context, params,
        colorIn, evalOut, depthIn, motionIn,
        inputs.renderWidth, inputs.renderHeight,
        inputs.outputWidth, inputs.outputHeight,
        mode, NgxContext::get_preset());

    if (!evalOk) {
        return false;
    }

    if (!directDest) {
        ID3D11Texture2D* dest = destinationTexture ? destinationTexture : inputs.color;
        if (dest) {
            context->CopyResource(dest, outputTexture_.Get());
        }
    }
    return true;
}

void NvidiaUpscalerBackend::resetHistory() {
    NgxContext::set_reset_requested(true);
}

// -------------------------------------------------------------------------
// AmdUpscalerBackend
// -------------------------------------------------------------------------

struct AmdUpscalerBackend::Impl {
    bool initialized = false;
    bool contextCreated = false;
    bool resetRequested = false;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> outputTexture;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outputUav;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> dilatedDepthTexture;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> dilatedMotionVectorsTexture;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> reconstructedPrevNearestDepthTexture;

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> reactiveCs;
    Microsoft::WRL::ComPtr<ID3D11Buffer> reactiveCb;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> reactiveLinearSampler;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> reactiveTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> reactiveSrv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> reactiveUav;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> prevColorTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> prevColorSrv;
    uint32_t reactiveWidth = 0;
    uint32_t reactiveHeight = 0;
    DXGI_FORMAT prevColorFormat = DXGI_FORMAT_UNKNOWN;
    bool prevColorValid = false;

    uint32_t maxRenderWidth = 0;
    uint32_t maxRenderHeight = 0;
    uint32_t maxOutputWidth = 0;
    uint32_t maxOutputHeight = 0;
    bool currentIsHdr = false;
    wchar_t lastErrorReason[256]{};

#if defined(EDPE_ENABLE_FSR)
    std::vector<uint8_t> scratchBuffer;
    FfxInterface ffxInterface{};
    FfxFsr3UpscalerContext fsrContext{};

    static DXGI_FORMAT ffxFormatToDxgi(FfxSurfaceFormat fmt) {
        switch (fmt) {
        case FFX_SURFACE_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_TYPELESS;
        case FFX_SURFACE_FORMAT_R32G32B32A32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
        case FFX_SURFACE_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case FFX_SURFACE_FORMAT_R32G32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
        case FFX_SURFACE_FORMAT_R32_UINT: return DXGI_FORMAT_R32_UINT;
        case FFX_SURFACE_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case FFX_SURFACE_FORMAT_R8G8B8A8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
        case FFX_SURFACE_FORMAT_R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case FFX_SURFACE_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
        case FFX_SURFACE_FORMAT_R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
        case FFX_SURFACE_FORMAT_R16G16_UINT: return DXGI_FORMAT_R16G16_UINT;
        case FFX_SURFACE_FORMAT_R16G16_SINT: return DXGI_FORMAT_R16G16_SINT;
        case FFX_SURFACE_FORMAT_R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
        case FFX_SURFACE_FORMAT_R16_UINT: return DXGI_FORMAT_R16_UINT;
        case FFX_SURFACE_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case FFX_SURFACE_FORMAT_R16_SNORM: return DXGI_FORMAT_R16_SNORM;
        case FFX_SURFACE_FORMAT_R8_UNORM: return DXGI_FORMAT_R8_UNORM;
        case FFX_SURFACE_FORMAT_R8_UINT: return DXGI_FORMAT_R8_UINT;
        case FFX_SURFACE_FORMAT_R8G8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
        case FFX_SURFACE_FORMAT_R8G8_UINT: return DXGI_FORMAT_R8G8_UINT;
        case FFX_SURFACE_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }

    static bool createSharedTexture(ID3D11Device* device,
        const FfxCreateResourceDescription& createDesc,
        DXGI_FORMAT fallbackFormat,
        uint32_t fallbackW,
        uint32_t fallbackH,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>* outTex) {
        if (!device || !outTex) return false;
        outTex->Reset();

        const FfxResourceDescription& resDesc = createDesc.resourceDescription;
        DXGI_FORMAT fmt = ffxFormatToDxgi(resDesc.format);
        if (fmt == DXGI_FORMAT_UNKNOWN) fmt = fallbackFormat;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = (resDesc.width > 0) ? resDesc.width : fallbackW;
        td.Height = (resDesc.height > 0) ? resDesc.height : fallbackH;
        td.MipLevels = (resDesc.mipCount > 0) ? resDesc.mipCount : 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.SampleDesc.Quality = 0;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        return SUCCEEDED(device->CreateTexture2D(&td, nullptr, outTex->GetAddressOf()));
    }

    static const wchar_t* getFfxErrorString(FfxErrorCode err) {
        switch (err) {
        case FFX_OK: return L"OK";
        case FFX_ERROR_INVALID_POINTER: return L"Invalid pointer";
        case FFX_ERROR_INVALID_ALIGNMENT: return L"Invalid alignment";
        case FFX_ERROR_INVALID_SIZE: return L"Invalid size";
        case FFX_EOF: return L"End of file";
        case FFX_ERROR_INVALID_PATH: return L"Invalid path";
        case FFX_ERROR_EOF: return L"EOF reached";
        case FFX_ERROR_MALFORMED_DATA: return L"Malformed data";
        case FFX_ERROR_OUT_OF_MEMORY: return L"Out of memory";
        case FFX_ERROR_INCOMPLETE_INTERFACE: return L"Incomplete interface";
        case FFX_ERROR_INVALID_ENUM: return L"Invalid enum";
        case FFX_ERROR_INVALID_ARGUMENT: return L"Invalid argument";
        case FFX_ERROR_OUT_OF_RANGE: return L"Out of range";
        case FFX_ERROR_NULL_DEVICE: return L"Null device";
        case FFX_ERROR_BACKEND_API_ERROR: return L"Backend API error";
        case FFX_ERROR_INSUFFICIENT_MEMORY: return L"Insufficient memory";
        default: return L"Unknown error";
        }
    }
#endif

    bool ensureOutputTexture(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format) {
        if (outputTexture && outputWidth == width && outputHeight == height && outputFormat == format) {
            return true;
        }
        outputTexture.Reset();
        outputUav.Reset();
        outputWidth = 0;
        outputHeight = 0;
        outputFormat = DXGI_FORMAT_UNKNOWN;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;

        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &outputTexture);
        if (FAILED(hr)) {
            swprintf_s(lastErrorReason, L"CreateTexture2D failed (HRESULT 0x%08X)", static_cast<unsigned>(hr));
            EdpeLog(L"EDPE: Failed to create AMD output texture");
            return false;
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;
        hr = device->CreateUnorderedAccessView(outputTexture.Get(), &uavDesc, &outputUav);
        if (FAILED(hr)) {
            swprintf_s(lastErrorReason, L"CreateUnorderedAccessView failed (HRESULT 0x%08X)", static_cast<unsigned>(hr));
            EdpeLog(L"EDPE: Failed to create AMD output UAV");
            outputTexture.Reset();
            return false;
        }

        outputWidth = width;
        outputHeight = height;
        outputFormat = format;
        return true;
    }

    void resetReactiveResources() {
        reactiveCs.Reset();
        reactiveCb.Reset();
        reactiveLinearSampler.Reset();
        reactiveTexture.Reset();
        reactiveSrv.Reset();
        reactiveUav.Reset();
        prevColorTexture.Reset();
        prevColorSrv.Reset();
        reactiveWidth = 0;
        reactiveHeight = 0;
        prevColorFormat = DXGI_FORMAT_UNKNOWN;
        prevColorValid = false;
    }

    void destroyFsrContext() {
#if defined(EDPE_ENABLE_FSR)
        dilatedDepthTexture.Reset();
        dilatedMotionVectorsTexture.Reset();
        reconstructedPrevNearestDepthTexture.Reset();
        resetReactiveResources();
        if (contextCreated) {
            ffxFsr3UpscalerContextDestroy(&fsrContext);
            contextCreated = false;
        }
#endif
    }

#if defined(EDPE_ENABLE_FSR)
    struct alignas(16) ReactiveParams {
        uint32_t renderWidth;
        uint32_t renderHeight;
        float invRenderWidth;
        float invRenderHeight;
        float mvScaleX;
        float mvScaleY;
        float hasPrev;
        float isHdr;
    };

    bool ensureReactivePipeline(ID3D11Device* device) {
        if (reactiveCs && reactiveCb && reactiveLinearSampler) return true;
        if (!device) return false;

        static constexpr char kFsrReactiveCsHlsl[] = R"(
Texture2D<float4> tCurrColor : register(t0);
Texture2D<float4> tPrevColor : register(t1);
Texture2D<float2> tMotion    : register(t2);
Texture2D<float>  tDepth     : register(t3);
SamplerState      sLinear    : register(s0);
RWTexture2D<float> uReactive : register(u0);

cbuffer ReactiveParams : register(b0) {
    uint2  renderSize;
    float2 invRenderSize;
    float2 mvScale;
    float  hasPrev;
    float  isHdr;
};

float3 toPerceptual(float3 c, float hdrFlag) {
    float3 pos = max(c, 0.0.xxx);
    return (hdrFlag > 0.5) ? (pos / (1.0.xxx + pos)) : saturate(pos);
}

[numthreads(16, 16, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= renderSize.x || id.y >= renderSize.y) return;
    int2 p = int2(id.xy);
    int2 maxCoord = int2(renderSize) - int2(1, 1);

    float3 currRaw = tCurrColor.Load(int3(p, 0)).rgb;
    if (!all(isfinite(currRaw)) || any(currRaw < -1e-4)) {
        uReactive[p] = 1.0;
        return;
    }

    float3 cTone = toPerceptual(currRaw, isHdr);
    float3 cMin = cTone;
    float3 cMax = cTone;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            int2 np = clamp(p + int2(dx, dy), int2(0, 0), maxCoord);
            float3 nRaw = tCurrColor.Load(int3(np, 0)).rgb;
            if (all(isfinite(nRaw))) {
                float3 nTone = toPerceptual(nRaw, isHdr);
                cMin = min(cMin, nTone);
                cMax = max(cMax, nTone);
            }
        }
    }
    float3 range3 = cMax - cMin;
    float localContrast = max(range3.r, max(range3.g, range3.b));

    if (hasPrev < 0.5) {
        uReactive[p] = saturate(localContrast * 0.35);
        return;
    }

    float2 mv = tMotion.Load(int3(p, 0)).xy * mvScale;
    if (!all(isfinite(mv))) {
        uReactive[p] = 0.90;
        return;
    }
    float2 prevUv = (float2(p) + 0.5 + mv) * invRenderSize;
    if (any(prevUv < 0.0) || any(prevUv > 1.0)) {
        uReactive[p] = 0.0;
        return;
    }

    float3 prevRaw = tPrevColor.SampleLevel(sLinear, prevUv, 0).rgb;
    if (!all(isfinite(prevRaw))) {
        uReactive[p] = 1.0;
        return;
    }
    float3 prevTone = toPerceptual(prevRaw, isHdr);
    float colorDiff = length(cTone - prevTone);

    float depth = tDepth.Load(int3(p, 0)).r;
    float isSkyOrTranslucent = (depth <= 1e-6) ? 1.0 : 0.0;

    float tempReact = saturate((colorDiff - 0.06) * 2.8);
    float thinFeatureBoost = saturate((localContrast - 0.18) * 2.0) * saturate((colorDiff - 0.025) * 4.0);
    float skyTrailBoost = isSkyOrTranslucent * saturate((colorDiff - 0.03) * 3.5);
    uReactive[p] = min(0.90, max(tempReact, max(thinFeatureBoost, skyTrailBoost)));
}
)";

        if (!reactiveCs) {
            Microsoft::WRL::ComPtr<ID3DBlob> csBlob, errBlob;
            if (FAILED(D3DCompile(kFsrReactiveCsHlsl, sizeof(kFsrReactiveCsHlsl) - 1,
                    nullptr, nullptr, nullptr, "cs_main", "cs_5_0", 0, 0, &csBlob, &errBlob)) ||
                !csBlob ||
                FAILED(device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(),
                    nullptr, &reactiveCs))) {
                return false;
            }
        }
        if (!reactiveCb) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizeof(ReactiveParams);
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&bd, nullptr, &reactiveCb))) return false;
        }
        if (!reactiveLinearSampler) {
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(device->CreateSamplerState(&sd, &reactiveLinearSampler))) return false;
        }
        return true;
    }

    bool ensureReactiveTargets(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT colorFmt) {
        if (!device || !width || !height) return false;
        if (reactiveTexture && reactiveUav && prevColorTexture && prevColorSrv &&
            reactiveWidth == width && reactiveHeight == height && prevColorFormat == colorFmt) {
            return true;
        }

        reactiveTexture.Reset();
        reactiveSrv.Reset();
        reactiveUav.Reset();
        prevColorTexture.Reset();
        prevColorSrv.Reset();
        reactiveWidth = 0;
        reactiveHeight = 0;
        prevColorFormat = DXGI_FORMAT_UNKNOWN;
        prevColorValid = false;

        D3D11_TEXTURE2D_DESC rDesc{};
        rDesc.Width = width;
        rDesc.Height = height;
        rDesc.MipLevels = 1;
        rDesc.ArraySize = 1;
        rDesc.Format = DXGI_FORMAT_R8_UNORM;
        rDesc.SampleDesc.Count = 1;
        rDesc.Usage = D3D11_USAGE_DEFAULT;
        rDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(device->CreateTexture2D(&rDesc, nullptr, &reactiveTexture))) return false;
        if (FAILED(device->CreateShaderResourceView(reactiveTexture.Get(), nullptr, &reactiveSrv))) return false;
        if (FAILED(device->CreateUnorderedAccessView(reactiveTexture.Get(), nullptr, &reactiveUav))) return false;

        D3D11_TEXTURE2D_DESC pDesc{};
        pDesc.Width = width;
        pDesc.Height = height;
        pDesc.MipLevels = 1;
        pDesc.ArraySize = 1;
        pDesc.Format = colorFmt;
        pDesc.SampleDesc.Count = 1;
        pDesc.Usage = D3D11_USAGE_DEFAULT;
        pDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&pDesc, nullptr, &prevColorTexture))) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = sanitizeConcreteUavFormat(colorFmt);
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(prevColorTexture.Get(), &srvDesc, &prevColorSrv))) return false;

        reactiveWidth = width;
        reactiveHeight = height;
        prevColorFormat = colorFmt;
        return true;
    }

    ID3D11Texture2D* generateReactiveMask(ID3D11DeviceContext* context, ID3D11Device* device,
                                          const TemporalFrameInputs& inputs,
                                          float mvScaleX, float mvScaleY, bool reset) {
        if (!context || !device || !inputs.color || !inputs.colorSrv ||
            !inputs.motionSrv || !inputs.depthSrv) {
            return nullptr;
        }
        D3D11_TEXTURE2D_DESC cDesc{};
        inputs.color->GetDesc(&cDesc);
        if (cDesc.Width != inputs.renderWidth || cDesc.Height != inputs.renderHeight) {
            return nullptr;
        }
        if (!ensureReactivePipeline(device) ||
            !ensureReactiveTargets(device, inputs.renderWidth, inputs.renderHeight, cDesc.Format)) {
            return nullptr;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(reactiveCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            return nullptr;
        }
        auto* params = static_cast<ReactiveParams*>(mapped.pData);
        params->renderWidth = inputs.renderWidth;
        params->renderHeight = inputs.renderHeight;
        params->invRenderWidth = 1.0f / static_cast<float>(inputs.renderWidth);
        params->invRenderHeight = 1.0f / static_cast<float>(inputs.renderHeight);
        params->mvScaleX = mvScaleX;
        params->mvScaleY = mvScaleY;
        params->hasPrev = (prevColorValid && !reset) ? 1.0f : 0.0f;
        params->isHdr = inputs.colorIsHdr ? 1.0f : 0.0f;
        context->Unmap(reactiveCb.Get(), 0);

        Microsoft::WRL::ComPtr<ID3D11ComputeShader> oldCs;
        Microsoft::WRL::ComPtr<ID3D11Buffer> oldCb;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> oldSamp;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> oldSrvs[4];
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> oldUav;
        context->CSGetShader(&oldCs, nullptr, nullptr);
        context->CSGetConstantBuffers(0, 1, &oldCb);
        context->CSGetSamplers(0, 1, &oldSamp);
        context->CSGetShaderResources(0, 4, oldSrvs[0].GetAddressOf());
        context->CSGetUnorderedAccessViews(0, 1, &oldUav);

        ID3D11ShaderResourceView* srvs[4] = {
            inputs.colorSrv,
            prevColorSrv.Get(),
            inputs.motionSrv,
            inputs.depthSrv
        };
        ID3D11UnorderedAccessView* uav = reactiveUav.Get();
        ID3D11Buffer* cb = reactiveCb.Get();
        ID3D11SamplerState* samp = reactiveLinearSampler.Get();

        context->CSSetShader(reactiveCs.Get(), nullptr, 0);
        context->CSSetConstantBuffers(0, 1, &cb);
        context->CSSetSamplers(0, 1, &samp);
        context->CSSetShaderResources(0, 4, srvs);
        context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context->Dispatch((inputs.renderWidth + 15) / 16, (inputs.renderHeight + 15) / 16, 1);

        ID3D11ShaderResourceView* nullSrvs[4] = {nullptr, nullptr, nullptr, nullptr};
        ID3D11UnorderedAccessView* nullUav = nullptr;
        context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        context->CSSetShaderResources(0, 4, nullSrvs);

        ID3D11ShaderResourceView* restoreSrvs[4] = {
            oldSrvs[0].Get(), oldSrvs[1].Get(), oldSrvs[2].Get(), oldSrvs[3].Get()
        };
        ID3D11UnorderedAccessView* restoreUav = oldUav.Get();
        ID3D11Buffer* restoreCb = oldCb.Get();
        ID3D11SamplerState* restoreSamp = oldSamp.Get();
        context->CSSetShaderResources(0, 4, restoreSrvs);
        context->CSSetUnorderedAccessViews(0, 1, &restoreUav, nullptr);
        context->CSSetConstantBuffers(0, 1, &restoreCb);
        context->CSSetSamplers(0, 1, &restoreSamp);
        context->CSSetShader(oldCs.Get(), nullptr, 0);

        return reactiveTexture.Get();
    }

    void capturePrevColor(ID3D11DeviceContext* context, ID3D11Texture2D* colorTex,
                          uint32_t width, uint32_t height) {
        if (!context || !colorTex || !prevColorTexture ||
            reactiveWidth != width || reactiveHeight != height) {
            return;
        }
        context->CopyResource(prevColorTexture.Get(), colorTex);
        prevColorValid = true;
    }
#endif

    bool ensureFsrContext(ID3D11Device* device, uint32_t renderW, uint32_t renderH,
                          uint32_t outputW, uint32_t outputH, bool isHdr) {
#if defined(EDPE_ENABLE_FSR)
        if (!device) return false;
        if (contextCreated && renderW <= maxRenderWidth && renderH <= maxRenderHeight &&
            outputW == maxOutputWidth && outputH == maxOutputHeight &&
            currentIsHdr == isHdr &&
            dilatedDepthTexture && dilatedMotionVectorsTexture && reconstructedPrevNearestDepthTexture) {
            return true;
        }

        destroyFsrContext();

        maxRenderWidth = renderW;
        maxRenderHeight = renderH;
        maxOutputWidth = outputW;
        maxOutputHeight = outputH;
        currentIsHdr = isHdr;

        FfxFsr3UpscalerContextDescription desc{};
        desc.flags = FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED |
                     FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE;
        if (isHdr) {
            desc.flags |= FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE |
                          FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
        }
        desc.maxRenderSize.width = maxRenderWidth;
        desc.maxRenderSize.height = maxRenderHeight;
        desc.maxUpscaleSize.width = maxOutputWidth;
        desc.maxUpscaleSize.height = maxOutputHeight;
        desc.backendInterface = ffxInterface;
        desc.fpMessage = [](FfxMsgType type, const wchar_t* msg) {
            if (type == FFX_MESSAGE_TYPE_ERROR) {
                wchar_t buf[512];
                swprintf_s(buf, L"EDPE FSR Error: %ls", msg ? msg : L"unknown");
                EdpeLog(buf);
            }
        };

        FfxErrorCode err = ffxFsr3UpscalerContextCreate(&fsrContext, &desc);
        if (err != FFX_OK) {
            wchar_t buf[256];
            swprintf_s(lastErrorReason, L"ffxFsr3UpscalerContextCreate failed (0x%08X: %ls)", static_cast<unsigned>(err), getFfxErrorString(err));
            swprintf_s(buf, L"EDPE: ffxFsr3UpscalerContextCreate failed (0x%08X)", static_cast<unsigned>(err));
            EdpeLog(buf);
            return false;
        }
        contextCreated = true;

        // Allocate the three shared working surfaces required by FSR 3.1.2's dilate and reprojection passes.
        // Without these textures, RegisterResourceDX11 maps null pointers to NULL resources and the dilate
        // pass discards all motion vectors, causing reprojection to read (0, 0) every frame.
        FfxFsr3UpscalerSharedResourceDescriptions sharedDescs{};
        err = ffxFsr3UpscalerGetSharedResourceDescriptions(&fsrContext, &sharedDescs);
        if (err != FFX_OK) {
            swprintf_s(lastErrorReason, L"ffxFsr3UpscalerGetSharedResourceDescriptions failed (0x%08X: %ls)", static_cast<unsigned>(err), getFfxErrorString(err));
            destroyFsrContext();
            return false;
        }

        if (!createSharedTexture(device, sharedDescs.dilatedDepth, DXGI_FORMAT_R32_FLOAT, maxRenderWidth, maxRenderHeight, &dilatedDepthTexture) ||
            !createSharedTexture(device, sharedDescs.dilatedMotionVectors, DXGI_FORMAT_R16G16_FLOAT, maxRenderWidth, maxRenderHeight, &dilatedMotionVectorsTexture) ||
            !createSharedTexture(device, sharedDescs.reconstructedPrevNearestDepth, DXGI_FORMAT_R32_UINT, maxRenderWidth, maxRenderHeight, &reconstructedPrevNearestDepthTexture)) {
            swprintf_s(lastErrorReason, L"Failed to create FSR 3.1 shared working textures");
            EdpeLog(L"EDPE: Failed to create FSR 3.1 shared working textures");
            destroyFsrContext();
            return false;
        }

        resetRequested = true;
        return true;
#else
        return false;
#endif
    }
};

AmdUpscalerBackend::AmdUpscalerBackend()
    : impl_(std::make_unique<Impl>()) {}

AmdUpscalerBackend::~AmdUpscalerBackend() {
    shutdown();
}

bool AmdUpscalerBackend::isAvailable() const {
#if defined(EDPE_ENABLE_FSR)
    return impl_ && impl_->initialized;
#else
    return false;
#endif
}

const wchar_t* AmdUpscalerBackend::unavailableReason() const {
#if defined(EDPE_ENABLE_FSR)
    if (isAvailable()) return L"";
    if (impl_ && impl_->lastErrorReason[0] != L'\0') {
        return impl_->lastErrorReason;
    }
    return L"AMD FidelityFX SDK D3D11 backend not initialized";
#else
    return L"AMD FidelityFX SDK not compiled into EDPE";
#endif
}

bool AmdUpscalerBackend::isModeSupported(UpscaleMode mode) const {
    if (!isAvailable()) return false;
    return mode == UpscaleMode::NativeAA ||
           mode == UpscaleMode::Quality ||
           mode == UpscaleMode::Balanced ||
           mode == UpscaleMode::Performance ||
           mode == UpscaleMode::UltraPerformance;
}

bool AmdUpscalerBackend::initialize(ID3D11Device* device, ID3D11DeviceContext* /*context*/) {
#if defined(EDPE_ENABLE_FSR)
    if (!impl_) impl_ = std::make_unique<Impl>();
    if (!device) {
        swprintf_s(impl_->lastErrorReason, L"D3D11 device is null (HRESULT 0x%08X)", static_cast<unsigned>(E_POINTER));
        return false;
    }

    size_t scratchSize = ffxGetScratchMemorySizeDX11(FFX_FSR3UPSCALER_CONTEXT_COUNT);
    impl_->scratchBuffer.resize(scratchSize);
    FfxDevice ffxDevice = ffxGetDeviceDX11_Fsr31(device);
    FfxErrorCode err = ffxGetInterfaceDX11(&impl_->ffxInterface, ffxDevice,
        impl_->scratchBuffer.data(), scratchSize, FFX_FSR3UPSCALER_CONTEXT_COUNT);
    if (err != FFX_OK) {
        swprintf_s(impl_->lastErrorReason, L"ffxGetInterfaceDX11 failed (0x%08X: %ls)", static_cast<unsigned>(err), Impl::getFfxErrorString(err));
        wchar_t buf[256];
        swprintf_s(buf, L"EDPE: ffxGetInterfaceDX11 failed (0x%08X)", static_cast<unsigned>(err));
        EdpeLog(buf);
        return false;
    }
    impl_->initialized = true;
    impl_->lastErrorReason[0] = L'\0';
    EdpeLog(L"EDPE: AMD FSR 3.1 D3D11 backend initialized successfully");
    return true;
#else
    return false;
#endif
}

void AmdUpscalerBackend::shutdown() {
    if (!impl_) return;
    impl_->destroyFsrContext();
    impl_->outputTexture.Reset();
    impl_->outputUav.Reset();
#if defined(EDPE_ENABLE_FSR)
    impl_->scratchBuffer.clear();
#endif
    impl_->initialized = false;
    impl_->lastErrorReason[0] = L'\0';
}

bool AmdUpscalerBackend::evaluate(ID3D11DeviceContext* context,
    const TemporalFrameInputs& inputs,
    ID3D11RenderTargetView* destinationRtv,
    ID3D11Texture2D* destinationTexture) {
#if defined(EDPE_ENABLE_FSR)
    if (!isAvailable() || !context || !impl_) return false;

    AmdFsrInputParams params{};
    const wchar_t* reason = nullptr;
    if (!translateToAmd(inputs, &params, &reason)) {
        wchar_t message[256];
        swprintf_s(message, L"EDPE: AMD input translation failed: %ls", reason ? reason : L"unknown");
        EdpeLog(message);
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device) return false;

    D3D11_TEXTURE2D_DESC outDesc{};
    if (destinationTexture) {
        destinationTexture->GetDesc(&outDesc);
    } else {
        inputs.color->GetDesc(&outDesc);
    }

    const DXGI_FORMAT uavFormat = sanitizeConcreteUavFormat(outDesc.Format);
    const bool directDest = canWriteDirectOutputUav(
        outDesc, inputs.outputWidth, inputs.outputHeight, destinationTexture, inputs.color, uavFormat);

    if (!directDest &&
        !impl_->ensureOutputTexture(device.Get(), inputs.outputWidth, inputs.outputHeight, outDesc.Format)) {
        return false;
    }

    if (!impl_->ensureFsrContext(device.Get(), inputs.renderWidth, inputs.renderHeight,
                                 inputs.outputWidth, inputs.outputHeight, inputs.colorIsHdr)) {
        return false;
    }

    FfxFsr3UpscalerDispatchDescription dispatchDesc{};
    dispatchDesc.commandList = ffxGetCommandListDX11(context);

    // Resources
    dispatchDesc.color = ffxGetResourceDX11_Fsr31(inputs.color,
        GetFfxResourceDescriptionDX11(inputs.color), L"FSR3_Color", FFX_RESOURCE_STATE_COMPUTE_READ);
    FfxResourceDescription depthDesc = GetFfxResourceDescriptionDX11(inputs.depth);
    if (depthDesc.format == FFX_SURFACE_FORMAT_UNKNOWN) {
        depthDesc.format = FFX_SURFACE_FORMAT_R32_FLOAT;
    }
    dispatchDesc.depth = ffxGetResourceDX11_Fsr31(inputs.depth,
        depthDesc, L"FSR3_Depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.motionVectors = ffxGetResourceDX11_Fsr31(inputs.motionVectors,
        GetFfxResourceDescriptionDX11(inputs.motionVectors), L"FSR3_MotionVectors", FFX_RESOURCE_STATE_COMPUTE_READ);

    // Shared working surfaces required by FSR 3.1.2 dilate and reprojection passes
    dispatchDesc.dilatedDepth = ffxGetResourceDX11_Fsr31(impl_->dilatedDepthTexture.Get(),
        GetFfxResourceDescriptionDX11(impl_->dilatedDepthTexture.Get()), L"FSR3_DilatedDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatchDesc.dilatedMotionVectors = ffxGetResourceDX11_Fsr31(impl_->dilatedMotionVectorsTexture.Get(),
        GetFfxResourceDescriptionDX11(impl_->dilatedMotionVectorsTexture.Get()), L"FSR3_DilatedMotionVectors", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatchDesc.reconstructedPrevNearestDepth = ffxGetResourceDX11_Fsr31(impl_->reconstructedPrevNearestDepthTexture.Get(),
        GetFfxResourceDescriptionDX11(impl_->reconstructedPrevNearestDepthTexture.Get()), L"FSR3_ReconstructedPrevNearestDepth", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    if (ID3D11Texture2D* reactiveTex = impl_->generateReactiveMask(
            context, device.Get(), inputs, params.motionVectorScaleX, params.motionVectorScaleY,
            params.reset || impl_->resetRequested)) {
        dispatchDesc.reactive = ffxGetResourceDX11_Fsr31(reactiveTex,
            GetFfxResourceDescriptionDX11(reactiveTex), L"FSR3_Reactive", FFX_RESOURCE_STATE_COMPUTE_READ);
    }

    ID3D11Texture2D* evalOut = directDest ? destinationTexture : impl_->outputTexture.Get();
    dispatchDesc.output = ffxGetResourceDX11_Fsr31(evalOut,
        GetFfxResourceDescriptionDX11(evalOut), L"FSR3_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

    // Jitter & Motion parameters
    dispatchDesc.jitterOffset.x = params.jitterX;
    dispatchDesc.jitterOffset.y = params.jitterY;
    dispatchDesc.motionVectorScale.x = params.motionVectorScaleX;
    dispatchDesc.motionVectorScale.y = params.motionVectorScaleY;
    dispatchDesc.renderSize.width = inputs.renderWidth;
    dispatchDesc.renderSize.height = inputs.renderHeight;
    dispatchDesc.upscaleSize.width = inputs.outputWidth;
    dispatchDesc.upscaleSize.height = inputs.outputHeight;

    dispatchDesc.enableSharpening = params.sharpness > 0.0f;
    dispatchDesc.sharpness = std::clamp(params.sharpness, 0.0f, 1.0f);
    dispatchDesc.frameTimeDelta = params.frameTimeDeltaMs > 0.0f ? params.frameTimeDeltaMs : 16.6f;
    dispatchDesc.preExposure = 1.0f;
    dispatchDesc.reset = params.reset || impl_->resetRequested;
    impl_->resetRequested = false;

    dispatchDesc.cameraFovAngleVertical = (inputs.cameraNow.scaleY > 0.0f)
        ? (2.0f * std::atan(1.0f / inputs.cameraNow.scaleY))
        : 1.0f;
    const float nearPlane = (inputs.cameraNow.depthB > 0.0f)
        ? inputs.cameraNow.depthB
        : 0.025f;
    if (params.depthInverted) {
        dispatchDesc.cameraNear = FLT_MAX;
        dispatchDesc.cameraFar = nearPlane;
    } else {
        dispatchDesc.cameraNear = nearPlane;
        dispatchDesc.cameraFar = FLT_MAX;
    }
    dispatchDesc.viewSpaceToMetersFactor = 1.0f;

    FfxErrorCode err;
    {
        EvaluatingUpscalerGuard evalGuard;
        err = ffxFsr3UpscalerContextDispatch(&impl_->fsrContext, &dispatchDesc);
    }
    if (err != FFX_OK) {
        swprintf_s(impl_->lastErrorReason, L"ffxFsr3UpscalerContextDispatch failed (0x%08X: %ls)", static_cast<unsigned>(err), Impl::getFfxErrorString(err));
        wchar_t buf[256];
        swprintf_s(buf, L"EDPE: ffxFsr3UpscalerContextDispatch failed (0x%08X)", static_cast<unsigned>(err));
        EdpeLog(buf);
        return false;
    }

    impl_->capturePrevColor(context, inputs.color, inputs.renderWidth, inputs.renderHeight);

    if (!directDest) {
        ID3D11Texture2D* dest = destinationTexture ? destinationTexture : inputs.color;
        if (dest) {
            context->CopyResource(dest, impl_->outputTexture.Get());
        }
    }
    return true;
#else
    return false;
#endif
}

void AmdUpscalerBackend::resetHistory() {
    if (impl_) {
        impl_->resetRequested = true;
        impl_->prevColorValid = false;
    }
}

// -------------------------------------------------------------------------
// TaaUpscalerBackend
// -------------------------------------------------------------------------

bool TaaUpscalerBackend::isAvailable() const {
    return pass_.isInitialized();
}

const wchar_t* TaaUpscalerBackend::unavailableReason() const {
    return pass_.isInitialized() ? L"" : L"D3D11 TAA pass not initialized";
}

bool TaaUpscalerBackend::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    return pass_.initialize(device, context);
}

void TaaUpscalerBackend::shutdown() {
    pass_.shutdown();
}

bool TaaUpscalerBackend::evaluate(ID3D11DeviceContext* context,
    const TemporalFrameInputs& inputs,
    ID3D11RenderTargetView* destinationRtv,
    ID3D11Texture2D* destinationTexture) {
    if (!pass_.isInitialized() || !context || !inputs.color || !destinationTexture) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!device) return false;

    // Resolve color SRV
    ID3D11ShaderResourceView* colorSrv = inputs.colorSrv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> localColorSrv;
    if (!colorSrv && inputs.color) {
        if (FAILED(device->CreateShaderResourceView(inputs.color, nullptr, &localColorSrv))) {
            return false;
        }
        colorSrv = localColorSrv.Get();
    }

    // Resolve motion SRV
    ID3D11ShaderResourceView* motionSrv = inputs.motionSrv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> localMotionSrv;
    if (!motionSrv && inputs.motionVectors) {
        device->CreateShaderResourceView(inputs.motionVectors, nullptr, &localMotionSrv);
        motionSrv = localMotionSrv.Get();
    }

    // Resolve depth SRV
    ID3D11ShaderResourceView* depthSrv = inputs.depthSrv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> localDepthSrv;
    if (!depthSrv && inputs.depth) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(inputs.depth, &srvDesc, &localDepthSrv);
        depthSrv = localDepthSrv.Get();
    }

    return pass_.render(context,
        colorSrv,
        motionSrv,
        depthSrv,
        destinationRtv,
        destinationTexture,
        inputs.outputWidth,
        inputs.outputHeight,
        inputs.jitterX,
        inputs.jitterY,
        inputs.reset,
        inputs.renderWidth,
        inputs.renderHeight,
        inputs.prevJitterX,
        inputs.prevJitterY,
        inputs.cameraNow.depthB,
        inputs.prevDepthSrv);
}

void TaaUpscalerBackend::resetHistory() {
    pass_.resetHistory();
}

// -------------------------------------------------------------------------
// UpscalerManager
// -------------------------------------------------------------------------

UpscalerManager& UpscalerManager::instance() {
    static UpscalerManager manager;
    return manager;
}

UpscalerManager::UpscalerManager() {
    diagnostics_.backend = currentBackendType_;
    diagnostics_.mode = currentMode_;
    swprintf_s(diagnostics_.statusMessage, L"Upscaler initialized: Bypass (Native)");
}

bool UpscalerManager::initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    device_ = device;
    context_ = context;

    nullBackend_.initialize(device, context);
    nvidiaBackend_.initialize(device, context);
    amdBackend_.initialize(device, context);
    taaBackend_.initialize(device, context);
    sharpenPass_.initialize(device, context);

    if (device) {
        D3D11_QUERY_DESC disjointDesc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
        D3D11_QUERY_DESC tsDesc{ D3D11_QUERY_TIMESTAMP, 0 };
        for (auto& slot : timingRing_) {
            slot = {};
            device->CreateQuery(&disjointDesc, &slot.disjoint);
            device->CreateQuery(&tsDesc, &slot.start);
            device->CreateQuery(&tsDesc, &slot.end);
        }
    }

    EdpeLog(L"EDPE: UpscalerManager initialized");
    return true;
}

void UpscalerManager::shutdown() {
    nullBackend_.shutdown();
    nvidiaBackend_.shutdown();
    amdBackend_.shutdown();
    taaBackend_.shutdown();
    sharpenPass_.shutdown();

    for (auto& slot : timingRing_) {
        slot = {};
    }
    timingWriteIndex_ = 0;
    lastGpuTimeMs_ = 0.0f;
    for (auto& bt : backendTiming_) {
        bt = {};
    }
    evalCount_ = 0;

    device_ = nullptr;
    context_ = nullptr;
    currentBackendType_ = UpscalerBackend::None;
    currentMode_ = UpscaleMode::Native;
    diagnostics_ = {};
    swprintf_s(diagnostics_.statusMessage, L"Upscaler shut down");
}

IUpscalerBackend* UpscalerManager::getBackendInstance(UpscalerBackend backend) {
    switch (backend) {
    case UpscalerBackend::None:
        return &nullBackend_;
    case UpscalerBackend::NvidiaDlss:
        return &nvidiaBackend_;
    case UpscalerBackend::AmdFsr:
        return &amdBackend_;
    case UpscalerBackend::Taa:
        return &taaBackend_;
    default:
        return &nullBackend_;
    }
}

bool UpscalerManager::isBackendAvailable(UpscalerBackend backend) const {
    switch (backend) {
    case UpscalerBackend::None:
        return true;
    case UpscalerBackend::NvidiaDlss:
        return nvidiaBackend_.isAvailable();
    case UpscalerBackend::AmdFsr:
        return amdBackend_.isAvailable();
    case UpscalerBackend::Taa:
        return taaBackend_.isAvailable();
    default:
        return false;
    }
}

const wchar_t* UpscalerManager::getBackendUnavailableReason(UpscalerBackend backend) const {
    switch (backend) {
    case UpscalerBackend::None:
        return L"";
    case UpscalerBackend::NvidiaDlss:
        return nvidiaBackend_.unavailableReason();
    case UpscalerBackend::AmdFsr:
        return amdBackend_.unavailableReason();
    case UpscalerBackend::Taa:
        return taaBackend_.unavailableReason();
    default:
        return L"Unknown backend";
    }
}

bool UpscalerManager::setBackend(UpscalerBackend backend) {
    if (backend == currentBackendType_) return true;

    auto* instance = getBackendInstance(backend);
    if (!instance->isAvailable()) {
        wchar_t message[256];
        swprintf_s(message, L"EDPE: cannot select upscaler backend %ls: %ls",
            getUpscalerBackendName(backend), instance->unavailableReason());
        EdpeLog(message);
        return false;
    }

    currentBackendType_ = backend;
    pendingReset_ = true;
    evalCount_ = 0;

    wchar_t message[128];
    swprintf_s(message, L"EDPE: upscaler backend changed to %ls", getUpscalerBackendName(backend));
    EdpeLog(message);
    return true;
}

bool UpscalerManager::setMode(UpscaleMode mode) {
    if (mode == currentMode_) return true;

    auto* instance = getBackendInstance(currentBackendType_);
    if (!instance->isModeSupported(mode)) {
        wchar_t message[160];
        swprintf_s(message, L"EDPE: mode %ls not supported by backend %ls",
            getUpscaleModeName(mode), instance->name());
        EdpeLog(message);
        return false;
    }

    currentMode_ = mode;
    pendingReset_ = true;

    wchar_t message[128];
    swprintf_s(message, L"EDPE: upscaler mode changed to %ls", getUpscaleModeName(mode));
    EdpeLog(message);
    return true;
}

bool UpscalerManager::evaluate(ID3D11DeviceContext* context,
    const TemporalFrameInputs& inputs,
    ID3D11RenderTargetView* destinationRtv,
    ID3D11Texture2D* destinationTexture) {
    if (context) {
        Microsoft::WRL::ComPtr<ID3D11Device> evalDevice;
        context->GetDevice(&evalDevice);
        if (evalDevice && evalDevice.Get() != device_) {
            const UpscalerBackend savedBackend = currentBackendType_;
            const UpscaleMode savedMode = currentMode_;
            const uint32_t savedPreset = NgxContext::get_preset();
            wchar_t msg[192];
            swprintf_s(msg, L"EDPE: rebinding UpscalerManager from device=%p to scene device=%p",
                device_, evalDevice.Get());
            EdpeLog(msg);
            shutdown();
            NgxContext::set_preset(savedPreset);
            initialize(evalDevice.Get(), context);
            setBackend(savedBackend);
            setMode(savedMode);
        }
    }
    diagnostics_.didRun = true;
    diagnostics_.backend = currentBackendType_;
    diagnostics_.mode = currentMode_;
    diagnostics_.renderWidth = inputs.renderWidth;
    diagnostics_.renderHeight = inputs.renderHeight;
    diagnostics_.outputWidth = inputs.outputWidth;
    diagnostics_.outputHeight = inputs.outputHeight;
    diagnostics_.jitterX = inputs.jitterX;
    diagnostics_.jitterY = inputs.jitterY;
    diagnostics_.frameTimeMs = inputs.frameTimeMs;
    diagnostics_.reset = inputs.reset || pendingReset_;

    // Validate inputs.
    const wchar_t* invalidReason = nullptr;
    if (!inputs.isValid(&invalidReason)) {
        diagnostics_.success = false;
        swprintf_s(diagnostics_.statusMessage, L"Inputs invalid: %ls",
            invalidReason ? invalidReason : L"unknown");
        // Fail open: return false to preserve original output.
        return false;
    }

    // Bypass mode.
    if (currentBackendType_ == UpscalerBackend::None || currentMode_ == UpscaleMode::Native) {
        diagnostics_.success = true;
        swprintf_s(diagnostics_.statusMessage, L"Bypass active (original rendering)");
        pendingReset_ = false;
        return true;
    }

    // Evaluate active upscaler backend.
    auto* backend = getBackendInstance(currentBackendType_);
    if (diagnostics_.reset) {
        backend->resetHistory();
    }

    const bool collectTiming =
        (evalCount_ < 4) || telemetryTimingEnabled_.load(std::memory_order_relaxed);
    const bool collectStatusText =
        (evalCount_ == 0) || telemetryStatusEnabled_.load(std::memory_order_relaxed);

    // Poll oldest GPU timestamp slot non-blockingly before reusing it (or drain in-flight slots when disabled).
    auto& slot = timingRing_[timingWriteIndex_];
    if (context && slot.inFlight && slot.disjoint && slot.start && slot.end) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
        UINT64 tStart = 0, tEnd = 0;
        if (context->GetData(slot.disjoint.Get(), &disjointData, sizeof(disjointData), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            context->GetData(slot.start.Get(), &tStart, sizeof(tStart), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            context->GetData(slot.end.Get(), &tEnd, sizeof(tEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            if (!disjointData.Disjoint && disjointData.Frequency > 0 && tEnd >= tStart) {
                lastGpuTimeMs_ = static_cast<float>(static_cast<double>(tEnd - tStart) * 1000.0 /
                    static_cast<double>(disjointData.Frequency));
            }
            slot.inFlight = false;
        }
    }

    const bool canQueryGpu = collectTiming && context && !slot.inFlight && slot.disjoint && slot.start && slot.end;
    if (canQueryGpu) {
        context->Begin(slot.disjoint.Get());
        context->End(slot.start.Get());
    }

    std::chrono::high_resolution_clock::time_point t0{};
    if (collectTiming) {
        t0 = std::chrono::high_resolution_clock::now();
    }
    bool evalSuccess = false;
    {
        EvaluatingUpscalerGuard evalGuard;
        evalSuccess = backend->evaluate(context, inputs, destinationRtv, destinationTexture);
        if (evalSuccess && currentBackendType_ != UpscalerBackend::AmdFsr && inputs.sharpness > 0.0f) {
            ID3D11Texture2D* sharpenTarget = destinationTexture ? destinationTexture : inputs.color;
            if (!sharpenPass_.apply(context, sharpenTarget, inputs.sharpness, inputs.colorIsHdr)) {
                static bool s_sharpenFailLogged = false;
                if (!s_sharpenFailLogged) {
                    s_sharpenFailLogged = true;
                    EdpeLog(L"EDPE: sharpen pass failed; output left unsharpened");
                }
            }
        }
    }
    if (evalSuccess) {
        ++evalCount_;
    }

    if (canQueryGpu) {
        context->End(slot.end.Get());
        context->End(slot.disjoint.Get());
        slot.inFlight = true;
        timingWriteIndex_ = (timingWriteIndex_ + 1) % kTimingRingSize;
    }

    if (collectTiming) {
        auto t1 = std::chrono::high_resolution_clock::now();
        const float cpuTimeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
        diagnostics_.gpuTimeMs = (lastGpuTimeMs_ > 0.0f) ? lastGpuTimeMs_ : cpuTimeMs;
    } else if (lastGpuTimeMs_ > 0.0f) {
        diagnostics_.gpuTimeMs = lastGpuTimeMs_;
    }
    diagnostics_.success = evalSuccess;

    if (!evalSuccess) {
        const wchar_t* reason = backend->unavailableReason();
        if (reason && reason[0] != L'\0') {
            swprintf_s(diagnostics_.statusMessage, L"Evaluation failed on %ls (%ls)",
                backend->name(), reason);
        } else {
            swprintf_s(diagnostics_.statusMessage, L"Evaluation failed on %ls; falling back to original output",
                backend->name());
        }
        wchar_t message[256];
        swprintf_s(message, L"EDPE: %ls evaluate failed; failing open", backend->name());
        EdpeLog(message);
        return false;
    }

    static unsigned s_eval_log_count = 0;
    if (++s_eval_log_count == 1) {
        wchar_t message[256];
        swprintf_s(message, L"EDPE: %ls reconstructed %ux%u -> %ux%u in %.3f ms (#%u)",
            backend->name(), inputs.renderWidth, inputs.renderHeight,
            inputs.outputWidth, inputs.outputHeight, diagnostics_.gpuTimeMs, s_eval_log_count);
        EdpeLog(message);
    }

    if (collectStatusText) {
        swprintf_s(diagnostics_.statusMessage, L"Reconstructed (%ux%u -> %ux%u) %.3f ms",
            inputs.renderWidth, inputs.renderHeight, inputs.outputWidth, inputs.outputHeight,
            diagnostics_.gpuTimeMs);
    }
    pendingReset_ = false;
    return true;
}

bool UpscalerManager::evaluateBackend(UpscalerBackend backendType,
    ID3D11DeviceContext* context,
    const TemporalFrameInputs& inputs,
    ID3D11RenderTargetView* destinationRtv,
    ID3D11Texture2D* destinationTexture) {
    if (backendType == UpscalerBackend::None || !isBackendAvailable(backendType)) {
        return false;
    }
    auto* backend = getBackendInstance(backendType);
    if (!backend) return false;
    if (!backend->isModeSupported(currentMode_)) return false;

    if (inputs.reset) {
        backend->resetHistory();
    }

    // Non-blocking per-backend GPU timing so each comparison image can show its own cost.
    BackendTiming* bt = nullptr;
    const auto backendIdx = static_cast<size_t>(backendType);
    if (context && device_ && backendIdx < kBackendCount) {
        bt = &backendTiming_[backendIdx];
        if (!bt->slot.disjoint) {
            D3D11_QUERY_DESC disjointDesc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            D3D11_QUERY_DESC tsDesc{ D3D11_QUERY_TIMESTAMP, 0 };
            device_->CreateQuery(&disjointDesc, &bt->slot.disjoint);
            device_->CreateQuery(&tsDesc, &bt->slot.start);
            device_->CreateQuery(&tsDesc, &bt->slot.end);
        }
        if (bt->slot.inFlight && bt->slot.disjoint && bt->slot.start && bt->slot.end) {
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
            UINT64 tStart = 0, tEnd = 0;
            if (context->GetData(bt->slot.disjoint.Get(), &disjointData, sizeof(disjointData), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                context->GetData(bt->slot.start.Get(), &tStart, sizeof(tStart), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                context->GetData(bt->slot.end.Get(), &tEnd, sizeof(tEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
                if (!disjointData.Disjoint && disjointData.Frequency > 0 && tEnd >= tStart) {
                    bt->lastMs = static_cast<float>(static_cast<double>(tEnd - tStart) * 1000.0 /
                        static_cast<double>(disjointData.Frequency));
                }
                bt->slot.inFlight = false;
            }
        }
    }
    const bool canQueryGpu = bt && !bt->slot.inFlight && bt->slot.disjoint && bt->slot.start && bt->slot.end;
    if (canQueryGpu) {
        context->Begin(bt->slot.disjoint.Get());
        context->End(bt->slot.start.Get());
    }

    bool evalSuccess = false;
    {
        EvaluatingUpscalerGuard evalGuard;
        evalSuccess = backend->evaluate(context, inputs, destinationRtv, destinationTexture);
        if (evalSuccess && backendType != UpscalerBackend::AmdFsr && inputs.sharpness > 0.0f) {
            ID3D11Texture2D* sharpenTarget = destinationTexture ? destinationTexture : inputs.color;
            if (!sharpenPass_.apply(context, sharpenTarget, inputs.sharpness, inputs.colorIsHdr)) {
                static bool s_sharpenFailLogged = false;
                if (!s_sharpenFailLogged) {
                    s_sharpenFailLogged = true;
                    EdpeLog(L"EDPE: sharpen pass failed; output left unsharpened");
                }
            }
        }
    }
    if (canQueryGpu) {
        context->End(bt->slot.end.Get());
        context->End(bt->slot.disjoint.Get());
        bt->slot.inFlight = true;
    }
    return evalSuccess;
}

bool UpscalerManager::isBackendModeSupported(UpscalerBackend backendType) const {
    if (backendType == UpscalerBackend::None || !isBackendAvailable(backendType)) {
        return false;
    }
    switch (backendType) {
    case UpscalerBackend::NvidiaDlss:
        return nvidiaBackend_.isModeSupported(currentMode_);
    case UpscalerBackend::AmdFsr:
        return amdBackend_.isModeSupported(currentMode_);
    case UpscalerBackend::Taa:
        return taaBackend_.isModeSupported(currentMode_);
    default:
        return false;
    }
}

void UpscalerManager::resetBackendHistory(UpscalerBackend backendType) {
    if (backendType == UpscalerBackend::None) return;
    if (auto* backend = getBackendInstance(backendType)) {
        backend->resetHistory();
    }
}

} // namespace edpe