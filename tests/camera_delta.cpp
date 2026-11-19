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

    // 23. SceneSubpassTracker keeps Z-prepass, G-buffer, and post-deferred sub-pass jitter
    // decisions consistent per frame even when Z-prepass runs before the main world camera.
    edpe::SceneSubpassTracker tracker23;
    if (!tracker23.recordOrGet(10, 1.2935f, 2.300f, 0.0675f, true)) return 56;
    if (!tracker23.findJittered(10, 1.2935f, 2.300f, 0.0675f)) return 57;
    if (!tracker23.findJitteredByDepthB(10, 0.0675f)) return 58;
    // Different frame or wrong near plane must not match.
    if (tracker23.findJittered(10, 1.2935f, 2.300f, 0.025f)) return 59;
    // If Z-prepass locked false (e.g. fast flick/cut), G-buffer must also stay false on that frame.
    if (tracker23.recordOrGet(11, 1.2935f, 2.300f, 0.0675f, false)) return 60;
    if (tracker23.recordOrGet(11, 1.2935f, 2.300f, 0.0675f, true)) return 61;
    if (tracker23.findJittered(11, 1.2935f, 2.300f, 0.0675f)) return 62;
    // Next frame starts fresh and can accept again.
    if (!tracker23.recordOrGet(12, 1.2935f, 2.300f, 0.0675f, true)) return 63;

    // 24. isSamePoseSceneSubpass identifies a same-pose viewmodel sub-pass before world staging
    // so it is not misclassified as a camera cut, while still rejecting true cuts or mirrors.
    edpe::CameraProjection worldBase = identity();
    worldBase.scaleX = 1.0525f;
    worldBase.scaleY = 1.871f;
    worldBase.depthB = 0.025f;
    edpe::CameraProjection viewmodelSubpass = worldBase;
    viewmodelSubpass.scaleX = 1.2935f;
    viewmodelSubpass.scaleY = 2.300f;
    viewmodelSubpass.depthB = 0.0675f;
    if (!edpe::isSamePoseSceneSubpass(viewmodelSubpass, worldBase, 0.05f)) return 64;
    // Camera cut (large jump) must NOT be accepted as a same-pose sub-pass.
    edpe::CameraProjection farSubpass = viewmodelSubpass;
    farSubpass.worldFromView[3] = 50.0f;
    if (edpe::isSamePoseSceneSubpass(farSubpass, worldBase, 0.05f)) return 65;
    // Mirrored reflection (negative determinant) must be rejected.
    edpe::CameraProjection mirroredSubpass = viewmodelSubpass;
    mirroredSubpass.worldFromView[0] = -mirroredSubpass.worldFromView[0];
    mirroredSubpass.worldFromView[4] = -mirroredSubpass.worldFromView[4];
    mirroredSubpass.worldFromView[8] = -mirroredSubpass.worldFromView[8];
    if (edpe::isSamePoseSceneSubpass(mirroredSubpass, worldBase, 0.05f)) return 66;
    // Same-depthB camera (e.g. Camera Suite FOV change or world pass with depthB=0.025)
    // must NOT be classified as a scene sub-pass so it never blocks camera-cut latching
    // or poisons SceneSubpassTracker for late forward hair passes (EID 4379 / EID 4553).
    edpe::CameraProjection cameraSuiteFov = worldBase;
    cameraSuiteFov.scaleX = 1.428f;
    cameraSuiteFov.scaleY = 2.539f;
    cameraSuiteFov.depthB = 0.025f;
    if (edpe::isSamePoseSceneSubpass(cameraSuiteFov, worldBase, 15.0f)) return 67;
    // Viewmodel sub-pass before world staging while ship moves (predicted by velocity) must pass.
    edpe::CameraProjection movingViewmodel = viewmodelSubpass;
    movingViewmodel.worldFromView[11] = 2.5f;
    if (!edpe::isSamePoseSceneSubpass(movingViewmodel, worldBase, 0.25f, 1.0f, {0.0f, 0.0f, 2.5f})) return 68;

    // 25. Diagonal forward-right movement (e.g. 0.0506m right, 0.0293m fwd, |d|=0.0585m)
    // MUST NOT be misclassified as a lateral IPD probe:
    auto diagMove = identity();
    diagMove.worldFromView[3] = 0.0506f;
    diagMove.worldFromView[7] = 0.0001f;
    diagMove.worldFromView[11] = 0.0293f;
    if (edpe::isStereoEyeOrIpdProbe(diagMove, identity())) return 67;
    if (edpe::isReflectionOrAuxiliaryCamera(diagMove, identity())) return 68;

    // 26. Ship-frame cockpit head probe (+0.1098m ship-Up, +0.0809m ship-Fwd) MUST still be
    // identified when pilot head is yawed left/right and pitched up (e.g. 20 deg yaw + 20 deg pitch):
    auto pitchedYawedHead = [&]() {
        auto c = identity();
        const float y = 20.0f * 3.14159265f / 180.0f;
        const float p = -20.0f * 3.14159265f / 180.0f; // pitch up
        const float cy = std::cos(y), sy = std::sin(y);
        const float cp = std::cos(p), sp = std::sin(p);
        // R = Ry * Rx
        c.worldFromView[0] = cy;       c.worldFromView[1] = sy * sp; c.worldFromView[2]  = sy * cp;
        c.worldFromView[4] = 0.0f;     c.worldFromView[5] = cp;      c.worldFromView[6]  = -sp;
        c.worldFromView[8] = -sy;      c.worldFromView[9] = cy * sp; c.worldFromView[10] = cy * cp;
        return c;
    };
    auto turnedHeadBase = pitchedYawedHead();
    auto turnedHeadProbe = turnedHeadBase;
    turnedHeadProbe.worldFromView[7] += 0.1098f;
    turnedHeadProbe.worldFromView[11] += 0.0809f;
    if (!edpe::isStereoEyeOrIpdProbe(turnedHeadProbe, turnedHeadBase)) return 69;
    if (!edpe::isReflectionOrAuxiliaryCamera(turnedHeadProbe, turnedHeadBase)) return 70;

    // 27. preservePositionWithOrientation keeps rotated orientation + projection from a panning
    // probe-offset frame while preserving continuous eye position (preventing fallback yaw stalls).
    edpe::CameraProjection rotatedProbe = turnedHeadBase;
    const float panAngle = 0.6f * 3.14159265f / 180.0f;
    const float cp = std::cos(panAngle), sp = std::sin(panAngle);
    for (int row = 0; row < 3; ++row) {
        const float c0 = turnedHeadBase.worldFromView[row * 4 + 0];
        const float c2 = turnedHeadBase.worldFromView[row * 4 + 2];
        rotatedProbe.worldFromView[row * 4 + 0] = c0 * cp + c2 * sp;
        rotatedProbe.worldFromView[row * 4 + 2] = -c0 * sp + c2 * cp;
    }
    rotatedProbe.worldFromView[7] += 0.1098f;
    rotatedProbe.worldFromView[11] += 0.0809f;
    const edpe::Vec3 basePos = edpe::cameraPosition(turnedHeadBase);
    const edpe::CameraProjection recovered = edpe::preservePositionWithOrientation(rotatedProbe, basePos);
    const auto recDelta = edpe::cameraDelta(turnedHeadBase, recovered);
    if (recDelta.translation > 1e-5) return 71;
    if (std::fabs(recDelta.rotationDeg - 0.6) > 0.1) return 72;

    // 28. ADS viewmodel sub-pass shares depthB=0.025 with the ADS world camera
    // (scaleX=3.1901, scaleY=5.6713 vs world scaleX=1.2634, scaleY=2.2460) and is
    // accepted ONLY when knownSubpassFromCb1=true (preserving Camera Suite rejection when false).
    edpe::CameraProjection adsWorld = identity();
    adsWorld.scaleX = 1.2634f;
    adsWorld.scaleY = 2.2460f;
    adsWorld.depthB = 0.025f;
    edpe::CameraProjection adsViewmodel = adsWorld;
    adsViewmodel.scaleX = 3.1901f;
    adsViewmodel.scaleY = 5.6713f;
    adsViewmodel.depthB = 0.025f;
    if (!edpe::isSamePoseSceneSubpass(adsViewmodel, adsWorld, 0.05f, 1.0f, {0.0f, 0.0f, 0.0f}, true)) return 73;
    if (edpe::isSamePoseSceneSubpass(adsViewmodel, adsWorld, 0.05f, 1.0f, {0.0f, 0.0f, 0.0f}, false)) return 74;

    edpe::SceneSubpassTracker adsTracker;
    if (!adsTracker.recordOrGet(20, 1.2935f, 2.300f, 0.0675f, true)) return 75;
    if (!adsTracker.recordOrGet(20, 3.1901f, 5.6713f, 0.025f, true)) return 76;
    if (!adsTracker.findJittered(20, 1.2935f, 2.300f, 0.0675f)) return 77;
    if (!adsTracker.findJittered(20, 3.1901f, 5.6713f, 0.025f)) return 78;
    if (adsTracker.find(20, 1.2634f, 2.2460f, 0.025f)) return 79;

    // 29. Smooth ADS / FOV zoom (>2% but <=25% per frame) MUST NOT trigger
    // isCameraProjectionCut or isCommittedCameraDiscontinuity (so Debug View does
    // not falsely reset history during zoom transitions), while >25% jumps,
    // depthB changes, chirality flips, and >45 deg rotation cuts DO trigger.
    edpe::CameraProjection smoothAdsStep = worldBase;
    smoothAdsStep.scaleX = 1.2634f; // ~20% zoom step from 1.0525
    smoothAdsStep.scaleY = 2.2460f;
    if (edpe::isCameraProjectionCut(smoothAdsStep, worldBase)) return 80;
    if (edpe::isCommittedCameraDiscontinuity(smoothAdsStep, worldBase)) return 81;

    // Frame-time hitch + smooth movement must not falsely trigger Debug View reset:
    auto movingStep = smoothAdsStep;
    movingStep.worldFromView[3] = 0.0682f;
    if (edpe::isCommittedCameraDiscontinuity(movingStep, worldBase)) return 82;

    // True projection cut (>25% scale jump or depthB change):
    if (!edpe::isCameraProjectionCut(adsViewmodel, worldBase)) return 83;
    if (!edpe::isCommittedCameraDiscontinuity(adsViewmodel, worldBase)) return 84;
    if (!edpe::isCameraProjectionCut(viewmodelSubpass, worldBase)) return 85;
    // Large rotation cut (>45 deg, trace < 2.414) or mirrored reflection:
    if (!edpe::isCommittedCameraDiscontinuity(yawed(50.0f), identity())) return 86;
    if (!edpe::isCommittedCameraDiscontinuity(planarReflection, identity())) return 87;

    // 30. External on-foot local-origin rebase (~22.9m jump, 0 deg rotation, same FOV/depthB)
    // inside 4-MRT G-buffer for <5 frames must hold continuous position & keep jitter enabled;
    // >=5 frames or pre-G-buffer (!inMainScene) or large rotation/FOV cut must not hold.
    edpe::CameraProjection originRebase = worldBase;
    originRebase.worldFromView[3] = -0.0741f;
    originRebase.worldFromView[7] = -4.1486f;
    originRebase.worldFromView[11] = -22.5333f;
    const float rebaseJump = 22.912f;
    if (!edpe::isTransientOriginRebaseHoldEligible(originRebase, worldBase, rebaseJump, 1, true, false)) return 88;
    if (!edpe::isTransientOriginRebaseHoldEligible(originRebase, worldBase, rebaseJump, 4, true, false)) return 89;
    // Streak >= 5 accepts persistent teleport instead of holding forever:
    if (edpe::isTransientOriginRebaseHoldEligible(originRebase, worldBase, rebaseJump, 5, true, false)) return 90;
    // Pre-G-buffer (!inMainScene) hologram jumps must not stage:
    if (edpe::isTransientOriginRebaseHoldEligible(originRebase, worldBase, rebaseJump, 1, false, false)) return 91;
    // Real rotation cut (>45 deg) or projection cut at >15m must not hold as origin rebase:
    auto rotatedJump = yawed(50.0f);
    rotatedJump.worldFromView[11] = -22.5333f;
    if (edpe::isTransientOriginRebaseHoldEligible(rotatedJump, identity(), rebaseJump, 1, true, false)) return 92;

    // 31. Persistent unpredicted teleport (>15m beyond velocity prediction) triggers scene cut,
    // while smooth high-speed continuous flight (residual <= 15m, step <= 50m) does not.
    const edpe::Vec3 teleportDelta{-0.0741f, -4.1486f, -22.5333f};
    const edpe::Vec3 walkingVel{0.0f, 0.0f, 0.08f};
    if (!edpe::isUnpredictedPositionCut(teleportDelta, walkingVel)) return 93;
    const edpe::Vec3 flightStep{0.0f, 0.0f, 30.0f};
    if (edpe::isUnpredictedPositionCut(flightStep, flightStep)) return 94;
    const edpe::Vec3 over50Step{0.0f, 0.0f, 55.0f};
    if (!edpe::isUnpredictedPositionCut(over50Step, over50Step)) return 95;

    return 0;
}
