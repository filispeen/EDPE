#pragma once

#include <cmath>

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

} // namespace edpe
