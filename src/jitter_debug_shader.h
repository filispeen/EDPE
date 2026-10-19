#pragma once

namespace edpe {

// HLSL shader for full-screen Subpixel Jitter Debug View.
// Converts game background to grayscale/dimmed ("гра стає сірішою") and highlights
// pixels affected by subpixel projection jitter in bright neon green ("зелені пікселі").
inline constexpr char kJitterDebugShader[] = R"(
cbuffer JitterDebugConstants : register(b0) {
    float4 resolution;   // x: renderWidth, y: renderHeight, z: 1/renderWidth, w: 1/renderHeight
    float4 jitterParams; // x: jitterPixelX, y: jitterPixelY, z: sensitivity, w: threshold
    float4 debugMode;    // x: mode (0: hybrid, 1: spatial shift, 2: temporal pulse, 3: binary mask), y: resetFlag, z: isHdr, w: 0
};

Texture2D<float4> currentFrame  : register(t0);
Texture2D<float4> historyFrame  : register(t1);
Texture2D<float2> motionVectors : register(t2);
Texture2D<float>  sceneDepth    : register(t3);

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

float4 SampleCatmullRom5(Texture2D<float4> tex, SamplerState s, float2 uv, float2 texSize, float2 invTexSize) {
    float2 samplePos = uv * texSize;
    float2 tc = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - tc;
    float2 f2 = f * f;
    float2 f3 = f2 * f;

    float2 w0 = f2 - 0.5 * (f3 + f);
    float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    float2 w3 = 0.5 * (f3 - f2);
    float2 w2 = 1.0 - w0 - w1 - w3;

    float2 w12 = w1 + w2;
    float2 tc12 = tc + w2 / w12;

    float4 c0 = tex.SampleLevel(s, float2(tc12.x, tc.y - 1.0) * invTexSize, 0);
    float4 c1 = tex.SampleLevel(s, float2(tc.x - 1.0, tc12.y) * invTexSize, 0);
    float4 c2 = tex.SampleLevel(s, float2(tc12.x, tc12.y) * invTexSize, 0);
    float4 c3 = tex.SampleLevel(s, float2(tc.x + 2.0, tc12.y) * invTexSize, 0);
    float4 c4 = tex.SampleLevel(s, float2(tc12.x, tc.y + 2.0) * invTexSize, 0);

    float totalWeight = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) + (w3.x * w12.y) + (w12.x * w3.y);
    float4 c = c0 * (w12.x * w0.y) + c1 * (w0.x * w12.y) + c2 * (w12.x * w12.y) + c3 * (w3.x * w12.y) + c4 * (w12.x * w3.y);
    return c / max(1e-4, totalWeight);
}

float3 NormalizeColor(float3 c) {
    c = max(c, 0.0);
    return (debugMode.z > 0.5) ? (c / (1.0 + c)) : saturate(c);
}

float4 ps_main(VSOutput pin) : SV_Target {
    float2 uv = pin.uv;
    float3 rawColor = NormalizeColor(currentFrame.SampleLevel(linearSampler, uv, 0).rgb);

    // 1. Subpixel de-jittering identical to kTaaShader:
    // Projection shifted geometry by +jitterPixels in render-space pixels.
    // The unjittered pixel center is sampled at centerUv = uv + jitterUv with 5-tap Catmull-Rom.
    float2 jitterUv = float2(jitterParams.x * resolution.z, jitterParams.y * resolution.w);
    float2 centerUv = uv + jitterUv;
    float4 centerSample = SampleCatmullRom5(currentFrame, linearSampler, centerUv, resolution.xy, resolution.zw);
    float3 unjittered = NormalizeColor(centerSample.rgb);

    float luma = dot(unjittered, float3(0.299, 0.587, 0.114));
    float3 grayBg = float3(luma, luma, luma) * 0.35;

    float3 diffSpatial = abs(rawColor - unjittered);
    float spatialDelta = max(diffSpatial.r, max(diffSpatial.g, diffSpatial.b));

    // 2. Motion vector, depth cockpit stabilization, and history reprojection identical to kTaaShader:
    float rawDepth = sceneDepth.SampleLevel(pointSampler, uv, 0).r;
    float2 mvPixels = motionVectors.SampleLevel(pointSampler, uv, 0).xy;
    bool isCockpit = (rawDepth >= 0.00714);
    if (isCockpit) {
        mvPixels = float2(0.0, 0.0);
    }

    float2 priorUv = uv + mvPixels * resolution.zw;
    bool isOffscreen = (priorUv.x < 0.0 || priorUv.x > 1.0 || priorUv.y < 0.0 || priorUv.y > 1.0);
    bool isReset = (debugMode.y > 0.5) || isOffscreen;

    float4 histSample = SampleCatmullRom5(historyFrame, linearSampler, priorUv, resolution.xy, resolution.zw);
    float3 histColor = NormalizeColor(histSample.rgb);
    float3 tempDiff = abs(rawColor - histColor);
    float tempDelta = isReset ? 0.0 : max(tempDiff.r, max(tempDiff.g, tempDiff.b));

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

    float3 neonGreen = float3(0.1, 1.0, 0.2);

    float3 finalColor = grayBg;
    if (mode == 3) {
        if (intensity >= threshold) {
            finalColor = neonGreen;
        }
    } else {
        if (intensity > threshold) {
            float factor = saturate((intensity - threshold) / max(threshold * 1.5, 0.005));
            finalColor = lerp(grayBg, neonGreen, factor);
        }
    }

    return float4(finalColor, 1.0);
}
)";

} // namespace edpe
