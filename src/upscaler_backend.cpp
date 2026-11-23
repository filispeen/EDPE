#include "upscaler_backend.h"
#include "context_census.h"
#include "log.h"
#include "ngx_context.h"
#include "temporal_math.h"
#include "amd_fsr4_engine.h"
#include "fsr4_logic.h"

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
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#endif

namespace edpe {

unsigned IUpscalerBackend::recommendedJitterPhaseCount(unsigned configuredBasePhases,
    uint32_t renderWidth, uint32_t renderHeight,
    uint32_t outputWidth, uint32_t outputHeight) const {
    return computeEffectiveJitterPhaseCount(configuredBasePhases, renderWidth, renderHeight, outputWidth, outputHeight);
}

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

    uint32_t maxRenderWidth = 0;
    uint32_t maxRenderHeight = 0;
    uint32_t maxOutputWidth = 0;
    uint32_t maxOutputHeight = 0;
    bool currentIsHdr = false;
    std::atomic<float> velocityFactor{AmdUpscalerBackend::kDefaultVelocityFactor};
    float appliedVelocityFactor = -1.0f;
    wchar_t lastErrorReason[256]{};

    // Optional FSR 4 provider: the official FidelityFX D3D12 upscaler through the D3D11/D3D12 bridge. The D3D11
    // FSR 3.1.2 path stays the fallback.
    std::unique_ptr<Fsr4Engine> fsr4;
    ID3D11Device* fsr4Device = nullptr;
    fsr4::ProviderState fsr4State;  // selection, init and failure-gate state (pure logic, see fsr4_logic.h)

    void applyVelocityFactorIfNeeded() {
#if defined(EDPE_ENABLE_FSR)
        if (!contextCreated) return;
        float vf = std::clamp(velocityFactor.load(std::memory_order_relaxed), 0.0f, 1.0f);
        if (std::fabs(vf - appliedVelocityFactor) > 1e-4f) {
            if (ffxFsr3UpscalerSetConstant(&fsrContext, FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR, &vf) == FFX_OK) {
                appliedVelocityFactor = vf;
            }
        }
#endif
    }

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

    void destroyFsrContext() {
#if defined(EDPE_ENABLE_FSR)
        dilatedDepthTexture.Reset();
        dilatedMotionVectorsTexture.Reset();
        reconstructedPrevNearestDepthTexture.Reset();
        appliedVelocityFactor = -1.0f;
        if (contextCreated) {
            ffxFsr3UpscalerContextDestroy(&fsrContext);
            contextCreated = false;
        }
#endif
    }

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

        applyVelocityFactorIfNeeded();
        resetRequested = true;
        return true;
#else
        return false;
#endif
    }
};

AmdUpscalerBackend::AmdUpscalerBackend()
    : impl_(std::make_unique<Impl>()) {
    impl_->fsr4State.setSelected(Fsr4Engine::selectedByConfig());
}

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

unsigned AmdUpscalerBackend::recommendedJitterPhaseCount(unsigned /*configuredBasePhases*/,
    uint32_t renderWidth, uint32_t renderHeight,
    uint32_t outputWidth, uint32_t outputHeight) const {
    // Match FSR 3.1.2's internal fJitterSequenceLength (ffxFsr3UpscalerGetJitterPhaseCount)
    // regardless of configuredBasePhases, because FSR3 ignores user overrides here and its
    // 4-frame LumaInstability window + lock decay depend on 8*scale^2 cycling.
    if (renderWidth == 0 || outputWidth == 0) {
        return 8u;
    }
#if defined(EDPE_ENABLE_FSR)
    const int32_t sdkPhases = ffxFsr3UpscalerGetJitterPhaseCount(
        static_cast<int32_t>(renderWidth),
        static_cast<int32_t>(outputWidth));
    return (sdkPhases >= 8) ? static_cast<unsigned>(sdkPhases) : 8u;
#else
    return computeEffectiveJitterPhaseCount(8u, renderWidth, renderHeight, outputWidth, outputHeight);
#endif
}

void AmdUpscalerBackend::setVelocityFactor(float factor) {
    if (!impl_) return;
    const float clean = std::isfinite(factor) ? std::clamp(factor, 0.0f, 1.0f) : kDefaultVelocityFactor;
    impl_->velocityFactor.store(clean, std::memory_order_relaxed);
}

float AmdUpscalerBackend::getVelocityFactor() const {
    return impl_ ? impl_->velocityFactor.load(std::memory_order_relaxed) : kDefaultVelocityFactor;
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

    // Optional FSR 4 path through the D3D11/D3D12 bridge; the FSR 3.1.2 path above stays the fallback.
    impl_->fsr4 = std::make_unique<Fsr4Engine>();
    impl_->fsr4Device = device;
    impl_->fsr4State.onBackendInitialize();
    // Probe FSR 4 once at startup when its files are present, so the menu can show it as unavailable (with the
    // reason) the way DLSS is, instead of discovering a refusal only after it was selected.
    if (Fsr4Engine::filesPresent()) tryInitFsr4();
    return true;
#else
    return false;
#endif
}

bool AmdUpscalerBackend::tryInitFsr4() {
    if (impl_->fsr4State.isEngineReady()) return true;
    if (!impl_->fsr4 || !impl_->fsr4Device) return false;
    const bool ok = impl_->fsr4->initialize(impl_->fsr4Device);
    impl_->fsr4State.onInitResult(ok);
    if (ok) {
        EdpeLog(L"EDPE: FSR 4 available; the D3D11 FSR 3.1.2 path stays the fallback");
        return true;
    }
    wchar_t buf[320];
    swprintf_s(buf, L"EDPE: FSR 4 unavailable: %ls; using FSR 3.1.2", impl_->fsr4->lastError());
    EdpeLog(buf);
    return false;
}

void AmdUpscalerBackend::setFsr4Selected(bool selected) {
    if (!impl_->fsr4State.setSelected(selected)) return;  // a fresh selection of FSR 4 retries a failed initialization
    EdpeLog(selected ? L"EDPE: AMD FSR provider switched to FSR 4" : L"EDPE: AMD FSR provider switched to FSR 3.1.2");
    resetHistory();  // the two providers keep separate histories
}

void AmdUpscalerBackend::retryFsr4() {
    if (!impl_->fsr4State.retry()) return;
    EdpeLog(L"EDPE: retrying FSR 4 initialization");
    resetHistory();
}

std::string AmdUpscalerBackend::describeProvider() const {
    const char* version = (impl_ && impl_->fsr4) ? impl_->fsr4->providerVersion() : "4";
    return fsr4::formatProviderLabel(impl_->fsr4State.label(), version);
}

bool AmdUpscalerBackend::isFsr4Selected() const { return impl_->fsr4State.isSelected(); }

bool AmdUpscalerBackend::isFsr4Running() const { return impl_->fsr4State.running(); }

bool AmdUpscalerBackend::isFsr4Capable() const { return impl_->fsr4State.isCapable(); }

const wchar_t* AmdUpscalerBackend::fsr4LastError() const {
    return impl_->fsr4 ? impl_->fsr4->lastError() : L"";
}

void AmdUpscalerBackend::shutdown() {
    if (!impl_) return;
    if (impl_->fsr4) impl_->fsr4->shutdown();
    impl_->fsr4State.onShutdown();
    impl_->fsr4Device = nullptr;
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

    // Initialize on first use when the startup probe did not leave the engine active (for example after a retry).
    if (impl_->fsr4State.needsInit()) tryInitFsr4();

    if (impl_->fsr4State.running() && impl_->fsr4) {
        // HDR (linear) color input is not supported by FSR 4 yet: use FSR 3.1.2 while HdrInput is on, without counting
        // these frames as failures.
        if (impl_->fsr4State.setHdrFallback(inputs.colorIsHdr)) {
            EdpeLog(L"EDPE: FSR 4 does not support HDR color input (HdrInput=1); using FSR 3.1.2 while it is on");
        }
    }

    if (impl_->fsr4State.running() && impl_->fsr4 && !impl_->fsr4State.isHdrFallback()) {
        ID3D11Texture2D* fsr4Dest = destinationTexture ? destinationTexture : inputs.color;
        if (impl_->fsr4->evaluate(context, inputs, fsr4Dest)) {
            impl_->fsr4State.onEvaluateSuccess();
            return true;
        }
        const auto failure = impl_->fsr4State.onEvaluateFailure();
        if (failure.logFirstFailure) {
            wchar_t buf[320];
            swprintf_s(buf, L"EDPE: FSR 4 evaluate failed: %ls; falling back to FSR 3.1.2 for this frame",
                       impl_->fsr4->lastError());
            EdpeLog(buf);
        }
        if (failure.disabled) {
            wchar_t buf[200];
            swprintf_s(buf, L"EDPE: FSR 4 disabled after %d consecutive failures; using FSR 3.1.2",
                       fsr4::kMaxConsecutiveFailures);
            EdpeLog(buf);
        }
        // fall through to the D3D11 FSR 3.1.2 path
    }

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
    impl_->applyVelocityFactorIfNeeded();

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

    // Leave dispatchDesc.reactive null (FSR binds 0.0 default) so its built-in de-jittered
    // ShadingChangePyramid, ComputeThinFeatureConfidence, and LumaInstability handle
    // un-vectored effects without wiping accumulation/locks on sub-pixel thin lines.

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
        if (impl_->fsr4) impl_->fsr4->resetHistory();
        impl_->resetRequested = true;
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

const IUpscalerBackend* UpscalerManager::getBackendInstance(UpscalerBackend backend) const {
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

void UpscalerManager::setFsr4Selected(bool selected) {
    if (selected == amdBackend_.isFsr4Selected()) return;
    amdBackend_.setFsr4Selected(selected);
    pendingReset_ = true;
}

bool UpscalerManager::isFsr4Available() const {
    return amdBackend_.isAvailable() && Fsr4Engine::filesPresent() && amdBackend_.isFsr4Capable();
}

const wchar_t* UpscalerManager::getFsr4UnavailableReason() const {
    if (!amdBackend_.isAvailable()) return amdBackend_.unavailableReason();
    if (!Fsr4Engine::filesPresent()) {
        return L"amd_fidelityfx_loader_dx12.dll / amd_fidelityfx_upscaler_dx12.dll not found beside dxgi.dll (or FSR 4 not compiled in)";
    }
    if (!amdBackend_.isFsr4Capable() || (amdBackend_.isFsr4Selected() && !amdBackend_.isFsr4Running())) {
        return amdBackend_.fsr4LastError();
    }
    return L"";
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

bool UpscalerManager::applySharpenToRtv(ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* sourceSrv,
    ID3D11RenderTargetView* targetRtv,
    uint32_t width, uint32_t height,
    float sharpness, bool hdr,
    bool callerRestoresState) {
    EvaluatingUpscalerGuard evalGuard;
    return sharpenPass_.applyToRtv(
        context, sourceSrv, targetRtv, width, height, sharpness, hdr, callerRestoresState);
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

unsigned UpscalerManager::getRecommendedJitterPhaseCount(unsigned configuredBasePhases,
    uint32_t renderWidth, uint32_t renderHeight,
    uint32_t outputWidth, uint32_t outputHeight) const {
    return getBackendInstance(currentBackendType_)->recommendedJitterPhaseCount(
        configuredBasePhases, renderWidth, renderHeight, outputWidth, outputHeight);
}

void UpscalerManager::setFsrVelocityFactor(float factor) {
    amdBackend_.setVelocityFactor(factor);
}

float UpscalerManager::getFsrVelocityFactor() const {
    return amdBackend_.getVelocityFactor();
}

} // namespace edpe