#pragma once

#include "temporal_math.h"

#include <cmath>
#include <cstddef>

namespace edpe {

// Experimental parser for the observed 5376-byte 2D scene constant buffer.
// It checks the measured relation between camera rows 932-943 and block 1080-1095.
// failReason (optional): 1 args, 2 nonfinite rows, 3 nonfinite block, 4 axis length,
// 5 axis dot, 6 scale or depthB, 7 x column, 8 y column, 9 z column, 10 w column, 11 tail words.
inline bool parseEliteCameraBlocks(const float* cam932, const float* proj1080,
    CameraProjection* camera, int* failReason = nullptr) {
    auto fail = [failReason](int code) { if (failReason) *failReason = code; return false; };
    if (!cam932 || !proj1080 || !camera) return fail(1);
    for (size_t i = 0; i < 12; ++i)
        if (!std::isfinite(cam932[i])) return fail(2);
    for (size_t i = 0; i < 16; ++i)
        if (!std::isfinite(proj1080[i])) return fail(3);
    for (size_t axis = 0; axis < 3; ++axis) {
        double length = 0;
        for (size_t row = 0; row < 3; ++row) {
            const double value = cam932[row * 4 + axis];
            length += value * value;
        }
        if (length < 0.98 || length > 1.02) return fail(4);
        for (size_t other = 0; other < axis; ++other) {
            double dot = 0;
            for (size_t row = 0; row < 3; ++row)
                dot += static_cast<double>(cam932[row * 4 + axis]) *
                    cam932[row * 4 + other];
            if (std::fabs(dot) > 0.02) return fail(5);
        }
    }

    double xLength = 0, yLength = 0;
    for (size_t row = 0; row < 3; ++row) {
        const double x = proj1080[row * 4];
        const double y = proj1080[row * 4 + 1];
        xLength += x * x;
        yLength += y * y;
    }
    const double scaleX = std::sqrt(xLength);
    const double scaleY = std::sqrt(yLength);
    if (!(scaleX > 0 && scaleY > 0 && proj1080[14] > 0 && proj1080[14] <= 1)) return fail(6);

    // During a rapid turn the unscaled rows can lag the projection block.
    // Take the coherent orientation from the block, retaining its row translation.
    for (size_t row = 0; row < 3; ++row) {
        const size_t c = row * 4, p = row * 4;
        if (std::fabs(proj1080[p] / scaleX - cam932[c]) > 0.02) return fail(7);
        if (std::fabs(proj1080[p + 1] / scaleY - cam932[c + 1]) > 0.02) return fail(8);
        if (std::fabs(proj1080[p + 2]) > 0.002) return fail(9);
        if (std::fabs(proj1080[p + 3] - cam932[c + 2]) > 0.02) return fail(10);
        camera->worldFromView[row * 4] = static_cast<float>(proj1080[p] / scaleX);
        camera->worldFromView[row * 4 + 1] = static_cast<float>(proj1080[p + 1] / scaleY);
        camera->worldFromView[row * 4 + 2] = proj1080[p + 3];
        camera->worldFromView[row * 4 + 3] = cam932[c + 3];
    }
    if (std::fabs(proj1080[12]) > 0.002 || std::fabs(proj1080[13]) > 0.002 ||
        std::fabs(proj1080[15]) > 0.002) return fail(11);

    camera->scaleX = static_cast<float>(scaleX);
    camera->scaleY = static_cast<float>(scaleY);
    camera->depthB = proj1080[14];
    return true;
}

inline bool parseEliteCamera(const float* words, size_t count, CameraProjection* camera,
    int* failReason = nullptr) {
    if (!words || count != 5376 / sizeof(float) || !camera) {
        if (failReason) *failReason = 1;
        return false;
    }
    return parseEliteCameraBlocks(&words[932], &words[1080], camera, failReason);
}

// Inverse of jitterEliteDotProjectionBlock on the 208-byte CB0 transpose layout
// (cb0[4..7], i.e. transposed cb1[270..273]). Row 3 holds view-Z forward.
inline bool estimateEliteTransposeBlockNdcOffset(const float* block16, NdcOffset* outOffset) {
    if (!block16 || !outOffset) return false;
    double ww = 0.0, xw = 0.0, yw = 0.0;
    for (size_t col = 0; col < 3; ++col) {
        const double x = block16[col];
        const double y = block16[4 + col];
        const double w = block16[12 + col];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w)) return false;
        ww += w * w;
        xw += x * w;
        yw += y * w;
    }
    if (!(ww > 1e-6)) return false;
    outOffset->x = static_cast<float>(xw / ww);
    outOffset->y = static_cast<float>(yw / ww);
    return true;
}

// Validates that a 16-float transpose projection (or Model*ViewProj) block matches refCam's
// depthB, FOV scales (within 2%), orthogonality, and positive chirality before injecting jitter.
inline bool matchesEliteTransposeProjectionBlock(const float* block16,
    const CameraProjection& refCam) {
    if (!block16 || !(refCam.scaleX > 0.0f && refCam.scaleY > 0.0f)) return false;
    for (size_t i = 0; i < 16; ++i)
        if (!std::isfinite(block16[i])) return false;
    // Transposed row 3 col 2 (depthB) lands at index 11; unused clip-Z row cells [8..10] must be near zero.
    if (!(block16[11] >= 0.01f && block16[11] <= 0.25f)) return false;
    if (refCam.depthB >= 0.01f && std::fabs(block16[11] - refCam.depthB) > 1e-3f) return false;
    if (std::fabs(block16[8]) > 0.002f || std::fabs(block16[9]) > 0.002f ||
        std::fabs(block16[10]) > 0.002f) {
        return false;
    }
    const double ww = static_cast<double>(block16[12]) * block16[12] +
                      static_cast<double>(block16[13]) * block16[13] +
                      static_cast<double>(block16[14]) * block16[14];
    if (!(ww > 1e-6)) return false;
    const double lenW = std::sqrt(ww);
    if (lenW < 0.05 || lenW > 1e8) return false;

    NdcOffset ndc{};
    if (!estimateEliteTransposeBlockNdcOffset(block16, &ndc)) return false;
    if (std::fabs(ndc.x) > 0.05f || std::fabs(ndc.y) > 0.05f) return false;

    double lenSq0 = 0.0, lenSq1 = 0.0, dot01 = 0.0;
    double r0[3]{}, r1[3]{};
    for (size_t i = 0; i < 3; ++i) {
        r0[i] = static_cast<double>(block16[i]) - static_cast<double>(ndc.x) * block16[12 + i];
        r1[i] = static_cast<double>(block16[4 + i]) - static_cast<double>(ndc.y) * block16[12 + i];
        lenSq0 += r0[i] * r0[i];
        lenSq1 += r1[i] * r1[i];
        dot01 += r0[i] * r1[i];
    }
    const double len0 = std::sqrt(lenSq0);
    const double len1 = std::sqrt(lenSq1);
    if (!(len0 > 0.0 && len1 > 0.0)) return false;
    if (std::fabs(dot01) > 0.02 * len0 * len1) return false;

    // Normalize out uniform model scale lenW to compare true perspective FOV scales.
    const double sx = len0 / lenW;
    const double sy = len1 / lenW;
    if (std::fabs(sx - refCam.scaleX) > 0.02 * refCam.scaleX ||
        std::fabs(sy - refCam.scaleY) > 0.02 * refCam.scaleY) {
        return false;
    }

    // Reject mirrored/planar reflection transforms via normalized right-handed determinant.
    const double w0 = block16[12], w1 = block16[13], w2 = block16[14];
    const double cx = r0[1] * r1[2] - r0[2] * r1[1];
    const double cy = r0[2] * r1[0] - r0[0] * r1[2];
    const double cz = r0[0] * r1[1] - r0[1] * r1[0];
    const double det = (cx * w0 + cy * w1 + cz * w2) / (len0 * len1 * lenW);
    return det > 0.85 && det < 1.15;
}

} // namespace edpe
