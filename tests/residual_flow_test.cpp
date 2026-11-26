#include "residual_flow_pass.h"
#include "residual_flow_shader.h"
#include "taa_pass.h"
#include "temporal_math.h"

#include <DirectXPackedVector.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

std::uint32_t packRgba8(float r, float g, float b, float a = 1.0f) {
    auto clampU8 = [](float v) -> std::uint32_t {
        const float c = std::clamp(v, 0.0f, 1.0f);
        return static_cast<std::uint32_t>(std::lround(c * 255.0f));
    };
    return clampU8(r) | (clampU8(g) << 8) | (clampU8(b) << 16) | (clampU8(a) << 24);
}

std::uint32_t packHalf2(float x, float y) {
    const std::uint16_t hx = DirectX::PackedVector::XMConvertFloatToHalf(x);
    const std::uint16_t hy = DirectX::PackedVector::XMConvertFloatToHalf(y);
    return static_cast<std::uint32_t>(hx) | (static_cast<std::uint32_t>(hy) << 16);
}

float pixelLuma(std::uint32_t rgba) {
    const float r = static_cast<float>(rgba & 0xFFu) / 255.0f;
    const float g = static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f;
    const float b = static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f;
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

bool testCompileO1(const char* src, size_t len, const char* entry) {
    ComPtr<ID3DBlob> code, err;
    const HRESULT hr = D3DCompile(src, len, nullptr, nullptr, nullptr,
        entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &err);
    if (FAILED(hr) || !code) {
        std::printf("D3DCompile -O1 failed for %s: hr=0x%08X %s\n",
            entry, static_cast<unsigned>(hr),
            err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        return false;
    }
    return true;
}

struct GpuFixture {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    ComPtr<ID3D11Texture2D> colorTex;
    ComPtr<ID3D11ShaderResourceView> colorSrv;
    ComPtr<ID3D11Texture2D> depthTex;
    ComPtr<ID3D11ShaderResourceView> depthSrv;
    ComPtr<ID3D11Texture2D> prevDepthTex;
    ComPtr<ID3D11ShaderResourceView> prevDepthSrv;
    ComPtr<ID3D11Texture2D> mvTex;
    ComPtr<ID3D11ShaderResourceView> mvSrv;
    ComPtr<ID3D11Texture2D> clsTex;
    ComPtr<ID3D11ShaderResourceView> clsSrv;

    ComPtr<ID3D11Texture2D> mvReadback;
    ComPtr<ID3D11Texture2D> colorFloatReadback;

    uint32_t w = 0;
    uint32_t h = 0;

    bool init(uint32_t width, uint32_t height) {
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
        }
        if (FAILED(hr) || !device || !context) return false;

        w = width;
        h = height;

        auto makeTex = [&](DXGI_FORMAT fmt, D3D11_USAGE usage, UINT bind, UINT cpu,
                           ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11ShaderResourceView>* srv) -> bool {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w;
            td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = fmt;
            td.SampleDesc.Count = 1;
            td.Usage = usage;
            td.BindFlags = bind;
            td.CPUAccessFlags = cpu;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &tex))) return false;
            if (srv && FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, srv->GetAddressOf()))) return false;
            return true;
        };

        return makeTex(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, colorTex, &colorSrv) &&
               makeTex(DXGI_FORMAT_R32_FLOAT, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, depthTex, &depthSrv) &&
               makeTex(DXGI_FORMAT_R32_FLOAT, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, prevDepthTex, &prevDepthSrv) &&
               makeTex(DXGI_FORMAT_R16G16_FLOAT, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, mvTex, &mvSrv) &&
               makeTex(DXGI_FORMAT_R8_UINT, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, clsTex, &clsSrv) &&
               makeTex(DXGI_FORMAT_R16G16_FLOAT, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, mvReadback, nullptr) &&
               makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, colorFloatReadback, nullptr);
    }

    void upload(const std::vector<uint32_t>& color,
                const std::vector<float>& depth,
                const std::vector<float>& prevDepth,
                const std::vector<uint32_t>& mv,
                const std::vector<uint8_t>& cls) {
        context->UpdateSubresource(colorTex.Get(), 0, nullptr, color.data(), w * 4u, 0);
        context->UpdateSubresource(depthTex.Get(), 0, nullptr, depth.data(), w * 4u, 0);
        context->UpdateSubresource(prevDepthTex.Get(), 0, nullptr, prevDepth.data(), w * 4u, 0);
        context->UpdateSubresource(mvTex.Get(), 0, nullptr, mv.data(), w * 4u, 0);
        context->UpdateSubresource(clsTex.Get(), 0, nullptr, cls.data(), w, 0);
    }

    std::vector<std::pair<float, float>> readMotion(ID3D11Texture2D* tex) {
        context->CopyResource(mvReadback.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        std::vector<std::pair<float, float>> out(static_cast<size_t>(w) * h);
        if (SUCCEEDED(context->Map(mvReadback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            for (uint32_t y = 0; y < h; ++y) {
                const auto* row = reinterpret_cast<const uint16_t*>(
                    static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
                for (uint32_t x = 0; x < w; ++x) {
                    out[static_cast<size_t>(y) * w + x] = {
                        DirectX::PackedVector::XMConvertHalfToFloat(row[x * 2 + 0]),
                        DirectX::PackedVector::XMConvertHalfToFloat(row[x * 2 + 1])
                    };
                }
            }
            context->Unmap(mvReadback.Get(), 0);
        }
        return out;
    }

    std::vector<float> readColorLuma(ID3D11Texture2D* tex) {
        context->CopyResource(colorFloatReadback.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        std::vector<float> out(static_cast<size_t>(w) * h, 0.0f);
        if (SUCCEEDED(context->Map(colorFloatReadback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            for (uint32_t y = 0; y < h; ++y) {
                const auto* row = reinterpret_cast<const uint16_t*>(
                    static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
                for (uint32_t x = 0; x < w; ++x) {
                    const float r = DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4 + 0]);
                    const float g = DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4 + 1]);
                    const float b = DirectX::PackedVector::XMConvertHalfToFloat(row[x * 4 + 2]);
                    out[static_cast<size_t>(y) * w + x] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                }
            }
            context->Unmap(colorFloatReadback.Get(), 0);
        }
        return out;
    }
};

void drawTexturedBox(std::vector<uint32_t>& color,
                     std::vector<float>& depth,
                     uint32_t w, uint32_t h,
                     float cx, float cy, float halfSize,
                     float fgDepth, float bgDepth,
                     float jx, float jy,
                     uint8_t clsVal = 1u,
                     std::vector<uint8_t>* cls = nullptr) {
    std::fill(color.begin(), color.end(), packRgba8(0.02f, 0.02f, 0.02f));
    std::fill(depth.begin(), depth.end(), bgDepth);
    if (cls) std::fill(cls->begin(), cls->end(), 1u);

    for (uint32_t y = 0; y < h; ++y) {
        const float uy = static_cast<float>(y) + 0.5f - jy;
        const float dy = uy - cy;
        if (std::fabs(dy) > halfSize) continue;
        for (uint32_t x = 0; x < w; ++x) {
            const float ux = static_cast<float>(x) + 0.5f - jx;
            const float dx = ux - cx;
            if (std::fabs(dx) > halfSize) continue;
            // Realistic multi-scale object surface (hull shading gradient + 8px panels + corner decal)
            const float u = std::clamp((dx + halfSize) / (2.0f * halfSize), 0.0f, 1.0f);
            const float v = std::clamp((dy + halfSize) / (2.0f * halfSize), 0.0f, 1.0f);
            const int panel8 = ((static_cast<int>(std::floor(dx * 0.125f)) +
                                 static_cast<int>(std::floor(dy * 0.125f))) & 1);
            const int seam = (std::fabs(dx - 0.35f * dy) < 1.8f) ? 1 : 0;
            const float lum = std::clamp(0.18f + 0.42f * u + 0.18f * v +
                                         (panel8 ? 0.22f : 0.0f) - (seam ? 0.16f : 0.0f),
                                         0.12f, 0.98f);
            const size_t idx = static_cast<size_t>(y) * w + x;
            color[idx] = packRgba8(lum, lum * 0.9f, lum * 0.8f);
            depth[idx] = fgDepth;
            if (cls) (*cls)[idx] = clsVal;
        }
    }
}

} // namespace

int main() {
    // 1. Wine/Proton -O1 HLSL shader compilation check for all 5 entry points
    if (!testCompileO1(edpe::kRFlowPyr0Shader, sizeof(edpe::kRFlowPyr0Shader) - 1, "cs_rflow_pyr0") ||
        !testCompileO1(edpe::kRFlowPyr1Shader, sizeof(edpe::kRFlowPyr1Shader) - 1, "cs_rflow_pyr1") ||
        !testCompileO1(edpe::kRFlowCoarseShader, sizeof(edpe::kRFlowCoarseShader) - 1, "cs_rflow_coarse") ||
        !testCompileO1(edpe::kRFlowRefineShader, sizeof(edpe::kRFlowRefineShader) - 1, "cs_rflow_refine") ||
        !testCompileO1(edpe::kRFlowApplyShader, sizeof(edpe::kRFlowApplyShader) - 1, "cs_rflow_apply")) {
        return 1;
    }

    constexpr uint32_t kW = 128;
    constexpr uint32_t kH = 128;
    GpuFixture gpu;
    if (!gpu.init(kW, kH)) {
        std::printf("GpuFixture init failed\n");
        return 2;
    }

    edpe::ResidualFlowPass rflow;
    if (!rflow.initialize(gpu.device.Get())) {
        std::printf("ResidualFlowPass init failed\n");
        return 3;
    }

    std::vector<uint32_t> color(kW * kH);
    std::vector<float> depth(kW * kH);
    std::vector<float> prevDepth(kW * kH, 0.01f);
    std::vector<uint32_t> baseMv(kW * kH, packHalf2(0.0f, 0.0f));
    std::vector<uint8_t> cls(kW * kH, 1u);

    // Test 2: Moving textured object (+4 px/f X, +2 px/f Y, baseMv = 0)
    // Expect recovered MV near (-4.0, -2.0) inside box, and < 0.05 px on background!
    drawTexturedBox(color, depth, kW, kH, 52.0f, 52.0f, 18.0f, 0.20f, 0.01f, 0.0f, 0.0f);
    prevDepth = depth;
    gpu.upload(color, depth, prevDepth, baseMv, cls);
    if (!rflow.render(gpu.context.Get(), gpu.colorSrv.Get(), gpu.mvSrv.Get(),
            gpu.depthSrv.Get(), gpu.clsSrv.Get(), kW, kH, 0.0f, 0.0f, 0.0f, 0.0f, true)) {
        return 4;
    }

    drawTexturedBox(color, depth, kW, kH, 56.0f, 54.0f, 18.0f, 0.20f, 0.01f, 0.0f, 0.0f);
    gpu.upload(color, depth, prevDepth, baseMv, cls);
    if (!rflow.render(gpu.context.Get(), gpu.colorSrv.Get(), gpu.mvSrv.Get(),
            gpu.depthSrv.Get(), gpu.clsSrv.Get(), kW, kH, 0.0f, 0.0f, 0.0f, 0.0f, false)) {
        return 5;
    }
    {
        auto outMv = gpu.readMotion(rflow.outputTexture());
        double sumX = 0.0, sumY = 0.0;
        int interiorCount = 0;
        float bgMaxMag = 0.0f;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const auto [mx, my] = outMv[static_cast<size_t>(y) * kW + x];
                if (x >= 44 && x <= 68 && y >= 42 && y <= 66) {
                    sumX += mx;
                    sumY += my;
                    ++interiorCount;
                } else if (depth[static_cast<size_t>(y) * kW + x] < 0.05f) {
                    bgMaxMag = std::max(bgMaxMag, std::hypot(mx, my));
                }
            }
        }
        const float meanX = static_cast<float>(sumX / interiorCount);
        const float meanY = static_cast<float>(sumY / interiorCount);
        const float err = std::hypot(meanX - (-4.0f), meanY - (-2.0f));
        std::printf("[ResidualFlow] MovingBox(+4,+2 mv=0): mean=(%.3f, %.3f) err=%.3f bgMax=%.4f\n",
            meanX, meanY, err, bgMaxMag);
        if (err > 0.75f || bgMaxMag > 0.05f) return 6;
    }

    // Test 3: Moving object on top of camera pan: baseMv=(+3.0,-1.0) + residual(-6.0,+2.0) = net(-3.0,+1.0)
    // Screen center moves by (+3.0, -1.0) frame-to-frame so MV = (-3.0, +1.0)
    drawTexturedBox(color, depth, kW, kH, 60.0f, 60.0f, 18.0f, 0.20f, 0.01f, 0.0f, 0.0f);
    prevDepth = depth;
    std::fill(baseMv.begin(), baseMv.end(), packHalf2(3.0f, -1.0f));
    gpu.upload(color, depth, prevDepth, baseMv, cls);
    rflow.render(gpu.context.Get(), gpu.colorSrv.Get(), gpu.mvSrv.Get(),
        gpu.depthSrv.Get(), gpu.clsSrv.Get(), kW, kH, 0.0f, 0.0f, 0.0f, 0.0f, true);

    drawTexturedBox(color, depth, kW, kH, 63.0f, 59.0f, 18.0f, 0.20f, 0.01f, 0.0f, 0.0f);
    gpu.upload(color, depth, prevDepth, baseMv, cls);
    rflow.render(gpu.context.Get(), gpu.colorSrv.Get(), gpu.mvSrv.Get(),
        gpu.depthSrv.Get(), gpu.clsSrv.Get(), kW, kH, 0.0f, 0.0f, 0.0f, 0.0f, false);
    {
        auto outMv = gpu.readMotion(rflow.outputTexture());
        double sumX = 0.0, sumY = 0.0;
        int interiorCount = 0;
        for (uint32_t y = 48; y <= 70; ++y) {
            for (uint32_t x = 52; x <= 74; ++x) {
                const auto [mx, my] = outMv[static_cast<size_t>(y) * kW + x];
                sumX += mx;
                sumY += my;
                ++interiorCount;
            }
        }
        const float meanX = static_cast<float>(sumX / interiorCount);
        const float meanY = static_cast<float>(sumY / interiorCount);
        const float err = std::hypot(meanX - (-3.0f), meanY - 1.0f);
        std::printf("[ResidualFlow] MovingOverPan(base=+3,-1 net=-3,+1): mean=(%.3f, %.3f) err=%.3f\n",
            meanX, meanY, err);
        if (err > 0.85f) return 7;
    }

    // Test 4: Disappearing blob -> residual MV must stay < 0.05 px on flat empty background
    std::fill(baseMv.begin(), baseMv.end(), packHalf2(0.0f, 0.0f));
    drawTexturedBox(color, depth, kW, kH, 64.0f, 64.0f, 16.0f, 0.20f, 0.01f, 0.0f, 0.0f);
    prevDepth = depth;
    gpu.upload(color, depth, prevDepth, baseMv, cls);
    rflow.render(gpu.context.Get(), gpu.colorSrv.Get(), gpu.mvSrv.Get(),
        gpu.depthSrv.Get(), gpu.clsSrv.Get(), kW, kH, 0.0f, 0.0f, 0.0f, 0.0f, true);

    std::fill(color.begin(), color.end(), packRgba8(0.02f, 0.02f, 0.02f));
    std::fill(depth.begin(), depth.end(), 0.01f);
    gpu.upload(color, depth, prevDepth, baseMv, cls);
    rflow.render(gpu.context.Get(), gpu.colorSrv.Get(), gpu.mvSrv.Get(),
        gpu.depthSrv.Get(), gpu.clsSrv.Get(), kW, kH, 0.0f, 0.0f, 0.0f, 0.0f, false);
    {
        auto outMv = gpu.readMotion(rflow.outputTexture());
        float maxMag = 0.0f;
        for (const auto& [mx, my] : outMv) maxMag = std::max(maxMag, std::hypot(mx, my));
        std::printf("[ResidualFlow] DisappearingBlob: maxMag=%.4f\n", maxMag);
        if (maxMag > 0.05f) return 8;
    }

    // Test 5: Static 1px thin line under 16-phase Halton jitter -> zero false residual MV
    float thinMaxMag = 0.0f;
    edpe::ProjectionJitter prevJ{};
    for (int t = 0; t < 16; ++t) {
        const auto j = edpe::projectionJitter(static_cast<uint64_t>(t), kW, kH, 16u);
        std::fill(color.begin(), color.end(), packRgba8(0.02f, 0.02f, 0.02f));
        std::fill(depth.begin(), depth.end(), 0.01f);
        for (uint32_t y = 16; y < 112; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const float ux = static_cast<float>(x) + 0.5f - j.pixelX;
                if (std::fabs(ux - 64.25f) <= 0.50f) {
                    const size_t idx = static_cast<size_t>(y) * kW + x;
                    color[idx] = packRgba8(1.0f, 1.0f, 1.0f);
                    depth[idx] = 0.20f;
                }
            }
        }
        if (t == 0) prevDepth = depth;
        gpu.upload(color, depth, prevDepth, baseMv, cls);
        rflow.render(gpu.context.Get(), gpu.colorSrv.Get(), gpu.mvSrv.Get(),
            gpu.depthSrv.Get(), gpu.clsSrv.Get(), kW, kH,
            j.pixelX, j.pixelY, prevJ.pixelX, prevJ.pixelY, t == 0);
        prevDepth = depth;
        prevJ = j;
        if (t >= 4) {
            auto outMv = gpu.readMotion(rflow.outputTexture());
            for (uint32_t yy = 0; yy < kH; ++yy) {
                for (uint32_t xx = 0; xx < kW; ++xx) {
                    const auto [mx, my] = outMv[static_cast<size_t>(yy) * kW + xx];
                    const float mag = std::hypot(mx, my);
                    if (mag > thinMaxMag) {
                        thinMaxMag = mag;
                        if (mag > 0.05f) {
                            std::printf("  [Static1pxHalton outlier t=%d (%u,%u) mv=(%.3f,%.3f)]\n",
                                t, xx, yy, mx, my);
                        }
                    }
                }
            }
        }
    }
    std::printf("[ResidualFlow] Static1pxHalton: maxMag=%.4f\n", thinMaxMag);
    if (thinMaxMag > 0.05f) return 9;

    // Test 6: kClassCockpitStatic (2) & kClassSky (0) protection -> pass-through baseMv
    drawTexturedBox(color, depth, kW, kH, 52.0f, 52.0f, 18.0f, 0.20f, 0.0f, 0.0f, 0.0f, 2u, &cls);
    prevDepth = depth;
    gpu.upload(color, depth, prevDepth, baseMv, cls);
    rflow.render(gpu.context.Get(), gpu.colorSrv.Get(), gpu.mvSrv.Get(),
        gpu.depthSrv.Get(), gpu.clsSrv.Get(), kW, kH, 0.0f, 0.0f, 0.0f, 0.0f, true);

    drawTexturedBox(color, depth, kW, kH, 56.0f, 54.0f, 18.0f, 0.20f, 0.0f, 0.0f, 0.0f, 2u, &cls);
    gpu.upload(color, depth, prevDepth, baseMv, cls);
    rflow.render(gpu.context.Get(), gpu.colorSrv.Get(), gpu.mvSrv.Get(),
        gpu.depthSrv.Get(), gpu.clsSrv.Get(), kW, kH, 0.0f, 0.0f, 0.0f, 0.0f, false);
    {
        auto outMv = gpu.readMotion(rflow.outputTexture());
        float protMax = 0.0f;
        for (const auto& [mx, my] : outMv) protMax = std::max(protMax, std::hypot(mx, my));
        std::printf("[ResidualFlow] CockpitAndSkyProtect: maxMag=%.4f\n", protMax);
        if (protMax > 0.01f) return 10;
    }

    // Test 7: End-to-end ResidualFlowPass -> TaaPass on +4 px/f moving textured box (baseMv = 0)
    edpe::TaaPass taa;
    if (!taa.initialize(gpu.device.Get(), gpu.context.Get())) return 11;

    float maxTrail = 0.0f;
    float interiorContrast = 0.0f;
    prevJ = {};
    for (int t = 0; t < 20; ++t) {
        const auto j = edpe::projectionJitter(static_cast<uint64_t>(t), kW, kH, 16u);
        const float cx = 24.0f + 4.0f * static_cast<float>(t);
        const float cy = 64.0f;
        drawTexturedBox(color, depth, kW, kH, cx, cy, 16.0f, 0.20f, 0.01f, j.pixelX, j.pixelY, 1u, &cls);
        if (t == 0) prevDepth = depth;
        gpu.upload(color, depth, prevDepth, baseMv, cls);

        if (!rflow.render(gpu.context.Get(), gpu.colorSrv.Get(), gpu.mvSrv.Get(),
                gpu.depthSrv.Get(), gpu.clsSrv.Get(), kW, kH,
                j.pixelX, j.pixelY, prevJ.pixelX, prevJ.pixelY, t == 0)) {
            return 12;
        }
        if (!taa.render(gpu.context.Get(), gpu.colorSrv.Get(), rflow.output(),
                gpu.depthSrv.Get(), nullptr, nullptr,
                kW, kH,
                j.pixelX, j.pixelY,
                t == 0,
                kW, kH,
                prevJ.pixelX, prevJ.pixelY,
                0.025f,
                gpu.prevDepthSrv.Get(),
                DXGI_FORMAT_UNKNOWN,
                nullptr,
                gpu.clsSrv.Get(),
                nullptr,
                false)) {
            return 13;
        }
        prevDepth = depth;
        prevJ = j;

        if (t == 19) {
            auto outLuma = gpu.readColorLuma(taa.latestOutputTexture());
            // Trailing wake: 6..26 px behind left edge (left edge = cx - 16 = 84)
            for (uint32_t y = 52; y <= 76; ++y) {
                for (uint32_t x = 58; x <= 78; ++x) {
                    const float l = outLuma[static_cast<size_t>(y) * kW + x];
                    maxTrail = std::max(maxTrail, std::fabs(l - 0.02f));
                }
            }
            float mn = 1.0f, mx = 0.0f;
            for (uint32_t y = 54; y <= 74; ++y) {
                for (uint32_t x = 90; x <= 110; ++x) {
                    const float l = outLuma[static_cast<size_t>(y) * kW + x];
                    mn = std::min(mn, l);
                    mx = std::max(mx, l);
                }
            }
            interiorContrast = mx - mn;
        }
    }
    std::printf("[ResidualFlow->TAA] MovingBox(+4px/f): maxTrail=%.4f interiorContrast=%.4f\n",
        maxTrail, interiorContrast);
    if (maxTrail > 0.015f || interiorContrast < 0.50f) return 14;

    return 0;
}
