#include "motion_shader.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <bit>
#include <cstdint>

using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;

    ComPtr<ID3DBlob> vertex_code, pixel_code;
    if (FAILED(D3DCompile(edpe::kMotionShader, sizeof(edpe::kMotionShader) - 1,
            nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vertex_code, nullptr)) ||
        FAILED(D3DCompile(edpe::kMotionShader, sizeof(edpe::kMotionShader) - 1,
            nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &pixel_code, nullptr))) return 2;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    if (FAILED(device->CreateVertexShader(vertex_code->GetBufferPointer(),
            vertex_code->GetBufferSize(), nullptr, &vertex)) ||
        FAILED(device->CreatePixelShader(pixel_code->GetBufferPointer(),
            pixel_code->GetBufferSize(), nullptr, &pixel))) return 3;

    std::array<std::uint64_t, 16> depth_values{};
    depth_values.fill(std::bit_cast<std::uint32_t>(0.0025f)); // depthB=.025, viewZ=10
    depth_values[0] = 0;        // Invalid far/background pixel.
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 4;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
    D3D11_SUBRESOURCE_DATA initial{depth_values.data(), 4 * sizeof(std::uint64_t), 0};
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11ShaderResourceView> depth_view;
    D3D11_SHADER_RESOURCE_VIEW_DESC depth_srv_desc{};
    depth_srv_desc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    depth_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    depth_srv_desc.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&desc, &initial, &depth)) ||
        FAILED(device->CreateShaderResourceView(depth.Get(), &depth_srv_desc, &depth_view))) return 4;

    UINT support = 0;
    if (FAILED(device->CheckFormatSupport(DXGI_FORMAT_R16G16_FLOAT, &support)) ||
        !(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET)) return 5;
    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> motion;
    ComPtr<ID3D11RenderTargetView> motion_view;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &motion)) ||
        FAILED(device->CreateRenderTargetView(motion.Get(), nullptr, &motion_view))) return 6;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &readback))) return 7;

    // Six row-major [R|C] rows, projection scales, depth coefficient and size.
    const float constants[32] = {
        1, 0, 0, 10,  0, 1, 0, 10,  0, 0, 1, 0,
        1, 0, 0,  0,  0, 1, 0,  0,  0, 0, 1, 0,
        1, 1, 1, 1,  .025f, 4, 4, 0
    };
    D3D11_BUFFER_DESC buffer_desc{};
    buffer_desc.ByteWidth = sizeof(constants);
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA buffer_data{constants, 0, 0};
    ComPtr<ID3D11Buffer> camera;
    if (FAILED(device->CreateBuffer(&buffer_desc, &buffer_data, &camera))) return 8;

    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3DDeviceContextState> motion_state;
    const auto level = device->GetFeatureLevel();
    if (FAILED(device.As(&device1)) || FAILED(context.As(&context1)) ||
        FAILED(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION,
            __uuidof(ID3D11Device1), nullptr, &motion_state))) return 11;
    const D3D11_VIEWPORT prior_viewport{0, 0, 2, 2, 0, 1};
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->RSSetViewports(1, &prior_viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ComPtr<ID3DDeviceContextState> prior_state;
    context1->SwapDeviceContextState(motion_state.Get(), &prior_state);

    const D3D11_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
    auto* rtv = motion_view.Get();
    auto* srv = depth_view.Get();
    auto* cb = camera.Get();
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vertex.Get(), nullptr, 0);
    context->PSSetShader(pixel.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &srv);
    context->PSSetConstantBuffers(0, 1, &cb);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context1->SwapDeviceContextState(prior_state.Get(), nullptr);
    ComPtr<ID3D11VertexShader> restored_vertex;
    D3D11_PRIMITIVE_TOPOLOGY restored_topology{};
    UINT restored_count = 1;
    D3D11_VIEWPORT restored_viewport{};
    context->VSGetShader(&restored_vertex, nullptr, nullptr);
    context->IAGetPrimitiveTopology(&restored_topology);
    context->RSGetViewports(&restored_count, &restored_viewport);
    if (restored_vertex.Get() != vertex.Get() ||
        restored_topology != D3D11_PRIMITIVE_TOPOLOGY_LINELIST ||
        restored_count != 1 || restored_viewport.Width != 2) return 12;
    context->CopyResource(readback.Get(), motion.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 9;
    const auto* first = static_cast<const std::uint16_t*>(mapped.pData);
    const auto* center = reinterpret_cast<const std::uint16_t*>(
        static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch + 4);
    // Half-float +2 is 0x4000; -2 is 0xC000.
    const bool valid = first[0] == 0 && first[1] == 0 &&
        center[0] == 0x4000 && center[1] == 0xC000;
    context->Unmap(readback.Get(), 0);
    return valid ? 0 : 10;
}
