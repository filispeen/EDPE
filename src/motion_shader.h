#pragma once

namespace edpe {

// Current-to-previous motion in render pixels, +Y downward, unjittered.
// Camera rows match CameraProjection; depth is reversed Z: depthB / viewZ.
// Compute shader [numthreads(8,8,1)] writes:
//   u0 OutMotion (R16G16_FLOAT): prev - cur in render pixels, +Y down, no jitter (fp16-safe)
//   u1 OutDepth  (R32_FLOAT):    canonical reversed-Z depth in world near-plane scale
//   u2 OutClass  (R8_UINT):      optional pixel class (when modeFlags bit 2 is set)
//   u3 OutReactive (R8_UNORM):   optional disocclusion/reactive mask (when modeFlags bit 9 is set)
inline constexpr char kMotionShader[] = R"(
#ifndef EDPE_MOTION_HAS_WEAPON
#define EDPE_MOTION_HAS_WEAPON 1
#endif
#ifndef EDPE_MOTION_HAS_CELESTIAL
#define EDPE_MOTION_HAS_CELESTIAL 1
#endif
#ifndef EDPE_MOTION_GPU_CAMERAS
#define EDPE_MOTION_GPU_CAMERAS 0
#endif

static const uint kClassSky           = 0u;
static const uint kClassWorld         = 1u;
static const uint kClassCockpitStatic = 2u;
static const uint kClassDisoccluded   = 3u;
static const uint kClassWeapon        = 4u;
static const uint kClassCelestial     = 5u;
static const uint kClassOffscreenFG   = 6u;
static const uint kClassReset         = 7u;

static const uint kWeaponStencilBit   = 0x10u;
static const uint kCockpitStencilBit  = 0x80u;
static const float kFp16Max           = 65504.0;

#if EDPE_MOTION_HAS_CELESTIAL
struct CelestialRecord {
    float4 relRow0; // view-space [R_rel_0 | t_rel_x] cur -> prev
    float4 relRow1; // view-space [R_rel_1 | t_rel_y]
    float4 relRow2; // view-space [R_rel_2 | t_rel_z]
    float4 box;     // screen AABB [minX, minY, maxX, maxY] (render px)
    float4 span;    // x=depthMin, y=depthMax, z=rMinPx, w=rMaxPx
    float4 center;  // xy=screen center (px), z=valid (>0.5), w=0
};
#endif

cbuffer Camera : register(b0) {
    float4 now0, now1, now2;
    float4 prev0, prev1, prev2; // prev0.x=translationStep, prev0.y=celestialDepthThresh, prev0.z=celestialCount
    float4 scales;     // current X/Y, previous X/Y
    float4 info;       // depthB, width, height, flags (bit0=hasPrevDepth, bit2=writeClass, bit3=reset, bit4=hasWeapon, bit5=hasCelestial, bit6=clampCockpit, bit8=hasCockpitTiles, bit9=writeReactive, bit10=inSrv)
    float4 jitterInfo; // jitterX, jitterY, prevJitterX, prevJitterY (render pixels)
};
Texture2D<float> sceneDepth : register(t0);
Texture2D<float> prevSceneDepth : register(t1);
Texture2D<float4> weaponMotion : register(t2);
Texture2D<uint2> sceneStencil : register(t3);
#if EDPE_MOTION_HAS_CELESTIAL
StructuredBuffer<CelestialRecord> celestialRecords : register(t4);
#endif

#ifdef EDPE_COCKPIT_TILE_PASS
RWTexture2D<float4> OutCockpitTiles : register(u0);
groupshared float3 gsCockpitAccum[256];

[numthreads(16, 16, 1)]
void cs_cockpit_tiles(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    float3 val = float3(0.0, 0.0, 0.0);
    if (tid.x < uint(info.y) && tid.y < uint(info.z)) {
        uint st = sceneStencil.Load(int3(tid.xy, 0)).g;
        if ((st & kCockpitStencilBit) != 0u) {
            float4 nwm = weaponMotion.Load(int3(tid.xy, 0));
            if (nwm.w > 0.5 && all(isfinite(nwm.xy)) && dot(nwm.xy, nwm.xy) <= 90000.0) {
                val = float3(nwm.xy, 1.0);
            }
        }
    }
    gsCockpitAccum[gi] = val;
    GroupMemoryBarrierWithGroupSync();
    if (gi < 128u) gsCockpitAccum[gi] += gsCockpitAccum[gi + 128u];
    GroupMemoryBarrierWithGroupSync();
    if (gi < 64u) gsCockpitAccum[gi] += gsCockpitAccum[gi + 64u];
    GroupMemoryBarrierWithGroupSync();
    if (gi < 32u) gsCockpitAccum[gi] += gsCockpitAccum[gi + 32u];
    GroupMemoryBarrierWithGroupSync();
    if (gi < 16u) gsCockpitAccum[gi] += gsCockpitAccum[gi + 16u];
    GroupMemoryBarrierWithGroupSync();
    if (gi < 8u) gsCockpitAccum[gi] += gsCockpitAccum[gi + 8u];
    GroupMemoryBarrierWithGroupSync();
    if (gi < 4u) gsCockpitAccum[gi] += gsCockpitAccum[gi + 4u];
    GroupMemoryBarrierWithGroupSync();
    if (gi < 2u) gsCockpitAccum[gi] += gsCockpitAccum[gi + 2u];
    GroupMemoryBarrierWithGroupSync();
    if (gi < 1u) gsCockpitAccum[gi] += gsCockpitAccum[gi + 1u];
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u) {
        float cnt = gsCockpitAccum[0].z;
        float2 avg = (cnt > 0.5) ? (gsCockpitAccum[0].xy / cnt) : float2(0.0, 0.0);
        OutCockpitTiles[gid.xy] = float4(avg, cnt, (cnt > 0.5) ? 1.0 : 0.0);
    }
}
#else
Texture2D<float4> cockpitTileTex : register(t5);
SamplerState pointSampler : register(s0);

RWTexture2D<float2> OutMotion : register(u0);
RWTexture2D<float> OutDepth : register(u1);
RWTexture2D<uint> OutClass : register(u2);
RWTexture2D<float> OutReactive : register(u3);
#if EDPE_MOTION_GPU_CAMERAS
cbuffer EliteNow : register(b1) { float4 currentWords[336]; };
cbuffer ElitePrevious : register(b2) { float4 previousWords[336]; };
#endif

struct MotionResult {
    float2 mv;
    float canonicalDepth;
    uint pixelClass;
};

float2 sanitizeMotion(float2 mv) {
    return all(isfinite(mv)) ? clamp(mv, -kFp16Max, kFp16Max) : float2(0.0, 0.0);
}

// Two-level history depth validation:
//   1. 2x2 GatherRed best-of-4 around (prevRasterPx - 0.5) with 1% relative tolerance.
//   2. Planar bilinear footprint when all 4 texels are in-bounds, finite, and > 0:
//      verifies planarity |d00 + d11 - d10 - d01| <= tol and |bilinear - expected| <= tol.
bool prevDepthMatches(float2 prevRasterPx, float expectedDepth, int2 maxCoord, out float outBestErr) {
    outBestErr = 1e30;
    if (!(expectedDepth > 0.0) || !isfinite(expectedDepth)) return false;
    float2 raster = prevRasterPx - 0.5;
    int2 base = int2(floor(raster));
    float tol = max(1e-6, 0.01 * expectedDepth);

    int2 clampedBase = clamp(base, int2(-1, -1), maxCoord);
    float2 gatherUv = (float2(clampedBase) + 1.0) * rcp(info.yz);
    float4 g = prevSceneDepth.GatherRed(pointSampler, gatherUv);
    float d00 = g.w;
    float d10 = g.z;
    float d01 = g.x;
    float d11 = g.y;

    float e00 = (d00 > 0.0 && d00 <= 1.0 && isfinite(d00)) ? abs(d00 - expectedDepth) : 1e30;
    float e10 = (d10 > 0.0 && d10 <= 1.0 && isfinite(d10)) ? abs(d10 - expectedDepth) : 1e30;
    float e01 = (d01 > 0.0 && d01 <= 1.0 && isfinite(d01)) ? abs(d01 - expectedDepth) : 1e30;
    float e11 = (d11 > 0.0 && d11 <= 1.0 && isfinite(d11)) ? abs(d11 - expectedDepth) : 1e30;
    float best = min(min(e00, e10), min(e01, e11));
    outBestErr = best;
    if (best <= tol) return true;

    if (any(base < 0) || any(base >= maxCoord)) return false;
    float4 quad = float4(d00, d10, d01, d11);
    if (!all(isfinite(quad)) || !all(quad > 0.0) || !all(quad <= 1.0)) return false;
    float2 f = raster - float2(base);
    float bilinear = lerp(lerp(d00, d10, f.x), lerp(d01, d11, f.x), f.y);
    float biErr = abs(bilinear - expectedDepth);
    if (biErr <= tol && abs(d00 + d11 - d10 - d01) <= tol) {
        outBestErr = min(outBestErr, biErr);
        return true;
    }
    return false;
}

#if EDPE_MOTION_HAS_WEAPON
bool sampleCockpitNeighborMotion(int2 pixCoord, int2 maxCoord, out float2 outMv) {
    outMv = float2(0.0, 0.0);
    if ((int(info.w + 0.1) & 256) != 0) {
        int2 tileMax = int2(maxCoord.x >> 4, maxCoord.y >> 4);
        int2 tileCoord = clamp(pixCoord >> 4, int2(0, 0), tileMax);
        float4 centerTile = cockpitTileTex.Load(int3(tileCoord, 0));
        if (centerTile.w > 0.5) {
            outMv = centerTile.xy;
            return true;
        }
        static const int kTileRadii[5] = {1, 2, 4, 8, 16};
        static const int2 kDirs[8] = {
            int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
            int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1)
        };
        [loop]
        for (int ri = 0; ri < 5; ++ri) {
            int r = kTileRadii[ri];
            float2 sum = float2(0.0, 0.0);
            float weightSum = 0.0;
            [unroll]
            for (int di = 0; di < 8; ++di) {
                int2 tc = clamp(tileCoord + kDirs[di] * r, int2(0, 0), tileMax);
                float4 t = cockpitTileTex.Load(int3(tc, 0));
                if (t.w > 0.5) {
                    sum += t.xy * t.z;
                    weightSum += t.z;
                }
            }
            if (weightSum > 0.5) {
                outMv = sum / weightSum;
                return true;
            }
        }
        return false;
    }

    static const int kRadii[5] = {8, 24, 64, 140, 260};
    static const int2 kDirs[8] = {
        int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
        int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1)
    };
    [loop]
    for (int ri = 0; ri < 5; ++ri) {
        int r = kRadii[ri];
        float2 sum = float2(0.0, 0.0);
        int count = 0;
        [unroll]
        for (int di = 0; di < 8; ++di) {
            int2 sc = clamp(pixCoord + kDirs[di] * r, int2(0, 0), maxCoord);
            uint st = sceneStencil.Load(int3(sc, 0)).g;
            if ((st & kCockpitStencilBit) == 0u) continue;
            float4 nwm = weaponMotion.Load(int3(sc, 0));
            if (nwm.w > 0.5 && all(isfinite(nwm.xy)) && dot(nwm.xy, nwm.xy) <= 90000.0) {
                sum += nwm.xy;
                ++count;
            }
        }
        if (count > 0) {
            outMv = sum / float(count);
            return true;
        }
    }
    return false;
}

bool tryWeaponOverride(int2 pixCoord, bool isReset, bool clampCockpit, out MotionResult outRes) {
    outRes.mv = float2(0.0, 0.0);
    outRes.canonicalDepth = 0.0;
    outRes.pixelClass = kClassWeapon;
    uint stencil = sceneStencil.Load(int3(pixCoord, 0)).g;
    uint masked = stencil & (kWeaponStencilBit | kCockpitStencilBit);
    if (clampCockpit ? (masked == 0u) : (masked != kWeaponStencilBit)) return false;
    float4 wm = weaponMotion.Load(int3(pixCoord, 0));
    if (wm.w <= 0.5 || !all(isfinite(wm.xyz))) return false;
    float mv2 = dot(wm.xy, wm.xy);
    // Reject impossible single-frame jumps (>300 px) from swapped or mismatched draws.
    if (mv2 > 90000.0) return false;
    if (clampCockpit && (stencil & kCockpitStencilBit) != 0u) {
        // For cockpit MVs > 6 px, verify consistency against nearby hull pixels.
        // Fast path: if any +-24px cockpit neighbor agrees within 25 px, accept immediately.
        // Slow path (isolated specks/slivers): compare against nearest cockpit hull.
        if (mv2 > 36.0) {
            int2 maxCoord = int2(info.y - 1.0, info.z - 1.0);
            static const int2 kProbe[4] = {int2(24, 0), int2(-24, 0), int2(0, 24), int2(0, -24)};
            bool localAgreed = false;
            [unroll]
            for (int i = 0; i < 4; ++i) {
                int2 sc = clamp(pixCoord + kProbe[i], int2(0, 0), maxCoord);
                if ((sceneStencil.Load(int3(sc, 0)).g & kCockpitStencilBit) == 0u) continue;
                float4 nwm = weaponMotion.Load(int3(sc, 0));
                if (nwm.w > 0.5 && all(isfinite(nwm.xy)) && dot(nwm.xy, nwm.xy) <= 90000.0) {
                    if (length(wm.xy - nwm.xy) <= 25.0) {
                        localAgreed = true;
                        break;
                    }
                }
            }
            if (!localAgreed) {
                float2 nAvg = float2(0.0, 0.0);
                if (sampleCockpitNeighborMotion(pixCoord, maxCoord, nAvg) &&
                    length(wm.xy - nAvg) > 25.0) {
                    return false;
                }
            }
        }
    } else {
        // On foot: heal isolated 1-2 px silhouette/depth-tie contour seams where an
        // overlapping mesh leaked a bogus MV against mutually consistent surrounding weapon MVs.
        int2 dims = int2(info.y, info.z);
        static const int2 kWeaponProbe[4] = {int2(6, 0), int2(-6, 0), int2(0, 6), int2(0, -6)};
        float2 nSamples[4];
        float2 nSum = float2(0.0, 0.0);
        int nCount = 0;
        [unroll]
        for (int i = 0; i < 4; ++i) {
            int2 sc = pixCoord + kWeaponProbe[i];
            if (sc.x < 0 || sc.x >= dims.x || sc.y < 0 || sc.y >= dims.y) continue;
            if ((sceneStencil.Load(int3(sc, 0)).g & (kWeaponStencilBit | kCockpitStencilBit)) != kWeaponStencilBit) continue;
            float4 nwm = weaponMotion.Load(int3(sc, 0));
            if (nwm.w > 0.5 && all(isfinite(nwm.xyz)) && dot(nwm.xy, nwm.xy) <= 90000.0) {
                nSamples[nCount] = nwm.xy;
                nSum += nwm.xy;
                nCount++;
            }
        }
        if (nCount >= 3) {
            float2 nAvg = nSum / float(nCount);
            float maxDev = 0.0;
            [unroll]
            for (int j = 0; j < 4; ++j) {
                if (j < nCount) {
                    maxDev = max(maxDev, length(nSamples[j] - nAvg));
                }
            }
            if (maxDev <= 8.0 && length(wm.xy - nAvg) > 12.0) {
                wm.xy = nAvg;
            }
        }
    }
    outRes.mv = wm.xy;
    outRes.canonicalDepth = wm.z;
    outRes.pixelClass = isReset ? kClassReset : kClassWeapon;
    return true;
}
#endif

#if EDPE_MOTION_HAS_CELESTIAL
bool tryCelestialOverride(float2 rawPixel, float depth, float3 view,
    float4 prevProjHalf, uint celestialCount, bool isReset, out MotionResult outRes) {
    outRes.mv = float2(0.0, 0.0);
    outRes.canonicalDepth = depth;
    outRes.pixelClass = kClassCelestial;
    uint count = min(celestialCount, 16u);
    [loop]
    for (uint i = 0u; i < count; ++i) {
        CelestialRecord rec = celestialRecords[i];
        if (rec.center.z <= 0.5) continue;
        if (rawPixel.x < rec.box.x || rawPixel.x > rec.box.z ||
            rawPixel.y < rec.box.y || rawPixel.y > rec.box.w) continue;
        if (depth < rec.span.x || depth > rec.span.y) continue;
        float r = length(rawPixel - rec.center.xy);
        if (r < rec.span.z || r > rec.span.w) continue;

        float3 prior = float3(dot(rec.relRow0.xyz, view) + rec.relRow0.w,
                              dot(rec.relRow1.xyz, view) + rec.relRow1.w,
                              dot(rec.relRow2.xyz, view) + rec.relRow2.w);
        if (prior.z <= 0.0) continue;
        float2 priorPixel = prior.xy * rcp(prior.z) * prevProjHalf.xy + prevProjHalf.zw;
        outRes.mv = priorPixel - rawPixel;
        outRes.canonicalDepth = depth;
        outRes.pixelClass = isReset ? kClassReset : kClassCelestial;
        return true;
    }
    return false;
}
#endif

)" R"(
MotionResult classifyAndFinishWorld(int2 pixCoord, float2 pixelCenter, float2 rawPixel,
    float2 priorPixel, float depth, float expectedPriorDepth, float z,
    bool hasPrevDepth, bool isReset, bool clampCockpit, bool hasWeapon, bool inSrv) {
    MotionResult res;
    res.canonicalDepth = depth;
    float2 worldMv = priorPixel - rawPixel;
    int2 maxCoord = int2(info.y - 1.0, info.z - 1.0);
    bool priorOffscreen = (priorPixel.x < 0.0 || priorPixel.x >= info.y ||
                           priorPixel.y < 0.0 || priorPixel.y >= info.z);

    bool worldMatches = false;
    float worldErr = 1e30;
    if (hasPrevDepth && !priorOffscreen) {
        worldMatches = prevDepthMatches(priorPixel + jitterInfo.zw, expectedPriorDepth, maxCoord, worldErr);
    }

    // Depth-validated cockpit & exterior ship hull (z < 150m) vs. world geometry disambiguation.
    // - Inside cockpit (z < 1.5m), static depth match always wins so vertical struts moving
    //   parallel to themselves do not accidentally pass worldMatches and inherit bogus world MVs.
    // - On exterior ship hull / nose (1.5m <= z < 150m), clamp when world reprojection fails
    //   or when static depth matches strictly tighter than world reprojection.
    bool canClampDistance = inSrv ? (z < 3.5) : ((z < 10.0) || (z < 150.0 && !priorOffscreen));
    if (clampCockpit && canClampDistance && hasPrevDepth) {
        // Un-stenciled exterior ship hull/wings/nacelles (e.g. Imperial Courier wings & nacelles):
        // validate against prevSceneDepth using the nearest captured cockpit WeaponMotion MV.
        // During Headlook + flight, worldMatches fails and staticMatches fails on thin nacelle edges,
        // whereas rawPixel + shipMv lands directly on the same hull depth in the previous frame.
#if EDPE_MOTION_HAS_WEAPON
        if (hasWeapon && (!worldMatches || z < 1.5 || worldErr > 1e-4 * depth)) {
            float2 shipMv = float2(0.0, 0.0);
            if (sampleCockpitNeighborMotion(pixCoord, maxCoord, shipMv)) {
                float2 shipPrevPx = rawPixel + shipMv + jitterInfo.zw;
                if (shipPrevPx.x >= 0.0 && shipPrevPx.x < info.y &&
                    shipPrevPx.y >= 0.0 && shipPrevPx.y < info.z) {
                    float shipErr = 1e30;
                    bool shipMatches = prevDepthMatches(shipPrevPx, depth, maxCoord, shipErr);
                    if (!shipMatches) {
                        int2 scShip = clamp(int2(floor(shipPrevPx)), 0, maxCoord);
                        float shipTol = max(1e-5, 0.025 * depth);
                        // Ring 1: 3x3 (-1..1)
                        [unroll]
                        for (int oy = -1; oy <= 1; ++oy) {
                            [unroll]
                            for (int ox = -1; ox <= 1; ++ox) {
                                float pd = prevSceneDepth.Load(int3(clamp(scShip + int2(ox, oy), 0, maxCoord), 0));
                                if (pd > 0.0 && pd <= 1.0 && isfinite(pd)) {
                                    shipErr = min(shipErr, abs(pd - depth));
                                }
                            }
                        }
                        // Ring 2: 8 perimeter taps at radius 2
                        if (shipErr > shipTol) {
                            static const int2 kRing2[8] = {
                                int2(-2,  0), int2(2,  0), int2(0, -2), int2(0,  2),
                                int2(-2, -2), int2(2, -2), int2(-2, 2), int2(2,  2)
                            };
                            [unroll]
                            for (int r2 = 0; r2 < 8; ++r2) {
                                float pd = prevSceneDepth.Load(int3(clamp(scShip + kRing2[r2], int2(0, 0), maxCoord), 0));
                                if (pd > 0.0 && pd <= 1.0 && isfinite(pd)) {
                                    shipErr = min(shipErr, abs(pd - depth));
                                }
                            }
                        }
                        // Ring 3: 4 cardinal taps at radius 3 (capped <= 3 px to prevent station wall oscillation)
                        if (shipErr > shipTol) {
                            static const int2 kOuter[4] = {
                                int2(3, 0), int2(-3, 0), int2(0, 3), int2(0, -3)
                            };
                            [unroll]
                            for (int oi = 0; oi < 4; ++oi) {
                                float pd = prevSceneDepth.Load(int3(clamp(scShip + kOuter[oi], int2(0, 0), maxCoord), 0));
                                if (pd > 0.0 && pd <= 1.0 && isfinite(pd)) {
                                    shipErr = min(shipErr, abs(pd - depth));
                                }
                            }
                        }
                        shipMatches = (shipErr <= shipTol);
                    }
                    bool preferShip = !worldMatches || (z < 1.5);
                    if (shipMatches && preferShip) {
                        res.mv = shipMv;
                        res.pixelClass = isReset ? kClassReset : kClassWeapon;
                        return res;
                    }
                }
            }
        }
#endif
        if (dot(worldMv, worldMv) > 0.0625) {
            float staticErr = 1e30;
            bool staticMatches = prevDepthMatches(rawPixel + jitterInfo.zw, depth, maxCoord, staticErr);
            if (!staticMatches) {
                int2 sc = clamp(int2(floor(rawPixel + jitterInfo.zw)), 0, maxCoord);
                float staticTol = max(1e-5, 0.0175 * depth);
                [unroll]
                for (int oy = -1; oy <= 1; ++oy) {
                    [unroll]
                    for (int ox = -1; ox <= 1; ++ox) {
                        float pd = prevSceneDepth.Load(int3(clamp(sc + int2(ox, oy), 0, maxCoord), 0));
                        if (pd > 0.0 && pd <= 1.0 && isfinite(pd)) {
                            staticErr = min(staticErr, abs(pd - depth));
                        }
                    }
                }
                // Inside close cockpit (z < 1.5m), also check +-2px cardinal neighbors to survive
                // 1-2px cockpit jolt / head-sway at elevator start/stop without leaking world MVs.
                if (z < 1.5 && staticErr > staticTol) {
                    float dL = prevSceneDepth.Load(int3(clamp(sc + int2(-2, 0), 0, maxCoord), 0));
                    float dR = prevSceneDepth.Load(int3(clamp(sc + int2(2, 0), 0, maxCoord), 0));
                    float dU = prevSceneDepth.Load(int3(clamp(sc + int2(0, -2), 0, maxCoord), 0));
                    float dD = prevSceneDepth.Load(int3(clamp(sc + int2(0, 2), 0, maxCoord), 0));
                    if (dL > 0.0 && dL <= 1.0 && isfinite(dL)) staticErr = min(staticErr, abs(dL - depth));
                    if (dR > 0.0 && dR <= 1.0 && isfinite(dR)) staticErr = min(staticErr, abs(dR - depth));
                    if (dU > 0.0 && dU <= 1.0 && isfinite(dU)) staticErr = min(staticErr, abs(dU - depth));
                    if (dD > 0.0 && dD <= 1.0 && isfinite(dD)) staticErr = min(staticErr, abs(dD - depth));
                }
                staticMatches = (staticErr <= staticTol);
            }
            // Use depth-relative tie-breaker so exterior ship nose/hull (depth ~ 0.002-0.01) is not
            // penalized by a fixed 1e-6 offset while flat world planes (worldErr == 0) stay World.
            bool preferStatic = !worldMatches || (z < 1.5);
            if (staticMatches && preferStatic) {
                res.mv = float2(0.0, 0.0);
                res.pixelClass = isReset ? kClassReset : kClassCockpitStatic;
                return res;
            }
        }
    }

    // Preserve real physical off-screen motion vectors for newly-revealed edges (FG policy).
    res.mv = worldMv;
    if (isReset) {
        res.pixelClass = kClassReset;
    } else if (priorOffscreen) {
        res.pixelClass = kClassOffscreenFG;
    } else if (hasPrevDepth && !worldMatches) {
        res.pixelClass = kClassDisoccluded;
    } else {
        res.pixelClass = kClassWorld;
    }
    return res;
}

#if EDPE_MOTION_GPU_CAMERAS
MotionResult reproject(float2 pixelCenter, int2 pixCoord,
    float4 current0, float4 current1, float4 current2,
    float4 prior0, float4 prior1, float4 prior2,
    float4 projection, float depthB, bool clampCockpit) {
    MotionResult res;
    res.mv = float2(0.0, 0.0);
    res.canonicalDepth = 0.0;
    res.pixelClass = kClassSky;

    if (info.y <= 0 || info.z <= 0 || projection.x <= 0 || projection.y <= 0) return res;
    float depth = sceneDepth.Load(int3(pixCoord, 0));
    res.canonicalDepth = depth;
    int modeFlags = int(info.w + 0.1);
    bool hasPrevDepth = (modeFlags & 1) != 0;
    bool isReset = (modeFlags & 8) != 0;
    bool hasWeapon = (modeFlags & 16) != 0;

#if EDPE_MOTION_HAS_WEAPON
    if (hasWeapon) {
        MotionResult wmRes;
        if (tryWeaponOverride(pixCoord, isReset, clampCockpit, wmRes)) return wmRes;
    }
#endif

    float2 rawPixel = pixelCenter - jitterInfo.xy;

    // 1. Skybox, distant stars, nebula, celestial background, and star bloom:
    float celestialDepthThresh = (depthB > 0.0) ? (depthB / 50000.0) : 1e-6;
    if (depth <= 0.0 || depth > 1.0 || depth < celestialDepthThresh) {
        float2 ndc = float2(2.0 * rawPixel.x / info.y - 1.0, 1.0 - 2.0 * rawPixel.y / info.z);
        float3 viewRay = float3(ndc.x / projection.x, ndc.y / projection.y, 1.0);
        float3 worldRay = float3(dot(current0.xyz, viewRay),
                                 dot(current1.xyz, viewRay),
                                 dot(current2.xyz, viewRay));
        float3 priorRay = float3(prior0.x * worldRay.x + prior1.x * worldRay.y + prior2.x * worldRay.z,
                                 prior0.y * worldRay.x + prior1.y * worldRay.y + prior2.y * worldRay.z,
                                 prior0.z * worldRay.x + prior1.z * worldRay.y + prior2.z * worldRay.z);
        res.pixelClass = isReset ? kClassReset : kClassSky;
        if (priorRay.z <= 0.0) return res;
        float2 priorNdc = projection.zw * priorRay.xy / priorRay.z;
        float2 priorPixel = float2((priorNdc.x + 1.0) * info.y * 0.5,
                                   (1.0 - priorNdc.y) * info.z * 0.5);
        res.mv = priorPixel - rawPixel;
        return res;
    }

    float z = depthB / depth;

    // 2. Static/world geometry camera-depth reprojection from the unjittered pixel coordinate:
    float2 ndc = float2(2.0 * rawPixel.x / info.y - 1.0, 1.0 - 2.0 * rawPixel.y / info.z);
    float3 view = float3(ndc.x * z / projection.x, ndc.y * z / projection.y, z);
    float3 world = float3(dot(current0.xyz, view) + current0.w,
                          dot(current1.xyz, view) + current1.w,
                          dot(current2.xyz, view) + current2.w);
    world -= float3(prior0.w, prior1.w, prior2.w);
    float3 prior = float3(prior0.x * world.x + prior1.x * world.y + prior2.x * world.z,
                          prior0.y * world.x + prior1.y * world.y + prior2.y * world.z,
                          prior0.z * world.x + prior1.z * world.y + prior2.z * world.z);
    res.pixelClass = isReset ? kClassReset : kClassWorld;
    if (prior.z <= 0.0) {
        if (clampCockpit && z < 10.0 && hasPrevDepth) {
            float se = 1e30;
            int2 mc = int2(info.y - 1.0, info.z - 1.0);
            if (prevDepthMatches(rawPixel + jitterInfo.zw, depth, mc, se)) {
                res.pixelClass = isReset ? kClassReset : kClassCockpitStatic;
            }
        }
        return res;
    }
    float2 priorNdc = projection.zw * prior.xy / prior.z;
    float2 priorPixel = float2((priorNdc.x + 1.0) * info.y * 0.5,
                               (1.0 - priorNdc.y) * info.z * 0.5);
    float expectedPriorDepth = depthB / prior.z;
    return classifyAndFinishWorld(pixCoord, pixelCenter, rawPixel, priorPixel,
        depth, expectedPriorDepth, z, hasPrevDepth, isReset, clampCockpit, hasWeapon, false);
}
#endif

)" R"(
MotionResult reprojectRel(float2 pixelCenter, int2 pixCoord,
    float4 rel0, float4 rel1, float4 rel2,
    float celestialDepthThresh, uint celestialCount,
    float4 invProj, float4 prevProjHalf, float depthB, bool clampCockpit) {
    MotionResult res;
    res.mv = float2(0.0, 0.0);
    res.canonicalDepth = 0.0;
    res.pixelClass = kClassSky;

    if (info.y <= 0 || info.z <= 0 || scales.x <= 0 || scales.y <= 0) return res;
    float depth = sceneDepth.Load(int3(pixCoord, 0));
    res.canonicalDepth = depth;
    int modeFlags = int(info.w + 0.1);
    bool hasPrevDepth = (modeFlags & 1) != 0;
    bool isReset = (modeFlags & 8) != 0;
    bool hasWeapon = (modeFlags & 16) != 0;
    bool hasCelestial = (modeFlags & 32) != 0;
    bool hasSceneStencil = (modeFlags & 128) != 0;
    bool inSrv = (modeFlags & 1024) != 0;

    // 0. First-person weapon / viewmodel override (stencil 0x10 + valid WeaponMotion):
#if EDPE_MOTION_HAS_WEAPON
    if (hasWeapon) {
        MotionResult wmRes;
        if (tryWeaponOverride(pixCoord, isReset, clampCockpit, wmRes)) return wmRes;
    }
#endif

    // 0b. Exact Cobra Engine stencil masks:
    //   - In ship/SRV (clampCockpit): bit 0x80 covers cockpit, canopy, dashboard, wings, hull.
    //     During normal flight/elevator, static depth matches at rawPixel -> mv = (0, 0).
    //     During Headlook (free look inside cockpit), the cockpit rotates across the screen
    //     with zero ship-relative translation -> pure-rotation reprojection (rotMv) matches
    //     prevSceneDepth cleanly even while the ship is flying at high speed.
    //     During external Camera Suite translation, falls back to worldMv if worldMatches.
    //   - On foot (!clampCockpit): bit 0x10/0x80 or zView < 0.60m fallback when WeaponMotion
    //     did not cover the pixel.
    if (hasSceneStencil && depth > 0.0 && depth <= 1.0) {
        uint st = sceneStencil.Load(int3(pixCoord, 0)).g;
        if (clampCockpit && (st & kCockpitStencilBit) != 0u) {
            float2 fillMv = float2(0.0, 0.0);
#if EDPE_MOTION_HAS_WEAPON
            int2 maxCoord = int2(info.y - 1.0, info.z - 1.0);
            bool filled = hasWeapon && sampleCockpitNeighborMotion(pixCoord, maxCoord, fillMv);
#else
            bool filled = false;
#endif
            res.mv = fillMv;
            res.canonicalDepth = depth;
            res.pixelClass = isReset ? kClassReset : (filled ? kClassWeapon : kClassCockpitStatic);
            return res;
        }
        if (!clampCockpit) {
            // Cover both stencil-tagged weapon/arms (0x10 with 0x80 clear) and near-camera
            // un-stenciled weapon parts (muzzle tip / scope rim / glass at z < 0.60m),
            // while excluding 0x80 parked SRV/ship pixels even when approached closely.
            float zView = (depthB > 0.0) ? (depthB / depth) : 1e30;
            bool isOnFootWeaponStencil = ((st & (kWeaponStencilBit | kCockpitStencilBit)) == kWeaponStencilBit);
            bool isNearUnstenciledWeapon = ((st & kCockpitStencilBit) == 0u) && (zView < 0.60);
            if (isOnFootWeaponStencil || isNearUnstenciledWeapon) {
                res.mv = float2(0.0, 0.0);
                res.canonicalDepth = depth;
                res.pixelClass = isReset ? kClassReset : kClassWeapon;
                return res;
            }
        }
    }

    float2 rawPixel = pixelCenter - jitterInfo.xy;
    float2 ndc = rawPixel * invProj.xy + float2(-1.0, 1.0);

    // 1. Skybox, distant stars, nebula, celestial background, and star bloom:
    if (depth <= 0.0 || depth > 1.0 || depth < celestialDepthThresh) {
        float3 viewRay = float3(ndc * invProj.zw, 1.0);
        float3 priorRay = float3(dot(rel0.xyz, viewRay),
                                 dot(rel1.xyz, viewRay),
                                 dot(rel2.xyz, viewRay));
        res.pixelClass = isReset ? kClassReset : kClassSky;
        if (priorRay.z <= 0.0) return res;
        float2 priorPixel = priorRay.xy * rcp(priorRay.z) * prevProjHalf.xy + prevProjHalf.zw;
        res.mv = priorPixel - rawPixel;
        return res;
    }

    float z = depthB / depth;
    float3 view = float3(ndc * invProj.zw * z, z);

#if EDPE_MOTION_HAS_CELESTIAL
    // 2. Supercruise planetary body motion override (CelestialRecords):
    if (hasCelestial && celestialCount > 0u) {
        MotionResult celRes;
        if (tryCelestialOverride(rawPixel, depth, view, prevProjHalf, celestialCount, isReset, celRes)) {
            return celRes;
        }
    }
#endif

    // 3. Static/world geometry camera-depth reprojection via CPU-composed relative transform:
    float3 prior = float3(dot(rel0.xyz, view) + rel0.w,
                          dot(rel1.xyz, view) + rel1.w,
                          dot(rel2.xyz, view) + rel2.w);
    res.pixelClass = isReset ? kClassReset : kClassWorld;
    if (prior.z <= 0.0) {
        if (clampCockpit && z < (inSrv ? 3.5 : 10.0) && hasPrevDepth) {
            float se = 1e30;
            int2 mc = int2(info.y - 1.0, info.z - 1.0);
            if (prevDepthMatches(rawPixel + jitterInfo.zw, depth, mc, se)) {
                res.pixelClass = isReset ? kClassReset : kClassCockpitStatic;
            }
        }
        return res;
    }
    float2 priorPixel = prior.xy * rcp(prior.z) * prevProjHalf.xy + prevProjHalf.zw;
    float expectedPriorDepth = depthB / prior.z;
    return classifyAndFinishWorld(pixCoord, pixelCenter, rawPixel, priorPixel,
        depth, expectedPriorDepth, z, hasPrevDepth, isReset, clampCockpit, hasWeapon, inSrv);
}

void writeOutputs(int2 pixCoord, MotionResult res) {
    OutMotion[pixCoord] = sanitizeMotion(res.mv);
    OutDepth[pixCoord] = isfinite(res.canonicalDepth) ? res.canonicalDepth : 0.0;
    int modeFlags = int(info.w + 0.1);
    if ((modeFlags & 4) != 0) {
        OutClass[pixCoord] = res.pixelClass;
    }
    if ((modeFlags & 512) != 0) {
        float reactive = (res.pixelClass == kClassDisoccluded ||
                          res.pixelClass == kClassOffscreenFG ||
                          res.pixelClass == kClassReset) ? 1.0 : 0.0;
        OutReactive[pixCoord] = reactive;
    }
}

[numthreads(8, 8, 1)]
void cs(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= uint(info.y) || tid.y >= uint(info.z)) return;
    int2 pixCoord = int2(tid.xy);
    float2 pixelCenter = float2(tid.xy) + 0.5;
    uint celestialCount = uint(prev0.z + 0.1);
    bool clampCockpit = (int(info.w + 0.1) & 64) != 0;
    MotionResult res = reprojectRel(pixelCenter, pixCoord,
        now0, now1, now2, prev0.y, celestialCount, prev1, prev2, info.x, clampCockpit);
    writeOutputs(pixCoord, res);
}

#if EDPE_MOTION_GPU_CAMERAS
[numthreads(8, 8, 1)]
void cs_gpu(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= uint(info.y) || tid.y >= uint(info.z)) return;
    int2 pixCoord = int2(tid.xy);
    float2 pixelCenter = float2(tid.xy) + 0.5;

    MotionResult res;
    res.mv = float2(0.0, 0.0);
    res.canonicalDepth = sceneDepth.Load(int3(pixCoord, 0));
    res.pixelClass = kClassSky;

    float3 currentW = float3(currentWords[270].w, currentWords[271].w, currentWords[272].w);
    float3 previousW = float3(previousWords[270].w, previousWords[271].w, previousWords[272].w);
    float curWW = max(dot(currentW, currentW), 1e-12);
    float prevWW = max(dot(previousW, previousW), 1e-12);

    float3 rawCurrentX = float3(currentWords[270].x, currentWords[271].x, currentWords[272].x);
    float3 rawCurrentY = float3(currentWords[270].y, currentWords[271].y, currentWords[272].y);
    float3 rawPreviousX = float3(previousWords[270].x, previousWords[271].x, previousWords[272].x);
    float3 rawPreviousY = float3(previousWords[270].y, previousWords[271].y, previousWords[272].y);

    // Strip any subpixel NDC offset folded into the GPU projection columns along W.
    float3 currentX = rawCurrentX - currentW * (dot(rawCurrentX, currentW) / curWW);
    float3 currentY = rawCurrentY - currentW * (dot(rawCurrentY, currentW) / curWW);
    float3 previousX = rawPreviousX - previousW * (dot(rawPreviousX, previousW) / prevWW);
    float3 previousY = rawPreviousY - previousW * (dot(rawPreviousY, previousW) / prevWW);

    float4 projection = float4(length(currentX), length(currentY),
        length(previousX), length(previousY));
    float depthB = currentWords[273].z;
    if (any(projection <= 0) || depthB <= 0 || depthB > 1) {
        writeOutputs(pixCoord, res);
        return;
    }
    float3 curOrig = currentWords[275].xyz;
    float3 prevOrig = previousWords[275].xyz;
    bool useCurOrig = all(isfinite(curOrig)) && any(abs(curOrig) > 1e-6);
    bool usePrevOrig = all(isfinite(prevOrig)) && any(abs(prevOrig) > 1e-6);
    float3 curPos = useCurOrig ? curOrig : float3(currentWords[233].w, currentWords[234].w, currentWords[235].w);
    float3 prevPos = usePrevOrig ? prevOrig : float3(previousWords[233].w, previousWords[234].w, previousWords[235].w);
    float4 current0 = float4(currentX.x / projection.x,
        currentY.x / projection.y, currentW.x, curPos.x);
    float4 current1 = float4(currentX.y / projection.x,
        currentY.y / projection.y, currentW.y, curPos.y);
    float4 current2 = float4(currentX.z / projection.x,
        currentY.z / projection.y, currentW.z, curPos.z);
    float4 prior0 = float4(previousX.x / projection.z,
        previousY.x / projection.w, previousW.x, prevPos.x);
    float4 prior1 = float4(previousX.y / projection.z,
        previousY.y / projection.w, previousW.y, prevPos.y);
    float4 prior2 = float4(previousX.z / projection.z,
        previousY.z / projection.w, previousW.z, prevPos.z);
    res = reproject(pixelCenter, pixCoord, current0, current1, current2,
        prior0, prior1, prior2, projection, depthB, false);
    writeOutputs(pixCoord, res);
}
#endif
#endif
)";

} // namespace edpe
