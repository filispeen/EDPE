#pragma once

namespace edpe {

// ============================================================================
// EDPE TAAv2 - Pass 1: Prepass (Closest-Depth 3x3 Velocity Dilation,
// Camera-Relative Plane-Predicted Depth Disocclusion, Thin-Feature Candidate)
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
    float4 depthParams;  // x: depthB, y: invScaleX, z: invScaleY, w: effectiveHdr (0/1)
    float4 camRelRow2;   // xyz: R_prev^T_row2 * R_now, w: dot(R_prev_col2, t_now - t_prev)
    uint4  flags;        // x: bit0=hasPrevDepth, bit1=hasClass, bit2=hasReactive, bit3=hasMotion, bit4=hasDepth, bit5=hasRelCamZ
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

float TonemapLuma(float3 c, bool effectiveHdr) {
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
    bool hasRelCamZ   = (modeFlags & 32u) != 0u;
    bool effectiveHdr = (depthParams.w > 0.5f);

    uint centerClass = hasClass ? g_MotionClass.Load(int3(px, 0)) : kClassWorld;
    float centerDepthRaw = hasDepth ? g_CurrDepth.Load(int3(px, 0)) : 0.0f;
    float3 centerRgb = g_CurrColor.Load(int3(px, 0)).rgb;

    bool centerIsSky = (hasDepth && centerDepthRaw <= 1e-6f) || (hasClass && centerClass == kClassSky);
    bool isCockpitStatic = (hasClass && centerClass == kClassCockpitStatic);

    // 1. Relative-depth closest-depth 3x3 velocity dilation (Reversed-Z, cockpit protected).
    // Works at all distances (> 250 m) and across high-contrast silhouettes without color gating.
    int2 bestOffset = int2(0, 0);
    float bestDepthRaw = centerDepthRaw;
    float dL = centerDepthRaw;
    float dR = centerDepthRaw;
    float dU = centerDepthRaw;
    float dD = centerDepthRaw;

    if (hasDepth && !isCockpitStatic) {
        [unroll]
        for (int dy = -1; dy <= 1; ++dy) {
            [unroll]
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                int2 p = clamp(px + int2(dx, dy), int2(0, 0), maxPx);
                float d = g_CurrDepth.Load(int3(p, 0));
                if (dx == -1 && dy == 0) dL = d;
                else if (dx == 1 && dy == 0) dR = d;
                else if (dx == 0 && dy == -1) dU = d;
                else if (dx == 0 && dy == 1) dD = d;
                if (d > bestDepthRaw) {
                    if (!hasClass || g_MotionClass.Load(int3(p, 0)) != kClassCockpitStatic) {
                        bestDepthRaw = d;
                        bestOffset = int2(dx, dy);
                    }
                }
            }
        }
    }

    float2 centerMvPx = (isCockpitStatic || !hasMotion)
        ? float2(0.0f, 0.0f)
        : g_MotionVectors.Load(int3(px, 0));
    bool isDepthSilhouette = hasDepth && (bestDepthRaw > max(centerDepthRaw * 1.05f, 1e-6f));
    bool dilateMv = false;
    float2 dilatedMvPx = centerMvPx;
    if (isDepthSilhouette && hasMotion && !isCockpitStatic) {
        int2 bestCoord = clamp(px + bestOffset, int2(0, 0), maxPx);
        float2 bestMvPx = g_MotionVectors.Load(int3(bestCoord, 0));
        if (length(bestMvPx - centerMvPx) > 0.15f) {
            dilateMv = true;
            dilatedMvPx = bestMvPx;
        }
    }
    float2 dilatedMvUV = dilatedMvPx * renderRes.zw;

    // 2. Plane-Predicted Depth Disocclusion (with camera Z-translation compensation)
    float2 currUV = (float2(px) + 0.5f) * renderRes.zw;
    float2 prevUV = currUV + dilatedMvUV;
    float2 prevDepthUV = prevUV + (jitterParams.zw - jitterParams.xy) * renderRes.zw;
    float reactive = hasReactive ? g_ReactiveMask.Load(int3(px, 0)) : 0.0f;
    float mvMag = length(dilatedMvPx);

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
        bool allPrevSky = (pd00 <= 1e-6f) && (pd10 <= 1e-6f) && (pd01 <= 1e-6f) && (pd11 <= 1e-6f);
        bool anyPrevGeo = (pd00 > 1e-6f) || (pd10 > 1e-6f) || (pd01 > 1e-6f) || (pd11 > 1e-6f);

        if (centerIsSky) {
            disocclusion = dilateMv ? (allPrevSky ? 0.0f : 1.0f) : (anyPrevSky ? 0.0f : 1.0f);
        } else if (!anyPrevGeo) {
            bool thinCurrDepth = ((centerDepthRaw > dL * 1.05f && centerDepthRaw > dR * 1.05f) ||
                                  (centerDepthRaw > dU * 1.05f && centerDepthRaw > dD * 1.05f));
            disocclusion = (mvMag < 0.35f && thinCurrDepth) ? 0.0f : 1.0f;
        } else {
            float depthB = (depthParams.x > 0.0f) ? depthParams.x : 0.025f;
            float centerZ = depthB / centerDepthRaw;
            float expectedPrevZ = centerZ;
            if (hasRelCamZ) {
                float2 rawPx = (float2(px) + 0.5f) - jitterParams.xy;
                float2 ndc = float2(2.0f * rawPx.x * renderRes.z - 1.0f, 1.0f - 2.0f * rawPx.y * renderRes.w);
                float3 viewPos = float3(ndc.x * centerZ * depthParams.y, ndc.y * centerZ * depthParams.z, centerZ);
                expectedPrevZ = dot(camRelRow2.xyz, viewPos) + camRelRow2.w;
            }

            if (expectedPrevZ <= 1e-4f) {
                disocclusion = 1.0f;
            } else {
                float zL = (dL > 1e-6f) ? (depthB / dL) : 1e9f;
                float zR = (dR > 1e-6f) ? (depthB / dR) : 1e9f;
                float zU = (dU > 1e-6f) ? (depthB / dU) : 1e9f;
                float zD = (dD > 1e-6f) ? (depthB / dD) : 1e9f;

                // min-gradient: ignores silhouette cliffs, preserves actual grazing-angle plane slope
                float dzdx = min(abs(zR - centerZ), abs(centerZ - zL));
                float dzdy = min(abs(zD - centerZ), abs(centerZ - zU));
                float slopeTol = dzdx + dzdy;

                float zThreshold = max(expectedPrevZ * 0.015f, slopeTol * 1.75f + 0.02f);

                // Discrete 2x2 Point-Load footprint around prevDepthUV (no bilinear fake depths)
                float minZDiff = 1e9f;
                if (pd00 > 1e-6f) minZDiff = min(minZDiff, abs(expectedPrevZ - depthB / pd00));
                if (pd10 > 1e-6f) minZDiff = min(minZDiff, abs(expectedPrevZ - depthB / pd10));
                if (pd01 > 1e-6f) minZDiff = min(minZDiff, abs(expectedPrevZ - depthB / pd01));
                if (pd11 > 1e-6f) minZDiff = min(minZDiff, abs(expectedPrevZ - depthB / pd11));

                float depthDisocc = saturate((minZDiff - zThreshold) / max(zThreshold * 1.5f, 1e-4f));

                // Suppress false grazing-angle OutReactive resets only on steep slopes where plane explains minZDiff,
                // and suppress stationary sub-1.0 emissive OutReactive so static hangar lights still anti-alias.
                bool steepGrazing = (slopeTol > expectedPrevZ * 0.03f);
                float planeExplain = steepGrazing ? (1.0f - saturate((minZDiff - zThreshold) / max(zThreshold * 0.5f, 1e-4f))) : 0.0f;
                float effReactive = reactive * (1.0f - planeExplain);
                bool allowStaticReactive = (reactive >= 0.96f) ||
                                           (mvMag >= 0.25f) ||
                                           (depthDisocc > 0.05f) ||
                                           (hasClass && (centerClass == kClassDisoccluded ||
                                                         centerClass == kClassOffscreenFG ||
                                                         centerClass == kClassReset));
                if (!allowStaticReactive) {
                    effReactive = 0.0f;
                }
                disocclusion = max(depthDisocc, effReactive);

                // Prevent false disocclusion on stationary <1px depth-jittered thin features (antennas/wires)
                if (disocclusion > 0.05f && mvMag < 0.35f) {
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
        }
    } else {
        bool allowStaticReactive = (reactive >= 0.96f) ||
                                   (mvMag >= 0.25f) ||
                                   (hasClass && (centerClass == kClassDisoccluded ||
                                                 centerClass == kClassOffscreenFG ||
                                                 centerClass == kClassReset));
        disocclusion = allowStaticReactive ? reactive : 0.0f;
    }

    // 3. Thin-Feature Detection (isolated 1px ridges/wires, excluding monotonic step edges & bevels)
    float thinFeature = 0.0f;
    if (!centerIsSky) {
        float lC = TonemapLuma(centerRgb, effectiveHdr);
        float lL = TonemapLuma(g_CurrColor.Load(int3(clamp(px + int2(-1, 0), int2(0, 0), maxPx), 0)).rgb, effectiveHdr);
        float lR = TonemapLuma(g_CurrColor.Load(int3(clamp(px + int2( 1, 0), int2(0, 0), maxPx), 0)).rgb, effectiveHdr);
        float lU = TonemapLuma(g_CurrColor.Load(int3(clamp(px + int2( 0,-1), int2(0, 0), maxPx), 0)).rgb, effectiveHdr);
        float lD = TonemapLuma(g_CurrColor.Load(int3(clamp(px + int2( 0, 1), int2(0, 0), maxPx), 0)).rgb, effectiveHdr);

        float hRidge = min(abs(lC - lL), abs(lC - lR));
        float vRidge = min(abs(lC - lU), abs(lC - lD));
        float hSameSign = ((lC - lL) * (lC - lR) > 0.0f) ? 1.0f : 0.0f;
        float vSameSign = ((lC - lU) * (lC - lD) > 0.0f) ? 1.0f : 0.0f;
        float hSym = saturate(1.5f - abs(lL - lR) / max(hRidge, 1e-4f));
        float vSym = saturate(1.5f - abs(lU - lD) / max(vRidge, 1e-4f));

        float ridgeStrength = max(hRidge * hSameSign * hSym, vRidge * vSameSign * vSym);
        thinFeature = saturate((ridgeStrength - 0.070f) * 8.0f) * (1.0f - disocclusion);
    }

    g_OutPrepass[px] = float4(dilatedMvUV, disocclusion, thinFeature);
}
)";

// ============================================================================
// EDPE TAAv2 - Pass 1.5: Quarter-Res Unvectored-Motion Patch Divergence
// Detects moving objects/shadows/textures without MVs across non-overlapping 4x4
// patches while remaining transparent to 1px stationary/slow-panning thin features.
// ============================================================================
inline constexpr char kTaaPatchDivShader[] = R"(
cbuffer TaaPatchDivCB : register(b0) {
    float4 renderRes; // x: rW, y: rH, z: 1/rW, w: 1/rH
    float4 patchRes;  // x: pW, y: pH, z: currJitterX, w: currJitterY
    float4 flags;     // x: effectiveHdr (0/1), yzw: unused
};

Texture2D<float4> g_CurrColor    : register(t0);
Texture2D<float4> g_HistoryColor : register(t1);
Texture2D<float4> g_Prepass      : register(t2);

RWTexture2D<float2> g_OutPatchDiv : register(u0);

SamplerState g_LinearClamp : register(s0);

float3 TonemapForward(float3 rgb, bool effectiveHdr) {
    rgb = max(rgb, 0.0f);
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    return rgb / (1.0f + peak);
}

float3 RGBToYCoCg(float3 rgb) {
    float Y  =  0.25f * rgb.r + 0.5f * rgb.g + 0.25f * rgb.b;
    float Co =  0.50f * rgb.r                - 0.50f * rgb.b;
    float Cg = -0.25f * rgb.r + 0.5f * rgb.g - 0.25f * rgb.b;
    return float3(Y, Co, Cg);
}

[numthreads(8, 8, 1)]
void cs_taa_patch_div(uint3 dtid : SV_DispatchThreadID) {
    int2 hp = int2(dtid.xy);
    if (hp.x >= int(patchRes.x) || hp.y >= int(patchRes.y)) {
        return;
    }

    bool effectiveHdr = (flags.x > 0.5f);
    int2 maxRenderPx = int2(renderRes.xy) - 1;
    int2 basePx = hp * 4;

    float3 currSum = 0.0f;
    float3 histSum = 0.0f;
    float minCurrY =  1e9f;
    float maxCurrY = -1e9f;
    float absDiffSum = 0.0f;
    int changedCount = 0;
    int validCount = 0;

    [unroll]
    for (int dy = 0; dy < 4; ++dy) {
        [unroll]
        for (int dx = 0; dx < 4; ++dx) {
            int2 rp = clamp(basePx + int2(dx, dy), int2(0, 0), maxRenderPx);
            float4 pre = g_Prepass.Load(int3(rp, 0));
            // Exclude disoccluded silhouette pixels so silhouette motion never poisons the 4x4 block.
            if (pre.z >= 0.25f) {
                continue;
            }
            float2 unjitUV = (float2(rp) + 0.5f - patchRes.zw) * renderRes.zw;
            float2 prevUV = unjitUV + pre.xy;
            if (any(prevUV < 0.0f) || any(prevUV > 1.0f)) {
                continue;
            }
            float3 cYcc = RGBToYCoCg(TonemapForward(g_CurrColor.Load(int3(rp, 0)).rgb, effectiveHdr));
            float3 hYcc = RGBToYCoCg(TonemapForward(g_HistoryColor.SampleLevel(g_LinearClamp, prevUV, 0.0f).rgb, effectiveHdr));
            float pxDiff = abs(cYcc.x - hYcc.x) + 0.5f * length(cYcc.yz - hYcc.yz);
            absDiffSum += pxDiff;
            if (pxDiff > 0.14f) {
                ++changedCount;
            }
            currSum += cYcc;
            histSum += hYcc;
            minCurrY = min(minCurrY, cYcc.x);
            maxCurrY = max(maxCurrY, cYcc.x);
            ++validCount;
        }
    }

    if (validCount < 8) {
        g_OutPatchDiv[hp] = float2(0.0f, 0.0f);
        return;
    }

    float invN = 1.0f / float(validCount);
    float3 currMean = currSum * invN;
    float3 histMean = histSum * invN;
    float diffY = currMean.x - histMean.x;
    float diffMag = abs(diffY) + 0.6f * length(currMean.yz - histMean.yz);

    // Subpixel jitter & silhouette sampling shift around high-contrast edges can perturb
    // mean 1-spp luma; subtract an edge-contrast allowance so static railings/edges stay calm.
    float spatialRange = max(maxCurrY - minCurrY, 0.0f);
    float edgeAllowance = 0.16f * spatialRange;

    // Detect moving periodic textures (where block mean is constant across opposite-phase stripes
    // but >=38-55% of interior pixels differ strongly) without firing on isolated 1px edges.
    float coverage = float(changedCount) * invN;
    float meanAbsDiff = absDiffSum * invN;
    float textureMoveScore = smoothstep(0.38f, 0.55f, coverage) * meanAbsDiff * 1.35f;

    g_OutPatchDiv[hp] = float2(max(max(diffMag - edgeAllowance, 0.0f), textureMoveScore), diffY);
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
    float4 extraParams;    // x: effectiveHdr (0/1), y: hasPrevPrepass (0/1), z: hasPatchDiv (0/1), w: unused
};

Texture2D<float4> g_CurrColor    : register(t0); // Render-res color (jittered)
Texture2D<float4> g_HistoryColor : register(t1); // Output-res R16G16B16A16_FLOAT (prev frame)
Texture2D<float4> g_HistoryAux   : register(t2); // Output-res R16G16B16A16_FLOAT (sampleCount, lockLife, oscillation, prevSignedDelta)
Texture2D<float4> g_Prepass      : register(t3); // Render-res RG=dilatedMvUV, B=disocc, A=thinFeature
Texture2D<uint>   g_MotionClass  : register(t4); // Render-res R8_UINT
Texture2D<float>  g_ReactiveMask : register(t5); // Render-res R8_UNORM
Texture2D<float4> g_PrevPrepass  : register(t6); // Previous-frame Prepass for 2D vector velShock
Texture2D<float2> g_PatchDiv     : register(t7); // Half-res unvectored-motion patch divergence

RWTexture2D<float4> g_OutColor   : register(u0); // Output-res R16G16B16A16_FLOAT
RWTexture2D<float4> g_OutAux     : register(u1); // Output-res R16G16B16A16_FLOAT

SamplerState g_LinearClamp : register(s0);
SamplerState g_PointClamp  : register(s1);

float3 TonemapForward(float3 rgb, bool effectiveHdr) {
    rgb = max(rgb, 0.0f);
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    return rgb / (1.0f + peak);
}

float3 TonemapInverse(float3 tm, bool effectiveHdr) {
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

float3 ClipAABBToward(float3 aabbMin, float3 aabbMax, float3 target, float3 history, out float outMaUnit) {
    float3 pClip = 0.5f * (aabbMax + aabbMin);
    float3 eClip = 0.5f * (aabbMax - aabbMin) + 1e-5f;
    float3 vClip = history - pClip;
    float3 aUnit = abs(vClip / eClip);
    outMaUnit = max(aUnit.x, max(aUnit.y, aUnit.z));
    if (outMaUnit <= 1.0f) {
        return history;
    }

    float3 pTarget = clamp(target, aabbMin, aabbMax);
    float3 dir = history - pTarget;
    float t = 1.0f;
    if (dir.x > 1e-6f)       t = min(t, (aabbMax.x - pTarget.x) / dir.x);
    else if (dir.x < -1e-6f) t = min(t, (aabbMin.x - pTarget.x) / dir.x);
    if (dir.y > 1e-6f)       t = min(t, (aabbMax.y - pTarget.y) / dir.y);
    else if (dir.y < -1e-6f) t = min(t, (aabbMin.y - pTarget.y) / dir.y);
    if (dir.z > 1e-6f)       t = min(t, (aabbMax.z - pTarget.z) / dir.z);
    else if (dir.z < -1e-6f) t = min(t, (aabbMin.z - pTarget.z) / dir.z);
    return pTarget + dir * saturate(t);
}

float3 SampleHistoryCatmullRom(float2 uv, bool effectiveHdr) {
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

    float3 s0 = TonemapForward(g_HistoryColor.SampleLevel(g_LinearClamp, float2(tc12.x, tc0.y),  0.0f).rgb, effectiveHdr);
    float3 s1 = TonemapForward(g_HistoryColor.SampleLevel(g_LinearClamp, float2(tc0.x,  tc12.y), 0.0f).rgb, effectiveHdr);
    float3 s2 = TonemapForward(g_HistoryColor.SampleLevel(g_LinearClamp, float2(tc12.x, tc12.y), 0.0f).rgb, effectiveHdr);
    float3 s3 = TonemapForward(g_HistoryColor.SampleLevel(g_LinearClamp, float2(tc3.x,  tc12.y), 0.0f).rgb, effectiveHdr);
    float3 s4 = TonemapForward(g_HistoryColor.SampleLevel(g_LinearClamp, float2(tc12.x, tc3.y),  0.0f).rgb, effectiveHdr);

    float wt0 = w12.x * w0.y;
    float wt1 = w0.x  * w12.y;
    float wt2 = w12.x * w12.y;
    float wt3 = w3.x  * w12.y;
    float wt4 = w12.x * w3.y;

    float weightSum = wt0 + wt1 + wt2 + wt3 + wt4;
    float3 result = (s0 * wt0 + s1 * wt1 + s2 * wt2 + s3 * wt3 + s4 * wt4) / max(weightSum, 1e-5f);

    // Clamp to the 4 nearest point-sampled history texels (preserves Catmull-Rom negative
    // sharpening lobes on gradients without bilinear pre-blur, while preventing 1px impulse ringing).
    int2 maxOut = int2(outputRes.xy) - 1;
    int2 pBase = int2(floor(samplePos - 0.5f));
    float3 p00 = TonemapForward(g_HistoryColor.Load(int3(clamp(pBase + int2(0, 0), int2(0, 0), maxOut), 0)).rgb, effectiveHdr);
    float3 p10 = TonemapForward(g_HistoryColor.Load(int3(clamp(pBase + int2(1, 0), int2(0, 0), maxOut), 0)).rgb, effectiveHdr);
    float3 p01 = TonemapForward(g_HistoryColor.Load(int3(clamp(pBase + int2(0, 1), int2(0, 0), maxOut), 0)).rgb, effectiveHdr);
    float3 p11 = TonemapForward(g_HistoryColor.Load(int3(clamp(pBase + int2(1, 1), int2(0, 0), maxOut), 0)).rgb, effectiveHdr);
    float3 minP = min(min(p00, p10), min(p01, p11));
    float3 maxP = max(max(p00, p10), max(p01, p11));
    return clamp(result, minP, maxP);
}

float FilterWeight(float2 offsetPx, float sigma) {
    float d2 = dot(offsetPx, offsetPx);
    return exp(-d2 / (2.0f * sigma * sigma));
}
)" R"(
[numthreads(8, 8, 1)]
void cs_taa_resolve(uint3 dtid : SV_DispatchThreadID) {
    int2 px = int2(dtid.xy);
    int2 maxOutPx = int2(outputRes.xy) - 1;
    if (px.x > maxOutPx.x || px.y > maxOutPx.y) {
        return;
    }

    bool effectiveHdr   = (extraParams.x > 0.5f);
    bool hasPrevPrepass = (extraParams.y > 0.5f);
    bool hasPatchDiv    = (extraParams.z > 0.5f);
    bool isSr           = (outputRes.x > renderRes.x + 0.5f) || (outputRes.y > renderRes.y + 0.5f);

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
    float reactive = hasReactive ? g_ReactiveMask.Load(int3(renderPx, 0)) : 0.0f;

    float2 mvPxOut = mvUV * outputRes.xy;
    float  velMagPx = length(mvPxOut);

    // Unvectored-motion 2D patch divergence (0 on 1px static/panning wires, high on moving objects)
    float patchDiv = 0.0f;
    if (hasPatchDiv && !(hasClass && mClass == kClassCockpitStatic)) {
        int2 quarterDims = (int2(renderRes.xy) + 3) / 4;
        int2 quarterMax = max(quarterDims - 1, int2(0, 0));
        int2 qp = clamp(int2(floor(renderPos * 0.25f)), int2(0, 0), quarterMax);
        float dC = g_PatchDiv.Load(int3(qp, 0)).x;
        float dL = g_PatchDiv.Load(int3(clamp(qp + int2(-1, 0), int2(0, 0), quarterMax), 0)).x;
        float dR = g_PatchDiv.Load(int3(clamp(qp + int2( 1, 0), int2(0, 0), quarterMax), 0)).x;
        float dU = g_PatchDiv.Load(int3(clamp(qp + int2( 0,-1), int2(0, 0), quarterMax), 0)).x;
        float dD = g_PatchDiv.Load(int3(clamp(qp + int2( 0, 1), int2(0, 0), quarterMax), 0)).x;
        float patch2D = min(max(dL, dR), max(dU, dD));
        float patchScore = min(dC, patch2D);
        patchDiv = smoothstep(0.075f, 0.20f, patchScore);
    }

    // 1. 3x3 current-frame neighborhood in YCoCg + adaptive jitter-aware filter
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

    float3 yccGrid[9];
    float filterSigma = lerp(0.47f, 0.36f, saturate(velMagPx * 0.25f + disocc + patchDiv));

    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            int2 sp = clamp(baseRenderPx + int2(dx, dy), int2(0, 0), maxRenderPx);
            float4 rawSample = g_CurrColor.Load(int3(sp, 0));
            if (dx == 0 && dy == 0) {
                centerAlpha = rawSample.a;
            }
            float3 ycc = RGBToYCoCg(TonemapForward(rawSample.rgb, effectiveHdr));
            yccGrid[(dy + 1) * 3 + (dx + 1)] = ycc;

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
        float3 outRgb = TonemapInverse(YCoCgToRGB(filteredCurr), effectiveHdr);
        g_OutColor[px] = float4(outRgb, centerAlpha);
        g_OutAux[px]   = float4(1.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // 2. History fetch: exact center bypass in static/cockpit, Catmull-Rom in motion
    float2 prevUV = currUV + mvUV;
    bool offscreen = any(prevUV < 0.0f) || any(prevUV > 1.0f);

    float3 histTm;
    float4 prevAux;
    float  prevSamples = 0.0f;
    if (offscreen) {
        histTm  = YCoCgToRGB(filteredCurr);
        prevAux = float4(0.0f, 0.0f, 0.0f, 0.0f);
        prevSamples = 0.0f;
        disocc  = 1.0f;
    } else if ((hasClass && mClass == kClassCockpitStatic) || velMagPx < 0.05f) {
        histTm  = TonemapForward(g_HistoryColor.Load(int3(px, 0)).rgb, effectiveHdr);
        prevAux = g_HistoryAux.Load(int3(px, 0));
        prevSamples = prevAux.x;
    } else {
        histTm  = SampleHistoryCatmullRom(prevUV, effectiveHdr);
        prevAux = g_HistoryAux.SampleLevel(g_LinearClamp, prevUV, 0.0f);
        prevSamples = prevAux.x;
        if (velMagPx > 0.85f) {
            // Point-min sampleCount across 2x2 footprint in faster motion (>0.85px) so high history counts
            // never bleed across silhouettes, while sub-pixel (<0.85px) thin features keep their count.
            int2 auxBase = int2(floor(prevUV * outputRes.xy - 0.5f));
            float s00 = g_HistoryAux.Load(int3(clamp(auxBase + int2(0, 0), int2(0, 0), maxOutPx), 0)).x;
            float s10 = g_HistoryAux.Load(int3(clamp(auxBase + int2(1, 0), int2(0, 0), maxOutPx), 0)).x;
            float s01 = g_HistoryAux.Load(int3(clamp(auxBase + int2(0, 1), int2(0, 0), maxOutPx), 0)).x;
            float s11 = g_HistoryAux.Load(int3(clamp(auxBase + int2(1, 1), int2(0, 0), maxOutPx), 0)).x;
            float minSamples = min(min(s00, s10), min(s01, s11));
            prevSamples = lerp(prevAux.x, minSamples, saturate((velMagPx - 0.85f) * 2.0f));
        }
    }

    float3 histYCoCg = RGBToYCoCg(histTm);

    // Gate patchDiv by pixel mismatch and stationary emissive reactive by distance outside
    // the 3x3 spatial envelope so static AA edges & slow edges in mixed blocks stay smooth.
    float patchPixelDiff = abs(histYCoCg.x - centerYCoCg.x) + 0.5f * length(histYCoCg.yz - centerYCoCg.yz);
    patchDiv *= smoothstep(0.045f, 0.14f, patchPixelDiff);
    if (velMagPx < 0.25f && disocc < 0.05f && reactive < 0.96f &&
        !(hasClass && (mClass == kClassOffscreenFG || mClass == kClassReset))) {
        float outsideBox = length(max(0.0f, min3x3 - histYCoCg) + max(0.0f, histYCoCg - max3x3));
        reactive *= smoothstep(0.025f, 0.10f, outsideBox);
    }

    float prevLockLife    = prevAux.y;
    float prevOsc         = prevAux.z;
    float prevSignedDelta = prevAux.w;

    // 3. 2D Vector velocity shock + Sign-alternating oscillation + Thin-feature lock update
    float velShock = 0.0f;
    if (!offscreen && hasPrevPrepass) {
        float2 prevMvUV = g_PrevPrepass.SampleLevel(g_PointClamp, clamp(prevUV, 0.0f, 1.0f), 0.0f).xy;
        float velDelta = length(mvPxOut - prevMvUV * outputRes.xy);
        velShock = saturate((velDelta - 1.25f) * 0.25f);
    }

    float stationaryWeight = saturate(1.0f - velMagPx * 4.0f);
    float centerDrop = histYCoCg.x - centerYCoCg.x;
    float neighborShifted = 0.0f;
    if (stationaryWeight > 0.0f && prevLockLife > 0.05f && thin < 0.05f && abs(centerDrop) > 0.08f) {
        // Probe +-2..3 px (beyond +-0.5 px Halton jitter excursion) to detect real thin-line
        // translation without false-triggering on stationary sub-pixel jittered lines.
        static const int2 kShiftOffs[8] = {
            int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2),
            int2(-3, 0), int2(3, 0), int2(0, -3), int2(0, 3)
        };
        [unroll]
        for (int si = 0; si < 8; ++si) {
            int2 sp = clamp(baseRenderPx + kShiftOffs[si], int2(0, 0), maxRenderPx);
            float cN = RGBToYCoCg(TonemapForward(g_CurrColor.Load(int3(sp, 0)).rgb, effectiveHdr)).x;
            float2 hUv = clamp(prevUV + float2(kShiftOffs[si]) * outputRes.zw, 0.0f, 1.0f);
            float hN = RGBToYCoCg(TonemapForward(g_HistoryColor.SampleLevel(g_LinearClamp, hUv, 0.0f).rgb, effectiveHdr)).x;
            float nGain = cN - hN;
            if (nGain * centerDrop > 0.015f && abs(hN - centerYCoCg.x) < 0.15f) {
                neighborShifted = 1.0f;
            }
        }
    }

    float killLock = saturate(disocc + reactive * 1.5f + velShock + patchDiv * 1.5f + neighborShifted * 1.5f);
    float lockLife = max(prevLockLife * 0.86f, thin) * (1.0f - killLock);

    // 4. Variance Clipping + Clip-Toward-Curr + Motion-Gated Instability
)" R"(
    float3 mean  = m1 / 9.0f;
    float3 sigma = sqrt(max(m2 / 9.0f - mean * mean, 0.0f));

    float gamma = gammaAndReset.x; // baseGamma
    gamma = lerp(gamma, 0.80f, saturate(velMagPx * 0.12f + reactive + velShock));
    gamma = lerp(gamma, 0.65f, patchDiv);
    gamma = lerp(gamma, gammaAndReset.y, lockLife * (1.0f - saturate(velMagPx * 0.20f + patchDiv * 2.0f)));
    float3 gammaVec = float3(gamma, lerp(gamma, max(gamma, 1.10f), patchDiv), lerp(gamma, max(gamma, 1.10f), patchDiv));

    float3 softMin = 0.5f * (min3x3 + minCross);
    float3 softMax = 0.5f * (max3x3 + maxCross);
    float3 varMin  = mean - gammaVec * sigma;
    float3 varMax  = mean + gammaVec * sigma;

    // Balance 3x3 diagonal corner retention in static scenes with cross-rounded clamping in motion.
    float softBoxWeight = lerp(0.45f, 1.0f, saturate(velMagPx * 0.25f + patchDiv));
    float3 boxMinEff = lerp(min3x3, softMin, softBoxWeight);
    float3 boxMaxEff = lerp(max3x3, softMax, softBoxWeight);

    float lockBoxExpand = lockLife * (1.0f - saturate(patchDiv * 2.0f));
    float3 clipMin = lerp(max(varMin, boxMinEff), varMin, lockBoxExpand);
    float3 clipMax = lerp(min(varMax, boxMaxEff), varMax, lockBoxExpand);
    if (patchDiv > 0.10f || neighborShifted > 0.5f) {
        clipMin = max(clipMin, min3x3);
        clipMax = min(clipMax, max3x3);
    }

    float3 clipTarget = lerp(0.5f * (clipMin + clipMax), filteredCurr, saturate(patchDiv + neighborShifted));
    float maUnit = 0.0f;
    float3 clippedHist = ClipAABBToward(clipMin, clipMax, clipTarget, histYCoCg, maUnit);

    // Sub-pixel (<1px) stationary thin-ridge retention across missed Halton jitter phases.
    // Gated by maUnit > 1.0 so unclipped pixels skip all 8 extra history loads.
    float statRidgeWeight = stationaryWeight * (1.0f - killLock);
    float histThinRidge = 0.0f;
    if (maUnit > 1.0f && statRidgeWeight > 0.0f && prevLockLife > 0.05f &&
        neighborShifted < 0.5f && patchDiv < 0.10f) {
        float2 ht = outputRes.zw;
        float hL  = RGBToYCoCg(TonemapForward(g_HistoryColor.SampleLevel(g_LinearClamp, clamp(prevUV + float2(-ht.x, 0.0f), 0.0f, 1.0f), 0.0f).rgb, effectiveHdr)).x;
        float hR  = RGBToYCoCg(TonemapForward(g_HistoryColor.SampleLevel(g_LinearClamp, clamp(prevUV + float2( ht.x, 0.0f), 0.0f, 1.0f), 0.0f).rgb, effectiveHdr)).x;
        float hU  = RGBToYCoCg(TonemapForward(g_HistoryColor.SampleLevel(g_LinearClamp, clamp(prevUV + float2(0.0f, -ht.y), 0.0f, 1.0f), 0.0f).rgb, effectiveHdr)).x;
        float hD  = RGBToYCoCg(TonemapForward(g_HistoryColor.SampleLevel(g_LinearClamp, clamp(prevUV + float2(0.0f,  ht.y), 0.0f, 1.0f), 0.0f).rgb, effectiveHdr)).x;
        float hL2 = RGBToYCoCg(TonemapForward(g_HistoryColor.SampleLevel(g_LinearClamp, clamp(prevUV + float2(-2.0f * ht.x, 0.0f), 0.0f, 1.0f), 0.0f).rgb, effectiveHdr)).x;
        float hR2 = RGBToYCoCg(TonemapForward(g_HistoryColor.SampleLevel(g_LinearClamp, clamp(prevUV + float2( 2.0f * ht.x, 0.0f), 0.0f, 1.0f), 0.0f).rgb, effectiveHdr)).x;
        float hU2 = RGBToYCoCg(TonemapForward(g_HistoryColor.SampleLevel(g_LinearClamp, clamp(prevUV + float2(0.0f, -2.0f * ht.y), 0.0f, 1.0f), 0.0f).rgb, effectiveHdr)).x;
        float hD2 = RGBToYCoCg(TonemapForward(g_HistoryColor.SampleLevel(g_LinearClamp, clamp(prevUV + float2(0.0f,  2.0f * ht.y), 0.0f, 1.0f), 0.0f).rgb, effectiveHdr)).x;

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
        histThinRidge = saturate((ridgeStr - 0.008f) * 28.0f);
        clippedHist = lerp(clippedHist, histYCoCg, 0.97f * statRidgeWeight * histThinRidge * saturate(prevLockLife * 2.0f));
    }

    // Motion-gated instability: dampens stationary/slow-pan jitter shimmer while NEVER
    // trapping trailing history behind unvectored moving objects (patchDiv / neighborShifted / large maUnit).
    float lumaDiff = abs(histYCoCg.x - centerYCoCg.x);
    float excThresh = (velMagPx > 0.05f && velMagPx < 0.75f) ? 2.40f : 1.65f;
    float clipDist = abs(histYCoCg.x - clippedHist.x) + 0.5f * length(histYCoCg.yz - clippedHist.yz);
    float significantClip = smoothstep(0.045f, 0.12f, clipDist);
    float clipExcursion = saturate((maUnit - excThresh) * 2.0f) * significantClip * (1.0f - lockLife * stationaryWeight) * (1.0f - histThinRidge * statRidgeWeight);
    float unvectoredFlag = saturate(patchDiv + neighborShifted + clipExcursion);
    float rawInstab = saturate((lumaDiff - 0.04f) * 6.0f) * (1.0f - disocc) * (1.0f - reactive) * (1.0f - unvectoredFlag);
    float oscillation = lerp(prevOsc * 0.85f, rawInstab, 0.35f) * (1.0f - unvectoredFlag);
    float currDelta = centerYCoCg.x - histYCoCg.x;

    // 5. Progressive convergence + Resample-Blur Compensation + Clip Rejection
    float clipReject = saturate((maUnit - excThresh) * 0.45f) * significantClip * (1.0f - lockLife * stationaryWeight) * (1.0f - histThinRidge * statRidgeWeight);
    float sampleDecay = saturate(disocc + reactive * 0.85f + patchDiv * 0.85f + neighborShifted * 0.85f + clipReject * 0.65f + velShock * 0.50f);
    float validSamples = prevSamples * (1.0f - sampleDecay);
    float newSamples   = min(validSamples + 1.0f, 64.0f);
    float convAlpha    = 1.0f / max(newSamples, 1.0f);

    float2 fPrev = abs(frac(prevUV * outputRes.xy - 0.5f) - 0.5f);
    float thinLockProt = saturate(max(lockLife, thin) * 2.0f);
    float resampleBlur = (velMagPx >= 0.25f) ? ((1.0f - 2.0f * min(fPrev.x, fPrev.y)) * (1.0f - thinLockProt)) : 0.0f;

    float motionBoost = saturate(reactive * 0.9f + velShock * 0.5f + velMagPx * 0.04f + resampleBlur * 0.55f);
    float minAlphaEff = lerp(jitterAndAlpha.z, jitterAndAlpha.w, motionBoost);
    minAlphaEff = max(minAlphaEff, max(max(patchDiv * 0.65f, neighborShifted * 0.65f), clipReject * 0.45f));

    float flickerDamp = lerp(1.0f, 0.35f,
        max(oscillation, max(lockLife, histThinRidge)) * (1.0f - saturate(killLock + clipReject + unvectoredFlag)));
    float alpha = max(convAlpha * lerp(1.0f, flickerDamp, saturate((newSamples - 6.0f) * 0.20f)),
                      max(minAlphaEff * flickerDamp, resampleBlur * 0.16f));

    // Zero-fetch Edge-Aware Spatial AA fallback when history is thin (disocclusion / fast motion)
    float3 currForBlend = (newSamples < 2.0f) ? lerp(centerYCoCg, filteredCurr, 0.85f) : filteredCurr;
    if (newSamples < 2.5f) {
        float yTL = yccGrid[0].x, yT = yccGrid[1].x, yTR = yccGrid[2].x;
        float yL  = yccGrid[3].x,                    yR  = yccGrid[5].x;
        float yBL = yccGrid[6].x, yB = yccGrid[7].x, yBR = yccGrid[8].x;
        float gx = (yTR + 2.0f * yR + yBR) - (yTL + 2.0f * yL + yBL);
        float gy = (yBL + 2.0f * yB + yBR) - (yTL + 2.0f * yT + yTR);
        float edgeMag = sqrt(gx * gx + gy * gy);
        if (edgeMag > 0.08f) {
            bool horizEdge = abs(gy) >= abs(gx);
            float3 tA = horizEdge ? yccGrid[3] : yccGrid[1];
            float3 tB = horizEdge ? yccGrid[5] : yccGrid[7];
            float3 tangentAvg = 0.5f * (tA + tB);
            float aaWeight = 0.28f * saturate((2.5f - newSamples) * 0.67f) * saturate((edgeMag - 0.08f) * 3.0f);
            currForBlend = lerp(currForBlend, tangentAvg, aaWeight);
        }
    }

    if (disocc > 0.95f) {
        alpha = 1.0f;
    }

    float3 resolvedYCoCg = lerp(clippedHist, currForBlend, saturate(alpha));
    float3 outRgb = TonemapInverse(YCoCgToRGB(resolvedYCoCg), effectiveHdr);

    if (any(isnan(outRgb) || isinf(outRgb))) {
        outRgb = max(g_CurrColor.Load(int3(renderPx, 0)).rgb, 0.0f);
    }

    g_OutColor[px] = float4(outRgb, centerAlpha);
    g_OutAux[px]   = float4(newSamples, lockLife, oscillation, currDelta);
}
)";

// ============================================================================
// EDPE TAAv2 - Pass 3: Instability-Aware Post-RCAS (Outside History Loop)
// Performs CAS weights in Peak-Karis normalized space; reaches exact 0 at effSharp == 0.
// ============================================================================
inline constexpr char kTaaPostRcasShader[] = R"(
cbuffer RcasCB : register(b0) {
    float2 g_Resolution;
    float  g_Sharpness;
    float  g_EffectiveHdr;
};

Texture2D<float4>   g_TaaColor : register(t0);
Texture2D<float4>   g_TaaAux   : register(t1); // R = sampleCount, G = lockLife, B = oscillation
RWTexture2D<float4> g_OutFinal : register(u0);

float3 TonemapForward(float3 rgb, bool effectiveHdr) {
    rgb = max(rgb, 0.0f);
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    return rgb / (1.0f + peak);
}

float3 TonemapInverse(float3 tm, bool effectiveHdr) {
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

    bool effectiveHdr = (g_EffectiveHdr > 0.5f);
    float3 cTm = TonemapForward(center.rgb, effectiveHdr);
    float3 nTm = TonemapForward(g_TaaColor.Load(int3(clamp(px + int2( 0,-1), int2(0, 0), maxPx), 0)).rgb, effectiveHdr);
    float3 sTm = TonemapForward(g_TaaColor.Load(int3(clamp(px + int2( 0, 1), int2(0, 0), maxPx), 0)).rgb, effectiveHdr);
    float3 wTm = TonemapForward(g_TaaColor.Load(int3(clamp(px + int2(-1, 0), int2(0, 0), maxPx), 0)).rgb, effectiveHdr);
    float3 eTm = TonemapForward(g_TaaColor.Load(int3(clamp(px + int2( 1, 0), int2(0, 0), maxPx), 0)).rgb, effectiveHdr);

    float4 aux = g_TaaAux.Load(int3(px, 0));
    float sampleGate = saturate((aux.x - 1.0f) * 0.25f + 0.20f);
    float effSharp = saturate(g_Sharpness) * (1.0f - saturate(aux.z * 1.25f)) * sampleGate;

    float3 minRGB = min(cTm, min(min(nTm, sTm), min(wTm, eTm)));
    float3 maxRGB = max(cTm, max(max(nTm, sTm), max(wTm, eTm)));
    float3 amp    = saturate(min(minRGB, 2.0f - maxRGB) / max(maxRGB, 1e-4f));
    float  peak   = -0.20f * effSharp;
    float3 weight = sqrt(amp) * peak;

    float3 sharpenedTm = saturate((cTm + (nTm + sTm + wTm + eTm) * weight) / max(1.0f + 4.0f * weight, 1e-4f));
    float3 outRgb = TonemapInverse(sharpenedTm, effectiveHdr);
    if (any(isnan(outRgb) || isinf(outRgb))) {
        outRgb = max(center.rgb, 0.0f);
    }
    g_OutFinal[px] = float4(outRgb, center.a);
}
)";

// ============================================================================
// EDPE TAAv2 - Display Blit (with Inline Post-RCAS) & Read-Only Debug Views (1..4)
// Supports arbitrary destination RTV formats and DEV debug visualizations
// without ever modifying historyTexture_ or historyAuxTex_.
// ============================================================================
inline constexpr char kTaaBlitShader[] = R"(
cbuffer BlitCB : register(b0) {
    float4 outputRes;  // x: outW, y: outH, z: 1/outW, w: 1/outH
    uint4  debugInfo;  // x: debugView (0=Off, 1=Disocclusion, 2=ThinFeatureLock, 3=LumaInstability, 4=SampleCount)
    float4 rcasParams; // x: rcasSharpness, y: effectiveHdr (0/1), zw: unused
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
    o.pos = float4(corner * float2(2.0f, -2.0f) + float2(-1, 1), 0.0f, 1.0f);
    o.uv = corner;
    return o;
}

float3 TonemapForwardBlit(float3 rgb) {
    rgb = max(rgb, 0.0f);
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    return rgb / (1.0f + peak);
}

float3 TonemapInverseBlit(float3 tm) {
    tm = max(tm, 0.0f);
    float peak = max(tm.r, max(tm.g, tm.b));
    float3 rgb = tm / max(1.0f - peak, 1.0f / 65504.0f);
    return min(rgb, float3(65024.0f, 65024.0f, 64512.0f));
}

float4 applyInlinePostRcas(int2 px, int2 maxPx, float sharpness) {
    float4 center = g_DisplayColor.Load(int3(px, 0));
    float3 cTm = TonemapForwardBlit(center.rgb);
    float3 nTm = TonemapForwardBlit(g_DisplayColor.Load(int3(clamp(px + int2( 0,-1), int2(0, 0), maxPx), 0)).rgb);
    float3 sTm = TonemapForwardBlit(g_DisplayColor.Load(int3(clamp(px + int2( 0, 1), int2(0, 0), maxPx), 0)).rgb);
    float3 wTm = TonemapForwardBlit(g_DisplayColor.Load(int3(clamp(px + int2(-1, 0), int2(0, 0), maxPx), 0)).rgb);
    float3 eTm = TonemapForwardBlit(g_DisplayColor.Load(int3(clamp(px + int2( 1, 0), int2(0, 0), maxPx), 0)).rgb);

    float4 aux = g_Aux.Load(int3(px, 0));
    float sampleGate = saturate((aux.x - 1.0f) * 0.25f + 0.20f);
    float effSharp = saturate(sharpness) * (1.0f - saturate(aux.z * 1.25f)) * sampleGate;

    float3 minRGB = min(cTm, min(min(nTm, sTm), min(wTm, eTm)));
    float3 maxRGB = max(cTm, max(max(nTm, sTm), max(wTm, eTm)));
    float3 amp    = saturate(min(minRGB, 2.0f - maxRGB) / max(maxRGB, 1e-4f));
    float  peak   = -0.20f * effSharp;
    float3 weight = sqrt(amp) * peak;

    float3 sharpenedTm = saturate((cTm + (nTm + sTm + wTm + eTm) * weight) / max(1.0f + 4.0f * weight, 1e-4f));
    float3 outRgb = TonemapInverseBlit(sharpenedTm);
    if (any(isnan(outRgb) || isinf(outRgb))) {
        outRgb = max(center.rgb, 0.0f);
    }
    return float4(outRgb, center.a);
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
        // 3 = LumaOscillation (magenta/amber)
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
        if (rcasParams.x > 1e-4f) {
            return applyInlinePostRcas(px, maxPx, rcasParams.x);
        }
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
