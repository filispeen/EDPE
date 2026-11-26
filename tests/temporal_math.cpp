#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "temporal_math.h"
#include "elite_camera.h"
#include "cobra_camera_inject.h"

#include <array>
#include <cmath>
#include <cstring>
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
        std::fabs(dx) >= 0.0001f || std::fabs(dy) >= 0.0001f) return 101;

    if (!edpe::cameraSkyboxMotion(now, previous, 100, 100, 49.5f, 49.5f, &dx, &dy) ||
        std::fabs(dx - 50.0f * std::tan(angle)) >= 0.0001f) return 102;

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
        std::fabs(parsed.scaleY - std::sqrt(3.0f)) > 0.001f) return 7;
    scene[1083] += .1f;
    if (edpe::parseEliteCamera(scene.data(), scene.size(), &parsed)) return 8;
    scene[1083] -= .1f;
    // The projection orientation may be one update ahead during a turn.
    scene[1089] -= .0048f;
    scene[1087] += .002f;
    if (!edpe::parseEliteCamera(scene.data(), scene.size(), &parsed) ||
        std::fabs(parsed.worldFromView[9] - scene[1089] / parsed.scaleY) > .0001f) return 9;
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
        return 10;
    // When cb1[275].xyz (words 1100..1102) is populated, parseEliteCamera prefers it over lagged cam932 translation.
    scene[1100] = 5.125f;
    scene[1101] = 4.500f;
    scene[1102] = 25.250f;
    if (!edpe::parseEliteCamera(scene.data(), scene.size(), &parsed) ||
        std::fabs(parsed.worldFromView[3] - 5.125f) > 1e-6f ||
        std::fabs(parsed.worldFromView[7] - 4.500f) > 1e-6f ||
        std::fabs(parsed.worldFromView[11] - 25.250f) > 1e-6f)
        return 109;

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
        std::fabs(third.pixelY + 7.0f / 18) > 0.000001f) return 9;
    if (edpe::projectionJitter(1, 0, 50).pixelX != 0 ||
        edpe::projectionJitter(1, 100, 0).ndcY != 0) return 10;

    // Match the two captured vertex shaders' cb1[270..273] clip arithmetic.
    const float clip_block[]{1, 0, 0, 0,
                             0, 1, 0, 0,
                             0, 0, 0, 1,
                             0, 0, .025f, 0};
    float shifted[16]{};
    if (!edpe::jitterEliteProjectionBlock(clip_block, shifted, third)) return 11;
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
        shifted[10] != clip_block[10] || shifted[14] != clip_block[14]) return 12;
    float unchanged[16]{};
    if (edpe::jitterEliteProjectionBlock(nullptr, unchanged, third) ||
        unchanged[0] != 0) return 13;

    float dot_block[16]{}, dot_shifted[16]{};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column)
            dot_block[row * 4 + column] = clip_block[column * 4 + row];
    if (!edpe::jitterEliteDotProjectionBlock(dot_block, dot_shifted, third)) return 14;
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column)
            if (std::fabs(dot_shifted[row * 4 + column] -
                    shifted[column * 4 + row]) > .000001f) return 15;
    if (!edpe::jitterEliteDotProjectionBlock(dot_block, dot_block, third)) return 16;
    for (unsigned i = 0; i < 16; ++i)
        if (std::fabs(dot_block[i] - dot_shifted[i]) > .000001f) return 17;

    // The two stored Odyssey 332841 blocks (9 digits) carry no built-in offset.
    edpe::NdcOffset native{};
    for (const float* stored : {block, turning_block})
        if (!edpe::estimateEliteBlockNdcOffset(stored, &native) ||
            std::fabs(native.x) > 1e-6f || std::fabs(native.y) > 1e-6f) return 18;

    // Offsets applied by jitterEliteProjectionBlock are recovered with the same sign.
    for (unsigned frame = 0; frame < 16; ++frame) {
        const auto jitter = edpe::projectionJitter(frame, 2560, 1440);
        float jittered[16]{};
        if (!edpe::jitterEliteProjectionBlock(block, jittered, jitter) ||
            !edpe::estimateEliteBlockNdcOffset(jittered, &native)) return 19;
        const float pixel_x = native.x * 2560 / 2, pixel_y = -native.y * 1440 / 2;
        if (std::fabs(pixel_x - jitter.pixelX) > .01f ||
            std::fabs(pixel_y - jitter.pixelY) > .01f) return 20;
    }
    const float no_w[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0};
    if (edpe::estimateEliteBlockNdcOffset(no_w, &native) ||
        edpe::estimateEliteBlockNdcOffset(nullptr, &native)) return 21;

    // -------------------------------------------------------------------------
    // Phase 5: Projection jitter sequence properties and validation bounds
    // -------------------------------------------------------------------------
    double sumX = 0, sumY = 0;
    for (unsigned frame = 0; frame < 64; ++frame) {
        const auto jitter = edpe::projectionJitter(frame, 2560, 1440);
        // Pixel offsets must remain strictly within [-0.5, 0.5]
        if (jitter.pixelX < -0.5f || jitter.pixelX > 0.5f ||
            jitter.pixelY < -0.5f || jitter.pixelY > 0.5f) return 22;
        // NDC offsets must scale with render dimensions
        if (std::fabs(jitter.ndcX - 2.0f * jitter.pixelX / 2560.0f) > 1e-6f ||
            std::fabs(jitter.ndcY - (-2.0f * jitter.pixelY / 1440.0f)) > 1e-6f) return 23;
        sumX += jitter.pixelX;
        sumY += jitter.pixelY;
    }
    // Halton sequence mean over 64 samples must stay close to 0 (< 0.1 pixel)
    if (std::fabs(sumX / 64.0) > 0.1 || std::fabs(sumY / 64.0) > 0.1) return 24;

    // Test rejection of invalid blocks (fail-open protection)
    float bad_block[16]{};
    std::memcpy(bad_block, block, sizeof(block));
    bad_block[5] = std::numeric_limits<float>::quiet_NaN();
    float out_block[16]{};
    if (edpe::jitterEliteProjectionBlock(bad_block, out_block, edpe::projectionJitter(0, 2560, 1440)))
        return 25;

    // Jittered projection block continues to pass parseEliteCamera checks
    for (unsigned frame = 0; frame < 16; ++frame) {
        const auto jitter = edpe::projectionJitter(frame, 2560, 1440);
        float jittered[16]{};
        if (!edpe::jitterEliteProjectionBlock(turning_block, jittered, jitter)) return 26;
        for (size_t i = 0; i < 16; ++i) scene[1080 + i] = jittered[i];
        edpe::CameraProjection jittered_cam{};
        if (!edpe::parseEliteCamera(scene.data(), scene.size(), &jittered_cam)) return 27;
    }

    // 208-byte CB0 transpose projection validation and jitter recovery
    float dot_turning[16]{};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 4; ++col)
            dot_turning[row * 4 + col] = turning_block[col * 4 + row];
    if (!edpe::estimateEliteTransposeBlockNdcOffset(dot_turning, &native) ||
        std::fabs(native.x) > 1e-5f || std::fabs(native.y) > 1e-5f) return 28;
    if (!edpe::matchesEliteTransposeProjectionBlock(dot_turning, parsed)) return 29;

    for (unsigned frame = 0; frame < 16; ++frame) {
        const auto jitter = edpe::projectionJitter(frame, 2560, 1440);
        float dot_jittered[16]{};
        if (!edpe::jitterEliteDotProjectionBlock(dot_turning, dot_jittered, jitter)) return 30;
        edpe::NdcOffset recovered{};
        if (!edpe::estimateEliteTransposeBlockNdcOffset(dot_jittered, &recovered) ||
            std::fabs(recovered.x - jitter.ndcX) > 1e-5f ||
            std::fabs(recovered.y - jitter.ndcY) > 1e-5f) return 31;
        if (!edpe::matchesEliteTransposeProjectionBlock(dot_jittered, parsed)) return 32;
    }

    float bad_dot[16]{};
    for (size_t i = 0; i < 16; ++i) bad_dot[i] = dot_turning[i];
    bad_dot[11] = 0.0f; // Invalid depthB
    if (edpe::matchesEliteTransposeProjectionBlock(bad_dot, parsed)) return 33;
    bad_dot[11] = dot_turning[11];
    bad_dot[10] = 0.5f; // Unexpected non-zero orthogonal/clip Z cell
    if (edpe::matchesEliteTransposeProjectionBlock(bad_dot, parsed)) return 34;

    auto wrong_fov_cam = parsed;
    wrong_fov_cam.scaleX *= 1.10f;
    if (edpe::matchesEliteTransposeProjectionBlock(dot_turning, wrong_fov_cam)) return 35;

    if (edpe::estimateEliteTransposeBlockNdcOffset(no_w, &native) ||
        edpe::matchesEliteTransposeProjectionBlock(no_w, parsed)) return 36;

    // Inverse UV frustum ray jitter invariance and unjitter round-trip recovery
    constexpr unsigned kRw = 2560;
    constexpr unsigned kRh = 1440;
    const auto inv_jitter = edpe::projectionJitter(3, kRw, kRh, 32);
    const float dxUv = inv_jitter.pixelX / static_cast<float>(kRw);
    const float dyUv = inv_jitter.pixelY / static_cast<float>(kRh);
    // Physically consistent inverse UV frustum rays for turning_block (A * B == T)
    const float tbRight[3]{turning_block[0] / parsed.scaleX, turning_block[4] / parsed.scaleX, turning_block[8] / parsed.scaleX};
    const float tbUp[3]{turning_block[1] / parsed.scaleY, turning_block[5] / parsed.scaleY, turning_block[9] / parsed.scaleY};
    const float tbFwd[3]{turning_block[3], turning_block[7], turning_block[11]};
    const std::array<float, 12> base_inv_rays{
         2.0f * tbRight[0] / parsed.scaleX,  2.0f * tbRight[1] / parsed.scaleX,  2.0f * tbRight[2] / parsed.scaleX, 0.0f, // dX
        -2.0f * tbUp[0] / parsed.scaleY,    -2.0f * tbUp[1] / parsed.scaleY,    -2.0f * tbUp[2] / parsed.scaleY,    0.0f, // dY
        tbFwd[0] - tbRight[0] / parsed.scaleX + tbUp[0] / parsed.scaleY,
        tbFwd[1] - tbRight[1] / parsed.scaleX + tbUp[1] / parsed.scaleY,
        tbFwd[2] - tbRight[2] / parsed.scaleX + tbUp[2] / parsed.scaleY, 0.0f // originCorner
    };
    auto evalRay = [](const float* rays, float u, float v, int c) {
        return rays[8 + c] + u * rays[c] + v * rays[4 + c];
    };
    std::array<float, 12> shifted_rays = base_inv_rays;
    edpe::cobra_camera::jitterInverseUvRay(shifted_rays.data(), dxUv, dyUv);
    for (int c = 0; c < 3; ++c) {
        const float expectedRay = evalRay(base_inv_rays.data(), 0.37f, 0.61f, c);
        const float reconstructedRay = evalRay(shifted_rays.data(), 0.37f + dxUv, 0.61f + dyUv, c);
        if (std::fabs(reconstructedRay - expectedRay) > 1e-6f) return 37;
    }
    edpe::cobra_camera::jitterInverseUvRay(
        shifted_rays.data(), -inv_jitter.ndcX * 0.5f, inv_jitter.ndcY * 0.5f);
    for (int i = 0; i < 12; ++i) {
        if (std::fabs(shifted_rays[i] - base_inv_rays[i]) > 1e-6f) return 38;
    }

    // Deferred lighting CB2 repairDeferredMissingPhase (no double-jitter)
    std::array<float, 16> cb2_jittered_fwd{};
    if (!edpe::jitterEliteProjectionBlock(turning_block, cb2_jittered_fwd.data(), inv_jitter)) return 39;
    std::array<float, 68> cb2_missing_phase{};
    std::memcpy(&cb2_missing_phase[40], cb2_jittered_fwd.data(), 16 * sizeof(float));
    std::memcpy(&cb2_missing_phase[56], base_inv_rays.data(), 12 * sizeof(float));
    if (!edpe::cobra_camera::repairDeferredMissingPhase(
            cb2_missing_phase.data(), cb2_missing_phase.size(),
            cb2_jittered_fwd.data(), base_inv_rays.data(), dxUv, dyUv)) return 40;
    for (int c = 0; c < 3; ++c) {
        const float expectedRay = evalRay(base_inv_rays.data(), 0.25f, 0.75f, c);
        const float repairedRay = evalRay(&cb2_missing_phase[56], 0.25f + dxUv, 0.75f + dyUv, c);
        if (std::fabs(repairedRay - expectedRay) > 1e-6f) return 41;
    }
    if (edpe::cobra_camera::repairDeferredMissingPhase(
            cb2_missing_phase.data(), cb2_missing_phase.size(),
            cb2_jittered_fwd.data(), base_inv_rays.data(), dxUv, dyUv)) return 42;

    // Deferred lighting CB2 repairDeferredWholeOmission (no double-jitter)
    std::array<float, 68> cb2_whole_omission{};
    std::memcpy(&cb2_whole_omission[40], turning_block, 16 * sizeof(float));
    std::memcpy(&cb2_whole_omission[56], base_inv_rays.data(), 12 * sizeof(float));
    if (!edpe::cobra_camera::repairDeferredWholeOmission(
            cb2_whole_omission.data(), cb2_whole_omission.size(),
            turning_block, inv_jitter, dxUv, dyUv)) return 43;
    for (size_t i = 0; i < 16; ++i) {
        if (std::fabs(cb2_whole_omission[40 + i] - cb2_jittered_fwd[i]) > 1e-5f) return 44;
    }
    for (int c = 0; c < 3; ++c) {
        const float expectedRay = evalRay(base_inv_rays.data(), 0.25f, 0.75f, c);
        const float repairedRay = evalRay(&cb2_whole_omission[56], 0.25f + dxUv, 0.75f + dyUv, c);
        if (std::fabs(repairedRay - expectedRay) > 1e-6f) return 45;
    }
    if (edpe::cobra_camera::repairDeferredWholeOmission(
            cb2_whole_omission.data(), cb2_whole_omission.size(),
            turning_block, inv_jitter, dxUv, dyUv)) return 46;

    // EDO Row-major UvToView (cb1[237..240] & clustered lighting cs cb0[10..13])
    edpe::CameraProjection edo_cam{};
    edo_cam.scaleX = 1.052385f;
    edo_cam.scaleY = 1.870907f;
    edo_cam.depthB = 0.025f;
    const std::array<float, 16> base_uv_to_view{
        2.0f / edo_cam.scaleX, 0.0f, 0.0f, -1.0f / edo_cam.scaleX,
        0.0f, -2.0f / edo_cam.scaleY, 0.0f,  1.0f / edo_cam.scaleY,
        0.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f / edo_cam.depthB, 0.0f
    };
    auto evalRowMajorRay = [](const float* rows, float u, float v, int r) {
        return rows[r * 4 + 0] * u + rows[r * 4 + 1] * v + rows[r * 4 + 3];
    };
    std::array<float, 16> uv_to_view = base_uv_to_view;
    if (!edpe::cobra_camera::repairRowMajorUvToViewBlock(uv_to_view.data(), edo_cam, dxUv, dyUv)) return 47;
    for (int r = 0; r < 3; ++r) {
        const float expected = evalRowMajorRay(base_uv_to_view.data(), 0.37f, 0.61f, r);
        const float actual = evalRowMajorRay(uv_to_view.data(), 0.37f + dxUv, 0.61f + dyUv, r);
        if (std::fabs(actual - expected) > 1e-5f) return 48;
    }
    if (edpe::cobra_camera::repairRowMajorUvToViewBlock(uv_to_view.data(), edo_cam, dxUv, dyUv)) return 49;
    if (!edpe::cobra_camera::unjitterRowMajorUvToViewBlock(uv_to_view.data(), edo_cam)) return 50;
    for (size_t i = 0; i < 16; ++i) {
        if (std::fabs(uv_to_view[i] - base_uv_to_view[i]) > 1e-5f) return 51;
    }

    // EDO Cascade Shadow Mask CB2 InvProj (592B cb2[0..4])
    std::array<float, 20> cascade_cb2{
        static_cast<float>(kRw), static_cast<float>(kRh),
        1.0f / static_cast<float>(kRw), 1.0f / static_cast<float>(kRh),
        1.0f / edo_cam.scaleX, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f / edo_cam.scaleY, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f / edo_cam.depthB,
        0.0f, 0.0f, 1.0f, 0.0f
    };
    auto evalCascadeRay = [](const float* cb, float u, float v, int c) {
        const float ndc_x = 2.0f * u - 1.0f;
        const float ndc_y = -2.0f * v + 1.0f;
        return ndc_x * cb[4 + c] + ndc_y * cb[8 + c] + cb[16 + c];
    };
    const std::array<float, 20> base_cascade_cb2 = cascade_cb2;
    if (!edpe::cobra_camera::repairCascadeShadowMaskInvProj(
            cascade_cb2.data(), cascade_cb2.size(), edo_cam, inv_jitter)) return 52;
    for (int c = 0; c < 3; ++c) {
        const float expected = evalCascadeRay(base_cascade_cb2.data(), 0.29f, 0.73f, c);
        const float actual = evalCascadeRay(cascade_cb2.data(), 0.29f + dxUv, 0.73f + dyUv, c);
        if (std::fabs(actual - expected) > 1e-5f) return 53;
    }
    if (edpe::cobra_camera::repairCascadeShadowMaskInvProj(
            cascade_cb2.data(), cascade_cb2.size(), edo_cam, inv_jitter)) return 54;

    // EDO Row-major Deferred Lighting CB2 (cb2[10..16] WholeOmission & MissingPhase)
    float rm_unjit_fwd[16]{};
    edpe::cobra_camera::transposeBlock4x4(turning_block, rm_unjit_fwd);
    float rm_jittered_fwd[16]{};
    if (!edpe::jitterEliteDotProjectionBlock(rm_unjit_fwd, rm_jittered_fwd, inv_jitter)) return 55;
    std::array<float, 12> rm_base_rays{
        base_inv_rays[0], base_inv_rays[4], 0.0f, base_inv_rays[8],
        base_inv_rays[1], base_inv_rays[5], 0.0f, base_inv_rays[9],
        base_inv_rays[2], base_inv_rays[6], 0.0f, base_inv_rays[10]
    };
    std::array<float, 68> rm_cb2_whole{};
    std::memcpy(&rm_cb2_whole[40], rm_unjit_fwd, sizeof(rm_unjit_fwd));
    std::memcpy(&rm_cb2_whole[56], rm_base_rays.data(), sizeof(rm_base_rays));
    if (!edpe::cobra_camera::repairRowMajorDeferredWholeOmission(
            rm_cb2_whole.data(), rm_cb2_whole.size(),
            turning_block, inv_jitter, dxUv, dyUv)) return 56;
    for (size_t i = 0; i < 16; ++i) {
        if (std::fabs(rm_cb2_whole[40 + i] - rm_jittered_fwd[i]) > 1e-5f) return 57;
    }
    for (int r = 0; r < 3; ++r) {
        const float expected = evalRowMajorRay(rm_base_rays.data(), 0.25f, 0.75f, r);
        const float actual = evalRowMajorRay(&rm_cb2_whole[56], 0.25f + dxUv, 0.75f + dyUv, r);
        if (std::fabs(actual - expected) > 1e-5f) return 58;
    }
    if (edpe::cobra_camera::repairRowMajorDeferredWholeOmission(
            rm_cb2_whole.data(), rm_cb2_whole.size(),
            turning_block, inv_jitter, dxUv, dyUv)) return 59;

    std::array<float, 68> rm_cb2_missing{};
    std::memcpy(&rm_cb2_missing[40], rm_jittered_fwd, sizeof(rm_jittered_fwd));
    std::memcpy(&rm_cb2_missing[56], rm_base_rays.data(), sizeof(rm_base_rays));
    std::array<float, 16> cm_jittered_fwd{};
    if (!edpe::jitterEliteProjectionBlock(turning_block, cm_jittered_fwd.data(), inv_jitter)) return 60;
    if (!edpe::cobra_camera::repairRowMajorDeferredMissingPhase(
            rm_cb2_missing.data(), rm_cb2_missing.size(),
            cm_jittered_fwd.data(), dxUv, dyUv)) return 61;
    for (int r = 0; r < 3; ++r) {
        const float expected = evalRowMajorRay(rm_base_rays.data(), 0.25f, 0.75f, r);
        const float actual = evalRowMajorRay(&rm_cb2_missing[56], 0.25f + dxUv, 0.75f + dyUv, r);
        if (std::fabs(actual - expected) > 1e-5f) return 62;
    }
    if (edpe::cobra_camera::repairRowMajorDeferredMissingPhase(
            rm_cb2_missing.data(), rm_cb2_missing.size(),
            cm_jittered_fwd.data(), dxUv, dyUv)) return 63;

    // World-space Proj * WorldView CB2 (non-zero column 3 translation from camera worldPos)
    const float ws_cb1_col_major[16]{
         0.517637f, -1.626622f, 0.0f, -0.046443f,
         0.896573f,  0.919756f, 0.0f, -0.180313f,
         0.189010f,  0.091906f, 0.0f,  0.982512f,
         0.0f,       0.0f,      0.025f, 0.0f
    };
    const edpe::Vec3 ws_cam_pos{-16.204481f, -15.919542f, -32.041382f};
    const edpe::Vec3 ws_wrong_pos{100.0f, -20.0f, 5.0f};
    std::array<float, 68> ws_cb2{};
    const float ws_rows10to16[28]{
         0.517637f,  0.896573f, 0.189010f,  28.717203f,
        -1.626622f,  0.919756f, 0.091906f,  -8.771667f,
         0.0f,       0.0f,      0.0f,       0.025000f,
        -0.046443f, -0.180313f, 0.982512f,  27.857958f,
         0.934771f,  0.929420f, 0.0f,      -0.978539f,
         1.619071f, -0.525530f, 0.0f,      -0.727083f,
         0.341323f, -0.052513f, 0.0f,       0.838108f
    };
    std::memcpy(&ws_cb2[40], ws_rows10to16, sizeof(ws_rows10to16));
    if (edpe::cobra_camera::repairRowMajorDeferredWholeOmission(
            ws_cb2.data(), ws_cb2.size(), ws_cb1_col_major, inv_jitter, dxUv, dyUv, nullptr)) return 64;
    if (edpe::cobra_camera::repairRowMajorDeferredWholeOmission(
            ws_cb2.data(), ws_cb2.size(), ws_cb1_col_major, inv_jitter, dxUv, dyUv, &ws_wrong_pos)) return 65;

    std::array<float, 68> ws_cb2_whole = ws_cb2;
    if (!edpe::cobra_camera::repairRowMajorDeferredWholeOmission(
            ws_cb2_whole.data(), ws_cb2_whole.size(), ws_cb1_col_major, inv_jitter, dxUv, dyUv, &ws_cam_pos)) return 66;
    auto evalWorldClip = [](const float* cb2_rows10to13, const float pt[3], float out4[4]) {
        for (int r = 0; r < 4; ++r) {
            out4[r] = cb2_rows10to13[r * 4 + 0] * pt[0] +
                      cb2_rows10to13[r * 4 + 1] * pt[1] +
                      cb2_rows10to13[r * 4 + 2] * pt[2] +
                      cb2_rows10to13[r * 4 + 3];
        }
    };
    const float test_pt[3]{-12.5f, -15.0f, -25.0f};
    float clip_before[4]{}, clip_after[4]{};
    evalWorldClip(&ws_cb2[40], test_pt, clip_before);
    evalWorldClip(&ws_cb2_whole[40], test_pt, clip_after);
    if (std::fabs((clip_after[0] / clip_after[3]) - (clip_before[0] / clip_before[3] + inv_jitter.ndcX)) > 1e-5f ||
        std::fabs((clip_after[1] / clip_after[3]) - (clip_before[1] / clip_before[3] + inv_jitter.ndcY)) > 1e-5f) {
        return 67;
    }
    for (int r = 0; r < 3; ++r) {
        const float expected = evalRowMajorRay(&ws_cb2[56], 0.25f, 0.75f, r);
        const float actual = evalRowMajorRay(&ws_cb2_whole[56], 0.25f + dxUv, 0.75f + dyUv, r);
        if (std::fabs(actual - expected) > 1e-5f) return 68;
    }
    if (edpe::cobra_camera::repairRowMajorDeferredWholeOmission(
            ws_cb2_whole.data(), ws_cb2_whole.size(), ws_cb1_col_major, inv_jitter, dxUv, dyUv, &ws_cam_pos)) return 69;

    std::array<float, 68> ws_cb2_missing = ws_cb2_whole;
    std::memcpy(&ws_cb2_missing[56], &ws_cb2[56], 12 * sizeof(float));
    float ws_cb1_jit_cm[16]{};
    if (!edpe::jitterEliteProjectionBlock(ws_cb1_col_major, ws_cb1_jit_cm, inv_jitter)) return 70;
    if (!edpe::cobra_camera::repairRowMajorDeferredMissingPhase(
            ws_cb2_missing.data(), ws_cb2_missing.size(), ws_cb1_jit_cm, dxUv, dyUv, &ws_cam_pos)) return 71;
    for (int r = 0; r < 3; ++r) {
        const float expected = evalRowMajorRay(&ws_cb2[56], 0.25f, 0.75f, r);
        const float actual = evalRowMajorRay(&ws_cb2_missing[56], 0.25f + dxUv, 0.75f + dyUv, r);
        if (std::fabs(actual - expected) > 1e-5f) return 72;
    }
    if (edpe::cobra_camera::repairRowMajorDeferredMissingPhase(
            ws_cb2_missing.data(), ws_cb2_missing.size(), ws_cb1_jit_cm, dxUv, dyUv, &ws_cam_pos)) return 73;

    // Self-contained WorldView (b2[2..5]) + InvView (b2[6..9]) inside deferred CB2:
    // even when the staged CB1 camera position is stale during translation, CB2's own
    // validated world position repairs both whole-omission and missing-phase blocks.
    const float ws_rows2to9[32]{
         0.491870f,  0.851943f,  0.179601f,  27.287724f,
        -0.869429f,  0.491609f,  0.049124f,  -4.688456f,
        -0.046443f, -0.180313f,  0.982512f,  27.857958f,
         0.0f,       0.0f,       0.0f,        1.0f,
         0.491870f, -0.869429f, -0.046443f, -16.204483f,
         0.851943f,  0.491609f, -0.180313f, -15.919544f,
         0.179601f,  0.049124f,  0.982512f, -32.041382f,
         0.0f,       0.0f,       0.0f,        1.0f
    };
    const edpe::Vec3 stale_moving_pos{-16.12f, -15.919542f, -32.041382f}; // +8.4 cm lag
    std::array<float, 68> self_cb2 = ws_cb2;
    std::memcpy(&self_cb2[8], ws_rows2to9, sizeof(ws_rows2to9));
    edpe::Vec3 extracted_pos{};
    if (!edpe::cobra_camera::extractWorldCamPosFromDeferredCb2(
            self_cb2.data(), self_cb2.size(), &extracted_pos)) return 74;
    if (std::fabs(extracted_pos.x - (-16.204483f)) > 1e-4f ||
        std::fabs(extracted_pos.y - (-15.919544f)) > 1e-4f ||
        std::fabs(extracted_pos.z - (-32.041382f)) > 1e-4f) return 75;

    std::array<float, 68> self_cb2_whole = self_cb2;
    if (!edpe::cobra_camera::repairRowMajorDeferredWholeOmission(
            self_cb2_whole.data(), self_cb2_whole.size(), ws_cb1_col_major, inv_jitter, dxUv, dyUv, &stale_moving_pos)) return 76;
    if (edpe::cobra_camera::repairRowMajorDeferredWholeOmission(
            self_cb2_whole.data(), self_cb2_whole.size(), ws_cb1_col_major, inv_jitter, dxUv, dyUv, &stale_moving_pos)) return 77;

    std::array<float, 68> self_cb2_missing = self_cb2_whole;
    std::memcpy(&self_cb2_missing[56], &ws_cb2[56], 12 * sizeof(float));
    if (!edpe::cobra_camera::repairRowMajorDeferredMissingPhase(
            self_cb2_missing.data(), self_cb2_missing.size(), ws_cb1_jit_cm, dxUv, dyUv, &stale_moving_pos)) return 78;

    // Inconsistent InvView/WorldView must fail extraction and reject stale external pos.
    std::array<float, 68> bad_self_cb2 = self_cb2;
    bad_self_cb2[11] += 1.0f;
    if (edpe::cobra_camera::extractWorldCamPosFromDeferredCb2(
            bad_self_cb2.data(), bad_self_cb2.size(), &extracted_pos)) return 79;
    if (edpe::cobra_camera::repairRowMajorDeferredWholeOmission(
            bad_self_cb2.data(), bad_self_cb2.size(), ws_cb1_col_major, inv_jitter, dxUv, dyUv, &stale_moving_pos)) return 80;

    // Captured 208-byte CB0 transpose Model*ViewProj layouts (edo(issue11).rdc):
    // Planet (15736, translated), Atmosphere (16044, scaled by 492878 + translated),
    // Terrain Proj (6796, identity rot), and Viewmodel Atmosphere (16208, sub-pass depthB/FOV).
    const edpe::CameraProjection worldCamRef{
        {-0.385895f, 0.489945f, -0.78169f, -112.6217f,
         -0.876311f, -0.459539f, 0.144578f, 180.6429f,
         -0.288382f, 0.740796f, 0.606678f, 193.4490f},
        1.0525f, 1.871f, 0.025f};
    const float planetMvpT[16]{
        -0.40611f, -0.922217f, -0.303488f, 179.564621f,
         0.916597f, -0.859747f,  1.385995f,  -9.583491f,
         0.0f,       0.0f,       0.0f,        0.025f,
        -0.781705f,  0.144592f,  0.606655f, -231.513412f};
    const float atmoMvpT[16]{
        -200162.859f, -454540.468f, -149582.796f, 0.0f,
         451770.437f, -423750.187f,  683126.375f, -724944.375f,
         0.0f,         0.0f,         0.0f,        0.025f,
        -385285.187f,  71266.414f,   299007.062f, 22424.172f};
    const float terrainProjT[16]{
        1.052385f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.870908f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.025f,
        0.0f, 0.0f, 1.0f, 0.0f};
    if (!edpe::matchesEliteTransposeProjectionBlock(planetMvpT, worldCamRef) ||
        !edpe::matchesEliteTransposeProjectionBlock(atmoMvpT, worldCamRef) ||
        !edpe::matchesEliteTransposeProjectionBlock(terrainProjT, worldCamRef)) return 81;

    float atmoJittered[16]{};
    const auto cb0Jit = edpe::projectionJitter(4, 3440, 1440);
    if (!edpe::jitterEliteDotProjectionBlock(atmoMvpT, atmoJittered, cb0Jit) ||
        !edpe::matchesEliteTransposeProjectionBlock(atmoJittered, worldCamRef)) return 82;
    edpe::NdcOffset atmoRecovered{};
    if (!edpe::estimateEliteTransposeBlockNdcOffset(atmoJittered, &atmoRecovered) ||
        std::fabs(atmoRecovered.x - cb0Jit.ndcX) > 1e-4f ||
        std::fabs(atmoRecovered.y - cb0Jit.ndcY) > 1e-4f) return 83;

    // Viewmodel 0.0675 / 1.2935 block (16208) must fail world 0.025 / 1.0525 ref,
    // pass viewmodel sub-pass ref, and mirrored reflection (det < 0) must be rejected.
    const float vmMvpT[16]{
        -0.499218f, -1.13365f, -0.373068f, 0.0f,
         1.126741f, -1.056857f, 1.703756f, 0.0f,
         0.0f,       0.0f,      0.0f,      0.0675f,
        -0.781705f,  0.144592f, 0.606655f, 0.0f};
    if (edpe::matchesEliteTransposeProjectionBlock(vmMvpT, worldCamRef)) return 84;
    edpe::CameraProjection vmCamRef = worldCamRef;
    vmCamRef.scaleX = 1.2935f;
    vmCamRef.scaleY = 2.300f;
    vmCamRef.depthB = 0.0675f;
    if (!edpe::matchesEliteTransposeProjectionBlock(vmMvpT, vmCamRef)) return 85;
    float mirroredMvpT[16]{};
    std::memcpy(mirroredMvpT, planetMvpT, sizeof(mirroredMvpT));
    mirroredMvpT[0] = -mirroredMvpT[0];
    mirroredMvpT[1] = -mirroredMvpT[1];
    mirroredMvpT[2] = -mirroredMvpT[2];
    if (edpe::matchesEliteTransposeProjectionBlock(mirroredMvpT, worldCamRef)) return 86;

    // Captured 192-byte CB0 transpose ViewProj layout (EID 4379 forward hair shading):
    const float hairCb0T[16]{
        -0.407122f, -0.964933f, -0.103300f, 0.0f,
        -0.009628f,  0.203164f, -1.859819f, -8.34e-7f,
         0.0f,       0.0f,       0.0f,       0.025f,
         0.922126f, -0.384059f, -0.046728f, 0.0f};
    if (!edpe::matchesEliteTransposeProjectionBlock(hairCb0T, worldCamRef)) return 87;
    float hairJittered[16]{};
    if (!edpe::jitterEliteDotProjectionBlock(hairCb0T, hairJittered, cb0Jit) ||
        !edpe::matchesEliteTransposeProjectionBlock(hairJittered, worldCamRef)) return 88;
    edpe::NdcOffset hairRecovered{};
    if (!edpe::estimateEliteTransposeBlockNdcOffset(hairJittered, &hairRecovered) ||
        std::fabs(hairRecovered.x - cb0Jit.ndcX) > 1e-4f ||
        std::fabs(hairRecovered.y - cb0Jit.ndcY) > 1e-4f) return 89;

    // Planetary surface cb1 layout (EDO1.rdc / EDO2.rdc):
    // cb1[233..235] (words 932..943) holds the planetary horizon rotation + true camera translation,
    // cb1[241] (words 964..967) duplicates the true camera translation,
    // cb1[277..279] (words 1108..1119) holds the active pass's view rotation rows, and
    // cb1[35..38] (words 140..155) holds the Main World Camera inverse projection basis even during ADS viewmodel passes.
    std::array<float, 1344> planetCb1{};
    const float horizonRows[12]{
        0.075821f, 0.534115f,  0.842005f, -168.080521f,
        0.856850f, -0.466772f, 0.218934f,  357.418030f,
        0.509960f, 0.704872f, -0.493048f,  333.031158f};
    std::memcpy(&planetCb1[932], horizonRows, sizeof(horizonRows));
    planetCb1[964] = -168.080521f;
    planetCb1[965] =  357.418030f;
    planetCb1[966] =  333.031158f;
    planetCb1[967] =  0.0f;

    const float yaw = 0.7f, pitch = -0.25f;
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float viewR[3][3]{
        {cy, sy * sp, sy * cp},
        {0.0f, cp, -sp},
        {-sy, cy * sp, cy * cp}};
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row) {
            planetCb1[1108 + col * 4 + row] = viewR[row][col];
        }
        planetCb1[1108 + col * 4 + 3] = 0.0f;
    }

    const float worldSx = 1.263376f, worldSy = 2.246002f, worldDb = 0.025f;
    for (int row = 0; row < 3; ++row) {
        planetCb1[140 + row] = viewR[row][0] / worldSx;
        planetCb1[144 + row] = viewR[row][1] / worldSy;
        planetCb1[152 + row] = viewR[row][2];
    }
    planetCb1[151] = 1.0f / worldDb;

    auto writeProj1080 = [&](float sx, float syScale, float db) {
        for (int row = 0; row < 3; ++row) {
            planetCb1[1080 + row * 4 + 0] = viewR[row][0] * sx;
            planetCb1[1080 + row * 4 + 1] = viewR[row][1] * syScale;
            planetCb1[1080 + row * 4 + 2] = 0.0f;
            planetCb1[1080 + row * 4 + 3] = viewR[row][2];
        }
        planetCb1[1080 + 12] = 0.0f;
        planetCb1[1080 + 13] = 0.0f;
        planetCb1[1080 + 14] = db;
        planetCb1[1080 + 15] = 0.0f;
    };

    writeProj1080(worldSx, worldSy, worldDb);
    edpe::CameraProjection planetParsed{};
    int planetFail = 0;
    if (edpe::parseEliteCameraBlocks(&planetCb1[932], &planetCb1[1080], &planetParsed, &planetFail) ||
        planetFail != 7) return 90;
    if (!edpe::parseEliteCamera(planetCb1.data(), planetCb1.size(), &planetParsed, &planetFail) ||
        planetFail != 0) return 91;
    if (std::fabs(planetParsed.scaleX - worldSx) > 1e-4f ||
        std::fabs(planetParsed.scaleY - worldSy) > 1e-4f ||
        std::fabs(planetParsed.depthB - worldDb) > 1e-5f ||
        std::fabs(planetParsed.worldFromView[3] - (-168.080521f)) > 1e-3f ||
        std::fabs(planetParsed.worldFromView[7] - 357.418030f) > 1e-3f ||
        std::fabs(planetParsed.worldFromView[11] - 333.031158f) > 1e-3f ||
        std::fabs(planetParsed.worldFromView[2] - viewR[0][2]) > 1e-4f) return 92;

    planetCb1[964] += 10.0f;
    if (edpe::parseEliteCamera(planetCb1.data(), planetCb1.size(), &planetParsed, &planetFail)) return 93;
    planetCb1[964] -= 10.0f;

    float extSx = 0.0f, extSy = 0.0f, extDb = 0.0f;
    if (!edpe::extractEliteWorldProjectionScales(planetCb1.data(), planetCb1.size(), &extSx, &extSy, &extDb) ||
        std::fabs(extSx - worldSx) > 1e-4f ||
        std::fabs(extSy - worldSy) > 1e-4f ||
        std::fabs(extDb - worldDb) > 1e-5f) return 94;
    if (!edpe::matchesEliteWorldProjectionScales(planetCb1.data(), planetCb1.size(), planetParsed) ||
        edpe::isEliteSubpassVersusWorldCb1(planetCb1.data(), planetCb1.size(), planetParsed)) return 95;

    // ADS viewmodel sub-pass shares depthB=0.025 with world, but has scaleX=3.190096, scaleY=5.671282.
    writeProj1080(3.190096f, 5.671282f, 0.025f);
    edpe::CameraProjection adsVmParsed{};
    if (!edpe::parseEliteCamera(planetCb1.data(), planetCb1.size(), &adsVmParsed, &planetFail) ||
        edpe::matchesEliteWorldProjectionScales(planetCb1.data(), planetCb1.size(), adsVmParsed) ||
        !edpe::isEliteSubpassVersusWorldCb1(planetCb1.data(), planetCb1.size(), adsVmParsed)) return 96;

    // Square cubemap frustum (1:1 aspect) must not be classified as a scene sub-pass.
    edpe::CameraProjection squareCubemap = planetParsed;
    squareCubemap.scaleX = 1.0f;
    squareCubemap.scaleY = 1.0f;
    if (edpe::isEliteSubpassVersusWorldCb1(planetCb1.data(), planetCb1.size(), squareCubemap)) return 97;

    // -------------------------------------------------------------------------
    // Stage 1: Halton (2,3) phase scaling formula (computeEffectiveJitterPhaseCount)
    // -------------------------------------------------------------------------
    // Base = 8 (edpe.ini default): scales strictly with ceil(8 * scale^2)
    if (edpe::computeEffectiveJitterPhaseCount(8, 2560, 1440, 2560, 1440) != 8u) return 110;   // 1.0x DLAA
    if (edpe::computeEffectiveJitterPhaseCount(8, 1707, 960, 2560, 1440) != 18u) return 111;   // 1.5x Quality
    if (edpe::computeEffectiveJitterPhaseCount(8, 1506, 847, 2560, 1440) != 24u) return 112;   // 1.7x Balanced
    if (edpe::computeEffectiveJitterPhaseCount(8, 1280, 720, 2560, 1440) != 32u) return 113;   // 2.0x Performance
    if (edpe::computeEffectiveJitterPhaseCount(8, 1280, 720, 3840, 2160) != 72u) return 114;   // 3.0x UltraPerformance

    // Base = 32: does NOT multiply 32 * scale^2 (avoids 128/256 phases!)
    if (edpe::computeEffectiveJitterPhaseCount(32, 2560, 1440, 2560, 1440) != 32u) return 115; // 1.0x DLAA
    if (edpe::computeEffectiveJitterPhaseCount(32, 1707, 960, 2560, 1440) != 32u) return 116;  // 1.5x Quality
    if (edpe::computeEffectiveJitterPhaseCount(32, 1506, 847, 2560, 1440) != 32u) return 117;  // 1.7x Balanced
    if (edpe::computeEffectiveJitterPhaseCount(32, 1280, 720, 2560, 1440) != 32u) return 118;  // 2.0x Performance
    if (edpe::computeEffectiveJitterPhaseCount(32, 1280, 720, 3840, 2160) != 72u) return 119;  // 3.0x UltraPerformance

    // Asymmetric axis ratio uses max(scW/rW, scH/rH) and clamps [8, max(64, min_phases)]
    if (edpe::computeEffectiveJitterPhaseCount(8, 1280, 1440, 2560, 1440) != 32u) return 120;
    if (edpe::computeEffectiveJitterPhaseCount(4, 2560, 1440, 2560, 1440) != 8u) return 121;
    if (edpe::computeEffectiveJitterPhaseCount(128, 2560, 1440, 2560, 1440) != 64u) return 122;

    // Default (basePhases == 0) defaults to 8 at NativeAA (1.0x) and scales via ceil(8 * scale^2)
    if (edpe::computeEffectiveJitterPhaseCount(0, 2560, 1440, 2560, 1440) != 8u) return 1221;
    if (edpe::computeEffectiveJitterPhaseCount(0, 1707, 960, 2560, 1440) != 18u) return 1222;
    if (edpe::computeEffectiveJitterPhaseCount(0, 1280, 720, 2560, 1440) != 32u) return 1223;
    if (edpe::computeEffectiveJitterPhaseCount(0, 1280, 720, 3840, 2160) != 72u) return 1224;

    // Karis reversible tone compression + YCoCg round-trip & R11G11B10_FLOAT safe clamp verification
    {
        auto tonemapKaris = [](float r, float g, float b, float& outR, float& outG, float& outB) {
            r = (r > 0.0f) ? r : 0.0f;
            g = (g > 0.0f) ? g : 0.0f;
            b = (b > 0.0f) ? b : 0.0f;
            const float peak = (std::max)(r, (std::max)(g, b));
            const float inv = 1.0f / (1.0f + peak);
            outR = r * inv; outG = g * inv; outB = b * inv;
        };
        auto rgbToYCoCg = [](float r, float g, float b, float& y, float& co, float& cg) {
            y  =  0.25f * r + 0.5f * g + 0.25f * b;
            co =  0.5f  * r            - 0.5f  * b;
            cg = -0.25f * r + 0.5f * g - 0.25f * b;
        };
        auto yCoCgToRgb = [](float y, float co, float cg, float& r, float& g, float& b) {
            r = (std::max)(y + co - cg, 0.0f);
            g = (std::max)(y + cg,      0.0f);
            b = (std::max)(y - co - cg, 0.0f);
        };
        auto invTonemapKaris = [](float r, float g, float b, float& outR, float& outG, float& outB) {
            r = (r > 0.0f) ? r : 0.0f;
            g = (g > 0.0f) ? g : 0.0f;
            b = (b > 0.0f) ? b : 0.0f;
            const float peak = (std::max)(r, (std::max)(g, b));
            const float denom = (std::max)(1.0f - peak, 1.0f / 65504.0f);
            outR = (std::min)(r / denom, 65024.0f);
            outG = (std::min)(g / denom, 65024.0f);
            outB = (std::min)(b / denom, 64512.0f);
        };

        // HDR value > 1.0 must round-trip without clipping to 1.0
        float kr, kg, kb, y, co, cg, ur, ug, ub, fr, fg, fb;
        tonemapKaris(12.5f, 3.25f, 0.75f, kr, kg, kb);
        rgbToYCoCg(kr, kg, kb, y, co, cg);
        yCoCgToRgb(y, co, cg, ur, ug, ub);
        invTonemapKaris(ur, ug, ub, fr, fg, fb);
        if (!std::isfinite(fr) || !std::isfinite(fg) || !std::isfinite(fb)) return 1225;
        if (std::fabs(fr - 12.5f) > 1e-3f || std::fabs(fg - 3.25f) > 1e-3f || std::fabs(fb - 0.75f) > 1e-3f) return 1226;

        // Extreme HDR / saturated compressed value must clamp below R11G11B10_FLOAT max with no NaN/Inf
        invTonemapKaris(1.0f, 1.0f, 1.0f, fr, fg, fb);
        if (!std::isfinite(fr) || !std::isfinite(fg) || !std::isfinite(fb)) return 1227;
        if (fr > 65024.0f || fg > 65024.0f || fb > 64512.0f) return 1228;
    }

    // Status.json Flags bit 26 (InSRV), Flags2 (OnFoot), and GuiFocus (6..9) parsing + cockpit clamp limits
    {
        // Ship cockpit (neither OnFoot nor InSRV)
        const auto ship = edpe::parseEliteStatusJsonFlags(
            R"({"Flags":16777224,"Flags2":0,"GuiFocus":0})");
        if (ship.mapGuiFocus || ship.onFoot || ship.inSrv) return 1230;
        if (edpe::computeCockpitClampMaxMeters(ship.onFoot, ship.inSrv) != 150.0f) return 1231;

        // InSRV (Flags bit 26 = 67108864 = 0x04000000) -> caps near-field split to 3.5 m
        const auto srv = edpe::parseEliteStatusJsonFlags(
            R"({"Flags":67108864,"Flags2":0,"GuiFocus":0})");
        if (srv.mapGuiFocus || srv.onFoot || !srv.inSrv) return 1232;
        if (edpe::computeCockpitClampMaxMeters(srv.onFoot, srv.inSrv) != 3.5f) return 1233;

        // OnFoot (Flags2 bit 0 = 1) -> disables cockpit clamping (0.0 m)
        const auto foot = edpe::parseEliteStatusJsonFlags(
            R"({"Flags":0,"Flags2":1,"GuiFocus":0})");
        if (foot.mapGuiFocus || !foot.onFoot || foot.inSrv) return 1234;
        if (edpe::computeCockpitClampMaxMeters(foot.onFoot, foot.inSrv) != 0.0f) return 1235;

        // Map/FSS modes (GuiFocus 6..9)
        for (int gf = 6; gf <= 9; ++gf) {
            char json[96]{};
            std::snprintf(json, sizeof(json), R"({"Flags":0,"Flags2":0,"GuiFocus":%d})", gf);
            const auto mapState = edpe::parseEliteStatusJsonFlags(json);
            if (!mapState.mapGuiFocus) return 1236;
        }
        const auto cockpitPanel = edpe::parseEliteStatusJsonFlags(
            R"({"Flags":0,"Flags2":0,"GuiFocus":1})");
        if (cockpitPanel.mapGuiFocus) return 1237;

        // Seated flags (InMainShip / InSRV / InTaxi) must suppress stale Flags2 on-foot bits
        const auto seatedShipStale = edpe::parseEliteStatusJsonFlags(
            R"({"Flags":16777216,"Flags2":8192,"GuiFocus":0})");
        if (seatedShipStale.onFoot || seatedShipStale.inSrv) return 1238;

        const auto seatedTaxiStale = edpe::parseEliteStatusJsonFlags(
            R"({"Flags":0,"Flags2":8194,"GuiFocus":0})");
        if (seatedTaxiStale.onFoot) return 1239;
    }

    // 6-DOF cameraDepthMotion accuracy across individual & simultaneous translations and rotations
    {
        auto makeCam = [](float px, float py, float pz, float yaw, float pitch, float roll) {
            const float cy = std::cos(yaw), sy = std::sin(yaw);
            const float cp = std::cos(pitch), sp = std::sin(pitch);
            const float cr = std::cos(roll), sr = std::sin(roll);
            edpe::CameraProjection c{};
            c.worldFromView[0] = cy * cr + sy * sp * sr;
            c.worldFromView[1] = -cy * sr + sy * sp * cr;
            c.worldFromView[2] = sy * cp;
            c.worldFromView[3] = px;
            c.worldFromView[4] = cp * sr;
            c.worldFromView[5] = cp * cr;
            c.worldFromView[6] = -sp;
            c.worldFromView[7] = py;
            c.worldFromView[8] = -sy * cr + cy * sp * sr;
            c.worldFromView[9] = sy * sr + cy * sp * cr;
            c.worldFromView[10] = cy * cp;
            c.worldFromView[11] = pz;
            c.scaleX = 1.2f;
            c.scaleY = 1.6f;
            c.depthB = 0.025f;
            return c;
        };
        struct Case { float vx, vy, vz, yaw, pitch, roll; };
        const Case cases[] = {
            {0.16f, 0.0f,  0.0f,  0.0f,   0.0f,   0.0f},   // TranslateX
            {0.0f,  0.12f, 0.0f,  0.0f,   0.0f,   0.0f},   // TranslateY
            {0.0f,  0.0f,  0.22f, 0.0f,   0.0f,   0.0f},   // TranslateZ
            {0.0f,  0.0f,  0.0f,  0.032f, 0.0f,   0.0f},   // RotateYaw
            {0.0f,  0.0f,  0.0f,  0.0f,   0.025f, 0.0f},   // RotatePitch
            {0.0f,  0.0f,  0.0f,  0.0f,   0.0f,   0.035f}, // RotateRoll
            {0.12f, 0.06f, 0.15f, 0.024f, 0.015f, 0.020f}, // Combined6DOF
        };
        const auto prevCam = makeCam(0.0f, 0.0f, 0.0f, 0.0f, -0.10f, 0.0f);
        const float wx = 1.25f, wy = -1.5f, wz = 8.0f;
        // Project world point into prevCam
        const auto projectPoint = [](const edpe::CameraProjection& c, float x, float y, float z,
                                     float& outPx, float& outPy, float& outDepth) -> bool {
            const float dx = x - c.worldFromView[3];
            const float dy = y - c.worldFromView[7];
            const float dz = z - c.worldFromView[11];
            const float vx = c.worldFromView[0] * dx + c.worldFromView[4] * dy + c.worldFromView[8] * dz;
            const float vy = c.worldFromView[1] * dx + c.worldFromView[5] * dy + c.worldFromView[9] * dz;
            const float vz = c.worldFromView[2] * dx + c.worldFromView[6] * dy + c.worldFromView[10] * dz;
            if (vz <= 0.1f) return false;
            outPx = (c.scaleX * vx / vz + 1.0f) * 128.0f * 0.5f - 0.5f;
            outPy = (1.0f - c.scaleY * vy / vz) * 96.0f * 0.5f - 0.5f;
            outDepth = c.depthB / vz;
            return true;
        };
        float prevPx = 0.0f, prevPy = 0.0f, prevDepth = 0.0f;
        if (!projectPoint(prevCam, wx, wy, wz, prevPx, prevPy, prevDepth)) return 1240;
        for (int ci = 0; ci < 7; ++ci) {
            const auto& cs = cases[ci];
            const auto nowCam = makeCam(cs.vx, cs.vy, cs.vz, cs.yaw, -0.10f + cs.pitch, cs.roll);
            float curPx = 0.0f, curPy = 0.0f, curDepth = 0.0f;
            if (!projectPoint(nowCam, wx, wy, wz, curPx, curPy, curDepth)) return 1241 + ci;
            float mvX = 0.0f, mvY = 0.0f;
            if (!edpe::cameraDepthMotion(nowCam, prevCam, 128u, 96u, curPx, curPy, curDepth, &mvX, &mvY)) {
                return 1250 + ci;
            }
            if (std::fabs((curPx + mvX) - prevPx) > 1e-3f || std::fabs((curPy + mvY) - prevPy) > 1e-3f) {
                return 1260 + ci;
            }
        }
    }

    // -------------------------------------------------------------------------
    // Stage 3: Per-frame phase consistency audit (checkRowPair & PhasePairAuditor)
    // -------------------------------------------------------------------------
    {
        const auto j0 = edpe::projectionJitter(0, 2560, 1440, 32);
        const auto j1 = edpe::projectionJitter(1, 2560, 1440, 32);
        const auto j2 = edpe::projectionJitter(2, 2560, 1440, 32);
        float blk0[16]{}, blk1[16]{}, blk2[16]{}, blk1Double[16]{};
        if (!edpe::jitterEliteProjectionBlock(turning_block, blk0, j0)) return 123;
        if (!edpe::jitterEliteProjectionBlock(turning_block, blk1, j1)) return 124;
        if (!edpe::jitterEliteProjectionBlock(turning_block, blk2, j2)) return 125;
        if (!edpe::jitterEliteProjectionBlock(blk1, blk1Double, j1)) return 126;

        // 1. Consistent consecutive phases: must match within 2e-6 NDC
        const auto okRes = edpe::checkRowPair(blk1, blk0, j1, j0, 2.0e-6f);
        if (!okRes.valid || !okRes.matched || okRes.maxAbsError > 2.0e-6f) return 127;

        // 2. Double jitter on current frame: must fail
        const auto dblRes = edpe::checkRowPair(blk1Double, blk0, j1, j0, 2.0e-6f);
        if (!dblRes.valid || dblRes.matched) return 128;

        // 3. Missed phase (unjittered turning_block when j1 expected): must fail
        const auto missRes = edpe::checkRowPair(turning_block, blk0, j1, j0, 2.0e-6f);
        if (!missRes.valid || missRes.matched) return 129;

        // 4. Stateful PhasePairAuditor tracking + reset
        edpe::PhasePairAuditor auditor;
        const auto r0 = auditor.recordFrame(blk0, j0);
        if (r0.valid || auditor.checkedCount() != 0 || auditor.mismatchedCount() != 0) return 130;
        const auto r1 = auditor.recordFrame(blk1, j1);
        if (!r1.valid || !r1.matched || auditor.checkedCount() != 1 || auditor.mismatchedCount() != 0) return 131;
        const auto r2 = auditor.recordFrame(blk1Double, j2);
        if (!r2.valid || r2.matched || auditor.checkedCount() != 2 || auditor.mismatchedCount() != 1) return 132;
        auditor.reset();
        const auto r3 = auditor.recordFrame(blk2, j2);
        if (r3.valid || auditor.checkedCount() != 2 || auditor.mismatchedCount() != 1) return 133;
    }

    // -------------------------------------------------------------------------
    // Stage 3: Deferred CB2 float32Gamma & UV->Clip non-degeneracy certification
    // -------------------------------------------------------------------------
    {
        const double u32 = 5.960464477539063e-8;
        const double expectedGamma32 = (32.0 * u32) / (1.0 - 32.0 * u32);
        if (std::fabs(static_cast<double>(edpe::cobra_camera::float32Gamma(32)) - expectedGamma32) > 1e-9) {
            return 134;
        }
        float normInf = 99.0f;
        if (!edpe::cobra_camera::certifyDeferredUvToClipNonDegenerate(ws_rows10to16, true, &normInf) ||
            normInf >= 0.01f) {
            return 135;
        }
        // Degenerate CB2 (zeroed dY column) must be rejected without patching
        std::array<float, 68> degen_cb2 = ws_cb2;
        degen_cb2[57] = 0.0f;
        degen_cb2[61] = 0.0f;
        degen_cb2[65] = 0.0f;
        float degenNorm = 0.0f;
        if (edpe::cobra_camera::certifyDeferredUvToClipNonDegenerate(&degen_cb2[40], true, &degenNorm) ||
            degenNorm < 1.0f) {
            return 136;
        }
        if (edpe::cobra_camera::repairRowMajorDeferredWholeOmission(
                degen_cb2.data(), degen_cb2.size(), ws_cb1_col_major, inv_jitter, dxUv, dyUv, &ws_cam_pos)) {
            return 137;
        }
    }

    // -------------------------------------------------------------------------
    // Stage 2: flushCamera state machine, WriteFaultTracker & prologue witness check
    // -------------------------------------------------------------------------
    {
        edpe::cobra_camera::InjectedCameraSet camSet;
        int dummyCamA = 0, dummyCamB = 0;
        if (!camSet.empty() || camSet.size() != 0) return 138;
        camSet.recordInjected(&dummyCamA);
        camSet.recordInjected(&dummyCamA); // duplicate must not double-count
        if (camSet.empty() || camSet.size() != 1 || !camSet.contains(&dummyCamA)) return 139;
        camSet.recordInjected(&dummyCamB);
        if (camSet.size() != 2) return 140;
        if (!camSet.consumeForFlush(&dummyCamA)) return 141; // one-shot flush for camA
        if (camSet.consumeForFlush(&dummyCamA)) return 142;  // second call must be no-op
        if (camSet.empty() || camSet.size() != 1) return 143; // gate stays open until camB flushes
        if (!camSet.consumeForFlush(&dummyCamB)) return 144;
        if (!camSet.empty() || camSet.size() != 0) return 145; // safe to close g_gate now

        edpe::cobra_camera::WriteFaultTracker tracker;
        // 7 faults within 5s must NOT stand down
        for (uint64_t i = 0; i < 7; ++i) {
            if (tracker.recordFault(10000 + i * 500)) return 146;
        }
        // Fault after >5s window expires old entries -> still NOT stood down
        if (tracker.recordFault(20000)) return 147;
        tracker.reset();
        // 8 faults within 5000ms -> permanent stand-down
        for (uint64_t i = 0; i < 7; ++i) {
            if (tracker.recordFault(50000 + i * 100)) return 148;
        }
        if (!tracker.recordFault(50700) || !tracker.isStoodDown()) return 149;

        // Prologue + camera field offset witnesses (+0x250 & +0x28C)
        uint8_t validCode[64]{};
        validCode[0] = 0x48; validCode[1] = 0x89; validCode[2] = 0x5C; validCode[3] = 0x24; validCode[4] = 0x08;
        const uint32_t offFlags = 0x250u, offBound = 0x28Cu;
        std::memcpy(validCode + 16, &offFlags, sizeof(offFlags));
        std::memcpy(validCode + 32, &offBound, sizeof(offBound));
        if (!edpe::cobra_camera::verifyRefreshFunctionBytes(validCode, sizeof(validCode))) return 150;

        uint8_t missingWitness[64]{};
        missingWitness[0] = 0x48; missingWitness[1] = 0x89; missingWitness[2] = 0x5C;
        if (edpe::cobra_camera::verifyRefreshFunctionBytes(missingWitness, sizeof(missingWitness))) return 151;

        uint8_t badPrologue[64]{};
        badPrologue[0] = 0xCC; badPrologue[1] = 0xCC;
        std::memcpy(badPrologue + 16, &offFlags, sizeof(offFlags));
        std::memcpy(badPrologue + 32, &offBound, sizeof(offBound));
        if (edpe::cobra_camera::verifyRefreshFunctionBytes(badPrologue, sizeof(badPrologue))) return 152;

        // Exact .pdata RUNTIME_FUNCTION entry checks
        size_t funcBytes = 0;
        constexpr uintptr_t kFakeBase = 0x140000000ull;
        if (!edpe::cobra_camera::isExactFunctionEntry(
                kFakeBase + 0x05921F0u, kFakeBase, 0x05921F0u, 0x0592490u, &funcBytes) ||
            funcBytes != 0x2A0u) {
            return 153;
        }
        if (edpe::cobra_camera::isExactFunctionEntry(
                kFakeBase + 0x0592200u, kFakeBase, 0x05921F0u, 0x0592490u, &funcBytes)) {
            return 154;
        }
        if (edpe::cobra_camera::isExactFunctionEntry(
                kFakeBase + 0x05921F0u, kFakeBase, 0x05921F0u, 0x05921F0u, &funcBytes)) {
            return 155;
        }
        if (edpe::cobra_camera::isExactFunctionEntry(
                kFakeBase + 0x05921F0u, kFakeBase, 0x0592490u, 0x05921F0u, &funcBytes)) {
            return 156;
        }
    }

    // -------------------------------------------------------------------------
    // Issue #24: System Map stale cb1[35..38] orientation & large cb0 translation
    // -------------------------------------------------------------------------
    {
        // System Map telephoto camera from sysmap_upscaled.rdc (EID 1356):
        // right=(1,0,0), up=(0, 0.087156, 0.996195), forward=(0, -0.996195, 0.087156)
        std::array<float, 1344> sysCb1{};
        const float sysRot[3][3]{
            {1.0f, 0.0f, 0.0f},
            {0.0f, 0.087156f, -0.996195f},
            {0.0f, 0.996195f, 0.087156f}};
        const float sysSx = 4.476538f, sysSy = 7.958291f, sysDb = 0.025f;
        for (size_t row = 0; row < 3; ++row) {
            sysCb1[932 + row * 4 + 0] = sysRot[row][0];
            sysCb1[932 + row * 4 + 1] = sysRot[row][1];
            sysCb1[932 + row * 4 + 2] = sysRot[row][2];
            sysCb1[932 + row * 4 + 3] = 0.0f;
            sysCb1[1080 + row * 4 + 0] = sysRot[row][0] * sysSx;
            sysCb1[1080 + row * 4 + 1] = sysRot[row][1] * sysSy;
            sysCb1[1080 + row * 4 + 2] = 0.0f;
            sysCb1[1080 + row * 4 + 3] = sysRot[row][2];
        }
        sysCb1[1080 + 14] = sysDb;

        // Stale space/cockpit camera left in cb1[35..38] (words 140..155), ~133 deg rotated away:
        const float stalePitch = 2.3f;
        const float cp = std::cos(stalePitch), sp = std::sin(stalePitch);
        const float staleRot[3][3]{
            {1.0f, 0.0f, 0.0f},
            {0.0f, cp, -sp},
            {0.0f, sp, cp}};
        auto writeStaleWorld140 = [&](float wsx, float wsy, float wdb) {
            for (size_t row = 0; row < 3; ++row) {
                sysCb1[140 + row] = staleRot[row][0] / wsx;
                sysCb1[144 + row] = staleRot[row][1] / wsy;
                sysCb1[152 + row] = staleRot[row][2];
            }
            sysCb1[151] = 1.0f / wdb;
        };
        writeStaleWorld140(0.9865f, 1.7538f, 0.025f);

        edpe::CameraProjection sysCam{};
        if (!edpe::parseEliteCamera(sysCb1.data(), sysCb1.size(), &sysCam)) return 153;
        if (edpe::isEliteSubpassVersusWorldCb1(sysCb1.data(), sysCb1.size(), sysCam)) return 154;

        // Stale rotated cb1[35..38] whose zoom matches sysCam must not match as world projection.
        writeStaleWorld140(sysSx, sysSy, sysDb);
        if (edpe::matchesEliteWorldProjectionScales(sysCb1.data(), sysCb1.size(), sysCam)) return 155;

        // Galaxy Map stars/lines cb0[4..7] (large translation) vs camera-relative grid (small).
        const float galStarsDot16[16]{
            -0.785879f, 0.000178f, -0.699936f, 56146.945312f,
             0.654531f, 1.591353f, -0.734494f, -80047.601562f,
             0.0f,      0.0f,       0.0f,       0.025f,
             0.565788f, -0.526092f, -0.63491f,  8569.803711f};
        const float galGridDot16[16]{
            -0.785879f, 0.000178f, -0.699936f, 0.0f,
             0.654531f, 1.591353f, -0.734494f, -0.007812f,
             0.0f,      0.0f,       0.0f,       0.025f,
             0.565788f, -0.526092f, -0.63491f,  0.0f};
        const float smallSceneDot16[16]{
            -0.785879f, 0.000178f, -0.699936f, 12.0f,
             0.654531f, 1.591353f, -0.734494f, -8.0f,
             0.0f,      0.0f,       0.0f,       0.025f,
             0.565788f, -0.526092f, -0.63491f,  22.0f};
        if (!edpe::hasLargeEliteTransposeTranslation(galStarsDot16) ||
            edpe::hasLargeEliteTransposeTranslation(galGridDot16) ||
            edpe::hasLargeEliteTransposeTranslation(smallSceneDot16) ||
            edpe::hasLargeEliteTransposeTranslation(nullptr)) return 156;
    }

    // -------------------------------------------------------------------------
    // Issue #30: ADS rifle & hands viewmodel vs world camera with skybox-space
    // cb1[35..38] (words 140..155) and local-to-skybox cb1[64..66] (words 256..267)
    // Captured from EDO2.rdc EIDs 8526..8622 (ADS rifle/hands) and EID 8635 (world).
    // -------------------------------------------------------------------------
    {
        std::array<float, 1344> edo2Cb1{};
        // cb1[35..38] (words 140..155) in skybox space (1/world_scaleX, 1/world_scaleY, 1/world_depthB):
        const float world140[16]{
             0.585254f,  0.517105f, 0.128764f, 0.0f,
            -0.291366f,  0.285239f, 0.178810f, 0.0f,
             0.0f,       0.0f,      0.0f,     40.0f,
             0.158155f, -0.403417f, 0.901245f, 0.0f};
        std::memcpy(&edo2Cb1[140], world140, sizeof(world140));

        // cb1[58..60] (words 232..243): shifted axis block [0,1,0,0; 0,0,1,0; 0,0,0,1] present in EDO2.rdc;
        // words[243] == 1.0f so any regression reading offset 232 fails isValidEliteSkyboxRotation12.
        const float block232[12]{
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};
        std::memcpy(&edo2Cb1[232], block232, sizeof(block232));

        // cb1[64..66] (words 256..267): local-to-skybox basis rotation R_64, followed by cb1[67]=(0,0,0,1)
        const float skybox256[12]{
            -0.987473f, -0.112762f, -0.110369f, 0.0f,
             0.056711f, -0.906380f,  0.418640f, 0.0f,
            -0.147243f,  0.407136f,  0.901421f, 0.0f};
        std::memcpy(&edo2Cb1[256], skybox256, sizeof(skybox256));
        edo2Cb1[271] = 1.0f;
        if (edpe::isValidEliteSkyboxRotation12(&edo2Cb1[232])) return 163;
        if (!edpe::isValidEliteSkyboxRotation12(&edo2Cb1[256])) return 164;

        // cam932 (words 932..943): planetary horizon basis + eye pos in EDO2.rdc
        const float cam932[12]{
            0.075821f,  0.534115f,  0.842005f, -168.080521f,
            0.856850f, -0.466772f,  0.218934f,  357.418030f,
            0.509960f,  0.704872f, -0.493048f,  333.031158f};
        std::memcpy(&edo2Cb1[932], cam932, sizeof(cam932));
        // cb1[241].xyz (words 964..966): live eye position matching cam932
        edo2Cb1[964] = -168.080505f;
        edo2Cb1[965] =  357.418030f;
        edo2Cb1[966] =  333.031158f;
        // cb1[275].xyz (words 1100..1102): vertex-shader render origin
        edo2Cb1[1100] = -0.302622f;
        edo2Cb1[1101] =  0.264623f;
        edo2Cb1[1102] = -0.398612f;
        // cb1[277..279] (words 1108..1119): local render-space view rotation rows R_277
        const float rot277[12]{
            -0.821768f, -0.482109f, 0.303756f, 0.0f,
             0.529653f, -0.449661f, 0.719217f, 0.0f,
            -0.210153f,  0.751915f, 0.624868f, 0.0f};
        std::memcpy(&edo2Cb1[1108], rot277, sizeof(rot277));

        auto setProj1080 = [&](float sx, float sy, float db) {
            for (size_t row = 0; row < 3; ++row) {
                edo2Cb1[1080 + row * 4 + 0] = rot277[0 * 4 + row] * sx;
                edo2Cb1[1080 + row * 4 + 1] = rot277[1 * 4 + row] * sy;
                edo2Cb1[1080 + row * 4 + 2] = 0.0f;
                edo2Cb1[1080 + row * 4 + 3] = rot277[2 * 4 + row];
            }
            edo2Cb1[1080 + 12] = 0.0f;
            edo2Cb1[1080 + 13] = 0.0f;
            edo2Cb1[1080 + 14] = db;
            edo2Cb1[1080 + 15] = 0.0f;
        };

        // 1) Main world G-buffer draw (EID 8635): scaleX=1.263396, scaleY=2.246037, depthB=0.025
        setProj1080(1.263396f, 2.246037f, 0.025f);
        edpe::CameraProjection worldCam{};
        if (!edpe::parseEliteCamera(edo2Cb1.data(), edo2Cb1.size(), &worldCam)) return 157;
        if (!edpe::matchesEliteWorldProjectionScales(edo2Cb1.data(), edo2Cb1.size(), worldCam)) return 158;
        if (edpe::isEliteSubpassVersusWorldCb1(edo2Cb1.data(), edo2Cb1.size(), worldCam)) return 159;

        // 2) ADS rifle & hands G-buffer draws (EIDs 8526..8622): same depthB=0.025 as world,
        //    narrower viewmodel FOV scaleX=3.190097, scaleY=5.671283.
        setProj1080(3.190097f, 5.671283f, 0.025f);
        edpe::CameraProjection adsRifleCam{};
        if (!edpe::parseEliteCamera(edo2Cb1.data(), edo2Cb1.size(), &adsRifleCam)) return 160;
        if (edpe::matchesEliteWorldProjectionScales(edo2Cb1.data(), edo2Cb1.size(), adsRifleCam)) return 161;
        if (!edpe::isEliteSubpassVersusWorldCb1(edo2Cb1.data(), edo2Cb1.size(), adsRifleCam)) return 162;
    }

    // Issue #30: Deferred CB0 viewport jitter must apply only to full-resolution viewports.
    if (!edpe::shouldApplyDeferredViewportJitter(2560.0f, 1080.0f, 2560u, 1080u)) return 165;
    if (!edpe::shouldApplyDeferredViewportJitter(2559.5f, 1080.5f, 2560u, 1080u)) return 166;
    if (edpe::shouldApplyDeferredViewportJitter(320.0f, 135.0f, 2560u, 1080u)) return 167;
    if (edpe::shouldApplyDeferredViewportJitter(640.0f, 270.0f, 2560u, 1080u)) return 168;
    if (edpe::shouldApplyDeferredViewportJitter(1280.0f, 540.0f, 2560u, 1080u)) return 169;
    if (edpe::shouldApplyDeferredViewportJitter(32.0f, 32.0f, 2560u, 1080u)) return 170;
    if (edpe::shouldApplyDeferredViewportJitter(2560.0f, 1080.0f, 0u, 1080u)) return 171;
    if (edpe::shouldApplyDeferredViewportJitter(std::numeric_limits<float>::quiet_NaN(), 1080.0f, 2560u, 1080u)) return 172;

    return 0; // All temporal math tests passed
}
