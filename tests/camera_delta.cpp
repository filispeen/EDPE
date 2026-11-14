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

    // 14. 4x IPD probe (0.2728m lateral jump) MUST be identified as auxiliary probe:
    auto quadEyeProbe = identity();
    quadEyeProbe.worldFromView[3] = 0.2728f;
    if (!edpe::isStereoEyeOrIpdProbe(quadEyeProbe, identity())) return 32;
    if (!edpe::isReflectionOrAuxiliaryCamera(quadEyeProbe, identity())) return 33;

    // 15. Stereo probe (0.1364m) with micro-FOV jitter (0.07% scaleX diff, inside old 1e-4..2% blind spot)
    // MUST still be caught as an auxiliary stereo probe:
    auto microFovProbe = identity();
    microFovProbe.worldFromView[3] = 0.1364f;
    microFovProbe.scaleX *= 1.0007f;
    if (!edpe::isStereoEyeOrIpdProbe(microFovProbe, identity())) return 34;
    if (!edpe::isReflectionOrAuxiliaryCamera(microFovProbe, identity())) return 35;

    // 16. Measured Cobra Odyssey cockpit head / canopy probe (0.1364m: dUp=0.1098m, dFwd=0.0809m, dRight=0.0000)
    // MUST be identified as auxiliary probe:
    auto cockpitHeadProbe = identity();
    cockpitHeadProbe.worldFromView[7] = 0.1098f;  // Up
    cockpitHeadProbe.worldFromView[11] = 0.0809f; // Forward
    if (!edpe::isStereoEyeOrIpdProbe(cockpitHeadProbe, identity())) return 36;
    if (!edpe::isReflectionOrAuxiliaryCamera(cockpitHeadProbe, identity())) return 37;

    // 17. matchesAnchorPrediction unit tests:
    const edpe::Vec3 anchorVel{0.05f, 0.0f, 0.0f};
    auto matchingCand = identity();
    matchingCand.worldFromView[3] = 0.05f;
    if (!edpe::matchesAnchorPrediction(matchingCand, identity(), anchorVel, 1.0f)) return 38;
    if (edpe::matchesAnchorPrediction(cockpitHeadProbe, identity(), anchorVel, 1.0f)) return 39;

    // 18. 180-deg rear-facing cockpit mirror probe (0.1364m, ~179.2 deg yaw):
    auto rearMirrorProbe = yawed(179.2f);
    rearMirrorProbe.worldFromView[7] = 0.1098f;
    rearMirrorProbe.worldFromView[11] = 0.0809f;
    if (!edpe::isRearOrAuxiliaryProbe(rearMirrorProbe, identity())) return 40;
    if (!edpe::isStereoEyeOrIpdProbe(rearMirrorProbe, identity())) return 41;
    if (!edpe::isReflectionOrAuxiliaryCamera(rearMirrorProbe, identity())) return 42;

    // 19. Pure 180-deg inverted view at same location (rotTrace ~ -1.0, dist = 0):
    auto invertedView = yawed(180.0f);
    if (!edpe::isRearOrAuxiliaryProbe(invertedView, identity())) return 43;
    if (!edpe::isStereoEyeOrIpdProbe(invertedView, identity())) return 44;

    // 20. Real 180-deg ship turnaround at 50m distance (MUST NOT be identified as rear/aux probe):
    auto shipTurnaround = yawed(180.0f);
    shipTurnaround.worldFromView[11] = 50.0f;
    if (edpe::isRearOrAuxiliaryProbe(shipTurnaround, identity())) return 45;

    // 21. On-foot hangar forward & diagonal run at IPD-matching speeds (0.0682m and 0.1364m/frame, rotTrace=3.0)
    // MUST NOT be rejected as rear/aux or stereo probes:
    for (float step : {0.0682f, 0.1364f}) {
        auto runForward = identity();
        runForward.worldFromView[11] = step;
        if (edpe::isRearOrAuxiliaryProbe(runForward, identity())) return 46;
        if (edpe::isStereoEyeOrIpdProbe(runForward, identity())) return 47;
        if (edpe::isReflectionOrAuxiliaryCamera(runForward, identity())) return 48;

        const float diagComponent = step * 0.70710678f;
        auto runDiagonal = identity();
        runDiagonal.worldFromView[3] = diagComponent;
        runDiagonal.worldFromView[11] = diagComponent;
        if (edpe::isRearOrAuxiliaryProbe(runDiagonal, identity())) return 49;
        if (edpe::isStereoEyeOrIpdProbe(runDiagonal, identity())) return 50;
        if (edpe::isReflectionOrAuxiliaryCamera(runDiagonal, identity())) return 51;
    }

    // 22. On-foot hangar strafe start (0.088m/frame from rest) and direction reversal (+0.088 -> -0.088)
    // MUST NOT be rejected as stereo/auxiliary probes:
    auto strafeStart = identity();
    strafeStart.worldFromView[3] = 0.088f;
    if (edpe::isStereoEyeOrIpdProbe(strafeStart, identity())) return 52;
    if (edpe::isReflectionOrAuxiliaryCamera(strafeStart, identity())) return 53;

    const edpe::Vec3 prevStrafeRight{0.088f, 0.0f, 0.0f};
    auto strafeReverse = identity();
    strafeReverse.worldFromView[3] = -0.088f;
    if (edpe::isStereoEyeOrIpdProbe(strafeReverse, identity(), prevStrafeRight)) return 54;
    if (edpe::isReflectionOrAuxiliaryCamera(strafeReverse, identity(), prevStrafeRight)) return 55;

    return 0;
}
