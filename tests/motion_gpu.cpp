#include "motion_pass.h"
#include "celestial_motion.h"

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
    if (std::fabs(turned_x - expected_x) >= .01f ||
        std::fabs(turned_y - expected_y) >= .01f) return 33;

    // Pure camera rotation (translationStep == 0) must clamp camera-attached viewmodel
    // geometry (disambiguated via static prevSceneDepth at z < 10m) to 0 MV
    // while keeping non-zero rotational MV for world background pixels.
    std::array<std::uint64_t, 16> vm_depths{};
    vm_depths.fill(std::bit_cast<std::uint32_t>(0.0025f));
    vm_depths[5] = std::bit_cast<std::uint32_t>(0.1676f); // pixel (1,1): z=0.149m viewmodel
    vm_depths[6] = std::bit_cast<std::uint32_t>(0.0357f); // pixel (2,1): z=0.70m barrel/cockpit
    D3D11_TEXTURE2D_DESC vm_depth_desc{};
    vm_depth_desc.Width = vm_depth_desc.Height = 4;
    vm_depth_desc.MipLevels = vm_depth_desc.ArraySize = 1;
    vm_depth_desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    vm_depth_desc.SampleDesc.Count = 1;
    vm_depth_desc.Usage = D3D11_USAGE_DEFAULT;
    vm_depth_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
    D3D11_SUBRESOURCE_DATA vm_init{vm_depths.data(), 4 * sizeof(std::uint64_t), 0};
    ComPtr<ID3D11Texture2D> vm_depth, prev_vm_depth;
    ComPtr<ID3D11ShaderResourceView> vm_depth_view, prev_vm_depth_view;
    if (FAILED(device->CreateTexture2D(&vm_depth_desc, &vm_init, &vm_depth)) ||
        FAILED(device->CreateShaderResourceView(vm_depth.Get(), &view_desc, &vm_depth_view)) ||
        FAILED(device->CreateTexture2D(&vm_depth_desc, &vm_init, &prev_vm_depth)) ||
        FAILED(device->CreateShaderResourceView(prev_vm_depth.Get(), &view_desc, &prev_vm_depth_view))) {
        return 34;
    }
    float bg_expected_x = 0.0f, bg_expected_y = 0.0f;
    if (!edpe::cameraDepthMotion(now, previous, 4, 4, 3, 1, .0025f,
            &bg_expected_x, &bg_expected_y)) return 38;
    if (!pass.render(vm_depth_view.Get(), now, previous, 4, 4,
            0.0f, 0.0f, 0.0f, 0.0f, false, prev_vm_depth_view.Get())) return 35;
    context->CopyResource(readback.Get(), output.Get());
    if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 36;
    const auto* row1 = reinterpret_cast<const std::uint16_t*>(
        static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch);
    const float vm_x = DirectX::PackedVector::XMConvertHalfToFloat(row1[2]);
    const float vm_y = DirectX::PackedVector::XMConvertHalfToFloat(row1[3]);
    const float bar_x = DirectX::PackedVector::XMConvertHalfToFloat(row1[4]);
    const float bar_y = DirectX::PackedVector::XMConvertHalfToFloat(row1[5]);
    const float bg_x = DirectX::PackedVector::XMConvertHalfToFloat(row1[6]);
    const float bg_y = DirectX::PackedVector::XMConvertHalfToFloat(row1[7]);
    context->Unmap(readback.Get(), 0);
    if (std::fabs(vm_x) >= 1e-4f || std::fabs(vm_y) >= 1e-4f) return 37;
    if (std::fabs(bar_x) >= 1e-4f || std::fabs(bar_y) >= 1e-4f) return 39;
    if (std::fabs(bg_x - bg_expected_x) >= 0.05f || std::fabs(bg_y - bg_expected_y) >= 0.05f) return 40;

    // Verify canonical OutDepth and OutClass outputs from Compute Shader MotionPass.
    if (!pass.canonicalDepth()) return 41;
    if (!pass.render(vm_depth_view.Get(), now, previous, 4, 4,
            0.0f, 0.0f, 0.0f, 0.0f, false, prev_vm_depth_view.Get(),
            /*preserveContextState=*/true, /*zeroTranslation=*/false,
            /*enableClassOutput=*/true)) return 42;
    if (!pass.pixelClass()) return 43;

    D3D11_TEXTURE2D_DESC depth_rb_desc{};
    depth_rb_desc.Width = depth_rb_desc.Height = 4;
    depth_rb_desc.MipLevels = depth_rb_desc.ArraySize = 1;
    depth_rb_desc.Format = DXGI_FORMAT_R32_FLOAT;
    depth_rb_desc.SampleDesc.Count = 1;
    depth_rb_desc.Usage = D3D11_USAGE_STAGING;
    depth_rb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> depth_rb;
    if (FAILED(device->CreateTexture2D(&depth_rb_desc, nullptr, &depth_rb))) return 44;
    context->CopyResource(depth_rb.Get(), pass.canonicalDepthTexture());
    if (FAILED(context->Map(depth_rb.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 45;
    const auto* drow1 = reinterpret_cast<const float*>(
        static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch);
    const float canonical_vm_d = drow1[1];
    context->Unmap(depth_rb.Get(), 0);
    if (std::fabs(canonical_vm_d - 0.1676f) >= 1e-4f) return 46;

    D3D11_TEXTURE2D_DESC cls_rb_desc = depth_rb_desc;
    cls_rb_desc.Format = DXGI_FORMAT_R8_UINT;
    ComPtr<ID3D11Texture2D> cls_rb;
    if (FAILED(device->CreateTexture2D(&cls_rb_desc, nullptr, &cls_rb))) return 47;
    context->CopyResource(cls_rb.Get(), pass.pixelClassTexture());
    if (FAILED(context->Map(cls_rb.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 48;
    const auto* crow1 = static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch;
    const std::uint8_t vm_cls = crow1[1];
    context->Unmap(cls_rb.Get(), 0);
    if (vm_cls != static_cast<std::uint8_t>(edpe::MotionPixelClass::CockpitStatic)) return 49;

    // Near world wall (z = 2.0m < 3.5m) during fast lateral translation (translationStep = 1.0m > 0.35m)
    // must NOT be zeroed when prevSceneDepth matches the reprojected world position.
    std::array<std::uint64_t, 16> near_cur{}, near_prev{};
    near_cur.fill(std::bit_cast<std::uint32_t>(0.0125f));  // z = 0.025 / 0.0125 = 2.0m
    near_prev.fill(std::bit_cast<std::uint32_t>(0.0125f)); // same plane at z = 2.0m
    D3D11_SUBRESOURCE_DATA near_cur_init{near_cur.data(), 4 * sizeof(std::uint64_t), 0};
    D3D11_SUBRESOURCE_DATA near_prev_init{near_prev.data(), 4 * sizeof(std::uint64_t), 0};
    ComPtr<ID3D11Texture2D> near_cur_tex, near_prev_tex;
    ComPtr<ID3D11ShaderResourceView> near_cur_srv, near_prev_srv;
    if (FAILED(device->CreateTexture2D(&vm_depth_desc, &near_cur_init, &near_cur_tex)) ||
        FAILED(device->CreateShaderResourceView(near_cur_tex.Get(), &view_desc, &near_cur_srv)) ||
        FAILED(device->CreateTexture2D(&vm_depth_desc, &near_prev_init, &near_prev_tex)) ||
        FAILED(device->CreateShaderResourceView(near_prev_tex.Get(), &view_desc, &near_prev_srv))) {
        return 50;
    }
    edpe::CameraProjection walkPrev{{1,0,0,0, 0,1,0,0, 0,0,1,0}, 1, 1, 0.025f};
    auto walkNow = walkPrev;
    walkNow.worldFromView[3] = 1.0f; // 1.0m lateral step (> 0.35m), pixel (1,1) reprojects to (2.5, 1.5) -> mv.x = +1.0
    if (!pass.render(near_cur_srv.Get(), walkNow, walkPrev, 4, 4,
            0.0f, 0.0f, 0.0f, 0.0f, false, near_prev_srv.Get(),
            /*preserveContextState=*/true, /*zeroTranslation=*/false,
            /*enableClassOutput=*/true)) return 51;
    context->CopyResource(readback.Get(), pass.outputTexture());
    context->CopyResource(cls_rb.Get(), pass.pixelClassTexture());
    if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 52;
    const auto* walk_row1 = reinterpret_cast<const std::uint16_t*>(
        static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch);
    const float walk_mv_x = DirectX::PackedVector::XMConvertHalfToFloat(walk_row1[2]);
    const float walk_mv_y = DirectX::PackedVector::XMConvertHalfToFloat(walk_row1[3]);
    context->Unmap(readback.Get(), 0);
    if (std::fabs(walk_mv_x - 1.0f) >= 0.05f || std::fabs(walk_mv_y) >= 0.05f) return 53;
    if (FAILED(context->Map(cls_rb.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 54;
    const std::uint8_t walk_cls = (static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch)[1];
    context->Unmap(cls_rb.Get(), 0);
    if (walk_cls != static_cast<std::uint8_t>(edpe::MotionPixelClass::World)) return 55;

    // Planar bilinear prevDepthMatches: sloped floor where depth varies linearly across 2x2 quad
    // by > 1% per texel, yet exact bilinear matches expected depth within 1% and twist == 0.
    std::array<std::uint64_t, 16> slope_prev{}, slope_cur{};
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const float d = 0.010f + 0.002f * static_cast<float>(x) + 0.002f * static_cast<float>(y);
            slope_prev[y * 4 + x] = std::bit_cast<std::uint32_t>(d);
            slope_cur[y * 4 + x] = std::bit_cast<std::uint32_t>(d);
        }
    }
    // Pixel (1,1) has priorPixel shifted by (+0.5, +0.5) via jitter-free small camera shift:
    // bilinear at (2.0, 2.0) -> raster (1.5, 1.5) -> midpoint of (1,1),(2,1),(1,2),(2,2) = 0.010 + 0.003 + 0.003 = 0.016.
    // Nearest texels are 0.014, 0.016, 0.016, 0.018; use shift (+0.5, 0) where midpoint of (1,1)=0.014 and (2,1)=0.016 is 0.015 (6.7% from both!).
    slope_cur[5] = std::bit_cast<std::uint32_t>(0.015f);
    D3D11_SUBRESOURCE_DATA slope_cur_init{slope_cur.data(), 4 * sizeof(std::uint64_t), 0};
    D3D11_SUBRESOURCE_DATA slope_prev_init{slope_prev.data(), 4 * sizeof(std::uint64_t), 0};
    ComPtr<ID3D11Texture2D> slope_cur_tex, slope_prev_tex;
    ComPtr<ID3D11ShaderResourceView> slope_cur_srv, slope_prev_srv;
    if (FAILED(device->CreateTexture2D(&vm_depth_desc, &slope_cur_init, &slope_cur_tex)) ||
        FAILED(device->CreateShaderResourceView(slope_cur_tex.Get(), &view_desc, &slope_cur_srv)) ||
        FAILED(device->CreateTexture2D(&vm_depth_desc, &slope_prev_init, &slope_prev_tex)) ||
        FAILED(device->CreateShaderResourceView(slope_prev_tex.Get(), &view_desc, &slope_prev_srv))) {
        return 56;
    }
    // For z = 0.025 / 0.015 = 1.66667m, scaleX = 1, width = 4: dx = 0.5 * z / (0.5 * 4) = 0.416667m shifts priorPixel.x by +0.5.
    auto slopeNow = walkPrev;
    slopeNow.worldFromView[3] = (0.025f / 0.015f) * 0.25f;
    if (!pass.render(slope_cur_srv.Get(), slopeNow, walkPrev, 4, 4,
            0.0f, 0.0f, 0.0f, 0.0f, false, slope_prev_srv.Get(),
            /*preserveContextState=*/true, /*zeroTranslation=*/false,
            /*enableClassOutput=*/true)) return 57;
    context->CopyResource(cls_rb.Get(), pass.pixelClassTexture());
    if (FAILED(context->Map(cls_rb.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 58;
    const std::uint8_t slope_cls = (static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch)[1];
    context->Unmap(cls_rb.Get(), 0);
    if (slope_cls != static_cast<std::uint8_t>(edpe::MotionPixelClass::World)) return 59;

    // Offscreen FG vector preservation & fp16 clamp:
    // Large lateral shift moves priorPixel well outside [0, 4) -> MV must stay physical (not zero),
    // finite, clamped to <= 65504, and tagged OffscreenFG.
    auto offscreenNow = walkPrev;
    offscreenNow.worldFromView[3] = 20.0f; // at z = 10m, priorPixel.x = 1.5 + 4.0 = 5.5 (offscreen), mv.x = +4.0
    if (!pass.render(depth_view.Get(), offscreenNow, walkPrev, 4, 4,
            0.0f, 0.0f, 0.0f, 0.0f, false, nullptr,
            /*preserveContextState=*/true, /*zeroTranslation=*/false,
            /*enableClassOutput=*/true)) return 60;
    context->CopyResource(readback.Get(), pass.outputTexture());
    context->CopyResource(cls_rb.Get(), pass.pixelClassTexture());
    if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 61;
    const auto* off_row1 = reinterpret_cast<const std::uint16_t*>(
        static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch);
    const float off_mv_x = DirectX::PackedVector::XMConvertHalfToFloat(off_row1[2]);
    context->Unmap(readback.Get(), 0);
    if (!std::isfinite(off_mv_x) || std::fabs(off_mv_x - 4.0f) >= 0.05f) return 62;
    if (FAILED(context->Map(cls_rb.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 63;
    const std::uint8_t off_cls = (static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch)[1];
    context->Unmap(cls_rb.Get(), 0);
    if (off_cls != static_cast<std::uint8_t>(edpe::MotionPixelClass::OffscreenFG)) return 64;

    // Extreme huge translation must clamp cleanly to fp16 max (65504), never producing Inf/NaN.
    auto hugeNow = walkPrev;
    hugeNow.worldFromView[3] = 1e7f;
    if (!pass.render(depth_view.Get(), hugeNow, walkPrev, 4, 4)) return 65;
    context->CopyResource(readback.Get(), pass.outputTexture());
    if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 66;
    const auto* huge_row1 = reinterpret_cast<const std::uint16_t*>(
        static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch);
    const float huge_mv_x = DirectX::PackedVector::XMConvertHalfToFloat(huge_row1[2]);
    context->Unmap(readback.Get(), 0);
    if (!std::isfinite(huge_mv_x) || std::fabs(huge_mv_x - 65504.0f) >= 64.0f) return 67;

    // Floating-origin re-anchor (zeroTranslation = true): 5000m jump with zero rotation -> 0 MV.
    auto rebaseNow = walkPrev;
    rebaseNow.worldFromView[3] = 5000.0f;
    if (!pass.render(depth_view.Get(), rebaseNow, walkPrev, 4, 4,
            0.0f, 0.0f, 0.0f, 0.0f, false, nullptr,
            /*preserveContextState=*/true, /*zeroTranslation=*/true)) return 68;
    context->CopyResource(readback.Get(), pass.outputTexture());
    if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 69;
    const auto* reb_row1 = reinterpret_cast<const std::uint16_t*>(
        static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch);
    const float reb_mv_x = DirectX::PackedVector::XMConvertHalfToFloat(reb_row1[2]);
    const float reb_mv_y = DirectX::PackedVector::XMConvertHalfToFloat(reb_row1[3]);
    context->Unmap(readback.Get(), 0);
    if (std::fabs(reb_mv_x) >= 1e-4f || std::fabs(reb_mv_y) >= 1e-4f) return 70;

    // Stage 2.1 GPU test: stencil 0x10 pixel covered by valid WeaponMotion overrides
    // OutMotion, rescales OutDepth to canonicalDepth, and classifies pixel as Weapon (4).
    std::array<std::uint64_t, 16> st_depths{};
    st_depths.fill(std::bit_cast<std::uint32_t>(0.0025f));
    // Pixel (1,1): depth=0.27f, stencil=0x10 (upper 32-bit word low byte in R32G8X24_TYPELESS).
    st_depths[5] = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(0.27f)) |
                   (static_cast<std::uint64_t>(0x10u) << 32);
    D3D11_SUBRESOURCE_DATA st_init{st_depths.data(), 4 * sizeof(std::uint64_t), 0};
    ComPtr<ID3D11Texture2D> st_depth_tex;
    ComPtr<ID3D11ShaderResourceView> st_depth_srv;
    if (FAILED(device->CreateTexture2D(&vm_depth_desc, &st_init, &st_depth_tex)) ||
        FAILED(device->CreateShaderResourceView(st_depth_tex.Get(), &view_desc, &st_depth_srv))) return 71;

    std::array<std::uint16_t, 16 * 4> wm_pixels{};
    wm_pixels[5 * 4 + 0] = DirectX::PackedVector::XMConvertFloatToHalf(3.25f);
    wm_pixels[5 * 4 + 1] = DirectX::PackedVector::XMConvertFloatToHalf(-1.5f);
    wm_pixels[5 * 4 + 2] = DirectX::PackedVector::XMConvertFloatToHalf(0.10f); // canonicalDepth
    wm_pixels[5 * 4 + 3] = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);  // valid
    D3D11_TEXTURE2D_DESC wm_desc{};
    wm_desc.Width = wm_desc.Height = 4;
    wm_desc.MipLevels = wm_desc.ArraySize = 1;
    wm_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    wm_desc.SampleDesc.Count = 1;
    wm_desc.Usage = D3D11_USAGE_DEFAULT;
    wm_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA wm_sub{wm_pixels.data(), 4 * 4 * sizeof(std::uint16_t), 0};
    ComPtr<ID3D11Texture2D> wm_tex;
    ComPtr<ID3D11ShaderResourceView> wm_srv;
    if (FAILED(device->CreateTexture2D(&wm_desc, &wm_sub, &wm_tex)) ||
        FAILED(device->CreateShaderResourceView(wm_tex.Get(), nullptr, &wm_srv))) return 72;

    if (!pass.render(st_depth_srv.Get(), walkPrev, walkPrev, 4, 4,
            0.0f, 0.0f, 0.0f, 0.0f, false, nullptr,
            /*preserveContextState=*/true, /*zeroTranslation=*/false,
            /*enableClassOutput=*/true, wm_srv.Get(), nullptr)) return 73;
    context->CopyResource(readback.Get(), pass.outputTexture());
    context->CopyResource(depth_rb.Get(), pass.canonicalDepthTexture());
    context->CopyResource(cls_rb.Get(), pass.pixelClassTexture());
    if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 74;
    const auto* wm_row1 = reinterpret_cast<const std::uint16_t*>(
        static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch);
    const float w_out_x = DirectX::PackedVector::XMConvertHalfToFloat(wm_row1[2]);
    const float w_out_y = DirectX::PackedVector::XMConvertHalfToFloat(wm_row1[3]);
    context->Unmap(readback.Get(), 0);
    if (std::fabs(w_out_x - 3.25f) >= 0.02f || std::fabs(w_out_y - (-1.5f)) >= 0.02f) return 75;
    if (FAILED(context->Map(depth_rb.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 76;
    const float w_out_d = reinterpret_cast<const float*>(
        static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch)[1];
    context->Unmap(depth_rb.Get(), 0);
    if (std::fabs(w_out_d - 0.10f) >= 1e-3f) return 77;
    if (FAILED(context->Map(cls_rb.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 78;
    const std::uint8_t w_out_cls = (static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch)[1];
    context->Unmap(cls_rb.Get(), 0);
    if (w_out_cls != static_cast<std::uint8_t>(edpe::MotionPixelClass::Weapon)) return 79;

    // Stage 2.2 GPU test: Supercruise CelestialRecord overrides world reprojection
    // for pixels inside the body's screen box, depth span, and radial shell.
    const float celCur[12] = {1,0,0,0, 0,1,0,0, 0,0,1,10.0f};
    const float celPrev[12] = {1,0,0,5.0f, 0,1,0,0, 0,0,1,10.0f}; // +5m X shift -> +1.0px at z=10m
    edpe::CelestialRecord celRec = edpe::buildCelestialRecord(
        celCur, celPrev, 0.0f, 0.0f, 4.0f, 4.0f, 0.001f, 0.01f, 1.5f, 1.5f, 0.0f, 4.0f);
    D3D11_BUFFER_DESC cel_desc{};
    cel_desc.ByteWidth = sizeof(edpe::CelestialRecord);
    cel_desc.Usage = D3D11_USAGE_DEFAULT;
    cel_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cel_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    cel_desc.StructureByteStride = sizeof(edpe::CelestialRecord);
    D3D11_SUBRESOURCE_DATA cel_init{&celRec, 0, 0};
    ComPtr<ID3D11Buffer> cel_buf;
    ComPtr<ID3D11ShaderResourceView> cel_srv;
    D3D11_SHADER_RESOURCE_VIEW_DESC cel_srv_desc{};
    cel_srv_desc.Format = DXGI_FORMAT_UNKNOWN;
    cel_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    cel_srv_desc.Buffer.NumElements = 1;
    if (FAILED(device->CreateBuffer(&cel_desc, &cel_init, &cel_buf)) ||
        FAILED(device->CreateShaderResourceView(cel_buf.Get(), &cel_srv_desc, &cel_srv))) return 80;

    if (!pass.render(depth_view.Get(), walkPrev, walkPrev, 4, 4,
            0.0f, 0.0f, 0.0f, 0.0f, false, nullptr,
            /*preserveContextState=*/true, /*zeroTranslation=*/false,
            /*enableClassOutput=*/true, nullptr, nullptr, cel_srv.Get(), 1u)) return 81;
    context->CopyResource(readback.Get(), pass.outputTexture());
    context->CopyResource(cls_rb.Get(), pass.pixelClassTexture());
    if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 82;
    const auto* cel_row1 = reinterpret_cast<const std::uint16_t*>(
        static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch);
    const float cel_mv_x = DirectX::PackedVector::XMConvertHalfToFloat(cel_row1[2]);
    context->Unmap(readback.Get(), 0);
    if (std::fabs(cel_mv_x - 1.0f) >= 0.05f) return 83;
    if (FAILED(context->Map(cls_rb.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 84;
    const std::uint8_t cel_cls = (static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch)[1];
    context->Unmap(cls_rb.Get(), 0);
    if (cel_cls != static_cast<std::uint8_t>(edpe::MotionPixelClass::Celestial)) return 85;

    // Ship cockpit/hull stencil mask (0x80): pixel with 0x95 at z=10m clamps to (0,0) CockpitStatic
    // even without prevDepth, while neighboring world pixel with 0x05 preserves world MV.
    std::array<std::uint64_t, 16> ship_st_depths{};
    ship_st_depths.fill(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(0.0025f)) |
                        (static_cast<std::uint64_t>(0x05u) << 32)); // hangar wall (z=10m, stencil=0x05)
    ship_st_depths[5] = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(0.0025f)) |
                        (static_cast<std::uint64_t>(0x95u) << 32); // ship nose (z=10m, stencil=0x95)
    D3D11_SUBRESOURCE_DATA ship_st_init{ship_st_depths.data(), 4 * sizeof(std::uint64_t), 0};
    ComPtr<ID3D11Texture2D> ship_st_tex;
    ComPtr<ID3D11ShaderResourceView> ship_st_srv;
    if (FAILED(device->CreateTexture2D(&vm_depth_desc, &ship_st_init, &ship_st_tex)) ||
        FAILED(device->CreateShaderResourceView(ship_st_tex.Get(), &view_desc, &ship_st_srv))) return 86;

    if (!pass.render(ship_st_srv.Get(), walkNow, walkPrev, 4, 4,
            0.0f, 0.0f, 0.0f, 0.0f, false, nullptr,
            /*preserveContextState=*/true, /*zeroTranslation=*/false,
            /*enableClassOutput=*/true, nullptr, nullptr, nullptr, 0u,
            /*clampCockpit=*/true)) return 87;
    context->CopyResource(readback.Get(), pass.outputTexture());
    context->CopyResource(cls_rb.Get(), pass.pixelClassTexture());
    if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 88;
    const auto* ship_row1 = reinterpret_cast<const std::uint16_t*>(
        static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch);
    const float ship_nose_x = DirectX::PackedVector::XMConvertHalfToFloat(ship_row1[2]);
    const float ship_nose_y = DirectX::PackedVector::XMConvertHalfToFloat(ship_row1[3]);
    const float ship_wall_x = DirectX::PackedVector::XMConvertHalfToFloat(ship_row1[4]);
    context->Unmap(readback.Get(), 0);
    if (std::fabs(ship_nose_x) >= 1e-4f || std::fabs(ship_nose_y) >= 1e-4f) return 89;
    if (std::fabs(ship_wall_x) < 0.1f) return 90;
    if (FAILED(context->Map(cls_rb.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 91;
    const auto* ship_cls_row1 = static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch;
    const std::uint8_t ship_nose_cls = ship_cls_row1[1];
    const std::uint8_t ship_wall_cls = ship_cls_row1[2];
    context->Unmap(cls_rb.Get(), 0);
    if (ship_nose_cls != static_cast<std::uint8_t>(edpe::MotionPixelClass::CockpitStatic)) return 92;
    if (ship_wall_cls != static_cast<std::uint8_t>(edpe::MotionPixelClass::World)) return 93;

    // On-foot weapon fallback (clampCockpit=false, weaponMotion=nullptr):
    // stencil 0x14 barrel and un-stenciled z<0.60m muzzle clamp to (0,0) Weapon,
    // while ground (z=2.74m, stencil=0x04) preserves world motion.
    std::array<std::uint64_t, 16> foot_depths{};
    foot_depths.fill(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(0.00912f)) |
                     (static_cast<std::uint64_t>(0x04u) << 32)); // ground (z=2.74m, stencil=0x04)
    foot_depths[5] = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(0.06485f)) |
                     (static_cast<std::uint64_t>(0x14u) << 32); // barrel (z=0.385m, stencil=0x14)
    foot_depths[6] = static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(0.0625f)) |
                     (static_cast<std::uint64_t>(0x04u) << 32); // un-stenciled muzzle (z=0.40m, stencil=0x04)
    D3D11_SUBRESOURCE_DATA foot_init{foot_depths.data(), 4 * sizeof(std::uint64_t), 0};
    ComPtr<ID3D11Texture2D> foot_tex;
    ComPtr<ID3D11ShaderResourceView> foot_srv;
    if (FAILED(device->CreateTexture2D(&vm_depth_desc, &foot_init, &foot_tex)) ||
        FAILED(device->CreateShaderResourceView(foot_tex.Get(), &view_desc, &foot_srv))) return 94;

    if (!pass.render(foot_srv.Get(), walkNow, walkPrev, 4, 4,
            0.0f, 0.0f, 0.0f, 0.0f, false, nullptr,
            /*preserveContextState=*/true, /*zeroTranslation=*/false,
            /*enableClassOutput=*/true, nullptr, nullptr, nullptr, 0u,
            /*clampCockpit=*/false)) return 95;
    context->CopyResource(readback.Get(), pass.outputTexture());
    context->CopyResource(cls_rb.Get(), pass.pixelClassTexture());
    if (FAILED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 96;
    const auto* foot_row1 = reinterpret_cast<const std::uint16_t*>(
        static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch);
    const float barrel_x = DirectX::PackedVector::XMConvertHalfToFloat(foot_row1[2]);
    const float barrel_y = DirectX::PackedVector::XMConvertHalfToFloat(foot_row1[3]);
    const float muzzle_x = DirectX::PackedVector::XMConvertHalfToFloat(foot_row1[4]);
    const float muzzle_y = DirectX::PackedVector::XMConvertHalfToFloat(foot_row1[5]);
    const float ground_x = DirectX::PackedVector::XMConvertHalfToFloat(foot_row1[6]);
    context->Unmap(readback.Get(), 0);
    if (std::fabs(barrel_x) >= 1e-4f || std::fabs(barrel_y) >= 1e-4f) return 97;
    if (std::fabs(muzzle_x) >= 1e-4f || std::fabs(muzzle_y) >= 1e-4f) return 98;
    if (std::fabs(ground_x) < 0.1f) return 99;
    if (FAILED(context->Map(cls_rb.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 100;
    const auto* foot_cls_row1 = static_cast<const std::uint8_t*>(mapped.pData) + mapped.RowPitch;
    const std::uint8_t barrel_cls = foot_cls_row1[1];
    const std::uint8_t muzzle_cls = foot_cls_row1[2];
    const std::uint8_t ground_cls = foot_cls_row1[3];
    context->Unmap(cls_rb.Get(), 0);
    if (barrel_cls != static_cast<std::uint8_t>(edpe::MotionPixelClass::Weapon)) return 101;
    if (muzzle_cls != static_cast<std::uint8_t>(edpe::MotionPixelClass::Weapon)) return 102;
    if (ground_cls == static_cast<std::uint8_t>(edpe::MotionPixelClass::Weapon)) return 103;

    return 0;
}
