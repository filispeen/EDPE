#pragma once

#include "temporal_math.h"

#include <cmath>
#include <cstddef>

namespace edpe {

// Experimental parser for the observed 5376-byte 2D scene constant buffer.
// It checks the measured relation between camera rows 932-943 and block 1080-1095.
inline bool parseEliteCamera(const float* words, size_t count, CameraProjection* camera) {
    if (!words || count != 5376 / sizeof(float) || !camera) return false;
    for (size_t i = 932; i < 944; ++i)
        if (!std::isfinite(words[i])) return false;
    for (size_t i = 1080; i < 1096; ++i)
        if (!std::isfinite(words[i])) return false;
    for (size_t axis = 0; axis < 3; ++axis) {
        double length = 0;
        for (size_t row = 0; row < 3; ++row) {
            const double value = words[932 + row * 4 + axis];
            length += value * value;
        }
        if (length < 0.98 || length > 1.02) return false;
        for (size_t other = 0; other < axis; ++other) {
            double dot = 0;
            for (size_t row = 0; row < 3; ++row)
                dot += static_cast<double>(words[932 + row * 4 + axis]) *
                    words[932 + row * 4 + other];
            if (std::fabs(dot) > 0.02) return false;
        }
    }

    double xNumerator = 0, xDenominator = 0, yNumerator = 0, yDenominator = 0;
    for (size_t row = 0; row < 3; ++row) {
        const double x = words[932 + row * 4];
        const double y = words[933 + row * 4];
        xNumerator += x * words[1080 + row * 4];
        xDenominator += x * x;
        yNumerator += y * words[1081 + row * 4];
        yDenominator += y * y;
    }
    const double scaleX = xNumerator / xDenominator;
    const double scaleY = yNumerator / yDenominator;
    if (!(scaleX > 0 && scaleY > 0 && words[1094] > 0 && words[1094] <= 1)) return false;

    for (size_t row = 0; row < 3; ++row) {
        const size_t c = 932 + row * 4, p = 1080 + row * 4;
        if (std::fabs(words[p] - scaleX * words[c]) > 0.002 ||
            std::fabs(words[p + 1] - scaleY * words[c + 1]) > 0.002 ||
            std::fabs(words[p + 2]) > 0.002 ||
            std::fabs(words[p + 3] - words[c + 2]) > 0.002) return false;
    }
    if (std::fabs(words[1092]) > 0.002 || std::fabs(words[1093]) > 0.002 ||
        std::fabs(words[1095]) > 0.002) return false;

    for (size_t i = 0; i < 12; ++i) camera->worldFromView[i] = words[932 + i];
    camera->scaleX = static_cast<float>(scaleX);
    camera->scaleY = static_cast<float>(scaleY);
    camera->depthB = words[1094];
    return true;
}

} // namespace edpe
