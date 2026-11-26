#include "motion_pass.h"
#include "taa_pass.h"
#include "temporal_math.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kWidth = 3840;
constexpr uint32_t kHeight = 2160;
constexpr float kScaleX = 0.9f;
constexpr float kScaleY = 1.6f;
constexpr float kDepthB = 0.025f;
constexpr float kFloorY = -2.0f;
constexpr float kBackWallZ = 24.0f;
constexpr float kPillarZ = 6.0f;
constexpr float kPillarHalfW = 0.65f;
constexpr float kPillarMinY = -2.0f;
constexpr float kPillarMaxY = 2.5f;
// Distant thin vertical railing bars at z = 21.5m (~2.5 px wide at 3840x2160)
constexpr float kRailZ = 21.5f;
constexpr float kRailHalfW = 0.015f; // ~2.4 px wide on 4K screen at z=21.5m
constexpr float kRailMinY = -2.0f;
constexpr float kRailMaxY = 1.6f;

struct MotionProfile {
    const char* name;
    float vx, vy, vz;       // world translation per frame (meters)
    float yaw, pitch, roll; // rotation per frame (radians)
};

edpe::CameraProjection buildCamera(float px, float py, float pz, float yaw, float pitch, float roll) {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cr = std::cos(roll), sr = std::sin(roll);

    // R = Ry(yaw) * Rx(pitch) * Rz(roll)
    const float r00 = cy * cr + sy * sp * sr;
    const float r01 = -cy * sr + sy * sp * cr;
    const float r02 = sy * cp;

    const float r10 = cp * sr;
    const float r11 = cp * cr;
    const float r12 = -sp;

    const float r20 = -sy * cr + cy * sp * sr;
    const float r21 = sy * sr + cy * sp * cr;
    const float r22 = cy * cp;

    edpe::CameraProjection cam{};
    cam.worldFromView[0] = r00; cam.worldFromView[1] = r01; cam.worldFromView[2]  = r02; cam.worldFromView[3]  = px;
    cam.worldFromView[4] = r10; cam.worldFromView[5] = r11; cam.worldFromView[6]  = r12; cam.worldFromView[7]  = py;
    cam.worldFromView[8] = r20; cam.worldFromView[9] = r21; cam.worldFromView[10] = r22; cam.worldFromView[11] = pz;
    cam.scaleX = kScaleX;
    cam.scaleY = kScaleY;
    cam.depthB = kDepthB;
    return cam;
}

enum class SurfaceKind : uint8_t {
    BackWall = 0,
    Floor = 1,
    Pillar = 2,
    DistantRail = 3,
};

struct RenderedScene {
    std::vector<uint32_t> color;       // RGBA8_UNORM
    std::vector<float> depth;          // Reversed-Z depthB / viewZ
    std::vector<SurfaceKind> surface;  // Ground-truth surface ID per pixel
    std::vector<float> luma;           // Ground-truth luma per pixel
};

uint8_t toU8(float v) {
    return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

uint32_t packRgba(float r, float g, float b) {
    return static_cast<uint32_t>(toU8(r)) |
           (static_cast<uint32_t>(toU8(g)) << 8) |
           (static_cast<uint32_t>(toU8(b)) << 16) |
           (0xFFu << 24);
}

float unpackLuma(uint32_t rgba) {
    const float r = static_cast<float>(rgba & 0xFFu) / 255.0f;
    const float g = static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f;
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// Ray-casts a synthetic hangar scene (contrasty floor grating + bright foreground pillar + dark back wall)
// with exact subpixel Halton jitter offset applied in screen space.
RenderedScene renderScene(const edpe::CameraProjection& cam, float jitterX, float jitterY) {
    RenderedScene out;
    const size_t n = static_cast<size_t>(kWidth) * kHeight;
    out.color.assign(n, packRgba(0.06f, 0.06f, 0.07f));
    out.depth.assign(n, 0.0f);
    out.surface.assign(n, SurfaceKind::BackWall);
    out.luma.assign(n, 0.06f);

    const float cx = cam.worldFromView[3];
    const float cy = cam.worldFromView[7];
    const float cz = cam.worldFromView[11];

    for (uint32_t iy = 0; iy < kHeight; ++iy) {
        for (uint32_t ix = 0; ix < kWidth; ++ix) {
            // Unjittered pixel center corresponding to rasterized sample (ix + 0.5, iy + 0.5)
            const float ux = (static_cast<float>(ix) + 0.5f) - jitterX;
            const float uy = (static_cast<float>(iy) + 0.5f) - jitterY;
            const float ndcX = 2.0f * ux / static_cast<float>(kWidth) - 1.0f;
            const float ndcY = 1.0f - 2.0f * uy / static_cast<float>(kHeight);

            const float vx = ndcX / cam.scaleX;
            const float vy = ndcY / cam.scaleY;
            const float vz = 1.0f;

            // World ray direction = R * viewRay (t parameter == viewZ!)
            const float dx = cam.worldFromView[0] * vx + cam.worldFromView[1] * vy + cam.worldFromView[2] * vz;
            const float dy = cam.worldFromView[4] * vx + cam.worldFromView[5] * vy + cam.worldFromView[6] * vz;
            const float dz = cam.worldFromView[8] * vx + cam.worldFromView[9] * vy + cam.worldFromView[10] * vz;

            float bestZ = 1e9f;
            SurfaceKind kind = SurfaceKind::BackWall;
            float r = 0.06f, g = 0.06f, b = 0.07f;

            // 1. Back wall plane z = kBackWallZ
            if (std::fabs(dz) > 1e-6f) {
                const float t = (kBackWallZ - cz) / dz;
                if (t > 0.25f && t < bestZ) {
                    bestZ = t;
                    kind = SurfaceKind::BackWall;
                    r = 0.06f; g = 0.06f; b = 0.07f;
                }
            }

            // 2. Hangar floor plane y = kFloorY with high-contrast grating stripes
            if (dy < -1e-6f) {
                const float t = (kFloorY - cy) / dy;
                if (t > 0.25f && t < bestZ) {
                    const float wx = cx + dx * t;
                    const float wz = cz + dz * t;
                    bestZ = t;
                    kind = SurfaceKind::Floor;
                    const int gx = static_cast<int>(std::floor(wx * 2.0f));
                    const int gz = static_cast<int>(std::floor(wz * 1.5f));
                    const bool stripe = ((gx ^ gz) & 1) != 0;
                    const float v = stripe ? 0.58f : 0.08f;
                    r = v; g = v * 0.95f; b = v * 0.85f;
                }
            }

            // 2b. Distant thin 2-3px vertical railing bars at z = kRailZ (x = -4.5..-2.0m, 0.45m spacing)
            if (std::fabs(dz) > 1e-6f) {
                const float t = (kRailZ - cz) / dz;
                if (t > 0.25f && t < bestZ) {
                    const float wx = cx + dx * t;
                    const float wy = cy + dy * t;
                    if (wx >= -4.8f && wx <= -1.8f && wy >= kRailMinY && wy <= kRailMaxY) {
                        const float cell = std::fmod(std::fabs(wx + 10.0f), 0.45f);
                        const float distToBar = (std::min)(cell, 0.45f - cell);
                        if (distToBar <= kRailHalfW) {
                            bestZ = t;
                            kind = SurfaceKind::DistantRail;
                            r = 0.88f; g = 0.82f; b = 0.45f;
                        }
                    }
                }
            }

            // 3. Bright foreground pillar/hull slab at z = kPillarZ
            if (std::fabs(dz) > 1e-6f) {
                const float t = (kPillarZ - cz) / dz;
                if (t > 0.25f && t < bestZ) {
                    const float wx = cx + dx * t;
                    const float wy = cy + dy * t;
                    if (std::fabs(wx) <= kPillarHalfW && wy >= kPillarMinY && wy <= kPillarMaxY) {
                        bestZ = t;
                        kind = SurfaceKind::Pillar;
                        r = 0.94f; g = 0.92f; b = 0.88f;
                    }
                }
            }

            const size_t idx = static_cast<size_t>(iy) * kWidth + ix;
            out.color[idx] = packRgba(r, g, b);
            out.depth[idx] = (bestZ < 1e8f) ? std::clamp(cam.depthB / bestZ, 0.0f, 1.0f) : 0.0f;
            out.surface[idx] = kind;
            out.luma[idx] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }
    }
    return out;
}

bool uploadDepth32G8(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, ID3D11Texture2D* staging,
                     const std::vector<float>& depth) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(staging, 0, D3D11_MAP_WRITE, 0, &m))) return false;
    struct D32S32 { float d; uint32_t s; };
    auto* bytes = static_cast<uint8_t*>(m.pData);
    for (uint32_t y = 0; y < kHeight; ++y) {
        auto* row = reinterpret_cast<D32S32*>(bytes + static_cast<size_t>(y) * m.RowPitch);
        const float* src = depth.data() + static_cast<size_t>(y) * kWidth;
        for (uint32_t x = 0; x < kWidth; ++x) {
            row[x].d = src[x];
            row[x].s = 0u;
        }
    }
    ctx->Unmap(staging, 0);
    ctx->CopyResource(dst, staging);
    return true;
}

bool runMotionProfile(ID3D11Device* device, ID3D11DeviceContext* ctx,
                      edpe::MotionPass& motionPass, edpe::TaaPass& taaPass,
                      ID3D11Texture2D* colorTex, ID3D11ShaderResourceView* colorSrv,
                      ID3D11Texture2D* depthTex[2], ID3D11ShaderResourceView* depthSrv[2],
                      ID3D11Texture2D* depthStaging,
                      ID3D11Texture2D* outTex, ID3D11RenderTargetView* outRtv,
                      ID3D11Texture2D* readbackStaging,
                      const MotionProfile& prof) {
    constexpr int kFrames = 8;
    constexpr int kEvalStart = 6;

    taaPass.resetHistory();

    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float yaw = 0.0f, pitch = -0.10f, roll = 0.0f;

    edpe::CameraProjection prevCam{};
    edpe::ProjectionJitter prevJ{};
    RenderedScene prevScene{};

    float worstDisocclGhost = 0.0f;
    double floorErrSum = 0.0;
    uint64_t floorPixelCount = 0;
    uint64_t disocclPixelCount = 0;
    double railPeakSum = 0.0;
    uint64_t railRowCount = 0;
    std::vector<float> railPrevRowPeaks;
    double railDeltaSqSum = 0.0;
    uint64_t railDeltaCount = 0;
    int curIdx = 0;

    for (int t = 0; t < kFrames; ++t) {
        if (t > 0) {
            px += prof.vx;
            py += prof.vy;
            pz += prof.vz;
            yaw += prof.yaw;
            pitch += prof.pitch;
            roll += prof.roll;
        }

        const auto camNow = buildCamera(px, py, pz, yaw, pitch, roll);
        const auto jit = edpe::projectionJitter(static_cast<uint64_t>(t), kWidth, kHeight, 8u);
        const auto scene = renderScene(camNow, jit.pixelX, jit.pixelY);
        RenderedScene refUnjittered;
        if (t >= kEvalStart - 1) {
            refUnjittered = renderScene(camNow, 0.0f, 0.0f);
        }

        ctx->UpdateSubresource(colorTex, 0, nullptr, scene.color.data(), kWidth * sizeof(uint32_t), 0);
        if (!uploadDepth32G8(ctx, depthTex[curIdx], depthStaging, scene.depth)) {
            return false;
        }

        const bool isReset = (t == 0);
        const auto& camPrev = isReset ? camNow : prevCam;
        ID3D11ShaderResourceView* prevDepthSrv = isReset ? nullptr : depthSrv[curIdx ^ 1];

        // Enable clampCockpit=true + enableReactiveOutput=true to validate both hangar floor
        // world-motion preservation and MotionPass -> TaaPass disocclusion rejection.
        if (!motionPass.render(depthSrv[curIdx], camNow, camPrev, kWidth, kHeight,
                               jit.pixelX, jit.pixelY,
                               isReset ? 0.0f : prevJ.pixelX,
                               isReset ? 0.0f : prevJ.pixelY,
                               isReset, prevDepthSrv,
                               /*preserveContextState=*/true,
                               /*zeroTranslation=*/false,
                               /*enableClassOutput=*/false,
                               /*weaponMotion=*/nullptr,
                               /*sceneStencil=*/nullptr,
                               /*celestialRecords=*/nullptr,
                               /*celestialCount=*/0u,
                               /*clampCockpit=*/true,
                               /*enableReactiveOutput=*/true,
                               /*inSrv=*/false)) {
            std::printf("  [FAIL] %s: MotionPass::render failed at frame %d\n", prof.name, t);
            return false;
        }

        if (!taaPass.render(ctx, colorSrv, motionPass.output(), depthSrv[curIdx],
                            outRtv, outTex, kWidth, kHeight,
                            jit.pixelX, jit.pixelY, isReset, kWidth, kHeight,
                            isReset ? 0.0f : prevJ.pixelX,
                            isReset ? 0.0f : prevJ.pixelY,
                            kDepthB, prevDepthSrv, DXGI_FORMAT_UNKNOWN,
                            motionPass.reactiveMask(), nullptr, nullptr, false,
                            &camNow, &camPrev)) {
            std::printf("  [FAIL] %s: TaaPass::render failed at frame %d\n", prof.name, t);
            return false;
        }

        if (t >= kEvalStart) {
            ctx->CopyResource(readbackStaging, outTex);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(ctx->Map(readbackStaging, 0, D3D11_MAP_READ, 0, &mapped))) return false;
            const auto* base = static_cast<const uint8_t*>(mapped.pData);

            for (uint32_t y = 4; y < kHeight - 4; ++y) {
                const auto* row = reinterpret_cast<const uint32_t*>(base + static_cast<size_t>(y) * mapped.RowPitch);
                for (uint32_t x = 4; x < kWidth - 4; ++x) {
                    const size_t idx = static_cast<size_t>(y) * kWidth + x;
                    const float outL = unpackLuma(row[x]);

                    // 1. Newly disoccluded dark back-wall pixels that were covered by the bright pillar last frame
                    if (refUnjittered.surface[idx] == SurfaceKind::BackWall &&
                        prevScene.surface[idx] == SurfaceKind::Pillar) {
                        // Exclude 1px anti-aliased boundary right next to the pillar or bright floor horizon
                        bool nearOtherSurface = false;
                        for (int oy = -1; oy <= 1 && !nearOtherSurface; ++oy) {
                            for (int ox = -1; ox <= 1; ++ox) {
                                if (refUnjittered.surface[static_cast<size_t>(y + oy) * kWidth + (x + ox)] != SurfaceKind::BackWall) {
                                    nearOtherSurface = true;
                                    break;
                                }
                            }
                        }
                        if (!nearOtherSurface) {
                            const float ghost = std::fabs(outL - refUnjittered.luma[idx]);
                            worstDisocclGhost = std::max(worstDisocclGhost, ghost);
                            ++disocclPixelCount;
                        }
                    }

                    // 2. Interior hangar floor pixels (exclude 1px grating transitions to measure temporal smearing)
                    if (refUnjittered.surface[idx] == SurfaceKind::Floor) {
                        const float refL = refUnjittered.luma[idx];
                        bool uniformPatch = true;
                        for (int oy = -1; oy <= 1 && uniformPatch; ++oy) {
                            for (int ox = -1; ox <= 1; ++ox) {
                                if (std::fabs(refUnjittered.luma[static_cast<size_t>(y + oy) * kWidth + (x + ox)] - refL) >= 0.02f) {
                                    uniformPatch = false;
                                    break;
                                }
                            }
                        }
                        if (uniformPatch) {
                            floorErrSum += std::fabs(outL - refL);
                            ++floorPixelCount;
                        }
                    }
                }
            }
            // 3. Distant 2-3px thin railing visibility & temporal flicker stability
            std::vector<float> curRowPeaks;
            curRowPeaks.reserve(kHeight / 2);
            for (uint32_t y = kHeight / 4; y < (kHeight * 5) / 8; y += 2) {
                const auto* row = reinterpret_cast<const uint32_t*>(base + static_cast<size_t>(y) * mapped.RowPitch);
                float rowMaxRail = 0.0f;
                bool hasRail = false;
                for (uint32_t x = 4; x < kWidth / 2; ++x) {
                    if (refUnjittered.surface[static_cast<size_t>(y) * kWidth + x] == SurfaceKind::DistantRail) {
                        hasRail = true;
                        for (int ox = -1; ox <= 1; ++ox) {
                            rowMaxRail = (std::max)(rowMaxRail, unpackLuma(row[x + ox]));
                        }
                    }
                }
                if (hasRail) {
                    railPeakSum += rowMaxRail;
                    ++railRowCount;
                    curRowPeaks.push_back(rowMaxRail);
                }
            }
            if (!railPrevRowPeaks.empty() && railPrevRowPeaks.size() == curRowPeaks.size()) {
                for (size_t ri = 0; ri < curRowPeaks.size(); ++ri) {
                    const double d = static_cast<double>(curRowPeaks[ri] - railPrevRowPeaks[ri]);
                    railDeltaSqSum += d * d;
                    ++railDeltaCount;
                }
            }
            railPrevRowPeaks = std::move(curRowPeaks);

            ctx->Unmap(readbackStaging, 0);
        }

        prevCam = camNow;
        prevJ = jit;
        if (t >= kEvalStart - 1) prevScene = std::move(refUnjittered);
        curIdx ^= 1;
    }

    const float meanFloorSmear = (floorPixelCount > 0)
        ? static_cast<float>(floorErrSum / static_cast<double>(floorPixelCount))
        : 0.0f;

    const float railMeanPeak = (railRowCount > 0)
        ? static_cast<float>(railPeakSum / static_cast<double>(railRowCount))
        : 0.0f;
    const float railFlickerRms = (railDeltaCount > 0)
        ? static_cast<float>(std::sqrt(railDeltaSqSum / static_cast<double>(railDeltaCount)))
        : 0.0f;

    std::printf("  [%-14s] disocclMaxGhost=%.4f (n=%llu) | floorMeanSmear=%.4f (n=%llu) | distRail2px peak=%.4f flickerRms=%.4f\n",
        prof.name, worstDisocclGhost, static_cast<unsigned long long>(disocclPixelCount),
        meanFloorSmear, static_cast<unsigned long long>(floorPixelCount),
        railMeanPeak, railFlickerRms);

    bool ok = true;
    if (worstDisocclGhost > 0.12f) {
        std::printf("    -> FAIL: disocclusion ghosting exceeded 0.12 (got %.4f)\n", worstDisocclGhost);
        ok = false;
    }
    if (meanFloorSmear > 0.06f) {
        std::printf("    -> FAIL: hangar floor smearing exceeded 0.06 (got %.4f)\n", meanFloorSmear);
        ok = false;
    }
    if (railRowCount > 0 && railMeanPeak < 0.45f) {
        std::printf("    -> FAIL: distant 2-3px rail visibility too low (peak=%.4f < 0.45)\n", railMeanPeak);
        ok = false;
    }
    if (railDeltaCount > 0 && railFlickerRms > 0.06f) {
        std::printf("    -> FAIL: distant 2-3px rail flickered/jittered (rms=%.4f > 0.06)\n", railFlickerRms);
        ok = false;
    }
    return ok;
}

} // namespace

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION, &device, nullptr, &ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION, &device, nullptr, &ctx);
    }
    if (FAILED(hr) || !device || !ctx) {
        std::printf("Failed to initialize D3D11 device\n");
        return 1;
    }

    edpe::MotionPass motionPass;
    edpe::TaaPass taaPass;
    if (!motionPass.initialize(device.Get(), ctx.Get()) ||
        !taaPass.initialize(device.Get(), ctx.Get())) {
        std::printf("Failed to initialize MotionPass or TaaPass\n");
        return 2;
    }

    auto makeTex = [&](DXGI_FORMAT fmt, UINT bind, D3D11_USAGE usage, UINT cpu, ComPtr<ID3D11Texture2D>& out) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kWidth;
        td.Height = kHeight;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = usage;
        td.BindFlags = bind;
        td.CPUAccessFlags = cpu;
        return SUCCEEDED(device->CreateTexture2D(&td, nullptr, &out));
    };

    ComPtr<ID3D11Texture2D> colorTex, depthTex[2], depthStaging, outTex, readbackStaging;
    ComPtr<ID3D11ShaderResourceView> colorSrv, depthSrv[2];
    ComPtr<ID3D11RenderTargetView> outRtv;

    if (!makeTex(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, colorTex) ||
        FAILED(device->CreateShaderResourceView(colorTex.Get(), nullptr, &colorSrv)) ||
        !makeTex(DXGI_FORMAT_R32G8X24_TYPELESS, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_WRITE, depthStaging) ||
        !makeTex(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, outTex) ||
        FAILED(device->CreateRenderTargetView(outTex.Get(), nullptr, &outRtv)) ||
        !makeTex(DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, readbackStaging)) {
        return 3;
    }
    for (int i = 0; i < 2; ++i) {
        if (!makeTex(DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, depthTex[i])) {
            return 4;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(depthTex[i].Get(), &sd, &depthSrv[i]))) {
            return 5;
        }
    }

    const MotionProfile profiles[] = {
        // Stationary distant thin-geometry AA stability baseline
        {"Stationary",    0.00f,  0.00f,  0.00f,  0.000f,  0.000f,  0.000f},
        // Individual translations
        {"TranslateX",    0.16f,  0.00f,  0.00f,  0.000f,  0.000f,  0.000f},
        {"TranslateY",    0.00f,  0.12f,  0.00f,  0.000f,  0.000f,  0.000f},
        {"TranslateZ",    0.00f,  0.00f,  0.22f,  0.000f,  0.000f,  0.000f},
        // Individual rotations
        {"RotateYaw",     0.00f,  0.00f,  0.00f,  0.032f,  0.000f,  0.000f},
        {"RotatePitch",   0.00f,  0.00f,  0.00f,  0.000f,  0.025f,  0.000f},
        {"RotateRoll",    0.00f,  0.00f,  0.00f,  0.000f,  0.000f,  0.035f},
        // Simultaneous 6-DOF translation + rotation
        {"Combined6DOF",  0.12f,  0.06f,  0.15f,  0.024f,  0.015f,  0.020f},
        // Fast forward Z flight (20% Z-change/frame at back wall, validates expectedPrevZ disocclusion)
        {"FastFlightZ",   0.00f,  0.00f,  2.00f,  0.000f,  0.000f,  0.000f},
    };

    auto runFarSilhouette150mTest = [&]() -> bool {
        constexpr uint32_t kFw = 1280;
        constexpr uint32_t kFh = 720;
        constexpr float kFarZ = 150.0f;
        constexpr float kSlabHalfW = 12.0f;

        ComPtr<ID3D11Texture2D> fColorTex, fDepthTex[2], fDepthStg, fOutTex, fReadback;
        ComPtr<ID3D11ShaderResourceView> fColorSrv, fDepthSrv[2];
        ComPtr<ID3D11RenderTargetView> fOutRtv;
        auto mk = [&](DXGI_FORMAT fmt, UINT bind, D3D11_USAGE usage, UINT cpu, ComPtr<ID3D11Texture2D>& out) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = kFw; td.Height = kFh; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = fmt; td.SampleDesc.Count = 1; td.Usage = usage;
            td.BindFlags = bind; td.CPUAccessFlags = cpu;
            return SUCCEEDED(device->CreateTexture2D(&td, nullptr, &out));
        };
        if (!mk(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, fColorTex) ||
            FAILED(device->CreateShaderResourceView(fColorTex.Get(), nullptr, &fColorSrv)) ||
            !mk(DXGI_FORMAT_R32G8X24_TYPELESS, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_WRITE, fDepthStg) ||
            !mk(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, fOutTex) ||
            FAILED(device->CreateRenderTargetView(fOutTex.Get(), nullptr, &fOutRtv)) ||
            !mk(DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, fReadback)) {
            return false;
        }
        for (int i = 0; i < 2; ++i) {
            if (!mk(DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, fDepthTex[i])) return false;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            if (FAILED(device->CreateShaderResourceView(fDepthTex[i].Get(), &sd, &fDepthSrv[i]))) return false;
        }

        edpe::TaaPass farTaa;
        if (!farTaa.initialize(device.Get(), ctx.Get())) return false;

        constexpr int kFarFrames = 8;
        edpe::CameraProjection prevCam{};
        edpe::ProjectionJitter prevJ{};
        std::vector<uint8_t> prevIsSky;
        float worstFarGhost = 0.0f;
        uint64_t farDisocclCount = 0;
        int cur = 0;

        for (int t = 0; t < kFarFrames; ++t) {
            const float camX = 1.5f * static_cast<float>(t); // ~5.76 px/frame at z = 150m
            const auto camNow = buildCamera(camX, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
            const auto jit = edpe::projectionJitter(static_cast<uint64_t>(t), kFw, kFh, 16u);

            std::vector<uint32_t> col(static_cast<size_t>(kFw) * kFh, packRgba(0.04f, 0.05f, 0.07f));
            std::vector<float> dep(static_cast<size_t>(kFw) * kFh, 0.0f);
            std::vector<uint8_t> isSky(static_cast<size_t>(kFw) * kFh, 1u);
            for (uint32_t y = kFh / 4; y < (kFh * 3) / 4; ++y) {
                for (uint32_t x = 0; x < kFw; ++x) {
                    const float ux = (static_cast<float>(x) + 0.5f) - jit.pixelX;
                    const float ndcX = 2.0f * ux / static_cast<float>(kFw) - 1.0f;
                    const float wx = camX + (ndcX / kScaleX) * kFarZ;
                    if (std::fabs(wx) <= kSlabHalfW) {
                        const size_t idx = static_cast<size_t>(y) * kFw + x;
                        col[idx] = packRgba(0.92f, 0.90f, 0.85f);
                        dep[idx] = kDepthB / kFarZ;
                        isSky[idx] = 0u;
                    }
                }
            }

            ctx->UpdateSubresource(fColorTex.Get(), 0, nullptr, col.data(), kFw * sizeof(uint32_t), 0);
            D3D11_MAPPED_SUBRESOURCE dm{};
            if (FAILED(ctx->Map(fDepthStg.Get(), 0, D3D11_MAP_WRITE, 0, &dm))) return false;
            struct D32S32 { float d; uint32_t s; };
            for (uint32_t y = 0; y < kFh; ++y) {
                auto* row = reinterpret_cast<D32S32*>(static_cast<uint8_t*>(dm.pData) + static_cast<size_t>(y) * dm.RowPitch);
                for (uint32_t x = 0; x < kFw; ++x) {
                    row[x].d = dep[static_cast<size_t>(y) * kFw + x];
                    row[x].s = 0u;
                }
            }
            ctx->Unmap(fDepthStg.Get(), 0);
            ctx->CopyResource(fDepthTex[cur].Get(), fDepthStg.Get());

            const bool isReset = (t == 0);
            const auto& camPrev = isReset ? camNow : prevCam;
            ID3D11ShaderResourceView* pDepth = isReset ? nullptr : fDepthSrv[cur ^ 1].Get();
            if (!motionPass.render(fDepthSrv[cur].Get(), camNow, camPrev, kFw, kFh,
                                   jit.pixelX, jit.pixelY,
                                   isReset ? 0.0f : prevJ.pixelX,
                                   isReset ? 0.0f : prevJ.pixelY,
                                   isReset, pDepth, true, false, false,
                                   nullptr, nullptr, nullptr, 0u, true, true, false)) {
                return false;
            }
            if (!farTaa.render(ctx.Get(), fColorSrv.Get(), motionPass.output(), fDepthSrv[cur].Get(),
                               fOutRtv.Get(), fOutTex.Get(), kFw, kFh,
                               jit.pixelX, jit.pixelY, isReset, kFw, kFh,
                               isReset ? 0.0f : prevJ.pixelX,
                               isReset ? 0.0f : prevJ.pixelY,
                               kDepthB, pDepth, DXGI_FORMAT_UNKNOWN,
                               motionPass.reactiveMask(), nullptr, nullptr, false,
                               &camNow, &camPrev)) {
                return false;
            }

            if (t == kFarFrames - 1) {
                ctx->CopyResource(fReadback.Get(), fOutTex.Get());
                D3D11_MAPPED_SUBRESOURCE rm{};
                if (FAILED(ctx->Map(fReadback.Get(), 0, D3D11_MAP_READ, 0, &rm))) return false;
                const float skyLuma = 0.2126f * 0.04f + 0.7152f * 0.05f + 0.0722f * 0.07f;
                for (uint32_t y = kFh / 4 + 4; y < (kFh * 3) / 4 - 4; ++y) {
                    const auto* row = reinterpret_cast<const uint32_t*>(
                        static_cast<const uint8_t*>(rm.pData) + static_cast<size_t>(y) * rm.RowPitch);
                    for (uint32_t x = 4; x < kFw - 4; ++x) {
                        const size_t idx = static_cast<size_t>(y) * kFw + x;
                        if (isSky[idx] && !prevIsSky[idx] &&
                            isSky[idx - 1] && isSky[idx + 1] && isSky[idx - 2] && isSky[idx + 2]) {
                            const float g = std::fabs(unpackLuma(row[x]) - skyLuma);
                            worstFarGhost = std::max(worstFarGhost, g);
                            ++farDisocclCount;
                        }
                    }
                }
                ctx->Unmap(fReadback.Get(), 0);
            }

            prevCam = camNow;
            prevJ = jit;
            prevIsSky = std::move(isSky);
            cur ^= 1;
        }
        farTaa.shutdown();

        std::printf("  [FarSilh150m  ] disocclMaxGhost=%.4f (n=%llu)\n",
            worstFarGhost, static_cast<unsigned long long>(farDisocclCount));
        if (farDisocclCount == 0 || worstFarGhost > 0.12f) {
            std::printf("    -> FAIL: 150m silhouette over sky ghosted (max=%.4f, n=%llu)\n",
                worstFarGhost, static_cast<unsigned long long>(farDisocclCount));
            return false;
        }
        return true;
    };

    std::printf("[TAA_Motion_Ghosting_And_Smearing_Suite 3840x2160]\n");
    ID3D11Texture2D* dTexRaw[2] = { depthTex[0].Get(), depthTex[1].Get() };
    ID3D11ShaderResourceView* dSrvRaw[2] = { depthSrv[0].Get(), depthSrv[1].Get() };

    bool allOk = true;
    for (const auto& prof : profiles) {
        allOk &= runMotionProfile(device.Get(), ctx.Get(), motionPass, taaPass,
            colorTex.Get(), colorSrv.Get(), dTexRaw, dSrvRaw, depthStaging.Get(),
            outTex.Get(), outRtv.Get(), readbackStaging.Get(), prof);
    }
    allOk &= runFarSilhouette150mTest();

    taaPass.shutdown();
    return allOk ? 0 : 10;
}
