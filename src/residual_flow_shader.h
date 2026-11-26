#pragma once

namespace edpe {

// ============================================================================
// EDPE Sparse Coarse-to-Fine Residual Optical Flow Shaders (cs_5_0, -O1 safe)
//
// Refines camera+depth base motion vectors with strictly confidence-gated
// residual screen-space displacement for moving unvectored world geometry
// (ships, SRVs, NPCs, station rings, animated parts) while protecting sky,
// static cockpit/weapon viewmodels, stationary sub-pixel lines, and flat areas.
// ============================================================================

inline constexpr char kRFlowPyr0Shader[] = R"(
static const uint kClassSky           = 0u;
static const uint kClassCockpitStatic = 2u;
static const uint kClassWeapon        = 4u;
static const uint kClassReset         = 7u;

cbuffer RFlowPyr0CB : register(b0) {
    float4 renderRes; // x: rW, y: rH, z: 1/rW, w: 1/rH
    float4 quarterRes;// x: qW, y: qH, z: 1/qW, w: 1/qH
    float4 flags;     // x: hasClass (0/1), yzw: unused
};

Texture2D<float4> g_CurrColor   : register(t0);
Texture2D<float>  g_CurrDepth   : register(t1);
Texture2D<uint>   g_MotionClass : register(t2);

RWTexture2D<float4> g_OutLumaQ  : register(u0); // x: meanY, y: maxY-minY, z: repDepth, w: eligible (0/1)

float KarisLuma(float3 rgb) {
    rgb = max(rgb, 0.0f);
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    float3 tm = rgb / (1.0f + peak);
    return dot(tm, float3(0.25f, 0.50f, 0.25f));
}

[numthreads(8, 8, 1)]
void cs_rflow_pyr0(uint3 dtid : SV_DispatchThreadID) {
    int2 qp = int2(dtid.xy);
    int2 qDims = int2(quarterRes.xy);
    if (qp.x >= qDims.x || qp.y >= qDims.y) {
        return;
    }

    bool hasClass = (flags.x > 0.5f);
    int2 rDims = int2(renderRes.xy);
    int2 basePx = qp * 4;

    float sumY = 0.0f;
    float minY = 1e9f;
    float maxY = -1e9f;
    float maxDepth = 0.0f;
    float minDepth = 1e9f;
    float dGrid[16];
    int validCount = 0;
    int eligCount = 0;

    [unroll]
    for (int dy = 0; dy < 4; ++dy) {
        [unroll]
        for (int dx = 0; dx < 4; ++dx) {
            int2 rp = basePx + int2(dx, dy);
            float d = 0.0f;
            if (rp.x < rDims.x && rp.y < rDims.y) {
                float y = KarisLuma(g_CurrColor.Load(int3(rp, 0)).rgb);
                d = g_CurrDepth.Load(int3(rp, 0));
                if (!isfinite(y)) y = 0.0f;
                if (!isfinite(d)) d = 0.0f;
                sumY += y;
                minY = min(minY, y);
                maxY = max(maxY, y);
                maxDepth = max(maxDepth, d);
                minDepth = min(minDepth, d);
                ++validCount;

                bool ok = (d > 1e-6f);
                if (hasClass && ok) {
                    uint cls = g_MotionClass.Load(int3(rp, 0));
                    if (cls == kClassSky || cls == kClassCockpitStatic ||
                        cls == kClassWeapon || cls == kClassReset) {
                        ok = false;
                    }
                }
                if (ok) {
                    ++eligCount;
                }
            }
            dGrid[dy * 4 + dx] = d;
        }
    }

    float repDepth = maxDepth;
    bool isThinSpike = false;
    if (validCount > 0 && maxDepth > max(minDepth * 1.35f, 1e-5f)) {
        int nearMaxCount = 0;
        float bgSum = 0.0f;
        int bgCount = 0;
        float fgThresh = maxDepth * 0.70f;
        [unroll]
        for (int k = 0; k < 16; ++k) {
            if (dGrid[k] >= fgThresh) {
                ++nearMaxCount;
            } else if (dGrid[k] > 0.0f) {
                bgSum += dGrid[k];
                ++bgCount;
            }
        }
        // Reject isolated <=1.25px thin wires/spikes (<6 of 16 pixels at foreground depth)
        // so Halton jitter crossing a 4x4 boundary never induces fake 4px flow.
        if (nearMaxCount * 16 < validCount * 6) {
            isThinSpike = true;
            repDepth = (bgCount > 0) ? (bgSum / float(bgCount)) : minDepth;
        }
    }

    float invN = (validCount > 0) ? (1.0f / float(validCount)) : 0.0f;
    float meanY = sumY * invN;
    float contrast = (!isThinSpike && validCount > 0) ? max(maxY - minY, 0.0f) : 0.0f;
    int minElig = hasClass ? ((validCount + 1) / 2) : 1;
    float eligible = (!isThinSpike && repDepth > 1e-6f && eligCount >= minElig) ? 1.0f : 0.0f;

    g_OutLumaQ[qp] = float4(meanY, contrast, repDepth, eligible);
}
)";

inline constexpr char kRFlowPyr1Shader[] = R"(
cbuffer RFlowPyr1CB : register(b0) {
    float4 quarterRes; // x: qW, y: qH, z: 1/qW, w: 1/qH
    float4 eighthRes;  // x: eW, y: eH, z: 1/eW, w: 1/eH
};

Texture2D<float4> g_LumaQ : register(t0);
RWTexture2D<float4> g_OutLumaE : register(u0); // x: meanY, y: maxContrast, z: maxDepth, w: anyEligible

[numthreads(8, 8, 1)]
void cs_rflow_pyr1(uint3 dtid : SV_DispatchThreadID) {
    int2 ep = int2(dtid.xy);
    int2 eDims = int2(eighthRes.xy);
    if (ep.x >= eDims.x || ep.y >= eDims.y) {
        return;
    }

    int2 qDims = int2(quarterRes.xy);
    int2 baseQ = ep * 2;
    float sumY = 0.0f;
    float maxC = 0.0f;
    float maxD = 0.0f;
    float elig = 0.0f;
    int validCount = 0;

    [unroll]
    for (int dy = 0; dy < 2; ++dy) {
        [unroll]
        for (int dx = 0; dx < 2; ++dx) {
            int2 qp = baseQ + int2(dx, dy);
            if (qp.x < qDims.x && qp.y < qDims.y) {
                float4 q = g_LumaQ.Load(int3(qp, 0));
                sumY += q.x;
                maxC = max(maxC, q.y);
                maxD = max(maxD, q.z);
                elig = max(elig, q.w);
                ++validCount;
            }
        }
    }

    float invN = (validCount > 0) ? (1.0f / float(validCount)) : 0.0f;
    g_OutLumaE[ep] = float4(sumY * invN, maxC, maxD, elig);
}
)";

inline constexpr char kRFlowCoarseShader[] = R"(
cbuffer RFlowSearchCB : register(b0) {
    float4 renderRes;  // x: rW, y: rH, z: 1/rW, w: 1/rH
    float4 gridRes;    // x: gW, y: gH, z: 1/gW, w: 1/gH (eighthRes here)
    float4 extraRes;   // x: unused, y: unused, z: jitterDeltaX, w: jitterDeltaY (prevJ - currJ)
    float4 flags;      // x: hasPrev (0/1), y: reset (0/1), z: unused, w: unused
};

Texture2D<float4> g_CurrLumaE   : register(t0);
Texture2D<float4> g_PrevLumaE   : register(t1);
Texture2D<float2> g_BaseMotion  : register(t2);

RWTexture2D<float4> g_OutCoarseFlow : register(u0); // xy: bestDeltaRenderPx, z: bestSad, w: baseSad

SamplerState g_LinearClamp : register(s0);

float evalSadE(int2 ep, int2 eMax, float2 baseMv, float2 deltaRenderPx,
               float currY[9], float currD[9]) {
    static const int2 kOff[9] = {
        int2(-1, -1), int2(0, -1), int2(1, -1),
        int2(-1,  0), int2(0,  0), int2(1,  0),
        int2(-1,  1), int2(0,  1), int2(1,  1)
    };
    static const float kW[9] = {
        0.25f, 0.50f, 0.25f,
        0.50f, 1.00f, 0.50f,
        0.25f, 0.50f, 0.25f
    };

    float2 jDelta = extraRes.zw;
    float sad = 0.0f;
    [unroll]
    for (int i = 0; i < 9; ++i) {
        int2 tep = clamp(ep + kOff[i], int2(0, 0), eMax);
        float2 currCenterPx = float2(tep) * 8.0f + 4.0f;
        float2 prevPx = currCenterPx + baseMv + deltaRenderPx + jDelta;
        float2 prevUv = prevPx * 0.125f * gridRes.zw;
        float pSad = 0.25f;
        if (!any(prevUv < 0.0f) && !any(prevUv > 1.0f)) {
            float4 prevSample = g_PrevLumaE.SampleLevel(g_LinearClamp, prevUv, 0.0f);
            pSad = abs(currY[i] - prevSample.x);
            if (currD[i] > 1e-6f && prevSample.z <= 1e-6f) {
                pSad += 0.15f;
            }
        }
        sad += kW[i] * pSad;
    }
    return sad * 0.25f; // sum(kW) == 4.0
}

[numthreads(8, 8, 1)]
void cs_rflow_coarse(uint3 dtid : SV_DispatchThreadID) {
    int2 ep = int2(dtid.xy);
    int2 eDims = int2(gridRes.xy);
    if (ep.x >= eDims.x || ep.y >= eDims.y) {
        return;
    }

    if (flags.x < 0.5f || flags.y > 0.5f) {
        g_OutCoarseFlow[ep] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    float4 centerE = g_CurrLumaE.Load(int3(ep, 0));
    if (centerE.w < 0.5f) {
        g_OutCoarseFlow[ep] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    int2 eMax = max(eDims - 1, int2(0, 0));
    float currY[9];
    float currD[9];
    float minY = centerE.x;
    float maxY = centerE.x;
    float maxC = centerE.y;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            int idx = (dy + 1) * 3 + (dx + 1);
            int2 tep = clamp(ep + int2(dx, dy), int2(0, 0), eMax);
            float4 s = g_CurrLumaE.Load(int3(tep, 0));
            currY[idx] = s.x;
            currD[idx] = s.z;
            if (s.w > 0.5f && abs(s.z - centerE.z) <= max(centerE.z * 0.35f, 1e-5f)) {
                minY = min(minY, s.x);
                maxY = max(maxY, s.x);
                maxC = max(maxC, s.y);
            }
        }
    }

    float patchContrast = max(maxC, maxY - minY);
    if (patchContrast < 0.025f) {
        g_OutCoarseFlow[ep] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    int2 maxRenderPx = max(int2(renderRes.xy) - 1, int2(0, 0));
    int2 centerRenderPx = clamp(ep * 8 + int2(4, 4), int2(0, 0), maxRenderPx);
    float2 baseMv = g_BaseMotion.Load(int3(centerRenderPx, 0));
    if (!all(isfinite(baseMv))) {
        baseMv = float2(0.0f, 0.0f);
    }

    float baseSadJ   = evalSadE(ep, eMax, baseMv, float2(0.0f, 0.0f), currY, currD);
    float baseSadNoJ = evalSadE(ep, eMax, baseMv, -extraRes.zw,       currY, currD);
    float baseSad    = min(baseSadJ, baseSadNoJ);
    if (baseSad < 0.048f) {
        g_OutCoarseFlow[ep] = float4(0.0f, 0.0f, baseSad, baseSad);
        return;
    }

    float2 bestDelta = float2(0.0f, 0.0f);
    float bestCost = baseSad;
    float bestSad = baseSad;

    // Stage 1: 5x5 grid at step 8 px (+-16 px full 2D) + +-24 px outer ring (8 pts)
    [loop]
    for (int gy = -2; gy <= 2; ++gy) {
        [loop]
        for (int gx = -2; gx <= 2; ++gx) {
            if (gx == 0 && gy == 0) continue;
            float2 cand = float2(float(gx * 8), float(gy * 8));
            float sad = evalSadE(ep, eMax, baseMv, cand, currY, currD);
            float cost = sad + 0.0005f * length(cand);
            if (cost < bestCost) {
                bestCost = cost;
                bestSad = sad;
                bestDelta = cand;
            }
        }
    }

    static const float2 kOuter8[8] = {
        float2(-24.0f,   0.0f), float2( 24.0f,   0.0f),
        float2(  0.0f, -24.0f), float2(  0.0f,  24.0f),
        float2(-24.0f, -24.0f), float2( 24.0f, -24.0f),
        float2(-24.0f,  24.0f), float2( 24.0f,  24.0f)
    };
    [unroll]
    for (int oi = 0; oi < 8; ++oi) {
        float2 cand = kOuter8[oi];
        float sad = evalSadE(ep, eMax, baseMv, cand, currY, currD);
        float cost = sad + 0.0005f * length(cand);
        if (cost < bestCost) {
            bestCost = cost;
            bestSad = sad;
            bestDelta = cand;
        }
    }

    // Stage 2: +-4 px refinement around Stage 1 winner
    float2 stage1Delta = bestDelta;
    static const float2 kStep4[8] = {
        float2(-4.0f,  0.0f), float2( 4.0f,  0.0f),
        float2( 0.0f, -4.0f), float2( 0.0f,  4.0f),
        float2(-4.0f, -4.0f), float2( 4.0f, -4.0f),
        float2(-4.0f,  4.0f), float2( 4.0f,  4.0f)
    };
    [unroll]
    for (int si = 0; si < 8; ++si) {
        float2 cand = stage1Delta + kStep4[si];
        float sad = evalSadE(ep, eMax, baseMv, cand, currY, currD);
        float cost = sad + 0.0005f * length(cand);
        if (cost < bestCost) {
            bestCost = cost;
            bestSad = sad;
            bestDelta = cand;
        }
    }

    g_OutCoarseFlow[ep] = float4(bestDelta.x, bestDelta.y, bestSad, baseSad);
}
)";

inline constexpr char kRFlowRefineShader[] = R"(
cbuffer RFlowRefineCB : register(b0) {
    float4 renderRes;  // x: rW, y: rH, z: 1/rW, w: 1/rH
    float4 gridRes;    // x: qW, y: qH, z: 1/qW, w: 1/qH (quarterRes)
    float4 extraRes;   // x: eW, y: eH, z: jitterDeltaX, w: jitterDeltaY
    float4 flags;      // x: hasPrev (0/1), y: reset (0/1), z: unused, w: unused
};

Texture2D<float4> g_CurrLumaQ   : register(t0);
Texture2D<float4> g_PrevLumaQ   : register(t1);
Texture2D<float2> g_BaseMotion  : register(t2);
Texture2D<float4> g_CoarseFlow  : register(t3);

RWTexture2D<float4> g_OutRefineFlow : register(u0); // xy: deltaRenderPx, z: confidence [0,1], w: currQ.z (maxDepth)

SamplerState g_LinearClamp : register(s0);

float evalSadQ(int2 qp, int2 qMax, float2 baseMv, float2 deltaRenderPx,
               float currY[9], float currD[9]) {
    static const int2 kOff[9] = {
        int2(-1, -1), int2(0, -1), int2(1, -1),
        int2(-1,  0), int2(0,  0), int2(1,  0),
        int2(-1,  1), int2(0,  1), int2(1,  1)
    };
    static const float kW[9] = {
        0.25f, 0.50f, 0.25f,
        0.50f, 1.00f, 0.50f,
        0.25f, 0.50f, 0.25f
    };

    float2 jDelta = extraRes.zw;
    float sad = 0.0f;
    [unroll]
    for (int i = 0; i < 9; ++i) {
        int2 tqp = clamp(qp + kOff[i], int2(0, 0), qMax);
        float2 currCenterPx = float2(tqp) * 4.0f + 2.0f;
        float2 prevPx = currCenterPx + baseMv + deltaRenderPx + jDelta;
        float2 prevUv = prevPx * 0.25f * gridRes.zw;
        float pSad = 0.25f;
        if (!any(prevUv < 0.0f) && !any(prevUv > 1.0f)) {
            float4 prevSample = g_PrevLumaQ.SampleLevel(g_LinearClamp, prevUv, 0.0f);
            pSad = abs(currY[i] - prevSample.x);
            if (currD[i] > 1e-6f && prevSample.z <= 1e-6f) {
                pSad += 0.15f;
            } else if (currD[i] > 1e-6f) {
                float dRel = abs(prevSample.z - currD[i]) / max(currD[i], 1e-5f);
                if (dRel > 0.35f) {
                    pSad += min(dRel - 0.35f, 1.0f) * 0.12f;
                }
            }
        }
        sad += kW[i] * pSad;
    }
    return sad * 0.25f;
}

[numthreads(8, 8, 1)]
void cs_rflow_refine(uint3 dtid : SV_DispatchThreadID) {
    int2 qp = int2(dtid.xy);
    int2 qDims = int2(gridRes.xy);
    if (qp.x >= qDims.x || qp.y >= qDims.y) {
        return;
    }

    float4 centerQ = g_CurrLumaQ.Load(int3(qp, 0));
    if (flags.x < 0.5f || flags.y > 0.5f || centerQ.w < 0.5f) {
        g_OutRefineFlow[qp] = float4(0.0f, 0.0f, 0.0f, centerQ.z);
        return;
    }

    int2 qMax = max(qDims - 1, int2(0, 0));
    float currY[9];
    float currD[9];
    float minY = centerQ.x;
    float maxY = centerQ.x;
    float maxC = centerQ.y;
    float centerDepthTol = max(centerQ.z * 0.35f, 1e-5f);

    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            int idx = (dy + 1) * 3 + (dx + 1);
            int2 tqp = clamp(qp + int2(dx, dy), int2(0, 0), qMax);
            float4 s = g_CurrLumaQ.Load(int3(tqp, 0));
            currY[idx] = s.x;
            currD[idx] = s.z;
            if (s.w > 0.5f && abs(s.z - centerQ.z) <= centerDepthTol) {
                minY = min(minY, s.x);
                maxY = max(maxY, s.x);
                maxC = max(maxC, s.y);
            }
        }
    }

    float patchContrast = max(maxC, maxY - minY);
    if (patchContrast < 0.025f) {
        g_OutRefineFlow[qp] = float4(0.0f, 0.0f, 0.0f, centerQ.z);
        return;
    }

    int2 maxRenderPx = max(int2(renderRes.xy) - 1, int2(0, 0));
    int2 centerRenderPx = clamp(qp * 4 + int2(2, 2), int2(0, 0), maxRenderPx);
    float2 baseMv = g_BaseMotion.Load(int3(centerRenderPx, 0));
    if (!all(isfinite(baseMv))) {
        baseMv = float2(0.0f, 0.0f);
    }

    float baseSadJ   = evalSadQ(qp, qMax, baseMv, float2(0.0f, 0.0f), currY, currD);
    float baseSadNoJ = evalSadQ(qp, qMax, baseMv, -extraRes.zw,       currY, currD);
    float baseSad    = min(baseSadJ, baseSadNoJ);

    // Gather starting candidates: (0,0) + center and 4 cardinal coarse neighbors
    int2 eDims = int2(extraRes.xy);
    int2 eMax = max(eDims - 1, int2(0, 0));
    int2 ep = clamp(qp / 2, int2(0, 0), eMax);
    static const int2 kCoarseOff[5] = {
        int2(0, 0), int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)
    };

    float2 initDelta = float2(0.0f, 0.0f);
    float bestCost = baseSad;
    float bestSad = baseSad;

    [unroll]
    for (int ci = 0; ci < 5; ++ci) {
        float2 cand = g_CoarseFlow.Load(int3(clamp(ep + kCoarseOff[ci], int2(0, 0), eMax), 0)).xy;
        if (dot(cand, cand) > 0.01f && all(isfinite(cand))) {
            float sad = evalSadQ(qp, qMax, baseMv, cand, currY, currD);
            float cost = sad + 0.0005f * length(cand);
            if (cost < bestCost) {
                bestCost = cost;
                bestSad = sad;
                initDelta = cand;
            }
        }
    }

    if (baseSad < 0.045f && length(initDelta) < 1.0f) {
        g_OutRefineFlow[qp] = float4(0.0f, 0.0f, 0.0f, centerQ.z);
        return;
    }

    // Fine Step 1: +-4 px 3x3 around initDelta
    float2 bestDelta = initDelta;
    [unroll]
    for (int dy4 = -1; dy4 <= 1; ++dy4) {
        [unroll]
        for (int dx4 = -1; dx4 <= 1; ++dx4) {
            if (dx4 == 0 && dy4 == 0) continue;
            float2 cand = initDelta + float2(float(dx4 * 4), float(dy4 * 4));
            float sad = evalSadQ(qp, qMax, baseMv, cand, currY, currD);
            float cost = sad + 0.0005f * length(cand);
            if (cost < bestCost) {
                bestCost = cost;
                bestSad = sad;
                bestDelta = cand;
            }
        }
    }

    // Fine Step 2: +-2 px 8-neighbor around step 1 winner
    float2 step1Delta = bestDelta;
    static const float2 kStep2[8] = {
        float2(-2.0f,  0.0f), float2( 2.0f,  0.0f),
        float2( 0.0f, -2.0f), float2( 0.0f,  2.0f),
        float2(-2.0f, -2.0f), float2( 2.0f, -2.0f),
        float2(-2.0f,  2.0f), float2( 2.0f,  2.0f)
    };
    [unroll]
    for (int s2 = 0; s2 < 8; ++s2) {
        float2 cand = step1Delta + kStep2[s2];
        float sad = evalSadQ(qp, qMax, baseMv, cand, currY, currD);
        float cost = sad + 0.0005f * length(cand);
        if (cost < bestCost) {
            bestCost = cost;
            bestSad = sad;
            bestDelta = cand;
        }
    }

    // Fine Step 3: +-1 px 4-cardinal + parabolic sub-pixel refinement
    float2 step2Delta = bestDelta;
    float sL = evalSadQ(qp, qMax, baseMv, step2Delta + float2(-1.0f,  0.0f), currY, currD);
    float sR = evalSadQ(qp, qMax, baseMv, step2Delta + float2( 1.0f,  0.0f), currY, currD);
    float sU = evalSadQ(qp, qMax, baseMv, step2Delta + float2( 0.0f, -1.0f), currY, currD);
    float sD = evalSadQ(qp, qMax, baseMv, step2Delta + float2( 0.0f,  1.0f), currY, currD);
    float sC = bestSad;

    float2 cand1[4] = {
        step2Delta + float2(-1.0f,  0.0f),
        step2Delta + float2( 1.0f,  0.0f),
        step2Delta + float2( 0.0f, -1.0f),
        step2Delta + float2( 0.0f,  1.0f)
    };
    float sad1[4] = { sL, sR, sU, sD };
    [unroll]
    for (int s1 = 0; s1 < 4; ++s1) {
        float cost = sad1[s1] + 0.0005f * length(cand1[s1]);
        if (cost < bestCost) {
            bestCost = cost;
            bestSad = sad1[s1];
            bestDelta = cand1[s1];
        }
    }

    // Parabolic subpixel step when centered on step2Delta
    if (length(bestDelta - step2Delta) < 0.01f) {
        float denomX = sL + sR - 2.0f * sC;
        float denomY = sU + sD - 2.0f * sC;
        float subX = (denomX > 1e-4f) ? clamp(0.5f * (sL - sR) / denomX, -0.5f, 0.5f) : 0.0f;
        float subY = (denomY > 1e-4f) ? clamp(0.5f * (sU - sD) / denomY, -0.5f, 0.5f) : 0.0f;
        float2 subCand = step2Delta + float2(subX, subY);
        float subSad = evalSadQ(qp, qMax, baseMv, subCand, currY, currD);
        if (subSad < bestSad) {
            bestSad = subSad;
            bestDelta = subCand;
        }
    }

    // Strict Confidence Gate:
    //   1) Meaningful SAD improvement (< 40% of baseSad) & low absolute residual error (< 0.055)
    //   2) Residual speed in [1.75, 32.0] px (immune to <= +-0.5 px Halton jitter & 4x4 blur shift)
    //   3) Directional uniqueness (+-4 px along winner direction is noticeably worse)
    //   4) Target prev pixel must not map foreground onto sky
    float dMag = length(bestDelta);
    float conf = 0.0f;
    if (bestSad < baseSad * 0.40f && bestSad < 0.055f && dMag >= 1.75f && dMag <= 32.0f) {
        float2 dir = bestDelta / max(dMag, 1e-4f);
        float sadPlus  = evalSadQ(qp, qMax, baseMv, bestDelta + dir * 4.0f, currY, currD);
        float sadMinus = evalSadQ(qp, qMax, baseMv, bestDelta - dir * 4.0f, currY, currD);
        if (max(sadPlus, sadMinus) > bestSad + 0.018f) {
            float2 prevCenterPx = float2(qp) * 4.0f + 2.0f + baseMv + bestDelta + extraRes.zw;
            float2 prevCenterUv = prevCenterPx * 0.25f * gridRes.zw;
            if (!any(prevCenterUv < 0.0f) && !any(prevCenterUv > 1.0f)) {
                float4 prevCenter = g_PrevLumaQ.SampleLevel(g_LinearClamp, prevCenterUv, 0.0f);
                if (prevCenter.z > 1e-6f &&
                    abs(prevCenter.z - centerQ.z) <= max(centerQ.z * 0.45f, 1e-5f)) {
                    conf = saturate((0.40f - bestSad / max(baseSad, 1e-4f)) * 4.5f) *
                           saturate((0.058f - bestSad) * 25.0f);
                }
            }
        }
    }

    g_OutRefineFlow[qp] = float4(bestDelta.x, bestDelta.y, conf, centerQ.z);
}
)";

inline constexpr char kRFlowApplyShader[] = R"(
static const uint kClassSky           = 0u;
static const uint kClassCockpitStatic = 2u;
static const uint kClassWeapon        = 4u;
static const uint kClassReset         = 7u;

cbuffer RFlowApplyCB : register(b0) {
    float4 renderRes; // x: rW, y: rH, z: 1/rW, w: 1/rH
    float4 qRes;      // x: qW, y: qH, z: 1/qW, w: 1/qH
    float4 flags;     // x: hasPrev (0/1), y: reset (0/1), z: hasClass (0/1), w: unused
};

Texture2D<float2> g_BaseMotion  : register(t0);
Texture2D<float>  g_CurrDepth   : register(t1);
Texture2D<uint>   g_MotionClass : register(t2);
Texture2D<float4> g_RefineFlow  : register(t3);

RWTexture2D<float2> g_OutMotion : register(u0);

[numthreads(8, 8, 1)]
void cs_rflow_apply(uint3 dtid : SV_DispatchThreadID) {
    int2 px = int2(dtid.xy);
    int2 rDims = int2(renderRes.xy);
    if (px.x >= rDims.x || px.y >= rDims.y) {
        return;
    }

    float2 baseMv = g_BaseMotion.Load(int3(px, 0));
    if (!all(isfinite(baseMv))) {
        baseMv = float2(0.0f, 0.0f);
    }

    if (flags.x < 0.5f || flags.y > 0.5f) {
        g_OutMotion[px] = clamp(baseMv, -65504.0f, 65504.0f);
        return;
    }

    float depth = g_CurrDepth.Load(int3(px, 0));
    if (!isfinite(depth) || depth <= 1e-6f) {
        g_OutMotion[px] = clamp(baseMv, -65504.0f, 65504.0f);
        return;
    }

    bool hasClass = (flags.z > 0.5f);
    if (hasClass) {
        uint cls = g_MotionClass.Load(int3(px, 0));
        if (cls == kClassSky || cls == kClassCockpitStatic ||
            cls == kClassWeapon || cls == kClassReset) {
            g_OutMotion[px] = clamp(baseMv, -65504.0f, 65504.0f);
            return;
        }
    }

    int2 qMax = max(int2(qRes.xy) - 1, int2(0, 0));
    int2 qp = clamp(px / 4, int2(0, 0), qMax);
    float depthTol = max(depth * 0.15f, 1e-5f);

    float4 cand[9];
    float2 refMv = float2(0.0f, 0.0f);
    float  bestConf = 0.0f;

    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            int idx = (dy + 1) * 3 + (dx + 1);
            int2 nqp = clamp(qp + int2(dx, dy), int2(0, 0), qMax);
            float4 nb = g_RefineFlow.Load(int3(nqp, 0));
            cand[idx] = nb;
            if (nb.z > 0.30f && all(isfinite(nb)) && abs(nb.w - depth) <= depthTol) {
                float score = nb.z * ((dx == 0 && dy == 0) ? 1.5f : 1.0f);
                if (score > bestConf) {
                    bestConf = score;
                    refMv = nb.xy;
                }
            }
        }
    }

    float2 sumMv = float2(0.0f, 0.0f);
    float  sumW  = 0.0f;
    int    agreeCount = 0;

    if (bestConf > 0.0f) {
        float dirTol = max(1.5f, 0.35f * length(refMv));
        [unroll]
        for (int i = 0; i < 9; ++i) {
            float4 nb = cand[i];
            if (nb.z > 0.30f && all(isfinite(nb)) && abs(nb.w - depth) <= depthTol &&
                length(nb.xy - refMv) <= dirTol) {
                float w = nb.z * ((i == 4) ? 2.0f : 1.0f);
                sumMv += nb.xy * w;
                sumW += w;
                ++agreeCount;
            }
        }
    }

    float2 outMv = baseMv;
    if (agreeCount >= 3 && sumW > 1.0f) {
        float2 resMv = sumMv / sumW;
        outMv = baseMv + resMv;
    }
    if (!all(isfinite(outMv))) {
        outMv = baseMv;
    }
    g_OutMotion[px] = clamp(outMv, -65504.0f, 65504.0f);
}
)";

} // namespace edpe
