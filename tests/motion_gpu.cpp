#include "motion_pass.h"

#include <DirectXPackedVector.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
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
    if (!correct) return 10;

    // Camera rows and projection scales from a user-confirmed Odyssey scene capture.
    previous = {{-.965338f,-.0708149f,-.251214f,19.5594f,
                 -.00114341f,.963628f,-.267244f,-39.2084f,
                 .261002f,-.257694f,-.930307f,-99.7282f},
                .974278629f,1.73205078f,.0250000004f};
    now = previous;
    now.worldFromView[3] += 2.5f;
    now.worldFromView[7] -= 1.25f;
    float expected_x = 0, expected_y = 0;
    if (!edpe::cameraDepthMotion(now, previous, 4, 4, 1, 1, .0025f,
            &expected_x, &expected_y)) return 11;
    if (!pass.render(depth_view.Get(), now, previous, 4, 4)) return 12;
    context->CopyResource(readback.Get(), output.Get());
    if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 13;
    valid = reinterpret_cast<const std::uint16_t*>(
        static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch + 4);
    const float actual_x = DirectX::PackedVector::XMConvertHalfToFloat(valid[0]);
    const float actual_y = DirectX::PackedVector::XMConvertHalfToFloat(valid[1]);
    context->Unmap(readback.Get(), 0);
    if (std::fabs(actual_x - expected_x) >= .01f ||
        std::fabs(actual_y - expected_y) >= .01f) return 14;

    // The observed 5376-byte dynamic camera CB can be retained on the GPU.
    D3D11_BUFFER_DESC camera_desc{};
    camera_desc.ByteWidth = 5376;
    camera_desc.Usage = D3D11_USAGE_DYNAMIC;
    camera_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    camera_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> game_camera, gpu_camera, verify_camera;
    if (FAILED(device->CreateBuffer(&camera_desc, nullptr, &game_camera))) return 15;
    D3D11_MAPPED_SUBRESOURCE camera_map{};
    if (FAILED(context->Map(game_camera.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
            &camera_map))) return 16;
    auto* camera_words = static_cast<float*>(camera_map.pData);
    std::fill_n(camera_words, 5376 / sizeof(float), 0.0f);
    camera_words[932] = 19.5594f;
    camera_words[1094] = .025f;
    context->Unmap(game_camera.Get(), 0);
    camera_desc.Usage = D3D11_USAGE_DEFAULT;
    camera_desc.CPUAccessFlags = 0;
    if (FAILED(device->CreateBuffer(&camera_desc, nullptr, &gpu_camera))) return 17;
    camera_desc.Usage = D3D11_USAGE_STAGING;
    camera_desc.BindFlags = 0;
    camera_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateBuffer(&camera_desc, nullptr, &verify_camera))) return 18;
    context->CopyResource(gpu_camera.Get(), game_camera.Get());
    context->CopyResource(verify_camera.Get(), gpu_camera.Get());
    if (FAILED(context->Map(verify_camera.Get(), 0, D3D11_MAP_READ, 0,
            &camera_map))) return 19;
    camera_words = static_cast<float*>(camera_map.pData);
    const bool camera_copied = camera_words[932] == 19.5594f &&
        camera_words[1094] == .025f;
    context->Unmap(verify_camera.Get(), 0);
    if (!camera_copied) return 20;

    static constexpr char camera_shader[] = R"(
        cbuffer Camera : register(b0) { float4 words[336]; };
        RWTexture2D<float4> result : register(u0);
        [numthreads(1, 1, 1)]
        void main(uint3 id : SV_DispatchThreadID) {
            result[id.xy] = float4(words[233].x, words[273].z, 0, 1);
        }
    )";
    ComPtr<ID3DBlob> code;
    ComPtr<ID3D11ComputeShader> shader;
    if (FAILED(D3DCompile(camera_shader, sizeof(camera_shader) - 1, nullptr,
            nullptr, nullptr, "main", "cs_5_0", 0, 0, &code, nullptr)) ||
        FAILED(device->CreateComputeShader(code->GetBufferPointer(),
            code->GetBufferSize(), nullptr, &shader))) return 21;
    D3D11_TEXTURE2D_DESC camera_result_desc{};
    camera_result_desc.Width = camera_result_desc.Height = 1;
    camera_result_desc.MipLevels = camera_result_desc.ArraySize = 1;
    camera_result_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    camera_result_desc.SampleDesc.Count = 1;
    camera_result_desc.Usage = D3D11_USAGE_DEFAULT;
    camera_result_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> camera_result, camera_result_readback;
    ComPtr<ID3D11UnorderedAccessView> camera_result_uav;
    if (FAILED(device->CreateTexture2D(&camera_result_desc, nullptr, &camera_result)) ||
        FAILED(device->CreateUnorderedAccessView(camera_result.Get(), nullptr,
            &camera_result_uav))) return 22;
    camera_result_desc.Usage = D3D11_USAGE_STAGING;
    camera_result_desc.BindFlags = 0;
    camera_result_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&camera_result_desc, nullptr,
            &camera_result_readback))) return 23;
    auto* gpu_cb = gpu_camera.Get();
    auto* uav = camera_result_uav.Get();
    context->CSSetShader(shader.Get(), nullptr, 0);
    context->CSSetConstantBuffers(0, 1, &gpu_cb);
    context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context->Dispatch(1, 1, 1);
    context->CopyResource(camera_result_readback.Get(), camera_result.Get());
    if (FAILED(context->Map(camera_result_readback.Get(), 0, D3D11_MAP_READ, 0,
            &camera_map))) return 24;
    const auto* shader_values = static_cast<const float*>(camera_map.pData);
    const bool shader_read = shader_values[0] == 19.5594f && shader_values[1] == .025f;
    context->Unmap(camera_result_readback.Get(), 0);
    if (!shader_read) return 25;

    // The game CB stores scaled basis columns at float offsets 1080..1091,
    // translation at 935/939/943 and reversed-Z depthB at 1094.
    auto pack_camera = [](const edpe::CameraProjection& camera) {
        std::array<float, 1344> words{};
        for (unsigned row = 0; row < 3; ++row) {
            words[932 + row * 4 + 3] = camera.worldFromView[row * 4 + 3];
            words[1080 + row * 4] = camera.worldFromView[row * 4] * camera.scaleX;
            words[1081 + row * 4] = camera.worldFromView[row * 4 + 1] * camera.scaleY;
            words[1083 + row * 4] = camera.worldFromView[row * 4 + 2];
        }
        words[1094] = camera.depthB;
        return words;
    };
    const auto now_words = pack_camera(now);
    const auto previous_words = pack_camera(previous);
    camera_desc.Usage = D3D11_USAGE_DEFAULT;
    camera_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    camera_desc.CPUAccessFlags = 0;
    ComPtr<ID3D11Buffer> previous_gpu_camera;
    if (FAILED(device->CreateBuffer(&camera_desc, nullptr, &previous_gpu_camera))) return 26;
    context->UpdateSubresource(gpu_camera.Get(), 0, nullptr, now_words.data(), 0, 0);
    context->UpdateSubresource(previous_gpu_camera.Get(), 0, nullptr,
        previous_words.data(), 0, 0);
    if (!pass.renderGpuCameras(depth_view.Get(), gpu_camera.Get(),
            previous_gpu_camera.Get(), 4, 4)) return 27;
    context->CopyResource(readback.Get(), output.Get());
    if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 28;
    valid = reinterpret_cast<const std::uint16_t*>(
        static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch + 4);
    const float gpu_x = DirectX::PackedVector::XMConvertHalfToFloat(valid[0]);
    const float gpu_y = DirectX::PackedVector::XMConvertHalfToFloat(valid[1]);
    context->Unmap(readback.Get(), 0);
    if (std::fabs(gpu_x - expected_x) >= .01f ||
        std::fabs(gpu_y - expected_y) >= .01f) return 29;

    // A camera turn exercises different current and previous projection bases.
    constexpr float cosine = .965925826f, sine = .258819045f;
    previous = {{1,0,0,0, 0,1,0,0, 0,0,1,0}, .974278629f,1.73205078f,.025f};
    now = {{cosine,0,sine,0, 0,1,0,0, -sine,0,cosine,0},
        previous.scaleX, previous.scaleY, previous.depthB};
    if (!edpe::cameraDepthMotion(now, previous, 4, 4, 1, 1, .0025f,
            &expected_x, &expected_y)) return 30;
    const auto turned_now = pack_camera(now);
    const auto turned_previous = pack_camera(previous);
    context->UpdateSubresource(gpu_camera.Get(), 0, nullptr, turned_now.data(), 0, 0);
    context->UpdateSubresource(previous_gpu_camera.Get(), 0, nullptr,
        turned_previous.data(), 0, 0);
    if (!pass.renderGpuCameras(depth_view.Get(), gpu_camera.Get(),
            previous_gpu_camera.Get(), 4, 4)) return 31;
    context->CopyResource(readback.Get(), output.Get());
    if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 32;
    valid = reinterpret_cast<const std::uint16_t*>(
        static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch + 4);
    const float turned_x = DirectX::PackedVector::XMConvertHalfToFloat(valid[0]);
    const float turned_y = DirectX::PackedVector::XMConvertHalfToFloat(valid[1]);
    context->Unmap(readback.Get(), 0);
    return std::fabs(turned_x - expected_x) < .01f &&
        std::fabs(turned_y - expected_y) < .01f ? 0 : 33;
}
