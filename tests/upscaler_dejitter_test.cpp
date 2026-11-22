#include "temporal_inputs.h"
#include "temporal_math.h"
#include "upscaler_backend.h"

#include <DirectXPackedVector.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr float kDepthB = 0.025f;
constexpr float kBgColor = 0.05f;
constexpr float kBgDepth = kDepthB / 50.0f;
constexpr float kFgDepth = kDepthB / 10.0f;

uint8_t floatToUnorm8(float v) {
    const float clamped = std::clamp(v, 0.0f, 1.0f);
    return static_cast<uint8_t>(std::lround(clamped * 255.0f));
}

float unorm8ToFloat(uint8_t b) {
    return static_cast<float>(b) / 255.0f;
}

uint32_t packRgba8(float r, float g, float b, float a = 1.0f) {
    return static_cast<uint32_t>(floatToUnorm8(r)) |
           (static_cast<uint32_t>(floatToUnorm8(g)) << 8) |
           (static_cast<uint32_t>(floatToUnorm8(b)) << 16) |
           (static_cast<uint32_t>(floatToUnorm8(a)) << 24);
}

uint32_t packHalf2(float x, float y) {
    const uint16_t hx = DirectX::PackedVector::XMConvertFloatToHalf(x);
    const uint16_t hy = DirectX::PackedVector::XMConvertFloatToHalf(y);
    return static_cast<uint32_t>(hx) | (static_cast<uint32_t>(hy) << 16);
}

float pixelLuma(uint32_t rgba) {
    const float r = unorm8ToFloat(static_cast<uint8_t>(rgba & 0xFFu));
    const float g = unorm8ToFloat(static_cast<uint8_t>((rgba >> 8) & 0xFFu));
    const float b = unorm8ToFloat(static_cast<uint8_t>((rgba >> 16) & 0xFFu));
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

struct FrameBuffers {
    uint32_t renderW = 0;
    uint32_t renderH = 0;
    std::vector<uint32_t> color;
    std::vector<float> depth;
    std::vector<uint32_t> motion;

    void resize(uint32_t w, uint32_t h) {
        renderW = w;
        renderH = h;
        const size_t n = static_cast<size_t>(w) * h;
        color.assign(n, packRgba8(kBgColor, kBgColor, kBgColor));
        depth.assign(n, kBgDepth);
        motion.assign(n, packHalf2(0.0f, 0.0f));
    }
};

struct GpuHarness {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    uint32_t rw = 0;
    uint32_t rh = 0;
    uint32_t ow = 0;
    uint32_t oh = 0;

    ComPtr<ID3D11Texture2D> colorTex;
    ComPtr<ID3D11ShaderResourceView> colorSrv;
    ComPtr<ID3D11Texture2D> depthTex[2];
    ComPtr<ID3D11ShaderResourceView> depthSrv[2];
    ComPtr<ID3D11Texture2D> motionTex;
    ComPtr<ID3D11ShaderResourceView> motionSrv;
    ComPtr<ID3D11Texture2D> outputTex;
    ComPtr<ID3D11RenderTargetView> outputRtv;
    ComPtr<ID3D11Texture2D> stagingTex;

    int currDepthIdx = 0;
    bool hasPrevDepth = false;

    bool initDevice() {
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                levels, 2, D3D11_SDK_VERSION, &device, nullptr, &context);
        }
        return SUCCEEDED(hr) && device && context;
    }

    bool allocateSurfaces(uint32_t renderW, uint32_t renderH, uint32_t outputW, uint32_t outputH) {
        rw = renderW;
        rh = renderH;
        ow = outputW;
        oh = outputH;
        currDepthIdx = 0;
        hasPrevDepth = false;

        auto makeTex = [&](uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bindFlags,
                           D3D11_USAGE usage, UINT cpuAccess, ComPtr<ID3D11Texture2D>& outTex) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = fmt;
            td.SampleDesc.Count = 1;
            td.Usage = usage;
            td.BindFlags = bindFlags;
            td.CPUAccessFlags = cpuAccess;
            return SUCCEEDED(device->CreateTexture2D(&td, nullptr, &outTex));
        };

        colorTex.Reset(); colorSrv.Reset();
        depthTex[0].Reset(); depthSrv[0].Reset();
        depthTex[1].Reset(); depthSrv[1].Reset();
        motionTex.Reset(); motionSrv.Reset();
        outputTex.Reset(); outputRtv.Reset();
        stagingTex.Reset();

        if (!makeTex(rw, rh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                     D3D11_USAGE_DEFAULT, 0, colorTex) ||
            FAILED(device->CreateShaderResourceView(colorTex.Get(), nullptr, &colorSrv))) {
            return false;
        }
        for (int i = 0; i < 2; ++i) {
            if (!makeTex(rw, rh, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE,
                         D3D11_USAGE_DEFAULT, 0, depthTex[i]) ||
                FAILED(device->CreateShaderResourceView(depthTex[i].Get(), nullptr, &depthSrv[i]))) {
                return false;
            }
        }
        if (!makeTex(rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
                     D3D11_USAGE_DEFAULT, 0, motionTex) ||
            FAILED(device->CreateShaderResourceView(motionTex.Get(), nullptr, &motionSrv))) {
            return false;
        }
        if (!makeTex(ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM,
                     D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                     D3D11_USAGE_DEFAULT, 0, outputTex) ||
            FAILED(device->CreateRenderTargetView(outputTex.Get(), nullptr, &outputRtv))) {
            return false;
        }
        if (!makeTex(ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, 0,
                     D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, stagingTex)) {
            return false;
        }
        return true;
    }

    bool uploadAndEvaluate(edpe::IUpscalerBackend& backend, const FrameBuffers& fb,
                           float jitterX, float jitterY, float prevJitterX, float prevJitterY,
                           bool reset, std::vector<uint32_t>& outPixels) {
        if (reset) {
            backend.resetHistory();
            hasPrevDepth = false;
        } else {
            currDepthIdx ^= 1;
        }

        context->UpdateSubresource(colorTex.Get(), 0, nullptr, fb.color.data(), rw * sizeof(uint32_t), 0);
        context->UpdateSubresource(depthTex[currDepthIdx].Get(), 0, nullptr, fb.depth.data(), rw * sizeof(float), 0);
        context->UpdateSubresource(motionTex.Get(), 0, nullptr, fb.motion.data(), rw * sizeof(uint32_t), 0);

        edpe::TemporalFrameInputs inputs{};
        inputs.color = colorTex.Get();
        inputs.colorSrv = colorSrv.Get();
        inputs.depth = depthTex[currDepthIdx].Get();
        inputs.depthSrv = depthSrv[currDepthIdx].Get();
        inputs.prevDepthSrv = hasPrevDepth ? depthSrv[currDepthIdx ^ 1].Get() : nullptr;
        inputs.motionVectors = motionTex.Get();
        inputs.motionSrv = motionSrv.Get();
        inputs.renderWidth = rw;
        inputs.renderHeight = rh;
        inputs.outputWidth = ow;
        inputs.outputHeight = oh;
        inputs.jitterX = jitterX;
        inputs.jitterY = jitterY;
        inputs.prevJitterX = prevJitterX;
        inputs.prevJitterY = prevJitterY;
        inputs.motionScaleX = 1.0f;
        inputs.motionScaleY = 1.0f;
        inputs.frameTimeMs = 16.667f;
        inputs.sharpness = 0.0f;
        inputs.reset = reset;
        inputs.depthInverted = true;
        inputs.depthInfinite = true;
        inputs.motionJittered = false;
        inputs.motionIsLowRes = true;
        inputs.colorIsHdr = false;
        inputs.cameraNow.scaleX = 1.0f;
        inputs.cameraNow.scaleY = 1.0f;
        inputs.cameraNow.depthB = kDepthB;
        inputs.cameraPrevious = inputs.cameraNow;

        if (!backend.evaluate(context.Get(), inputs, outputRtv.Get(), outputTex.Get())) {
            return false;
        }

        hasPrevDepth = true;

        context->CopyResource(stagingTex.Get(), outputTex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return false;
        }
        outPixels.resize(static_cast<size_t>(ow) * oh);
        const auto* srcBytes = static_cast<const uint8_t*>(mapped.pData);
        for (uint32_t y = 0; y < oh; ++y) {
            std::memcpy(outPixels.data() + static_cast<size_t>(y) * ow,
                        srcBytes + static_cast<size_t>(y) * mapped.RowPitch,
                        static_cast<size_t>(ow) * sizeof(uint32_t));
        }
        context->Unmap(stagingTex.Get(), 0);
        return true;
    }
};

struct StepEdgeMetrics {
    float spanX = 0.0f;
    float spanY = 0.0f;
    float stdX = 0.0f;
    float stdY = 0.0f;
    float errX = 0.0f;
    float errY = 0.0f;
    float rmsX = 0.0f;
    float rmsY = 0.0f;
};

StepEdgeMetrics measureStepEdge(GpuHarness& gpu, edpe::IUpscalerBackend& backend,
                                uint32_t renderW, uint32_t renderH,
                                uint32_t outputW, uint32_t outputH,
                                float jitterSign) {
    constexpr int kFrames = 48;
    constexpr int kWarmup = 16;

    const float scaleX = static_cast<float>(outputW) / static_cast<float>(renderW);
    const float scaleY = static_cast<float>(outputH) / static_cast<float>(renderH);

    // Top half: vertical step edge at ux = 64.5. Bottom half: horizontal step edge at uy = 96.5.
    constexpr float kVertEdgeU = 64.5f;
    constexpr float kHorizEdgeU = 96.5f;
    constexpr float kBright = 0.90f;

    const float edgeOutX = kVertEdgeU * scaleX;
    const float edgeOutY = kHorizEdgeU * scaleY;
    const int bx0 = std::max(0, static_cast<int>(std::floor(edgeOutX)) - 2);
    const int bx1 = std::min(static_cast<int>(outputW) - 1, static_cast<int>(std::floor(edgeOutX)) + 2);
    const int by0 = std::max(0, static_cast<int>(std::floor(edgeOutY)) - 2);
    const int by1 = std::min(static_cast<int>(outputH) - 1, static_cast<int>(std::floor(edgeOutY)) + 2);

    auto stepGtLuma = [](int p, float edgeOut) {
        const float pf = static_cast<float>(p);
        float cov = 0.0f;
        if (pf + 1.0f <= edgeOut) {
            cov = 0.0f;
        } else if (pf >= edgeOut) {
            cov = 1.0f;
        } else {
            cov = (pf + 1.0f) - edgeOut;
        }
        return kBgColor + cov * (kBright - kBgColor);
    };

    std::vector<float> posXList;
    std::vector<float> posYList;
    std::vector<std::vector<float>> bandXFrames;
    std::vector<std::vector<float>> bandYFrames;
    double errXSum = 0.0, errYSum = 0.0;
    size_t errXCount = 0, errYCount = 0;

    FrameBuffers fb;
    std::vector<uint32_t> outPixels;
    edpe::ProjectionJitter prevJ{};
    const unsigned phaseCount = backend.recommendedJitterPhaseCount(32u, renderW, renderH, outputW, outputH);

    for (int t = 0; t < kFrames; ++t) {
        const auto j = edpe::projectionJitter(static_cast<uint64_t>(t), renderW, renderH, phaseCount);
        fb.resize(renderW, renderH);

        for (uint32_t iy = 0; iy < renderH; ++iy) {
            const float sy = static_cast<float>(iy) + 0.5f;
            const float uy = sy - j.pixelY;
            for (uint32_t ix = 0; ix < renderW; ++ix) {
                const float sx = static_cast<float>(ix) + 0.5f;
                const float ux = sx - j.pixelX;
                const size_t idx = static_cast<size_t>(iy) * renderW + ix;

                bool fg = false;
                if (iy < renderH / 2) {
                    fg = (ux >= kVertEdgeU);
                } else {
                    fg = (uy >= kHorizEdgeU);
                }
                if (fg) {
                    fb.color[idx] = packRgba8(kBright, kBright, kBright);
                    fb.depth[idx] = kFgDepth;
                }
            }
        }

        const float passJx = j.pixelX * jitterSign;
        const float passJy = j.pixelY * jitterSign;
        const float passPrevJx = (t > 0) ? (prevJ.pixelX * jitterSign) : 0.0f;
        const float passPrevJy = (t > 0) ? (prevJ.pixelY * jitterSign) : 0.0f;

        if (!gpu.uploadAndEvaluate(backend, fb, passJx, passJy, passPrevJx, passPrevJy, t == 0, outPixels)) {
            return {999.0f, 999.0f, 999.0f, 999.0f, 999.0f, 999.0f, 999.0f, 999.0f};
        }
        prevJ = j;

        if (t >= kWarmup) {
            const int wx0 = static_cast<int>(std::round((kVertEdgeU - 8.0f) * scaleX));
            const int wx1 = static_cast<int>(std::round((kVertEdgeU + 8.0f) * scaleX));
            const uint32_t ry0 = static_cast<uint32_t>(std::round(16 * scaleY));
            const uint32_t ry1 = static_cast<uint32_t>(std::round(48 * scaleY));
            double sumEdgeX = 0.0;
            int rowCount = 0;
            std::vector<float> bandX;
            bandX.reserve(static_cast<size_t>(ry1 - ry0) * (bx1 - bx0 + 1));
            for (uint32_t y = ry0; y < ry1; ++y) {
                double normSum = 0.0;
                for (int x = wx0; x <= wx1; ++x) {
                    const float l = pixelLuma(outPixels[static_cast<size_t>(y) * outputW + x]);
                    normSum += std::clamp((l - kBgColor) / (kBright - kBgColor), 0.0f, 1.0f);
                    if (x >= bx0 && x <= bx1) {
                        errXSum += std::fabs(l - stepGtLuma(x, edgeOutX));
                        ++errXCount;
                        bandX.push_back(l);
                    }
                }
                sumEdgeX += static_cast<double>(wx1 + 1) - normSum;
                ++rowCount;
            }
            posXList.push_back(static_cast<float>(sumEdgeX / rowCount));
            bandXFrames.push_back(std::move(bandX));

            const int wy0 = static_cast<int>(std::round((kHorizEdgeU - 8.0f) * scaleY));
            const int wy1 = static_cast<int>(std::round((kHorizEdgeU + 8.0f) * scaleY));
            const uint32_t cx0 = static_cast<uint32_t>(std::round(24 * scaleX));
            const uint32_t cx1 = static_cast<uint32_t>(std::round(104 * scaleX));
            double sumEdgeY = 0.0;
            int colCount = 0;
            std::vector<float> bandY;
            bandY.reserve(static_cast<size_t>(cx1 - cx0) * (by1 - by0 + 1));
            for (uint32_t x = cx0; x < cx1; ++x) {
                double normSum = 0.0;
                for (int y = wy0; y <= wy1; ++y) {
                    const float l = pixelLuma(outPixels[static_cast<size_t>(y) * outputW + x]);
                    normSum += std::clamp((l - kBgColor) / (kBright - kBgColor), 0.0f, 1.0f);
                    if (y >= by0 && y <= by1) {
                        errYSum += std::fabs(l - stepGtLuma(y, edgeOutY));
                        ++errYCount;
                        bandY.push_back(l);
                    }
                }
                sumEdgeY += static_cast<double>(wy1 + 1) - normSum;
                ++colCount;
            }
            posYList.push_back(static_cast<float>(sumEdgeY / colCount));
            bandYFrames.push_back(std::move(bandY));
        }
    }

    auto calcStats = [](const std::vector<float>& vals, float& outSpan, float& outStd) {
        auto [mn, mx] = std::minmax_element(vals.begin(), vals.end());
        outSpan = *mx - *mn;
        double mean = 0.0;
        for (float v : vals) mean += v;
        mean /= vals.size();
        double var = 0.0;
        for (float v : vals) {
            const double d = v - mean;
            var += d * d;
        }
        outStd = static_cast<float>(std::sqrt(var / vals.size()));
    };
    auto calcRmsDelta = [](const std::vector<std::vector<float>>& frames) {
        double sq = 0.0;
        size_t n = 0;
        for (size_t i = 1; i < frames.size(); ++i) {
            for (size_t k = 0; k < frames[i].size(); ++k) {
                const double d = frames[i][k] - frames[i - 1][k];
                sq += d * d;
                ++n;
            }
        }
        return n > 0 ? static_cast<float>(std::sqrt(sq / n)) : 0.0f;
    };

    StepEdgeMetrics m{};
    calcStats(posXList, m.spanX, m.stdX);
    calcStats(posYList, m.spanY, m.stdY);
    m.errX = errXCount > 0 ? static_cast<float>(errXSum / errXCount) : 999.0f;
    m.errY = errYCount > 0 ? static_cast<float>(errYSum / errYCount) : 999.0f;
    m.rmsX = calcRmsDelta(bandXFrames);
    m.rmsY = calcRmsDelta(bandYFrames);
    return m;
}

struct ThinLineMetrics {
    float subpixelPeakSpan = 0.0f;
    float subpixelRmsDelta = 0.0f;
    float subpixelMeanPeak = 0.0f;
    float diagPeakSpan = 0.0f;
    float diagRmsDelta = 0.0f;
};

ThinLineMetrics measureStaticThinLines(GpuHarness& gpu, edpe::IUpscalerBackend& backend,
                                       uint32_t renderW, uint32_t renderH,
                                       uint32_t outputW, uint32_t outputH) {
    constexpr int kFrames = 48;
    constexpr int kWarmup = 16;

    const float scaleX = static_cast<float>(outputW) / static_cast<float>(renderW);
    const float scaleY = static_cast<float>(outputH) / static_cast<float>(renderH);

    // Top half: 0.45 px wide vertical thin line at ux = 64.25 (point-sampled hits ~45% of Halton phases).
    // Bottom half: 1.0 px wide diagonal thin line (0.6*(ux-64) - 0.8*(uy-96) = 0).
    constexpr float kSubpixelCenterX = 64.25f;
    constexpr float kSubpixelHalfW = 0.225f;
    constexpr float kDiagHalfW = 0.50f;

    std::vector<float> subPeakSeries;
    std::vector<float> diagPeakSeries;
    std::vector<std::vector<float>> subWindowFrames;
    std::vector<std::vector<float>> diagWindowFrames;

    FrameBuffers fb;
    std::vector<uint32_t> outPixels;
    edpe::ProjectionJitter prevJ{};
    const unsigned phaseCount = backend.recommendedJitterPhaseCount(32u, renderW, renderH, outputW, outputH);

    for (int t = 0; t < kFrames; ++t) {
        const auto j = edpe::projectionJitter(static_cast<uint64_t>(t), renderW, renderH, phaseCount);
        fb.resize(renderW, renderH);

        for (uint32_t iy = 0; iy < renderH; ++iy) {
            const float sy = static_cast<float>(iy) + 0.5f;
            const float uy = sy - j.pixelY;
            for (uint32_t ix = 0; ix < renderW; ++ix) {
                const float sx = static_cast<float>(ix) + 0.5f;
                const float ux = sx - j.pixelX;
                const size_t idx = static_cast<size_t>(iy) * renderW + ix;

                bool hit = false;
                if (iy >= 8 && iy < 56) {
                    hit = std::fabs(ux - kSubpixelCenterX) <= kSubpixelHalfW;
                } else if (iy >= 72 && iy < 120) {
                    const float dist = std::fabs(0.6f * (ux - 64.0f) - 0.8f * (uy - 96.0f));
                    hit = (dist <= kDiagHalfW);
                }
                if (hit) {
                    fb.color[idx] = packRgba8(1.0f, 1.0f, 1.0f);
                    fb.depth[idx] = kFgDepth;
                }
            }
        }

        if (!gpu.uploadAndEvaluate(backend, fb, j.pixelX, j.pixelY,
                                   t > 0 ? prevJ.pixelX : 0.0f,
                                   t > 0 ? prevJ.pixelY : 0.0f,
                                   t == 0, outPixels)) {
            return {999.0f, 999.0f, 0.0f, 999.0f, 999.0f};
        }
        prevJ = j;

        if (t >= kWarmup) {
            const uint32_t subY0 = static_cast<uint32_t>(std::round(16 * scaleY));
            const uint32_t subY1 = static_cast<uint32_t>(std::round(48 * scaleY));
            const uint32_t subX0 = static_cast<uint32_t>(std::round((kSubpixelCenterX - 2.0f) * scaleX));
            const uint32_t subX1 = static_cast<uint32_t>(std::round((kSubpixelCenterX + 2.0f) * scaleX));

            float subPeak = 0.0f;
            std::vector<float> subWin;
            for (uint32_t y = subY0; y < subY1; ++y) {
                float rowMax = 0.0f;
                for (uint32_t x = subX0; x <= subX1; ++x) {
                    const float l = pixelLuma(outPixels[static_cast<size_t>(y) * outputW + x]);
                    rowMax = std::max(rowMax, l);
                    subWin.push_back(l);
                }
                subPeak += rowMax;
            }
            subPeak /= static_cast<float>(subY1 - subY0);
            subPeakSeries.push_back(subPeak);
            subWindowFrames.push_back(std::move(subWin));

            const uint32_t diagY0 = static_cast<uint32_t>(std::round(80 * scaleY));
            const uint32_t diagY1 = static_cast<uint32_t>(std::round(112 * scaleY));
            const uint32_t diagX0 = static_cast<uint32_t>(std::round(32 * scaleX));
            const uint32_t diagX1 = static_cast<uint32_t>(std::round(96 * scaleX));

            float diagRowPeakSum = 0.0f;
            int diagRowCount = 0;
            std::vector<float> diagWin;
            for (uint32_t y = diagY0; y < diagY1; ++y) {
                float rowMax = 0.0f;
                for (uint32_t x = diagX0; x < diagX1; ++x) {
                    const float l = pixelLuma(outPixels[static_cast<size_t>(y) * outputW + x]);
                    rowMax = std::max(rowMax, l);
                    diagWin.push_back(l);
                }
                diagRowPeakSum += rowMax;
                ++diagRowCount;
            }
            diagPeakSeries.push_back(diagRowPeakSum / static_cast<float>(diagRowCount));
            diagWindowFrames.push_back(std::move(diagWin));
        }
    }

    auto computeRmsFrameDelta = [](const std::vector<std::vector<float>>& frames) {
        double sqSum = 0.0;
        size_t count = 0;
        for (size_t i = 1; i < frames.size(); ++i) {
            for (size_t k = 0; k < frames[i].size(); ++k) {
                const double d = frames[i][k] - frames[i - 1][k];
                sqSum += d * d;
                ++count;
            }
        }
        return count > 0 ? static_cast<float>(std::sqrt(sqSum / count)) : 0.0f;
    };

    ThinLineMetrics m{};
    auto [subMn, subMx] = std::minmax_element(subPeakSeries.begin(), subPeakSeries.end());
    m.subpixelPeakSpan = *subMx - *subMn;
    double subMean = 0.0;
    for (float v : subPeakSeries) subMean += v;
    m.subpixelMeanPeak = static_cast<float>(subMean / subPeakSeries.size());
    m.subpixelRmsDelta = computeRmsFrameDelta(subWindowFrames);

    auto [diagMn, diagMx] = std::minmax_element(diagPeakSeries.begin(), diagPeakSeries.end());
    m.diagPeakSpan = *diagMx - *diagMn;
    m.diagRmsDelta = computeRmsFrameDelta(diagWindowFrames);
    return m;
}

struct PanningThinLineMetrics {
    float peakSpan = 0.0f;
    float rmsStepDelta = 0.0f;
    float meanPeak = 0.0f;
};

PanningThinLineMetrics measureSlowPanningThinLine(GpuHarness& gpu, edpe::IUpscalerBackend& backend,
                                                  uint32_t renderW, uint32_t renderH,
                                                  uint32_t outputW, uint32_t outputH) {
    constexpr int kFrames = 64;
    constexpr int kWarmup = 32;

    const float scaleX = static_cast<float>(outputW) / static_cast<float>(renderW);
    const float scaleY = static_cast<float>(outputH) / static_cast<float>(renderH);

    // 1.0 render px vertical bright line moving +0.5 render px/frame (alternates integer / half-integer).
    // Entire scene shares uniform camera-pan motion mv = (-0.5, 0.0) so prevDepthMatches agrees everywhere.
    constexpr float kPanSpeedX = 0.5f;
    constexpr float kLineStartX = 32.0f;

    std::vector<float> peakSeries;
    FrameBuffers fb;
    std::vector<uint32_t> outPixels;
    edpe::ProjectionJitter prevJ{};
    const unsigned phaseCount = backend.recommendedJitterPhaseCount(32u, renderW, renderH, outputW, outputH);

    for (int t = 0; t < kFrames; ++t) {
        const auto j = edpe::projectionJitter(static_cast<uint64_t>(t), renderW, renderH, phaseCount);
        const float lineX = kLineStartX + kPanSpeedX * static_cast<float>(t);
        fb.resize(renderW, renderH);

        const uint32_t mvPacked = packHalf2(-kPanSpeedX, 0.0f);
        std::fill(fb.motion.begin(), fb.motion.end(), mvPacked);

        for (uint32_t iy = 0; iy < renderH; ++iy) {
            for (uint32_t ix = 0; ix < renderW; ++ix) {
                const float sx = static_cast<float>(ix) + 0.5f;
                const float ux = sx - j.pixelX;
                if (std::fabs(ux - lineX) <= 0.5f) {
                    const size_t idx = static_cast<size_t>(iy) * renderW + ix;
                    fb.color[idx] = packRgba8(1.0f, 1.0f, 1.0f);
                    fb.depth[idx] = kFgDepth;
                }
            }
        }

        if (!gpu.uploadAndEvaluate(backend, fb, j.pixelX, j.pixelY,
                                   t > 0 ? prevJ.pixelX : 0.0f,
                                   t > 0 ? prevJ.pixelY : 0.0f,
                                   t == 0, outPixels)) {
            return {999.0f, 999.0f, 0.0f};
        }
        prevJ = j;

        if (t >= kWarmup) {
            const float outLineX = lineX * scaleX;
            const int x0 = std::max(0, static_cast<int>(std::floor(outLineX - 2.0f * scaleX)));
            const int x1 = std::min(static_cast<int>(outputW) - 1, static_cast<int>(std::ceil(outLineX + 2.0f * scaleX)));
            const uint32_t y0 = static_cast<uint32_t>(std::round(24 * scaleY));
            const uint32_t y1 = static_cast<uint32_t>(std::round(104 * scaleY));

            float rowPeakSum = 0.0f;
            for (uint32_t y = y0; y < y1; ++y) {
                float rowMax = 0.0f;
                for (int x = x0; x <= x1; ++x) {
                    rowMax = std::max(rowMax, pixelLuma(outPixels[static_cast<size_t>(y) * outputW + x]));
                }
                rowPeakSum += rowMax;
            }
            peakSeries.push_back(rowPeakSum / static_cast<float>(y1 - y0));
        }
    }

    auto [mn, mx] = std::minmax_element(peakSeries.begin(), peakSeries.end());
    double mean = 0.0;
    for (float v : peakSeries) mean += v;
    mean /= peakSeries.size();

    double sqDelta = 0.0;
    for (size_t i = 1; i < peakSeries.size(); ++i) {
        const double d = peakSeries[i] - peakSeries[i - 1];
        sqDelta += d * d;
    }

    PanningThinLineMetrics m{};
    m.peakSpan = *mx - *mn;
    m.rmsStepDelta = static_cast<float>(std::sqrt(sqDelta / (peakSeries.size() - 1)));
    m.meanPeak = static_cast<float>(mean);
    return m;
}

float measureUnvectoredBlobGhostResidual(GpuHarness& gpu, edpe::IUpscalerBackend& backend,
                                         uint32_t renderW, uint32_t renderH,
                                         uint32_t outputW, uint32_t outputH) {
    constexpr int kTotalFrames = 36;
    constexpr int kDisappearFrame = 20;
    constexpr int kMeasureFrame = 30;

    const float scaleX = static_cast<float>(outputW) / static_cast<float>(renderW);
    const float scaleY = static_cast<float>(outputH) / static_cast<float>(renderH);

    FrameBuffers fb;
    std::vector<uint32_t> outPixels;
    edpe::ProjectionJitter prevJ{};
    float residualAtFrame30 = 999.0f;
    const unsigned phaseCount = backend.recommendedJitterPhaseCount(32u, renderW, renderH, outputW, outputH);

    for (int t = 0; t < kTotalFrames; ++t) {
        const auto j = edpe::projectionJitter(static_cast<uint64_t>(t), renderW, renderH, phaseCount);
        fb.resize(renderW, renderH);

        if (t < kDisappearFrame) {
            for (uint32_t iy = 0; iy < renderH; ++iy) {
                const float uy = static_cast<float>(iy) + 0.5f - j.pixelY;
                for (uint32_t ix = 0; ix < renderW; ++ix) {
                    const float ux = static_cast<float>(ix) + 0.5f - j.pixelX;
                    if (ux >= 58.0f && ux <= 70.0f && uy >= 58.0f && uy <= 70.0f) {
                        const size_t idx = static_cast<size_t>(iy) * renderW + ix;
                        fb.color[idx] = packRgba8(0.90f, 0.90f, 0.90f);
                    }
                }
            }
        }

        if (!gpu.uploadAndEvaluate(backend, fb, j.pixelX, j.pixelY,
                                   t > 0 ? prevJ.pixelX : 0.0f,
                                   t > 0 ? prevJ.pixelY : 0.0f,
                                   t == 0, outPixels)) {
            return 999.0f;
        }
        prevJ = j;

        if (t == kMeasureFrame) {
            const uint32_t x0 = static_cast<uint32_t>(std::round(60 * scaleX));
            const uint32_t x1 = static_cast<uint32_t>(std::round(68 * scaleX));
            const uint32_t y0 = static_cast<uint32_t>(std::round(60 * scaleY));
            const uint32_t y1 = static_cast<uint32_t>(std::round(68 * scaleY));
            double sum = 0.0;
            int cnt = 0;
            for (uint32_t y = y0; y < y1; ++y) {
                for (uint32_t x = x0; x < x1; ++x) {
                    sum += pixelLuma(outPixels[static_cast<size_t>(y) * outputW + x]);
                    ++cnt;
                }
            }
            residualAtFrame30 = static_cast<float>(sum / cnt) - kBgColor;
        }
    }

    return residualAtFrame30;
}

#if defined(EDPE_ENABLE_FSR)
PanningThinLineMetrics measureRealisticSlowPan(GpuHarness& gpu, edpe::AmdUpscalerBackend& backend, float velocityFactor) {
    constexpr uint32_t kWidth = 1920;
    constexpr uint32_t kHeight = 64;
    constexpr int kFrames = 48;
    constexpr int kWarmup = 16;
    constexpr float kPanSpeedX = 0.35f;
    constexpr float kLineStartX = 640.0f;
    constexpr float kLineHalfW = 0.275f; // 0.55 px wide line

    if (!gpu.allocateSurfaces(kWidth, kHeight, kWidth, kHeight)) {
        return {999.0f, 999.0f, 0.0f};
    }

    backend.setVelocityFactor(velocityFactor);
    const unsigned phaseCount = backend.recommendedJitterPhaseCount(32u, kWidth, kHeight, kWidth, kHeight);
    const uint32_t mvPacked = packHalf2(-kPanSpeedX, 0.0f);

    std::vector<float> peakSeries;
    FrameBuffers fb;
    std::vector<uint32_t> outPixels;
    edpe::ProjectionJitter prevJ{};

    for (int t = 0; t < kFrames; ++t) {
        const auto j = edpe::projectionJitter(static_cast<uint64_t>(t), kWidth, kHeight, phaseCount);
        const float lineX = kLineStartX + kPanSpeedX * static_cast<float>(t);
        fb.resize(kWidth, kHeight);
        std::fill(fb.motion.begin(), fb.motion.end(), mvPacked);

        const int xScan0 = std::max(0, static_cast<int>(std::floor(lineX - 3.0f)));
        const int xScan1 = std::min(static_cast<int>(kWidth) - 1, static_cast<int>(std::ceil(lineX + 3.0f)));
        for (uint32_t iy = 0; iy < kHeight; ++iy) {
            for (int ix = xScan0; ix <= xScan1; ++ix) {
                const float sx = static_cast<float>(ix) + 0.5f;
                const float ux = sx - j.pixelX;
                if (std::fabs(ux - lineX) <= kLineHalfW) {
                    const size_t idx = static_cast<size_t>(iy) * kWidth + ix;
                    fb.color[idx] = packRgba8(1.0f, 1.0f, 1.0f);
                    fb.depth[idx] = kFgDepth;
                }
            }
        }

        if (!gpu.uploadAndEvaluate(backend, fb, j.pixelX, j.pixelY,
                                   t > 0 ? prevJ.pixelX : 0.0f,
                                   t > 0 ? prevJ.pixelY : 0.0f,
                                   t == 0, outPixels)) {
            return {999.0f, 999.0f, 0.0f};
        }
        prevJ = j;

        if (t >= kWarmup) {
            const int x0 = std::max(0, static_cast<int>(std::floor(lineX)) - 2);
            const int x1 = std::min(static_cast<int>(kWidth) - 1, static_cast<int>(std::ceil(lineX)) + 2);
            float rowPeakSum = 0.0f;
            for (uint32_t y = 16; y < 48; ++y) {
                float rowMax = 0.0f;
                for (int x = x0; x <= x1; ++x) {
                    rowMax = std::max(rowMax, pixelLuma(outPixels[static_cast<size_t>(y) * kWidth + x]));
                }
                rowPeakSum += rowMax;
            }
            peakSeries.push_back(rowPeakSum / 32.0f);
        }
    }

    auto [mn, mx] = std::minmax_element(peakSeries.begin(), peakSeries.end());
    double mean = 0.0;
    for (float v : peakSeries) mean += v;
    mean /= peakSeries.size();

    double sqDelta = 0.0;
    for (size_t i = 1; i < peakSeries.size(); ++i) {
        const double d = peakSeries[i] - peakSeries[i - 1];
        sqDelta += d * d;
    }

    PanningThinLineMetrics m{};
    m.peakSpan = *mx - *mn;
    m.rmsStepDelta = static_cast<float>(std::sqrt(sqDelta / (peakSeries.size() - 1)));
    m.meanPeak = static_cast<float>(mean);
    return m;
}
#endif

bool runSuiteForBackend(const char* label, GpuHarness& gpu, edpe::IUpscalerBackend& backend,
                        uint32_t renderW, uint32_t renderH, uint32_t outputW, uint32_t outputH) {
    if (!gpu.allocateSurfaces(renderW, renderH, outputW, outputH)) {
        std::printf("[FAIL] %s: surface allocation failed\n", label);
        return false;
    }

    const unsigned phaseCount = backend.recommendedJitterPhaseCount(32u, renderW, renderH, outputW, outputH);
    const StepEdgeMetrics pos = measureStepEdge(gpu, backend, renderW, renderH, outputW, outputH, +1.0f);
    const StepEdgeMetrics neg = measureStepEdge(gpu, backend, renderW, renderH, outputW, outputH, -1.0f);
    const ThinLineMetrics thin = measureStaticThinLines(gpu, backend, renderW, renderH, outputW, outputH);
    const PanningThinLineMetrics pan = measureSlowPanningThinLine(gpu, backend, renderW, renderH, outputW, outputH);
    const float ghost = measureUnvectoredBlobGhostResidual(gpu, backend, renderW, renderH, outputW, outputH);

    std::printf(
        "[%s %ux%u->%ux%u phases=%u]\n"
        "  StepEdge(+jit): span=(%.4f, %.4f) std=(%.4f, %.4f) err=(%.4f, %.4f) rms=(%.4f, %.4f)\n"
        "  StepEdge(-jit): span=(%.4f, %.4f) err=(%.4f, %.4f) rms=(%.4f, %.4f)\n"
        "  StaticThinLine: sub0.45px span=%.4f rms=%.4f meanPeak=%.4f | diag1px span=%.4f rms=%.4f\n"
        "  SlowPan1px:     peakSpan=%.4f rmsDelta=%.4f meanPeak=%.4f\n"
        "  BlobGhost@+10f: residual=%.4f\n",
        label, renderW, renderH, outputW, outputH, phaseCount,
        pos.spanX, pos.spanY, pos.stdX, pos.stdY, pos.errX, pos.errY, pos.rmsX, pos.rmsY,
        neg.spanX, neg.spanY, neg.errX, neg.errY, neg.rmsX, neg.rmsY,
        thin.subpixelPeakSpan, thin.subpixelRmsDelta, thin.subpixelMeanPeak,
        thin.diagPeakSpan, thin.diagRmsDelta,
        pan.peakSpan, pan.rmsStepDelta, pan.meanPeak,
        ghost);

    const bool isSr = (outputW > renderW);
    const float maxEdgeSpan = isSr ? 0.25f : 0.15f;
    bool ok = true;

    if (!(pos.spanX < maxEdgeSpan && pos.spanY < maxEdgeSpan)) {
        std::printf("  -> FAIL: StepEdge(+jit) span exceeded %.2f\n", maxEdgeSpan);
        ok = false;
    }
    if (!(pos.rmsX < neg.rmsX && pos.rmsY < neg.rmsY)) {
        std::printf("  -> FAIL: StepEdge(+jit) rms did not strictly beat (-jit) (sign mismatch!)\n");
        ok = false;
    }
    if (!(thin.subpixelMeanPeak >= 0.16f)) {
        std::printf("  -> FAIL: Static 0.45px thin line visibility too low (meanPeak=%.4f < 0.16)\n",
                    thin.subpixelMeanPeak);
        ok = false;
    }
    if (!(thin.subpixelPeakSpan < 0.17f && thin.subpixelRmsDelta < 0.025f)) {
        std::printf("  -> FAIL: Static 0.45px thin line flickered (span=%.4f, rms=%.4f)\n",
                    thin.subpixelPeakSpan, thin.subpixelRmsDelta);
        ok = false;
    }
    if (!(thin.diagPeakSpan < 0.08f && thin.diagRmsDelta < 0.015f)) {
        std::printf("  -> FAIL: Static 1.0px diagonal line flickered (span=%.4f, rms=%.4f)\n",
                    thin.diagPeakSpan, thin.diagRmsDelta);
        ok = false;
    }
    if (!(pan.peakSpan < 0.20f && pan.rmsStepDelta < 0.055f)) {
        std::printf("  -> FAIL: Slow-panning 1.0px thin line flickered (span=%.4f, rms=%.4f)\n",
                    pan.peakSpan, pan.rmsStepDelta);
        ok = false;
    }
    if (!(ghost < 0.05f)) {
        std::printf("  -> FAIL: Un-vectored blob ghost residual %.4f >= 0.05\n", ghost);
        ok = false;
    }
    return ok;
}

#if defined(EDPE_ENABLE_FSR)
struct WalkShimmerMetrics {
    float rmsDelta = 0.0f;
    float p99Jump = 0.0f;
};

WalkShimmerMetrics measurePerfSrWalkingShimmer(GpuHarness& gpu, edpe::AmdUpscalerBackend& backend, float velocityFactor) {
    constexpr uint32_t kRw = 1280;
    constexpr uint32_t kRh = 64;
    constexpr uint32_t kOw = 2560;
    constexpr uint32_t kOh = 128;
    constexpr int kFrames = 48;
    constexpr int kWarmup = 16;
    constexpr float kWalkSpeed = 1.50f; // render px/f = 3.0 display px/f -> f4KVelocity = 4.5

    if (!gpu.allocateSurfaces(kRw, kRh, kOw, kOh)) {
        return {999.0f, 999.0f};
    }

    backend.setVelocityFactor(velocityFactor);
    const unsigned phaseCount = backend.recommendedJitterPhaseCount(32u, kRw, kRh, kOw, kOh);
    const uint32_t mvPacked = packHalf2(-kWalkSpeed, 0.0f);
    const uint32_t bgPacked = packRgba8(0.04f, 0.04f, 0.04f);

    FrameBuffers fb;
    std::vector<uint32_t> outPixels;
    std::vector<uint32_t> prevPixels;
    std::vector<float> deltas;
    double sqSum = 0.0;
    edpe::ProjectionJitter prevJ{};

    for (int t = 0; t < kFrames; ++t) {
        const auto j = edpe::projectionJitter(static_cast<uint64_t>(t), kRw, kRh, phaseCount);
        fb.renderW = kRw;
        fb.renderH = kRh;
        const size_t n = static_cast<size_t>(kRw) * kRh;
        fb.color.assign(n, bgPacked);
        fb.depth.assign(n, kFgDepth);
        fb.motion.assign(n, mvPacked);

        const float lobeX = static_cast<float>(kRw) * 0.45f + 0.90f * static_cast<float>(t);
        const int ix0 = std::max(0, static_cast<int>(std::floor(static_cast<float>(kRw) * 0.30f)));
        const int ix1 = std::min(static_cast<int>(kRw) - 1, static_cast<int>(std::ceil(static_cast<float>(kRw) * 0.70f)));
        for (uint32_t iy = 0; iy < kRh; ++iy) {
            const float uy = static_cast<float>(iy) + 0.5f - j.pixelY;
            for (int ix = ix0; ix <= ix1; ++ix) {
                const float ux = static_cast<float>(ix) + 0.5f - j.pixelX;
                const float wx = ux - kWalkSpeed * static_cast<float>(t);

                const float ribCoord = (wx + 0.15f * uy) / 3.5f;
                const float ribFrac = ribCoord - std::floor(ribCoord);
                const float ribDist = std::min(ribFrac, 1.0f - ribFrac) * 3.5f;
                const float ribVal = (ribDist <= 0.325f) ? 0.85f : 0.04f;

                const float micro = 0.5f + 0.5f * std::sin(wx * 2.7f + 0.4f * std::sin(uy * 3.1f)) *
                                                  std::cos(uy * 2.3f - 0.5f * std::cos(wx * 1.9f));
                const float env = std::exp(-((ux - lobeX) * (ux - lobeX)) / (2.0f * 40.0f * 40.0f));
                const float glint = std::pow(std::clamp((micro - 0.55f) / 0.45f, 0.0f, 1.0f), 2.5f);
                const float v = std::max(ribVal, 0.04f + 0.91f * env * glint);

                fb.color[static_cast<size_t>(iy) * kRw + ix] = packRgba8(v, v, v);
            }
        }

        if (!gpu.uploadAndEvaluate(backend, fb, j.pixelX, j.pixelY,
                                   t > 0 ? prevJ.pixelX : 0.0f,
                                   t > 0 ? prevJ.pixelY : 0.0f,
                                   t == 0, outPixels)) {
            return {999.0f, 999.0f};
        }
        prevJ = j;

        if (t >= kWarmup + 1) {
            constexpr uint32_t x0 = 960;
            constexpr uint32_t x1 = 1600;
            constexpr uint32_t y0 = 32;
            constexpr uint32_t y1 = 96;
            for (uint32_t y = y0; y < y1; ++y) {
                for (uint32_t x = x0; x < x1; ++x) {
                    const float cur = pixelLuma(outPixels[static_cast<size_t>(y) * kOw + x]);
                    const float prev = pixelLuma(prevPixels[static_cast<size_t>(y) * kOw + (x - 3u)]);
                    const float ad = std::fabs(cur - prev);
                    deltas.push_back(ad);
                    sqSum += static_cast<double>(ad) * ad;
                }
            }
        }
        prevPixels = outPixels;
    }

    WalkShimmerMetrics m{};
    m.rmsDelta = static_cast<float>(std::sqrt(sqSum / deltas.size()));
    const size_t p99Idx = static_cast<size_t>(0.99 * static_cast<double>(deltas.size() - 1));
    std::nth_element(deltas.begin(), deltas.begin() + p99Idx, deltas.end());
    m.p99Jump = deltas[p99Idx];
    return m;
}
#endif

} // namespace

int main() {
    GpuHarness gpu;
    if (!gpu.initDevice()) {
        std::printf("Failed to initialize D3D11 device\n");
        return 1;
    }

    bool allOk = true;

#if defined(EDPE_ENABLE_FSR)
    {
        edpe::AmdUpscalerBackend fsrBackend;
        if (fsrBackend.recommendedJitterPhaseCount(32u, 2560, 1440, 2560, 1440) != 8u ||
            fsrBackend.recommendedJitterPhaseCount(32u, 128, 128, 192, 192) != 18u ||
            fsrBackend.recommendedJitterPhaseCount(32u, 1280, 720, 2560, 1440) != 32u ||
            fsrBackend.recommendedJitterPhaseCount(32u, 1280, 720, 3840, 2160) != 72u ||
            fsrBackend.recommendedJitterPhaseCount(32u, 0, 0, 0, 0) != 8u) {
            std::printf("  -> FAIL: AmdUpscalerBackend recommendedJitterPhaseCount mismatch\n");
            allOk = false;
        }
        const unsigned quality1440Phases = fsrBackend.recommendedJitterPhaseCount(32u, 1707, 960, 2560, 1440);
        if (quality1440Phases != 17u) {
            std::printf("  -> FAIL: AmdUpscalerBackend Quality 1440p phase count %u != 17\n", quality1440Phases);
            allOk = false;
        }
        if (std::fabs(fsrBackend.getVelocityFactor() - edpe::AmdUpscalerBackend::kDefaultVelocityFactor) > 1e-5f) {
            std::printf("  -> FAIL: AmdUpscalerBackend default velocityFactor mismatch\n");
            allOk = false;
        }
        fsrBackend.setVelocityFactor(-0.5f);
        if (fsrBackend.getVelocityFactor() != 0.0f) {
            std::printf("  -> FAIL: velocityFactor lower clamp failed\n");
            allOk = false;
        }
        fsrBackend.setVelocityFactor(2.0f);
        if (fsrBackend.getVelocityFactor() != 1.0f) {
            std::printf("  -> FAIL: velocityFactor upper clamp failed\n");
            allOk = false;
        }
        fsrBackend.setVelocityFactor(edpe::AmdUpscalerBackend::kDefaultVelocityFactor);

        if (!fsrBackend.initialize(gpu.device.Get(), gpu.context.Get())) {
            std::printf("Failed to initialize AmdUpscalerBackend: %ls\n", fsrBackend.unavailableReason());
            return 2;
        }
        allOk &= runSuiteForBackend("AMD_FSR_NativeAA", gpu, fsrBackend, 128, 128, 128, 128);
        allOk &= runSuiteForBackend("AMD_FSR_QualitySR", gpu, fsrBackend, 128, 128, 192, 192);

        const PanningThinLineMetrics panVf100 = measureRealisticSlowPan(gpu, fsrBackend, 1.00f);
        const PanningThinLineMetrics panVf020 = measureRealisticSlowPan(gpu, fsrBackend, 0.20f);
        const PanningThinLineMetrics panVf000 = measureRealisticSlowPan(gpu, fsrBackend, 0.00f);
        std::printf(
            "[AMD_FSR_RealisticSlowPan 1920x64 0.55px @ +0.35px/f]\n"
            "  vf=1.00 (SDK default): peakSpan=%.4f rmsDelta=%.4f meanPeak=%.4f\n"
            "  vf=0.20:               peakSpan=%.4f rmsDelta=%.4f meanPeak=%.4f\n"
            "  vf=0.00 (EDPE def):    peakSpan=%.4f rmsDelta=%.4f meanPeak=%.4f\n",
            panVf100.peakSpan, panVf100.rmsStepDelta, panVf100.meanPeak,
            panVf020.peakSpan, panVf020.rmsStepDelta, panVf020.meanPeak,
            panVf000.peakSpan, panVf000.rmsStepDelta, panVf000.meanPeak);
        if (!(panVf000.rmsStepDelta < panVf100.rmsStepDelta && panVf000.peakSpan < panVf100.peakSpan)) {
            std::printf("  -> FAIL: vf=0.00 did not improve slow-pan stability over vf=1.00\n");
            allOk = false;
        }

        const WalkShimmerMetrics walkVf100 = measurePerfSrWalkingShimmer(gpu, fsrBackend, 1.00f);
        const WalkShimmerMetrics walkVf020 = measurePerfSrWalkingShimmer(gpu, fsrBackend, 0.20f);
        const WalkShimmerMetrics walkVf000 = measurePerfSrWalkingShimmer(gpu, fsrBackend, 0.00f);
        fsrBackend.setVelocityFactor(edpe::AmdUpscalerBackend::kDefaultVelocityFactor);
        std::printf(
            "[AMD_FSR_PerfSR_0.5x_Walk 1280x64->2560x128 @ +1.50px/f]\n"
            "  vf=1.00 (SDK default): rmsDelta=%.4f p99Jump=%.4f\n"
            "  vf=0.20:               rmsDelta=%.4f p99Jump=%.4f\n"
            "  vf=0.00 (EDPE def):    rmsDelta=%.4f p99Jump=%.4f\n",
            walkVf100.rmsDelta, walkVf100.p99Jump,
            walkVf020.rmsDelta, walkVf020.p99Jump,
            walkVf000.rmsDelta, walkVf000.p99Jump);
        if (!(walkVf000.rmsDelta < walkVf020.rmsDelta && walkVf000.rmsDelta < walkVf100.rmsDelta &&
              walkVf000.p99Jump < walkVf020.p99Jump && walkVf000.p99Jump < walkVf100.p99Jump)) {
            std::printf("  -> FAIL: vf=0.00 did not strictly reduce 0.5x walking shimmer vs vf=0.20 and vf=1.00\n");
            allOk = false;
        }
        fsrBackend.shutdown();
    }
#endif

    {
        edpe::TaaUpscalerBackend taaBackend;
        if (taaBackend.recommendedJitterPhaseCount(32u, 2560, 1440, 2560, 1440) != 32u) {
            std::printf("  -> FAIL: TaaUpscalerBackend recommendedJitterPhaseCount != 32\n");
            allOk = false;
        }
        if (!taaBackend.initialize(gpu.device.Get(), gpu.context.Get())) {
            std::printf("Failed to initialize TaaUpscalerBackend: %ls\n", taaBackend.unavailableReason());
            return 3;
        }
        allOk &= runSuiteForBackend("Native_D3D11_TAA", gpu, taaBackend, 128, 128, 128, 128);
        taaBackend.shutdown();
    }

    return allOk ? 0 : 10;
}
