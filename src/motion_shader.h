#pragma once

namespace edpe {

// Current-to-previous motion in render pixels, +Y downward.
// Camera rows match CameraProjection; depth is reversed Z: depthB / viewZ.
inline constexpr char kMotionShader[] = R"(
cbuffer Camera : register(b0) {
    float4 now0, now1, now2;
    float4 prev0, prev1, prev2;
    float4 scales;     // current X/Y, previous X/Y
    float4 info;       // depthB, width, height, hasPrevDepth
    float4 jitterInfo; // jitterX, jitterY, prevJitterX, prevJitterY (render pixels)
};
Texture2D<float> sceneDepth : register(t0);
Texture2D<float> prevSceneDepth : register(t1);
cbuffer EliteNow : register(b1) { float4 currentWords[336]; };
cbuffer ElitePrevious : register(b2) { float4 previousWords[336]; };

float4 vs(uint id : SV_VertexID) : SV_POSITION {
    float2 corner = float2((id << 1) & 2, id & 2);
    return float4(corner * float2(2, -2) + float2(-1, 1), 0, 1);
}

float2 reproject(float4 pixel, float4 current0, float4 current1,
    float4 current2, float4 prior0, float4 prior1, float4 prior2,
    float4 projection, float depthB, bool clampCockpit) {
    if (info.y <= 0 || info.z <= 0 || projection.x <= 0 || projection.y <= 0) return 0;
    float hudScale = 1.0;
    float depth = sceneDepth.Load(int3(int2(pixel.xy), 0));

    // Subtract current raster subpixel jitter phase so camera reprojection operates
    // on true unjittered screen coordinates and outputs jitter-free motion vectors.
    float2 rawPixel = pixel.xy - jitterInfo.xy;

    // 1. Skybox, distant stars, nebula, celestial background, and star bloom:
    // When depth <= 0.0 or depth > 1.0 (reversed-Z clear/invalid) or at extreme
    // astronomical distance (z >= 50,000m, rawDepth < depthB / 50000.0),
    // translation is negligible (T / infinity = 0). Pure analytical ray rotation
    // prevents star erasure, eliminates float32 precision loss, and matches
    // star glow/bloom with the star body.
    float celestialDepthThresh = (depthB > 0.0) ? (depthB / 50000.0) : 1e-6;
    if (depth <= 0.0 || depth > 1.0 || depth < celestialDepthThresh) {
        float2 ndc = float2(2.0 * rawPixel.x / info.y - 1.0, 1.0 - 2.0 * rawPixel.y / info.z);
        float3 viewRay = float3(ndc.x / projection.x, ndc.y / projection.y, 1.0);
        float3 worldRay = float3(dot(current0.xyz, viewRay),
                                 dot(current1.xyz, viewRay),
                                 dot(current2.xyz, viewRay));
        float3 priorRay = float3(prior0.x * worldRay.x + prior1.x * worldRay.y + prior2.x * worldRay.z,
                                 prior0.y * worldRay.x + prior1.y * worldRay.y + prior2.y * worldRay.z,
                                 prior0.z * worldRay.x + prior1.z * worldRay.y + prior2.z * worldRay.z);
        if (priorRay.z <= 0.0) return 0.0;
        float2 priorNdc = projection.zw * priorRay.xy / priorRay.z;
        float2 priorPixel = float2((priorNdc.x + 1.0) * info.y * 0.5,
                                   (1.0 - priorNdc.y) * info.z * 0.5);
        return (priorPixel - rawPixel) * hudScale;
    }

    float z = depthB / depth;

    // 2. Dynamic world geometry (space stations, ships, asteroids, terrain):
    // Standard camera-depth reprojection from the unjittered pixel coordinate.
    float2 ndc = float2(2.0 * rawPixel.x / info.y - 1.0, 1.0 - 2.0 * rawPixel.y / info.z);
    float3 view = float3(ndc.x * z / projection.x, ndc.y * z / projection.y, z);
    float3 world = float3(dot(current0.xyz, view) + current0.w,
                          dot(current1.xyz, view) + current1.w,
                          dot(current2.xyz, view) + current2.w);
    world -= float3(prior0.w, prior1.w, prior2.w);
    float3 prior = float3(prior0.x * world.x + prior1.x * world.y + prior2.x * world.z,
                          prior0.y * world.x + prior1.y * world.y + prior2.y * world.z,
                          prior0.z * world.x + prior1.z * world.y + prior2.z * world.z);
    if (prior.z <= 0.0) return 0.0;
    float2 priorNdc = projection.zw * prior.xy / prior.z;
    float2 priorPixel = float2((priorNdc.x + 1.0) * info.y * 0.5,
                               (1.0 - priorNdc.y) * info.z * 0.5);
    float2 worldMv = (priorPixel - rawPixel) * hudScale;

    // 3. Fast depth-validated cockpit vs. world geometry disambiguation:
    // During ship flight (translationStep > 0.35m/frame), geometry within 3.5m is strictly
    // inside the ship cockpit and moves with the ship frame (zero screen velocity).
    // Only when camera translation is active (translationStep > 0.001) do worldMv and
    // static cockpit velocity differ, requiring at most 2 point loads from prevSceneDepth.
    if (clampCockpit && z < 10.0) {
        float3 camDelta = float3(current0.w - prior0.w, current1.w - prior1.w, current2.w - prior2.w);
        float translationStep = length(camDelta);
        if (z < 3.5 && translationStep > 0.35) {
            return 0.0;
        }
        if (info.w > 0.5 && translationStep > 0.001 && length(worldMv) > 0.25) {
            int2 maxCoord = int2(info.y - 1.0, info.z - 1.0);
            int2 pWorld = clamp(int2(priorPixel + jitterInfo.zw), int2(0, 0), maxCoord);
            int2 pStatic = clamp(int2(rawPixel + jitterInfo.zw), int2(0, 0), maxCoord);
            float dWorld = prevSceneDepth.Load(int3(pWorld, 0));
            float dStatic = prevSceneDepth.Load(int3(pStatic, 0));
            float expectedPriorDepth = depthB / prior.z;
            float tol = max(1e-5, 0.025 * depth);
            bool worldMatches = (dWorld > 0.0 && dWorld <= 1.0) && (abs(dWorld - expectedPriorDepth) <= tol);
            bool staticMatches = (dStatic > 0.0 && dStatic <= 1.0) && (abs(dStatic - depth) <= tol);
            if (!worldMatches && staticMatches) {
                return 0.0;
            }
        }
    }

    return worldMv;
}

float2 ps(float4 pixel : SV_POSITION) : SV_Target {
    return reproject(pixel, now0, now1, now2, prev0, prev1, prev2, scales, info.x, true);
}

float2 ps_gpu(float4 pixel : SV_POSITION) : SV_Target {
    float3 currentW = float3(currentWords[270].w, currentWords[271].w, currentWords[272].w);
    float3 previousW = float3(previousWords[270].w, previousWords[271].w, previousWords[272].w);
    float curWW = max(dot(currentW, currentW), 1e-12);
    float prevWW = max(dot(previousW, previousW), 1e-12);

    float3 rawCurrentX = float3(currentWords[270].x, currentWords[271].x, currentWords[272].x);
    float3 rawCurrentY = float3(currentWords[270].y, currentWords[271].y, currentWords[272].y);
    float3 rawPreviousX = float3(previousWords[270].x, previousWords[271].x, previousWords[272].x);
    float3 rawPreviousY = float3(previousWords[270].y, previousWords[271].y, previousWords[272].y);

    // Strip any subpixel NDC offset folded into the GPU projection columns along W.
    float3 currentX = rawCurrentX - currentW * (dot(rawCurrentX, currentW) / curWW);
    float3 currentY = rawCurrentY - currentW * (dot(rawCurrentY, currentW) / curWW);
    float3 previousX = rawPreviousX - previousW * (dot(rawPreviousX, previousW) / prevWW);
    float3 previousY = rawPreviousY - previousW * (dot(rawPreviousY, previousW) / prevWW);

    float4 projection = float4(length(currentX), length(currentY),
        length(previousX), length(previousY));
    float depthB = currentWords[273].z;
    if (any(projection <= 0) || depthB <= 0 || depthB > 1) return 0;
    float4 current0 = float4(currentX.x / projection.x,
        currentY.x / projection.y, currentW.x, currentWords[233].w);
    float4 current1 = float4(currentX.y / projection.x,
        currentY.y / projection.y, currentW.y, currentWords[234].w);
    float4 current2 = float4(currentX.z / projection.x,
        currentY.z / projection.y, currentW.z, currentWords[235].w);
    float4 prior0 = float4(previousX.x / projection.z,
        previousY.x / projection.w, previousW.x, previousWords[233].w);
    float4 prior1 = float4(previousX.y / projection.z,
        previousY.y / projection.w, previousW.y, previousWords[234].w);
    float4 prior2 = float4(previousX.z / projection.z,
        previousY.z / projection.w, previousW.z, previousWords[235].w);
    return reproject(pixel, current0, current1, current2, prior0, prior1,
        prior2, projection, depthB, false);
}
)";

} // namespace edpe
