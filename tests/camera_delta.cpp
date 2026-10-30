#include "camera_delta.h"

#include <cmath>

namespace {
edpe::CameraProjection identity() {
    return {{1, 0, 0, 0,
             0, 1, 0, 0,
             0, 0, 1, 0}, 0.974278629f, 1.73205101f, 0.025f};
}

edpe::CameraProjection yawed(float degrees) {
    auto camera = identity();
    const float a = degrees * 3.14159265f / 180.0f;
    camera.worldFromView[0] = camera.worldFromView[10] = std::cos(a);
    camera.worldFromView[2] = std::sin(a);
    camera.worldFromView[8] = -std::sin(a);
    return camera;
}
} // namespace

int main() {
    // Pure translation: distance only.
    auto moved = identity();
    moved.worldFromView[3] = 3;
    moved.worldFromView[11] = 4;
    auto d = edpe::cameraDelta(identity(), moved);
    if (std::fabs(d.translation - 5) > 1e-6 || d.rotationDeg > 1e-3 || d.projection != 0) return 1;

    // Pure yaw: angle only, either order.
    d = edpe::cameraDelta(identity(), yawed(10));
    if (std::fabs(d.rotationDeg - 10) > 1e-3 || d.translation != 0) return 2;
    d = edpe::cameraDelta(yawed(10), yawed(-5));
    if (std::fabs(d.rotationDeg - 15) > 1e-3) return 3;

    // FOV change shows as projection change.
    auto zoomed = identity();
    zoomed.scaleY = 2.0f;
    if (edpe::cameraDelta(identity(), zoomed).projection < 0.2) return 4;

    edpe::CameraDeltaTracker tracker;
    // First sample: nothing to compare, no flags.
    auto r = tracker.feed(10, true, identity());
    if (r.flags || r.haveDelta || r.logEvent) return 5;
    // Smooth motion below thresholds.
    r = tracker.feed(11, true, yawed(0.5f));
    if (r.flags || !r.haveDelta || r.gap != 1) return 6;
    // Skipped frames and a large turn.
    r = tracker.feed(14, true, yawed(30));
    if (r.flags != (edpe::kCameraDeltaGap | edpe::kCameraDeltaRotation) || r.gap != 3 || !r.logEvent)
        return 7;
    // Parser rejection after valid, then repeated rejection logs once.
    r = tracker.feed(15, false, {});
    if (r.flags != edpe::kCameraDeltaLost) return 8;
    r = tracker.feed(16, false, {});
    if (r.flags) return 9;
    // Regained, compared against the last valid camera (frame 14), with a jump.
    moved = yawed(30);
    moved.worldFromView[7] = 1000;
    r = tracker.feed(17, true, moved);
    if (r.flags != (edpe::kCameraDeltaRegained | edpe::kCameraDeltaGap | edpe::kCameraDeltaTranslation) ||
        r.gap != 3 || std::fabs(r.delta.translation - 1000) > 1e-3)
        return 10;
    const auto& w = tracker.window();
    if (w.samples != 6 || w.invalid != 2 || w.gaps != 2 || w.deltas != 3 || w.events != 3 ||
        w.firstFrame != 10 || w.lastFrame != 17 || std::fabs(w.maxRotationDeg - 29.5) > 1e-3)
        return 11;

    // Rate limit: at most kEventsPerWindow logged events, rest counted.
    tracker.reset();
    unsigned long long frame = 0;
    unsigned logged = 0;
    bool summary = false;
    for (unsigned i = 0; i < edpe::CameraDeltaTracker::kWindow; ++i) {
        r = tracker.feed(frame, true, yawed(i % 2 ? 20.0f : 0.0f));
        frame += 1;
        logged += r.logEvent;
        summary = r.summary;
    }
    if (logged != edpe::CameraDeltaTracker::kEventsPerWindow || !summary ||
        tracker.window().suppressed != edpe::CameraDeltaTracker::kWindow - 1 - logged)
        return 12;
    tracker.nextWindow();
    if (tracker.window().samples) return 13;
    // History survives the window boundary.
    r = tracker.feed(frame, true, yawed(0));
    if (!r.haveDelta || r.gap != 1) return 14;

    // Unit tests for isReflectionOrAuxiliaryCamera:
    // 1. Stationary identical camera is not auxiliary:
    if (edpe::isReflectionOrAuxiliaryCamera(identity(), identity())) return 15;

    // 2. High-speed straight forward flight (e.g. 5 meters along forward axis) is not auxiliary:
    auto forwardFlight = identity();
    forwardFlight.worldFromView[11] = 5.0f;
    if (edpe::isReflectionOrAuxiliaryCamera(forwardFlight, identity())) return 16;

    // 3. Stereo auxiliary eye / IPD probe (0.1364m lateral jump with identical rotation) MUST be identified as auxiliary:
    auto stereoProbe = identity();
    stereoProbe.worldFromView[3] = 0.1364f;
    if (!edpe::isReflectionOrAuxiliaryCamera(stereoProbe, identity())) return 17;

    // 4. Single eye probe (0.0682m lateral jump with identical rotation) MUST be identified as auxiliary:
    auto singleEyeProbe = identity();
    singleEyeProbe.worldFromView[3] = 0.0682f;
    if (!edpe::isReflectionOrAuxiliaryCamera(singleEyeProbe, identity())) return 18;

    // 5. Large sideways flight (> 0.40m, e.g. 1.0m) is not an IPD probe:
    auto largeSideways = identity();
    largeSideways.worldFromView[3] = 1.0f;
    if (edpe::isReflectionOrAuxiliaryCamera(largeSideways, identity())) return 19;

    // 6. Planar reflection camera (det < 0.5) MUST be identified as auxiliary:
    auto planarReflection = identity();
    planarReflection.worldFromView[0] = -1.0f;
    if (!edpe::isReflectionOrAuxiliaryCamera(planarReflection, identity())) return 20;

    // 7. FOV mismatch MUST be identified as auxiliary:
    if (!edpe::isReflectionOrAuxiliaryCamera(zoomed, identity())) return 21;

    // 8. Sudden orientation jump (> 45 deg) MUST be identified as auxiliary:
    if (!edpe::isReflectionOrAuxiliaryCamera(yawed(50), identity())) return 22;

    // 9. Lateral thruster strafe (Q/E) at 10 m/s with velocity history MUST NOT be identified as auxiliary:
    const edpe::Vec3 strafeVelocity{0.1667f, 0.0f, 0.0f}; // ~10 m/s at 60 FPS
    auto strafing = identity();
    strafing.worldFromView[3] = 0.1667f;
    if (edpe::isReflectionOrAuxiliaryCamera(strafing, identity(), strafeVelocity)) return 23;
    if (edpe::isStereoEyeOrIpdProbe(strafing, identity(), strafeVelocity)) return 24;

    // 10. Stereo auxiliary probe arriving during 10 m/s strafe MUST be identified as auxiliary probe:
    auto strafeWithProbe = identity();
    strafeWithProbe.worldFromView[3] = 0.1667f + 0.0682f;
    if (!edpe::isStereoEyeOrIpdProbe(strafeWithProbe, identity(), strafeVelocity)) return 25;
    if (!edpe::isReflectionOrAuxiliaryCamera(strafeWithProbe, identity(), strafeVelocity)) return 26;

    // 11. Supercruise high-speed forward flight at 50,000 m/s MUST NOT be identified as auxiliary:
    const edpe::Vec3 scVelocity{0.0f, 0.0f, 833.33f};
    auto scFlight = identity();
    scFlight.worldFromView[11] = 833.33f;
    if (edpe::isReflectionOrAuxiliaryCamera(scFlight, identity(), scVelocity)) return 27;
    if (edpe::isStereoEyeOrIpdProbe(scFlight, identity(), scVelocity)) return 28;

    // 12. Supercruise flight with 68.2 mm stereo probe jump MUST be identified as auxiliary probe:
    auto scWithProbe = identity();
    scWithProbe.worldFromView[3] = 0.0682f;
    scWithProbe.worldFromView[11] = 833.33f;
    if (!edpe::isStereoEyeOrIpdProbe(scWithProbe, identity(), scVelocity)) return 29;
    if (!edpe::isReflectionOrAuxiliaryCamera(scWithProbe, identity(), scVelocity)) return 30;

    // 13. Variable framerate (dtRatio = 2.0, e.g. 120 FPS -> 60 FPS) strafe test:
    const edpe::Vec3 halfVelocity{0.0833f, 0.0f, 0.0f};
    auto dtCand = identity();
    dtCand.worldFromView[3] = 0.0833f * 2.0f;
    if (edpe::isReflectionOrAuxiliaryCamera(dtCand, identity(), halfVelocity, 2.0f)) return 31;

    return 0;
}
