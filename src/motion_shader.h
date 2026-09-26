#pragma once

namespace edpe {

// Experimental current-to-previous motion in render pixels, +Y downward.
// Camera rows match CameraProjection; depth is reversed Z: depthB / viewZ.
inline constexpr char kMotionShader[] = R"(
cbuffer Camera : register(b0) {
    float4 now0, now1, now2;
    float4 prev0, prev1, prev2;
    float4 scales; // current X/Y, previous X/Y
    float4 info;   // depthB, width, height, unused
};
Texture2D<float> sceneDepth : register(t0);

float4 vs(uint id : SV_VertexID) : SV_POSITION {
    float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}

float2 ps(float4 pixel : SV_POSITION) : SV_Target {
    float depth = sceneDepth.Load(int3(int2(pixel.xy), 0));
    if (depth <= 0 || depth > 1) return 0;
    float z = info.x / depth;
    float2 ndc = float2(2 * pixel.x / info.y - 1, 1 - 2 * pixel.y / info.z);
    float3 view = float3(ndc.x * z / scales.x, ndc.y * z / scales.y, z);
    float3 world = float3(dot(now0.xyz, view) + now0.w,
                          dot(now1.xyz, view) + now1.w,
                          dot(now2.xyz, view) + now2.w);
    world -= float3(prev0.w, prev1.w, prev2.w);
    float3 prior = float3(prev0.x * world.x + prev1.x * world.y + prev2.x * world.z,
                          prev0.y * world.x + prev1.y * world.y + prev2.y * world.z,
                          prev0.z * world.x + prev1.z * world.y + prev2.z * world.z);
    if (prior.z <= 0) return 0;
    float2 priorNdc = scales.zw * prior.xy / prior.z;
    float2 priorPixel = float2((priorNdc.x + 1) * info.y * 0.5,
                               (1 - priorNdc.y) * info.z * 0.5);
    return priorPixel - pixel.xy;
}
)";

} // namespace edpe
