#include "ngx_context.h"
#include "sharpen_pass.h"
#include "taa_shader.h"
#include "temporal_inputs.h"
#include "temporal_math.h"
#include "upscaler_backend.h"

#include <DirectXPackedVector.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
    DXGI_FORMAT colorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT depthFormat = DXGI_FORMAT_R32_FLOAT;

    ComPtr<ID3D11Texture2D> colorTex;
    ComPtr<ID3D11ShaderResourceView> colorSrv;
    ComPtr<ID3D11Texture2D> depthTex[2];
    ComPtr<ID3D11ShaderResourceView> depthSrv[2];
    ComPtr<ID3D11Texture2D> depthUploadStagingTex;
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

    bool allocateSurfaces(uint32_t renderW, uint32_t renderH, uint32_t outputW, uint32_t outputH,
                          DXGI_FORMAT cFmt = DXGI_FORMAT_R8G8B8A8_UNORM,
                          DXGI_FORMAT dFmt = DXGI_FORMAT_R32_FLOAT) {
        rw = renderW;
        rh = renderH;
        ow = outputW;
        oh = outputH;
        colorFormat = cFmt;
        depthFormat = dFmt;
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
        depthUploadStagingTex.Reset();
        motionTex.Reset(); motionSrv.Reset();
        outputTex.Reset(); outputRtv.Reset();
        stagingTex.Reset();

        if (!makeTex(rw, rh, colorFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                     D3D11_USAGE_DEFAULT, 0, colorTex) ||
            FAILED(device->CreateShaderResourceView(colorTex.Get(), nullptr, &colorSrv))) {
            return false;
        }
        const DXGI_FORMAT depthSrvFmt = (depthFormat == DXGI_FORMAT_R32G8X24_TYPELESS)
            ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
            : depthFormat;
        for (int i = 0; i < 2; ++i) {
            if (!makeTex(rw, rh, depthFormat, D3D11_BIND_SHADER_RESOURCE,
                         D3D11_USAGE_DEFAULT, 0, depthTex[i])) {
                return false;
            }
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = depthSrvFmt;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            if (FAILED(device->CreateShaderResourceView(depthTex[i].Get(), &srvDesc, &depthSrv[i]))) {
                return false;
            }
        }
        if (depthFormat == DXGI_FORMAT_R32G8X24_TYPELESS) {
            if (!makeTex(rw, rh, depthFormat, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_WRITE, depthUploadStagingTex)) {
                return false;
            }
        }
        if (!makeTex(rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE,
                     D3D11_USAGE_DEFAULT, 0, motionTex) ||
            FAILED(device->CreateShaderResourceView(motionTex.Get(), nullptr, &motionSrv))) {
            return false;
        }
        if (!makeTex(ow, oh, colorFormat,
                     D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                     D3D11_USAGE_DEFAULT, 0, outputTex) ||
            FAILED(device->CreateRenderTargetView(outputTex.Get(), nullptr, &outputRtv))) {
            return false;
        }
        if (!makeTex(ow, oh, colorFormat, 0,
                     D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, stagingTex)) {
            return false;
        }
        return true;
    }

    bool ensureSurfaces(uint32_t renderW, uint32_t renderH, uint32_t outputW, uint32_t outputH,
                        DXGI_FORMAT cFmt = DXGI_FORMAT_R8G8B8A8_UNORM,
                        DXGI_FORMAT dFmt = DXGI_FORMAT_R32_FLOAT) {
        if (colorTex && rw == renderW && rh == renderH && ow == outputW && oh == outputH &&
            colorFormat == cFmt && depthFormat == dFmt) {
            return true;
        }
        return allocateSurfaces(renderW, renderH, outputW, outputH, cFmt, dFmt);
    }

    bool uploadDepth(const std::vector<float>& depthData) {
        if (depthFormat == DXGI_FORMAT_R32_FLOAT) {
            context->UpdateSubresource(depthTex[currDepthIdx].Get(), 0, nullptr, depthData.data(), rw * sizeof(float), 0);
            return true;
        }
        if (depthFormat == DXGI_FORMAT_R32G8X24_TYPELESS && depthUploadStagingTex) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(depthUploadStagingTex.Get(), 0, D3D11_MAP_WRITE, 0, &mapped))) {
                return false;
            }
            struct Depth32Stencil32 {
                float d;
                uint32_t s;
            };
            auto* dstBytes = static_cast<uint8_t*>(mapped.pData);
            for (uint32_t y = 0; y < rh; ++y) {
                auto* dstRow = reinterpret_cast<Depth32Stencil32*>(dstBytes + static_cast<size_t>(y) * mapped.RowPitch);
                const float* srcRow = depthData.data() + static_cast<size_t>(y) * rw;
                for (uint32_t x = 0; x < rw; ++x) {
                    dstRow[x].d = srcRow[x];
                    dstRow[x].s = 0u;
                }
            }
            context->Unmap(depthUploadStagingTex.Get(), 0);
            context->CopyResource(depthTex[currDepthIdx].Get(), depthUploadStagingTex.Get());
            return true;
        }
        return false;
    }

    edpe::TemporalFrameInputs makeInputs(float jitterX, float jitterY, float prevJitterX, float prevJitterY,
                                         bool reset, float sharpness = 0.0f) const {
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
        inputs.sharpness = sharpness;
        inputs.reset = reset;
        inputs.depthInverted = true;
        inputs.depthInfinite = true;
        inputs.motionJittered = false;
        inputs.motionIsLowRes = true;
        inputs.colorIsHdr = (colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                             colorFormat == DXGI_FORMAT_R11G11B10_FLOAT);
        inputs.cameraNow.scaleX = 1.0f;
        inputs.cameraNow.scaleY = 1.0f;
        inputs.cameraNow.depthB = kDepthB;
        inputs.cameraPrevious = inputs.cameraNow;
        return inputs;
    }

    bool uploadAndEvaluateLuma(edpe::IUpscalerBackend& backend, const FrameBuffers& fb,
                               float jitterX, float jitterY, float prevJitterX, float prevJitterY,
                               bool reset, std::vector<float>& outLuma) {
        if (fb.renderW != rw || fb.renderH != rh) {
            return false;
        }
        if (reset) {
            backend.resetHistory();
            hasPrevDepth = false;
        } else {
            currDepthIdx ^= 1;
        }

        if (colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            const size_t n = static_cast<size_t>(rw) * rh;
            std::vector<uint16_t> hdr(n * 4u);
            for (size_t i = 0; i < n; ++i) {
                const uint32_t c = fb.color[i];
                const float r = unorm8ToFloat(static_cast<uint8_t>(c & 0xFFu));
                const float g = unorm8ToFloat(static_cast<uint8_t>((c >> 8) & 0xFFu));
                const float b = unorm8ToFloat(static_cast<uint8_t>((c >> 16) & 0xFFu));
                const float a = unorm8ToFloat(static_cast<uint8_t>((c >> 24) & 0xFFu));
                hdr[i * 4u + 0u] = DirectX::PackedVector::XMConvertFloatToHalf(r);
                hdr[i * 4u + 1u] = DirectX::PackedVector::XMConvertFloatToHalf(g);
                hdr[i * 4u + 2u] = DirectX::PackedVector::XMConvertFloatToHalf(b);
                hdr[i * 4u + 3u] = DirectX::PackedVector::XMConvertFloatToHalf(a);
            }
            context->UpdateSubresource(colorTex.Get(), 0, nullptr, hdr.data(), rw * 8u, 0);
        } else {
            context->UpdateSubresource(colorTex.Get(), 0, nullptr, fb.color.data(), rw * sizeof(uint32_t), 0);
        }
        if (!uploadDepth(fb.depth)) {
            return false;
        }
        context->UpdateSubresource(motionTex.Get(), 0, nullptr, fb.motion.data(), rw * sizeof(uint32_t), 0);

        const auto inputs = makeInputs(jitterX, jitterY, prevJitterX, prevJitterY, reset, 0.0f);
        if (!backend.evaluate(context.Get(), inputs, outputRtv.Get(), outputTex.Get())) {
            return false;
        }

        hasPrevDepth = true;

        context->CopyResource(stagingTex.Get(), outputTex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            return false;
        }
        outLuma.resize(static_cast<size_t>(ow) * oh);
        const auto* srcBytes = static_cast<const uint8_t*>(mapped.pData);
        if (colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            for (uint32_t y = 0; y < oh; ++y) {
                const auto* row = reinterpret_cast<const uint16_t*>(srcBytes + static_cast<size_t>(y) * mapped.RowPitch);
                for (uint32_t x = 0; x < ow; ++x) {
                    const float r = DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4u + 0u]);
                    const float g = DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4u + 1u]);
                    const float b = DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4u + 2u]);
                    outLuma[static_cast<size_t>(y) * ow + x] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                }
            }
        } else {
            for (uint32_t y = 0; y < oh; ++y) {
                const auto* row = reinterpret_cast<const uint32_t*>(srcBytes + static_cast<size_t>(y) * mapped.RowPitch);
                for (uint32_t x = 0; x < ow; ++x) {
                    outLuma[static_cast<size_t>(y) * ow + x] = pixelLuma(row[x]);
                }
            }
        }
        context->Unmap(stagingTex.Get(), 0);
        return true;
    }

    bool uploadAndEvaluate(edpe::IUpscalerBackend& backend, const FrameBuffers& fb,
                           float jitterX, float jitterY, float prevJitterX, float prevJitterY,
                           bool reset, std::vector<uint32_t>& outPixels, float sharpness = 0.0f) {
        if (fb.renderW != rw || fb.renderH != rh) {
            return false;
        }
        if (reset) {
            backend.resetHistory();
            hasPrevDepth = false;
        } else {
            currDepthIdx ^= 1;
        }

        context->UpdateSubresource(colorTex.Get(), 0, nullptr, fb.color.data(), rw * sizeof(uint32_t), 0);
        if (!uploadDepth(fb.depth)) {
            return false;
        }
        context->UpdateSubresource(motionTex.Get(), 0, nullptr, fb.motion.data(), rw * sizeof(uint32_t), 0);

        const auto inputs = makeInputs(jitterX, jitterY, prevJitterX, prevJitterY, reset, sharpness);
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

StepEdgeMetrics measureStepEdgeXY(GpuHarness& gpu, edpe::IUpscalerBackend& backend,
                                  uint32_t renderW, uint32_t renderH,
                                  uint32_t outputW, uint32_t outputH,
                                  float signX, float signY,
                                  unsigned overridePhaseCount = 0,
                                  float sharpness = 0.0f) {
    if (!gpu.ensureSurfaces(renderW, renderH, outputW, outputH)) {
        return {999.0f, 999.0f, 999.0f, 999.0f, 999.0f, 999.0f, 999.0f, 999.0f};
    }
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
    const unsigned phaseCount = overridePhaseCount ? overridePhaseCount
                                                 : backend.recommendedJitterPhaseCount(32u, renderW, renderH, outputW, outputH);

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

        const float passJx = j.pixelX * signX;
        const float passJy = j.pixelY * signY;
        const float passPrevJx = (t > 0) ? (prevJ.pixelX * signX) : 0.0f;
        const float passPrevJy = (t > 0) ? (prevJ.pixelY * signY) : 0.0f;

        if (!gpu.uploadAndEvaluate(backend, fb, passJx, passJy, passPrevJx, passPrevJy, t == 0, outPixels, sharpness)) {
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

StepEdgeMetrics measureStepEdge(GpuHarness& gpu, edpe::IUpscalerBackend& backend,
                                uint32_t renderW, uint32_t renderH,
                                uint32_t outputW, uint32_t outputH,
                                float jitterSign) {
    return measureStepEdgeXY(gpu, backend, renderW, renderH, outputW, outputH, jitterSign, jitterSign);
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
                                       uint32_t outputW, uint32_t outputH,
                                       float signX = 1.0f, float signY = 1.0f,
                                       unsigned overridePhaseCount = 0,
                                       float sharpness = 0.0f) {
    if (!gpu.ensureSurfaces(renderW, renderH, outputW, outputH)) {
        return {999.0f, 999.0f, 0.0f, 999.0f, 999.0f};
    }
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
    const unsigned phaseCount = overridePhaseCount ? overridePhaseCount
                                                 : backend.recommendedJitterPhaseCount(32u, renderW, renderH, outputW, outputH);

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

        if (!gpu.uploadAndEvaluate(backend, fb, j.pixelX * signX, j.pixelY * signY,
                                   t > 0 ? prevJ.pixelX * signX : 0.0f,
                                   t > 0 ? prevJ.pixelY * signY : 0.0f,
                                   t == 0, outPixels, sharpness)) {
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
                                                  uint32_t outputW, uint32_t outputH,
                                                  float signX = 1.0f, float signY = 1.0f,
                                                  unsigned overridePhaseCount = 0,
                                                  float sharpness = 0.0f) {
    if (!gpu.ensureSurfaces(renderW, renderH, outputW, outputH)) {
        return {999.0f, 999.0f, 0.0f};
    }
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
    const unsigned phaseCount = overridePhaseCount ? overridePhaseCount
                                                 : backend.recommendedJitterPhaseCount(32u, renderW, renderH, outputW, outputH);

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
                                   t == 0, outPixels, sharpness)) {
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
                                         uint32_t outputW, uint32_t outputH,
                                         float signX = 1.0f, float signY = 1.0f,
                                         unsigned overridePhaseCount = 0,
                                         float sharpness = 0.0f) {
    if (!gpu.ensureSurfaces(renderW, renderH, outputW, outputH)) {
        return 999.0f;
    }
    constexpr int kTotalFrames = 36;
    constexpr int kDisappearFrame = 20;
    constexpr int kMeasureFrame = 30;

    const float scaleX = static_cast<float>(outputW) / static_cast<float>(renderW);
    const float scaleY = static_cast<float>(outputH) / static_cast<float>(renderH);

    FrameBuffers fb;
    std::vector<uint32_t> outPixels;
    edpe::ProjectionJitter prevJ{};
    float residualAtFrame30 = 999.0f;
    const unsigned phaseCount = overridePhaseCount ? overridePhaseCount
                                                 : backend.recommendedJitterPhaseCount(32u, renderW, renderH, outputW, outputH);

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

        if (!gpu.uploadAndEvaluate(backend, fb, j.pixelX * signX, j.pixelY * signY,
                                   t > 0 ? prevJ.pixelX * signX : 0.0f,
                                   t > 0 ? prevJ.pixelY * signY : 0.0f,
                                   t == 0, outPixels, sharpness)) {
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

struct GpuTimingStats {
    bool ok = false;
    double avgMs = 0.0;
    double minMs = 0.0;
    double medianMs = 0.0;
    double maxMs = 0.0;
    double fastest5[5]{};
    int fastestCount = 0;
};

StepEdgeMetrics measureStepEdgeScaled(GpuHarness& gpu, edpe::IUpscalerBackend& backend,
                                      uint32_t renderW, uint32_t renderH,
                                      uint32_t outputW, uint32_t outputH,
                                      float signX, float signY,
                                      DXGI_FORMAT colorFmt = DXGI_FORMAT_R8G8B8A8_UNORM,
                                      int totalFrames = 48,
                                      int warmupFrames = 16,
                                      bool printTrace = false) {
    if (!gpu.allocateSurfaces(renderW, renderH, outputW, outputH, colorFmt, DXGI_FORMAT_R32_FLOAT)) {
        return {999.0f, 999.0f, 999.0f, 999.0f, 999.0f, 999.0f, 999.0f, 999.0f};
    }
    const int kFrames = totalFrames;
    const int kWarmup = warmupFrames;

    const float scaleX = static_cast<float>(outputW) / static_cast<float>(renderW);
    const float scaleY = static_cast<float>(outputH) / static_cast<float>(renderH);

    // Top half: vertical step edge at vertEdgeU = 0.25*renderW + 0.5.
    // Bottom half: horizontal step edge at horizEdgeU = 0.75*renderH + 0.5.
    const float vertEdgeU = static_cast<float>(renderW) * 0.25f + 0.5f;
    const float horizEdgeU = static_cast<float>(renderH) * 0.75f + 0.5f;
    constexpr float kBright = 0.90f;

    const float edgeOutX = vertEdgeU * scaleX;
    const float edgeOutY = horizEdgeU * scaleY;
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
    std::vector<float> outLuma;
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
                    fg = (ux >= vertEdgeU);
                } else {
                    fg = (uy >= horizEdgeU);
                }
                if (fg) {
                    fb.color[idx] = packRgba8(kBright, kBright, kBright);
                    fb.depth[idx] = kFgDepth;
                }
            }
        }

        const float passJx = j.pixelX * signX;
        const float passJy = j.pixelY * signY;
        const float passPrevJx = (t > 0) ? (prevJ.pixelX * signX) : 0.0f;
        const float passPrevJy = (t > 0) ? (prevJ.pixelY * signY) : 0.0f;

        if (!gpu.uploadAndEvaluateLuma(backend, fb, passJx, passJy, passPrevJx, passPrevJy, t == 0, outLuma)) {
            return {999.0f, 999.0f, 999.0f, 999.0f, 999.0f, 999.0f, 999.0f, 999.0f};
        }
        prevJ = j;

        if (t >= kWarmup) {
            const int wx0 = std::max(0, static_cast<int>(std::round((vertEdgeU - 8.0f) * scaleX)));
            const int wx1 = std::min(static_cast<int>(outputW) - 1, static_cast<int>(std::round((vertEdgeU + 8.0f) * scaleX)));
            const uint32_t ry0 = static_cast<uint32_t>(std::round(static_cast<float>(renderH) * 0.10f * scaleY));
            const uint32_t ry1 = static_cast<uint32_t>(std::round(static_cast<float>(renderH) * 0.35f * scaleY));
            double sumEdgeX = 0.0;
            int rowCount = 0;
            std::vector<float> bandX;
            bandX.reserve(static_cast<size_t>(ry1 - ry0) * (bx1 - bx0 + 1));
            for (uint32_t y = ry0; y < ry1; ++y) {
                double normSum = 0.0;
                for (int x = wx0; x <= wx1; ++x) {
                    const float l = outLuma[static_cast<size_t>(y) * outputW + x];
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

            const int wy0 = std::max(0, static_cast<int>(std::round((horizEdgeU - 8.0f) * scaleY)));
            const int wy1 = std::min(static_cast<int>(outputH) - 1, static_cast<int>(std::round((horizEdgeU + 8.0f) * scaleY)));
            const uint32_t cx0 = static_cast<uint32_t>(std::round(static_cast<float>(renderW) * 0.15f * scaleX));
            const uint32_t cx1 = static_cast<uint32_t>(std::round(static_cast<float>(renderW) * 0.65f * scaleX));
            double sumEdgeY = 0.0;
            int colCount = 0;
            std::vector<float> bandY;
            bandY.reserve(static_cast<size_t>(cx1 - cx0) * (by1 - by0 + 1));
            for (uint32_t x = cx0; x < cx1; ++x) {
                double normSum = 0.0;
                for (int y = wy0; y <= wy1; ++y) {
                    const float l = outLuma[static_cast<size_t>(y) * outputW + x];
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
    if (printTrace) {
        std::printf("    posX trace (first %zu post-warmup frames):", std::min<size_t>(16u, posXList.size()));
        for (size_t i = 0; i < std::min<size_t>(16u, posXList.size()); ++i) {
            std::printf(" %.4f", posXList[i]);
        }
        std::printf("\n");
    }
    calcStats(posXList, m.spanX, m.stdX);
    calcStats(posYList, m.spanY, m.stdY);
    m.errX = errXCount > 0 ? static_cast<float>(errXSum / errXCount) : 999.0f;
    m.errY = errYCount > 0 ? static_cast<float>(errYSum / errYCount) : 999.0f;
    m.rmsX = calcRmsDelta(bandXFrames);
    m.rmsY = calcRmsDelta(bandYFrames);
    return m;
}

bool seedBenchmarkSurfaces(GpuHarness& gpu) {
    const size_t n = static_cast<size_t>(gpu.rw) * gpu.rh;
    if (gpu.colorFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        std::vector<uint16_t> hdr(n * 4u);
        for (uint32_t y = 0; y < gpu.rh; ++y) {
            for (uint32_t x = 0; x < gpu.rw; ++x) {
                const bool hi = (((x >> 4) ^ (y >> 4)) & 1u) != 0u;
                const float c = hi ? 0.85f : 0.15f;
                const uint16_t hc = DirectX::PackedVector::XMConvertFloatToHalf(c);
                const uint16_t ha = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
                const size_t base = (static_cast<size_t>(y) * gpu.rw + x) * 4u;
                hdr[base + 0] = hc;
                hdr[base + 1] = hc;
                hdr[base + 2] = hc;
                hdr[base + 3] = ha;
            }
        }
        gpu.context->UpdateSubresource(gpu.colorTex.Get(), 0, nullptr, hdr.data(), gpu.rw * 8u, 0);
    } else {
        std::vector<uint32_t> ldr(n);
        for (uint32_t y = 0; y < gpu.rh; ++y) {
            for (uint32_t x = 0; x < gpu.rw; ++x) {
                const bool hi = (((x >> 4) ^ (y >> 4)) & 1u) != 0u;
                const float c = hi ? 0.85f : 0.15f;
                ldr[static_cast<size_t>(y) * gpu.rw + x] = packRgba8(c, c, c);
            }
        }
        gpu.context->UpdateSubresource(gpu.colorTex.Get(), 0, nullptr, ldr.data(), gpu.rw * sizeof(uint32_t), 0);
    }

    std::vector<float> depth(n);
    for (uint32_t y = 0; y < gpu.rh; ++y) {
        for (uint32_t x = 0; x < gpu.rw; ++x) {
            const bool fg = (x > gpu.rw / 4 && x < (gpu.rw * 3) / 4);
            depth[static_cast<size_t>(y) * gpu.rw + x] = fg ? kFgDepth : kBgDepth;
        }
    }
    if (!gpu.uploadDepth(depth)) {
        return false;
    }

    std::vector<uint32_t> motion(n, packHalf2(0.25f, -0.15f));
    gpu.context->UpdateSubresource(gpu.motionTex.Get(), 0, nullptr, motion.data(), gpu.rw * sizeof(uint32_t), 0);
    return true;
}

GpuTimingStats measureUpscalerGpuTimeMs(GpuHarness& gpu, edpe::IUpscalerBackend& backend,
                                        uint32_t renderW, uint32_t renderH,
                                        uint32_t outputW, uint32_t outputH,
                                        DXGI_FORMAT colorFmt, float sharpness) {
    GpuTimingStats stats{};
    if (!gpu.allocateSurfaces(renderW, renderH, outputW, outputH, colorFmt, DXGI_FORMAT_R32_FLOAT)) {
        return stats;
    }
    if (!seedBenchmarkSurfaces(gpu)) {
        return stats;
    }

    ComPtr<ID3D11Query> disjointQ, startQ, endQ;
    D3D11_QUERY_DESC dqDesc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    D3D11_QUERY_DESC tsDesc{ D3D11_QUERY_TIMESTAMP, 0 };
    if (FAILED(gpu.device->CreateQuery(&dqDesc, &disjointQ)) ||
        FAILED(gpu.device->CreateQuery(&tsDesc, &startQ)) ||
        FAILED(gpu.device->CreateQuery(&tsDesc, &endQ))) {
        return stats;
    }

    constexpr int kWarmup = 10;
    constexpr int kTimed = 30;
    const unsigned phaseCount = backend.recommendedJitterPhaseCount(32u, renderW, renderH, outputW, outputH);
    edpe::ProjectionJitter prevJ{};

    std::vector<double> samples;
    samples.reserve(kTimed);

    for (int t = 0; t < kWarmup + kTimed; ++t) {
        const auto j = edpe::projectionJitter(static_cast<uint64_t>(t), renderW, renderH, phaseCount);
        const bool isReset = (t == 0);
        if (isReset) {
            backend.resetHistory();
            gpu.hasPrevDepth = false;
        }
        const auto inputs = gpu.makeInputs(
            j.pixelX, j.pixelY,
            t > 0 ? prevJ.pixelX : 0.0f,
            t > 0 ? prevJ.pixelY : 0.0f,
            isReset, sharpness);
        prevJ = j;

        const bool timed = (t >= kWarmup);
        if (timed) {
            gpu.context->Begin(disjointQ.Get());
            gpu.context->End(startQ.Get());
        }
        if (!backend.evaluate(gpu.context.Get(), inputs, gpu.outputRtv.Get(), gpu.outputTex.Get())) {
            return stats;
        }
        gpu.hasPrevDepth = true;
        if (timed) {
            gpu.context->End(endQ.Get());
            gpu.context->End(disjointQ.Get());
            gpu.context->Flush();

            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
            UINT64 tStart = 0, tEnd = 0;
            while (gpu.context->GetData(disjointQ.Get(), &disjointData, sizeof(disjointData), 0) == S_FALSE) {}
            while (gpu.context->GetData(startQ.Get(), &tStart, sizeof(tStart), 0) == S_FALSE) {}
            while (gpu.context->GetData(endQ.Get(), &tEnd, sizeof(tEnd), 0) == S_FALSE) {}
            if (!disjointData.Disjoint && disjointData.Frequency > 0 && tEnd >= tStart) {
                const double ms = static_cast<double>(tEnd - tStart) * 1000.0 /
                                  static_cast<double>(disjointData.Frequency);
                samples.push_back(ms);
            }
        }
    }

    if (!samples.empty()) {
        std::sort(samples.begin(), samples.end());
        double sumMs = 0.0;
        for (double v : samples) sumMs += v;
        stats.ok = true;
        stats.avgMs = sumMs / static_cast<double>(samples.size());
        stats.minMs = samples.front();
        stats.medianMs = samples[samples.size() / 2u];
        stats.maxMs = samples.back();
        stats.fastestCount = static_cast<int>(std::min<size_t>(5u, samples.size()));
        for (int i = 0; i < stats.fastestCount; ++i) {
            stats.fastest5[i] = samples[i];
        }
    }
    return stats;
}

GpuTimingStats measureSharpenGpuTimeMs(GpuHarness& gpu, uint32_t width, uint32_t height,
                                       float sharpness, bool hdr, bool directRtv = false) {
    GpuTimingStats stats{};
    const DXGI_FORMAT fmt = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!gpu.allocateSurfaces(width, height, width, height, fmt, DXGI_FORMAT_R32_FLOAT)) {
        return stats;
    }
    if (!seedBenchmarkSurfaces(gpu)) {
        return stats;
    }

    edpe::SharpenPass sharpen;
    if (!sharpen.initialize(gpu.device.Get(), gpu.context.Get())) {
        return stats;
    }

    ComPtr<ID3D11Query> disjointQ, startQ, endQ;
    D3D11_QUERY_DESC dqDesc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
    D3D11_QUERY_DESC tsDesc{ D3D11_QUERY_TIMESTAMP, 0 };
    if (FAILED(gpu.device->CreateQuery(&dqDesc, &disjointQ)) ||
        FAILED(gpu.device->CreateQuery(&tsDesc, &startQ)) ||
        FAILED(gpu.device->CreateQuery(&tsDesc, &endQ))) {
        sharpen.shutdown();
        return stats;
    }

    constexpr int kWarmup = 10;
    constexpr int kTimed = 30;
    std::vector<double> samples;
    samples.reserve(kTimed);

    for (int t = 0; t < kWarmup + kTimed; ++t) {
        const bool timed = (t >= kWarmup);
        if (timed) {
            gpu.context->Begin(disjointQ.Get());
            gpu.context->End(startQ.Get());
        }
        const bool ok = directRtv
            ? sharpen.applyToRtv(gpu.context.Get(), gpu.colorSrv.Get(), gpu.outputRtv.Get(),
                                 width, height, sharpness, hdr, false)
            : sharpen.apply(gpu.context.Get(), gpu.outputTex.Get(), sharpness, hdr);
        if (!ok) {
            sharpen.shutdown();
            return stats;
        }
        if (timed) {
            gpu.context->End(endQ.Get());
            gpu.context->End(disjointQ.Get());
            gpu.context->Flush();

            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
            UINT64 tStart = 0, tEnd = 0;
            while (gpu.context->GetData(disjointQ.Get(), &disjointData, sizeof(disjointData), 0) == S_FALSE) {}
            while (gpu.context->GetData(startQ.Get(), &tStart, sizeof(tStart), 0) == S_FALSE) {}
            while (gpu.context->GetData(endQ.Get(), &tEnd, sizeof(tEnd), 0) == S_FALSE) {}
            if (!disjointData.Disjoint && disjointData.Frequency > 0 && tEnd >= tStart) {
                const double ms = static_cast<double>(tEnd - tStart) * 1000.0 /
                                  static_cast<double>(disjointData.Frequency);
                samples.push_back(ms);
            }
        }
    }

    sharpen.shutdown();
    if (!samples.empty()) {
        std::sort(samples.begin(), samples.end());
        double sumMs = 0.0;
        for (double v : samples) sumMs += v;
        stats.ok = true;
        stats.avgMs = sumMs / static_cast<double>(samples.size());
        stats.minMs = samples.front();
        stats.medianMs = samples[samples.size() / 2u];
        stats.maxMs = samples.back();
        stats.fastestCount = static_cast<int>(std::min<size_t>(5u, samples.size()));
        for (int i = 0; i < stats.fastestCount; ++i) {
            stats.fastest5[i] = samples[i];
        }
    }
    return stats;
}

int runDlssDiag(GpuHarness& gpu) {
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    if (SUCCEEDED(gpu.device.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(adapter->GetDesc(&ad))) {
            std::printf("Adapter: %ls (Vendor=0x%04X Device=0x%04X)\n",
                        ad.Description, ad.VendorId, ad.DeviceId);
        }
    }

#if !(defined(EDPE_ENABLE_NGX) && EDPE_ENABLE_NGX)
    std::printf("DLSS diagnostic unavailable: EDPE_ENABLE_NGX is not enabled.\n");
    return 4;
#else
    edpe::NvidiaUpscalerBackend nvidiaBackend;
    if (!nvidiaBackend.initialize(gpu.device.Get(), gpu.context.Get())) {
        std::printf("Failed to initialize NvidiaUpscalerBackend: %ls\n", nvidiaBackend.unavailableReason());
        return 4;
    }
    std::printf("NGX init status: %ls\n", edpe::NgxContext::get_status_message());
    {
        const edpe::NgxOverrideInfo ov = edpe::NgxContext::get_override_info();
        std::printf("NGX override info: version=%s dllOverride=%d drsGlobal=%d overrideApplied=%d runtimePreset=%s\n",
                    ov.dlssVersion[0] ? ov.dlssVersion : "(unknown)",
                    ov.dllOverrideEnabled ? 1 : 0,
                    ov.drsGlobalPresetOverride ? 1 : 0,
                    ov.presetOverrideApplied ? 1 : 0,
                    edpe::ngxPresetDisplayNameA(edpe::NgxContext::get_effective_runtime_preset()));
    }

#if defined(EDPE_ENABLE_FSR)
    edpe::AmdUpscalerBackend fsrBackend;
    if (!fsrBackend.initialize(gpu.device.Get(), gpu.context.Get())) {
        std::printf("Failed to initialize AmdUpscalerBackend: %ls\n", fsrBackend.unavailableReason());
        nvidiaBackend.shutdown();
        return 2;
    }
#endif

    struct PresetInfo {
        uint32_t id;
        const char* name;
    };
    const PresetInfo diagPresets[] = {
        {  5u, "E" },
        { 11u, "K" },
        { 13u, "M" },
    };

    std::printf("\n=== 1. NGX OPTIMAL SETTINGS ===\n");
    const struct {
        uint32_t w, h;
        edpe::UpscaleMode mode;
        const char* label;
    } optQueries[] = {
        { 2560, 1440, edpe::UpscaleMode::Quality,          "2560x1440 Quality" },
        { 2560, 1440, edpe::UpscaleMode::Balanced,         "2560x1440 Balanced" },
        { 2560, 1440, edpe::UpscaleMode::Performance,      "2560x1440 Performance" },
        { 2560, 1440, edpe::UpscaleMode::UltraPerformance, "2560x1440 UltraPerf" },
        { 2560, 1440, edpe::UpscaleMode::NativeAA,         "2560x1440 NativeAA" },
        {  512,  320, edpe::UpscaleMode::Performance,      "512x320 Performance" },
    };
    for (const auto& q : optQueries) {
        edpe::DlssOptimalSettings os{};
        const bool ok = edpe::NgxContext::queryOptimalSettings(q.w, q.h, q.mode, &os);
        std::printf("  %-24s: ok=%d opt=%ux%u min=%ux%u max=%ux%u\n",
                    q.label, ok ? 1 : 0, os.optimalWidth, os.optimalHeight,
                    os.minWidth, os.minHeight, os.maxWidth, os.maxHeight);
    }

    std::printf("\n=== 2. 640x360->1280x720 (50%% Perf SR) STEP-EDGE & JITTER SCALE ===\n");
#if defined(EDPE_ENABLE_FSR)
    {
        const StepEdgeMetrics m = measureStepEdgeScaled(gpu, fsrBackend, 640, 360, 1280, 720, +1.0f, +1.0f);
        std::printf("  [AMD FSR 3.1 (+1,+1)]           span=(%.4f, %.4f) std=(%.4f, %.4f) err=(%.4f, %.4f) rms=(%.4f, %.4f)\n",
                    m.spanX, m.spanY, m.stdX, m.stdY, m.errX, m.errY, m.rmsX, m.rmsY);
    }
#endif
    for (const auto& pr : diagPresets) {
        edpe::NgxContext::set_preset(pr.id);
        const StepEdgeMetrics m = measureStepEdgeScaled(gpu, nvidiaBackend, 640, 360, 1280, 720, +1.0f, +1.0f);
        std::printf("  [DLSS Preset %s (+1,+1) RGBA8]    span=(%.4f, %.4f) std=(%.4f, %.4f) err=(%.4f, %.4f) rms=(%.4f, %.4f) | %ls\n",
                    pr.name, m.spanX, m.spanY, m.stdX, m.stdY, m.errX, m.errY, m.rmsX, m.rmsY,
                    edpe::NgxContext::get_status_message());
    }
    {
        edpe::NgxContext::set_preset(11u);
        const struct {
            float sx, sy;
            const char* label;
        } scaleTests[] = {
            { +0.5f, +0.5f, "(+0.5,+0.5)" },
            { +2.0f, +2.0f, "(+2.0,+2.0)" },
            { -1.0f, -1.0f, "(-1.0,-1.0)" },
            {  0.0f,  0.0f, "( 0.0, 0.0)" },
        };
        for (const auto& st : scaleTests) {
            const StepEdgeMetrics m = measureStepEdgeScaled(gpu, nvidiaBackend, 640, 360, 1280, 720, st.sx, st.sy);
            std::printf("  [DLSS Preset K %s]     span=(%.4f, %.4f) std=(%.4f, %.4f) err=(%.4f, %.4f) rms=(%.4f, %.4f)\n",
                        st.label, m.spanX, m.spanY, m.stdX, m.stdY, m.errX, m.errY, m.rmsX, m.rmsY);
        }
        const StepEdgeMetrics hdrM = measureStepEdgeScaled(
            gpu, nvidiaBackend, 640, 360, 1280, 720, +1.0f, +1.0f, DXGI_FORMAT_R16G16B16A16_FLOAT);
        std::printf("  [DLSS Preset K (+1,+1) FP16 HDR] span=(%.4f, %.4f) std=(%.4f, %.4f) err=(%.4f, %.4f) rms=(%.4f, %.4f) | %ls\n",
                    hdrM.spanX, hdrM.spanY, hdrM.stdX, hdrM.stdY, hdrM.errX, hdrM.errY, hdrM.rmsX, hdrM.rmsY,
                    edpe::NgxContext::get_status_message());
    }

    std::printf("\n=== 3. 1280x720->2560x1440 (1440p 50%% Perf SR) STEP-EDGE ===\n");
#if defined(EDPE_ENABLE_FSR)
    {
        const StepEdgeMetrics m = measureStepEdgeScaled(gpu, fsrBackend, 1280, 720, 2560, 1440, +1.0f, +1.0f);
        std::printf("  [AMD FSR 3.1 (+1,+1)]           span=(%.4f, %.4f) std=(%.4f, %.4f) err=(%.4f, %.4f) rms=(%.4f, %.4f)\n",
                    m.spanX, m.spanY, m.stdX, m.stdY, m.errX, m.errY, m.rmsX, m.rmsY);
    }
#endif
    for (const auto& pr : diagPresets) {
        edpe::NgxContext::set_preset(pr.id);
        const StepEdgeMetrics m = measureStepEdgeScaled(gpu, nvidiaBackend, 1280, 720, 2560, 1440, +1.0f, +1.0f);
        std::printf("  [DLSS Preset %s (+1,+1) RGBA8]    span=(%.4f, %.4f) std=(%.4f, %.4f) err=(%.4f, %.4f) rms=(%.4f, %.4f) | %ls\n",
                    pr.name, m.spanX, m.spanY, m.stdX, m.stdY, m.errX, m.errY, m.rmsX, m.rmsY,
                    edpe::NgxContext::get_status_message());
    }

    std::printf("\n=== 4. GPU TIMING TABLE (1280x720->2560x1440, 10 warmup + 30 timed) ===\n");
    auto printTimingRow = [](const char* label, const GpuTimingStats& st, const wchar_t* extra = nullptr) {
        std::printf("  %-34s ok=%d min=%.3f med=%.3f avg=%.3f max=%.3f fast5=[%.3f, %.3f, %.3f, %.3f, %.3f]%ls%ls\n",
                    label, st.ok ? 1 : 0, st.minMs, st.medianMs, st.avgMs, st.maxMs,
                    st.fastestCount > 0 ? st.fastest5[0] : 0.0,
                    st.fastestCount > 1 ? st.fastest5[1] : 0.0,
                    st.fastestCount > 2 ? st.fastest5[2] : 0.0,
                    st.fastestCount > 3 ? st.fastest5[3] : 0.0,
                    st.fastestCount > 4 ? st.fastest5[4] : 0.0,
                    extra ? L" | " : L"", extra ? extra : L"");
    };
#if defined(EDPE_ENABLE_FSR)
    printTimingRow("AMD FSR 3.1 (sharp=0.50, RGBA8)",
                   measureUpscalerGpuTimeMs(gpu, fsrBackend, 1280, 720, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0.5f));
#endif
    for (const auto& pr : diagPresets) {
        edpe::NgxContext::set_preset(pr.id);
        char lbl[64];
        std::snprintf(lbl, sizeof(lbl), "NVIDIA DLSS Preset %s (RGBA8)", pr.name);
        const auto st = measureUpscalerGpuTimeMs(
            gpu, nvidiaBackend, 1280, 720, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0.0f);
        printTimingRow(lbl, st, edpe::NgxContext::get_status_message());
    }
    printTimingRow("EDPE SharpenPass (sharp=0.50)",
                   measureSharpenGpuTimeMs(gpu, 2560, 1440, 0.5f, false, false));
    printTimingRow("EDPE SharpenPass::applyToRtv",
                   measureSharpenGpuTimeMs(gpu, 2560, 1440, 0.5f, false, true));

    std::printf("\n=== 5. PRESET K (QUALITY/BALANCED) vs PRESET M (PERF 50%%) AT 2560x1440 OUTPUT ===\n");
    {
        const struct {
            uint32_t rw, rh;
            const char* modeLabel;
            int totalFrames;
            int warmupFrames;
            bool trace;
        } cmpModes[] = {
            { 1707, 960, "Quality 1707x960->2560x1440 (66.7%)", 48, 16, false },
            { 1485, 835, "Balanced 1485x835->2560x1440 (58.0%)", 48, 16, false },
            { 1280, 720, "Perf 1280x720->2560x1440 (50.0%, f=96/w=64)", 96, 64, true },
        };
        for (const auto& cm : cmpModes) {
#if defined(EDPE_ENABLE_FSR)
            {
                const StepEdgeMetrics fm = measureStepEdgeScaled(
                    gpu, fsrBackend, cm.rw, cm.rh, 2560, 1440, +1.0f, +1.0f,
                    DXGI_FORMAT_R8G8B8A8_UNORM, cm.totalFrames, cm.warmupFrames, false);
                std::printf("  [AMD FSR 3.1  %s] span=(%.4f, %.4f) std=(%.4f, %.4f) err=(%.4f, %.4f) rms=(%.4f, %.4f)\n",
                            cm.modeLabel, fm.spanX, fm.spanY, fm.stdX, fm.stdY, fm.errX, fm.errY, fm.rmsX, fm.rmsY);
            }
#endif
            edpe::NgxContext::set_preset(11u);
            const StepEdgeMetrics dm = measureStepEdgeScaled(
                gpu, nvidiaBackend, cm.rw, cm.rh, 2560, 1440, +1.0f, +1.0f,
                DXGI_FORMAT_R8G8B8A8_UNORM, cm.totalFrames, cm.warmupFrames, cm.trace);
            std::printf("  [NVIDIA DLSS  %s] span=(%.4f, %.4f) std=(%.4f, %.4f) err=(%.4f, %.4f) rms=(%.4f, %.4f) | %ls\n",
                        cm.modeLabel, dm.spanX, dm.spanY, dm.stdX, dm.stdY, dm.errX, dm.errY, dm.rmsX, dm.rmsY,
                        edpe::NgxContext::get_status_message());
        }

        std::printf("\n  --- GPU Timing for Quality (1707x960) & Balanced (1485x835) -> 2560x1440 ---\n");
#if defined(EDPE_ENABLE_FSR)
        printTimingRow("AMD FSR Quality 1707x960 (sharp=0.50)",
                       measureUpscalerGpuTimeMs(gpu, fsrBackend, 1707, 960, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0.5f));
        printTimingRow("AMD FSR Balanced 1485x835 (sharp=0.50)",
                       measureUpscalerGpuTimeMs(gpu, fsrBackend, 1485, 835, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0.5f));
#endif
        edpe::NgxContext::set_preset(11u);
        printTimingRow("NVIDIA DLSS Quality 1707x960 (Preset K)",
                       measureUpscalerGpuTimeMs(gpu, nvidiaBackend, 1707, 960, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0.0f),
                       edpe::NgxContext::get_status_message());
        edpe::NgxContext::set_preset(11u);
        printTimingRow("NVIDIA DLSS Balanced 1485x835 (Preset K)",
                       measureUpscalerGpuTimeMs(gpu, nvidiaBackend, 1485, 835, 2560, 1440, DXGI_FORMAT_R8G8B8A8_UNORM, 0.0f),
                       edpe::NgxContext::get_status_message());
    }

#if defined(EDPE_ENABLE_FSR)
    fsrBackend.shutdown();
#endif
    nvidiaBackend.shutdown();
    return 0;
#endif
}

struct VerticalLineWidthMetrics {
    float peakSpan = 0.0f;
    float rmsDelta = 0.0f;
    float maxPixDelta = 0.0f;
    float meanPeak = 0.0f;
};

VerticalLineWidthMetrics measureVerticalThinLineWidth(
    GpuHarness& gpu, edpe::IUpscalerBackend& backend,
    uint32_t renderW, uint32_t renderH,
    uint32_t outputW, uint32_t outputH,
    float lineWidthPx, bool depthDiscontinuity,
    unsigned phaseCount, float sharpness) {
    if (!gpu.ensureSurfaces(renderW, renderH, outputW, outputH)) {
        return {999.0f, 999.0f, 999.0f, 0.0f};
    }
    constexpr int kFrames = 64;
    constexpr int kWarmup = 32;
    constexpr float kCenterX = 64.25f;
    const float halfW = lineWidthPx * 0.5f;
    const float lineDepth = depthDiscontinuity ? (kDepthB / 20.0f) : (kDepthB / 40.0f);
    const float bgDepth = kDepthB / 40.0f;
    const float scaleX = static_cast<float>(outputW) / static_cast<float>(renderW);
    const float scaleY = static_cast<float>(outputH) / static_cast<float>(renderH);

    std::vector<float> peakSeries;
    std::vector<std::vector<float>> windowFrames;
    FrameBuffers fb;
    std::vector<uint32_t> outPixels;
    edpe::ProjectionJitter prevJ{};

    for (int t = 0; t < kFrames; ++t) {
        const auto j = edpe::projectionJitter(static_cast<uint64_t>(t), renderW, renderH, phaseCount);
        fb.resize(renderW, renderH);
        std::fill(fb.depth.begin(), fb.depth.end(), bgDepth);

        for (uint32_t iy = 16; iy < renderH - 16; ++iy) {
            for (uint32_t ix = 0; ix < renderW; ++ix) {
                const float sx = static_cast<float>(ix) + 0.5f;
                const float ux = sx - j.pixelX;
                if (std::fabs(ux - kCenterX) <= halfW) {
                    const size_t idx = static_cast<size_t>(iy) * renderW + ix;
                    fb.color[idx] = packRgba8(0.85f, 0.85f, 0.85f);
                    fb.depth[idx] = lineDepth;
                }
            }
        }

        if (!gpu.uploadAndEvaluate(backend, fb, j.pixelX, j.pixelY,
                                   t > 0 ? prevJ.pixelX : 0.0f,
                                   t > 0 ? prevJ.pixelY : 0.0f,
                                   t == 0, outPixels, sharpness)) {
            return {999.0f, 999.0f, 999.0f, 0.0f};
        }
        prevJ = j;

        if (t >= kWarmup) {
            const uint32_t y0 = static_cast<uint32_t>(std::round(32 * scaleY));
            const uint32_t y1 = static_cast<uint32_t>(std::round((renderH - 32) * scaleY));
            const uint32_t x0 = static_cast<uint32_t>(std::round((kCenterX - 3.0f) * scaleX));
            const uint32_t x1 = static_cast<uint32_t>(std::round((kCenterX + 3.0f) * scaleX));
            float peakSum = 0.0f;
            std::vector<float> win;
            for (uint32_t y = y0; y < y1; ++y) {
                float rmax = 0.0f;
                for (uint32_t x = x0; x <= x1; ++x) {
                    const float l = pixelLuma(outPixels[static_cast<size_t>(y) * outputW + x]);
                    rmax = std::max(rmax, l);
                    win.push_back(l);
                }
                peakSum += rmax;
            }
            peakSeries.push_back(peakSum / static_cast<float>(y1 - y0));
            windowFrames.push_back(std::move(win));
        }
    }

    VerticalLineWidthMetrics m{};
    auto [mn, mx] = std::minmax_element(peakSeries.begin(), peakSeries.end());
    m.peakSpan = *mx - *mn;
    double peakMean = 0.0;
    for (float v : peakSeries) peakMean += v;
    m.meanPeak = static_cast<float>(peakMean / peakSeries.size());

    double sqSum = 0.0;
    size_t cnt = 0;
    float maxD = 0.0f;
    for (size_t i = 1; i < windowFrames.size(); ++i) {
        for (size_t k = 0; k < windowFrames[i].size(); ++k) {
            const float d = std::fabs(windowFrames[i][k] - windowFrames[i - 1][k]);
            sqSum += static_cast<double>(d) * d;
            maxD = std::max(maxD, d);
            ++cnt;
        }
    }
    m.rmsDelta = cnt > 0 ? static_cast<float>(std::sqrt(sqSum / cnt)) : 0.0f;
    m.maxPixDelta = maxD;
    return m;
}

int runFsrDiag(GpuHarness& gpu) {
    if (!gpu.allocateSurfaces(128, 128, 128, 128)) {
        std::printf("Failed to allocate surfaces\n");
        return 1;
    }
    const float widths[] = { 0.45f, 0.85f, 1.25f, 1.75f };

#if defined(EDPE_ENABLE_FSR)
    {
        edpe::AmdUpscalerBackend fsrBackend;
        if (!fsrBackend.initialize(gpu.device.Get(), gpu.context.Get())) {
            std::printf("FSR init failed\n");
            return 2;
        }
        const unsigned phaseList[] = { 8u, 15u, 16u, 32u };
        const float sharpList[] = { 0.0f, 0.50f };
        for (float sharp : sharpList) {
            std::printf("\n=== AMD FSR NativeAA 128x128 (vf=0.00, sharpness=%.2f) ===\n", sharp);
            for (unsigned phases : phaseList) {
                const StepEdgeMetrics edge = measureStepEdgeXY(gpu, fsrBackend, 128, 128, 128, 128, +1.0f, +1.0f, phases, sharp);
                const PanningThinLineMetrics pan = measureSlowPanningThinLine(gpu, fsrBackend, 128, 128, 128, 128, +1.0f, +1.0f, phases, sharp);
                const float ghost = measureUnvectoredBlobGhostResidual(gpu, fsrBackend, 128, 128, 128, 128, +1.0f, +1.0f, phases, sharp);
                std::printf("  [phases=%2u] StepEdge span=(%.4f, %.4f) err=(%.4f, %.4f) rms=(%.4f, %.4f) | SlowPan span=%.4f rms=%.4f | Ghost=%.4f\n",
                            phases, edge.spanX, edge.spanY, edge.errX, edge.errY, edge.rmsX, edge.rmsY,
                            pan.peakSpan, pan.rmsStepDelta, ghost);
                for (float w : widths) {
                    const auto flat = measureVerticalThinLineWidth(gpu, fsrBackend, 128, 128, 128, 128, w, false, phases, sharp);
                    const auto disc = measureVerticalThinLineWidth(gpu, fsrBackend, 128, 128, 128, 128, w, true, phases, sharp);
                    std::printf("    w=%.2fpx flat:(span=%.4f rms=%.4f maxD=%.4f pk=%.3f) depthEdge:(span=%.4f rms=%.4f maxD=%.4f pk=%.3f)\n",
                                w, flat.peakSpan, flat.rmsDelta, flat.maxPixDelta, flat.meanPeak,
                                disc.peakSpan, disc.rmsDelta, disc.maxPixDelta, disc.meanPeak);
                }
            }
        }
        fsrBackend.shutdown();
    }
#endif

    {
        edpe::TaaUpscalerBackend taaBackend;
        if (!taaBackend.initialize(gpu.device.Get(), gpu.context.Get())) {
            std::printf("TAA init failed\n");
            return 3;
        }
        std::printf("\n=== Native D3D11 TAA 128x128 (phases=32, sharpness=0.00) ===\n");
        const StepEdgeMetrics edge = measureStepEdgeXY(gpu, taaBackend, 128, 128, 128, 128, +1.0f, +1.0f, 32u, 0.0f);
        const PanningThinLineMetrics pan = measureSlowPanningThinLine(gpu, taaBackend, 128, 128, 128, 128, +1.0f, +1.0f, 32u, 0.0f);
        const float ghost = measureUnvectoredBlobGhostResidual(gpu, taaBackend, 128, 128, 128, 128, +1.0f, +1.0f, 32u, 0.0f);
        std::printf("  [phases=32] StepEdge span=(%.4f, %.4f) err=(%.4f, %.4f) rms=(%.4f, %.4f) | SlowPan span=%.4f rms=%.4f | Ghost=%.4f\n",
                    edge.spanX, edge.spanY, edge.errX, edge.errY, edge.rmsX, edge.rmsY,
                    pan.peakSpan, pan.rmsStepDelta, ghost);
        for (float w : widths) {
            const auto flat = measureVerticalThinLineWidth(gpu, taaBackend, 128, 128, 128, 128, w, false, 32u, 0.0f);
            const auto disc = measureVerticalThinLineWidth(gpu, taaBackend, 128, 128, 128, 128, w, true, 32u, 0.0f);
            std::printf("    w=%.2fpx flat:(span=%.4f rms=%.4f maxD=%.4f pk=%.3f) depthEdge:(span=%.4f rms=%.4f maxD=%.4f pk=%.3f)\n",
                        w, flat.peakSpan, flat.rmsDelta, flat.maxPixDelta, flat.meanPeak,
                        disc.peakSpan, disc.rmsDelta, disc.maxPixDelta, disc.meanPeak);
        }
        taaBackend.shutdown();
    }
    return 0;
}

bool verifyVerticalThinLines(const char* label, GpuHarness& gpu, edpe::IUpscalerBackend& backend, unsigned phases) {
    bool ok = true;
    const float testWidths[] = { 0.85f, 1.75f };
    for (float w : testWidths) {
        const auto m = measureVerticalThinLineWidth(gpu, backend, 128, 128, 128, 128, w, true, phases, 0.0f);
        std::printf("  [%s] VertThinLine(w=%.2fpx, depthEdge): span=%.4f rms=%.4f maxD=%.4f pk=%.3f\n",
                    label, w, m.peakSpan, m.rmsDelta, m.maxPixDelta, m.meanPeak);
        if (!(m.rmsDelta < 0.008f && m.maxPixDelta < 0.040f)) {
            std::printf("  -> FAIL: %s w=%.2fpx vertical line flickered (rms=%.4f, maxD=%.4f)\n",
                        label, w, m.rmsDelta, m.maxPixDelta);
            ok = false;
        }
    }
    return ok;
}

struct UnvectoredBoxMetrics {
    float maxTrailGhost = 0.0f;
    float meanTrailGhost = 0.0f;
    float interiorContrast = 0.0f;
};

UnvectoredBoxMetrics measureUnvectoredMovingTexturedBox(
    GpuHarness& gpu, edpe::IUpscalerBackend& backend) {
    constexpr uint32_t kW = 128;
    constexpr uint32_t kH = 128;
    constexpr int kFrames = 28;
    constexpr int kMeasureFrame = 24;
    constexpr float kSpeedX = 3.0f;
    constexpr float kBoxW = 16.0f;
    constexpr float kBoxH = 24.0f;

    if (!gpu.ensureSurfaces(kW, kH, kW, kH)) {
        return {999.0f, 999.0f, 0.0f};
    }

    FrameBuffers fb;
    std::vector<uint32_t> outPixels;
    edpe::ProjectionJitter prevJ{};
    const unsigned phaseCount = backend.recommendedJitterPhaseCount(32u, kW, kH, kW, kH);
    UnvectoredBoxMetrics m{};

    for (int t = 0; t < kFrames; ++t) {
        const auto j = edpe::projectionJitter(static_cast<uint64_t>(t), kW, kH, phaseCount);
        const float boxX0 = 20.0f + kSpeedX * static_cast<float>(t);
        const float boxX1 = boxX0 + kBoxW;
        const float boxY0 = 52.0f;
        const float boxY1 = boxY0 + kBoxH;

        fb.resize(kW, kH);
        std::fill(fb.depth.begin(), fb.depth.end(), kFgDepth);

        for (uint32_t iy = 0; iy < kH; ++iy) {
            const float uy = static_cast<float>(iy) + 0.5f - j.pixelY;
            for (uint32_t ix = 0; ix < kW; ++ix) {
                const float ux = static_cast<float>(ix) + 0.5f - j.pixelX;
                if (ux >= boxX0 && ux <= boxX1 && uy >= boxY0 && uy <= boxY1) {
                    const int stripePhase = static_cast<int>(std::floor((ux - boxX0) / 2.0f)) & 1;
                    const float v = stripePhase ? 0.90f : 0.15f;
                    fb.color[static_cast<size_t>(iy) * kW + ix] = packRgba8(v, v, v);
                }
            }
        }

        if (!gpu.uploadAndEvaluate(backend, fb, j.pixelX, j.pixelY,
                                   t > 0 ? prevJ.pixelX : 0.0f,
                                   t > 0 ? prevJ.pixelY : 0.0f,
                                   t == 0, outPixels, 0.0f)) {
            return {999.0f, 999.0f, 0.0f};
        }
        prevJ = j;

        if (t == kMeasureFrame) {
            const int trailX0 = std::max(0, static_cast<int>(std::floor(boxX0)) - 6);
            const int trailX1 = std::max(0, static_cast<int>(std::floor(boxX0)) - 4);
            double trailSum = 0.0;
            int trailCnt = 0;
            for (uint32_t y = 56; y < 72; ++y) {
                for (int x = trailX0; x <= trailX1; ++x) {
                    const float g = std::fabs(pixelLuma(outPixels[static_cast<size_t>(y) * kW + x]) - kBgColor);
                    m.maxTrailGhost = std::max(m.maxTrailGhost, g);
                    trailSum += g;
                    ++trailCnt;
                }
            }
            m.meanTrailGhost = (trailCnt > 0) ? static_cast<float>(trailSum / trailCnt) : 999.0f;

            float minIn = 1.0f, maxIn = 0.0f;
            const int inX0 = static_cast<int>(std::ceil(boxX0)) + 2;
            const int inX1 = static_cast<int>(std::floor(boxX1)) - 2;
            for (uint32_t y = 58; y < 70; ++y) {
                for (int x = inX0; x <= inX1; ++x) {
                    const float l = pixelLuma(outPixels[static_cast<size_t>(y) * kW + x]);
                    minIn = std::min(minIn, l);
                    maxIn = std::max(maxIn, l);
                }
            }
            m.interiorContrast = maxIn - minIn;
        }
    }
    return m;
}

float measureUnvectoredMovingThinLineGhost(
    GpuHarness& gpu, edpe::IUpscalerBackend& backend) {
    constexpr uint32_t kW = 128;
    constexpr uint32_t kH = 128;
    constexpr int kFrames = 28;
    constexpr int kMeasureFrame = 24;
    constexpr float kSpeedX = 2.0f;

    if (!gpu.ensureSurfaces(kW, kH, kW, kH)) {
        return 999.0f;
    }

    FrameBuffers fb;
    std::vector<uint32_t> outPixels;
    edpe::ProjectionJitter prevJ{};
    const unsigned phaseCount = backend.recommendedJitterPhaseCount(32u, kW, kH, kW, kH);
    float maxGhost = 0.0f;

    for (int t = 0; t < kFrames; ++t) {
        const auto j = edpe::projectionJitter(static_cast<uint64_t>(t), kW, kH, phaseCount);
        const float lineX = 24.0f + kSpeedX * static_cast<float>(t);

        fb.resize(kW, kH);
        std::fill(fb.depth.begin(), fb.depth.end(), kFgDepth);

        for (uint32_t iy = 16; iy < kH - 16; ++iy) {
            for (uint32_t ix = 0; ix < kW; ++ix) {
                const float ux = static_cast<float>(ix) + 0.5f - j.pixelX;
                if (std::fabs(ux - lineX) <= 0.5f) {
                    fb.color[static_cast<size_t>(iy) * kW + ix] = packRgba8(0.90f, 0.90f, 0.90f);
                }
            }
        }

        if (!gpu.uploadAndEvaluate(backend, fb, j.pixelX, j.pixelY,
                                   t > 0 ? prevJ.pixelX : 0.0f,
                                   t > 0 ? prevJ.pixelY : 0.0f,
                                   t == 0, outPixels, 0.0f)) {
            return 999.0f;
        }
        prevJ = j;

        if (t == kMeasureFrame) {
            const int trailX0 = std::max(0, static_cast<int>(std::floor(lineX)) - 6);
            const int trailX1 = std::max(0, static_cast<int>(std::floor(lineX)) - 4);
            for (uint32_t y = 32; y < 96; ++y) {
                for (int x = trailX0; x <= trailX1; ++x) {
                    const float g = std::fabs(pixelLuma(outPixels[static_cast<size_t>(y) * kW + x]) - kBgColor);
                    maxGhost = std::max(maxGhost, g);
                }
            }
        }
    }
    return maxGhost;
}

float measureVectoredFractionalSharpness(
    GpuHarness& gpu, edpe::IUpscalerBackend& backend, float speedPx) {
    constexpr uint32_t kW = 128;
    constexpr uint32_t kH = 128;
    constexpr int kFrames = 24;

    if (!gpu.ensureSurfaces(kW, kH, kW, kH)) {
        return 0.0f;
    }

    FrameBuffers fb;
    std::vector<uint32_t> outPixels;
    edpe::ProjectionJitter prevJ{};
    const unsigned phaseCount = backend.recommendedJitterPhaseCount(32u, kW, kH, kW, kH);
    const uint32_t mvPacked = packHalf2(-speedPx, 0.0f);
    float contrast = 0.0f;

    for (int t = 0; t < kFrames; ++t) {
        const auto j = edpe::projectionJitter(static_cast<uint64_t>(t), kW, kH, phaseCount);
        fb.resize(kW, kH);
        std::fill(fb.depth.begin(), fb.depth.end(), kFgDepth);
        std::fill(fb.motion.begin(), fb.motion.end(), mvPacked);

        for (uint32_t iy = 16; iy < kH - 16; ++iy) {
            for (uint32_t ix = 0; ix < kW; ++ix) {
                const float ux = static_cast<float>(ix) + 0.5f - j.pixelX;
                const float wx = ux - speedPx * static_cast<float>(t);
                const int cell = static_cast<int>(std::floor(wx * 0.5f)) & 1;
                const float v = cell ? 0.90f : 0.10f;
                fb.color[static_cast<size_t>(iy) * kW + ix] = packRgba8(v, v, v);
            }
        }

        if (!gpu.uploadAndEvaluate(backend, fb, j.pixelX, j.pixelY,
                                   t > 0 ? prevJ.pixelX : 0.0f,
                                   t > 0 ? prevJ.pixelY : 0.0f,
                                   t == 0, outPixels, 0.0f)) {
            return 0.0f;
        }
        prevJ = j;

        if (t == kFrames - 1) {
            double rowMinSum = 0.0, rowMaxSum = 0.0;
            int rows = 0;
            for (uint32_t y = 32; y < 96; ++y) {
                float rMin = 1.0f, rMax = 0.0f;
                for (uint32_t x = 48; x < 96; ++x) {
                    const float l = pixelLuma(outPixels[static_cast<size_t>(y) * kW + x]);
                    rMin = std::min(rMin, l);
                    rMax = std::max(rMax, l);
                }
                rowMinSum += rMin;
                rowMaxSum += rMax;
                ++rows;
            }
            contrast = static_cast<float>((rowMaxSum - rowMinSum) / rows);
        }
    }
    return contrast;
}

} // namespace

int main(int argc, char** argv) {
    GpuHarness gpu;
    if (!gpu.initDevice()) {
        std::printf("Failed to initialize D3D11 device\n");
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dlss-diag") == 0) {
            return runDlssDiag(gpu);
        }
        if (std::strcmp(argv[i], "--fsr-diag") == 0) {
            return runFsrDiag(gpu);
        }
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
        allOk &= verifyVerticalThinLines(
            "AMD_FSR_NativeAA", gpu, fsrBackend,
            fsrBackend.recommendedJitterPhaseCount(32u, 128, 128, 128, 128));
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
        if (taaBackend.recommendedJitterPhaseCount(0u, 2560, 1440, 2560, 1440) != 16u ||
            taaBackend.recommendedJitterPhaseCount(8u, 2560, 1440, 2560, 1440) != 16u ||
            taaBackend.recommendedJitterPhaseCount(0u, 1707, 960, 2560, 1440) != 18u ||
            taaBackend.recommendedJitterPhaseCount(32u, 2560, 1440, 2560, 1440) != 32u) {
            std::printf("  -> FAIL: TaaUpscalerBackend recommendedJitterPhaseCount mismatch\n");
            allOk = false;
        }
        if (!taaBackend.initialize(gpu.device.Get(), gpu.context.Get())) {
            std::printf("Failed to initialize TaaUpscalerBackend: %ls\n", taaBackend.unavailableReason());
            return 3;
        }
        allOk &= runSuiteForBackend("Native_D3D11_TAA", gpu, taaBackend, 128, 128, 128, 128);
        {
            const StepEdgeMetrics taaEdge = measureStepEdge(gpu, taaBackend, 128, 128, 128, 128, +1.0f);
            if (!(taaEdge.rmsX < 0.007f && taaEdge.rmsY < 0.007f)) {
                std::printf("  -> FAIL: Native_D3D11_TAA StepEdge(+jit) rms (%.4f, %.4f) >= 0.007\n",
                            taaEdge.rmsX, taaEdge.rmsY);
                allOk = false;
            }
            if (!(taaEdge.errX < 0.035f && taaEdge.errY < 0.035f)) {
                std::printf("  -> FAIL: Native_D3D11_TAA StepEdge(+jit) default-RCAS err (%.4f, %.4f) >= 0.035\n",
                            taaEdge.errX, taaEdge.errY);
                allOk = false;
            }
            allOk &= verifyVerticalThinLines(
                "Native_D3D11_TAA", gpu, taaBackend,
                taaBackend.recommendedJitterPhaseCount(32u, 128, 128, 128, 128));

            // Shallow-angle 1:6 slope AA quality test under default TAA settings (rcasSharpness = 0.30f)
            {
                constexpr uint32_t kSw = 192;
                constexpr uint32_t kSh = 96;
                if (!gpu.ensureSurfaces(kSw, kSh, kSw, kSh)) {
                    allOk = false;
                } else {
                    constexpr int kFrames = 48;
                    constexpr int kWarmup = 16;
                    const unsigned phaseCount = taaBackend.recommendedJitterPhaseCount(32u, kSw, kSh, kSw, kSh);
                    FrameBuffers fb;
                    std::vector<uint32_t> outPixels;
                    std::vector<std::vector<float>> bandFrames;
                    edpe::ProjectionJitter prevJ{};
                    double taaSqErr = 0.0;
                    double rawSqErr = 0.0;
                    size_t sampleCnt = 0;

                    auto shallowGtLuma = [](uint32_t ix, uint32_t iy) {
                        const float yEdge = 48.0f + (static_cast<float>(ix) + 0.5f - 96.0f) / 6.0f;
                        const float cov = std::clamp((static_cast<float>(iy) + 1.0f) - yEdge, 0.0f, 1.0f);
                        return kBgColor + cov * (0.90f - kBgColor);
                    };

                    for (int t = 0; t < kFrames; ++t) {
                        const auto j = edpe::projectionJitter(static_cast<uint64_t>(t), kSw, kSh, phaseCount);
                        fb.resize(kSw, kSh);
                        for (uint32_t iy = 0; iy < kSh; ++iy) {
                            const float uy = static_cast<float>(iy) + 0.5f - j.pixelY;
                            for (uint32_t ix = 0; ix < kSw; ++ix) {
                                const float ux = static_cast<float>(ix) + 0.5f - j.pixelX;
                                const float yEdge = 48.0f + (ux - 96.0f) / 6.0f;
                                if (uy >= yEdge) {
                                    const size_t idx = static_cast<size_t>(iy) * kSw + ix;
                                    fb.color[idx] = packRgba8(0.90f, 0.90f, 0.90f);
                                    fb.depth[idx] = kFgDepth;
                                }
                            }
                        }
                        if (!gpu.uploadAndEvaluate(taaBackend, fb, j.pixelX, j.pixelY,
                                                   t > 0 ? prevJ.pixelX : 0.0f,
                                                   t > 0 ? prevJ.pixelY : 0.0f,
                                                   t == 0, outPixels, 0.0f)) {
                            allOk = false;
                            break;
                        }
                        prevJ = j;

                        if (t >= kWarmup) {
                            std::vector<float> band;
                            for (uint32_t ix = 16; ix < kSw - 16; ++ix) {
                                const float yEdge = 48.0f + (static_cast<float>(ix) + 0.5f - 96.0f) / 6.0f;
                                for (uint32_t iy = 24; iy < kSh - 24; ++iy) {
                                    if (std::fabs(static_cast<float>(iy) + 0.5f - yEdge) <= 2.5f) {
                                        const float gt = shallowGtLuma(ix, iy);
                                        const float outL = pixelLuma(outPixels[static_cast<size_t>(iy) * kSw + ix]);
                                        const float rawL = pixelLuma(fb.color[static_cast<size_t>(iy) * kSw + ix]);
                                        const double dT = static_cast<double>(outL - gt);
                                        const double dR = static_cast<double>(rawL - gt);
                                        taaSqErr += dT * dT;
                                        rawSqErr += dR * dR;
                                        ++sampleCnt;
                                        band.push_back(outL);
                                    }
                                }
                            }
                            bandFrames.push_back(std::move(band));
                        }
                    }
                    const float taaRmse = sampleCnt > 0 ? static_cast<float>(std::sqrt(taaSqErr / sampleCnt)) : 999.0f;
                    const float rawRmse = sampleCnt > 0 ? static_cast<float>(std::sqrt(rawSqErr / sampleCnt)) : 1.0f;
                    double sqDelta = 0.0;
                    size_t deltaCnt = 0;
                    for (size_t f = 1; f < bandFrames.size(); ++f) {
                        for (size_t k = 0; k < bandFrames[f].size(); ++k) {
                            const double d = static_cast<double>(bandFrames[f][k] - bandFrames[f - 1][k]);
                            sqDelta += d * d;
                            ++deltaCnt;
                        }
                    }
                    const float shallowRmsDelta = deltaCnt > 0 ? static_cast<float>(std::sqrt(sqDelta / deltaCnt)) : 999.0f;
                    std::printf("  [Native_D3D11_TAA] ShallowSlope1_6(defaultRCAS): taaRmse=%.4f rawRmse=%.4f rmsDelta=%.4f\n",
                                taaRmse, rawRmse, shallowRmsDelta);
                    if (!(taaRmse < rawRmse * 0.45f && taaRmse < 0.08f && shallowRmsDelta < 0.012f)) {
                        std::printf("  -> FAIL: Shallow 1:6 slope AA degraded (taaRmse=%.4f rawRmse=%.4f rmsDelta=%.4f)\n",
                                    taaRmse, rawRmse, shallowRmsDelta);
                        allOk = false;
                    }
                }
            }

            // Static Emissive Reactive (reactive = 0.85f) must preserve full TAA convergence on static edges
            {
                constexpr uint32_t kEw = 128;
                constexpr uint32_t kEh = 128;
                if (!gpu.ensureSurfaces(kEw, kEh, kEw, kEh)) {
                    allOk = false;
                } else {
                    D3D11_TEXTURE2D_DESC rtd{};
                    rtd.Width = kEw; rtd.Height = kEh; rtd.MipLevels = 1; rtd.ArraySize = 1;
                    rtd.Format = DXGI_FORMAT_R8_UNORM; rtd.SampleDesc.Count = 1;
                    rtd.Usage = D3D11_USAGE_DEFAULT; rtd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    std::vector<uint8_t> reactBytes(static_cast<size_t>(kEw) * kEh, 217u); // ~0.85f
                    D3D11_SUBRESOURCE_DATA reactInit{ reactBytes.data(), kEw, 0 };
                    ComPtr<ID3D11Texture2D> reactTex;
                    ComPtr<ID3D11ShaderResourceView> reactSrv;
                    if (FAILED(gpu.device->CreateTexture2D(&rtd, &reactInit, &reactTex)) ||
                        FAILED(gpu.device->CreateShaderResourceView(reactTex.Get(), nullptr, &reactSrv))) {
                        allOk = false;
                    } else {
                        constexpr int kFrames = 32;
                        constexpr int kWarmup = 16;
                        const unsigned phaseCount = taaBackend.recommendedJitterPhaseCount(32u, kEw, kEh, kEw, kEh);
                        FrameBuffers fb;
                        edpe::ProjectionJitter prevJ{};
                        std::vector<std::vector<float>> bandX;
                        double errSum = 0.0;
                        size_t errCnt = 0;

                        for (int t = 0; t < kFrames; ++t) {
                            const auto j = edpe::projectionJitter(static_cast<uint64_t>(t), kEw, kEh, phaseCount);
                            fb.resize(kEw, kEh);
                            for (uint32_t iy = 0; iy < kEh; ++iy) {
                                for (uint32_t ix = 0; ix < kEw; ++ix) {
                                    const float ux = static_cast<float>(ix) + 0.5f - j.pixelX;
                                    if (ux >= 64.5f) {
                                        const size_t idx = static_cast<size_t>(iy) * kEw + ix;
                                        fb.color[idx] = packRgba8(0.90f, 0.90f, 0.90f);
                                        fb.depth[idx] = kFgDepth;
                                    }
                                }
                            }
                            if (t == 0) {
                                taaBackend.resetHistory();
                                gpu.hasPrevDepth = false;
                            } else {
                                gpu.currDepthIdx ^= 1;
                            }
                            gpu.context->UpdateSubresource(gpu.colorTex.Get(), 0, nullptr, fb.color.data(), kEw * sizeof(uint32_t), 0);
                            gpu.uploadDepth(fb.depth);
                            gpu.context->UpdateSubresource(gpu.motionTex.Get(), 0, nullptr, fb.motion.data(), kEw * sizeof(uint32_t), 0);
                            auto inputs = gpu.makeInputs(j.pixelX, j.pixelY, t > 0 ? prevJ.pixelX : 0.0f, t > 0 ? prevJ.pixelY : 0.0f, t == 0, 0.0f);
                            inputs.reactiveMask = reactTex.Get();
                            inputs.reactiveMaskSrv = reactSrv.Get();
                            if (!taaBackend.evaluate(gpu.context.Get(), inputs, gpu.outputRtv.Get(), gpu.outputTex.Get())) {
                                allOk = false;
                                break;
                            }
                            gpu.hasPrevDepth = true;
                            prevJ = j;

                            if (t >= kWarmup) {
                                gpu.context->CopyResource(gpu.stagingTex.Get(), gpu.outputTex.Get());
                                D3D11_MAPPED_SUBRESOURCE mapped{};
                                if (SUCCEEDED(gpu.context->Map(gpu.stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                                    std::vector<float> band;
                                    const auto* base = static_cast<const uint8_t*>(mapped.pData);
                                    for (uint32_t y = 24; y < kEh - 24; ++y) {
                                        const auto* row = reinterpret_cast<const uint32_t*>(base + static_cast<size_t>(y) * mapped.RowPitch);
                                        for (uint32_t x = 62; x <= 66; ++x) {
                                            const float l = pixelLuma(row[x]);
                                            const float cov = std::clamp((static_cast<float>(x) + 1.0f) - 64.5f, 0.0f, 1.0f);
                                            const float gt = kBgColor + cov * (0.90f - kBgColor);
                                            errSum += std::fabs(l - gt);
                                            ++errCnt;
                                            band.push_back(l);
                                        }
                                    }
                                    gpu.context->Unmap(gpu.stagingTex.Get(), 0);
                                    bandX.push_back(std::move(band));
                                }
                            }
                        }
                        const float reactErr = errCnt > 0 ? static_cast<float>(errSum / errCnt) : 999.0f;
                        double sqD = 0.0;
                        size_t dCnt = 0;
                        for (size_t f = 1; f < bandX.size(); ++f) {
                            for (size_t k = 0; k < bandX[f].size(); ++k) {
                                const double d = static_cast<double>(bandX[f][k] - bandX[f - 1][k]);
                                sqD += d * d;
                                ++dCnt;
                            }
                        }
                        const float reactRms = dCnt > 0 ? static_cast<float>(std::sqrt(sqD / dCnt)) : 999.0f;
                        std::printf("  [Native_D3D11_TAA] StaticEmissiveReactive(0.85): err=%.4f rms=%.4f\n",
                                    reactErr, reactRms);
                        if (!(reactErr < 0.032f && reactRms < 0.010f)) {
                            std::printf("  -> FAIL: Static emissive reactive killed AA (err=%.4f, rms=%.4f)\n",
                                        reactErr, reactRms);
                            allOk = false;
                        }
                    }
                }
            }

            const auto boxM = measureUnvectoredMovingTexturedBox(gpu, taaBackend);
            std::printf("  [Native_D3D11_TAA] UnvectoredBox(+3px/f, mv=0): maxTrail=%.4f meanTrail=%.4f interiorContrast=%.4f\n",
                        boxM.maxTrailGhost, boxM.meanTrailGhost, boxM.interiorContrast);
            if (!(boxM.maxTrailGhost < 0.05f && boxM.interiorContrast >= 0.45f)) {
                std::printf("  -> FAIL: Unvectored moving textured box failed (maxTrail=%.4f >= 0.05 or contrast=%.4f < 0.45)\n",
                            boxM.maxTrailGhost, boxM.interiorContrast);
                allOk = false;
            }

            const float thinGhost = measureUnvectoredMovingThinLineGhost(gpu, taaBackend);
            std::printf("  [Native_D3D11_TAA] UnvectoredThin1px(+2px/f, mv=0): maxTrail=%.4f\n", thinGhost);
            if (!(thinGhost < 0.06f)) {
                std::printf("  -> FAIL: Unvectored moving 1px line trailed (maxTrail=%.4f >= 0.06)\n", thinGhost);
                allOk = false;
            }

            const float sharp05 = measureVectoredFractionalSharpness(gpu, taaBackend, 0.5f);
            const float sharp15 = measureVectoredFractionalSharpness(gpu, taaBackend, 1.5f);
            std::printf("  [Native_D3D11_TAA] VectoredGrating2px contrast: @0.5px/f=%.4f @1.5px/f=%.4f\n",
                        sharp05, sharp15);
            if (!(sharp05 >= 0.40f && sharp15 >= 0.40f)) {
                std::printf("  -> FAIL: Vectored fractional-speed grating smeared (0.5=%.4f, 1.5=%.4f < 0.40)\n",
                            sharp05, sharp15);
                allOk = false;
            }
        }

        // Verify TaaPass allocates DXGI_FORMAT_R16G16B16A16_FLOAT history buffers even for 8-bit LDR output
        ID3D11Texture2D* taaHist = taaBackend.latestOutputTexture();
        if (!taaHist) {
            std::printf("  -> FAIL: TaaUpscalerBackend latestOutputTexture is null\n");
            allOk = false;
        } else {
            D3D11_TEXTURE2D_DESC histDesc{};
            taaHist->GetDesc(&histDesc);
            if (histDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
                std::printf("  -> FAIL: TaaPass history format %u != DXGI_FORMAT_R16G16B16A16_FLOAT\n",
                            static_cast<unsigned>(histDesc.Format));
                allOk = false;
            }
        }

        // Verify HDR > 1.0 round-trip and R11G11B10_FLOAT safe upper clamp (no NaN/Inf)
        {
            constexpr uint32_t kHdrW = 16;
            constexpr uint32_t kHdrH = 16;
            std::vector<float> hdrPixels(static_cast<size_t>(kHdrW) * kHdrH * 4u, 0.0f);
            for (uint32_t y = 0; y < kHdrH; ++y) {
                for (uint32_t x = 0; x < kHdrW; ++x) {
                    const size_t idx = (static_cast<size_t>(y) * kHdrW + x) * 4u;
                    if (x < kHdrW / 2) {
                        hdrPixels[idx + 0] = 8.0f;
                        hdrPixels[idx + 1] = 2.5f;
                        hdrPixels[idx + 2] = 0.5f;
                        hdrPixels[idx + 3] = 1.0f;
                    } else {
                        hdrPixels[idx + 0] = 100000.0f;
                        hdrPixels[idx + 1] = 100000.0f;
                        hdrPixels[idx + 2] = 100000.0f;
                        hdrPixels[idx + 3] = 1.0f;
                    }
                }
            }
            D3D11_TEXTURE2D_DESC td{};
            td.Width = kHdrW;
            td.Height = kHdrH;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            D3D11_SUBRESOURCE_DATA initData{ hdrPixels.data(), kHdrW * 16u, 0 };
            ComPtr<ID3D11Texture2D> hdrInTex, hdrOutTex, hdrStageTex;
            ComPtr<ID3D11ShaderResourceView> hdrInSrv;
            ComPtr<ID3D11RenderTargetView> hdrOutRtv;
            D3D11_TEXTURE2D_DESC outDesc = td;
            D3D11_TEXTURE2D_DESC stageDesc = td;
            stageDesc.Usage = D3D11_USAGE_STAGING;
            stageDesc.BindFlags = 0;
            stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

            if (SUCCEEDED(gpu.device->CreateTexture2D(&td, &initData, &hdrInTex)) &&
                SUCCEEDED(gpu.device->CreateShaderResourceView(hdrInTex.Get(), nullptr, &hdrInSrv)) &&
                SUCCEEDED(gpu.device->CreateTexture2D(&outDesc, nullptr, &hdrOutTex)) &&
                SUCCEEDED(gpu.device->CreateRenderTargetView(hdrOutTex.Get(), nullptr, &hdrOutRtv)) &&
                SUCCEEDED(gpu.device->CreateTexture2D(&stageDesc, nullptr, &hdrStageTex))) {
                edpe::TaaPass hdrPass;
                if (hdrPass.initialize(gpu.device.Get(), gpu.context.Get())) {
                    const bool ok = hdrPass.render(gpu.context.Get(), hdrInSrv.Get(), nullptr, nullptr,
                        hdrOutRtv.Get(), hdrOutTex.Get(), kHdrW, kHdrH, 0.0f, 0.0f, true);
                    if (!ok || hdrPass.historyFormat() != DXGI_FORMAT_R16G16B16A16_FLOAT) {
                        std::printf("  -> FAIL: TaaPass HDR render or fp16 history format check failed\n");
                        allOk = false;
                    } else {
                        gpu.context->CopyResource(hdrStageTex.Get(), hdrOutTex.Get());
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        if (SUCCEEDED(gpu.context->Map(hdrStageTex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                            const auto* row = reinterpret_cast<const float*>(
                                static_cast<const uint8_t*>(mapped.pData) + (kHdrH / 2) * mapped.RowPitch);
                            const float rLo = row[2 * 4 + 0];
                            const float gLo = row[2 * 4 + 1];
                            const float bLo = row[2 * 4 + 2];
                            const float rHi = row[(kHdrW - 2) * 4 + 0];
                            const float gHi = row[(kHdrW - 2) * 4 + 1];
                            const float bHi = row[(kHdrW - 2) * 4 + 2];
                            gpu.context->Unmap(hdrStageTex.Get(), 0);
                            if (!std::isfinite(rLo) || !std::isfinite(gLo) || !std::isfinite(bLo) ||
                                std::fabs(rLo - 8.0f) > 0.1f || std::fabs(gLo - 2.5f) > 0.1f || std::fabs(bLo - 0.5f) > 0.05f) {
                                std::printf("  -> FAIL: TaaPass HDR > 1.0 round-trip clipped (%.3f, %.3f, %.3f)\n", rLo, gLo, bLo);
                                allOk = false;
                            }
                            if (!std::isfinite(rHi) || !std::isfinite(gHi) || !std::isfinite(bHi) ||
                                rHi > 65024.0f || gHi > 65024.0f || bHi > 64512.0f) {
                                std::printf("  -> FAIL: TaaPass extreme HDR not safely clamped (%.1f, %.1f, %.1f)\n", rHi, gHi, bHi);
                                allOk = false;
                            }
                        }
                    }
                    hdrPass.shutdown();
                }
            }
        }

        // Verify UpscalerManager::evaluate NEVER sharpens TaaPass history in-place when destinationTexture == nullptr
        {
            constexpr uint32_t kSw = 32;
            constexpr uint32_t kSh = 32;
            FrameBuffers fb;
            fb.resize(kSw, kSh);
            for (uint32_t y = 0; y < kSh; ++y) {
                for (uint32_t x = 0; x < kSw; ++x) {
                    const float v = ((x + y) & 1u) ? 0.90f : 0.10f;
                    fb.color[static_cast<size_t>(y) * kSw + x] = packRgba8(v, v, v);
                }
            }
            if (gpu.allocateSurfaces(kSw, kSh, kSw, kSh)) {
                gpu.context->UpdateSubresource(gpu.colorTex.Get(), 0, nullptr, fb.color.data(), kSw * sizeof(uint32_t), 0);
                gpu.uploadDepth(fb.depth);
                gpu.context->UpdateSubresource(gpu.motionTex.Get(), 0, nullptr, fb.motion.data(), kSw * sizeof(uint32_t), 0);
                edpe::UpscalerManager& mgr = edpe::UpscalerManager::instance();
                if (mgr.initialize(gpu.device.Get(), gpu.context.Get()) &&
                    mgr.setBackend(edpe::UpscalerBackend::Taa) &&
                    mgr.setMode(edpe::UpscaleMode::NativeAA)) {
                    edpe::TemporalFrameInputs in = gpu.makeInputs(0.0f, 0.0f, 0.0f, 0.0f, true, 0.0f);

                    auto readBackFp16 = [&](ID3D11Texture2D* tex, std::vector<uint16_t>& out) -> bool {
                        if (!tex) return false;
                        D3D11_TEXTURE2D_DESC d{};
                        tex->GetDesc(&d);
                        d.Usage = D3D11_USAGE_STAGING;
                        d.BindFlags = 0;
                        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        ComPtr<ID3D11Texture2D> stg;
                        if (FAILED(gpu.device->CreateTexture2D(&d, nullptr, &stg))) return false;
                        gpu.context->CopyResource(stg.Get(), tex);
                        D3D11_MAPPED_SUBRESOURCE m{};
                        if (FAILED(gpu.context->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
                        out.resize(static_cast<size_t>(d.Width) * d.Height * 4u);
                        for (uint32_t row = 0; row < d.Height; ++row) {
                            std::memcpy(out.data() + static_cast<size_t>(row) * d.Width * 4u,
                                static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(row) * m.RowPitch,
                                static_cast<size_t>(d.Width) * 8u);
                        }
                        gpu.context->Unmap(stg.Get(), 0);
                        return true;
                    };

                    in.sharpness = 0.0f;
                    std::vector<uint16_t> histNoSharp, histWithSharp;
                    const bool ok0 = mgr.evaluate(gpu.context.Get(), in, nullptr, nullptr, false);
                    const bool rb0 = ok0 && readBackFp16(mgr.getTaaLatestOutputTexture(), histNoSharp);

                    in.sharpness = 0.85f;
                    const bool ok1 = mgr.evaluate(gpu.context.Get(), in, nullptr, nullptr, false);
                    const bool rb1 = ok1 && readBackFp16(mgr.getTaaLatestOutputTexture(), histWithSharp);

                    if (!rb0 || !rb1 || histNoSharp != histWithSharp) {
                        std::printf("  -> FAIL: UpscalerManager::evaluate modified TAA history in-place when destinationTexture == nullptr\n");
                        allOk = false;
                    }

                    // Verify [TAA] RcasSharpness == 0.0f ignores Upscaler::Sharpness (0.85f),
                    // while RcasSharpness > 0 writes to display texture without mutating historyTexture_.
                    edpe::TaaSettings ts{};
                    ts.rcasSharpness = 0.0f;
                    ts.debugView = 0;
                    mgr.setTaaSettings(ts);
                    std::vector<uint16_t> dispNoRcas, histAfterRcas, dispWithRcas;
                    const bool ok2 = mgr.evaluate(gpu.context.Get(), in, nullptr, nullptr, false);
                    const bool rb2 = ok2 && readBackFp16(mgr.getTaaLatestDisplayTexture(), dispNoRcas);
                    if (!rb2 || mgr.getTaaLatestDisplayTexture() != mgr.getTaaLatestOutputTexture()) {
                        std::printf("  -> FAIL: RcasSharpness=0.0f should expose untouched history directly\n");
                        allOk = false;
                    }

                    ts.rcasSharpness = 0.60f;
                    mgr.setTaaSettings(ts);
                    in.sharpness = 0.0f;
                    const bool ok3 = mgr.evaluate(gpu.context.Get(), in, nullptr, nullptr, false);
                    const bool rb3a = ok3 && readBackFp16(mgr.getTaaLatestOutputTexture(), histAfterRcas);
                    const bool rb3b = ok3 && readBackFp16(mgr.getTaaLatestDisplayTexture(), dispWithRcas);
                    if (!rb3a || !rb3b || histAfterRcas != histNoSharp || dispWithRcas == histNoSharp) {
                        std::printf("  -> FAIL: PostRCAS either mutated historyTexture_ or failed to sharpen displayTexture\n");
                        allOk = false;
                    }

                    // Verify inline ps_taa_blit PostRCAS (destinationTexture != nullptr) matches CS cs_taa_post_rcas
                    {
                        D3D11_TEXTURE2D_DESC itd{};
                        itd.Width = kSw; itd.Height = kSh; itd.MipLevels = 1; itd.ArraySize = 1;
                        itd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; itd.SampleDesc.Count = 1;
                        itd.Usage = D3D11_USAGE_DEFAULT;
                        itd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                        ComPtr<ID3D11Texture2D> inlineTex;
                        ComPtr<ID3D11RenderTargetView> inlineRtv;
                        std::vector<uint16_t> inlineWithRcas, inlineZeroRcas;
                        if (FAILED(gpu.device->CreateTexture2D(&itd, nullptr, &inlineTex)) ||
                            FAILED(gpu.device->CreateRenderTargetView(inlineTex.Get(), nullptr, &inlineRtv)) ||
                            !mgr.evaluate(gpu.context.Get(), in, inlineRtv.Get(), inlineTex.Get(), false) ||
                            !readBackFp16(inlineTex.Get(), inlineWithRcas)) {
                            std::printf("  -> FAIL: Inline PostRCAS evaluate/readback failed\n");
                            allOk = false;
                        } else {
                            float maxDiff = 0.0f;
                            for (size_t i = 0; i < inlineWithRcas.size(); ++i) {
                                const float a = DirectX::PackedVector::XMConvertHalfToFloat(inlineWithRcas[i]);
                                const float b = DirectX::PackedVector::XMConvertHalfToFloat(dispWithRcas[i]);
                                maxDiff = std::max(maxDiff, std::fabs(a - b));
                            }
                            if (maxDiff > 0.002f) {
                                std::printf("  -> FAIL: Inline PostRCAS mismatch vs CS PostRCAS (maxDiff=%.5f > 0.002)\n", maxDiff);
                                allOk = false;
                            }
                            ts.rcasSharpness = 0.0f;
                            mgr.setTaaSettings(ts);
                            if (!mgr.evaluate(gpu.context.Get(), in, inlineRtv.Get(), inlineTex.Get(), false) ||
                                !readBackFp16(inlineTex.Get(), inlineZeroRcas)) {
                                allOk = false;
                            } else {
                                float maxZeroDiff = 0.0f;
                                for (size_t i = 0; i < inlineZeroRcas.size(); ++i) {
                                    const float a = DirectX::PackedVector::XMConvertHalfToFloat(inlineZeroRcas[i]);
                                    const float b = DirectX::PackedVector::XMConvertHalfToFloat(dispNoRcas[i]);
                                    maxZeroDiff = std::max(maxZeroDiff, std::fabs(a - b));
                                }
                                if (maxZeroDiff > 0.002f) {
                                    std::printf("  -> FAIL: Inline blit with RcasSharpness=0.0f modified output (maxDiff=%.5f)\n", maxZeroDiff);
                                    allOk = false;
                                }
                            }
                        }
                    }

                    // Verify DebugView 1..4 run cleanly without mutating historyTexture_
                    for (int dv = 1; dv <= 4; ++dv) {
                        ts.debugView = dv;
                        mgr.setTaaSettings(ts);
                        std::vector<uint16_t> histDv, dispDv;
                        const bool okDv = mgr.evaluate(gpu.context.Get(), in, nullptr, nullptr, false);
                        if (!okDv ||
                            !readBackFp16(mgr.getTaaLatestOutputTexture(), histDv) ||
                            !readBackFp16(mgr.getTaaLatestDisplayTexture(), dispDv) ||
                            histDv != histNoSharp) {
                            std::printf("  -> FAIL: TAA DebugView=%d failed or mutated history\n", dv);
                            allOk = false;
                        }
                    }
                    mgr.setTaaSettings(edpe::TaaSettings{});
                    mgr.shutdown();
                }
            }
        }
        taaBackend.shutdown();
    }

    // Verify all TAAv2 HLSL shaders compile cleanly with -O1 (Wine/Proton d3dcompiler_47 compatibility)
    {
        struct ShaderCase {
            const char* src;
            size_t len;
            const char* entry;
            const char* profile;
        };
        const ShaderCase cases[] = {
            { edpe::kTaaPrepassShader,  sizeof(edpe::kTaaPrepassShader) - 1,  "cs_taa_prepass",   "cs_5_0" },
            { edpe::kTaaPatchDivShader, sizeof(edpe::kTaaPatchDivShader) - 1, "cs_taa_patch_div", "cs_5_0" },
            { edpe::kTaaResolveShader,  sizeof(edpe::kTaaResolveShader) - 1,  "cs_taa_resolve",   "cs_5_0" },
            { edpe::kTaaPostRcasShader, sizeof(edpe::kTaaPostRcasShader) - 1, "cs_taa_post_rcas", "cs_5_0" },
            { edpe::kTaaBlitShader,     sizeof(edpe::kTaaBlitShader) - 1,     "cs_taa_debug",     "cs_5_0" },
            { edpe::kTaaBlitShader,     sizeof(edpe::kTaaBlitShader) - 1,     "vs_taa_blit",      "vs_5_0" },
            { edpe::kTaaBlitShader,     sizeof(edpe::kTaaBlitShader) - 1,     "ps_taa_blit",      "ps_5_0" },
        };
        for (const auto& sc : cases) {
            ComPtr<ID3DBlob> code, err;
            const HRESULT hr = D3DCompile(sc.src, sc.len, nullptr, nullptr, nullptr,
                sc.entry, sc.profile, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
            if (FAILED(hr) || !code) {
                std::printf("  -> FAIL: TAAv2 D3DCompile(%s/%s, -O1) failed hr=0x%08X: %s\n",
                    sc.entry, sc.profile, static_cast<unsigned>(hr),
                    (err && err->GetBufferPointer()) ? static_cast<const char*>(err->GetBufferPointer()) : "");
                allOk = false;
            }
        }
    }

    return allOk ? 0 : 10;
}
