#pragma once

namespace edpe {

inline constexpr char kUiCompositeShader[] = R"hlsl(
Texture2D<float4> tDlss : register(t0);
Texture2D<float4> tGame : register(t1);
Texture2D<float4> tLowWorld : register(t2);
Texture2D<float4> tHudMask : register(t3);
Texture2D<float4> tHudTonemapped : register(t4);
SamplerState sLinear : register(s0);

cbuffer CompositeParams : register(b0) {
    float2 renderSize;
    float2 renderTexel;
    float2 outputSize;
    float2 outputTexel;
    float4 flags; // x: isHdr (1.0 = HDR, 0.0 = LDR), y: isNativeRes, z: preserveUi, w: isNativeHud (1.0/2.0 = raw float HUD, 3.0/4.0 = float HUD + Pass 52 tonemapped HUD at t4)
};

float3 tonemapLinear(float3 c) {
    float3 x = max(c, 0.0);
    float a = 2.51;
    float b = 0.03;
    float c_coeff = 2.43;
    float d = 0.59;
    float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c_coeff * x + d) + e));
}

float3 toLdr(float3 c) {
    return pow(tonemapLinear(c), 1.0 / 2.2);
}

float max3(float3 v) {
    return max(v.r, max(v.g, v.b));
}

float3 sampleLowWorldLdrAtOutputPos(int2 outPos);

// Safely inspect a corner pixel where tHudMask has zero HUD coverage (RGB == 0, A == 1)
// to determine whether tHudTonemapped was tonemapped from the merged scene+HUD HDR
// (H_scene * T + L_hud, where the HUD-free corner matches tLowWorld) or from HUD-only
// over black (returning the 3D LUT black floor at HDR = 0).
float3 sampleHudTonemapBlackFloor(bool isNativeSizedHud, out bool tonemapHasScene) {
    tonemapHasScene = true;
    int2 dim = (int2)(isNativeSizedHud ? outputSize : renderSize);
    int maxX = max(dim.x - 1, 0);
    int maxY = max(dim.y - 1, 0);
    int2 outMax = max((int2)outputSize - int2(1, 1), int2(0, 0));
    int3 corners[4] = {
        int3(0, 0, 0),
        int3(maxX, 0, 0),
        int3(0, maxY, 0),
        int3(maxX, maxY, 0)
    };
    int2 outCorners[4] = {
        int2(0, 0),
        int2(outMax.x, 0),
        int2(0, outMax.y),
        int2(outMax.x, outMax.y)
    };
    [unroll]
    for (int i = 0; i < 4; ++i) {
        float4 m = tHudMask.Load(corners[i]);
        if (all(m.rgb <= 1e-6) && m.a >= 0.999) {
            float3 cornerTone = tHudTonemapped.Load(corners[i]).rgb;
            float3 cornerLow = sampleLowWorldLdrAtOutputPos(outCorners[i]);
            if (max3(cornerTone) <= 0.04 && max3(cornerLow - cornerTone) > 0.05) {
                tonemapHasScene = false;
                return cornerTone;
            }
            tonemapHasScene = true;
            return float3(0.0, 0.0, 0.0);
        }
    }
    return float3(0.0, 0.0, 0.0);
}

float3 sampleGameLdr(int2 pos) {
    int2 maxOut = max((int2)outputSize - int2(1, 1), int2(0, 0));
    int2 p = clamp(pos, int2(0, 0), maxOut);
    float3 c = tGame.Load(int3(p, 0)).rgb;
    return (flags.x > 0.5) ? toLdr(c) : c;
}

float3 sampleLowWorldLdrAtOutputPos(int2 outPos) {
    if (flags.y > 0.5) {
        int2 maxOut = max((int2)outputSize - int2(1, 1), int2(0, 0));
        int2 p = clamp(outPos, int2(0, 0), maxOut);
        float3 c = tLowWorld.Load(int3(p, 0)).rgb;
        return (flags.x > 0.5) ? toLdr(c) : c;
    } else {
        float2 uv = (float2(outPos) + float2(0.5, 0.5)) * outputTexel;
        float3 c = tLowWorld.SampleLevel(sLinear, uv, 0).rgb;
        return (flags.x > 0.5) ? toLdr(c) : c;
    }
}

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput vs_main(uint id : SV_VertexID) {
    VSOutput vout;
    vout.uv = float2((id << 1) & 2, id & 2);
    vout.pos = float4(vout.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return vout;
}

float4 ps_main(VSOutput pin) : SV_Target {
    int2 outPos = int2(pin.pos.xy);
    int3 pixelPos = int3(outPos, 0);

    float3 cDlss = tDlss.Load(pixelPos).rgb;
    if (flags.z < 0.5) {
        if (flags.x > 0.5) cDlss = toLdr(cDlss);
        return float4(saturate(cDlss), 1.0);
    }

    // Step 1: Reconstructed 3D world (which already carries the native Pass 36-45 HUD Bloom glow halo)
    // + separated Pass 35 cockpit HUD layer (tHudMask: RGB = linear HUD radiance, A = transmittance T;
    // tHudTonemapped: exact Pass 52 game-tonemapped, auto-exposed, 3D-LUT-graded HUD output).
    float3 cScene;
    float hudMask = 0.0;
    bool hasHud = false;
    float pass54Transmittance = 1.0;
    if (flags.w > 0.5) {
        bool isNativeSizedHud = (flags.w < 1.5) || (flags.w > 2.5 && flags.w < 3.5);
        bool hasTonemappedHud = (flags.w > 2.5);
        float4 hSample = isNativeSizedHud
            ? tHudMask.Load(pixelPos)
            : tHudMask.SampleLevel(sLinear, pin.uv, 0);
        float hudTransmittance = saturate(hSample.a);
        hasHud = any(hSample.rgb > 1e-5) || (hudTransmittance < 0.999);
        if (!hasHud) {
            cScene = (flags.x > 0.5) ? toLdr(cDlss) : saturate(cDlss);
        } else if (flags.x > 0.5) {
            float3 combinedHdr = cDlss * hudTransmittance + hSample.rgb;
            cScene = toLdr(combinedHdr);
        } else if (hasTonemappedHud) {
            float4 hudPostSample = isNativeSizedHud
                ? tHudTonemapped.Load(pixelPos)
                : tHudTonemapped.SampleLevel(sLinear, pin.uv, 0);
            pass54Transmittance = saturate(hudPostSample.a);
            float3 hudPost = hudPostSample.rgb;
            bool tonemapHasScene = true;
            float3 hudBlack = sampleHudTonemapBlackFloor(isNativeSizedHud, tonemapHasScene);
            float3 dlssLin = pow(max(cDlss, 0.0), 2.2);
            if (tonemapHasScene) {
                // tHudTonemapped holds exact Pass 52 Tonemap(H_scene * T + L_hud, Bloom) at native resolution.
                // Replace the pre-reconstruction background term (lowLin * T) with the DLSS/DLAA-reconstructed
                // background term (dlssLin * T) in linear light so translucent HUD panels, orange headers,
                // and 3D holograms preserve 1:1 stock tonemap/bloom/LUT brightness without 3D edge jitter.
                float3 cLow = sampleLowWorldLdrAtOutputPos(outPos);
                float3 lowLin = pow(max(cLow, 0.0), 2.2);
                float3 postLin = pow(max(hudPost, 0.0), 2.2);
                float3 bgLowLin = min(lowLin * hudTransmittance, postLin);
                float3 bgScale = bgLowLin / max(lowLin, float3(1e-4, 1e-4, 1e-4));
                float3 bgDlssLin = (lowLin > float3(1e-4, 1e-4, 1e-4))
                    ? (dlssLin * bgScale)
                    : (dlssLin * hudTransmittance);
                float3 hudEmitLin = max(postLin - bgLowLin, 0.0);
                float3 combinedLin = bgDlssLin + hudEmitLin;
                cScene = pow(saturate(combinedLin), 1.0 / 2.2);
            } else {
                float3 hudToned = max(hudPost - hudBlack, 0.0);
                float3 hudLin = pow(hudToned, 2.2);
                float3 bgLin = dlssLin * hudTransmittance;
                float3 combinedLin = bgLin + hudLin - bgLin * hudLin;
                cScene = pow(saturate(combinedLin), 1.0 / 2.2);
            }
        } else {
            float3 dlssLin = pow(max(cDlss, 0.0), 2.2);
            float3 hudLin = tonemapLinear(hSample.rgb * 120.0);
            float3 bgLin = dlssLin * hudTransmittance;
            float3 combinedLin = bgLin + hudLin - bgLin * hudLin;
            cScene = pow(saturate(combinedLin), 1.0 / 2.2);
        }
        // Do not early-return on hasHud pixels: Pass 54 post-tonemap 2D menus (Starport Services,
        // ESC Pause Menu, modal dialogs, mouse cursor) are drawn into tGame after Pass 52 Tonemapping
        // and must composite ON TOP OF cScene (which already holds the reconstructed 3D world +
        // Pass 35 Cockpit HUD). Where Pass 54 drew nothing (cGame == cLow),
        // Step 2 produces darkWeight == 0 and uiMask == 0, preserving cScene 100% untouched.
    } else {
        cScene = (flags.x > 0.5) ? toLdr(cDlss) : cDlss;
        float hudRaw = (flags.y > 0.5)
            ? tHudMask.Load(pixelPos).r
            : tHudMask.SampleLevel(sLinear, pin.uv, 0).r;
        hudMask = smoothstep(0.005, 0.08, hudRaw);
    }

    // Step 2: Post-tonemap Pass 53/54 2D UI overlay extraction (Main Menu, ESC Pause Menu,
    // Station Services 2D screens, mouse cursor) by decomposing post-UI backbuffer (tGame)
    // vs pre-UI tonemapped world (tLowWorld) into:
    //  (a) Multiplicative darkening transmittance T_dark = cGame / cLow (for the Main Menu and
    //      ESC Menu soft dark vignette / drop-shadow washes so the underlying 3D scene cScene
    //      stays 100% upscaled and anti-aliased with zero subpixel jitter leakage!), and
    //  (b) Crisp emissive / chromatic / opaque 2D UI elements (logos, buttons, text, portraits,
    //      news cards, mouse cursor).
    float3 cGame = sampleGameLdr(outPos);
    float3 cLow = sampleLowWorldLdrAtOutputPos(outPos);
    if (flags.w > 0.5 && pass54Transmittance > 0.995 && max3(abs(cGame - cLow)) <= 0.015) {
        return float4(saturate(cScene), 1.0);
    }

    // 4-neighbor cross at +/-2 output pixels to measure local 2D UI gradients vs 3D world gradients
    // and detect dark glyphs (e.g. black "CONTINUE" letters) inside bright emissive buttons.
    float3 gL = sampleGameLdr(outPos + int2(-2, 0));
    float3 gR = sampleGameLdr(outPos + int2( 2, 0));
    float3 gU = sampleGameLdr(outPos + int2(0, -2));
    float3 gD = sampleGameLdr(outPos + int2(0,  2));

    float3 lL = sampleLowWorldLdrAtOutputPos(outPos + int2(-2, 0));
    float3 lR = sampleLowWorldLdrAtOutputPos(outPos + int2( 2, 0));
    float3 lU = sampleLowWorldLdrAtOutputPos(outPos + int2(0, -2));
    float3 lD = sampleLowWorldLdrAtOutputPos(outPos + int2(0,  2));

    float3 wMin = min(cLow, min(min(lL, lR), min(lU, lD)));
    float3 wMax = max(cLow, max(max(lL, lR), max(lU, lD)));
    if (flags.y < 0.5) {
        float2 lowPos = pin.uv * renderSize - float2(0.5, 0.5);
        int2 lowBase = (int2)floor(lowPos);
        int maxW = (int)renderSize.x - 1;
        int maxH = (int)renderSize.y - 1;
        int2 p00 = clamp(lowBase, int2(0, 0), int2(maxW, maxH));
        int2 p10 = clamp(lowBase + int2(1, 0), int2(0, 0), int2(maxW, maxH));
        int2 p01 = clamp(lowBase + int2(0, 1), int2(0, 0), int2(maxW, maxH));
        int2 p11 = clamp(lowBase + int2(1, 1), int2(0, 0), int2(maxW, maxH));

        float3 w00 = tLowWorld.Load(int3(p00, 0)).rgb;
        float3 w10 = tLowWorld.Load(int3(p10, 0)).rgb;
        float3 w01 = tLowWorld.Load(int3(p01, 0)).rgb;
        float3 w11 = tLowWorld.Load(int3(p11, 0)).rgb;
        if (flags.x > 0.5) {
            w00 = toLdr(w00);
            w10 = toLdr(w10);
            w01 = toLdr(w01);
            w11 = toLdr(w11);
        }
        wMin = min(wMin, min(min(w00, w10), min(w01, w11)));
        wMax = max(wMax, max(max(w00, w10), max(w01, w11)));
    }
    float edgeRange = max3(wMax - wMin);
    float gameFlatH = min(max3(abs(cGame - gL)), max3(abs(cGame - gR)));
    float gameFlatV = min(max3(abs(cGame - gU)), max3(abs(cGame - gD)));
    float gameRange = max(gameFlatH, gameFlatV);
    float edgeTol = min(edgeRange, gameRange);

    // (a) Multiplicative vignette / shadow darkening applied to reconstructed cScene ONLY
    // where tGame preserves the 3D spatial gradient (vigPreserve). On flat opaque/semi-opaque
    // UI cards (Main Menu Options / Continue cards, Starport Services cards) where gameRange is
    // flat while the 3D hangar has girders/lights or subpixel jitter, vigPreserve = 0 prevents
    // (cScene - cLow) subpixel jitter from ever bleeding through the card.
    float negDiff = max(0.0, max3(cLow - cGame) - 0.15 * edgeTol);
    float darkWeight = smoothstep(0.010, 0.030, negDiff);
    float3 tDark = clamp(cGame / max(cLow, float3(0.004, 0.004, 0.004)), 0.0, 1.0);
    float kVig = clamp(max3(cGame) / max(max3(cLow), 0.01), 0.0, 1.0);
    float worldVar = max(edgeRange, max3(abs(cScene - cLow)));
    float expectedGameRange = kVig * worldVar;
    float vigPreserve = (worldVar > 0.008)
        ? saturate((gameRange - 0.012) / max(expectedGameRange, 0.012))
        : 0.0;
    float3 cDarkened = clamp(cGame + (tDark * vigPreserve) * (cScene - cLow), 0.0, 1.0);
    float3 cBase = lerp(cScene, cDarkened, darkWeight);

    // (b) Detect genuine emissive, chromatic, or opaque 2D UI elements (excluding neutral vignette darkening):
    // 1. Positive brightening above cLow / wMax
    float posCenter = max(0.0, max3(cGame - cLow) - 0.15 * edgeTol);
    float posBox = max(0.0, max3(cGame - wMax));
    float posNeigh = max(
        max(max3(gL - lL), max3(gR - lR)),
        max(max3(gU - lU), max3(gD - lD))
    );
    float posUi = max(max(posCenter, posBox * 2.0), 0.65 * max(0.0, posNeigh - 0.15 * edgeTol));

    // 2. Chromaticity shift (color change orthogonal to neutral scalar darkening cGame = k * cLow)
    float kScale = clamp(dot(cGame, cLow) / max(dot(cLow, cLow), 1e-4), 0.0, 1.5);
    float chromaDiff = max(0.0, max3(abs(cGame - cLow * kScale)) - 0.15 * edgeTol);

    // 3. Sharp 2D UI edges in cGame that do not exist in cLow, OR opaque/flat UI cards
    //    that flatten underlying 3D hangar edges or subpixel jitter in cLow/cScene
    float gradGame = max(max3(abs(gR - gL)), max3(abs(gD - gU)));
    float gradLow  = max(max3(abs(lR - lL)), max3(abs(lD - lU)));
    float extraUiEdge = max(0.0, gradGame - gradLow - 0.025 - 0.20 * edgeTol);
    bool inUiCard = (max3(abs(cGame - cLow)) > 0.010) ||
                    (max3(abs(gL - lL)) > 0.012 && max3(abs(gR - lR)) > 0.012) ||
                    (max3(abs(gU - lU)) > 0.012 && max3(abs(gD - lD)) > 0.012);
    float flatCardMainMenu = (!hasHud && inUiCard && (
        (expectedGameRange > 0.015 && gameRange < 0.45 * expectedGameRange) ||
        (gameRange < 0.018 && worldVar > 0.006)
    )) ? max(worldVar - gameRange, 0.045) : 0.0;
    float flatOverlayHud = (hasHud && max3(abs(cGame - cLow)) > 0.025 && edgeRange > 0.04 &&
                            gradGame < 0.35 * edgeRange && max3(wMax - cGame) > 0.03)
        ? (edgeRange - gradGame)
        : 0.0;
    float opaqueDarkCard = max(
        (negDiff > 0.03 && max3(cGame) < 0.08 && gradLow > 0.04 && gradGame < 0.30 * gradLow)
            ? (gradLow - gradGame)
            : 0.0,
        max(flatCardMainMenu, flatOverlayHud)
    );

    float dUi = max(max(posUi, chromaDiff * 1.5), max(extraUiEdge, opaqueDarkCard));
    float pass54Mask = 1.0 - pass54Transmittance;
    float uiMask = max(max(smoothstep(0.012, 0.038, dUi), hudMask), pass54Mask);

    float3 finalColor = lerp(cBase, cGame, uiMask);
    return float4(saturate(finalColor), 1.0);
}
)hlsl";

} // namespace edpe
