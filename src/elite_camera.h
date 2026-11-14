#pragma once

#include "temporal_math.h"

#include <cmath>
#include <cstddef>

namespace edpe {

// Experimental parser for the observed 5376-byte 2D scene constant buffer.
// It checks the measured relation between camera rows 932-943 and block 1080-1095.
// failReason (optional): 1 args, 2 nonfinite rows, 3 nonfinite block, 4 axis length,
// 5 axis dot, 6 scale or depthB, 7 x column, 8 y column, 9 z column, 10 w column, 11 tail words.
inline bool parseEliteCamera(const float* words, size_t count, CameraProjection* camera,
    int* failReason = nullptr) {
    auto fail = [failReason](int code) { if (failReason) *failReason = code; return false; };
    if (!words || count != 5376 / sizeof(float) || !camera) return fail(1);
    for (size_t i = 932; i < 944; ++i)
        if (!std::isfinite(words[i])) return fail(2);
    for (size_t i = 1080; i < 1096; ++i)
        if (!std::isfinite(words[i])) return fail(3);
    for (size_t axis = 0; axis < 3; ++axis) {
        double length = 0;
        for (size_t row = 0; row < 3; ++row) {
            const double value = words[932 + row * 4 + axis];
            length += value * value;
        }
        if (length < 0.98 || length > 1.02) return fail(4);
        for (size_t other = 0; other < axis; ++other) {
            double dot = 0;
            for (size_t row = 0; row < 3; ++row)
                dot += static_cast<double>(words[932 + row * 4 + axis]) *
                    words[932 + row * 4 + other];
            if (std::fabs(dot) > 0.02) return fail(5);
        }
    }

    double xLength = 0, yLength = 0;
    for (size_t row = 0; row < 3; ++row) {
        const double x = words[1080 + row * 4];
        const double y = words[1081 + row * 4];
        xLength += x * x;
        yLength += y * y;
    }
    const double scaleX = std::sqrt(xLength);
    const double scaleY = std::sqrt(yLength);
    if (!(scaleX > 0 && scaleY > 0 && words[1094] > 0 && words[1094] <= 1)) return fail(6);

    // During a rapid turn the unscaled rows can lag the projection block.
    // Take the coherent orientation from the block, retaining its row translation.
    for (size_t row = 0; row < 3; ++row) {
        const size_t c = 932 + row * 4, p = 1080 + row * 4;
        if (std::fabs(words[p] / scaleX - words[c]) > 0.02) return fail(7);
        if (std::fabs(words[p + 1] / scaleY - words[c + 1]) > 0.02) return fail(8);
        if (std::fabs(words[p + 2]) > 0.002) return fail(9);
        if (std::fabs(words[p + 3] - words[c + 2]) > 0.02) return fail(10);
        camera->worldFromView[row * 4] = static_cast<float>(words[p] / scaleX);
        camera->worldFromView[row * 4 + 1] = static_cast<float>(words[p + 1] / scaleY);
        camera->worldFromView[row * 4 + 2] = words[p + 3];
        camera->worldFromView[row * 4 + 3] = words[c + 3];
    }
    if (std::fabs(words[1092]) > 0.002 || std::fabs(words[1093]) > 0.002 ||
        std::fabs(words[1095]) > 0.002) return fail(11);

    camera->scaleX = static_cast<float>(scaleX);
    camera->scaleY = static_cast<float>(scaleY);
    camera->depthB = words[1094];
    return true;
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

// Validates that a 16-float transpose projection block matches refCam's orientation,
// FOV scales (within 2%), and perspective depthB layout before injecting jitter.
inline bool matchesEliteTransposeProjectionBlock(const float* block16,
    const CameraProjection& refCam) {
    if (!block16 || !(refCam.scaleX > 0.0f && refCam.scaleY > 0.0f)) return false;
    for (size_t i = 0; i < 16; ++i)
        if (!std::isfinite(block16[i])) return false;
    // Transposed row 3 col 2 (depthB) lands at index 11; unused projection cells must be near zero.
    if (!(block16[11] >= 0.01f && block16[11] <= 0.25f)) return false;
    if (std::fabs(block16[3]) > 0.002f || std::fabs(block16[7]) > 0.002f ||
        std::fabs(block16[8]) > 0.002f || std::fabs(block16[9]) > 0.002f ||
        std::fabs(block16[10]) > 0.002f || std::fabs(block16[15]) > 0.002f) {
        return false;
    }
    const double ww = static_cast<double>(block16[12]) * block16[12] +
                      static_cast<double>(block16[13]) * block16[13] +
                      static_cast<double>(block16[14]) * block16[14];
    if (ww < 0.96 || ww > 1.04) return false;

    NdcOffset ndc{};
    if (!estimateEliteTransposeBlockNdcOffset(block16, &ndc)) return false;
    if (std::fabs(ndc.x) > 0.05f || std::fabs(ndc.y) > 0.05f) return false;

    double lenSq0 = 0.0, lenSq1 = 0.0;
    double r0[3]{}, r1[3]{};
    for (size_t i = 0; i < 3; ++i) {
        r0[i] = static_cast<double>(block16[i]) - static_cast<double>(ndc.x) * block16[12 + i];
        r1[i] = static_cast<double>(block16[4 + i]) - static_cast<double>(ndc.y) * block16[12 + i];
        lenSq0 += r0[i] * r0[i];
        lenSq1 += r1[i] * r1[i];
    }
    const double len0 = std::sqrt(lenSq0);
    const double len1 = std::sqrt(lenSq1);
    if (!(len0 > 0.0 && len1 > 0.0)) return false;
    if (std::fabs(len0 - refCam.scaleX) > 0.02 * refCam.scaleX ||
        std::fabs(len1 - refCam.scaleY) > 0.02 * refCam.scaleY) {
        return false;
    }

    double trace = 0.0;
    for (size_t row = 0; row < 3; ++row) {
        trace += (r0[row] / len0) * refCam.worldFromView[row * 4] +
                 (r1[row] / len1) * refCam.worldFromView[row * 4 + 1] +
                 static_cast<double>(block16[12 + row]) * refCam.worldFromView[row * 4 + 2];
    }
    return trace > 2.95;
}

} // namespace edpe
