#pragma once

namespace edpe {

inline constexpr char kUiCompositeShader[] = R"hlsl(
Texture2D<float4> tDlss : register(t0);
Texture2D<float4> tGame : register(t1);
Texture2D<float4> tLowWorld : register(t2);
SamplerState sLinear : register(s0);

cbuffer CompositeParams : register(b0) {
    float2 renderSize;
    float2 renderTexel;
    float2 outputSize;
    float2 outputTexel;
};

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

    float3 cDlss = tDlss.Load(pixelPos).rgb;
    float3 cGame = tGame.Load(pixelPos).rgb;
    float3 cLow  = tLowWorld.SampleLevel(sLinear, pin.uv, 0).rgb;

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

    return float4(saturate(finalColor), 1.0);
}
)hlsl";

} // namespace edpe