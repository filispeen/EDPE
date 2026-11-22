#pragma once

#include "temporal_math.h"

#include <algorithm>
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

inline bool hasMatchingEliteCb1CameraPosition(const float* words, size_t count,
    const float* cam932) {
    if (!words || count != 5376 / sizeof(float) || !cam932) return false;
    float cam241[4]{};
    for (size_t i = 0; i < 4; ++i) {
        cam241[i] = words[964 + i];
        if (!std::isfinite(cam241[i])) return false;
    }
    if (std::fabs(cam241[3]) > 0.002f) return false;
    if (std::fabs(cam241[0]) <= 1e-4f && std::fabs(cam241[1]) <= 1e-4f &&
        std::fabs(cam241[2]) <= 1e-4f) {
        return false;
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        const float pos = cam932[axis * 4 + 3];
        if (!std::isfinite(pos)) return false;
        const float tol = std::max(1e-2f, 1e-4f * std::fabs(pos));
        if (std::fabs(cam241[axis] - pos) > tol) return false;
    }
    return true;
}

// Extracts the vertex-shader scene render origin from cb1[275].xyz (words 1100..1102).
// Cobra's geometry shaders subtract cb1[275].xyz before multiplying by cb1[270..273],
// so this origin is always synchronous with proj1080 even when cam932 lags during motion.
inline bool extractEliteRenderOrigin(const float* origin1100,
    float* outX, float* outY, float* outZ) {
    if (!origin1100 || !outX || !outY || !outZ) return false;
    const float x = origin1100[0];
    const float y = origin1100[1];
    const float z = origin1100[2];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    if (std::fabs(x) <= 1e-6f && std::fabs(y) <= 1e-6f && std::fabs(z) <= 1e-6f) {
        return false;
    }
    *outX = x;
    *outY = y;
    *outZ = z;
    return true;
}

inline bool parseEliteCameraWithCb1Fallback(const float* cam932, const float* proj1080,
    const float* words, size_t count, CameraProjection* camera, int* failReason = nullptr,
    const float* origin1100 = nullptr) {
    auto applyRenderOrigin = [&]() {
        const float* originSrc = origin1100
            ? origin1100
            : ((words && count == 5376 / sizeof(float)) ? &words[1100] : nullptr);
        float ox = 0.0f, oy = 0.0f, oz = 0.0f;
        if (extractEliteRenderOrigin(originSrc, &ox, &oy, &oz)) {
            camera->worldFromView[3] = ox;
            camera->worldFromView[7] = oy;
            camera->worldFromView[11] = oz;
        }
    };
    int primaryReason = 0;
    if (parseEliteCameraBlocks(cam932, proj1080, camera, &primaryReason)) {
        applyRenderOrigin();
        if (failReason) *failReason = 0;
        return true;
    }
    // On planetary surfaces, Cobra stores the local-horizon/cubemap basis in cam932's
    // 3x3 rotation while keeping the true camera world position in cam932[3,7,11] (and
    // cb1[241].xyz) and the active pass's view rotation rows in cb1[277..279] (words 1108..1119).
    if ((primaryReason == 7 || primaryReason == 8 || primaryReason == 10) &&
        hasMatchingEliteCb1CameraPosition(words, count, cam932)) {
        float rot277[12]{};
        bool finite277 = true;
        for (size_t i = 0; i < 12; ++i) {
            rot277[i] = words[1108 + i];
            if (!std::isfinite(rot277[i])) {
                finite277 = false;
                break;
            }
        }
        if (finite277 &&
            std::fabs(rot277[3]) <= 0.002f &&
            std::fabs(rot277[7]) <= 0.002f &&
            std::fabs(rot277[11]) <= 0.002f) {
            const float camFrom277[12] = {
                rot277[0], rot277[4], rot277[8], cam932[3],
                rot277[1], rot277[5], rot277[9], cam932[7],
                rot277[2], rot277[6], rot277[10], cam932[11]
            };
            if (parseEliteCameraBlocks(camFrom277, proj1080, camera, nullptr)) {
                applyRenderOrigin();
                if (failReason) *failReason = 0;
                return true;
            }
        }
    }
    if (failReason) *failReason = primaryReason;
    return false;
}

inline bool parseEliteCamera(const float* words, size_t count, CameraProjection* camera,
    int* failReason = nullptr) {
    if (!words || count != 5376 / sizeof(float) || !camera) {
        if (failReason) *failReason = 1;
        return false;
    }
    return parseEliteCameraWithCb1Fallback(
        &words[932], &words[1080], words, count, camera, failReason);
}

// Extracts the Main World Camera inverse projection scales from cb1[35..38] (words 140..155):
// |cb1[35].xyz| = 1/world_scaleX, |cb1[36].xyz| = 1/world_scaleY,
// cb1[37] = (0, 0, 0, 1/world_depthB), |cb1[38].xyz| = 1.
inline bool extractEliteWorldProjectionScalesFromBlock16(const float* block16,
    float* outWorldScaleX, float* outWorldScaleY, float* outWorldDepthB) {
    if (!block16 || !outWorldScaleX || !outWorldScaleY || !outWorldDepthB) return false;
    for (size_t i = 0; i < 16; ++i) {
        if (!std::isfinite(block16[i])) return false;
    }
    if (std::fabs(block16[3]) > 0.002f || std::fabs(block16[7]) > 0.002f ||
        std::fabs(block16[8]) > 0.002f || std::fabs(block16[9]) > 0.002f ||
        std::fabs(block16[10]) > 0.002f || std::fabs(block16[15]) > 0.002f) {
        return false;
    }
    if (!(block16[11] >= 4.0f && block16[11] <= 100.0f)) return false;

    double len0Sq = 0.0, len1Sq = 0.0, len2Sq = 0.0;
    double dot01 = 0.0, dot02 = 0.0, dot12 = 0.0;
    for (size_t i = 0; i < 3; ++i) {
        const double v0 = block16[i];
        const double v1 = block16[4 + i];
        const double v2 = block16[12 + i];
        len0Sq += v0 * v0;
        len1Sq += v1 * v1;
        len2Sq += v2 * v2;
        dot01 += v0 * v1;
        dot02 += v0 * v2;
        dot12 += v1 * v2;
    }
    if (len2Sq < 0.98 || len2Sq > 1.02) return false;
    const double len0 = std::sqrt(len0Sq);
    const double len1 = std::sqrt(len1Sq);
    if (len0 < 0.05 || len0 > 10.0 || len1 < 0.05 || len1 > 10.0) return false;
    if (std::fabs(dot01) > 0.02 * len0 * len1 ||
        std::fabs(dot02) > 0.02 * len0 ||
        std::fabs(dot12) > 0.02 * len1) {
        return false;
    }

    *outWorldScaleX = static_cast<float>(1.0 / len0);
    *outWorldScaleY = static_cast<float>(1.0 / len1);
    *outWorldDepthB = 1.0f / block16[11];
    return true;
}

inline bool extractEliteWorldProjectionScales(const float* words, size_t count,
    float* outWorldScaleX, float* outWorldScaleY, float* outWorldDepthB) {
    if (!words || count != 5376 / sizeof(float)) return false;
    float local16[16]{};
    for (size_t i = 0; i < 16; ++i) local16[i] = words[140 + i];
    return extractEliteWorldProjectionScalesFromBlock16(
        local16, outWorldScaleX, outWorldScaleY, outWorldDepthB);
}

inline float eliteWorldBlock16RotationTrace(const float* block16,
    float worldScaleX, float worldScaleY, const CameraProjection& parsedCam) {
    double trace = 0.0;
    for (size_t row = 0; row < 3; ++row) {
        trace += (static_cast<double>(block16[row]) * worldScaleX) * parsedCam.worldFromView[row * 4 + 0];
        trace += (static_cast<double>(block16[4 + row]) * worldScaleY) * parsedCam.worldFromView[row * 4 + 1];
        trace += static_cast<double>(block16[12 + row]) * parsedCam.worldFromView[row * 4 + 2];
    }
    return static_cast<float>(trace);
}

inline bool matchesEliteWorldProjectionScalesFromBlock16(const float* block16,
    const CameraProjection& parsedCam) {
    float wsx = 0.0f, wsy = 0.0f, wdb = 0.0f;
    if (!extractEliteWorldProjectionScalesFromBlock16(block16, &wsx, &wsy, &wdb)) {
        return false;
    }
    if (!(parsedCam.scaleX > 0.0f && parsedCam.scaleY > 0.0f)) return false;
    if (eliteWorldBlock16RotationTrace(block16, wsx, wsy, parsedCam) < 2.50f) {
        return false;
    }
    return std::fabs(parsedCam.scaleX - wsx) <= 0.02f * wsx &&
           std::fabs(parsedCam.scaleY - wsy) <= 0.02f * wsy &&
           std::fabs(parsedCam.depthB - wdb) <= 1e-3f;
}

inline bool matchesEliteWorldProjectionScales(const float* words, size_t count,
    const CameraProjection& parsedCam) {
    if (!words || count != 5376 / sizeof(float)) return false;
    float local16[16]{};
    for (size_t i = 0; i < 16; ++i) local16[i] = words[140 + i];
    return matchesEliteWorldProjectionScalesFromBlock16(local16, parsedCam);
}

inline bool isEliteSubpassVersusWorldBlock16(const float* block16,
    const CameraProjection& parsedCam) {
    float wsx = 0.0f, wsy = 0.0f, wdb = 0.0f;
    if (!extractEliteWorldProjectionScalesFromBlock16(block16, &wsx, &wsy, &wdb)) {
        return false;
    }
    if (!(parsedCam.scaleX > 0.0f && parsedCam.scaleY > 0.0f) ||
        parsedCam.depthB < 0.01f || parsedCam.depthB > 0.25f) {
        return false;
    }
    // A viewmodel/ADS sub-pass shares the active world camera orientation (trace ~3.0).
    // On System Map / Galaxy Map, Cobra leaves the stale space/cockpit camera in cb1[35..38]
    // with an unrelated orientation (e.g. trace = -0.965, ~133 deg apart).
    if (eliteWorldBlock16RotationTrace(block16, wsx, wsy, parsedCam) < 2.50f) {
        return false;
    }
    const float worldAspect = wsy / wsx;
    const float candAspect = parsedCam.scaleY / parsedCam.scaleX;
    if (std::fabs(candAspect - worldAspect) > 0.05f * worldAspect) return false;
    return std::fabs(parsedCam.scaleX - wsx) > 0.02f * wsx ||
           std::fabs(parsedCam.scaleY - wsy) > 0.02f * wsy ||
           std::fabs(parsedCam.depthB - wdb) > 1e-3f;
}

inline bool isEliteSubpassVersusWorldCb1(const float* words, size_t count,
    const CameraProjection& parsedCam) {
    if (!words || count != 5376 / sizeof(float)) return false;
    float local16[16]{};
    for (size_t i = 0; i < 16; ++i) local16[i] = words[140 + i];
    return isEliteSubpassVersusWorldBlock16(local16, parsedCam);
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

// Returns true when cb0[4..7] carries large world-origin translation terms (e.g. Galaxy Map
// absolute ~50,000 LY coords where cb0[4..5].w ~ 56k..80k). Injecting subpixel jitter into
// float32 cb0[4..7] then causes 1/256..1/128 clip quantization (±0.5..2.0 px jitter at w=5..20).
inline bool hasLargeEliteTransposeTranslation(const float* block16, float threshold = 1024.0f) {
    if (!block16) return false;
    return std::fabs(block16[3]) > threshold ||
           std::fabs(block16[7]) > threshold ||
           std::fabs(block16[15]) > threshold;
}

} // namespace edpe
