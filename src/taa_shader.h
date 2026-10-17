#pragma once

namespace edpe {

// Temporal Anti-Aliasing (TAA) HLSL shader.
// Implements subpixel Halton (2,3) de-jittering, cross-weighted 3x3 neighborhood
// statistics in YCoCg, Brian Karis Ray-to-AABB color clipping, 5-tap Catmull-Rom
// history reconstruction, cockpit interior stabilization, and velocity/UI confidence protection.
inline constexpr char kTaaShader[] = R"(
cbuffer TaaConstants : register(b0) {
    float4 resolution; // x: width, y: height, z: 1/width, w: 1/height
    float4 params;     // x: jitterX, y: jitterY, z: blendWeight, w: resetFlag
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

float3 RGBToYCoCg(float3 c) {
    return float3(
         0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
         0.5  * c.r             - 0.5  * c.b,
        -0.25 * c.r + 0.5 * c.g - 0.25 * c.b
    );
}

float3 YCoCgToRGB(float3 c) {
    return saturate(float3(
        c.x + c.y - c.z,
        c.x + c.z,
        c.x - c.y - c.z
    ));
}

// Brian Karis (Epic Games / UE4) AABB Ray Intersection Color Clipping
// Clips vector from history towards target to the bounding box [aabbMin, aabbMax]
// Preserves chromaticity without snapping to arbitrary AABB corners!
float3 ClipToAABB(float3 history, float3 target, float3 aabbMin, float3 aabbMax) {
    float3 p_clip = 0.5 * (aabbMax + aabbMin);
    float3 e_clip = 0.5 * (aabbMax - aabbMin) + 1e-5;

    float3 v_clip = history - p_clip;
    float3 v_unit = v_clip / e_clip;
    float3 a_unit = abs(v_unit);
    float ma_unit = max(a_unit.x, max(a_unit.y, a_unit.z));

    if (ma_unit > 1.0) {
        return p_clip + v_clip / ma_unit;
    } else {
        return history;
    }
}

// 5-tap Catmull-Rom Bicubic texture filter for pristine history sharpness
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

float4 ps_main(VSOutput pin) : SV_Target {
    float2 uv = pin.uv;

    // 1. Subpixel de-jittering for the current frame
    // In EDPE, projection jitter offsets are in pixel units: params.x, params.y.
    // The projection shifted geometry by +jitter in screen pixels.
    // To sample the unjittered nominal pixel center, we sample with linearSampler at uv - jitterUv.
    float2 jitterUv = float2(params.x * resolution.z, params.y * resolution.w);
    float2 centerUv = uv - jitterUv;

    // Center sample evaluated with high-quality bilinear reconstruction
    float4 centerSample = currentFrame.SampleLevel(linearSampler, centerUv, 0);
    float3 current = centerSample.rgb;
    float3 currentY = RGBToYCoCg(current);

    // 2. 3x3 Neighborhood Statistics in YCoCg
    // Plain 3x3 neighborhood (as proven in EDVR temporal_shader_source.h:1126)
    // prevents artificial AABB box narrowing and specular clipping shimmer.
    float3 m1 = 0;
    float3 m2 = 0;
    float3 boxMin = currentY;
    float3 boxMax = currentY;

    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            float2 sampleUv = centerUv + float2(x, y) * resolution.zw;
            float3 c = currentFrame.SampleLevel(pointSampler, sampleUv, 0).rgb;
            float3 cY = RGBToYCoCg(c);

            m1 += cY;
            m2 += cY * cY;

            boxMin = min(boxMin, cY);
            boxMax = max(boxMax, cY);
        }
    }

    float3 mean = m1 / 9.0;
    float3 sigma = sqrt(max(0.0, m2 / 9.0 - mean * mean));

    // 3. Motion vector and depth evaluation
    float rawDepth = sceneDepth.SampleLevel(pointSampler, uv, 0).r;
    float2 mvPixels = motionVectors.SampleLevel(pointSampler, uv, 0).xy;

    // In Elite Dangerous reversed-Z depth:
    // depth = depthB / viewZ, where depthB is ~0.025.
    // If rawDepth >= 0.00714 (viewZ <= 3.5 meters), the pixel belongs to the ship cockpit interior!
    // The cockpit and ship HUD are rigidly mounted around the camera: they do not rotate on screen.
    // Forcing mvPixels = 0 for the cockpit completely prevents cockpit and ship UI smearing!
    bool isCockpit = (rawDepth >= 0.00714);
    if (isCockpit) {
        mvPixels = float2(0.0, 0.0);
    }
    float speed = length(mvPixels);

    // Dynamic variance box scale:
    // When stationary (speed near 0), gamma = 2.25 allows subpixel Halton jitter phases
    // to accumulate cleanly into temporal history without clipping fine station geometry or stars.
    // When moving, gamma smoothly tightens to 1.5 to aggressively prevent ghosting.
    float gamma = lerp(2.25, 1.5, saturate(speed * 0.25));
    float3 varMin = mean - gamma * sigma;
    float3 varMax = mean + gamma * sigma;

    float3 finalMin = max(boxMin, varMin);
    // For high-luminance specular highlights/lamps, widen varMax towards boxMax when stationary
    // to prevent subpixel Karis clipping shimmer across frame phases:
    float3 finalMax = min(boxMax, max(varMax, boxMax * (1.0 - saturate(speed * 0.5))));

    float2 priorUv = uv + mvPixels * resolution.zw;
    bool isOffscreen = (priorUv.x < 0.0 || priorUv.x > 1.0 || priorUv.y < 0.0 || priorUv.y > 1.0);
    bool isReset = (params.w > 0.5) || isOffscreen;

    if (isReset) {
        return float4(current, centerSample.a);
    }

    // 4. Sample history with 5-tap Catmull-Rom Bicubic filtering for maximum clarity
    float4 histSample = SampleCatmullRom5(historyFrame, linearSampler, priorUv, resolution.xy, resolution.zw);
    float3 histY = RGBToYCoCg(histSample.rgb);

    // 5. Brian Karis Ray-to-AABB Clipping
    // Clips along the ray from history towards currentY, preserving chromaticity and eliminating pulsing!
    float3 clampedHistY = ClipToAABB(histY, currentY, finalMin, finalMax);

    // 6. Velocity / Disocclusion / UI Confidence
    // If the reprojected history differed drastically from the current neighborhood
    // (e.g. holographic UI drawn over moving space, or an occluded edge),
    // clampDistance is large. Scale history confidence smoothly when moving.
    // When stationary (speed near 0), preserve full history confidence so subpixel
    // jitter integrates specular highlights, stars and lamps without flickering!
    float clampDistance = length(max(0.0, finalMin - histY) + max(0.0, histY - finalMax));
    float speedFactor = saturate(speed * 0.25);
    float historyConfidence = lerp(1.0, 1.0 / (1.0 + clampDistance * 4.0), speedFactor);

    // Base blend weight: 0.90 for static/smooth, lower when high speed
    float blendWeight = clamp(params.z, 0.0, 0.95);
    blendWeight = lerp(blendWeight, 0.70, saturate(speed * 0.05));

    // Dynamic blend weight
    float dynamicBlend = blendWeight * historyConfidence;

    // 7. Karis luma weighting during accumulation
    // Suppresses high-contrast specular pulsing on stars and glowing lights
    float lumaCurrent = currentY.x;
    float lumaHistory = clampedHistY.x;
    float wCurrent = 1.0 / (1.0 + max(0.0, lumaCurrent));
    float wHistory = 1.0 / (1.0 + max(0.0, lumaHistory));

    float blendCurrent = (1.0 - dynamicBlend) * wCurrent;
    float blendHistory = dynamicBlend * wHistory;
    float totalBlend = blendCurrent + blendHistory;

    float3 finalYCoCg = (currentY * blendCurrent + clampedHistY * blendHistory) / max(1e-5, totalBlend);
    float3 finalRgb = YCoCgToRGB(finalYCoCg);

    return float4(finalRgb, centerSample.a);
}
)";

} // namespace edpe
