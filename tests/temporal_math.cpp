#include "temporal_math.h"
#include "elite_camera.h"

#include <array>
#include <cmath>

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
    scene[1083] += .01f;
    if (edpe::parseEliteCamera(scene.data(), scene.size(), &parsed)) return 8;
}
