#include "motion_pass.h"

#include <array>
#include <bit>
#include <cstdint>

using Microsoft::WRL::ComPtr;

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context))) return 1;

    std::array<std::uint64_t, 16> depths{};
    depths.fill(std::bit_cast<std::uint32_t>(0.0025f));
    depths[0] = 0;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 4;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
    D3D11_SUBRESOURCE_DATA initial{depths.data(), 4 * sizeof(std::uint64_t), 0};
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11ShaderResourceView> depth_view;
    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
    view_desc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view_desc.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateTexture2D(&desc, &initial, &depth)) ||
        FAILED(device->CreateShaderResourceView(depth.Get(), &view_desc, &depth_view))) return 2;

    edpe::MotionPass pass;
    if (!pass.initialize(device.Get(), context.Get())) return 3;
    edpe::CameraProjection previous{{1,0,0,0, 0,1,0,0, 0,0,1,0}, 1,1,.025f};
    auto now = previous;
    now.worldFromView[3] = now.worldFromView[7] = 10;
    const D3D11_VIEWPORT prior_viewport{0, 0, 2, 2, 0, 1};
    context->RSSetViewports(1, &prior_viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    if (!pass.render(depth_view.Get(), now, previous, 4, 4) || !pass.output()) return 4;

    D3D11_PRIMITIVE_TOPOLOGY topology{};
    UINT viewport_count = 1;
    D3D11_VIEWPORT viewport{};
    context->IAGetPrimitiveTopology(&topology);
    context->RSGetViewports(&viewport_count, &viewport);
    if (topology != D3D11_PRIMITIVE_TOPOLOGY_LINELIST ||
        viewport_count != 1 || viewport.Width != 2) return 5;

    auto* first_output = pass.output();
    if (!pass.render(depth_view.Get(), now, previous, 4, 4) ||
        pass.output() != first_output) return 6;

    ComPtr<ID3D11Resource> output_resource;
    pass.output()->GetResource(&output_resource);
    ComPtr<ID3D11Texture2D> output;
    if (FAILED(output_resource.As(&output))) return 7;
    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &readback))) return 8;
    context->CopyResource(readback.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 9;
    const auto* invalid = static_cast<const std::uint16_t*>(mapped.pData);
    const auto* valid = reinterpret_cast<const std::uint16_t*>(
        static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch + 4);
    const bool correct = invalid[0] == 0 && invalid[1] == 0 &&
        valid[0] == 0x4000 && valid[1] == 0xC000;
    context->Unmap(readback.Get(), 0);
    return correct ? 0 : 10;
}
