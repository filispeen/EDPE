#pragma once

namespace edpe {

// Temporal Anti-Aliasing (TAA) HLSL shader.
// Implements subpixel Halton (2,3) de-jittering, 5-tap cross nearest-depth motion vector dilation,
// 3x3 neighborhood statistics in YCoCg around the de-jittered sample, Brian Karis Ray-to-AABB
// color clipping, 5-tap Catmull-Rom history reconstruction, and depth-based disocclusion rejection.
inline constexpr char kTaaShader[] = R"(
cbuffer TaaConstants : register(b0) {
    float4 resolution;       // x: outputWidth, y: outputHeight, z: 1/outputWidth, w: 1/outputHeight
    float4 renderResolution; // x: renderWidth, y: renderHeight, z: 1/renderWidth, w: 1/renderHeight
    float4 params;           // x: jitterX, y: jitterY, z: blendWeight, w: resetFlag
    float4 jitterHistory;    // x: prevJitterX, y: prevJitterY, z: depthB, w: hasPrevDepth
};

Texture2D<float4> currentFrame  : register(t0);
Texture2D<float4> historyFrame  : register(t1);
Texture2D<float2> motionVectors : register(t2);
Texture2D<float>  sceneDepth    : register(t3);
Texture2D<float>  prevSceneDepth: register(t4);

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
    // To sample the feature that corresponds to the nominal unjittered pixel center, we sample at uv + jitterUv.
    float2 jitterUv = float2(params.x * renderResolution.z, params.y * renderResolution.w);
    float2 centerUv = clamp(uv + jitterUv, 0.0, 1.0);

    // Hardware bilinear sample at de-jittered center (clamped to 3x3 neighborhood box below)
    float4 centerSample = currentFrame.SampleLevel(linearSampler, centerUv, 0);
    float3 current = max(centerSample.rgb, 0.0);
    float3 currentY = RGBToYCoCg(current);

    // 2. 3x3 Neighborhood Statistics in YCoCg around centerUv
    float3 m1 = 0;
    float3 m2 = 0;
    float3 boxMin = float3(1e9, 1e9, 1e9);
    float3 boxMax = float3(-1e9, -1e9, -1e9);

    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            float2 sampleUv = centerUv + float2(x, y) * renderResolution.zw;
            float3 c = currentFrame.SampleLevel(pointSampler, sampleUv, 0).rgb;
            float3 cY = RGBToYCoCg(c);

            m1 += cY;
            m2 += cY * cY;

            boxMin = min(boxMin, cY);
            boxMax = max(boxMax, cY);
        }
    }

    // 5-tap cross/corner nearest-depth dilation (Reversed-Z: larger depth = closer to camera)
    float centerDepth = sceneDepth.SampleLevel(pointSampler, uv, 0).r;
    float nearestDepth = centerDepth;
    float2 nearestUv = uv;

    float2 d00 = uv + float2(-1.0, -1.0) * renderResolution.zw;
    float2 d10 = uv + float2( 1.0, -1.0) * renderResolution.zw;
    float2 d01 = uv + float2(-1.0,  1.0) * renderResolution.zw;
    float2 d11 = uv + float2( 1.0,  1.0) * renderResolution.zw;

    float z00 = sceneDepth.SampleLevel(pointSampler, d00, 0).r;
    if (z00 > nearestDepth) { nearestDepth = z00; nearestUv = d00; }
    float z10 = sceneDepth.SampleLevel(pointSampler, d10, 0).r;
    if (z10 > nearestDepth) { nearestDepth = z10; nearestUv = d10; }
    float z01 = sceneDepth.SampleLevel(pointSampler, d01, 0).r;
    if (z01 > nearestDepth) { nearestDepth = z01; nearestUv = d01; }
    float z11 = sceneDepth.SampleLevel(pointSampler, d11, 0).r;
    if (z11 > nearestDepth) { nearestDepth = z11; nearestUv = d11; }

    currentY = clamp(currentY, boxMin, boxMax);
    current = YCoCgToRGB(currentY);

    float3 mean = m1 * (1.0 / 9.0);
    float3 sigma = sqrt(max(0.0, m2 * (1.0 / 9.0) - mean * mean));

    // 3. Dilated motion vector evaluation (motionVectors already exclude jitter and handle cockpit depth validation)
    float2 mvPixels = motionVectors.SampleLevel(pointSampler, nearestUv, 0).xy;
    float speed = length(mvPixels);

    // Dynamic variance box scale:
    // Tightens under camera rotation/movement so stale foreground colors at geometry silhouettes
    // cannot linger inside a wide neighborhood box and stretch geometry along the motion direction.
    float gamma = lerp(1.75, 0.85, saturate(speed * 0.35));
    float3 varMin = mean - gamma * sigma;
    float3 varMax = mean + gamma * sigma;

    float3 finalMin = max(boxMin, varMin);
    float stationaryBoost = saturate(1.0 - speed * 2.0);
    float3 finalMax = min(boxMax, lerp(varMax, boxMax, stationaryBoost * 0.5));

    float2 priorUv = uv + mvPixels * renderResolution.zw;
    bool isOffscreen = (priorUv.x < 0.0 || priorUv.x > 1.0 || priorUv.y < 0.0 || priorUv.y > 1.0);
    bool isReset = (params.w > 0.5) || isOffscreen;

    if (isReset) {
        return float4(current, centerSample.a);
    }

    // 4. Sample history with 5-tap Catmull-Rom Bicubic filtering for maximum clarity
    float4 histSample = SampleCatmullRom5(historyFrame, linearSampler, priorUv, resolution.xy, resolution.zw);
    float3 histY = RGBToYCoCg(histSample.rgb);

    // 5. Fast disocclusion check against previous frame depth buffer
    float depthConfidence = 1.0;
    if (jitterHistory.w > 0.5 && speed > 0.5 && centerDepth > 0.0) {
        float2 prevJitterUv = jitterHistory.xy * renderResolution.zw;
        float pd = prevSceneDepth.SampleLevel(pointSampler, priorUv + prevJitterUv, 0).r;
        if (pd > max(centerDepth * 1.06, centerDepth + 0.0003)) {
            depthConfidence = 0.0;
        } else {
            float relDiff = abs(pd - centerDepth) / max(centerDepth, 1e-6);
            if (relDiff > 0.06) {
                depthConfidence = saturate(1.0 - (relDiff - 0.06) * 8.0);
            }
        }
    }

    // 6. Brian Karis Ray-to-AABB Clipping
    float3 clampedHistY = ClipToAABB(histY, currentY, finalMin, finalMax);

    // 7. Velocity / Disocclusion / UI Confidence
    float clampDistance = length(max(0.0, finalMin - histY) + max(0.0, histY - finalMax));
    float speedFactor = saturate(speed * 0.35);
    float historyConfidence = lerp(1.0, 1.0 / (1.0 + clampDistance * 6.0), speedFactor) * depthConfidence;

    // Base blend weight: 0.90 for static/smooth, lower when high speed
    float blendWeight = clamp(params.z, 0.0, 0.95);
    blendWeight = lerp(blendWeight, 0.65, saturate(speed * 0.08));

    // Dynamic blend weight
    float dynamicBlend = blendWeight * historyConfidence;

    // 8. Karis luma weighting during accumulation
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
