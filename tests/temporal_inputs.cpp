#include "temporal_inputs.h"
#include "upscaler_backend.h"
#include "ngx_context.h"
#include "config.h"

#include <cmath>
#include <cstring>

namespace {

// Create dummy non-null pointers for interface testing without creating D3D11 device.
ID3D11Texture2D* dummyTexture() {
    return reinterpret_cast<ID3D11Texture2D*>(0x1000);
}

ID3D11ShaderResourceView* dummySrv() {
    return reinterpret_cast<ID3D11ShaderResourceView*>(0x2000);
}

edpe::TemporalFrameInputs createValidTestInputs() {
    edpe::TemporalFrameInputs inputs{};
    inputs.color = dummyTexture();
    inputs.colorSrv = dummySrv();
    inputs.depth = dummyTexture();
    inputs.depthSrv = dummySrv();
    inputs.motionVectors = dummyTexture();
    inputs.motionSrv = dummySrv();
    inputs.renderWidth = 1708;
    inputs.renderHeight = 960;
    inputs.outputWidth = 2560;
    inputs.outputHeight = 1440;
    inputs.jitterX = 0.35f;
    inputs.jitterY = -0.42f;
    inputs.frameTimeMs = 16.6f;
    inputs.reset = false;
    inputs.depthInverted = true;
    inputs.depthInfinite = true;
    inputs.motionJittered = false;
    inputs.motionIsLowRes = true;
    return inputs;
}

} // namespace

int main() {
    // -------------------------------------------------------------------------
    // Test 1: calculateRenderResolution scaling math and edge cases
    // -------------------------------------------------------------------------
    uint32_t rw = 0, rh = 0;

    // Zero dimensions must fail
    if (edpe::calculateRenderResolution(0, 1440, edpe::UpscaleMode::Quality, &rw, &rh)) return 1;
    if (edpe::calculateRenderResolution(2560, 0, edpe::UpscaleMode::Quality, &rw, &rh)) return 2;
    if (edpe::calculateRenderResolution(2560, 1440, edpe::UpscaleMode::Quality, nullptr, &rh)) return 3;

    // Native & NativeAA: exactly 1:1
    if (!edpe::calculateRenderResolution(2560, 1440, edpe::UpscaleMode::Native, &rw, &rh) ||
        rw != 2560 || rh != 1440) return 4;
    if (!edpe::calculateRenderResolution(2560, 1440, edpe::UpscaleMode::NativeAA, &rw, &rh) ||
        rw != 2560 || rh != 1440) return 5;

    // Quality: 2560x1440 -> ~1707x960, rounded to even: 1708x960
    if (!edpe::calculateRenderResolution(2560, 1440, edpe::UpscaleMode::Quality, &rw, &rh) ||
        rw != 1708 || rh != 960) return 6;

    // Performance: 2560x1440 -> exactly 1280x720
    if (!edpe::calculateRenderResolution(2560, 1440, edpe::UpscaleMode::Performance, &rw, &rh) ||
        rw != 1280 || rh != 720) return 7;

    // 4K (3840x2160) Performance: exactly 1920x1080
    if (!edpe::calculateRenderResolution(3840, 2160, edpe::UpscaleMode::Performance, &rw, &rh) ||
        rw != 1920 || rh != 1080) return 8;

    // Render dimensions must always be even and not exceed output
    if ((rw & 1) != 0 || (rh & 1) != 0) return 9;
    if (rw > 3840 || rh > 2160) return 10;

    // -------------------------------------------------------------------------
    // Test 2: TemporalFrameInputs::isValid validation checks
    // -------------------------------------------------------------------------
    auto inputs = createValidTestInputs();
    const wchar_t* reason = nullptr;
    if (!inputs.isValid(&reason)) return 11;

    // Missing color
    auto invalidInputs = inputs;
    invalidInputs.color = nullptr;
    invalidInputs.colorSrv = nullptr;
    if (invalidInputs.isValid(&reason) || !reason) return 12;

    // Missing depth
    invalidInputs = inputs;
    invalidInputs.depth = nullptr;
    invalidInputs.depthSrv = nullptr;
    if (invalidInputs.isValid(&reason) || !reason) return 13;

    // Missing motion vectors
    invalidInputs = inputs;
    invalidInputs.motionVectors = nullptr;
    invalidInputs.motionSrv = nullptr;
    if (invalidInputs.isValid(&reason) || !reason) return 14;

    // Render dimension > output dimension
    invalidInputs = inputs;
    invalidInputs.renderWidth = 3840;
    if (invalidInputs.isValid(&reason) || !reason) return 15;

    // Non-finite jitter
    invalidInputs = inputs;
    invalidInputs.jitterX = std::numeric_limits<float>::quiet_NaN();
    if (invalidInputs.isValid(&reason) || !reason) return 16;
    invalidInputs.jitterX = 0.0f;
    invalidInputs.jitterY = std::numeric_limits<float>::infinity();
    if (invalidInputs.isValid(&reason) || !reason) return 17;

    // Negative frame time
    invalidInputs = inputs;
    invalidInputs.frameTimeMs = -1.0f;
    if (invalidInputs.isValid(&reason) || !reason) return 18;

    // -------------------------------------------------------------------------
    // Test 3: NVIDIA convention translation
    // -------------------------------------------------------------------------
    edpe::NvidiaDlssInputParams nvParams{};
    if (!edpe::translateToNvidia(inputs, &nvParams, &reason)) return 19;
    if (nvParams.jitterX != inputs.jitterX || nvParams.jitterY != inputs.jitterY) return 20;
    if (nvParams.motionScaleX != 1.0f || nvParams.motionScaleY != 1.0f) return 21;
    if (!nvParams.depthInverted || !nvParams.motionLowRes || nvParams.motionJittered) return 22;
    if (!nvParams.isHdr || nvParams.inReset) return 23;

    // Check reset translation
    inputs.reset = true;
    if (!edpe::translateToNvidia(inputs, &nvParams, &reason) || !nvParams.inReset) return 24;
    inputs.reset = false;

    // -------------------------------------------------------------------------
    // Test 4: AMD convention translation
    // -------------------------------------------------------------------------
    edpe::AmdFsrInputParams amdParams{};
    if (!edpe::translateToAmd(inputs, &amdParams, &reason)) return 25;
    if (amdParams.jitterX != inputs.jitterX || amdParams.jitterY != inputs.jitterY) return 26;
    if (amdParams.motionVectorScaleX != 1.0f || amdParams.motionVectorScaleY != 1.0f) return 27;
    if (!amdParams.depthInverted || !amdParams.depthInfinite || amdParams.reset) return 28;

    inputs.reset = true;
    if (!edpe::translateToAmd(inputs, &amdParams, &reason) || !amdParams.reset) return 29;
    inputs.reset = false;

    // -------------------------------------------------------------------------
    // Test 5: UpscalerManager coordinator and fail-open behavior
    // -------------------------------------------------------------------------
    auto& manager = edpe::UpscalerManager::instance();
    if (manager.getBackend() != edpe::UpscalerBackend::None) return 30;
    if (manager.getMode() != edpe::UpscaleMode::Native) return 31;

    // None backend is always available
    if (!manager.isBackendAvailable(edpe::UpscalerBackend::None)) return 32;

    // Vendor backends are gated and safe-disabled
    if (manager.isBackendAvailable(edpe::UpscalerBackend::NvidiaDlss)) return 33;
    if (manager.isBackendAvailable(edpe::UpscalerBackend::AmdFsr)) return 34;

    const wchar_t* nvReason = manager.getBackendUnavailableReason(edpe::UpscalerBackend::NvidiaDlss);
    if (!nvReason || wcslen(nvReason) == 0) return 35;

    const wchar_t* amdReason = manager.getBackendUnavailableReason(edpe::UpscalerBackend::AmdFsr);
    if (!amdReason || wcslen(amdReason) == 0) return 36;

    // Attempting to select unavailable backend fails safely and keeps None
    if (manager.setBackend(edpe::UpscalerBackend::NvidiaDlss)) return 37;
    if (manager.getBackend() != edpe::UpscalerBackend::None) return 38;

    if (manager.setBackend(edpe::UpscalerBackend::AmdFsr)) return 39;
    if (manager.getBackend() != edpe::UpscalerBackend::None) return 40;

    // Evaluate in None mode succeeds (bypass)
    if (!manager.evaluate(nullptr, inputs, nullptr, nullptr)) return 41;
    const auto& diag = manager.getDiagnostics();
    if (!diag.didRun || !diag.success || diag.backend != edpe::UpscalerBackend::None) return 42;

    // Evaluate with invalid inputs fails open (returns false so caller falls back to original frame)
    if (manager.evaluate(nullptr, invalidInputs, nullptr, nullptr)) return 43;
    if (manager.getDiagnostics().success) return 44;

    // -------------------------------------------------------------------------
    // Test 6: DLSS SR presets, optimal settings safety, and Elite SSAA multipliers
    // -------------------------------------------------------------------------
    edpe::NgxContext::set_preset(11u);
    if (edpe::NgxContext::get_preset() != 11u) return 45;
    edpe::NgxContext::set_preset(0u);
    if (edpe::NgxContext::get_preset() != 0u) return 46;

    edpe::DlssOptimalSettings opt{};
    if (edpe::NgxContext::queryOptimalSettings(2560, 1440, edpe::UpscaleMode::Quality, &opt)) return 47;
    if (opt.supported) return 48;

    if (std::fabs(edpe::Config::getTargetSsaaMultiplier(0, 2) - 1.0f) > 1e-5f) return 49;
    if (std::fabs(edpe::Config::getTargetSsaaMultiplier(1, 1) - 1.0f) > 1e-5f) return 50;
    if (std::fabs(edpe::Config::getTargetSsaaMultiplier(1, 2) - 0.666667f) > 1e-5f) return 51;
    if (std::fabs(edpe::Config::getTargetSsaaMultiplier(1, 3) - 0.580000f) > 1e-5f) return 52;
    if (std::fabs(edpe::Config::getTargetSsaaMultiplier(1, 4) - 0.500000f) > 1e-5f) return 53;
    if (std::fabs(edpe::Config::getTargetSsaaMultiplier(1, 5) - 0.333333f) > 1e-5f) return 54;

    // -------------------------------------------------------------------------
    // Test 7: selectModeByRatio validation
    // -------------------------------------------------------------------------
    if (edpe::selectModeByRatio(2560, 2560) != edpe::UpscaleMode::NativeAA) return 55;
    if (edpe::selectModeByRatio(3840, 2560) != edpe::UpscaleMode::NativeAA) return 56;
    if (edpe::selectModeByRatio(0, 2560) != edpe::UpscaleMode::UltraPerformance) return 57;

    if (edpe::selectModeByRatio(1707, 2560) != edpe::UpscaleMode::Quality) return 58;
    if (edpe::selectModeByRatio(1664, 2560) != edpe::UpscaleMode::Quality) return 59; // 0.65x
    if (edpe::selectModeByRatio(1485, 2560) != edpe::UpscaleMode::Balanced) return 60; // 0.58x
    if (edpe::selectModeByRatio(1280, 2560) != edpe::UpscaleMode::Performance) return 61; // 0.50x
    if (edpe::selectModeByRatio(853, 2560) != edpe::UpscaleMode::UltraPerformance) return 62; // 0.33x

    // -------------------------------------------------------------------------
    // Test 8: AMD FSR backend interface and translation contract
    // -------------------------------------------------------------------------
    edpe::AmdFsrInputParams amdParams8{};
    const wchar_t* amdReason8 = nullptr;
    if (!edpe::translateToAmd(inputs, &amdParams8, &amdReason8)) return 63;
    if (std::fabs(amdParams8.jitterX - inputs.jitterX) > 1e-6f) return 64;
    if (std::fabs(amdParams8.jitterY - inputs.jitterY) > 1e-6f) return 65;
    if (std::fabs(amdParams8.motionVectorScaleX - inputs.motionScaleX) > 1e-6f) return 66;
    if (std::fabs(amdParams8.motionVectorScaleY - inputs.motionScaleY) > 1e-6f) return 67;
    if (!amdParams8.depthInverted || !amdParams8.depthInfinite) return 68;

    // AmdUpscalerBackend instantiation and lifecycle
    edpe::AmdUpscalerBackend amdBackend;
    if (amdBackend.backendType() != edpe::UpscalerBackend::AmdFsr) return 69;
    if (std::wcscmp(amdBackend.name(), L"AMD FSR") != 0) return 70;
    // Without D3D11 device initialized, evaluate should fail open safely
    if (amdBackend.evaluate(nullptr, inputs, nullptr, nullptr)) return 71;

    return 0; // All temporal input layer tests passed
}
