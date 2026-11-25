#pragma once

#include "temporal_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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

inline double relativeRotationAngleDeg(const float* p, const float* n) {
    // R_rel = R_prev^T * R_now in double precision; atan2(s, c) avoids acos
    // precision collapse for small camera turns (< 0.02 deg).
    double r[9]{};
    for (unsigned i = 0; i < 3; ++i) {
        for (unsigned j = 0; j < 3; ++j) {
            r[i * 3 + j] = static_cast<double>(p[0 * 4 + i]) * n[0 * 4 + j] +
                           static_cast<double>(p[1 * 4 + i]) * n[1 * 4 + j] +
                           static_cast<double>(p[2 * 4 + i]) * n[2 * 4 + j];
        }
    }
    const double c = 0.5 * (r[0] + r[4] + r[8] - 1.0);
    const double sx = r[7] - r[5];
    const double sy = r[2] - r[6];
    const double sz = r[3] - r[1];
    const double s = 0.5 * std::sqrt(sx * sx + sy * sy + sz * sz);
    return std::atan2(s, c) * 57.29577951308232;
}

inline CameraDelta cameraDelta(const CameraProjection& previous, const CameraProjection& now) {
    const float* p = previous.worldFromView;
    const float* n = now.worldFromView;
    double moved = 0;
    for (unsigned row = 0; row < 3; ++row) {
        const double d = static_cast<double>(n[row * 4 + 3]) - p[row * 4 + 3];
        moved += d * d;
    }
    return {std::sqrt(moved), relativeRotationAngleDeg(p, n),
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

    // Кабінний / наголовний зонд Cobra Odyssey (строго 2x IPD = 136.4 мм: +10.98 см Up, +8.09 см Fwd у системі корабля):
    // Обмежуємо перевірку лише гармонікою 136.4 мм, щоб звичайний рух по схилах/сходах (~68.2 мм/кадр)
    // або вертикальний тяг/джетпак (~272.8 мм/кадр) не класифікувались як зонд.
    const bool isCockpitHeadProbe =
        (std::fabs(dist - 0.1364f) < 0.012f) && (dUp >= 0.40f * dist);

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

// True when near plane changes or projection scale jumps >25%
// (real camera cut rather than smooth FOV / ADS zoom).
inline bool isCameraProjectionCut(
    const CameraProjection& current,
    const CameraProjection& previous)
{
    if (previous.scaleX <= 0.0f || previous.scaleY <= 0.0f) return false;
    return std::fabs(previous.depthB - current.depthB) > 1e-3f ||
           std::fabs(previous.scaleX - current.scaleX) > 0.25f * previous.scaleX ||
           std::fabs(previous.scaleY - current.scaleY) > 0.25f * previous.scaleY;
}

// True when two consecutive committed main-scene cameras represent a true
// discontinuity (chirality flip, >41 deg rotation cut, or projection cut)
// without triggering false resets on smooth FOV zoom or variable dt hitches.
inline bool isCommittedCameraDiscontinuity(
    const CameraProjection& current,
    const CameraProjection& previous)
{
    const float det = cameraRotationDeterminant(current);
    if (det < 0.85f || det > 1.15f) return true;
    if (previous.scaleX <= 0.0f || previous.scaleY <= 0.0f) return false;
    if (isCameraProjectionCut(current, previous)) return true;
    return cameraRotationTrace(current, previous) < 2.414f;
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

// True when a large translation jump inside the main 4-MRT G-buffer has the same
// orientation and projection as the committed camera for <5 consecutive frames
// (external on-foot local-origin rebase that snaps back after 1-4 frames).
inline bool isTransientOriginRebaseHoldEligible(
    const CameraProjection& candidate,
    const CameraProjection& committed,
    float jumpResidual,
    unsigned teleportStreak,
    bool inMainScene,
    bool cutStreakSatisfied,
    float prevSpeed = 0.0f)
{
    if (!inMainScene || cutStreakSatisfied || teleportStreak >= 5) return false;
    if (prevSpeed > 5.0f) return false;
    if (jumpResidual <= 15.0f || jumpResidual >= 2000.0f) return false;
    if (isCameraProjectionCut(candidate, committed)) return false;
    return cameraRotationTrace(candidate, committed) > 2.414f;
}

// Tracks per-frame transient origin-rebase/teleport hold streak with a hard cap of
// 4 consecutive held frames so rejected mid-frame passes cannot wipe the streak to 0.
struct TeleportHoldState {
    static constexpr unsigned kMaxConsecutiveHoldFrames = 4;

    unsigned rejectionStreak = 0;
    unsigned consecutiveHoldFrames = 0;
    uint64_t lastRejectionFrame = ~0ull;

    void noteJumpCandidate(uint64_t frame, bool alreadyStagedInScene) noexcept {
        if (!alreadyStagedInScene && lastRejectionFrame != frame) {
            lastRejectionFrame = frame;
            ++rejectionStreak;
        }
    }

    unsigned effectiveStreak() const noexcept {
        return std::max(rejectionStreak, consecutiveHoldFrames + 1);
    }

    void onStagedWin(bool inMainScene, bool isTeleportHoldCandidate) noexcept {
        if (inMainScene && !isTeleportHoldCandidate) {
            rejectionStreak = 0;
        }
    }

    bool canCommitHold() const noexcept {
        return consecutiveHoldFrames < kMaxConsecutiveHoldFrames;
    }

    void onCommit(bool usedTeleportHold) noexcept {
        if (usedTeleportHold) {
            ++consecutiveHoldFrames;
            rejectionStreak = std::max(rejectionStreak, consecutiveHoldFrames);
        } else {
            rejectionStreak = 0;
            consecutiveHoldFrames = 0;
        }
    }

    void reset() noexcept {
        rejectionStreak = 0;
        consecutiveHoldFrames = 0;
        lastRejectionFrame = ~0ull;
    }
};

// True when a committed translation step exceeds normal camera bounds (>50m)
// or jumps >15m beyond linear velocity prediction (persistent teleport/cut).
inline bool isUnpredictedPositionCut(
    const Vec3& stepDelta,
    const Vec3& predictedDelta)
{
    const float dist = length(stepDelta);
    if (dist > 50.0f) return true;
    if (dist <= 15.0f) return false;
    return length(stepDelta - predictedDelta) > 15.0f;
}

// True when Cobra Engine re-anchors the local floating origin (~5 km rebase):
// large translation jump (>= 50 m) with unchanged projection, near-identical
// orientation (trace >= 2.9996, ~1.15 deg), and the new position pulled closer
// to the local origin than both the previous position and the step length.
inline bool isFloatingOriginReanchor(
    const CameraProjection& prev,
    const CameraProjection& now,
    double* outStepM = nullptr)
{
    const double px = static_cast<double>(prev.worldFromView[3]);
    const double py = static_cast<double>(prev.worldFromView[7]);
    const double pz = static_cast<double>(prev.worldFromView[11]);
    const double nx = static_cast<double>(now.worldFromView[3]);
    const double ny = static_cast<double>(now.worldFromView[7]);
    const double nz = static_cast<double>(now.worldFromView[11]);
    const double dx = nx - px;
    const double dy = ny - py;
    const double dz = nz - pz;
    const double stepM = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (outStepM) *outStepM = stepM;
    if (stepM < 50.0) return false;
    if (isCameraProjectionCut(now, prev)) return false;
    if (cameraRotationTrace(now, prev) < 2.9996f) return false;
    const double prevLen = std::sqrt(px * px + py * py + pz * pz);
    const double nowLen = std::sqrt(nx * nx + ny * ny + nz * nz);
    return nowLen < prevLen && nowLen < stepM;
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
    const float minDist = std::min(rawDist, predDist);
    const float trace = cameraRotationTrace(candidate, ref);
    // Planetary origin-rebased sub-passes (verified by cb1[35..38] world scales,
    // e.g. (-0.30, 0.26, -0.40) vs planetary (-134.89, 331.26, 289.81), same orientation).
    const bool originRebase =
        knownSubpassFromCb1 && rawDist > 15.0f && rawDist < 500.0f && trace > 2.95f;
    if (!originRebase && minDist > maxIntraDist) return false;
    // Allow rapid camera flicks up to ~45 deg/frame (trace > 2.40, matching
    // isAuthoritativeMainSceneCamera) while still rejecting 90/180 deg probes/cubemaps.
    if (trace <= 2.40f) return false;
    return !isRearOrAuxiliaryProbe(candidate, ref, last_velocity, dtRatio);
}

// Validates perspective camera geometry for the main display view:
// finite positive scales, main-scene near-plane range [0.01, 0.25], right-handed
// orthonormal rotation det in [0.85, 1.15], unit-length perspective W column (if proj1080
// is supplied), non-square/non-tiny viewport (if vpW/vpH are supplied), and screen-matching
// aspect ratio (rejecting 1:1 square shadow/spotlight/cubemap cameras on widescreen).
inline bool isMainCameraGeometryValid(
    const CameraProjection& candidate,
    const float* proj1080,
    float vpW,
    float vpH,
    unsigned outW,
    unsigned outH)
{
    if (!(candidate.scaleX > 0.0f && candidate.scaleY > 0.0f)) return false;
    if (candidate.depthB < 0.01f || candidate.depthB > 0.25f) return false;

    const float det = cameraRotationDeterminant(candidate);
    if (det < 0.85f || det > 1.15f) return false;

    if (proj1080) {
        const double w0 = proj1080[3], w1 = proj1080[7], w2 = proj1080[11];
        const double wLenSq = w0 * w0 + w1 * w1 + w2 * w2;
        if (wLenSq < 0.5 || wLenSq > 1.5) return false;
    }

    if (std::isfinite(vpW) && std::isfinite(vpH) && vpW > 0.0f && vpH > 0.0f) {
        if (outW > outH && std::fabs(vpW - vpH) < 1.0f) return false;
        if (vpW < 64.0f || vpH < 64.0f) return false;
    }

    const float camAspect = candidate.scaleY / candidate.scaleX;
    if (outW > outH && std::fabs(camAspect - 1.0f) < 0.03f) return false;
    if (outW > 0 && outH > 0) {
        const float screenAspect = static_cast<float>(outW) / static_cast<float>(outH);
        if (std::fabs(camAspect - screenAspect) > 0.05f * screenAspect) return false;
    }
    return true;
}

// True when a cb1[35..38]-mismatched pass (e.g. during an ADS FOV ramp where cb1[35..38]
// lags proj1080 by >2%) is a smooth world-camera FOV step rather than a viewmodel sub-pass
// (which jumps ~2.5x in scaleX or uses depthB=0.0675) or a distant auxiliary spotlight.
inline bool isSmoothWorldFovTransitionCandidate(
    const CameraProjection& candidate,
    const CameraProjection& committed,
    float worldScaleX,
    float worldDepthB,
    float maxPosDist = 1.0f,
    float dtRatio = 1.0f,
    const Vec3& last_velocity = {0.0f, 0.0f, 0.0f})
{
    if (!(committed.scaleX > 0.0f && committed.scaleY > 0.0f)) return false;
    if (!(candidate.scaleX > 0.0f && candidate.scaleY > 0.0f)) return false;
    if (std::fabs(candidate.depthB - committed.depthB) > 1e-3f) return false;
    if (worldDepthB > 0.0f && std::fabs(candidate.depthB - worldDepthB) > 1e-3f) return false;

    const float det = cameraRotationDeterminant(candidate);
    if (det < 0.85f || det > 1.15f) return false;

    // Require smooth single-frame FOV step relative to committed AND proximity to cb1[35..38]
    // world scale so the ~2.52x ADS viewmodel sub-pass (e.g. 2.374 vs 0.94 or 3.19 vs 1.26)
    // is never promoted as the main world camera.
    const float ratioCommittedX = candidate.scaleX / committed.scaleX;
    const float ratioCommittedY = candidate.scaleY / committed.scaleY;
    if (ratioCommittedX < 0.65f || ratioCommittedX > 1.55f ||
        ratioCommittedY < 0.65f || ratioCommittedY > 1.55f) {
        return false;
    }
    if (worldScaleX > 0.0f) {
        const float ratioWorldX = candidate.scaleX / worldScaleX;
        if (ratioWorldX < 0.65f || ratioWorldX > 1.55f) return false;
    }

    if (cameraRotationTrace(candidate, committed) <= 2.40f) return false;
    if (isRearOrAuxiliaryProbe(candidate, committed, last_velocity, dtRatio)) return false;

    const Vec3 candPos = cameraPosition(candidate);
    const Vec3 prevPos = cameraPosition(committed);
    const float clampedDtRatio = std::clamp(dtRatio, 0.25f, 4.0f);
    const float rawDist = length(candPos - prevPos);
    const float predDist = length(candPos - (prevPos + last_velocity * clampedDtRatio));
    return std::min(rawDist, predDist) <= maxPosDist;
}

} // namespace edpe
