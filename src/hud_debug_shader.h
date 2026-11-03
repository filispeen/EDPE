#pragma once

namespace edpe {

// HLSL shader for full-screen HUD & UI Mask Debug View.
// Converts game background to grayscale/dimmed and highlights UI mask pixels:
// - Cockpit Holographic HUD mask in bright Amber/Orange (float3(1.0, 0.55, 0.05))
// - Menu 2D UI diff mask in bright Cyan (float3(0.05, 0.85, 1.0))
// - Overlap in Neon Yellow (float3(0.95, 1.0, 0.1))
inline constexpr char kHudDebugShader[] = R"(
cbuffer HudDebugConstants : register(b0) {
    float4 resolution;       // x: renderWidth, y: renderHeight, z: 1/renderWidth, w: 1/renderHeight
    float4 outputResolution; // x: outputWidth, y: outputHeight, z: 1/outputWidth, w: 1/outputHeight
    float4 debugParams;      // x: opacity, y: threshold, z: mode, w: isHdr
};

Texture2D<float4> currentFrame   : register(t0); // Current rendered frame / backbuffer
Texture2D<float4> hudMask        : register(t1); // Cockpit HUD replay mask
Texture2D<float4> lowResWorld    : register(t2); // Low-res 3D world input (s_dlaa_input_srv)
Texture2D<float4> gameBackbuffer : register(t3); // High-res game backbuffer (s_backbuffer_copy_srv)

SamplerState linearSampler : register(s0);
SamplerState pointSampler  : register(s1);

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOutput vs_main(uint id : SV_VertexID) {
    VSOutput o;
    float2 corner = float2((id << 1) & 2, id & 2);
    o.pos = float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = float2(corner.x, corner.y);
    return o;
}

float3 toLdr(float3 c) {
    float3 mapped = max(c, 0.0) / (1.0 + max(c, 0.0));
    return pow(mapped, 1.0 / 2.2);
}

float4 ps_main(VSOutput pin) : SV_Target {
    float2 uv = pin.uv;
    float3 rawColor = saturate(currentFrame.SampleLevel(pointSampler, uv, 0).rgb);

    // Dimmed grayscale scene background (identical to Subpixel Jitter Debug View)
    float luma = dot(rawColor, float3(0.299, 0.587, 0.114));
    float3 grayBg = float3(luma, luma, luma) * 0.35;

    // 1. Cockpit Holographic HUD mask (Pass 35 replay mask)
    float4 hSample = hudMask.SampleLevel(linearSampler, uv, 0);
    float hudVal = saturate(max(hSample.r, max(hSample.g, hSample.b)));

    // 2. Menu 2D UI diff mask (|gameBackbuffer - lowResWorld|)
    float3 gameSample = saturate(gameBackbuffer.SampleLevel(linearSampler, uv, 0).rgb);
    float3 lowRaw     = lowResWorld.SampleLevel(linearSampler, uv, 0).rgb;
    float3 lowSample  = (debugParams.w > 0.5) ? toLdr(lowRaw) : saturate(lowRaw);
    float3 delta = abs(gameSample - lowSample);
    float diffMax = max(delta.r, max(delta.g, delta.b));
    float threshold = max(debugParams.y, 0.001);
    float menuVal = smoothstep(threshold, threshold * 2.5, diffMax);

    float opacity = max(debugParams.x, 0.01);
    int mode = int(debugParams.z + 0.5);

    // Iconic Elite Dangerous UI colors:
    float3 neonAmber  = float3(1.0, 0.55, 0.05); // Cockpit Holographic HUD (Elite Amber/Orange)
    float3 neonCyan   = float3(0.05, 0.85, 1.0); // Menu 2D UI (Neon Cyan)
    float3 neonYellow = float3(0.95, 1.0, 0.1);  // Overlap (Neon Yellow)

    float3 finalColor = grayBg;

    if (mode == 0) {
        // Mode 0: Combined UI Mask (All UI in Amber)
        float comb = max(hudVal, menuVal);
        if (comb > 0.005) {
            float factor = saturate(comb * opacity);
            finalColor = lerp(grayBg, neonAmber, factor);
        }
    } else if (mode == 1) {
        // Mode 1: Cockpit Holographic HUD Mask Only (Pass 35)
        if (hudVal > 0.005) {
            float factor = saturate(hudVal * opacity);
            finalColor = lerp(grayBg, neonAmber, factor);
        }
    } else if (mode == 2) {
        // Mode 2: Menu 2D UI Diff Mask Only
        if (menuVal > 0.005) {
            float factor = saturate(menuVal * opacity);
            finalColor = lerp(grayBg, neonCyan, factor);
        }
    } else if (mode == 3) {
        // Mode 3: Split Heatmap (Cockpit HUD = Amber, Menu UI = Cyan, Overlap = Yellow)
        bool hasHud = (hudVal > 0.05);
        bool hasMenu = (menuVal > 0.05);
        if (hasHud && hasMenu) {
            float factor = saturate(max(hudVal, menuVal) * opacity);
            finalColor = lerp(grayBg, neonYellow, factor);
        } else if (hasHud) {
            float factor = saturate(hudVal * opacity);
            finalColor = lerp(grayBg, neonAmber, factor);
        } else if (hasMenu) {
            float factor = saturate(menuVal * opacity);
            finalColor = lerp(grayBg, neonCyan, factor);
        }
    } else if (mode == 4) {
        // Mode 4: Binary Mask Only (Pure Black Background)
        float comb = max(hudVal, menuVal);
        finalColor = (comb >= 0.5) ? neonAmber : float3(0.0, 0.0, 0.0);
    }

    return float4(finalColor, 1.0);
}
)";

} // namespace edpe
