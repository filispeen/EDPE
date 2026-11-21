#pragma once

namespace edpe {

// Full-screen DEV=1 visualization of MotionPass OutClass (R8_UINT) over dimmed game frame.
inline constexpr char kMotionClassDebugShader[] = R"(
cbuffer MotionClassDebugConstants : register(b0) {
    float4 dims; // x: classWidth, y: classHeight, z: opacity, w: isHdr
};

Texture2D<float4> currentFrame : register(t0);
Texture2D<uint>   classMap     : register(t1);
SamplerState linearSampler     : register(s0);

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

float3 classColor(uint cls) {
    if (cls == 0u) return float3(0.12, 0.22, 0.45); // Sky
    if (cls == 1u) return float3(0.15, 0.78, 0.30); // World
    if (cls == 2u) return float3(0.95, 0.65, 0.10); // CockpitStatic
    if (cls == 3u) return float3(0.95, 0.18, 0.18); // Disoccluded
    if (cls == 4u) return float3(0.10, 0.85, 0.95); // Weapon
    if (cls == 5u) return float3(0.68, 0.28, 0.95); // Celestial
    if (cls == 6u) return float3(0.95, 0.20, 0.75); // OffscreenFG
    return float3(0.95, 0.95, 0.95);                // Reset
}

float4 ps_main(VSOutput pin) : SV_Target {
    float3 scene = max(currentFrame.SampleLevel(linearSampler, pin.uv, 0).rgb, 0.0);
    if (dims.w > 0.5) scene = scene / (1.0 + scene);
    float luma = dot(saturate(scene), float3(0.299, 0.587, 0.114));
    float3 baseBg = float3(luma, luma, luma) * 0.35;

    int2 maxCoord = max(int2(dims.xy) - 1, int2(0, 0));
    int2 coord = clamp(int2(pin.uv * dims.xy), int2(0, 0), maxCoord);
    uint cls = classMap.Load(int3(coord, 0));
    float3 tint = classColor(cls);
    float alpha = saturate(dims.z);
    return float4(lerp(baseBg, tint, alpha), 1.0);
}
)";

// Lightweight 1/4-stride compute histogram counting all 8 MotionPixelClass bins.
inline constexpr char kMotionClassHistogramShader[] = R"(
cbuffer HistConstants : register(b0) {
    uint4 histDims; // x: width, y: height, z: stride (4), w: 0
};
Texture2D<uint> classMap : register(t0);
RWStructuredBuffer<uint> histogram : register(u0);

[numthreads(8, 8, 1)]
void cs_main(uint3 tid : SV_DispatchThreadID) {
    uint2 coord = tid.xy * histDims.z;
    if (coord.x >= histDims.x || coord.y >= histDims.y) return;
    uint cls = min(classMap.Load(int3(int2(coord), 0)), 7u);
    InterlockedAdd(histogram[cls], 1u);
}
)";

} // namespace edpe
