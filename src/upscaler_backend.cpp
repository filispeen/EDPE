#include "upscaler_backend.h"
#include "log.h"
#include "ngx_context.h"

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

void NvidiaUpscalerBackend::shutdown() {
    outputTexture_.Reset();
    outputUav_.Reset();
    outputWidth_ = 0;
    outputHeight_ = 0;
    outputFormat_ = DXGI_FORMAT_UNKNOWN;
    if (initialized_) {
        NgxContext::shutdown();
        initialized_ = false;
    }
}

bool NvidiaUpscalerBackend::ensureOutputTexture(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    if (outputTexture_ && outputWidth_ == width && outputHeight_ == height && outputFormat_ == format) {
        return true;
    }
    outputTexture_.Reset();
    outputUav_.Reset();
    outputWidth_ = 0;
    outputHeight_ = 0;
    outputFormat_ = DXGI_FORMAT_UNKNOWN;

    if (!device || !width || !height) return false;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(device->CreateTexture2D(&desc, nullptr, &outputTexture_))) {
        EdpeLog(L"EDPE: failed to create DLAA intermediate output texture");
        return false;
    }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = format;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;

    if (FAILED(device->CreateUnorderedAccessView(outputTexture_.Get(), &uavDesc, &outputUav_))) {
        EdpeLog(L"EDPE: failed to create DLAA intermediate output UAV");
        outputTexture_.Reset();
        return false;
    }

    outputWidth_ = width;
    outputHeight_ = height;
    outputFormat_ = format;
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

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    context->GetDevice(&device);
    if (!ensureOutputTexture(device.Get(), inputs.outputWidth, inputs.outputHeight, outDesc.Format)) {
        return false;
    }

    ID3D11Resource* colorIn = inputs.color;
    ID3D11Resource* depthIn = inputs.depth;
    ID3D11Resource* motionIn = inputs.motionVectors;
    ID3D11Resource* reactiveIn = inputs.reactiveMask;

    const UpscaleMode mode = UpscalerManager::instance().getMode();
    bool evalOk = NgxContext::evaluateDlss(context, params,
        colorIn, outputTexture_.Get(), depthIn, motionIn, reactiveIn,
        inputs.renderWidth, inputs.renderHeight,
        inputs.outputWidth, inputs.outputHeight,
        mode, NgxContext::get_preset());

    if (!evalOk) {
        return false;
    }

    ID3D11Texture2D* dest = destinationTexture ? destinationTexture : inputs.color;
    if (dest) {
        context->CopyResource(dest, outputTexture_.Get());
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

    uint32_t maxRenderWidth = 0;
    uint32_t maxRenderHeight = 0;
    uint32_t maxOutputWidth = 0;
    uint32_t maxOutputHeight = 0;
    bool currentIsHdr = false;

#if defined(EDPE_ENABLE_FSR)
    std::vector<uint8_t> scratchBuffer;
    FfxInterface ffxInterface{};
    FfxFsr3UpscalerContext fsrContext{};
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
            EdpeLog(L"EDPE: Failed to create AMD output texture");
            return false;
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = format;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        uavDesc.Texture2D.MipSlice = 0;
        hr = device->CreateUnorderedAccessView(outputTexture.Get(), &uavDesc, &outputUav);
        if (FAILED(hr)) {
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
        if (contextCreated) {
            ffxFsr3UpscalerContextDestroy(&fsrContext);
            contextCreated = false;
        }
#endif
    }

    bool ensureFsrContext(ID3D11Device* /*device*/, uint32_t renderW, uint32_t renderH,
                          uint32_t outputW, uint32_t outputH, bool isHdr) {
#if defined(EDPE_ENABLE_FSR)
        if (contextCreated && renderW <= maxRenderWidth && renderH <= maxRenderHeight &&
            outputW == maxOutputWidth && outputH == maxOutputHeight &&
            currentIsHdr == isHdr) {
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
                     FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE |
                     FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE;
        if (isHdr) {
            desc.flags |= FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
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
            swprintf_s(buf, L"EDPE: ffxFsr3UpscalerContextCreate failed (%d)", err);
            EdpeLog(buf);
            return false;
        }
        contextCreated = true;
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
    if (!device) return false;

    size_t scratchSize = ffxGetScratchMemorySizeDX11(FFX_FSR3UPSCALER_CONTEXT_COUNT);
    impl_->scratchBuffer.resize(scratchSize);
    FfxDevice ffxDevice = ffxGetDeviceDX11_Fsr31(device);
    FfxErrorCode err = ffxGetInterfaceDX11(&impl_->ffxInterface, ffxDevice,
        impl_->scratchBuffer.data(), scratchSize, FFX_FSR3UPSCALER_CONTEXT_COUNT);
    if (err != FFX_OK) {
        wchar_t buf[256];
        swprintf_s(buf, L"EDPE: ffxGetInterfaceDX11 failed (%d)", err);
        EdpeLog(buf);
        return false;
    }
    impl_->initialized = true;
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

    D3D11_TEXTURE2D_DESC outDesc{};
    if (destinationTexture) {
        destinationTexture->GetDesc(&outDesc);
    } else {
        inputs.color->GetDesc(&outDesc);
    }

    if (!impl_->ensureOutputTexture(device.Get(), inputs.outputWidth, inputs.outputHeight, outDesc.Format)) {
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
    dispatchDesc.depth = ffxGetResourceDX11_Fsr31(inputs.depth,
        GetFfxResourceDescriptionDX11(inputs.depth), L"FSR3_Depth", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.motionVectors = ffxGetResourceDX11_Fsr31(inputs.motionVectors,
        GetFfxResourceDescriptionDX11(inputs.motionVectors), L"FSR3_MotionVectors", FFX_RESOURCE_STATE_COMPUTE_READ);

    if (inputs.reactiveMask) {
        dispatchDesc.reactive = ffxGetResourceDX11_Fsr31(inputs.reactiveMask,
            GetFfxResourceDescriptionDX11(inputs.reactiveMask), L"FSR3_Reactive", FFX_RESOURCE_STATE_COMPUTE_READ);
    }

    dispatchDesc.output = ffxGetResourceDX11_Fsr31(impl_->outputTexture.Get(),
        GetFfxResourceDescriptionDX11(impl_->outputTexture.Get()), L"FSR3_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);

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
    dispatchDesc.cameraNear = (inputs.cameraNow.depthB > 0.0f)
        ? inputs.cameraNow.depthB
        : 0.1f;
    dispatchDesc.cameraFar = FLT_MAX;
    dispatchDesc.viewSpaceToMetersFactor = 1.0f;

    FfxErrorCode err = ffxFsr3UpscalerContextDispatch(&impl_->fsrContext, &dispatchDesc);
    if (err != FFX_OK) {
        wchar_t buf[256];
        swprintf_s(buf, L"EDPE: ffxFsr3UpscalerContextDispatch failed (%d)", err);
        EdpeLog(buf);
        return false;
    }

    ID3D11Texture2D* dest = destinationTexture ? destinationTexture : inputs.color;
    if (dest) {
        context->CopyResource(dest, impl_->outputTexture.Get());
    }
    return true;
#else
    return false;
#endif
}

void AmdUpscalerBackend::resetHistory() {
    if (impl_) {
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
        inputs.renderHeight);
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

    for (auto& slot : timingRing_) {
        slot = {};
    }
    timingWriteIndex_ = 0;
    lastGpuTimeMs_ = 0.0f;
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

    // Poll oldest GPU timestamp slot non-blockingly before reusing it.
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

    const bool canQueryGpu = context && !slot.inFlight && slot.disjoint && slot.start && slot.end;
    if (canQueryGpu) {
        context->Begin(slot.disjoint.Get());
        context->End(slot.start.Get());
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    const bool evalSuccess = backend->evaluate(context, inputs, destinationRtv, destinationTexture);
    auto t1 = std::chrono::high_resolution_clock::now();
    if (evalSuccess) {
        ++evalCount_;
    }

    if (canQueryGpu) {
        context->End(slot.end.Get());
        context->End(slot.disjoint.Get());
        slot.inFlight = true;
        timingWriteIndex_ = (timingWriteIndex_ + 1) % kTimingRingSize;
    }

    const float cpuTimeMs = std::chrono::duration<float, std::milli>(t1 - t0).count();
    diagnostics_.gpuTimeMs = (lastGpuTimeMs_ > 0.0f) ? lastGpuTimeMs_ : cpuTimeMs;
    diagnostics_.success = evalSuccess;

    if (!evalSuccess) {
        // Fail open requirement: log warning, clear reset, restore normal path.
        swprintf_s(diagnostics_.statusMessage, L"Evaluation failed on %ls; falling back to original output",
            backend->name());
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

    swprintf_s(diagnostics_.statusMessage, L"Reconstructed (%ux%u -> %ux%u) %.3f ms",
        inputs.renderWidth, inputs.renderHeight, inputs.outputWidth, inputs.outputHeight,
        diagnostics_.gpuTimeMs);
    pendingReset_ = false;
    return true;
}

} // namespace edpe