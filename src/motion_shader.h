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
cbuffer EliteNow : register(b1) { float4 currentWords[336]; };
cbuffer ElitePrevious : register(b2) { float4 previousWords[336]; };

float4 vs(uint id : SV_VertexID) : SV_POSITION {
    float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}

float2 reproject(float4 pixel, float4 current0, float4 current1,
    float4 current2, float4 prior0, float4 prior1, float4 prior2,
    float4 projection, float depthB) {
    float depth = sceneDepth.Load(int3(int2(pixel.xy), 0));
    if (depth <= 0 || depth > 1) return 0;
    float z = depthB / depth;
    float2 ndc = float2(2 * pixel.x / info.y - 1, 1 - 2 * pixel.y / info.z);
    float3 view = float3(ndc.x * z / projection.x, ndc.y * z / projection.y, z);
    float3 world = float3(dot(current0.xyz, view) + current0.w,
                          dot(current1.xyz, view) + current1.w,
                          dot(current2.xyz, view) + current2.w);
    world -= float3(prior0.w, prior1.w, prior2.w);
    float3 prior = float3(prior0.x * world.x + prior1.x * world.y + prior2.x * world.z,
                          prior0.y * world.x + prior1.y * world.y + prior2.y * world.z,
                          prior0.z * world.x + prior1.z * world.y + prior2.z * world.z);
    if (prior.z <= 0) return 0;
    float2 priorNdc = projection.zw * prior.xy / prior.z;
    float2 priorPixel = float2((priorNdc.x + 1) * info.y * 0.5,
                               (1 - priorNdc.y) * info.z * 0.5);
    return priorPixel - pixel.xy;
}

float2 ps(float4 pixel : SV_POSITION) : SV_Target {
    return reproject(pixel, now0, now1, now2, prev0, prev1, prev2, scales, info.x);
}

float2 ps_gpu(float4 pixel : SV_POSITION) : SV_Target {
    float3 currentX = float3(currentWords[270].x, currentWords[271].x, currentWords[272].x);
    float3 currentY = float3(currentWords[270].y, currentWords[271].y, currentWords[272].y);
    float3 previousX = float3(previousWords[270].x, previousWords[271].x, previousWords[272].x);
    float3 previousY = float3(previousWords[270].y, previousWords[271].y, previousWords[272].y);
    float4 projection = float4(length(currentX), length(currentY),
        length(previousX), length(previousY));
    float depthB = currentWords[273].z;
    if (any(projection <= 0) || depthB <= 0 || depthB > 1) return 0;
    float4 current0 = float4(currentWords[270].x / projection.x,
        currentWords[270].y / projection.y, currentWords[270].w, currentWords[233].w);
    float4 current1 = float4(currentWords[271].x / projection.x,
        currentWords[271].y / projection.y, currentWords[271].w, currentWords[234].w);
    float4 current2 = float4(currentWords[272].x / projection.x,
        currentWords[272].y / projection.y, currentWords[272].w, currentWords[235].w);
    float4 prior0 = float4(previousWords[270].x / projection.z,
        previousWords[270].y / projection.w, previousWords[270].w, previousWords[233].w);
    float4 prior1 = float4(previousWords[271].x / projection.z,
        previousWords[271].y / projection.w, previousWords[271].w, previousWords[234].w);
    float4 prior2 = float4(previousWords[272].x / projection.z,
        previousWords[272].y / projection.w, previousWords[272].w, previousWords[235].w);
    return reproject(pixel, current0, current1, current2, prior0, prior1,
        prior2, projection, depthB);
}
)";

} // namespace edpe
