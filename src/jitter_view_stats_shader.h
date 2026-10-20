#pragma once

namespace edpe {

inline constexpr char kJitterViewStatsShader[] = R"(
cbuffer StatsConstants : register(b0) {
    uint width;
    uint height;
    uint2 padding;
};

Texture2D<float4> inputTex : register(t0);
RWStructuredBuffer<uint> tiles : register(u0);

groupshared uint s_tileCount[64];

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

    s_tileCount[groupIndex] = localCount;
    GroupMemoryBarrierWithGroupSync();

    [unroll]
    for (uint s = 32; s > 0; s >>= 1) {
        if (groupIndex < s) {
            s_tileCount[groupIndex] += s_tileCount[groupIndex + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (groupIndex == 0) {
        uint tileIdx = groupID.y * 64 + groupID.x;
        tiles[tileIdx] = s_tileCount[0];
    }
}
)";

}
