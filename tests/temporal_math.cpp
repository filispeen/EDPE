#include "temporal_math.h"
#include "elite_camera.h"

#include <array>
#include <cmath>
#include <limits>

int main() {
    const edpe::CameraProjection previous{{1, 0, 0, 0,
                                           0, 1, 0, 0,
                                           0, 0, 1, 0}, 1, 1, 0.025f};
    auto now = previous;
    float dx = 0, dy = 0;
    constexpr float depth = 0.0025f; // viewZ = 10
    if (!edpe::cameraDepthMotion(now, previous, 100, 100, 49.5f, 49.5f, depth, &dx, &dy) ||
        std::fabs(dx) >= 0.0001f || std::fabs(dy) >= 0.0001f) return 1;

    now.worldFromView[3] = 1; // Camera moved right; static world moved right in the previous image.
    if (!edpe::cameraDepthMotion(now, previous, 100, 100, 49.5f, 49.5f, depth, &dx, &dy) ||
        std::fabs(dx - 5.0f) >= 0.0001f || std::fabs(dy) >= 0.0001f) return 2;

    now = previous;
    now.worldFromView[7] = 1; // Camera moved up; static world moved up in the previous image.
    if (!edpe::cameraDepthMotion(now, previous, 100, 100, 49.5f, 49.5f, depth, &dx, &dy) ||
        std::fabs(dx) >= 0.0001f || std::fabs(dy + 5.0f) >= 0.0001f) return 3;

    now = previous;
    constexpr float angle = 0.1f;
    now.worldFromView[0] = now.worldFromView[10] = std::cos(angle);
    now.worldFromView[2] = std::sin(angle);
    now.worldFromView[8] = -std::sin(angle);
    if (!edpe::cameraDepthMotion(now, previous, 100, 100, 49.5f, 49.5f, depth, &dx, &dy) ||
        std::fabs(dx - 50.0f * std::tan(angle)) >= 0.0001f) return 4;

    if (edpe::cameraDepthMotion(now, previous, 100, 100, 49.5f, 49.5f, 0, &dx, &dy)) return 5;
    if (edpe::cameraDepthMotion(now, previous, 0, 100, 49.5f, 49.5f, depth, &dx, &dy)) return 6;

    // Skybox / infinity motion tests:
    auto trans_only = previous;
    trans_only.worldFromView[3] = 100.0f;
    trans_only.worldFromView[7] = -50.0f;
    if (!edpe::cameraSkyboxMotion(trans_only, previous, 100, 100, 49.5f, 49.5f, &dx, &dy) ||
        std::fabs(dx) >= 0.0001f || std::fabs(dy) >= 0.0001f) return 7;

    if (!edpe::cameraSkyboxMotion(now, previous, 100, 100, 49.5f, 49.5f, &dx, &dy) ||
        std::fabs(dx - 50.0f * std::tan(angle)) >= 0.0001f) return 8;

    // Rounded fields from a user-confirmed Odyssey 332841 scene capture.
    std::array<float, 1344> scene{};
    const float rows[]{-.965338f, -.0708149f, -.251214f, 19.5594f,
                       -.00114341f, .963628f, -.267244f, -39.2084f,
                       .261002f, -.257694f, -.930307f, -99.7282f};
    const float block[]{-.940506756f, -.122647151f, 0, -.251219571f,
                        -.00110631355f, 1.6690501f, 0, -.267250597f,
                        .254292488f, -.446352154f, 0, -.93030417f,
                        0, 0, .0250000004f, 0};
    for (size_t i = 0; i < 12; ++i) scene[932 + i] = rows[i];
    for (size_t i = 0; i < 16; ++i) scene[1080 + i] = block[i];
    edpe::CameraProjection parsed{};
    if (!edpe::parseEliteCamera(scene.data(), scene.size(), &parsed) ||
        std::fabs(parsed.scaleX - std::sqrt(3.0f) * 1440 / 2560) > 0.001f ||
        std::fabs(parsed.scaleY - std::sqrt(3.0f)) > 0.001f) return 9;
    scene[1083] += .1f;
    if (edpe::parseEliteCamera(scene.data(), scene.size(), &parsed)) return 10;
    scene[1083] -= .1f;
    // The projection orientation may be one update ahead during a turn.
    scene[1089] -= .0048f;
    scene[1087] += .002f;
    if (!edpe::parseEliteCamera(scene.data(), scene.size(), &parsed) ||
        std::fabs(parsed.worldFromView[9] - scene[1089] / parsed.scaleY) > .0001f) return 11;
    scene.fill(0);
    const float turning_rows[]{.974892f, .148231f, .166172f, 4.80449f,
                               -.152375f, .988245f, .0124005f, 4.34171f,
                               -.16238f, -.0374095f, .986019f, 24.7976f};
    const float turning_block[]{.949832499f, .255993724f, 0, .166460797f,
                                -.148358271f, 1.71167052f, 0, .0143891573f,
                                -.158198923f, -.0682011172f, 0, .985943079f,
                                0, 0, .0250000004f, 0};
    for (size_t i = 0; i < 12; ++i) scene[932 + i] = turning_rows[i];
    for (size_t i = 0; i < 16; ++i) scene[1080 + i] = turning_block[i];
    if (!edpe::parseEliteCamera(scene.data(), scene.size(), &parsed) ||
        std::fabs(parsed.worldFromView[9] - turning_block[9] / parsed.scaleY) > .0001f)
        return 12;

    const auto first = edpe::projectionJitter(0, 100, 50);
    const auto second = edpe::projectionJitter(1, 100, 50);
    const auto third = edpe::projectionJitter(2, 100, 50);
    if (std::fabs(first.pixelX) > 0.000001f ||
        std::fabs(first.pixelY + 1.0f / 6) > 0.000001f ||
        std::fabs(first.ndcY - 1.0f / 150) > 0.000001f ||
        std::fabs(second.pixelX + 0.25f) > 0.000001f ||
        std::fabs(second.pixelY - 1.0f / 6) > 0.000001f ||
        std::fabs(second.ndcX + 0.005f) > 0.000001f ||
        std::fabs(third.pixelX - 0.25f) > 0.000001f ||
        std::fabs(third.pixelY + 7.0f / 18) > 0.000001f) return 13;
    if (edpe::projectionJitter(1, 0, 50).pixelX != 0 ||
        edpe::projectionJitter(1, 100, 0).ndcY != 0) return 14;

    // Match the two captured vertex shaders' cb1[270..273] clip arithmetic.
    const float clip_block[]{1, 0, 0, 0,
                             0, 1, 0, 0,
                             0, 0, 0, 1,
                             0, 0, .025f, 0};
    float shifted[16]{};
    if (!edpe::jitterEliteProjectionBlock(clip_block, shifted, third)) return 15;
    const auto clip = [](const float* b, unsigned component) {
        return b[component] + 2 * b[4 + component] + 10 * b[8 + component] +
            b[12 + component];
    };
    const float original_w = clip(clip_block, 3), shifted_w = clip(shifted, 3);
    const float pixel_shift_x = 50 * (clip(shifted, 0) / shifted_w -
        clip(clip_block, 0) / original_w);
    const float pixel_shift_y = -25 * (clip(shifted, 1) / shifted_w -
        clip(clip_block, 1) / original_w);
    if (std::fabs(pixel_shift_x - third.pixelX) > .00001f ||
        std::fabs(pixel_shift_y - third.pixelY) > .00001f ||
        shifted[10] != clip_block[10] || shifted[14] != clip_block[14]) return 16;
    float unchanged[16]{};
    if (edpe::jitterEliteProjectionBlock(nullptr, unchanged, third) ||
        unchanged[0] != 0) return 17;

    float dot_block[16]{}, dot_shifted[16]{};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column)
            dot_block[row * 4 + column] = clip_block[column * 4 + row];
    if (!edpe::jitterEliteDotProjectionBlock(dot_block, dot_shifted, third)) return 18;
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column)
            if (std::fabs(dot_shifted[row * 4 + column] -
                    shifted[column * 4 + row]) > .000001f) return 19;
    if (!edpe::jitterEliteDotProjectionBlock(dot_block, dot_block, third)) return 20;
    for (unsigned i = 0; i < 16; ++i)
        if (std::fabs(dot_block[i] - dot_shifted[i]) > .000001f) return 21;

    // The two stored Odyssey 332841 blocks (9 digits) carry no built-in offset.
    edpe::NdcOffset native{};
    for (const float* stored : {block, turning_block})
        if (!edpe::estimateEliteBlockNdcOffset(stored, &native) ||
            std::fabs(native.x) > 1e-6f || std::fabs(native.y) > 1e-6f) return 22;

    // Offsets applied by jitterEliteProjectionBlock are recovered with the same sign.
    for (unsigned frame = 0; frame < 16; ++frame) {
        const auto jitter = edpe::projectionJitter(frame, 2560, 1440);
        float jittered[16]{};
        if (!edpe::jitterEliteProjectionBlock(block, jittered, jitter) ||
            !edpe::estimateEliteBlockNdcOffset(jittered, &native)) return 23;
        const float pixel_x = native.x * 2560 / 2, pixel_y = -native.y * 1440 / 2;
        if (std::fabs(pixel_x - jitter.pixelX) > .01f ||
            std::fabs(pixel_y - jitter.pixelY) > .01f) return 24;
    }
    const float no_w[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0};
    if (edpe::estimateEliteBlockNdcOffset(no_w, &native) ||
        edpe::estimateEliteBlockNdcOffset(nullptr, &native)) return 25;

    // -------------------------------------------------------------------------
    // Phase 5: Projection jitter sequence properties and validation bounds
    // -------------------------------------------------------------------------
    double sumX = 0, sumY = 0;
    for (unsigned frame = 0; frame < 64; ++frame) {
        const auto jitter = edpe::projectionJitter(frame, 2560, 1440);
        // Pixel offsets must remain strictly within [-0.5, 0.5]
        if (jitter.pixelX < -0.5f || jitter.pixelX > 0.5f ||
            jitter.pixelY < -0.5f || jitter.pixelY > 0.5f) return 26;
        // NDC offsets must scale with render dimensions
        if (std::fabs(jitter.ndcX - 2.0f * jitter.pixelX / 2560.0f) > 1e-6f ||
            std::fabs(jitter.ndcY - (-2.0f * jitter.pixelY / 1440.0f)) > 1e-6f) return 27;
        sumX += jitter.pixelX;
        sumY += jitter.pixelY;
    }
    // Halton sequence mean over 64 samples must stay close to 0 (< 0.1 pixel)
    if (std::fabs(sumX / 64.0) > 0.1 || std::fabs(sumY / 64.0) > 0.1) return 28;

    // Test rejection of invalid blocks (fail-open protection)
    float bad_block[16]{};
    std::memcpy(bad_block, block, sizeof(block));
    bad_block[5] = std::numeric_limits<float>::quiet_NaN();
    float out_block[16]{};
    if (edpe::jitterEliteProjectionBlock(bad_block, out_block, edpe::projectionJitter(0, 2560, 1440)))
        return 29;

    // Jittered projection block continues to pass parseEliteCamera checks
    for (unsigned frame = 0; frame < 16; ++frame) {
        const auto jitter = edpe::projectionJitter(frame, 2560, 1440);
        float jittered[16]{};
        if (!edpe::jitterEliteProjectionBlock(turning_block, jittered, jitter)) return 30;
        for (size_t i = 0; i < 16; ++i) scene[1080 + i] = jittered[i];
        edpe::CameraProjection jittered_cam{};
        if (!edpe::parseEliteCamera(scene.data(), scene.size(), &jittered_cam)) return 31;
    }

    // -------------------------------------------------------------------------
    // Phase 6: Linear depth camera motion equivalence and validation
    // -------------------------------------------------------------------------
    // Given rawDepth = 0.0025f and depthB = 0.025f, linearZ = depthB / rawDepth = 10.0f.
    const float raw_d = 0.0025f;
    const float lin_z = 0.025f / raw_d; // 10.0m
    float dx_raw = 0, dy_raw = 0;
    float dx_lin = 0, dy_lin = 0;

    auto cam_a = previous;
    auto cam_b = previous;
    cam_b.worldFromView[3] = 1.0f; // translation X
    cam_b.worldFromView[7] = 2.0f; // translation Y
    cam_b.worldFromView[11] = -0.5f; // translation Z

    if (!edpe::cameraDepthMotion(cam_b, cam_a, 100, 100, 49.5f, 49.5f, raw_d, &dx_raw, &dy_raw)) return 32;
    if (!edpe::cameraLinearDepthMotion(cam_b, cam_a, 100, 100, 49.5f, 49.5f, lin_z, &dx_lin, &dy_lin)) return 33;
    if (std::fabs(dx_raw - dx_lin) > 1e-5f || std::fabs(dy_raw - dy_lin) > 1e-5f) return 34;

    // Rotation test
    cam_b = cam_a;
    cam_b.worldFromView[0] = cam_b.worldFromView[10] = std::cos(0.05f);
    cam_b.worldFromView[2] = std::sin(0.05f);
    cam_b.worldFromView[8] = -std::sin(0.05f);
    if (!edpe::cameraDepthMotion(cam_b, cam_a, 100, 100, 25.0f, 75.0f, raw_d, &dx_raw, &dy_raw)) return 35;
    if (!edpe::cameraLinearDepthMotion(cam_b, cam_a, 100, 100, 25.0f, 75.0f, lin_z, &dx_lin, &dy_lin)) return 36;
    if (std::fabs(dx_raw - dx_lin) > 1e-5f || std::fabs(dy_raw - dy_lin) > 1e-5f) return 37;

    // Fail-open validation for cameraLinearDepthMotion
    if (edpe::cameraLinearDepthMotion(cam_b, cam_a, 100, 100, 49.5f, 49.5f, 0.0f, &dx_lin, &dy_lin)) return 38;
    if (edpe::cameraLinearDepthMotion(cam_b, cam_a, 100, 100, 49.5f, 49.5f, -5.0f, &dx_lin, &dy_lin)) return 39;
    if (edpe::cameraLinearDepthMotion(cam_b, cam_a, 0, 100, 49.5f, 49.5f, lin_z, &dx_lin, &dy_lin)) return 40;
    if (edpe::cameraLinearDepthMotion(cam_b, cam_a, 100, 100, 49.5f, 49.5f, std::numeric_limits<float>::quiet_NaN(), &dx_lin, &dy_lin)) return 41;

    // -------------------------------------------------------------------------
    // Phase 6 (D1): Absolute hand-computed motion and edge case validation
    // -------------------------------------------------------------------------
    // Hand-computed expected motion test for pure Y translation:
    // width = 100, height = 100, pixelX = 49.5f, pixelY = 49.5f (center pixel, ndcX = 0, ndcY = 0).
    // now = identity (scaleX = 1, scaleY = 1), lin_z = 10.0m.
    // previous = identity rotation, translated +2.0m along Y (worldFromView[7] = 2.0f).
    // In previous view space, world point (0, 0, 10) is (0, -2, 10).
    // prevNdcY = 1.0 * (-2.0) / 10.0 = -0.2.
    // prevPixelY = (1.0 - (-0.2)) * 50 - 0.5 = 1.2 * 50 - 0.5 = 59.5.
    // dy = prevPixelY - pixelY = 59.5 - 49.5 = +10.0 pixels.
    // dx = 0.0 pixels.
    edpe::CameraProjection hand_now{{1,0,0,0, 0,1,0,0, 0,0,1,0}, 1.0f, 1.0f, 0.025f};
    edpe::CameraProjection hand_prev = hand_now;
    hand_prev.worldFromView[7] = 2.0f;
    float hand_dx = 0, hand_dy = 0;
    if (!edpe::cameraLinearDepthMotion(hand_now, hand_prev, 100, 100, 49.5f, 49.5f, 10.0f, &hand_dx, &hand_dy)) return 42;
    if (std::fabs(hand_dx - 0.0f) > 1e-4f || std::fabs(hand_dy - 10.0f) > 1e-4f) return 43;

    // Hand-computed expected motion test for pure Y rotation:
    // now = identity (scaleX = 1, scaleY = 1), lin_z = 10.0m.
    // center pixel (49.5, 49.5) -> view ray (0, 0, 10) -> world (0, 0, 10).
    // Rotate previous camera by theta = 0.1 rad around Y axis:
    // R_prev = [cos, 0, sin; 0, 1, 0; -sin, 0, cos].
    // prev view ray = R_prev^T * (0, 0, 10) = (-10 * sin(0.1), 0, 10 * cos(0.1)).
    // prevNdcX = -tan(0.1).
    // prevPixelX = (-tan(0.1) + 1.0) * 50 - 0.5.
    // dx = -50.0 * tan(0.1) = -50.0 * 0.100334672 = -5.0167336 pixels.
    // dy = 0.0 pixels.
    hand_prev = hand_now;
    const float theta = 0.1f;
    hand_prev.worldFromView[0] = std::cos(theta);
    hand_prev.worldFromView[2] = std::sin(theta);
    hand_prev.worldFromView[8] = -std::sin(theta);
    hand_prev.worldFromView[10] = std::cos(theta);
    const float expected_rot_dx = -50.0f * std::tan(theta);
    if (!edpe::cameraLinearDepthMotion(hand_now, hand_prev, 100, 100, 49.5f, 49.5f, 10.0f, &hand_dx, &hand_dy)) return 44;
    if (std::fabs(hand_dx - expected_rot_dx) > 1e-4f || std::fabs(hand_dy - 0.0f) > 1e-4f) return 45;

    // Reject infinity:
    if (edpe::cameraLinearDepthMotion(hand_now, hand_prev, 100, 100, 49.5f, 49.5f, std::numeric_limits<float>::infinity(), &hand_dx, &hand_dy)) return 46;

    // Reject previous.scaleX = 0:
    auto zero_scale = hand_prev;
    zero_scale.scaleX = 0.0f;
    if (edpe::cameraLinearDepthMotion(hand_now, zero_scale, 100, 100, 49.5f, 49.5f, 10.0f, &hand_dx, &hand_dy)) return 47;

    // Reject previous.scaleY = 0:
    zero_scale = hand_prev;
    zero_scale.scaleY = 0.0f;
    if (edpe::cameraLinearDepthMotion(hand_now, zero_scale, 100, 100, 49.5f, 49.5f, 10.0f, &hand_dx, &hand_dy)) return 48;

    // Unit tests for detectSceneCut:
    float testMoveDist = 0.0f, testRotTrace = 0.0f;
    if (!edpe::detectSceneCut(hand_now, hand_prev, ~0ull, &testMoveDist, &testRotTrace)) return 49;
    if (testRotTrace != 3.0f || testMoveDist != 0.0f) return 50;

    // Identical camera states: no scene cut
    if (edpe::detectSceneCut(hand_now, hand_now, 0, &testMoveDist, &testRotTrace)) return 51;
    if (testMoveDist != 0.0f || std::fabs(testRotTrace - 3.0f) > 1e-4f) return 52;

    // Large rotation (rotTrace < 2.0f): scene cut
    auto rotated_cam = hand_now;
    rotated_cam.worldFromView[0] = rotated_cam.worldFromView[10] = 0.0f;
    rotated_cam.worldFromView[2] = 1.0f;
    rotated_cam.worldFromView[8] = -1.0f;
    if (!edpe::detectSceneCut(rotated_cam, hand_now, 0, &testMoveDist, &testRotTrace)) return 53;

    // High velocity straight flight (moveDist = 100, rotTrace = 3.0f): must NOT trigger cut
    auto flight_cam = hand_now;
    flight_cam.worldFromView[11] = 100.0f;
    if (edpe::detectSceneCut(flight_cam, hand_now, 0, &testMoveDist, &testRotTrace)) return 54;

    // Teleport (moveDist = 6000.0f): scene cut
    flight_cam.worldFromView[11] = 6000.0f;
    if (!edpe::detectSceneCut(flight_cam, hand_now, 0, &testMoveDist, &testRotTrace)) return 55;

    // Unit tests for updateCameraHistory:
    edpe::CameraHistory camHist{};
    if (edpe::updateCameraHistory(camHist, 1, nullptr, nullptr)) return 56;
    if (!edpe::updateCameraHistory(camHist, 1, &hand_now, nullptr)) return 57;
    if (camHist.currentFrame != 1 || camHist.previousFrame != ~0ull) return 58;
    if (edpe::updateCameraHistory(camHist, 1, &hand_now, nullptr)) return 59; // same frame returns false

    // Next frame: current becomes previous
    auto next_cam = hand_now;
    next_cam.worldFromView[3] = 5.0f;
    if (!edpe::updateCameraHistory(camHist, 2, &next_cam, nullptr)) return 60;
    if (camHist.currentFrame != 2 || camHist.previousFrame != 1) return 61;
    if (camHist.previous.worldFromView[3] != 0.0f || camHist.current.worldFromView[3] != 5.0f) return 62;

    // Fallback streak test:
    auto fallback_cam = hand_now;
    fallback_cam.worldFromView[3] = 99.0f;
    if (!edpe::updateCameraHistory(camHist, 3, nullptr, &fallback_cam)) return 63;
    if (camHist.fallbackStreak != 1 || camHist.current.worldFromView[3] != 5.0f) return 64;
    if (!edpe::updateCameraHistory(camHist, 4, nullptr, &fallback_cam)) return 65;
    if (camHist.fallbackStreak != 2 || camHist.current.worldFromView[3] != 5.0f) return 66;
    if (!edpe::updateCameraHistory(camHist, 5, nullptr, &fallback_cam)) return 67;
    if (camHist.current.worldFromView[3] != 99.0f || camHist.fallbackStreak != 0) return 68;

    return 0; // All temporal math tests passed
}
