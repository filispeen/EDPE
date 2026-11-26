#pragma once

namespace edpe {

// Temporal Anti-Aliasing (TAA) HLSL shader.
// Implements jitter-aware 3x3 Gaussian current-frame de-jittering (sigma = 0.47 px),
// reversible Brian Karis tone compression in YCoCg space (HDR-safe for R11G11B10_FLOAT),
// 3x3 nearest-depth motion vector dilation, directed 4x4 disocclusion depth guard,
// Brian Karis Ray-to-AABB color clipping, and 5-tap Catmull-Rom fp16 history reconstruction.
inline constexpr char kTaaShader[] = R"(
cbuffer TaaConstants : register(b0) {
    float4 resolution;       // x: outputWidth, y: outputHeight, z: 1/outputWidth, w: 1/outputHeight
    float4 renderResolution; // x: renderWidth, y: renderHeight, z: 1/renderWidth, w: 1/renderHeight
    float4 params;           // x: jitterX, y: jitterY, z: blendWeight, w: resetFlag
    float4 jitterHistory;    // x: prevJitterX, y: prevJitterY, z: depthB, w: hasPrevDepth
    float4 reactiveInfo;     // x: hasReactiveMask, y: maxBlendCap, zw: unused
};

Texture2D<float4> currentFrame  : register(t0);
Texture2D<float4> historyFrame  : register(t1);
Texture2D<float2> motionVectors : register(t2);
Texture2D<float>  sceneDepth    : register(t3);
Texture2D<float>  prevSceneDepth: register(t4);
Texture2D<float>  motionReactive: register(t5);

SamplerState linearSampler : register(s0);
SamplerState pointSampler  : register(s1);

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

struct PSOutput {
    float4 history : SV_Target0;
    float4 dest    : SV_Target1;
};

VSOutput vs_main(uint id : SV_VertexID) {
    VSOutput o;
    float2 corner = float2((id << 1) & 2, id & 2);
    o.pos = float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = float2(corner.x, corner.y);
    return o;
}

float3 TonemapKaris(float3 c) {
    c = max(c, 0.0);
    float peak = max(c.r, max(c.g, c.b));
    return c / (1.0 + peak);
}

float3 InverseTonemapKaris(float3 c) {
    c = max(c, 0.0);
    float peak = max(c.r, max(c.g, c.b));
    float3 rgb = c / max(1.0 - peak, 1.0 / 65504.0);
    return min(rgb, float3(65024.0, 65024.0, 64512.0));
}

float3 RGBToYCoCg(float3 c) {
    return float3(
         0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
         0.5  * c.r             - 0.5  * c.b,
        -0.25 * c.r + 0.5 * c.g - 0.25 * c.b
    );
}

float3 YCoCgToRGB(float3 c) {
    return max(float3(
        c.x + c.y - c.z,
        c.x + c.z,
        c.x - c.y - c.z
    ), 0.0);
}

// Brian Karis (Epic Games / UE4) AABB Ray Intersection Color Clipping
// Clips vector from history towards target to the bounding box [aabbMin, aabbMax]
// Preserves chromaticity without snapping to arbitrary AABB corners.
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

// Clean 5-tap Catmull-Rom Bicubic texture filter for history reconstruction
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

PSOutput ps_main(VSOutput pin) {
    float2 uv = pin.uv;

    // 1. Jitter-aware 3x3 Gaussian de-jittering + unweighted 3x3 YCoCg statistics
    //    + full 3x3 nearest-depth dilation around centerUv.
    // In EDPE, projection jitter offsets are in pixel units: params.x, params.y.
    // The projection shifted geometry by +jitter in screen pixels.
    float2 targetPos = uv * renderResolution.xy + params.xy;
    float2 baseTexel = floor(targetPos) + 0.5;
    float2 fracOffset = targetPos - baseTexel;
    float2 centerUv = clamp(baseTexel * renderResolution.zw, 0.0, 1.0);

    const float kInvTwoSigmaSq = 1.0 / (2.0 * 0.47 * 0.47);
    float3 currentKaris = 0.0;
    float weightSum = 0.0;
    float centerAlpha = 1.0;

    float3 m1 = 0.0;
    float3 m2 = 0.0;
    float3 boxMin = float3(1e9, 1e9, 1e9);
    float3 boxMax = float3(-1e9, -1e9, -1e9);

    float centerDepth = sceneDepth.SampleLevel(pointSampler, centerUv, 0).r;
    float nearestDepth = centerDepth;
    float minDepth = centerDepth;
    float2 nearestUv = centerUv;
    float4 centerRaw = currentFrame.SampleLevel(pointSampler, centerUv, 0);
    float3 centerKaris = TonemapKaris(centerRaw.rgb);
    float3 nearestKaris = centerKaris;

    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            float2 sampleUv = clamp((baseTexel + float2(x, y)) * renderResolution.zw, 0.0, 1.0);
            float4 rawSample = currentFrame.SampleLevel(pointSampler, sampleUv, 0);
            float3 cKaris = TonemapKaris(rawSample.rgb);
            if (x == 0 && y == 0) {
                centerAlpha = rawSample.a;
            }

            float2 d = float2(x, y) - fracOffset;
            float w = exp(-dot(d, d) * kInvTwoSigmaSq);
            currentKaris += cKaris * w;
            weightSum += w;

            float3 cY = RGBToYCoCg(cKaris);
            m1 += cY;
            m2 += cY * cY;
            boxMin = min(boxMin, cY);
            boxMax = max(boxMax, cY);

            float z = sceneDepth.SampleLevel(pointSampler, sampleUv, 0).r;
            if (z > nearestDepth) {
                nearestDepth = z;
                nearestUv = sampleUv;
                nearestKaris = cKaris;
            }
            minDepth = min(minDepth, z);
        }
    }

    float3 currentY = clamp(RGBToYCoCg(currentKaris / max(weightSum, 1e-5)), boxMin, boxMax);

    float3 mean = m1 * (1.0 / 9.0);
    float3 sigma = sqrt(max(0.0, m2 * (1.0 / 9.0) - mean * mean));

    // 2. Edge-gated dilated motion vector evaluation:
    //    - On smooth sloped surfaces (e.g. hangar floors/walls), use centerUv so depth gradients
    //      do not systematically bias every floor pixel's MV by +1 px toward the camera.
    //    - On true depth silhouettes, only dilate MV from nearestUv when centerUv shares similar
    //      color with the foreground (anti-aliased edge); pure background pixels keep their own
    //      MV at centerUv so they never reproject into foreground history and ghost.
    bool isDepthSilhouette = (nearestDepth > centerDepth * 1.05 + 1e-4);
    bool dilateMv = isDepthSilhouette && (length(centerKaris - nearestKaris) < 0.12);
    float2 mvUv = dilateMv ? nearestUv : centerUv;
    float2 mvPixels = motionVectors.SampleLevel(pointSampler, mvUv, 0).xy;
    float speed = length(mvPixels);

    // Smooth variance-clip gamma: active even at speed == 0 so un-tracked movers do not ghost.
    float gamma = lerp(1.35, 0.80, saturate(speed * 0.25));
    float3 finalMin = max(boxMin, mean - gamma * sigma);
    float3 finalMax = min(boxMax, mean + gamma * sigma);

    float2 priorUv = uv + mvPixels * renderResolution.zw;
    bool isOffscreen = (priorUv.x < 0.0 || priorUv.x > 1.0 || priorUv.y < 0.0 || priorUv.y > 1.0);
    bool isReset = (params.w > 0.5) || isOffscreen;

    if (isReset) {
        float4 outColor = float4(InverseTonemapKaris(YCoCgToRGB(currentY)), centerAlpha);
        PSOutput o;
        o.history = outColor;
        o.dest = outColor;
        return o;
    }

    // 3. Sample history with 5-tap Catmull-Rom Bicubic filtering in Karis-compressed YCoCg space
    float4 histSample = SampleCatmullRom5(historyFrame, linearSampler, priorUv, resolution.xy, resolution.zw);
    float3 histY = RGBToYCoCg(TonemapKaris(histSample.rgb));

    // 4. Disocclusion confidence: prefer MotionPass prevDepthMatches reactive mask when available;
    //    otherwise fall back to directed 4x4 current-footprint guard against nearestDepth.
    float depthConfidence = 1.0;
    float2 prevJitterUv = jitterHistory.xy * renderResolution.zw;
    if (reactiveInfo.x > 0.5) {
        // Sample both centerUv (newly disoccluded background) and nearestUv (dilated silhouette)
        // so foreground dilation never masks a disoccluded pixel, and fully drop stale history.
        float reactiveCenter = motionReactive.SampleLevel(pointSampler, centerUv, 0).r;
        float reactiveMv = motionReactive.SampleLevel(pointSampler, mvUv, 0).r;
        float reactive = max(reactiveCenter, reactiveMv);
        depthConfidence = 1.0 - saturate(reactive);
    } else if (jitterHistory.w > 0.5 && nearestDepth > 0.0 && speed > 0.5) {
        float2 prevUv = clamp(priorUv + prevJitterUv, 0.0, 1.0);
        float pd = prevSceneDepth.SampleLevel(pointSampler, prevUv, 0).r;

        float envMax = nearestDepth;
        float envMin = minDepth;
        float upperThresh = max(envMax * 1.15, envMax + 0.001);
        float lowerThresh = envMin * 0.75 - 0.001;

        if (pd > upperThresh || (envMin > 0.0 && pd < lowerThresh)) {
            // Widen the current 3x3 depth footprint by 1 texel along sign(round(prevPixel) - prevPixel)
            // to form a directed 4x4 guard so subpixel jitter on ridges/silhouettes does not trigger false resets.
            float2 prevPixel = prevUv * renderResolution.xy;
            float2 fracDelta = round(prevPixel) - prevPixel;
            float2 guardDir = float2(fracDelta.x >= 0.0 ? 1.0 : -1.0, fracDelta.y >= 0.0 ? 1.0 : -1.0);

            [unroll]
            for (int k = -1; k <= 1; ++k) {
                float2 uvCol = clamp((baseTexel + float2(2.0 * guardDir.x, (float)k)) * renderResolution.zw, 0.0, 1.0);
                float zCol = sceneDepth.SampleLevel(pointSampler, uvCol, 0).r;
                envMax = max(envMax, zCol);
                envMin = min(envMin, zCol);

                float2 uvRow = clamp((baseTexel + float2((float)k, 2.0 * guardDir.y)) * renderResolution.zw, 0.0, 1.0);
                float zRow = sceneDepth.SampleLevel(pointSampler, uvRow, 0).r;
                envMax = max(envMax, zRow);
                envMin = min(envMin, zRow);
            }
            float2 uvCorner = clamp((baseTexel + 2.0 * guardDir) * renderResolution.zw, 0.0, 1.0);
            float zCorner = sceneDepth.SampleLevel(pointSampler, uvCorner, 0).r;
            envMax = max(envMax, zCorner);
            envMin = min(envMin, zCorner);

            upperThresh = max(envMax * 1.15, envMax + 0.001);
            lowerThresh = envMin * 0.75 - 0.001;

            if (pd > upperThresh) {
                float excess = (pd - upperThresh) / max(envMax, 1e-5);
                depthConfidence = saturate(1.0 - excess * 4.0);
            } else if (envMin > 0.0 && pd < lowerThresh) {
                float deficit = (lowerThresh - pd) / max(envMin, 1e-5);
                depthConfidence = saturate(1.0 - deficit * 4.0);
            }
        }
    }

    // 5. Brian Karis Ray-to-AABB Clipping + symmetric +-1 px stationary sub-pixel ridge retention
    //    Retains static sub-pixel thin lines (< 1 px) across missed Halton jitter phases using
    //    symmetric +-1 px history neighbors (mathematically zero on monotonic step edges and blobs).
    float3 clampedHistY = ClipToAABB(histY, currentY, finalMin, finalMax);
    // Strictly stationary-only (< 0.25 px/f): never bypass ClipToAABB during motion,
    // otherwise grazing-angle 1px floor decals and specular streaks below screen center smear/ghost.
    float stationaryWeight = saturate(1.0 - speed * 4.0) * depthConfidence;
    float thinRidge = 0.0;
    if (stationaryWeight > 0.0) {
        float2 histTexel = resolution.zw;
        float hL  = RGBToYCoCg(TonemapKaris(historyFrame.SampleLevel(linearSampler, clamp(priorUv + float2(-histTexel.x, 0.0), 0.0, 1.0), 0).rgb)).x;
        float hR  = RGBToYCoCg(TonemapKaris(historyFrame.SampleLevel(linearSampler, clamp(priorUv + float2( histTexel.x, 0.0), 0.0, 1.0), 0).rgb)).x;
        float hU  = RGBToYCoCg(TonemapKaris(historyFrame.SampleLevel(linearSampler, clamp(priorUv + float2(0.0, -histTexel.y), 0.0, 1.0), 0).rgb)).x;
        float hD  = RGBToYCoCg(TonemapKaris(historyFrame.SampleLevel(linearSampler, clamp(priorUv + float2(0.0,  histTexel.y), 0.0, 1.0), 0).rgb)).x;
        float hL2 = RGBToYCoCg(TonemapKaris(historyFrame.SampleLevel(linearSampler, clamp(priorUv + float2(-2.0 * histTexel.x, 0.0), 0.0, 1.0), 0).rgb)).x;
        float hR2 = RGBToYCoCg(TonemapKaris(historyFrame.SampleLevel(linearSampler, clamp(priorUv + float2( 2.0 * histTexel.x, 0.0), 0.0, 1.0), 0).rgb)).x;
        float hU2 = RGBToYCoCg(TonemapKaris(historyFrame.SampleLevel(linearSampler, clamp(priorUv + float2(0.0, -2.0 * histTexel.y), 0.0, 1.0), 0).rgb)).x;
        float hD2 = RGBToYCoCg(TonemapKaris(historyFrame.SampleLevel(linearSampler, clamp(priorUv + float2(0.0,  2.0 * histTexel.y), 0.0, 1.0), 0).rgb)).x;
        float ridgeX = max(0.0, abs(histY.x - 0.5 * (hL + hR)) - 0.5 * abs(hL - hR));
        float ridgeY = max(0.0, abs(histY.x - 0.5 * (hU + hD)) - 0.5 * abs(hU - hD));
        float ridgeX2L = max(0.0, abs(histY.x - 0.5 * (hL2 + hR)) - 0.5 * abs(hL2 - hR));
        float ridgeX2R = max(0.0, abs(histY.x - 0.5 * (hL + hR2)) - 0.5 * abs(hL - hR2));
        float ridgeY2U = max(0.0, abs(histY.x - 0.5 * (hU2 + hD)) - 0.5 * abs(hU2 - hD));
        float ridgeY2D = max(0.0, abs(histY.x - 0.5 * (hU + hD2)) - 0.5 * abs(hU - hD2));
        float ridge2X = max(
            min(ridgeX2L, max(0.0, abs(hL - 0.5 * (hL2 + hR)) - 0.5 * abs(hL2 - hR))),
            min(ridgeX2R, max(0.0, abs(hR - 0.5 * (hL + hR2)) - 0.5 * abs(hL - hR2))));
        float ridge2Y = max(
            min(ridgeY2U, max(0.0, abs(hU - 0.5 * (hU2 + hD)) - 0.5 * abs(hU2 - hD))),
            min(ridgeY2D, max(0.0, abs(hD - 0.5 * (hU + hD2)) - 0.5 * abs(hU - hD2))));
        float ridgeStrength = max(max(ridgeX, ridgeY), 0.85 * max(ridge2X, ridge2Y));
        thinRidge = saturate((ridgeStrength - 0.005) * 32.0);
        clampedHistY = lerp(clampedHistY, histY, 0.98 * stationaryWeight * thinRidge);
    }

    // 6. Velocity / Disocclusion Confidence:
    //    Keep strong temporal accumulation when history fits inside the variance box (preserving AA),
    //    and only dampen confidence when history was clipped far outside or flagged disoccluded.
    float clampDistance = length(max(0.0, finalMin - histY) + max(0.0, histY - finalMax));
    float staticVerified = stationaryWeight * (1.0 - saturate(clampDistance * 12.0));
    float staticThinLock = stationaryWeight * thinRidge;
    float normClamp = clampDistance / max(length(sigma), 0.02);
    float speedFactor = saturate(speed * 0.35);
    float historyConfidence = lerp(1.0, 1.0 / (1.0 + normClamp * 0.85), speedFactor) * depthConfidence;

    float blendWeight = clamp(params.z, 0.0, 0.95);
    blendWeight = lerp(blendWeight, 0.96, staticVerified);
    blendWeight = lerp(blendWeight, 0.975, staticThinLock);
    blendWeight = lerp(blendWeight, 0.72, saturate(speed * 0.10));
    blendWeight = min(blendWeight, reactiveInfo.y);

    float dynamicBlend = blendWeight * historyConfidence;

    // 7. Temporal accumulation in Karis-compressed YCoCg space, then inverse Karis
    float3 finalYCoCg = lerp(currentY, clampedHistY, dynamicBlend);
    float3 finalRgb = InverseTonemapKaris(YCoCgToRGB(finalYCoCg));

    float4 outColor = float4(finalRgb, centerAlpha);
    PSOutput o;
    o.history = outColor;
    o.dest = outColor;
    return o;
}
)";

} // namespace edpe
