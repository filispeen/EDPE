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

} // namespace edpe
