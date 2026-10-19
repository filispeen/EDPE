#pragma once

namespace edpe {

// HLSL shader for full-screen Subpixel Jitter Debug View.
// Converts game background to grayscale/dimmed ("гра стає сірішою") and highlights
// pixels affected by subpixel projection jitter in bright neon green ("зелені пікселі").
inline constexpr char kJitterDebugShader[] = R"(
cbuffer JitterDebugConstants : register(b0) {
    float4 resolution;   // x: width, y: height, z: 1/width, w: 1/height
    float4 jitterParams; // x: jitterPixelX, y: jitterPixelY, z: sensitivity, w: threshold
    float4 debugMode;    // x: mode (0: hybrid, 1: spatial shift, 2: temporal pulse, 3: binary mask), y: resetFlag, z: 0, w: 0
};

Texture2D<float4> currentFrame : register(t0);
Texture2D<float4> historyFrame : register(t1);
Texture2D<float2> motionVectors: register(t2);

SamplerState linearSampler : register(s0);
SamplerState pointSampler  : register(s1);

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOutput vs_main(uint id : SV_VertexID) {
    VSOutput o;
    float2 corner = float2((id << 1) & 2, id & 2);
    o.pos = float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = float2(corner.x, corner.y);
    return o;
}

float4 ps_main(VSOutput pin) : SV_Target {
    float2 uv = pin.uv;
    float3 color = max(currentFrame.SampleLevel(linearSampler, uv, 0).rgb, 0.0);

    // 1. Grayscale desaturated background ("гра стає сірішою")
    float luma = dot(color, float3(0.299, 0.587, 0.114));
    float3 grayBg = float3(luma, luma, luma) * 0.35; // Dimmed grayscale for clear green visibility

    // 2. Spatial subpixel jitter displacement
    // In EDPE, jitter is in pixel units: jitterParams.x, jitterParams.y.
    // Jitter displaced the camera projection by +jitterPixels.
    // An unjittered sample is at (uv + jitterUv).
    float2 jitterUv = float2(jitterParams.x * resolution.z, jitterParams.y * resolution.w);
    float3 unjittered = max(currentFrame.SampleLevel(linearSampler, uv + jitterUv, 0).rgb, 0.0);
    float3 diffSpatial = abs(color - unjittered);
    float spatialDelta = max(diffSpatial.r, max(diffSpatial.g, diffSpatial.b));

    // 3. Temporal difference (frame-to-frame jitter pulsing)
    float2 mvPixels = motionVectors.SampleLevel(pointSampler, uv, 0).xy;
    float2 motionUv = mvPixels * resolution.zw;
    float2 histUv = uv + motionUv;
    float3 histColor = max(historyFrame.SampleLevel(linearSampler, histUv, 0).rgb, 0.0);
    float3 tempDiff = abs(color - histColor);
    float tempDelta = (debugMode.y > 0.5) ? 0.0 : max(tempDiff.r, max(tempDiff.g, tempDiff.b));

    // Evaluate jitter delta based on selected mode:
    // mode 0: Hybrid (spatial shift + temporal pulse)
    // mode 1: Spatial displacement only
    // mode 2: Temporal pulse only
    // mode 3: Binary threshold mask
    int mode = int(debugMode.x + 0.5);
    float delta = 0.0;
    if (mode == 1) {
        delta = spatialDelta * 2.5;
    } else if (mode == 2) {
        delta = tempDelta * 1.5;
    } else {
        delta = max(spatialDelta * 2.0, tempDelta * 1.25);
    }

    float sensitivity = max(jitterParams.z, 0.01);
    float threshold = max(jitterParams.w, 0.001);
    float intensity = delta * sensitivity;

    // Bright vivid neon green
    float3 neonGreen = float3(0.1, 1.0, 0.2);

    float3 finalColor = grayBg;
    if (mode == 3) {
        // Binary mask mode: solid green if above threshold
        if (intensity >= threshold) {
            finalColor = neonGreen;
        }
    } else {
        // Continuous green overlay on affected jitter pixels
        if (intensity > threshold) {
            float factor = saturate((intensity - threshold) / max(threshold * 1.5, 0.005));
            finalColor = lerp(grayBg, neonGreen, factor);
        }
    }

    return float4(finalColor, 1.0);
}
)";

} // namespace edpe
