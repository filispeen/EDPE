#pragma once

namespace edpe {

// Temporal Anti-Aliasing (TAA) HLSL shader.
// Implements 3x3 YCoCg neighborhood variance clipping, camera-depth motion vector
// reprojection, and subpixel Halton (2,3) jitter accumulation.
inline constexpr char kTaaShader[] = R"(
cbuffer TaaConstants : register(b0) {
    float4 resolution; // x: width, y: height, z: 1/width, w: 1/height
    float4 params;     // x: jitterX, y: jitterY, z: blendWeight, w: resetFlag
};

Texture2D<float4> currentFrame : register(t0);
Texture2D<float4> historyFrame : register(t1);
Texture2D<float2> motionVectors: register(t2);

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

float3 RGBToYCoCg(float3 c) {
    return float3(
         0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
         0.5  * c.r             - 0.5  * c.b,
        -0.25 * c.r + 0.5 * c.g - 0.25 * c.b
    );
}

float3 YCoCgToRGB(float3 c) {
    return saturate(float3(
        c.x + c.y - c.z,
        c.x + c.z,
        c.x - c.y - c.z
    ));
}

float4 ps_main(VSOutput pin) : SV_Target {
    float2 uv = pin.uv;

    // Center sample
    float4 centerSample = currentFrame.SampleLevel(pointSampler, uv, 0);
    float3 current = centerSample.rgb;
    float3 currentY = RGBToYCoCg(current);

    // 3x3 neighborhood statistics in YCoCg space
    float3 m1 = 0;
    float3 m2 = 0;
    float3 boxMin = currentY;
    float3 boxMax = currentY;

    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            float2 sampleUv = uv + float2(x, y) * resolution.zw;
            float3 c = currentFrame.SampleLevel(pointSampler, sampleUv, 0).rgb;
            float3 cY = RGBToYCoCg(c);
            m1 += cY;
            m2 += cY * cY;
            boxMin = min(boxMin, cY);
            boxMax = max(boxMax, cY);
        }
    }

    // Variance bounding box
    float3 mean = m1 / 9.0;
    float3 sigma = sqrt(max(0.0, m2 / 9.0 - mean * mean));
    float3 varMin = mean - 1.25 * sigma;
    float3 varMax = mean + 1.25 * sigma;

    float3 finalMin = max(boxMin, varMin);
    float3 finalMax = min(boxMax, varMax);

    // Sample motion vectors
    // EDPE motion vectors store: (priorPixel - currentPixel) in pixels
    float2 mvPixels = motionVectors.SampleLevel(pointSampler, uv, 0).xy;
    float2 priorUv = uv + mvPixels * resolution.zw;

    bool isOffscreen = (priorUv.x < 0.0 || priorUv.x > 1.0 || priorUv.y < 0.0 || priorUv.y > 1.0);
    bool isReset = (params.w > 0.5) || isOffscreen;

    if (isReset) {
        return float4(current, centerSample.a);
    }

    // Sample history with bilinear filtering
    float4 histSample = historyFrame.SampleLevel(linearSampler, priorUv, 0);
    float3 histY = RGBToYCoCg(histSample.rgb);

    // Variance clip history
    histY = clamp(histY, finalMin, finalMax);
    float3 clampedHistory = YCoCgToRGB(histY);

    // Dynamic blend weight
    float blendWeight = clamp(params.z, 0.0, 0.96);
    float speed = length(mvPixels);
    blendWeight = lerp(blendWeight, 0.70, saturate(speed * 0.05));

    float3 blended = lerp(current, clampedHistory, blendWeight);
    return float4(blended, centerSample.a);
}
)";

} // namespace edpe
