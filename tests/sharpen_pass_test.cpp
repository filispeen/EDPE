#include "sharpen_pass.h"

#include <d3d11.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

uint32_t packRgba8(float r, float g, float b, float a = 1.0f) {
    auto clampU8 = [](float v) -> uint32_t {
        return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    };
    return clampU8(r) | (clampU8(g) << 8) | (clampU8(b) << 16) | (clampU8(a) << 24);
}

bool createDevice(ComPtr<ID3D11Device>& dev, ComPtr<ID3D11DeviceContext>& ctx) {
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    }
    return SUCCEEDED(hr) && dev && ctx;
}

bool testLdrParity() {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (!createDevice(dev, ctx)) {
        std::printf("  -> FAIL: D3D11CreateDevice failed\n");
        return false;
    }

    edpe::SharpenPass sharpen;
    if (!sharpen.initialize(dev.Get(), ctx.Get())) {
        std::printf("  -> FAIL: SharpenPass::initialize failed\n");
        return false;
    }

    constexpr uint32_t kW = 128;
    constexpr uint32_t kH = 128;
    std::vector<uint32_t> pixels(static_cast<size_t>(kW) * kH);
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const float edge = ((x + y / 2u) % 16u < 8u) ? 0.78f : 0.22f;
            const float grad = 0.08f * std::sin(static_cast<float>(x) * 0.35f) * std::cos(static_cast<float>(y) * 0.25f);
            pixels[static_cast<size_t>(y) * kW + x] = packRgba8(
                edge + grad, edge * 0.85f - grad, 0.5f * edge + 0.2f, 0.90f);
        }
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW;
    td.Height = kH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    D3D11_SUBRESOURCE_DATA initData{ pixels.data(), kW * 4u, 0 };
    ComPtr<ID3D11Texture2D> srcTex, applyTex, directTex, staging;
    if (FAILED(dev->CreateTexture2D(&td, &initData, &srcTex)) ||
        FAILED(dev->CreateTexture2D(&td, &initData, &applyTex)) ||
        FAILED(dev->CreateTexture2D(&td, nullptr, &directTex))) {
        std::printf("  -> FAIL: CreateTexture2D failed\n");
        return false;
    }

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) {
        std::printf("  -> FAIL: staging CreateTexture2D failed\n");
        return false;
    }

    ComPtr<ID3D11ShaderResourceView> srcSrv;
    ComPtr<ID3D11RenderTargetView> directRtv;
    if (FAILED(dev->CreateShaderResourceView(srcTex.Get(), nullptr, &srcSrv)) ||
        FAILED(dev->CreateRenderTargetView(directTex.Get(), nullptr, &directRtv))) {
        std::printf("  -> FAIL: CreateSRV/CreateRTV failed\n");
        return false;
    }

    if (!sharpen.apply(ctx.Get(), applyTex.Get(), 0.50f, false)) {
        std::printf("  -> FAIL: SharpenPass::apply failed\n");
        return false;
    }
    if (!sharpen.applyToRtv(ctx.Get(), srcSrv.Get(), directRtv.Get(), kW, kH, 0.50f, false, false)) {
        std::printf("  -> FAIL: SharpenPass::applyToRtv failed\n");
        return false;
    }

    auto readback = [&](ID3D11Texture2D* tex, std::vector<uint32_t>& out) -> bool {
        ctx->CopyResource(staging.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        out.resize(static_cast<size_t>(kW) * kH);
        const auto* src = static_cast<const uint8_t*>(m.pData);
        for (uint32_t y = 0; y < kH; ++y) {
            std::memcpy(out.data() + static_cast<size_t>(y) * kW,
                        src + static_cast<size_t>(y) * m.RowPitch, kW * 4u);
        }
        ctx->Unmap(staging.Get(), 0);
        return true;
    };

    std::vector<uint32_t> applyOut, directOut;
    if (!readback(applyTex.Get(), applyOut) || !readback(directTex.Get(), directOut)) {
        std::printf("  -> FAIL: readback failed\n");
        return false;
    }

    int maxDiff = 0;
    int changedFromInput = 0;
    for (size_t i = 0; i < applyOut.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            const int a = static_cast<int>((applyOut[i] >> (8 * c)) & 0xFFu);
            const int b = static_cast<int>((directOut[i] >> (8 * c)) & 0xFFu);
            const int orig = static_cast<int>((pixels[i] >> (8 * c)) & 0xFFu);
            maxDiff = std::max(maxDiff, std::abs(a - b));
            if (c < 3 && std::abs(b - orig) > 2) {
                ++changedFromInput;
            }
        }
    }

    std::printf("  LDR applyToRtv vs apply: maxChannelDiff=%d, sharpenedSamples=%d\n",
                maxDiff, changedFromInput);
    sharpen.shutdown();
    return maxDiff == 0 && changedFromInput > 100;
}

bool testHdrParity() {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (!createDevice(dev, ctx)) return false;

    edpe::SharpenPass sharpen;
    if (!sharpen.initialize(dev.Get(), ctx.Get())) return false;

    constexpr uint32_t kW = 64;
    constexpr uint32_t kH = 64;
    std::vector<uint16_t> pixels(static_cast<size_t>(kW) * kH * 4u);
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const float val = ((x ^ y) & 4u) ? 2.4f : 0.3f;
            const uint16_t hv = DirectX::PackedVector::XMConvertFloatToHalf(val);
            const uint16_t ha = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
            const size_t base = (static_cast<size_t>(y) * kW + x) * 4u;
            pixels[base + 0] = hv;
            pixels[base + 1] = hv;
            pixels[base + 2] = hv;
            pixels[base + 3] = ha;
        }
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW;
    td.Height = kH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    D3D11_SUBRESOURCE_DATA initData{ pixels.data(), kW * 8u, 0 };
    ComPtr<ID3D11Texture2D> srcTex, applyTex, directTex, staging;
    if (FAILED(dev->CreateTexture2D(&td, &initData, &srcTex)) ||
        FAILED(dev->CreateTexture2D(&td, &initData, &applyTex)) ||
        FAILED(dev->CreateTexture2D(&td, nullptr, &directTex))) {
        return false;
    }

    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &staging))) return false;

    ComPtr<ID3D11ShaderResourceView> srcSrv;
    ComPtr<ID3D11RenderTargetView> directRtv;
    if (FAILED(dev->CreateShaderResourceView(srcTex.Get(), nullptr, &srcSrv)) ||
        FAILED(dev->CreateRenderTargetView(directTex.Get(), nullptr, &directRtv))) {
        return false;
    }

    if (!sharpen.apply(ctx.Get(), applyTex.Get(), 0.50f, true) ||
        !sharpen.applyToRtv(ctx.Get(), srcSrv.Get(), directRtv.Get(), kW, kH, 0.50f, true, true)) {
        return false;
    }

    auto readback = [&](ID3D11Texture2D* tex, std::vector<uint16_t>& out) -> bool {
        ctx->CopyResource(staging.Get(), tex);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
        out.resize(static_cast<size_t>(kW) * kH * 4u);
        const auto* src = static_cast<const uint8_t*>(m.pData);
        for (uint32_t y = 0; y < kH; ++y) {
            std::memcpy(out.data() + static_cast<size_t>(y) * kW * 4u,
                        src + static_cast<size_t>(y) * m.RowPitch, kW * 8u);
        }
        ctx->Unmap(staging.Get(), 0);
        return true;
    };

    std::vector<uint16_t> applyOut, directOut;
    if (!readback(applyTex.Get(), applyOut) || !readback(directTex.Get(), directOut)) {
        return false;
    }

    float maxDiff = 0.0f;
    for (size_t i = 0; i < applyOut.size(); ++i) {
        const float a = DirectX::PackedVector::XMConvertHalfToFloat(applyOut[i]);
        const float b = DirectX::PackedVector::XMConvertHalfToFloat(directOut[i]);
        maxDiff = std::max(maxDiff, std::fabs(a - b));
    }

    std::printf("  HDR applyToRtv vs apply: maxDiff=%.6f\n", maxDiff);
    sharpen.shutdown();
    return maxDiff < 1e-4f;
}

} // namespace

int main() {
    std::printf("[SharpenPassTest]\n");
    bool ok = true;
    ok &= testLdrParity();
    ok &= testHdrParity();
    std::printf("[SharpenPassTest] %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
