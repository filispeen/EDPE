#pragma once

#include "temporal_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <d3d11.h>

namespace edpe {

// Upscaler backend identity (AGENTS.md and PLAN.md section 4).
enum class UpscalerBackend : uint32_t {
    None = 0,
    NvidiaDlss = 1,
    AmdFsr = 2,
    Taa = 3
};

// Target reconstruction mode.
enum class UpscaleMode : uint32_t {
    Native = 0,           // 1:1 bypass / original game presentation
    NativeAA = 1,         // 1:1 native-resolution temporal AA (DLAA / FSR Native AA / TAA)
    Quality = 2,          // ~1.5x scaling per dimension (66.7% resolution)
    Balanced = 3,         // ~1.7x scaling per dimension (58.8% resolution)
    Performance = 4,      // 2.0x scaling per dimension (50.0% resolution)
    UltraPerformance = 5  // 3.0x scaling per dimension (33.3% resolution)
};

inline const wchar_t* getUpscalerBackendName(UpscalerBackend backend) {
    switch (backend) {
    case UpscalerBackend::None: return L"None (Bypass)";
    case UpscalerBackend::NvidiaDlss: return L"NVIDIA DLSS / DLAA";
    case UpscalerBackend::AmdFsr: return L"AMD FSR";
    case UpscalerBackend::Taa: return L"TAA";
    default: return L"Unknown";
    }
}

inline const wchar_t* getUpscaleModeName(UpscaleMode mode) {
    switch (mode) {
    case UpscaleMode::Native: return L"Native";
    case UpscaleMode::NativeAA: return L"Native AA";
    case UpscaleMode::Quality: return L"Quality";
    case UpscaleMode::Balanced: return L"Balanced";
    case UpscaleMode::Performance: return L"Performance";
    case UpscaleMode::UltraPerformance: return L"Ultra Performance";
    default: return L"Unknown";
    }
}

inline float getUpscaleScaleFactor(UpscaleMode mode) {
    switch (mode) {
    case UpscaleMode::Native:
    case UpscaleMode::NativeAA:
        return 1.0f;
    case UpscaleMode::Quality:
        return 1.5f;
    case UpscaleMode::Balanced:
        return 1.7f;
    case UpscaleMode::Performance:
        return 2.0f;
    case UpscaleMode::UltraPerformance:
        return 3.0f;
    default:
        return 1.0f;
    }
}

// Computes input render resolution from presentation output resolution.
// Dimensions are rounded to even integers for GPU texture alignment.
inline bool calculateRenderResolution(uint32_t outputWidth, uint32_t outputHeight,
    UpscaleMode mode, uint32_t* outRenderWidth, uint32_t* outRenderHeight) {
    if (!outRenderWidth || !outRenderHeight || !outputWidth || !outputHeight) return false;
    if (mode == UpscaleMode::Native || mode == UpscaleMode::NativeAA) {
        *outRenderWidth = outputWidth;
        *outRenderHeight = outputHeight;
        return true;
    }
    const float scale = getUpscaleScaleFactor(mode);
    if (scale <= 0.0f) return false;

    uint32_t rw = static_cast<uint32_t>(std::round(static_cast<float>(outputWidth) / scale));
    uint32_t rh = static_cast<uint32_t>(std::round(static_cast<float>(outputHeight) / scale));
    // Ensure even dimensions and at least 2x2.
    rw = (std::max(2u, rw) + 1u) & ~1u;
    rh = (std::max(2u, rh) + 1u) & ~1u;

    // Render dimensions must not exceed output dimensions.
    rw = std::min(rw, outputWidth);
    rh = std::min(rh, outputHeight);

    *outRenderWidth = rw;
    *outRenderHeight = rh;
    return true;
}

// Automatically selects the closest UpscaleMode based on actual render vs output resolution.
inline UpscaleMode selectModeByRatio(uint32_t renderWidth, uint32_t outputWidth) {
    if (!outputWidth || renderWidth >= outputWidth) return UpscaleMode::NativeAA;
    const float ratio = static_cast<float>(renderWidth) / static_cast<float>(outputWidth);
    if (ratio >= 0.62f) return UpscaleMode::Quality;
    if (ratio >= 0.54f) return UpscaleMode::Balanced;
    if (ratio >= 0.42f) return UpscaleMode::Performance;
    return UpscaleMode::UltraPerformance;
}

// Vendor-neutral temporal inputs owned by EDPE (AGENTS.md & PLAN.md section 4/27).
// Represents the unified handoff boundary to any upscaler backend.
struct TemporalFrameInputs {
    // Primary scene textures (D3D11 resources for the 2D path).
    ID3D11Texture2D* color = nullptr;
    ID3D11ShaderResourceView* colorSrv = nullptr;

    ID3D11Texture2D* depth = nullptr;
    ID3D11ShaderResourceView* depthSrv = nullptr;

    ID3D11Texture2D* motionVectors = nullptr;
    ID3D11ShaderResourceView* motionSrv = nullptr;

    // Optional reactive / composition masks.
    ID3D11Texture2D* reactiveMask = nullptr;
    ID3D11ShaderResourceView* reactiveMaskSrv = nullptr;

    ID3D11Texture2D* transparencyMask = nullptr;
    ID3D11ShaderResourceView* transparencyMaskSrv = nullptr;

    ID3D11Texture2D* exposure = nullptr;
    ID3D11ShaderResourceView* exposureSrv = nullptr;

    // Dimensions.
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;

    // Subpixel jitter in render pixels (EDPE Halton sequence convention).
    float jitterX = 0.0f;
    float jitterY = 0.0f;

    // Elapsed frame delta time in milliseconds.
    float frameTimeMs = 0.0f;

    // Camera discontinuity / scene cut / history invalidation flag.
    bool reset = false;

    // Camera projection states (current and previous frame).
    CameraProjection cameraNow{};
    CameraProjection cameraPrevious{};

    // Depth buffer convention: Elite uses reversed Z (depth = depthB / viewZ, clear = 0.0).
    bool depthInverted = true;
    bool depthInfinite = true;

    // Motion vector format convention: EDPE writes current->previous displacement in render pixels.
    bool motionJittered = false; // EDPE motion excludes subpixel projection jitter
    bool motionIsLowRes = true;  // Motion generated at render resolution
    bool colorIsHdr = true;      // True for linear HDR scene targets, false for tone-mapped LDR backbuffer
    float motionScaleX = 1.0f;   // Motion scale multiplier (supports sign inversion for debug/tuning)
    float motionScaleY = 1.0f;

    // Validates that all required inputs are present, finite, and dimensionally coherent.
    bool isValid(const wchar_t** outReason = nullptr) const {
        if (!renderWidth || !renderHeight) {
            if (outReason) *outReason = L"render dimensions must be non-zero";
            return false;
        }
        if (!outputWidth || !outputHeight) {
            if (outReason) *outReason = L"output dimensions must be non-zero";
            return false;
        }
        if (renderWidth > outputWidth || renderHeight > outputHeight) {
            if (outReason) *outReason = L"render dimensions exceed output dimensions";
            return false;
        }
        if (!color && !colorSrv) {
            if (outReason) *outReason = L"color resource missing";
            return false;
        }
        if (!depth && !depthSrv) {
            if (outReason) *outReason = L"depth resource missing";
            return false;
        }
        if (!motionVectors && !motionSrv) {
            if (outReason) *outReason = L"motion vectors resource missing";
            return false;
        }
        if (!std::isfinite(jitterX) || !std::isfinite(jitterY)) {
            if (outReason) *outReason = L"jitter values must be finite";
            return false;
        }
        if (!std::isfinite(frameTimeMs) || frameTimeMs < 0.0f) {
            if (outReason) *outReason = L"frame time must be non-negative and finite";
            return false;
        }
        return true;
    }
};

// NVIDIA DLSS / DLAA translated input parameters (docs/nvidia-dlaa-input-contract.md).
struct NvidiaDlssInputParams {
    float jitterX = 0.0f;         // In input/render pixels
    float jitterY = 0.0f;
    float motionScaleX = 1.0f;    // Motion scale multiplier
    float motionScaleY = 1.0f;
    bool depthInverted = true;    // NVSDK_NGX_DLSS_Feature_Flags_DepthInverted
    bool motionLowRes = true;     // NVSDK_NGX_DLSS_Feature_Flags_MVLowRes
    bool motionJittered = false;  // NVSDK_NGX_DLSS_Feature_Flags_MVJittered
    bool isHdr = true;            // NVSDK_NGX_DLSS_Feature_Flags_IsHDR
    bool inReset = false;         // InReset = 1 on scene cut or camera jump
    float frameTimeMs = 0.0f;
};

// Translates EDPE temporal conventions to NVIDIA NGX conventions.
inline bool translateToNvidia(const TemporalFrameInputs& inputs,
    NvidiaDlssInputParams* outParams, const wchar_t** outReason = nullptr) {
    if (!outParams) {
        if (outReason) *outReason = L"null output params pointer";
        return false;
    }
    if (!inputs.isValid(outReason)) return false;

    // Jitter: NGX expects jitter in input/render pixels (same unit as EDPE Halton).
    outParams->jitterX = inputs.jitterX;
    outParams->jitterY = inputs.jitterY;

    // Motion vector scale: EDPE generates displacement in render pixels.
    outParams->motionScaleX = inputs.motionScaleX;
    outParams->motionScaleY = inputs.motionScaleY;

    // Depth convention: Elite's reversed Z maps to DepthInverted.
    outParams->depthInverted = inputs.depthInverted;

    // Feature flags:
    outParams->motionLowRes = inputs.motionIsLowRes;
    outParams->motionJittered = inputs.motionJittered;
    outParams->isHdr = inputs.colorIsHdr;
    outParams->inReset = inputs.reset;
    outParams->frameTimeMs = inputs.frameTimeMs;
    return true;
}

// AMD FidelityFX FSR translated input parameters (docs/amd-fsr-input-contract.md).
struct AmdFsrInputParams {
    float jitterX = 0.0f;             // In render pixels
    float jitterY = 0.0f;
    float motionVectorScaleX = 1.0f;  // Screen pixel scale
    float motionVectorScaleY = 1.0f;
    bool depthInverted = true;        // FFX_FSR_ENABLE_DEPTH_INVERTED
    bool depthInfinite = true;        // FFX_FSR_ENABLE_DEPTH_INFINITE
    bool reset = false;               // reset flag on camera jump
    float frameTimeDeltaMs = 0.0f;
};

// Translates EDPE temporal conventions to AMD FidelityFX FSR conventions.
inline bool translateToAmd(const TemporalFrameInputs& inputs,
    AmdFsrInputParams* outParams, const wchar_t** outReason = nullptr) {
    if (!outParams) {
        if (outReason) *outReason = L"null output params pointer";
        return false;
    }
    if (!inputs.isValid(outReason)) return false;

    // Jitter: AMD FSR expects jitter applied to color/depth in pixel units.
    outParams->jitterX = inputs.jitterX;
    outParams->jitterY = inputs.jitterY;

    // Motion vector scale: EDPE generates render pixels; FSR expects screen pixels.
    outParams->motionVectorScaleX = inputs.motionScaleX;
    outParams->motionVectorScaleY = inputs.motionScaleY;

    // Depth convention: Elite reversed Z and infinite far plane.
    outParams->depthInverted = inputs.depthInverted;
    outParams->depthInfinite = inputs.depthInfinite;

    outParams->reset = inputs.reset;
    outParams->frameTimeDeltaMs = inputs.frameTimeMs;
    return true;
}

} // namespace edpe
