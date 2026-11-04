#pragma once

namespace edpe {

inline constexpr char kUiCompositeShader[] = R"hlsl(
Texture2D<float4> tDlss : register(t0);
Texture2D<float4> tGame : register(t1);
Texture2D<float4> tLowWorld : register(t2);
Texture2D<float4> tHudMask : register(t3);
SamplerState sLinear : register(s0);

cbuffer CompositeParams : register(b0) {
    float2 renderSize;
    float2 renderTexel;
    float2 outputSize;
    float2 outputTexel;
    float4 flags; // x: isHdr (1.0 = HDR inputs, 0.0 = LDR inputs), y: isNativeRes, z: preserveUi, w: isNativeHud
};

float3 toLdr(float3 c) {
    float3 x = max(c, 0.0);
    float a = 2.51;
    float b = 0.03;
    float c_coeff = 2.43;
    float d = 0.59;
    float e = 0.14;
    float3 mapped = saturate((x * (a * x + b)) / (x * (c_coeff * x + d) + e));
    return pow(mapped, 1.0 / 2.2);
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
    int3 pixelPos = int3(pin.pos.xy, 0);

    if (flags.z < 0.5) {
        float3 cOut = tDlss.Load(pixelPos).rgb;
        if (flags.x > 0.5) cOut = toLdr(cOut);
        return float4(saturate(cOut), 1.0);
    }

    // Direct door composite: pure reconstructed clean 3D world + native unjittered HUD overlay.
    // The diff mask is completely bypassed so the reconstructed world is NEVER replaced with jittered tGame!
    if (flags.w > 0.5) {
        float3 cDlss = tDlss.Load(pixelPos).rgb;
        float4 hSample = tHudMask.Load(pixelPos);
        float hudTransmittance = saturate(hSample.a);

        if (flags.x > 0.5) {
            // HDR mode: both cDlss and hSample.rgb are in linear HDR space.
            // Composite linearly in HDR, then tonemap together to LDR!
            float3 combinedHdr = cDlss * hudTransmittance + hSample.rgb;
            return float4(toLdr(combinedHdr), 1.0);
        } else {
            // SDR mode: cDlss already contains Cobra's full post-processing (bloom, exposure, 3D LUT tonemapping).
            // hSample.rgb is linear HDR HUD radiance; map it to display LDR via toLdr() while backing dims cDlss.
            float3 hudColor = toLdr(hSample.rgb);
            float3 finalColor = saturate(cDlss * hudTransmittance + hudColor);
            return float4(finalColor, 1.0);
        }
    }

    // Sample Pass 35 HUD mask at exact pixel center without spatial dilation so
    // 3D background pixels surrounding HUD text/lines are never replaced with jittered tGame.
    float4 hSample = (flags.w > 0.5) ? tHudMask.Load(pixelPos) : tHudMask.SampleLevel(sLinear, pin.uv, 0);
    float hudRaw = max(hSample.r, max(hSample.g, hSample.b));
    float hudMask = smoothstep(0.005, 0.08, hudRaw);

    if (flags.w < 0.5 && hudMask > 0.0) {
        // Distinguish sharp/opaque HUD elements (text, numbers, icons, reticle lines)
        // from flat translucent HUD panels where 3D geometry edges are visible behind the panel.
        float3 hC_ldr = toLdr(hSample.rgb);
        float3 hN_ldr = toLdr(tHudMask.SampleLevel(sLinear, pin.uv + float2(0.0, -renderTexel.y), 0).rgb);
        float3 hS_ldr = toLdr(tHudMask.SampleLevel(sLinear, pin.uv + float2(0.0,  renderTexel.y), 0).rgb);
        float3 hE_ldr = toLdr(tHudMask.SampleLevel(sLinear, pin.uv + float2( renderTexel.x, 0.0), 0).rgb);
        float3 hW_ldr = toLdr(tHudMask.SampleLevel(sLinear, pin.uv + float2(-renderTexel.x, 0.0), 0).rgb);
        float3 hMin = min(hC_ldr, min(min(hN_ldr, hS_ldr), min(hE_ldr, hW_ldr)));
        float3 hMax = max(hC_ldr, max(max(hN_ldr, hS_ldr), max(hE_ldr, hW_ldr)));
        float3 hDiff = hMax - hMin;
        float hudRange = max(hDiff.r, max(hDiff.g, hDiff.b));
        float hudOpaque = smoothstep(0.08, 0.25, hudRaw);

        int maxW = (int)renderSize.x - 1;
        int maxH = (int)renderSize.y - 1;
        int2 pC = clamp((int2)floor(pin.uv * renderSize), int2(0, 0), int2(maxW, maxH));
        int2 pN = clamp(pC + int2(0, -1), int2(0, 0), int2(maxW, maxH));
        int2 pS = clamp(pC + int2(0,  1), int2(0, 0), int2(maxW, maxH));
        int2 pE = clamp(pC + int2( 1, 0), int2(0, 0), int2(maxW, maxH));
        int2 pW = clamp(pC + int2(-1, 0), int2(0, 0), int2(maxW, maxH));
        float3 lC = tLowWorld.Load(int3(pC, 0)).rgb;
        float3 lN = tLowWorld.Load(int3(pN, 0)).rgb;
        float3 lS = tLowWorld.Load(int3(pS, 0)).rgb;
        float3 lE = tLowWorld.Load(int3(pE, 0)).rgb;
        float3 lW = tLowWorld.Load(int3(pW, 0)).rgb;
        if (flags.x > 0.5) {
            lC = toLdr(lC);
            lN = toLdr(lN);
            lS = toLdr(lS);
            lE = toLdr(lE);
            lW = toLdr(lW);
        }
        float3 lMin = min(lC, min(min(lN, lS), min(lE, lW)));
        float3 lMax = max(lC, max(max(lN, lS), max(lE, lW)));
        float3 lDiff = lMax - lMin;
        float lowRange = max(lDiff.r, max(lDiff.g, lDiff.b));

        float bgOnlyEdge = max(0.0, lowRange - hudRange * 2.0);
        float bgEdgeFactor = smoothstep(0.02, 0.10, bgOnlyEdge);
        hudMask *= lerp(1.0, hudOpaque, bgEdgeFactor);
    }

    [branch]
    if (flags.y > 0.5 && flags.x < 0.5) {
        float3 gameN = tGame.Load(pixelPos).rgb;
        float3 lowN = tLowWorld.Load(pixelPos).rgb;
        float3 dlssN = tDlss.Load(pixelPos).rgb;
        float3 nd = abs(gameN - lowN);
        float d = max(nd.r, max(nd.g, nd.b));
        float a = smoothstep(0.04, 0.35, d);
        float3 darken = saturate(gameN / max(lowN, 0.002));
        float3 isDark = step(gameN + 0.0005, lowN);
        float3 t = lerp(float3(1.0 - a, 1.0 - a, 1.0 - a), darken, isDark);
        t *= (1.0 - saturate(hudMask));
        return float4(saturate(gameN + t * (dlssN - lowN)), 1.0);
    }

    float3 cDlss = tDlss.Load(pixelPos).rgb;
    float3 cGame = tGame.Load(pixelPos).rgb;
    float3 cLow  = tLowWorld.SampleLevel(sLinear, pin.uv, 0).rgb;

    if (flags.x > 0.5) {
        cDlss = toLdr(cDlss);
        cLow  = toLdr(cLow);
    }

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

    float3 wMin = min(min(w00, w10), min(w01, w11));
    float3 wMax = max(max(w00, w10), max(w01, w11));

    float3 outsideMin = max(float3(0.0, 0.0, 0.0), wMin - cGame);
    float3 outsideMax = max(float3(0.0, 0.0, 0.0), cGame - wMax);
    float dBox = max(max(outsideMin.r, outsideMin.g), outsideMin.b) +
                 max(max(outsideMax.r, outsideMax.g), outsideMax.b);

    float3 diff = abs(cGame - cLow);
    float dLinear = max(diff.r, max(diff.g, diff.b));

    float3 edgeDiff = wMax - wMin;
    float edgeRange = max(edgeDiff.r, max(edgeDiff.g, edgeDiff.b));

    float dUi = max(dBox * 2.0, dLinear - 0.20 * edgeRange);
    float uiMask = smoothstep(0.010, 0.035, dUi);
    float alphaUi = smoothstep(0.04, 0.35, dLinear);

    float3 deltaUi = (cGame - cLow) - alphaUi * (cDlss - cLow);
    float3 finalColor = cDlss + uiMask * deltaUi;

    if (flags.w > 0.5) {
        float3 hudColor = (flags.x > 0.5) ? hSample.rgb : toLdr(hSample.rgb);
        float3 combined = finalColor * saturate(hSample.a) + hudColor;
        finalColor = (flags.x > 0.5) ? toLdr(combined) : saturate(combined);
    } else {
        finalColor = lerp(finalColor, cGame, saturate(hudMask));
    }

    return float4(saturate(finalColor), 1.0);
}
)hlsl";

} // namespace edpe
