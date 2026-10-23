#pragma once
#include <d3d11.h>
#include <windows.h>

void hookShaderCreation(ID3D11Device* device);
void syncConfigOnce();

namespace edpe {
void installD3D11Hooks();
}

extern "C" HRESULT WINAPI EdpeD3D11CreateDevice(
    IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE driver_type,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL* feature_levels,
    UINT feature_level_count,
    UINT sdk_version,
    ID3D11Device** device,
    D3D_FEATURE_LEVEL* feature_level,
    ID3D11DeviceContext** context);
