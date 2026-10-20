#pragma once

namespace edpe {

inline constexpr char kReactiveMaskShader[] = R"hlsl(
Texture2D<float4> tEmissive : register(t0);
Texture2D<float4> tPreHud   : register(t1);
Texture2D<float4> tPostHud  : register(t2);

cbuffer ReactiveParams : register(b0) {
    float emissiveWeight;
    float hudWeight;
    float emissiveThreshold;
    float hudThreshold;
    float hasEmissive;
    float hasHud;
    float renderWidth;
    float renderHeight;
};

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOutput vs_main(uint id : SV_VertexID) {
    VSOutput vout;
    vout.uv = float2((id << 1) & 2, id & 2);
    vout.pos = float4(vout.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return vout;
}

float4 ps_main(VSOutput pin) : SV_Target {
    int3 pixelPos = int3(pin.pos.xy, 0);
    float reactive = 0.0f;

    // 1. G-Buffer RTV3 Emissive Contribution (Pass 10: lamps, lasers, thrusters, cockpit indicators)
    if (hasEmissive > 0.5f) {
        float3 emissive = tEmissive.Load(pixelPos).rgb;
        float emissiveLuma = dot(emissive, float3(0.2126f, 0.7152f, 0.0722f));
        if (emissiveLuma > emissiveThreshold) {
            float eDiff = emissiveLuma - emissiveThreshold;
            float r = saturate(eDiff * emissiveWeight);
            reactive = max(reactive, r);
        }
    }

    // 2. Cockpit Holographic HUD Contribution (Pass 35: 32 draws directly into HDR target)
    if (hasHud > 0.5f) {
        float3 pre = tPreHud.Load(pixelPos).rgb;
        float3 post = tPostHud.Load(pixelPos).rgb;
        float3 diff = abs(post - pre);
        float hudDiff = max(diff.r, max(diff.g, diff.b));
        if (hudDiff > hudThreshold) {
            // Holographic lines need strong reactivity to prevent temporal smearing
            float r = saturate((hudDiff - hudThreshold) * hudWeight + 0.35f);
            reactive = max(reactive, r);
        }
    }

    return float4(reactive, 0.0f, 0.0f, 1.0f);
}
)hlsl";

} // namespace edpe
