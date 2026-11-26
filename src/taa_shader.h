#pragma once

namespace edpe {

// ============================================================================
// EDPE TAAv2 - Pass 1: Prepass (Closest-Depth 3x3 Velocity Dilation,
// Plane-Predicted Depth Disocclusion, Thin-Feature Lock Candidate)
// Target: cs_5_0 (Direct3D 11, compiled with -O1 for Wine/Proton compatibility)
//
// EDPE Resource Conventions:
//   OutMotion (R16G16_FLOAT): prev - curr in render pixels (+Y down, unjittered)
//   OutDepth / PrevDepth (R32_FLOAT): infinite Reversed-Z (depth = depthB / viewZ; sky <= 1e-6)
//   OutClass (R8_UINT): 0=Sky, 1=World, 2=CockpitStatic, 3=Disoccluded, 4=Weapon, 5=Celestial, 6=OffscreenFG, 7=Reset
//   OutReactive (R8_UNORM): MotionPass disocclusion/reactive mask
// ============================================================================
inline constexpr char kTaaPrepassShader[] = R"(
static const uint kClassSky           = 0u;
static const uint kClassWorld         = 1u;
static const uint kClassCockpitStatic = 2u;
static const uint kClassDisoccluded   = 3u;
static const uint kClassWeapon        = 4u;
static const uint kClassCelestial     = 5u;
static const uint kClassOffscreenFG   = 6u;
static const uint kClassReset         = 7u;

cbuffer TaaPrepassCB : register(b0) {
    float4 renderRes;    // x: width, y: height, z: 1/width, w: 1/height
    float4 jitterParams; // x: currJitterPx.x, y: currJitterPx.y, z: prevJitterPx.x, w: prevJitterPx.y
    float4 depthParams;  // x: depthB, yzw: unused
    uint4  flags;        // x: bit0=hasPrevDepth, bit1=hasClass, bit2=hasReactive, bit3=hasMotion, bit4=hasDepth
};

Texture2D<float4> g_CurrColor     : register(t0);
Texture2D<float>  g_CurrDepth     : register(t1);
Texture2D<float>  g_PrevDepth     : register(t2);
Texture2D<float2> g_MotionVectors : register(t3);
Texture2D<uint>   g_MotionClass   : register(t4);
Texture2D<float>  g_ReactiveMask  : register(t5);

// R16G16B16A16_FLOAT: RG = dilated MV (UV, prev - curr), B = disocclusion [0..1], A = thinFeature [0..1]
RWTexture2D<float4> g_OutPrepass  : register(u0);

SamplerState g_PointClamp : register(s0);

float3 KarisForward(float3 c) {
    c = max(c, 0.0f);
    float peak = max(c.r, max(c.g, c.b));
    return c / (1.0f + peak);
}

float KarisLuma(float3 c) {
    float l = dot(max(c, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
    return l / (1.0f + l);
}

[numthreads(8, 8, 1)]
void cs_taa_prepass(uint3 dtid : SV_DispatchThreadID) {
    int2 px = int2(dtid.xy);
    int2 maxPx = int2(renderRes.xy) - 1;
    if (px.x > maxPx.x || px.y > maxPx.y) {
        return;
    }

    uint modeFlags = flags.x;
    bool hasPrevDepth = (modeFlags & 1u) != 0u;
    bool hasClass     = (modeFlags & 2u) != 0u;
    bool hasReactive  = (modeFlags & 4u) != 0u;
    bool hasMotion    = (modeFlags & 8u) != 0u;
    bool hasDepth     = (modeFlags & 16u) != 0u;

    uint centerClass = hasClass ? g_MotionClass.Load(int3(px, 0)) : kClassWorld;
    float centerDepthRaw = hasDepth ? g_CurrDepth.Load(int3(px, 0)) : 0.0f;
    float3 centerRgb = g_CurrColor.Load(int3(px, 0)).rgb;
    float3 centerKaris = KarisForward(centerRgb);

    bool centerIsSky = (hasDepth && centerDepthRaw <= 1e-6f) || (hasClass && centerClass == kClassSky);
    bool isCockpitStatic = (hasClass && centerClass == kClassCockpitStatic);

    // 1. Edge-gated closest-depth 3x3 velocity dilation (Reversed-Z, cockpit protected)
    int2 bestOffset = int2(0, 0);
    float bestDepthRaw = centerDepthRaw;
    float3 bestKaris = centerKaris;

    if (hasDepth && !isCockpitStatic) {
        [unroll]
        for (int dy = -1; dy <= 1; ++dy) {
            [unroll]
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                int2 p = clamp(px + int2(dx, dy), int2(0, 0), maxPx);
                if (hasClass && g_MotionClass.Load(int3(p, 0)) == kClassCockpitStatic) {
                    continue;
                }
                float d = g_CurrDepth.Load(int3(p, 0));
                if (d > bestDepthRaw) {
                    bestDepthRaw = d;
                    bestOffset = int2(dx, dy);
                    bestKaris = KarisForward(g_CurrColor.Load(int3(p, 0)).rgb);
                }
            }
        }
    }

    bool isDepthSilhouette = hasDepth && (bestDepthRaw > centerDepthRaw * 1.05f + 1e-4f);
    bool dilateMv = isDepthSilhouette && (length(centerKaris - bestKaris) < 0.12f);
    int2 mvCoord = dilateMv ? clamp(px + bestOffset, int2(0, 0), maxPx) : px;

    float2 dilatedMvPx = (isCockpitStatic || !hasMotion)
        ? float2(0.0f, 0.0f)
        : g_MotionVectors.Load(int3(mvCoord, 0));
    float2 dilatedMvUV = dilatedMvPx * renderRes.zw;

    // 2. Plane-Predicted Depth Disocclusion (PointClamp PrevDepth + Sky bypass + gated OutReactive)
    float2 currUV = (float2(px) + 0.5f) * renderRes.zw;
    float2 prevUV = currUV + dilatedMvUV;
    float2 prevDepthUV = prevUV + (jitterParams.zw - jitterParams.xy) * renderRes.zw;
    float reactive = hasReactive ? g_ReactiveMask.Load(int3(px, 0)) : 0.0f;

    float disocclusion = 0.0f;
    if (any(prevUV < 0.0f) || any(prevUV > 1.0f) ||
        (hasClass && (centerClass == kClassOffscreenFG || centerClass == kClassReset))) {
        disocclusion = 1.0f;
    } else if (isCockpitStatic) {
        disocclusion = 0.0f;
    } else if (hasDepth && hasPrevDepth) {
        int2 maxPrevPx = maxPx;
        float2 prevRasterPx = clamp(prevDepthUV, 0.0f, 1.0f) * renderRes.xy;
        int2 basePrev = int2(floor(prevRasterPx - 0.5f));
        float pd00 = g_PrevDepth.Load(int3(clamp(basePrev + int2(0, 0), int2(0, 0), maxPrevPx), 0));
        float pd10 = g_PrevDepth.Load(int3(clamp(basePrev + int2(1, 0), int2(0, 0), maxPrevPx), 0));
        float pd01 = g_PrevDepth.Load(int3(clamp(basePrev + int2(0, 1), int2(0, 0), maxPrevPx), 0));
        float pd11 = g_PrevDepth.Load(int3(clamp(basePrev + int2(1, 1), int2(0, 0), maxPrevPx), 0));

        bool anyPrevSky = (pd00 <= 1e-6f) || (pd10 <= 1e-6f) || (pd01 <= 1e-6f) || (pd11 <= 1e-6f);
        bool anyPrevGeo = (pd00 > 1e-6f) || (pd10 > 1e-6f) || (pd01 > 1e-6f) || (pd11 > 1e-6f);

        if (centerIsSky) {
            disocclusion = anyPrevSky ? 0.0f : 1.0f;
        } else if (!anyPrevGeo) {
            float dL = g_CurrDepth.Load(int3(clamp(px + int2(-1, 0), int2(0, 0), maxPx), 0));
            float dR = g_CurrDepth.Load(int3(clamp(px + int2( 1, 0), int2(0, 0), maxPx), 0));
            float dU = g_CurrDepth.Load(int3(clamp(px + int2( 0,-1), int2(0, 0), maxPx), 0));
            float dD = g_CurrDepth.Load(int3(clamp(px + int2( 0, 1), int2(0, 0), maxPx), 0));
            bool thinCurrDepth = ((centerDepthRaw > dL * 1.05f && centerDepthRaw > dR * 1.05f) ||
                                  (centerDepthRaw > dU * 1.05f && centerDepthRaw > dD * 1.05f));
            disocclusion = (length(dilatedMvPx) < 0.35f && thinCurrDepth) ? 0.0f : 1.0f;
        } else {
            float depthB = (depthParams.x > 0.0f) ? depthParams.x : 0.025f;
            float centerZ = depthB / centerDepthRaw;

            float dL = g_CurrDepth.Load(int3(clamp(px + int2(-1, 0), int2(0, 0), maxPx), 0));
            float dR = g_CurrDepth.Load(int3(clamp(px + int2( 1, 0), int2(0, 0), maxPx), 0));
            float dU = g_CurrDepth.Load(int3(clamp(px + int2( 0,-1), int2(0, 0), maxPx), 0));
            float dD = g_CurrDepth.Load(int3(clamp(px + int2( 0, 1), int2(0, 0), maxPx), 0));

            float zL = (dL > 1e-6f) ? (depthB / dL) : 1e9f;
            float zR = (dR > 1e-6f) ? (depthB / dR) : 1e9f;
            float zU = (dU > 1e-6f) ? (depthB / dU) : 1e9f;
            float zD = (dD > 1e-6f) ? (depthB / dD) : 1e9f;

            // min-gradient: ignores silhouette cliffs, preserves actual grazing-angle plane slope
            float dzdx = min(abs(zR - centerZ), abs(centerZ - zL));
            float dzdy = min(abs(zD - centerZ), abs(centerZ - zU));
            float slopeTol = dzdx + dzdy;

            float zThreshold = max(centerZ * 0.015f, slopeTol * 1.75f + 0.02f);

            // Discrete 2x2 Point-Load footprint around prevDepthUV (no bilinear fake depths)
            float minZDiff = 1e9f;
            if (pd00 > 1e-6f) minZDiff = min(minZDiff, abs(centerZ - depthB / pd00));
            if (pd10 > 1e-6f) minZDiff = min(minZDiff, abs(centerZ - depthB / pd10));
            if (pd01 > 1e-6f) minZDiff = min(minZDiff, abs(centerZ - depthB / pd01));
            if (pd11 > 1e-6f) minZDiff = min(minZDiff, abs(centerZ - depthB / pd11));

            float depthDisocc = saturate((minZDiff - zThreshold) / max(zThreshold * 1.5f, 1e-4f));

            // Suppress false grazing-angle OutReactive resets whenever the tangent plane explains minZDiff.
            float planeExplain = 1.0f - saturate((minZDiff - zThreshold) / max(zThreshold * 0.5f, 1e-4f));
            float effReactive = reactive * (1.0f - planeExplain);
            disocclusion = max(depthDisocc, effReactive);

            // Prevent false disocclusion on stationary <1px depth-jittered thin features (antennas/wires)
            if (disocclusion > 0.05f && length(dilatedMvPx) < 0.35f) {
                int2 pCenter = clamp(int2(floor(prevRasterPx)), int2(0, 0), maxPrevPx);
                float pdC = g_PrevDepth.Load(int3(pCenter, 0));
                float pdL = g_PrevDepth.Load(int3(clamp(pCenter + int2(-1, 0), int2(0, 0), maxPrevPx), 0));
                float pdR = g_PrevDepth.Load(int3(clamp(pCenter + int2( 1, 0), int2(0, 0), maxPrevPx), 0));
                float pdU = g_PrevDepth.Load(int3(clamp(pCenter + int2( 0,-1), int2(0, 0), maxPrevPx), 0));
                float pdD = g_PrevDepth.Load(int3(clamp(pCenter + int2( 0, 1), int2(0, 0), maxPrevPx), 0));
                bool prevThinSpike = ((pdC > pdL * 1.05f && pdC > pdR * 1.05f) ||
                                      (pdC > pdU * 1.05f && pdC > pdD * 1.05f));
                bool currThinSpike = ((centerDepthRaw > dL * 1.05f && centerDepthRaw > dR * 1.05f) ||
                                      (centerDepthRaw > dU * 1.05f && centerDepthRaw > dD * 1.05f));
                if (prevThinSpike || currThinSpike) {
                    disocclusion = 0.0f;
                }
            }
        }
    } else {
        disocclusion = reactive;
    }

    // 3. Thin-Feature Detection (1px ridges/lines in Karis-luma)
    float thinFeature = 0.0f;
    if (!centerIsSky) {
        float lC = KarisLuma(centerRgb);
        float lL = KarisLuma(g_CurrColor.Load(int3(clamp(px + int2(-1, 0), int2(0, 0), maxPx), 0)).rgb);
        float lR = KarisLuma(g_CurrColor.Load(int3(clamp(px + int2( 1, 0), int2(0, 0), maxPx), 0)).rgb);
        float lU = KarisLuma(g_CurrColor.Load(int3(clamp(px + int2( 0,-1), int2(0, 0), maxPx), 0)).rgb);
        float lD = KarisLuma(g_CurrColor.Load(int3(clamp(px + int2( 0, 1), int2(0, 0), maxPx), 0)).rgb);

        float hRidge = min(abs(lC - lL), abs(lC - lR));
        float vRidge = min(abs(lC - lU), abs(lC - lD));
        float hSameSign = ((lC - lL) * (lC - lR) > 0.0f) ? 1.0f : 0.0f;
        float vSameSign = ((lC - lU) * (lC - lD) > 0.0f) ? 1.0f : 0.0f;

        float ridgeStrength = max(hRidge * hSameSign, vRidge * vSameSign);
        thinFeature = saturate((ridgeStrength - 0.035f) * 8.0f) * (1.0f - disocclusion);
    }

    g_OutPrepass[px] = float4(dilatedMvUV, disocclusion, thinFeature);
}
)";

// ============================================================================
// EDPE TAAv2 - Pass 2: Temporal Resolve
// Target: cs_5_0 (Direct3D 11, compiled with -O1 for Wine/Proton compatibility)
// ============================================================================
inline constexpr char kTaaResolveShader[] = R"(
static const uint kClassCockpitStatic = 2u;
static const uint kClassOffscreenFG   = 6u;
static const uint kClassReset         = 7u;

cbuffer TaaResolveCB : register(b0) {
    float4 outputRes;      // x: outW, y: outH, z: 1/outW, w: 1/outH
    float4 renderRes;      // x: renW, y: renH, z: 1/renW, w: 1/renH
    float4 jitterAndAlpha; // x: jitterPx.x, y: jitterPx.y, z: minAlpha, w: maxAlpha
    float4 gammaAndReset;  // x: baseGamma, y: lockMaxGamma, z: resetFlag, w: flags (bit0=hasClass, bit1=hasReactive)
};

Texture2D<float4> g_CurrColor    : register(t0); // Render-res color (jittered)
Texture2D<float4> g_HistoryColor : register(t1); // Output-res R16G16B16A16_FLOAT (prev frame)
Texture2D<float4> g_HistoryAux   : register(t2); // Output-res R16G16B16A16_FLOAT (sampleCount, lockLife, instability, prevVelMag)
Texture2D<float4> g_Prepass      : register(t3); // Render-res RG=dilatedMvUV, B=disocc, A=thinFeature
Texture2D<uint>   g_MotionClass  : register(t4); // Render-res R8_UINT
Texture2D<float>  g_ReactiveMask : register(t5); // Render-res R8_UNORM

RWTexture2D<float4> g_OutColor   : register(u0); // Output-res R16G16B16A16_FLOAT
RWTexture2D<float4> g_OutAux     : register(u1); // Output-res R16G16B16A16_FLOAT

SamplerState g_LinearClamp : register(s0);
SamplerState g_PointClamp  : register(s1);

float3 KarisForward(float3 rgb) {
    rgb = max(rgb, 0.0f);
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    return rgb / (1.0f + peak);
}

float3 KarisInverse(float3 tm) {
    tm = max(tm, 0.0f);
    float peak = max(tm.r, max(tm.g, tm.b));
    float3 rgb = tm / max(1.0f - peak, 1.0f / 65504.0f);
    return min(rgb, float3(65024.0f, 65024.0f, 64512.0f));
}

float3 RGBToYCoCg(float3 rgb) {
    float Y  =  0.25f * rgb.r + 0.5f * rgb.g + 0.25f * rgb.b;
    float Co =  0.50f * rgb.r                - 0.50f * rgb.b;
    float Cg = -0.25f * rgb.r + 0.5f * rgb.g - 0.25f * rgb.b;
    return float3(Y, Co, Cg);
}

float3 YCoCgToRGB(float3 ycocg) {
    float tmp = ycocg.x - ycocg.z;
    float r   = tmp + ycocg.y;
    float g   = ycocg.x + ycocg.z;
    float b   = tmp - ycocg.y;
    return max(float3(r, g, b), 0.0f);
}

float3 ClipAABB(float3 aabbMin, float3 aabbMax, float3 history) {
    float3 pClip = 0.5f * (aabbMax + aabbMin);
    float3 eClip = 0.5f * (aabbMax - aabbMin) + 1e-5f;
    float3 vClip = history - pClip;
    float3 vUnit = vClip / eClip;
    float3 aUnit = abs(vUnit);
    float  maUnit = max(aUnit.x, max(aUnit.y, aUnit.z));
    if (maUnit > 1.0f) {
        return pClip + vClip / maUnit;
    }
    return history;
}

float3 SampleHistoryCatmullRom(float2 uv) {
    float2 samplePos = uv * outputRes.xy;
    float2 tc1 = floor(samplePos - 0.5f) + 0.5f;
    float2 f   = samplePos - tc1;
    float2 f2  = f * f;
    float2 f3  = f2 * f;

    float2 w0  = f2 - 0.5f * (f3 + f);
    float2 w1  = 1.5f * f3 - 2.5f * f2 + 1.0f;
    float2 w3  = 0.5f * (f3 - f2);
    float2 w2  = 1.0f - w0 - w1 - w3;

    float2 w12  = w1 + w2;
    float2 tc0  = (tc1 - 1.0f) * outputRes.zw;
    float2 tc3  = (tc1 + 2.0f) * outputRes.zw;
    float2 tc12 = (tc1 + w2 / max(w12, 1e-5f)) * outputRes.zw;

    float3 s0 = KarisForward(g_HistoryColor.SampleLevel(g_LinearClamp, float2(tc12.x, tc0.y),  0.0f).rgb);
    float3 s1 = KarisForward(g_HistoryColor.SampleLevel(g_LinearClamp, float2(tc0.x,  tc12.y), 0.0f).rgb);
    float3 s2 = KarisForward(g_HistoryColor.SampleLevel(g_LinearClamp, float2(tc12.x, tc12.y), 0.0f).rgb);
    float3 s3 = KarisForward(g_HistoryColor.SampleLevel(g_LinearClamp, float2(tc3.x,  tc12.y), 0.0f).rgb);
    float3 s4 = KarisForward(g_HistoryColor.SampleLevel(g_LinearClamp, float2(tc12.x, tc3.y),  0.0f).rgb);

    float wt0 = w12.x * w0.y;
    float wt1 = w0.x  * w12.y;
    float wt2 = w12.x * w12.y;
    float wt3 = w3.x  * w12.y;
    float wt4 = w12.x * w3.y;

    float weightSum = wt0 + wt1 + wt2 + wt3 + wt4;
    float3 result = (s0 * wt0 + s1 * wt1 + s2 * wt2 + s3 * wt3 + s4 * wt4) / max(weightSum, 1e-5f);

    float3 minS = min(s2, min(min(s0, s1), min(s3, s4)));
    float3 maxS = max(s2, max(max(s0, s1), max(s3, s4)));
    return clamp(result, minS, maxS);
}

float FilterWeight(float2 offsetPx, float sigma) {
    float d2 = dot(offsetPx, offsetPx);
    return exp(-d2 / (2.0f * sigma * sigma));
}

[numthreads(8, 8, 1)]
void cs_taa_resolve(uint3 dtid : SV_DispatchThreadID) {
    int2 px = int2(dtid.xy);
    int2 maxOutPx = int2(outputRes.xy) - 1;
    if (px.x > maxOutPx.x || px.y > maxOutPx.y) {
        return;
    }

    int2 maxRenderPx = int2(renderRes.xy) - 1;
    float2 currUV = (float2(px) + 0.5f) * outputRes.zw;

    // Map output pixel into render space (works for both 1.0x native and renderScale < 1.0).
    // In EDPE, projection shifted geometry by +jitterPx in render pixels.
    float2 renderPos = currUV * renderRes.xy + jitterAndAlpha.xy;
    float2 baseRenderTexel = floor(renderPos) + 0.5f;
    float2 fracOffset = renderPos - baseRenderTexel;
    int2 baseRenderPx = clamp(int2(floor(renderPos)), int2(0, 0), maxRenderPx);
    int2 renderPx = clamp(int2(floor(currUV * renderRes.xy)), int2(0, 0), maxRenderPx);

    float4 pre = g_Prepass.SampleLevel(g_PointClamp, currUV, 0.0f);
    float2 mvUV = pre.xy;
    float  disocc = pre.z;
    float  thin = pre.w;

    uint resFlags = uint(gammaAndReset.w + 0.5f);
    bool hasClass    = (resFlags & 1u) != 0u;
    bool hasReactive = (resFlags & 2u) != 0u;

    uint  mClass   = hasClass ? g_MotionClass.Load(int3(renderPx, 0)) : 1u;
    float rawReact = hasReactive ? g_ReactiveMask.Load(int3(renderPx, 0)) : 0.0f;
    float reactive = (hasClass && (mClass == kClassOffscreenFG || mClass == kClassReset))
        ? rawReact
        : rawReact * saturate(disocc * 4.0f);

    float2 mvPxOut = mvUV * outputRes.xy;
    float  velMagPx = length(mvPxOut);

    // 1. 3x3 current-frame neighborhood in Peak-Karis YCoCg + adaptive jitter-aware filter
    float3 m1 = 0.0f;
    float3 m2 = 0.0f;
    float3 min3x3   =  1e9f;
    float3 max3x3   = -1e9f;
    float3 minCross =  1e9f;
    float3 maxCross = -1e9f;

    float3 filteredCurr = 0.0f;
    float  filterWsum   = 0.0f;
    float3 centerYCoCg  = 0.0f;
    float  centerAlpha  = 1.0f;

    float filterSigma = lerp(0.47f, 0.36f, saturate(velMagPx * 0.25f + disocc));

    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            int2 sp = clamp(baseRenderPx + int2(dx, dy), int2(0, 0), maxRenderPx);
            float4 rawSample = g_CurrColor.Load(int3(sp, 0));
            if (dx == 0 && dy == 0) {
                centerAlpha = rawSample.a;
            }
            float3 ycc = RGBToYCoCg(KarisForward(rawSample.rgb));

            m1 += ycc;
            m2 += ycc * ycc;
            min3x3 = min(min3x3, ycc);
            max3x3 = max(max3x3, ycc);

            if (abs(dx) + abs(dy) <= 1) {
                minCross = min(minCross, ycc);
                maxCross = max(maxCross, ycc);
            }
            if (dx == 0 && dy == 0) {
                centerYCoCg = ycc;
            }

            float w = FilterWeight(float2(dx, dy) - fracOffset, filterSigma);
            filteredCurr += ycc * w;
            filterWsum   += w;
        }
    }

    filteredCurr = clamp(filteredCurr / max(filterWsum, 1e-5f), min3x3, max3x3);

    // Global reset (scene_cut / teleport / resize / phase change / GuiFocus 6..9)
    if (gammaAndReset.z > 0.5f) {
        float3 outRgb = KarisInverse(YCoCgToRGB(filteredCurr));
        g_OutColor[px] = float4(outRgb, centerAlpha);
        g_OutAux[px]   = float4(1.0f, 0.0f, 0.0f, velMagPx);
        return;
    }

    // 2. History fetch: exact center bypass in static/cockpit, 5-tap Catmull-Rom in motion
    float2 prevUV = currUV + mvUV;
    bool offscreen = any(prevUV < 0.0f) || any(prevUV > 1.0f);

    float3 histTm;
    float4 prevAux;
    if (offscreen) {
        histTm  = YCoCgToRGB(filteredCurr);
        prevAux = float4(0.0f, 0.0f, 0.0f, velMagPx);
        disocc  = 1.0f;
    } else if ((hasClass && mClass == kClassCockpitStatic) || velMagPx < 0.05f) {
        histTm  = KarisForward(g_HistoryColor.Load(int3(px, 0)).rgb);
        prevAux = g_HistoryAux.Load(int3(px, 0));
    } else {
        histTm  = SampleHistoryCatmullRom(prevUV);
        prevAux = g_HistoryAux.SampleLevel(g_LinearClamp, prevUV, 0.0f);
    }

    float3 histYCoCg = RGBToYCoCg(histTm);

    float prevSamples     = prevAux.x;
    float prevLockLife    = prevAux.y;
    float prevInstability = prevAux.z;
    float prevVelMag      = prevAux.w;

    // 3. Dual-velocity shock & Lock / Luma Instability update
    float velDelta = abs(velMagPx - prevVelMag);
    float velShock = saturate((velDelta - 1.5f) * 0.25f);

    float killLock = saturate(disocc + reactive * 1.5f + velShock);
    float lockLife = max(prevLockLife * 0.88f, thin) * (1.0f - killLock);

    float lumaDiff = abs(histYCoCg.x - centerYCoCg.x);
    float rawInstab = saturate((lumaDiff - 0.04f) * 6.0f) * (1.0f - disocc) * (1.0f - reactive);
    float instability = lerp(prevInstability * 0.85f, rawInstab, 0.35f);

    // 4. Adaptive Variance Clipping + Soft Rounded Box + Playdead ClipAABB
    float3 mean  = m1 / 9.0f;
    float3 sigma = sqrt(max(m2 / 9.0f - mean * mean, 0.0f));

    float gamma = gammaAndReset.x; // baseGamma
    gamma = lerp(gamma, 0.80f, saturate(velMagPx * 0.12f + reactive + velShock));
    gamma = lerp(gamma, gammaAndReset.y, lockLife * (1.0f - saturate(velMagPx * 0.20f)));

    float3 softMin = 0.5f * (min3x3 + minCross);
    float3 softMax = 0.5f * (max3x3 + maxCross);
    float3 varMin  = mean - gamma * sigma;
    float3 varMax  = mean + gamma * sigma;

    float3 clipMin = lerp(max(varMin, softMin), varMin, lockLife);
    float3 clipMax = lerp(min(varMax, softMax), varMax, lockLife);

    float3 clippedHist = ClipAABB(clipMin, clipMax, histYCoCg);

    // Sub-pixel (<1px) stationary thin-ridge retention across missed Halton jitter phases.
    // Symmetric +-1px / +-2px ridge operator is mathematically zero inside blobs/steps,
    // so un-vectored moving blobs still vanish cleanly without ghosting.
    float stationaryWeight = saturate(1.0f - velMagPx * 4.0f) * (1.0f - killLock);
    float histThinRidge = 0.0f;
    if (stationaryWeight > 0.0f && prevLockLife > 0.02f) {
        float2 ht = outputRes.zw;
        float hL  = RGBToYCoCg(KarisForward(g_HistoryColor.SampleLevel(g_LinearClamp, clamp(prevUV + float2(-ht.x, 0.0f), 0.0f, 1.0f), 0.0f).rgb)).x;
        float hR  = RGBToYCoCg(KarisForward(g_HistoryColor.SampleLevel(g_LinearClamp, clamp(prevUV + float2( ht.x, 0.0f), 0.0f, 1.0f), 0.0f).rgb)).x;
        float hU  = RGBToYCoCg(KarisForward(g_HistoryColor.SampleLevel(g_LinearClamp, clamp(prevUV + float2(0.0f, -ht.y), 0.0f, 1.0f), 0.0f).rgb)).x;
        float hD  = RGBToYCoCg(KarisForward(g_HistoryColor.SampleLevel(g_LinearClamp, clamp(prevUV + float2(0.0f,  ht.y), 0.0f, 1.0f), 0.0f).rgb)).x;
        float hL2 = RGBToYCoCg(KarisForward(g_HistoryColor.SampleLevel(g_LinearClamp, clamp(prevUV + float2(-2.0f * ht.x, 0.0f), 0.0f, 1.0f), 0.0f).rgb)).x;
        float hR2 = RGBToYCoCg(KarisForward(g_HistoryColor.SampleLevel(g_LinearClamp, clamp(prevUV + float2( 2.0f * ht.x, 0.0f), 0.0f, 1.0f), 0.0f).rgb)).x;
        float hU2 = RGBToYCoCg(KarisForward(g_HistoryColor.SampleLevel(g_LinearClamp, clamp(prevUV + float2(0.0f, -2.0f * ht.y), 0.0f, 1.0f), 0.0f).rgb)).x;
        float hD2 = RGBToYCoCg(KarisForward(g_HistoryColor.SampleLevel(g_LinearClamp, clamp(prevUV + float2(0.0f,  2.0f * ht.y), 0.0f, 1.0f), 0.0f).rgb)).x;

        float ridgeX = max(0.0f, abs(histYCoCg.x - 0.5f * (hL + hR)) - 0.5f * abs(hL - hR));
        float ridgeY = max(0.0f, abs(histYCoCg.x - 0.5f * (hU + hD)) - 0.5f * abs(hU - hD));
        float ridge2X = max(
            min(max(0.0f, abs(histYCoCg.x - 0.5f * (hL2 + hR)) - 0.5f * abs(hL2 - hR)),
                max(0.0f, abs(hL - 0.5f * (hL2 + hR)) - 0.5f * abs(hL2 - hR))),
            min(max(0.0f, abs(histYCoCg.x - 0.5f * (hL + hR2)) - 0.5f * abs(hL - hR2)),
                max(0.0f, abs(hR - 0.5f * (hL + hR2)) - 0.5f * abs(hL - hR2))));
        float ridge2Y = max(
            min(max(0.0f, abs(histYCoCg.x - 0.5f * (hU2 + hD)) - 0.5f * abs(hU2 - hD)),
                max(0.0f, abs(hU - 0.5f * (hU2 + hD)) - 0.5f * abs(hU2 - hD))),
            min(max(0.0f, abs(histYCoCg.x - 0.5f * (hU + hD2)) - 0.5f * abs(hU - hD2)),
                max(0.0f, abs(hD - 0.5f * (hU + hD2)) - 0.5f * abs(hU - hD2))));
        float ridgeStr = max(max(ridgeX, ridgeY), 0.85f * max(ridge2X, ridge2Y));
        histThinRidge = saturate((ridgeStr - 0.005f) * 32.0f);
        clippedHist = lerp(clippedHist, histYCoCg, 0.97f * stationaryWeight * histThinRidge * saturate(prevLockLife * 2.0f));
    }

    // 5. Progressive convergence (sampleCount) + anti-flicker damping
    float validSamples = prevSamples * (1.0f - saturate(disocc + reactive * 0.85f));
    float newSamples   = min(validSamples + 1.0f, 64.0f);
    float convAlpha    = 1.0f / max(newSamples, 1.0f);

    float minAlphaEff = lerp(jitterAndAlpha.z, jitterAndAlpha.w,
        saturate(reactive * 0.9f + velShock * 0.5f + velMagPx * 0.04f));
    float flickerDamp = lerp(1.0f, 0.35f, max(instability, max(lockLife, histThinRidge)) * (1.0f - killLock));
    float alpha = max(convAlpha * lerp(1.0f, flickerDamp, saturate((newSamples - 2.0f) * 0.25f)),
                      minAlphaEff * flickerDamp);

    float3 currForBlend = (newSamples < 2.0f) ? lerp(centerYCoCg, filteredCurr, 0.85f) : filteredCurr;
    if (disocc > 0.95f) {
        alpha = 1.0f;
    }

    float3 resolvedYCoCg = lerp(clippedHist, currForBlend, saturate(alpha));
    float3 outRgb = KarisInverse(YCoCgToRGB(resolvedYCoCg));

    if (any(isnan(outRgb) || isinf(outRgb))) {
        outRgb = max(g_CurrColor.Load(int3(renderPx, 0)).rgb, 0.0f);
    }

    g_OutColor[px] = float4(outRgb, centerAlpha);
    g_OutAux[px]   = float4(newSamples, lockLife, instability, velMagPx);
}
)";

// ============================================================================
// EDPE TAAv2 - Pass 3: Instability-Aware Post-RCAS (Outside History Loop)
// Performs CAS weights in Peak-Karis [0, 1] space so HDR stars & LDR behave safely.
// ============================================================================
inline constexpr char kTaaPostRcasShader[] = R"(
cbuffer RcasCB : register(b0) {
    float2 g_Resolution;
    float  g_Sharpness;
    float  _pad0;
};

Texture2D<float4>   g_TaaColor : register(t0);
Texture2D<float4>   g_TaaAux   : register(t1); // B = lumaInstability
RWTexture2D<float4> g_OutFinal : register(u0);

float3 KarisForward(float3 rgb) {
    rgb = max(rgb, 0.0f);
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    return rgb / (1.0f + peak);
}

float3 KarisInverse(float3 tm) {
    tm = max(tm, 0.0f);
    float peak = max(tm.r, max(tm.g, tm.b));
    float3 rgb = tm / max(1.0f - peak, 1.0f / 65504.0f);
    return min(rgb, float3(65024.0f, 65024.0f, 64512.0f));
}

[numthreads(8, 8, 1)]
void cs_taa_post_rcas(uint3 dtid : SV_DispatchThreadID) {
    int2 px = int2(dtid.xy);
    int2 maxPx = int2(g_Resolution) - 1;
    if (px.x > maxPx.x || px.y > maxPx.y) {
        return;
    }

    float4 center = g_TaaColor.Load(int3(px, 0));
    if (g_Sharpness <= 1e-4f) {
        g_OutFinal[px] = center;
        return;
    }

    float3 cTm = KarisForward(center.rgb);
    float3 nTm = KarisForward(g_TaaColor.Load(int3(clamp(px + int2( 0,-1), int2(0, 0), maxPx), 0)).rgb);
    float3 sTm = KarisForward(g_TaaColor.Load(int3(clamp(px + int2( 0, 1), int2(0, 0), maxPx), 0)).rgb);
    float3 wTm = KarisForward(g_TaaColor.Load(int3(clamp(px + int2(-1, 0), int2(0, 0), maxPx), 0)).rgb);
    float3 eTm = KarisForward(g_TaaColor.Load(int3(clamp(px + int2( 1, 0), int2(0, 0), maxPx), 0)).rgb);

    float instab = g_TaaAux.Load(int3(px, 0)).z;
    float effSharp = saturate(g_Sharpness) * (1.0f - saturate(instab * 1.25f));

    float3 minRGB = min(cTm, min(min(nTm, sTm), min(wTm, eTm)));
    float3 maxRGB = max(cTm, max(max(nTm, sTm), max(wTm, eTm)));
    float3 amp    = saturate(min(minRGB, 2.0f - maxRGB) / max(maxRGB, 1e-4f));
    float  peak   = -1.0f / lerp(8.0f, 5.0f, effSharp);
    float3 weight = sqrt(amp) * peak;

    float3 sharpenedTm = saturate((cTm + (nTm + sTm + wTm + eTm) * weight) / max(1.0f + 4.0f * weight, 1e-4f));
    float3 outRgb = KarisInverse(sharpenedTm);
    if (any(isnan(outRgb) || isinf(outRgb))) {
        outRgb = max(center.rgb, 0.0f);
    }
    g_OutFinal[px] = float4(outRgb, center.a);
}
)";

// ============================================================================
// EDPE TAAv2 - Display Blit & Read-Only Debug Views (1..4)
// Supports arbitrary destination RTV formats and DEV debug visualizations
// without ever modifying historyTexture_ or historyAuxTex_.
// ============================================================================
inline constexpr char kTaaBlitShader[] = R"(
cbuffer BlitCB : register(b0) {
    float4 outputRes; // x: outW, y: outH, z: 1/outW, w: 1/outH
    uint4  debugInfo; // x: debugView (0=Off, 1=Disocclusion, 2=ThinFeatureLock, 3=LumaInstability, 4=SampleCount)
};

Texture2D<float4>   g_DisplayColor : register(t0);
Texture2D<float4>   g_Aux          : register(t1);
Texture2D<float4>   g_Prepass      : register(t2);
RWTexture2D<float4> g_OutDebug     : register(u0);

SamplerState g_PointClamp : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut vs_taa_blit(uint id : SV_VertexID) {
    VSOut o;
    float2 corner = float2((id << 1) & 2, id & 2);
    o.pos = float4(corner * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    o.uv = corner;
    return o;
}

float3 computeDebugColor(int2 px, float2 uv) {
    float3 baseCol = max(g_DisplayColor.Load(int3(px, 0)).rgb, 0.0f);
    float baseLuma = dot(baseCol, float3(0.2126f, 0.7152f, 0.0722f));
    float bg = (baseLuma / (1.0f + baseLuma)) * 0.25f;

    float4 aux = g_Aux.Load(int3(px, 0));
    float4 pre = g_Prepass.SampleLevel(g_PointClamp, uv, 0.0f);

    if (debugInfo.x == 1u) {
        // 1 = Disocclusion (red/orange)
        float v = saturate(pre.z);
        return lerp(float3(bg, bg, bg), float3(1.0f, 0.15f, 0.05f), v);
    } else if (debugInfo.x == 2u) {
        // 2 = ThinFeatureLock (cyan/green)
        float v = saturate(aux.y);
        return lerp(float3(bg, bg, bg), float3(0.10f, 0.95f, 0.75f), v);
    } else if (debugInfo.x == 3u) {
        // 3 = LumaInstability (magenta/amber)
        float v = saturate(aux.z);
        return lerp(float3(bg, bg, bg), float3(1.0f, 0.25f, 0.85f), v);
    } else if (debugInfo.x == 4u) {
        // 4 = SampleCount (red 0 -> yellow 0.5 -> green 1.0 over 1..16)
        float t = saturate((aux.x - 1.0f) / 15.0f);
        float3 lowMid = lerp(float3(0.95f, 0.10f, 0.10f), float3(0.95f, 0.85f, 0.10f), saturate(t * 2.0f));
        float3 midHi  = lerp(float3(0.95f, 0.85f, 0.10f), float3(0.10f, 0.90f, 0.25f), saturate((t - 0.5f) * 2.0f));
        float3 heat   = (t < 0.5f) ? lowMid : midHi;
        return lerp(float3(bg, bg, bg), heat, 0.85f);
    }
    return baseCol;
}

float4 ps_taa_blit(VSOut pin) : SV_Target {
    int2 maxPx = int2(outputRes.xy) - 1;
    int2 px = clamp(int2(pin.pos.xy), int2(0, 0), maxPx);
    if (debugInfo.x == 0u) {
        return g_DisplayColor.Load(int3(px, 0));
    }
    return float4(computeDebugColor(px, pin.uv), 1.0f);
}

[numthreads(8, 8, 1)]
void cs_taa_debug(uint3 dtid : SV_DispatchThreadID) {
    int2 px = int2(dtid.xy);
    int2 maxPx = int2(outputRes.xy) - 1;
    if (px.x > maxPx.x || px.y > maxPx.y) {
        return;
    }
    float2 uv = (float2(px) + 0.5f) * outputRes.zw;
    g_OutDebug[px] = float4(computeDebugColor(px, uv), 1.0f);
}
)";

} // namespace edpe
