#pragma once

#include "temporal_math.h"

#include <algorithm>
#include <cmath>

namespace edpe {

// Observation only (docs/elite-input-conventions.md step 4). Compares two
// parsed CameraProjection values: row-major view-to-world [R | C], rotation
// columns 0..2 and translation column 3 (see temporal_math.h).
struct CameraDelta {
    double translation;  // |C_now - C_previous| in scene units
    double rotationDeg;  // angle of R_previous^T * R_now
    double projection;   // max |scale change| over X and Y
    double depthB;       // |depthB change|
};

inline CameraDelta cameraDelta(const CameraProjection& previous, const CameraProjection& now) {
    const float* p = previous.worldFromView;
    const float* n = now.worldFromView;
    double moved = 0, trace = 0;
    for (unsigned row = 0; row < 3; ++row) {
        const double d = static_cast<double>(n[row * 4 + 3]) - p[row * 4 + 3];
        moved += d * d;
        for (unsigned column = 0; column < 3; ++column)
            trace += static_cast<double>(p[row * 4 + column]) * n[row * 4 + column];
    }
    const double cosine = std::clamp((trace - 1.0) * 0.5, -1.0, 1.0);
    return {std::sqrt(moved), std::acos(cosine) * 57.29577951308232,
        std::max(std::fabs(static_cast<double>(now.scaleX) - previous.scaleX),
                 std::fabs(static_cast<double>(now.scaleY) - previous.scaleY)),
        std::fabs(static_cast<double>(now.depthB) - previous.depthB)};
}

inline float cameraRotationDeterminant(const CameraProjection& cam) {
    const float* m = cam.worldFromView;
    return m[0] * (m[5] * m[10] - m[6] * m[9])
         - m[1] * (m[4] * m[10] - m[6] * m[8])
         + m[2] * (m[4] * m[9]  - m[5] * m[8]);
}

inline float cameraRotationTrace(const CameraProjection& a, const CameraProjection& b) {
    const float* c = a.worldFromView;
    const float* p = b.worldFromView;
    return (c[0]*p[0] + c[4]*p[4] + c[8]*p[8]) +
           (c[1]*p[1] + c[5]*p[5] + c[9]*p[9]) +
           (c[2]*p[2] + c[6]*p[6] + c[10]*p[10]);
}

inline bool isReflectionOrAuxiliaryCamera(const CameraProjection& candidate, const CameraProjection& baseline) {
    // 1. Planar reflection matrices invert coordinate chirality: det(R) = -1.0.
    const float det = cameraRotationDeterminant(candidate);
    if (det < 0.5f) return true;

    if (baseline.scaleX <= 0.0f || baseline.scaleY <= 0.0f) return false;

    // 2. Projection FOV / scale mismatch against active main camera:
    if (std::fabs(candidate.scaleX - baseline.scaleX) > 0.02f * baseline.scaleX ||
        std::fabs(candidate.scaleY - baseline.scaleY) > 0.02f * baseline.scaleY) {
        return true;
    }

    // 3. Discontinuous single-frame orientation or probe offset relative to active baseline:
    const float* c = candidate.worldFromView;
    const float* b = baseline.worldFromView;
    const float dx = c[3] - b[3];
    const float dy = c[7] - b[7];
    const float dz = c[11] - b[11];
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float rotTrace = cameraRotationTrace(candidate, baseline);

    // Abrupt single-frame orientation jump (> 45 deg, rotTrace < 2.40) at any distance is an auxiliary/probe camera:
    if (rotTrace < 2.40f) {
        return true;
    }

    // Auxiliary mirror / panel / probe camera with combined orientation (> 18 deg) and position jump:
    if (dist > 0.05f && rotTrace < 2.90f) {
        return true;
    }

    // 4. Auxiliary stereo eye / IPD / lateral reflection probe check:
    // Cobra engine periodically evaluates a secondary stereo eye or cockpit probe (~1 Hz).
    // In Odyssey 2D mode, this probe has identical orientation (rotTrace > 2.99) and identical
    // FOV/aspect, but exhibits a discrete lateral eye displacement (e.g. 68.2 mm IPD or 136.4 mm stereo baseline).
    const float dRight   = dx * c[0] + dy * c[4] + dz * c[8];
    const float dUp      = dx * c[1] + dy * c[5] + dz * c[9];
    const float dForward = dx * c[2] + dy * c[6] + dz * c[10];
    const float absRight = std::fabs(dRight);
    const float absUp    = std::fabs(dUp);
    const float absFwd   = std::fabs(dForward);

    // If the displacement matches the stereo eye / IPD baseline range (0.04m .. 0.35m)
    // with overwhelmingly lateral motion (|dRight| >= 80% of total distance),
    // near-zero forward and vertical motion, and identical orientation:
    if (dist >= 0.04f && dist <= 0.35f && absRight >= 0.80f * dist && absUp < 0.04f && absFwd < 0.04f && rotTrace > 2.99f) {
        return true;
    }

    return false;
}

enum CameraDeltaFlag : unsigned {
    kCameraDeltaRotation = 1,     // rotation above the logging threshold
    kCameraDeltaTranslation = 2,  // translation above the logging threshold
    kCameraDeltaProjection = 4,   // scale or depthB changed
    kCameraDeltaGap = 8,          // Present frames skipped since the last valid sample
    kCameraDeltaLost = 16,        // parser rejected a sample after a valid one
    kCameraDeltaRegained = 32,    // first valid sample after rejected ones
};

// Logging thresholds only. They select lines worth reading in step 5; they
// are not reset thresholds for any temporal backend.
struct CameraDeltaThresholds {
    double rotationDeg = 2.0;
    double translation = 50.0;
    double projection = 1e-4;
};

// Feeds one sample per rendered frame and decides what to log. Events are
// rate limited per window; the summary reports how many were suppressed.
class CameraDeltaTracker {
public:
    static constexpr unsigned kWindow = 120;
    static constexpr unsigned kEventsPerWindow = 8;

    struct Result {
        unsigned flags = 0;
        bool logEvent = false;       // flags != 0 and within the rate limit
        bool haveDelta = false;      // delta valid (both samples parsed)
        unsigned long long gap = 0;  // frame - previous valid frame
        CameraDelta delta{};
        bool summary = false;        // window complete; read window() then call nextWindow()
    };

    struct Window {
        unsigned samples = 0, invalid = 0, gaps = 0, events = 0, suppressed = 0;
        double maxTranslation = 0, maxRotationDeg = 0, sumTranslation = 0, sumRotationDeg = 0;
        unsigned deltas = 0;
        unsigned long long firstFrame = 0, lastFrame = 0;
    };

    explicit CameraDeltaTracker(CameraDeltaThresholds thresholds = {}) : thresholds_(thresholds) {}

    Result feed(unsigned long long frame, bool valid, const CameraProjection& camera) {
        Result result;
        if (!window_.samples) window_.firstFrame = frame;
        window_.lastFrame = frame;
        ++window_.samples;
        if (!valid) {
            ++window_.invalid;
            if (have_previous_ && !lost_) result.flags |= kCameraDeltaLost;
            lost_ = true;
        } else {
            if (lost_ && have_previous_) result.flags |= kCameraDeltaRegained;
            lost_ = false;
            if (have_previous_) {
                result.haveDelta = true;
                result.gap = frame - previous_frame_;
                result.delta = cameraDelta(previous_, camera);
                if (result.gap > 1) { result.flags |= kCameraDeltaGap; ++window_.gaps; }
                if (result.delta.rotationDeg > thresholds_.rotationDeg)
                    result.flags |= kCameraDeltaRotation;
                if (result.delta.translation > thresholds_.translation)
                    result.flags |= kCameraDeltaTranslation;
                if (result.delta.projection > thresholds_.projection ||
                    result.delta.depthB > thresholds_.projection)
                    result.flags |= kCameraDeltaProjection;
                ++window_.deltas;
                window_.sumTranslation += result.delta.translation;
                window_.sumRotationDeg += result.delta.rotationDeg;
                window_.maxTranslation = std::max(window_.maxTranslation, result.delta.translation);
                window_.maxRotationDeg = std::max(window_.maxRotationDeg, result.delta.rotationDeg);
            }
            previous_ = camera;
            previous_frame_ = frame;
            have_previous_ = true;
        }
        if (result.flags) {
            if (window_.events < kEventsPerWindow) { ++window_.events; result.logEvent = true; }
            else ++window_.suppressed;
        }
        result.summary = window_.samples >= kWindow;
        return result;
    }

    const Window& window() const { return window_; }
    void nextWindow() { window_ = {}; }
    void reset() { *this = CameraDeltaTracker(thresholds_); }

private:
    CameraDeltaThresholds thresholds_;
    Window window_{};
    CameraProjection previous_{};
    unsigned long long previous_frame_ = 0;
    bool have_previous_ = false;
    bool lost_ = false;
};

} // namespace edpe
