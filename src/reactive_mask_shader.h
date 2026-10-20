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

inline constexpr char kReactiveMaskDebugShader[] = R"hlsl(
cbuffer ReactiveDebugParams : register(b0) {
    float4 params; // x: mode, y: boost, z: isHdr, w: 0
};

Texture2D<float>  tReactiveMask : register(t0);
Texture2D<float4> tSceneColor   : register(t1);

SamplerState sPoint  : register(s0);
SamplerState sLinear : register(s1);

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

float3 TurboHeatmap(float t) {
    t = saturate(t);
    float3 c;
    c.r = saturate(1.5f - abs(t * 3.0f - 2.25f));
    c.g = saturate(1.5f - abs(t * 3.0f - 1.5f));
    c.b = saturate(1.5f - abs(t * 3.0f - 0.75f));
    if (t > 0.85f) {
        c += (t - 0.85f) / 0.15f;
    }
    return saturate(c);
}

float4 ps_main(VSOutput pin) : SV_Target {
    float2 uv = pin.uv;
    float r = tReactiveMask.SampleLevel(sPoint, uv, 0).r;
    r = saturate(r * max(params.y, 0.01f));

    int mode = int(params.x + 0.5f);

    if (mode == 0) {
        // Mode 0: Pure Grayscale Mask (direct: white = high reactivity, black = 0)
        return float4(r, r, r, 1.0f);
    } else if (mode == 1) {
        // Mode 1: Magenta Highlight Overlay on dimmed game scene
        float3 color = tSceneColor.SampleLevel(sLinear, uv, 0).rgb;
        if (params.z > 0.5f) {
            color = max(color, 0.0f);
            color = color / (1.0f + color);
        }
        float luma = dot(color, float3(0.299f, 0.587f, 0.114f));
        float3 grayBg = float3(luma, luma, luma) * 0.35f;
        float3 magenta = float3(1.0f, 0.1f, 0.85f);
        float3 finalColor = lerp(grayBg, magenta, r);
        return float4(finalColor, 1.0f);
    } else if (mode == 2) {
        // Mode 2: Thermal Heatmap
        float3 heat = TurboHeatmap(r);
        return float4(heat, 1.0f);
    } else {
        // Mode 3: Cyan Highlight Overlay on dimmed game scene
        float3 color = tSceneColor.SampleLevel(sLinear, uv, 0).rgb;
        if (params.z > 0.5f) {
            color = max(color, 0.0f);
            color = color / (1.0f + color);
        }
        float luma = dot(color, float3(0.299f, 0.587f, 0.114f));
        float3 grayBg = float3(luma, luma, luma) * 0.35f;
        float3 cyan = float3(0.0f, 0.95f, 1.0f);
        float3 finalColor = lerp(grayBg, cyan, r);
        return float4(finalColor, 1.0f);
    }
}
)hlsl";

} // namespace edpe
