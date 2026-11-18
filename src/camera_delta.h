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

// 2nd-order kinematic residual filter (Jerk/Impulse Filter) for Cobra stereo eye / IPD probes.
// Eliminates the FOV blind spot (1e-4f..2%) and prevents velocity poisoning rebounds.
inline bool matchesAnchorPrediction(
    const CameraProjection& cand,
    const CameraProjection& anchor,
    const Vec3& velocity,
    float dtRatio)
{
    if (anchor.scaleX <= 0.0f || anchor.scaleY <= 0.0f) {
        return false;
    }
    if (cameraRotationTrace(cand, anchor) < 2.985f) {
        return false;
    }
    if (std::fabs(cand.scaleX - anchor.scaleX) > 0.02f * anchor.scaleX) {
        return false;
    }
    const float r = std::clamp(dtRatio, 0.25f, 4.0f);
    const Vec3 predicted = cameraPosition(anchor) + velocity * r;
    return length(cameraPosition(cand) - predicted) < 0.045f;
}

inline bool isIpdHarmonicDistance(float dist) {
    return std::fabs(dist - 0.0682f) < 0.010f ||
           std::fabs(dist - 0.1364f) < 0.012f ||
           std::fabs(dist - 0.2728f) < 0.015f;
}

inline bool isRearOrAuxiliaryProbe(
    const CameraProjection& candidate,
    const CameraProjection& baseline,
    const Vec3& last_velocity = {0.0f, 0.0f, 0.0f},
    float dtRatio = 1.0f)
{
    (void)last_velocity;
    (void)dtRatio;
    if (baseline.scaleX <= 0.0f || baseline.scaleY <= 0.0f) {
        return false;
    }

    const Vec3 candPos = cameraPosition(candidate);
    const Vec3 prevPos = cameraPosition(baseline);
    const float rawDist = length(candPos - prevPos);
    const float rotTrace = cameraRotationTrace(candidate, baseline);

    // Дзеркальний зонд заднього огляду / ліхтаря кабіни або 180-град інвертований зонд:
    // Розгортання ~180 град (rotTrace < -0.5f при rawDist < 1.5м, або rotTrace < 0.5f зі зміщенням 0.05м..1.5м).
    if ((rotTrace < -0.5f && rawDist < 1.5f) ||
        (rotTrace < 0.5f && rawDist >= 0.05f && rawDist < 1.5f)) {
        return true;
    }

    return false;
}

inline bool matchesIpdOrCockpitProbeDisplacement(
    const Vec3& delta,
    float dist,
    const Vec3& right,
    const Vec3& up,
    const Vec3& fwd)
{
    if (!isIpdHarmonicDistance(dist)) {
        return false;
    }
    const float dRight = std::fabs(dot(delta, right));
    const float dUp    = std::fabs(dot(delta, up));
    const float dFwd   = std::fabs(dot(delta, fwd));

    // Латеральний стерео-очний зонд (IPD 68.2 мм, 136.4 мм, 272.8 мм)
    const bool isLateralIpd = (dRight >= 0.96f * dist && dUp < 0.015f && dFwd < 0.015f);

    // Кабінний / наголовний зонд Cobra Odyssey (136.4 мм: +10.98 см Up, +8.09 см Fwd у системі корабля):
    // При повороті/підйомі голови пілота проєкція на head-Right та head-Fwd змінюється, тоді як вертикальна
    // складова (head-Up >= 0.40 * dist) зберігається у всьому діапазоні огляду в кабіні (yaw ±180°, pitch -30°..+65°).
    const bool isCockpitHeadProbe = (dUp >= 0.40f * dist);

    return isLateralIpd || isCockpitHeadProbe;
}

inline bool isStereoEyeOrIpdProbe(
    const CameraProjection& candidate,
    const CameraProjection& baseline,
    const Vec3& last_velocity = {0.0f, 0.0f, 0.0f},
    float dtRatio = 1.0f)
{
    if (baseline.scaleX <= 0.0f || baseline.scaleY <= 0.0f) {
        return false;
    }

    // 0. Зонд заднього огляду або IPD-гармоніка (відсікає 180° інвертований зонд)
    if (isRearOrAuxiliaryProbe(candidate, baseline, last_velocity, dtRatio)) {
        return true;
    }

    // 1. Стерео-очний зонд у межах кадру має близьку орієнтацію (rotTrace > 2.995f, ~4° допуск під час обертання голови).
    const float rotTrace = cameraRotationTrace(candidate, baseline);
    if (rotTrace <= 2.995f) {
        return false;
    }

    // 2. УСУНЕННЯ СЛІПОЇ ЗОНИ FOV: узгоджуємо поріг із isReflectionOrAuxiliaryCamera (2%).
    // Будь-яке мікро-тремтіння scaleX/scaleY стерео-зонда (< 2%) тепер гарантовано перевіряється тут.
    if (std::fabs(candidate.scaleX - baseline.scaleX) > 0.02f * baseline.scaleX ||
        std::fabs(candidate.scaleY - baseline.scaleY) > 0.02f * baseline.scaleY) {
        return false;
    }

    const Vec3 prevPos = cameraPosition(baseline);
    const Vec3 candPos = cameraPosition(candidate);
    const Vec3 right   = cameraRight(candidate);
    const Vec3 up      = cameraUp(candidate);
    const Vec3 fwd     = cameraForward(candidate);

    // 3. Перевірка 1: Прямий латеральний/наголовний IPD-зсув відносно baseline (1x 68.2 мм, 2x 136.4 мм, 4x 272.8 мм).
    // Спрацьовує лише при стаціонарній камері або при внутрішньокадровому порівнянні пасів (last_velocity == 0).
    const Vec3 rawDelta = candPos - prevPos;
    const float rawDist = length(rawDelta);
    if (length(last_velocity) < 0.025f &&
        matchesIpdOrCockpitProbeDisplacement(rawDelta, rawDist, right, up, fwd)) {
        return true;
    }

    // 4. Перевірка 2: Кінематична нев'язка 2-го порядку (під час стрейфу Q/E та Supercruise).
    const float clampedDtRatio = std::clamp(dtRatio, 0.25f, 4.0f);
    const Vec3 predictedPos = prevPos + (last_velocity * clampedDtRatio);
    const Vec3 residual = candPos - predictedPos;
    const float resDist = length(residual);

    return matchesIpdOrCockpitProbeDisplacement(residual, resDist, right, up, fwd);
}

inline bool isReflectionOrAuxiliaryCamera(
    const CameraProjection& candidate,
    const CameraProjection& baseline,
    const Vec3& last_velocity = {0.0f, 0.0f, 0.0f},
    float dtRatio = 1.0f)
{
    // 1. Хіральність та ортонормованість (відсікає планарні дзеркала ліхтаря кабіни det < 0)
    const float det = cameraRotationDeterminant(candidate);
    if (det < 0.85f || det > 1.15f) return true;

    if (baseline.scaleX <= 0.0f || baseline.scaleY <= 0.0f) return false;

    // 2. Стрибок масштабу проєкції (FOV > 2%) відносно базової камери
    const float scaleDiffX = std::fabs(candidate.scaleX - baseline.scaleX);
    const float scaleDiffY = std::fabs(candidate.scaleY - baseline.scaleY);
    if (scaleDiffX > 0.02f * baseline.scaleX || scaleDiffY > 0.02f * baseline.scaleY) {
        return true;
    }

    // 3. Стерео-зонд (68.2 мм / 136.4 мм / 272.8 мм)
    if (isStereoEyeOrIpdProbe(candidate, baseline, last_velocity, dtRatio)) {
        return true;
    }

    // 4. Однокадровий стрибок орієнтації (> 45 град, rotTrace < 2.40)
    const float rotTrace = cameraRotationTrace(candidate, baseline);
    if (rotTrace < 2.40f) {
        return true;
    }

    return false;
}

// Preserves a continuous eye position while adopting an updated orientation/projection
// (used when a frame carries only a single-frame ~1 Hz IPD/cockpit probe translation offset).
inline CameraProjection preservePositionWithOrientation(
    const CameraProjection& orientationSource,
    const Vec3& continuousPosition)
{
    CameraProjection out = orientationSource;
    cameraSetPosition(out, continuousPosition);
    return out;
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

// Tracks per-frame jitter decisions for same-pose scene sub-passes (e.g. viewmodel/near-plane
// passes whose near plane/FOV differ from the main world camera) so Z-prepass, G-buffer, and
// post-deferred passes sharing that projection always receive identical jitter decisions.
struct SceneSubpassEntry {
    float scaleX = 0.0f;
    float scaleY = 0.0f;
    float depthB = 0.0f;
    bool jittered = false;
};

class SceneSubpassTracker {
public:
    static constexpr size_t kMaxSubpasses = 4;

    void reset() {
        frame_ = ~0ull;
        count_ = 0;
    }

    void beginFrame(unsigned long long frame) {
        if (frame_ != frame) {
            frame_ = frame;
            count_ = 0;
        }
    }

    const SceneSubpassEntry* find(unsigned long long frame, float scaleX, float scaleY, float depthB) const {
        if (frame_ != frame) return nullptr;
        for (size_t i = 0; i < count_; ++i) {
            const auto& e = entries_[i];
            if (std::fabs(e.depthB - depthB) <= 1e-3f &&
                std::fabs(e.scaleX - scaleX) <= 0.01f * e.scaleX &&
                std::fabs(e.scaleY - scaleY) <= 0.01f * e.scaleY) {
                return &e;
            }
        }
        return nullptr;
    }

    const SceneSubpassEntry* findJittered(unsigned long long frame, float scaleX, float scaleY, float depthB) const {
        const auto* e = find(frame, scaleX, scaleY, depthB);
        return (e && e->jittered) ? e : nullptr;
    }

    const SceneSubpassEntry* findJitteredByDepthB(unsigned long long frame, float depthB) const {
        if (frame_ != frame) return nullptr;
        for (size_t i = 0; i < count_; ++i) {
            const auto& e = entries_[i];
            if (e.jittered && std::fabs(e.depthB - depthB) <= 1e-3f) {
                return &e;
            }
        }
        return nullptr;
    }

    bool recordOrGet(unsigned long long frame, float scaleX, float scaleY, float depthB, bool proposedJitter) {
        beginFrame(frame);
        if (const auto* existing = find(frame, scaleX, scaleY, depthB)) {
            return existing->jittered;
        }
        if (count_ < kMaxSubpasses) {
            entries_[count_++] = {scaleX, scaleY, depthB, proposedJitter};
        }
        return proposedJitter;
    }

    size_t count() const { return count_; }
    size_t countForFrame(unsigned long long frame) const { return (frame_ == frame) ? count_ : 0; }
    const SceneSubpassEntry& entry(size_t i) const { return entries_[i]; }

private:
    unsigned long long frame_ = ~0ull;
    SceneSubpassEntry entries_[kMaxSubpasses]{};
    size_t count_ = 0;
};

// Returns true when `candidate` shares the same camera pose/chirality as `ref`
// within a scene-internal sub-pass perspective range (different FOV / near plane
// such as the on-foot viewmodel Z-prepass / G-buffer), so it is never mistaken
// for a camera cut or auxiliary probe.
inline bool isSamePoseSceneSubpass(const CameraProjection& candidate,
                                   const CameraProjection& ref,
                                   float maxIntraDist,
                                   float dtRatio = 1.0f,
                                   const Vec3& last_velocity = {0.0f, 0.0f, 0.0f},
                                   bool knownSubpassFromCb1 = false) {
    if (ref.scaleX <= 0.0f) return false;
    // A scene sub-pass usually differs in near plane (e.g. viewmodel depthB=0.0675 vs world
    // depthB=0.025), or shares world depthB=0.025 during ADS when cb1[35..38] confirms
    // a distinct sub-pass FOV scale (e.g. viewmodel scaleX=3.1901 vs world scaleX=1.2634).
    const bool distinctDepthB = std::fabs(candidate.depthB - ref.depthB) > 1e-3f;
    const bool distinctScale =
        std::fabs(candidate.scaleX - ref.scaleX) > 0.02f * ref.scaleX ||
        std::fabs(candidate.scaleY - ref.scaleY) > 0.02f * ref.scaleY;
    if (!distinctDepthB && !(knownSubpassFromCb1 && distinctScale)) return false;
    const float det = cameraRotationDeterminant(candidate);
    if (det < 0.85f || det > 1.15f) return false;
    if (candidate.depthB < 0.01f || candidate.depthB > 0.25f) return false;
    const Vec3 candPos = cameraPosition(candidate);
    const Vec3 refPos = cameraPosition(ref);
    const float rawDist = length(candPos - refPos);
    const float clampedDtRatio = std::clamp(dtRatio, 0.25f, 4.0f);
    const float predDist = length(candPos - (refPos + last_velocity * clampedDtRatio));
    if (std::min(rawDist, predDist) > maxIntraDist) return false;
    const float trace = cameraRotationTrace(candidate, ref);
    if (trace <= 2.95f) return false;
    return !isRearOrAuxiliaryProbe(candidate, ref, last_velocity, dtRatio);
}

} // namespace edpe
