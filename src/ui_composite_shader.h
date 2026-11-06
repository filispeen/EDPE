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

    float3 cDlss = tDlss.Load(pixelPos).rgb;
    if (flags.z < 0.5) {
        if (flags.x > 0.5) cDlss = toLdr(cDlss);
        return float4(saturate(cDlss), 1.0);
    }

    // Pure separated HUD layer composite (no reactive/diff mask):
    // Pass 35 Cockpit HUD is captured into tHudMask (RGB = HUD radiance, A = background transmittance).
    // Pass 54 2D UI (Main Menu, Pause Menu, Station UI, Crosshair) renders natively onto the backbuffer after this pass.
    float4 hSample = (flags.w > 0.5)
        ? tHudMask.Load(pixelPos)
        : tHudMask.SampleLevel(sLinear, pin.uv, 0);
    float hudTransmittance = (flags.w > 0.5) ? saturate(hSample.a) : 1.0;

    if (flags.x > 0.5) {
        float3 combinedHdr = cDlss * hudTransmittance + hSample.rgb;
        return float4(toLdr(combinedHdr), 1.0);
    } else {
        float3 hudColor = toLdr(hSample.rgb);
        float3 finalColor = saturate(cDlss * hudTransmittance + hudColor);
        return float4(finalColor, 1.0);
    }
}
)hlsl";

} // namespace edpe
