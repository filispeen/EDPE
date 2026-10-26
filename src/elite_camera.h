#pragma once

#include "temporal_math.h"

#include <cmath>
#include <cstddef>

namespace edpe {

inline bool parseEliteCamera(const float* words, size_t count, CameraProjection* camera, const char** outReason = nullptr) {
    if (outReason) *outReason = "ok";
    if (!words) {
        if (outReason) *outReason = "null words";
        return false;
    }
    if (count != 5376 / sizeof(float)) {
        if (outReason) *outReason = "count mismatch";
        return false;
    }
    if (!camera) {
        if (outReason) *outReason = "null camera";
        return false;
    }
    for (size_t i = 932; i < 944; ++i) {
        if (!std::isfinite(words[i])) {
            if (outReason) *outReason = "non-finite row values";
            return false;
        }
    }
    for (size_t i = 1080; i < 1096; ++i) {
        if (!std::isfinite(words[i])) {
            if (outReason) *outReason = "non-finite block values";
            return false;
        }
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        double length = 0;
        for (size_t row = 0; row < 3; ++row) {
            const double value = words[932 + row * 4 + axis];
            length += value * value;
        }
        if (length < 0.98 || length > 1.02) {
            if (outReason) *outReason = "row axis length out of range";
            return false;
        }
        for (size_t other = 0; other < axis; ++other) {
            double dot = 0;
            for (size_t row = 0; row < 3; ++row)
                dot += static_cast<double>(words[932 + row * 4 + axis]) *
                    words[932 + row * 4 + other];
            if (std::fabs(dot) > 0.02) {
                if (outReason) *outReason = "row axes not orthogonal";
                return false;
            }
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
    if (!(scaleX > 0 && scaleY > 0 && words[1094] > 0 && words[1094] <= 1)) {
        if (outReason) *outReason = "invalid scale or depthB";
        return false;
    }

    for (size_t row = 0; row < 3; ++row) {
        const size_t c = 932 + row * 4, p = 1080 + row * 4;
        if (std::fabs(words[p] / scaleX - words[c]) > 0.06) {
            if (outReason) *outReason = "proj vs row X mismatch";
            return false;
        }
        if (std::fabs(words[p + 1] / scaleY - words[c + 1]) > 0.06) {
            if (outReason) *outReason = "proj vs row Y mismatch";
            return false;
        }
        if (std::fabs(words[p + 2]) > 0.002) {
            if (outReason) *outReason = "proj Z non-zero";
            return false;
        }
        if (std::fabs(words[p + 3] - words[c + 2]) > 0.06) {
            if (outReason) *outReason = "proj vs row Z mismatch";
            return false;
        }
        camera->worldFromView[row * 4] = static_cast<float>(words[p] / scaleX);
        camera->worldFromView[row * 4 + 1] = static_cast<float>(words[p + 1] / scaleY);
        camera->worldFromView[row * 4 + 2] = words[p + 3];
        camera->worldFromView[row * 4 + 3] = words[c + 3];
    }
    if (std::fabs(words[1092]) > 0.002 || std::fabs(words[1093]) > 0.002 ||
        std::fabs(words[1095]) > 0.002) {
        if (outReason) *outReason = "block zero fields non-zero";
        return false;
    }

    camera->scaleX = static_cast<float>(scaleX);
    camera->scaleY = static_cast<float>(scaleY);
    camera->depthB = words[1094];
    return true;
}

}
