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

    return 0; // All temporal math tests passed
}
