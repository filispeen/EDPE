#include "hud_debug_pass.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>

using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context))) {
        std::cerr << "Failed to create WARP device\n";
        return 1;
    }

    edpe::HudDebugPass pass;
    if (!pass.initialize(device.Get(), context.Get())) {
        std::cerr << "Failed to initialize HudDebugPass\n";
        return 2;
    }

    // 4x4 Output backbuffer and RTV
    D3D11_TEXTURE2D_DESC outDesc{};
    outDesc.Width = outDesc.Height = 4;
    outDesc.MipLevels = outDesc.ArraySize = 1;
    outDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    outDesc.SampleDesc.Count = 1;
    outDesc.Usage = D3D11_USAGE_DEFAULT;
    outDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ComPtr<ID3D11Texture2D> targetTex;
    ComPtr<ID3D11RenderTargetView> targetRtv;
    if (FAILED(device->CreateTexture2D(&outDesc, nullptr, &targetTex)) ||
        FAILED(device->CreateRenderTargetView(targetTex.Get(), nullptr, &targetRtv))) {
        return 3;
    }

    // 4x4 Staging readback texture
    D3D11_TEXTURE2D_DESC stageDesc = outDesc;
    stageDesc.Usage = D3D11_USAGE_STAGING;
    stageDesc.BindFlags = 0;
    stageDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stagingTex;
    if (FAILED(device->CreateTexture2D(&stageDesc, nullptr, &stagingTex))) {
        return 4;
    }

    // 4x4 current frame: neutral gray/blue
    std::array<uint32_t, 16> framePixels{};
    framePixels.fill(0xFF808080); // 50% gray
    D3D11_SUBRESOURCE_DATA frameInit{framePixels.data(), 4 * sizeof(uint32_t), 0};
    ComPtr<ID3D11Texture2D> frameTex;
    ComPtr<ID3D11ShaderResourceView> frameSrv;
    if (FAILED(device->CreateTexture2D(&outDesc, &frameInit, &frameTex)) ||
        FAILED(device->CreateShaderResourceView(frameTex.Get(), nullptr, &frameSrv))) {
        return 5;
    }

    // 4x4 HUD mask (R8_UNORM): rows 0-2 = 0, row 3 = 255 (HUD present)
    D3D11_TEXTURE2D_DESC maskDesc{};
    maskDesc.Width = maskDesc.Height = 4;
    maskDesc.MipLevels = maskDesc.ArraySize = 1;
    maskDesc.Format = DXGI_FORMAT_R8_UNORM;
    maskDesc.SampleDesc.Count = 1;
    maskDesc.Usage = D3D11_USAGE_DEFAULT;
    maskDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::array<uint8_t, 16> maskPixels{};
    for (int i = 0; i < 12; ++i) maskPixels[i] = 0;
    for (int i = 12; i < 16; ++i) maskPixels[i] = 255;
    D3D11_SUBRESOURCE_DATA maskInit{maskPixels.data(), 4 * sizeof(uint8_t), 0};
    ComPtr<ID3D11Texture2D> maskTex;
    ComPtr<ID3D11ShaderResourceView> maskSrv;
    if (FAILED(device->CreateTexture2D(&maskDesc, &maskInit, &maskTex)) ||
        FAILED(device->CreateShaderResourceView(maskTex.Get(), nullptr, &maskSrv))) {
        return 6;
    }

    // 4x4 game backbuffer: rows 0,1,3 match lowRes; row 2 differs (menu UI: white vs black)
    std::array<uint32_t, 16> gamePixels{};
    for (int i = 0; i < 8; ++i) gamePixels[i] = 0xFF000000;  // black
    for (int i = 8; i < 12; ++i) gamePixels[i] = 0xFFFFFFFF; // white (menu diff)
    for (int i = 12; i < 16; ++i) gamePixels[i] = 0xFF000000;
    D3D11_SUBRESOURCE_DATA gameInit{gamePixels.data(), 4 * sizeof(uint32_t), 0};
    ComPtr<ID3D11Texture2D> gameTex;
    ComPtr<ID3D11ShaderResourceView> gameSrv;
    if (FAILED(device->CreateTexture2D(&outDesc, &gameInit, &gameTex)) ||
        FAILED(device->CreateShaderResourceView(gameTex.Get(), nullptr, &gameSrv))) {
        return 7;
    }

    // 2x2 low-res world: all black
    D3D11_TEXTURE2D_DESC lowDesc = outDesc;
    lowDesc.Width = lowDesc.Height = 2;
    std::array<uint32_t, 4> lowPixels{};
    lowPixels.fill(0xFF000000);
    D3D11_SUBRESOURCE_DATA lowInit{lowPixels.data(), 2 * sizeof(uint32_t), 0};
    ComPtr<ID3D11Texture2D> lowTex;
    ComPtr<ID3D11ShaderResourceView> lowSrv;
    if (FAILED(device->CreateTexture2D(&lowDesc, &lowInit, &lowTex)) ||
        FAILED(device->CreateShaderResourceView(lowTex.Get(), nullptr, &lowSrv))) {
        return 8;
    }

    auto readBackPixel = [&](int x, int y, uint8_t& r, uint8_t& g, uint8_t& b) {
        context->CopyResource(stagingTex.Get(), targetTex.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch;
        const auto* px = row + x * 4;
        r = px[0];
        g = px[1];
        b = px[2];
        context->Unmap(stagingTex.Get(), 0);
        return true;
    };

    // Test 1: Mode 0 (Combined Mask)
    // Row 0 (no mask): grayscale background (r == g && g == b)
    // Row 3 (hudMask = 1.0): bright Amber (r > g && g > b)
    if (!pass.render(context.Get(), frameSrv.Get(), maskSrv.Get(), lowSrv.Get(), gameSrv.Get(),
            targetRtv.Get(), 4, 4, 4, 4, 1.0f, 0.02f, 0, false)) {
        std::cerr << "Mode 0 render failed\n";
        return 10;
    }
    uint8_t r = 0, g = 0, b = 0;
    if (!readBackPixel(1, 0, r, g, b)) return 11;
    if (std::abs(static_cast<int>(r) - static_cast<int>(g)) > 2 ||
        std::abs(static_cast<int>(g) - static_cast<int>(b)) > 2) {
        std::cerr << "Mode 0 row 0 not grayscale: " << (int)r << "," << (int)g << "," << (int)b << "\n";
        return 12;
    }
    if (!readBackPixel(1, 3, r, g, b)) return 13;
    if (r <= g || g <= b || r < 140) {
        std::cerr << "Mode 0 row 3 not amber: " << (int)r << "," << (int)g << "," << (int)b << "\n";
        return 14;
    }

    // Test 2: Mode 1 (Cockpit HUD Only)
    // Row 2 (menu diff only): must NOT be colored; should be grayscale
    // Row 3 (cockpit HUD): must be Amber
    if (!pass.render(context.Get(), frameSrv.Get(), maskSrv.Get(), lowSrv.Get(), gameSrv.Get(),
            targetRtv.Get(), 4, 4, 4, 4, 1.0f, 0.02f, 1, false)) {
        return 20;
    }
    if (!readBackPixel(1, 2, r, g, b)) return 21;
    if (std::abs(static_cast<int>(r) - static_cast<int>(g)) > 2 ||
        std::abs(static_cast<int>(g) - static_cast<int>(b)) > 2) {
        std::cerr << "Mode 1 row 2 should be grayscale: " << (int)r << "," << (int)g << "," << (int)b << "\n";
        return 22;
    }
    if (!readBackPixel(1, 3, r, g, b)) return 23;
    if (r <= g || g <= b || r < 140) {
        std::cerr << "Mode 1 row 3 should be amber\n";
        return 24;
    }

    // Test 3: Mode 2 (Menu UI Diff Only)
    // Row 2 (menu diff): must be Cyan (b > 150, g > 150, r < 50)
    // Row 3 (cockpit HUD only): must be grayscale
    if (!pass.render(context.Get(), frameSrv.Get(), maskSrv.Get(), lowSrv.Get(), gameSrv.Get(),
            targetRtv.Get(), 4, 4, 4, 4, 1.0f, 0.02f, 2, false)) {
        return 30;
    }
    if (!readBackPixel(1, 2, r, g, b)) return 31;
    if (b <= r || g <= r || b < 140) {
        std::cerr << "Mode 2 row 2 should be cyan: " << (int)r << "," << (int)g << "," << (int)b << "\n";
        return 32;
    }
    if (!readBackPixel(1, 3, r, g, b)) return 33;
    if (std::abs(static_cast<int>(r) - static_cast<int>(g)) > 2) {
        std::cerr << "Mode 2 row 3 should be grayscale\n";
        return 34;
    }

    // Test 4: Mode 3 (Split Heatmap)
    // Row 2: Cyan (Menu UI)
    // Row 3: Amber (Cockpit HUD)
    if (!pass.render(context.Get(), frameSrv.Get(), maskSrv.Get(), lowSrv.Get(), gameSrv.Get(),
            targetRtv.Get(), 4, 4, 4, 4, 1.0f, 0.02f, 3, false)) {
        return 40;
    }
    if (!readBackPixel(1, 2, r, g, b)) return 41;
    if (b <= r || g <= r) {
        std::cerr << "Mode 3 row 2 should be cyan\n";
        return 42;
    }
    if (!readBackPixel(1, 3, r, g, b)) return 43;
    if (r <= g || g <= b) {
        std::cerr << "Mode 3 row 3 should be amber\n";
        return 44;
    }

    // Test 5: Mode 4 (Binary Mask Only)
    // Row 0: Pure Black
    // Row 3: Amber (or highlighted)
    if (!pass.render(context.Get(), frameSrv.Get(), maskSrv.Get(), lowSrv.Get(), gameSrv.Get(),
            targetRtv.Get(), 4, 4, 4, 4, 1.0f, 0.02f, 4, false)) {
        return 50;
    }
    if (!readBackPixel(1, 0, r, g, b)) return 51;
    if (r != 0 || g != 0 || b != 0) {
        std::cerr << "Mode 4 row 0 should be pure black: " << (int)r << "," << (int)g << "," << (int)b << "\n";
        return 52;
    }
    if (!readBackPixel(1, 3, r, g, b)) return 53;
    if (r < 180 || g < 90) {
        std::cerr << "Mode 4 row 3 should be highlighted\n";
        return 54;
    }

    // Test 6: Null Safety with null SRVs
    if (!pass.render(context.Get(), nullptr, nullptr, nullptr, nullptr, targetRtv.Get(), 4, 4, 4, 4)) {
        std::cerr << "Null fallback render failed\n";
        return 60;
    }

    pass.shutdown();
    std::cout << "All 6 HudDebugPass tests passed successfully!\n";
    return 0;
}
