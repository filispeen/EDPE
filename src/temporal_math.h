#pragma once

#include <cmath>
#include <cstdint>

namespace edpe {

// Experimental 2D convention: row-major view-to-world [R | C], +Z forward.
// scaleX/Y are clip-space projection multipliers; depth = depthB / viewZ.
struct CameraProjection {
    float worldFromView[12];
    float scaleX;
    float scaleY;
    float depthB;
};

// Current-to-previous motion in render-resolution pixels, with +Y downward.
// Pixel coordinates name pixel centres by integer index; jitter is excluded.
inline bool cameraDepthMotion(const CameraProjection& now, const CameraProjection& previous,
    unsigned width, unsigned height, float pixelX, float pixelY, float rawDepth,
    float* motionX, float* motionY) {
    if (!motionX || !motionY || !width || !height ||
        !(rawDepth > 0.0f && rawDepth <= 1.0f) ||
        !(now.depthB > 0.0f && now.scaleX > 0.0f && now.scaleY > 0.0f &&
          previous.scaleX > 0.0f && previous.scaleY > 0.0f) ||
        !std::isfinite(rawDepth) || !std::isfinite(now.depthB) ||
        !std::isfinite(pixelX) || !std::isfinite(pixelY)) return false;

    const double z = static_cast<double>(now.depthB) / rawDepth;
    const double x = (2.0 * (pixelX + 0.5) / width - 1.0) * z / now.scaleX;
    const double y = (1.0 - 2.0 * (pixelY + 0.5) / height) * z / now.scaleY;
    const auto* n = now.worldFromView;
    const double worldX = n[0] * x + n[1] * y + n[2] * z + n[3] - previous.worldFromView[3];
    const double worldY = n[4] * x + n[5] * y + n[6] * z + n[7] - previous.worldFromView[7];
    const double worldZ = n[8] * x + n[9] * y + n[10] * z + n[11] - previous.worldFromView[11];
    const auto* p = previous.worldFromView;
    const double prevX = p[0] * worldX + p[4] * worldY + p[8] * worldZ;
    const double prevY = p[1] * worldX + p[5] * worldY + p[9] * worldZ;
    const double prevZ = p[2] * worldX + p[6] * worldY + p[10] * worldZ;
    if (!(prevZ > 0.0) || !std::isfinite(prevZ)) return false;

    const double dx = (previous.scaleX * prevX / prevZ + 1.0) * width * 0.5 - 0.5 - pixelX;
    const double dy = (1.0 - previous.scaleY * prevY / prevZ) * height * 0.5 - 0.5 - pixelY;
    if (!std::isfinite(dx) || !std::isfinite(dy)) return false;
    *motionX = static_cast<float>(dx);
    *motionY = static_cast<float>(dy);
    return true;
}

struct ProjectionJitter {
    float pixelX, pixelY;
    float ndcX, ndcY;
};

// Halton 2,3 in pixel units; D3D viewport maps positive NDC Y upward.
// Math only: applying this to Elite's projection requires a verified pass and
// a working temporal reconstruction path that can always disable the offset.
inline ProjectionJitter projectionJitter(std::uint64_t frame, unsigned width, unsigned height) {
    if (!width || !height) return {};
    auto halton = [](std::uint64_t index, std::uint64_t base) {
        double value = 0, weight = 1;
        while (index) {
            weight /= base;
            value += weight * (index % base);
            index /= base;
        }
        return value;
    };
    const auto index = frame + 1;
    const float x = static_cast<float>(halton(index, 2) - 0.5);
    const float y = static_cast<float>(halton(index, 3) - 0.5);
    return {x, y, 2 * x / width, -2 * y / height};
}

// CPU-only: the observed vertex shaders form clip position as
// position.x * block[0] + position.y * block[1] + position.z * block[2]
// + block[3], with each block row containing four float components.
// Adding NDC jitter times clip W to clip X/Y therefore changes columns
// X/Y by the corresponding fraction of column W. No game buffer is edited.
inline bool jitterEliteProjectionBlock(const float* source, float* output,
    ProjectionJitter jitter) {
    if (!source || !output || !std::isfinite(jitter.ndcX) ||
        !std::isfinite(jitter.ndcY)) return false;
    float result[16];
    for (unsigned i = 0; i < 16; ++i) {
        if (!std::isfinite(source[i])) return false;
        result[i] = source[i];
    }
    for (unsigned row = 0; row < 4; ++row) {
        const unsigned base = row * 4;
        result[base] += jitter.ndcX * source[base + 3];
        result[base + 1] += jitter.ndcY * source[base + 3];
        if (!std::isfinite(result[base]) || !std::isfinite(result[base + 1]))
            return false;
    }
    for (unsigned i = 0; i < 16; ++i) output[i] = result[i];
    return true;
}

// The alternate scene shader uses dot(cb0[4..7], position): the transpose
// of the block above. Reuse the same checked CPU math in that layout.
inline bool jitterEliteDotProjectionBlock(const float* source, float* output,
    ProjectionJitter jitter) {
    if (!source || !output) return false;
    float transposed[16], shifted[16];
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column)
            transposed[row * 4 + column] = source[column * 4 + row];
    if (!jitterEliteProjectionBlock(transposed, shifted, jitter)) return false;
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column)
            output[row * 4 + column] = shifted[column * 4 + row];
    return true;
}

} // namespace edpe
