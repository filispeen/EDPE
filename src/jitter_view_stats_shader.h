#pragma once

namespace edpe {

inline constexpr char kJitterViewStatsShader[] = R"(
cbuffer StatsConstants : register(b0) {
    uint width;
    uint height;
    uint sceneWidth;
    uint sceneHeight;
    uint isHdr;
    uint hasScene;
    uint2 padding;
};

Texture2D<float4> inputTex : register(t0);
Texture2D<float4> sceneTex : register(t1);
RWStructuredBuffer<uint> tiles : register(u0);

groupshared uint s_tileCount[64];
groupshared uint s_tileLuma[64];

[numthreads(8, 8, 1)]
void cs_main(uint3 groupID : SV_GroupID,
             uint3 threadID : SV_GroupThreadID,
             uint groupIndex : SV_GroupIndex) {
    uint x0 = groupID.x * width / 64;
    uint x1 = (groupID.x + 1) * width / 64;
    uint y0 = groupID.y * height / 36;
    uint y1 = (groupID.y + 1) * height / 36;

    uint localCount = 0;
    for (uint y = y0 + threadID.y; y < y1; y += 8) {
        for (uint x = x0 + threadID.x; x < x1; x += 8) {
            float4 c = inputTex.Load(int3(x, y, 0));
            if (c.g - max(c.r, c.b) > 0.04) {
                localCount++;
            }
        }
    }

    uint localLuma = 0;
    if (hasScene != 0 && sceneWidth > 0 && sceneHeight > 0) {
        uint sx0 = groupID.x * sceneWidth / 64;
        uint sx1 = (groupID.x + 1) * sceneWidth / 64;
        uint sy0 = groupID.y * sceneHeight / 36;
        uint sy1 = (groupID.y + 1) * sceneHeight / 36;

        for (uint sy = sy0 + threadID.y; sy < sy1; sy += 8) {
            for (uint sx = sx0 + threadID.x; sx < sx1; sx += 8) {
                float3 sc = sceneTex.Load(int3(sx, sy, 0)).rgb;
                sc = max(sc, 0.0);
                float3 normC = (isHdr != 0) ? (sc / (1.0 + sc)) : saturate(sc);
                float l = dot(normC, float3(0.299, 0.587, 0.114));
                localLuma += (uint)clamp(round(l * 1023.0), 0.0, 1023.0);
            }
        }
    }

    s_tileCount[groupIndex] = localCount;
    s_tileLuma[groupIndex] = localLuma;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint s = 32; s > 0; s >>= 1) {
        if (groupIndex < s) {
            s_tileCount[groupIndex] += s_tileCount[groupIndex + s];
            s_tileLuma[groupIndex] += s_tileLuma[groupIndex + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (groupIndex == 0) {
        uint tileIdx = groupID.y * 64 + groupID.x;
        tiles[tileIdx] = s_tileCount[0];
        tiles[64 * 36 + tileIdx] = s_tileLuma[0];
    }
}
)";

}
