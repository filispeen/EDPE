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

// Safely sample the Pass 52 tonemapper's black floor (3D LUT toe lift at HDR = 0)
// strictly from a corner pixel where tHudMask has zero HUD coverage (RGB == 0, A == 1).
// When the cockpit camera rotates in a circle, HUD geometry or space dust can pass over
// corner (0, 0); blindly reading tHudTonemapped(0, 0) on that frame would read bright
// orange HUD radiance as hudBlack and subtract it from the entire screen, erasing the HUD!
float3 sampleHudTonemapBlackFloor(bool isNativeSizedHud) {
    int2 dim = (int2)(isNativeSizedHud ? outputSize : renderSize);
    int maxX = max(dim.x - 1, 0);
    int maxY = max(dim.y - 1, 0);
    int3 corners[4] = {
        int3(0, 0, 0),
        int3(maxX, 0, 0),
        int3(0, maxY, 0),
        int3(maxX, maxY, 0)
    };
    [unroll]
    for (int i = 0; i < 4; ++i) {
        float4 m = tHudMask.Load(corners[i]);
        if (all(m.rgb <= 1e-6) && m.a >= 0.999) {
            return min(tHudTonemapped.Load(corners[i]).rgb, float3(0.04, 0.04, 0.04));
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
    if (flags.w > 0.5) {
        bool isNativeSizedHud = (flags.w < 1.5) || (flags.w > 2.5 && flags.w < 3.5);
        bool hasTonemappedHud = (flags.w > 2.5);
        float4 hSample = isNativeSizedHud
            ? tHudMask.Load(pixelPos)
            : tHudMask.SampleLevel(sLinear, pin.uv, 0);
        float hudTransmittance = saturate(hSample.a);
        bool hasHud = any(hSample.rgb > 1e-5) || (hudTransmittance < 0.999);
        if (!hasHud) {
            cScene = (flags.x > 0.5) ? toLdr(cDlss) : saturate(cDlss);
        } else if (flags.x > 0.5) {
            float3 combinedHdr = cDlss * hudTransmittance + hSample.rgb;
            cScene = toLdr(combinedHdr);
        } else if (hasTonemappedHud) {
            float3 hudPost = isNativeSizedHud
                ? tHudTonemapped.Load(pixelPos).rgb
                : tHudTonemapped.SampleLevel(sLinear, pin.uv, 0).rgb;
            float3 hudBlack = sampleHudTonemapBlackFloor(isNativeSizedHud);
            float3 hudToned = max(hudPost - hudBlack, 0.0);
            cScene = saturate(cDlss * hudTransmittance + hudToned);
        } else {
            float3 dlssLin = pow(max(cDlss, 0.0), 2.2);
            float3 hudLin = tonemapLinear(hSample.rgb * 120.0);
            float3 bgLin = dlssLin * hudTransmittance;
            float3 combinedLin = bgLin + hudLin - bgLin * hudLin;
            cScene = pow(saturate(combinedLin), 1.0 / 2.2);
        }
        // During normal cockpit flight (flags.w > 0.5; no 2D modal/ESC menu active),
        // tGame (presentBackbuffer) does not contain the separated Pass 35 Cockpit HUD.
        // Return cScene directly so post-tonemap noise/motion-blur differences between
        // tGame and tLowWorld during camera rotation never overwrite cScene and erase the HUD.
        return float4(saturate(cScene), 1.0);
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

    // (a) Multiplicative vignette / shadow darkening applied directly to reconstructed cScene:
    // Because both cGame and cLow contain the exact same pre-AA 3D scene (cGame = T_vig * cLow),
    // the ratio tDark = cGame / cLow cancels out the jittered/low-res 3D edges of cLow identically!
    float negDiff = max(0.0, max3(cLow - cGame));
    float darkWeight = smoothstep(0.010, 0.030, negDiff);
    float3 tDark = clamp(cGame / max(cLow, float3(0.004, 0.004, 0.004)), 0.0, 1.0);
    float3 cBase = cScene * lerp(float3(1.0, 1.0, 1.0), tDark, darkWeight);

    // (b) Detect genuine emissive, chromatic, or opaque 2D UI elements (excluding neutral vignette darkening):
    // 1. Positive brightening above cLow / wMax
    float posCenter = max(0.0, max3(cGame - cLow) - 0.15 * edgeRange);
    float posBox = max(0.0, max3(cGame - wMax));
    float posNeigh = max(
        max(max3(gL - lL), max3(gR - lR)),
        max(max3(gU - lU), max3(gD - lD))
    );
    float posUi = max(max(posCenter, posBox * 2.0), 0.65 * max(0.0, posNeigh - 0.15 * edgeRange));

    // 2. Chromaticity shift (color change orthogonal to neutral scalar darkening cGame = k * cLow)
    float kScale = clamp(dot(cGame, cLow) / max(dot(cLow, cLow), 1e-4), 0.0, 1.5);
    float chromaDiff = max(0.0, max3(abs(cGame - cLow * kScale)) - 0.15 * edgeRange);

    // 3. Sharp 2D UI edges in cGame that do not exist in cLow, OR opaque dark UI cards
    //    that completely flatten a strong underlying 3D edge in cLow
    float gradGame = max(max3(abs(gR - gL)), max3(abs(gD - gU)));
    float gradLow  = max(max3(abs(lR - lL)), max3(abs(lD - lU)));
    float extraUiEdge = max(0.0, gradGame - gradLow - 0.025);
    float opaqueDarkCard = (negDiff > 0.03 && max3(cGame) < 0.08 && gradLow > 0.04 && gradGame < 0.30 * gradLow)
        ? (gradLow - gradGame)
        : 0.0;

    float dUi = max(max(posUi, chromaDiff * 1.5), max(extraUiEdge, opaqueDarkCard));
    float uiMask = max(smoothstep(0.012, 0.038, dUi), hudMask);

    float3 finalColor = lerp(cBase, cGame, uiMask);
    return float4(saturate(finalColor), 1.0);
}
)hlsl";

} // namespace edpe
