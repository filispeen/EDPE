#include "upscaler_backend.h"
#include "log.h"
#include "ngx_context.h"

#include <cwchar>

namespace edpe {

// -------------------------------------------------------------------------
// NvidiaUpscalerBackend
// -------------------------------------------------------------------------

bool NvidiaUpscalerBackend::isAvailable() const {
    return NgxContext::is_dlss_available() || NgxContext::is_dlaa_available();
}

const wchar_t* NvidiaUpscalerBackend::unavailableReason() const {
    if (isAvailable()) return L"";
    return L"NGX runtime blocked: input conventions unverified in Elite and shutdown lifecycle safety pending";
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
    if (initialized_) {
        NgxContext::shutdown();
        initialized_ = false;
    }
}

bool NvidiaUpscalerBackend::evaluate(ID3D11DeviceContext* /*context*/,
    const TemporalFrameInputs& inputs,
    ID3D11RenderTargetView* /*destinationRtv*/,
    ID3D11Texture2D* /*destinationTexture*/) {
    if (!isAvailable()) return false;

    // Translate EDPE temporal conventions to NVIDIA NGX expectations.
    NvidiaDlssInputParams params{};
    const wchar_t* reason = nullptr;
    if (!translateToNvidia(inputs, &params, &reason)) {
        wchar_t message[256];
        swprintf_s(message, L"EDPE: NVIDIA input translation failed: %ls", reason ? reason : L"unknown");
        EdpeLog(message);
        return false;
    }

    // NGX evaluation will be wired when runtime safety & Elite conventions are verified.
    return false;
}

void NvidiaUpscalerBackend::resetHistory() {
    NgxContext::set_reset_requested(true);
}

// -------------------------------------------------------------------------
// AmdUpscalerBackend
// -------------------------------------------------------------------------

bool AmdUpscalerBackend::isAvailable() const {
    return false; // Official backend investigation pending.
}

const wchar_t* AmdUpscalerBackend::unavailableReason() const {
    return L"AMD FidelityFX backend pending D3D11/D3D12 interop architecture selection (PLAN.md section 12)";
}

bool AmdUpscalerBackend::isModeSupported(UpscaleMode mode) const {
    return mode == UpscaleMode::NativeAA ||
           mode == UpscaleMode::Quality ||
           mode == UpscaleMode::Balanced ||
           mode == UpscaleMode::Performance ||
           mode == UpscaleMode::UltraPerformance;
}

bool AmdUpscalerBackend::initialize(ID3D11Device* /*device*/, ID3D11DeviceContext* /*context*/) {
    initialized_ = false;
    return false;
}

void AmdUpscalerBackend::shutdown() {
    initialized_ = false;
}

bool AmdUpscalerBackend::evaluate(ID3D11DeviceContext* /*context*/,
    const TemporalFrameInputs& inputs,
    ID3D11RenderTargetView* /*destinationRtv*/,
    ID3D11Texture2D* /*destinationTexture*/) {
    if (!isAvailable()) return false;

    // Translate EDPE temporal conventions to AMD FidelityFX expectations.
    AmdFsrInputParams params{};
    const wchar_t* reason = nullptr;
    if (!translateToAmd(inputs, &params, &reason)) {
        wchar_t message[256];
        swprintf_s(message, L"EDPE: AMD input translation failed: %ls", reason ? reason : L"unknown");
        EdpeLog(message);
        return false;
    }

    return false;
}

void AmdUpscalerBackend::resetHistory() {}

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

    EdpeLog(L"EDPE: UpscalerManager initialized");
    return true;
}

void UpscalerManager::shutdown() {
    nullBackend_.shutdown();
    nvidiaBackend_.shutdown();
    amdBackend_.shutdown();

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

    const bool evalSuccess = backend->evaluate(context, inputs, destinationRtv, destinationTexture);
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

    swprintf_s(diagnostics_.statusMessage, L"Reconstructed (%ux%u -> %ux%u)",
        inputs.renderWidth, inputs.renderHeight, inputs.outputWidth, inputs.outputHeight);
    pendingReset_ = false;
    return true;
}

} // namespace edpe
