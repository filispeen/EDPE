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
    const std::array<float, 12> base_inv_rays{
         2.0f,  0.1f,  0.2f, 0.0f, // dX
        -0.1f, -1.2f,  0.15f, 0.0f, // dY
        -0.95f, 0.55f, -1.0f, 0.0f  // originCorner
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
         1.8f,  0.2f, 0.0f, rm_unjit_fwd[12] - 0.5f * 1.8f - 0.5f * 0.2f,
        -0.1f, -1.1f, 0.0f, rm_unjit_fwd[13] - 0.5f * (-0.1f) - 0.5f * (-1.1f),
         0.3f, -0.2f, 0.0f, rm_unjit_fwd[14] - 0.5f * 0.3f - 0.5f * (-0.2f)
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

    return 0; // All temporal math tests passed
}
