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

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};

inline float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline float length(const Vec3& v) {
    return std::sqrt(dot(v, v));
}

inline Vec3 cameraRight(const CameraProjection& c) {
    return {c.worldFromView[0], c.worldFromView[4], c.worldFromView[8]};
}

inline Vec3 cameraUp(const CameraProjection& c) {
    return {c.worldFromView[1], c.worldFromView[5], c.worldFromView[9]};
}

inline Vec3 cameraForward(const CameraProjection& c) {
    return {c.worldFromView[2], c.worldFromView[6], c.worldFromView[10]};
}

inline Vec3 cameraPosition(const CameraProjection& c) {
    return {c.worldFromView[3], c.worldFromView[7], c.worldFromView[11]};
}

inline void cameraSetPosition(CameraProjection& c, const Vec3& p) {
    c.worldFromView[3] = p.x;
    c.worldFromView[7] = p.y;
    c.worldFromView[11] = p.z;
}

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

// Current-to-previous rotational motion for skybox/stars at infinity, +Y downward.
// Pixel coordinates name pixel centres by integer index; jitter is excluded.
inline bool cameraSkyboxMotion(const CameraProjection& now, const CameraProjection& previous,
    unsigned width, unsigned height, float pixelX, float pixelY,
    float* motionX, float* motionY) {
    if (!motionX || !motionY || !width || !height ||
        !(now.scaleX > 0.0f && now.scaleY > 0.0f &&
          previous.scaleX > 0.0f && previous.scaleY > 0.0f) ||
        !std::isfinite(pixelX) || !std::isfinite(pixelY)) return false;

    const double ndcX = 2.0 * (pixelX + 0.5) / width - 1.0;
    const double ndcY = 1.0 - 2.0 * (pixelY + 0.5) / height;
    const double vx = ndcX / now.scaleX;
    const double vy = ndcY / now.scaleY;
    constexpr double vz = 1.0;

    const auto* n = now.worldFromView;
    const double wx = n[0] * vx + n[1] * vy + n[2] * vz;
    const double wy = n[4] * vx + n[5] * vy + n[6] * vz;
    const double wz = n[8] * vx + n[9] * vy + n[10] * vz;

    const auto* p = previous.worldFromView;
    const double px = p[0] * wx + p[4] * wy + p[8] * wz;
    const double py = p[1] * wx + p[5] * wy + p[9] * wz;
    const double pz = p[2] * wx + p[6] * wy + p[10] * wz;

    if (!(pz > 0.0) || !std::isfinite(pz)) return false;

    const double dx = (previous.scaleX * px / pz + 1.0) * width * 0.5 - 0.5 - pixelX;
    const double dy = (1.0 - previous.scaleY * py / pz) * height * 0.5 - 0.5 - pixelY;
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
// phaseCount wraps the frame index to a periodic cycle (standard 32 for DLAA/DLSS).
inline ProjectionJitter projectionJitter(std::uint64_t frame, unsigned width, unsigned height, unsigned phaseCount = 32) {
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
    const auto index = (phaseCount > 0) ? ((frame % phaseCount) + 1) : (frame + 1);
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

struct NdcOffset {
    float x, y;
};

// Observation only: inverse of jitterEliteProjectionBlock. Rows 0..2 of the
// 4x4 block are view-space inputs, columns are clip X, Y, Z, W; column W is
// the view Z axis. For a rigid camera columns X and Y are orthogonal to column
// W, so their component along column W is an NDC offset already folded into
// the block (x right, y up, same sign as ProjectionJitter::ndcX/ndcY).
inline bool estimateEliteBlockNdcOffset(const float* block, NdcOffset* out) {
    if (!block || !out) return false;
    double ww = 0, xw = 0, yw = 0;
    for (unsigned row = 0; row < 3; ++row) {
        const double x = block[row * 4], y = block[row * 4 + 1], w = block[row * 4 + 3];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w)) return false;
        ww += w * w;
        xw += x * w;
        yw += y * w;
    }
    if (!(ww > 1e-6)) return false;
    out->x = static_cast<float>(xw / ww);
    out->y = static_cast<float>(yw / ww);
    return true;
}

} // namespace edpe
