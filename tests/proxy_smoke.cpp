#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <cstring>
#include <cstdio>

int forwarded_keys = 0;
int forwarded_insert = 0;

LRESULT CALLBACK testWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_KEYDOWN && wparam == 'A') ++forwarded_keys;
    if (message == WM_KEYDOWN && wparam == VK_INSERT) ++forwarded_insert;
    return DefWindowProcW(window, message, wparam, lparam);
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) return 1;
    const HANDLE stale_log = CreateFileW(argv[3], GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (stale_log == INVALID_HANDLE_VALUE) return 11;
    constexpr char stale_line[] = "OLD_SESSION\n";
    DWORD seeded = 0;
    const BOOL seed_ok = WriteFile(stale_log, stale_line, sizeof(stale_line) - 1, &seeded, nullptr);
    CloseHandle(stale_log);
    if (!seed_ok || seeded != sizeof(stale_line) - 1) return 11;
    const HMODULE dxgi_proxy = LoadLibraryW(argv[2]);
    if (!dxgi_proxy) return 5;
    const HMODULE proxy = LoadLibraryW(argv[1]);
    if (!proxy) return 2;
    const auto create_device = reinterpret_cast<decltype(&D3D11CreateDevice)>(
        GetProcAddress(proxy, "D3D11CreateDevice"));
    if (!create_device) return 3;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const HRESULT result = create_device(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    FreeLibrary(proxy);
    if (FAILED(result) || !device || !context) return 4;
    void* original_om_set = (*reinterpret_cast<void***>(context))[33];

    const auto create_factory = reinterpret_cast<decltype(&CreateDXGIFactory1)>(
        GetProcAddress(dxgi_proxy, "CreateDXGIFactory1"));
    if (!create_factory) return 6;
    const auto create_factory2 = reinterpret_cast<decltype(&CreateDXGIFactory2)>(
        GetProcAddress(dxgi_proxy, "CreateDXGIFactory2"));
    if (!create_factory2) return 6;
    const auto present_count = reinterpret_cast<unsigned long long (WINAPI*)()>(
        GetProcAddress(dxgi_proxy, "EDPE_ObservedPresentCount"));
    if (!present_count) return 7;
    const auto menu_visible = reinterpret_cast<BOOL (WINAPI*)()>(
        GetProcAddress(dxgi_proxy, "EDPE_MenuVisible"));
    if (!menu_visible) return 7;
    const auto request_bind_sequence = reinterpret_cast<BOOL (WINAPI*)()>(
        GetProcAddress(dxgi_proxy, "EDPE_RequestBindSequence"));
    if (!request_bind_sequence) return 7;
    const auto request_depth_snapshot = reinterpret_cast<BOOL (WINAPI*)(UINT)>(
        GetProcAddress(dxgi_proxy, "EDPE_RequestDepthSnapshot"));
    if (!request_depth_snapshot) return 7;
    const auto request_camera_pair = reinterpret_cast<BOOL (WINAPI*)(UINT)>(
        GetProcAddress(dxgi_proxy, "EDPE_RequestCameraPair"));
    if (!request_camera_pair) return 7;
    const auto request_motion_pair = reinterpret_cast<BOOL (WINAPI*)(UINT)>(
        GetProcAddress(dxgi_proxy, "EDPE_RequestMotionPair"));
    if (!request_motion_pair) return 7;
    const auto scene_depth_candidate = reinterpret_cast<int (WINAPI*)()>(
        GetProcAddress(dxgi_proxy, "EDPE_SceneDepthCandidate"));
    if (!scene_depth_candidate) return 7;
    IDXGIFactory1* factory = nullptr;
    const HRESULT factory_result = create_factory(__uuidof(IDXGIFactory1),
        reinterpret_cast<void**>(&factory));
    if (FAILED(factory_result) || !factory) return 8;
    IDXGIFactory2* factory2 = nullptr;
    const HRESULT factory2_result = create_factory2(0, __uuidof(IDXGIFactory2),
        reinterpret_cast<void**>(&factory2));
    if (FAILED(factory2_result) || !factory2) return 8;
    factory2->Release();
    IDXGIFactory7* newer_factory = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory7),
            reinterpret_cast<void**>(&newer_factory)))) {
        newer_factory->IsCurrent();
        newer_factory->Release();
    }

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = testWndProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"EDPE_SMOKE_TEST";
    if (!RegisterClassW(&window_class)) return 9;
    const HWND window = CreateWindowExW(0, window_class.lpszClassName, L"EDPE test", WS_OVERLAPPEDWINDOW,
        0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 9;
    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = 64;
    desc.BufferDesc.Height = 64;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.OutputWindow = window;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* swap_chain = nullptr;
    const HRESULT swap_result = factory->CreateSwapChain(device, &desc, &swap_chain);
    if (FAILED(swap_result) || !swap_chain) return 10;
    D3D11_TEXTURE2D_DESC depth_desc{};
    depth_desc.Width = 64;
    depth_desc.Height = 64;
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* depth_texture = nullptr;
    if (FAILED(device->CreateTexture2D(&depth_desc, nullptr, &depth_texture))) return 10;
    ID3D11DepthStencilView* depth_view = nullptr;
    D3D11_DEPTH_STENCIL_VIEW_DESC depth_view_desc{};
    depth_view_desc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    depth_view_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(device->CreateDepthStencilView(depth_texture, &depth_view_desc, &depth_view))) return 10;
    D3D11_TEXTURE2D_DESC color_desc = depth_desc;
    color_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* color_texture = nullptr;
    if (FAILED(device->CreateTexture2D(&color_desc, nullptr, &color_texture))) return 10;
    ID3D11RenderTargetView* color_view = nullptr;
    if (FAILED(device->CreateRenderTargetView(color_texture, nullptr, &color_view))) return 10;
    color_desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
    ID3D11Texture2D* second_color_texture = nullptr;
    if (FAILED(device->CreateTexture2D(&color_desc, nullptr, &second_color_texture))) return 10;
    ID3D11RenderTargetView* second_color_view = nullptr;
    if (FAILED(device->CreateRenderTargetView(second_color_texture, nullptr, &second_color_view))) return 10;
    color_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    ID3D11Texture2D* mrt0_texture = nullptr;
    if (FAILED(device->CreateTexture2D(&color_desc, nullptr, &mrt0_texture))) return 10;
    ID3D11RenderTargetView* mrt0_view = nullptr;
    if (FAILED(device->CreateRenderTargetView(mrt0_texture, nullptr, &mrt0_view))) return 10;
    color_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ID3D11Texture2D* mrt2_texture = nullptr;
    if (FAILED(device->CreateTexture2D(&color_desc, nullptr, &mrt2_texture))) return 10;
    ID3D11RenderTargetView* mrt2_view = nullptr;
    if (FAILED(device->CreateRenderTargetView(mrt2_texture, nullptr, &mrt2_view))) return 10;
    const float clear_color[4]{0.25f, 0.5f, 0.75f, 1.0f};
    context->ClearRenderTargetView(second_color_view, clear_color);
    D3D11_BUFFER_DESC probe_desc{};
    probe_desc.ByteWidth = 5376;
    probe_desc.Usage = D3D11_USAGE_DYNAMIC;
    probe_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    probe_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    float probe_values[5376 / sizeof(float)]{};
    probe_values[795] = 0.025f;
    probe_values[1094] = 0.025f;
    probe_values[1080] = 1.25f;
    probe_values[932] = probe_values[937] = probe_values[942] = 1.0f;
    D3D11_SUBRESOURCE_DATA probe_initial{};
    probe_initial.pSysMem = probe_values;
    ID3D11Buffer* probe_buffer = nullptr;
    if (FAILED(device->CreateBuffer(&probe_desc, &probe_initial, &probe_buffer))) return 10;
    context->VSSetConstantBuffers(1, 1, &probe_buffer);
    const float depth_pass_words[12]{1,2,3,4,5,6,7,8,9,10,11,12};
    D3D11_BUFFER_DESC depth_pass_desc = probe_desc;
    depth_pass_desc.ByteWidth = sizeof(depth_pass_words);
    D3D11_SUBRESOURCE_DATA depth_pass_initial{};
    depth_pass_initial.pSysMem = depth_pass_words;
    ID3D11Buffer* depth_pass_buffer = nullptr;
    if (FAILED(device->CreateBuffer(&depth_pass_desc, &depth_pass_initial,
            &depth_pass_buffer))) return 10;
    context->VSSetConstantBuffers(2, 1, &depth_pass_buffer);
    context->OMSetRenderTargets(0, nullptr, depth_view);
    IDXGISwapChain4* newer_swap_chain = nullptr;
    if (SUCCEEDED(swap_chain->QueryInterface(__uuidof(IDXGISwapChain4),
            reinterpret_cast<void**>(&newer_swap_chain)))) {
        newer_swap_chain->GetCurrentBackBufferIndex();
        newer_swap_chain->Release();
    }
    const HRESULT present_result = swap_chain->Present(0, DXGI_PRESENT_TEST);
    const HRESULT second_present_result = swap_chain->Present(0, DXGI_PRESENT_TEST);
    const bool observed = present_count() == 2;
    const HRESULT first_real_present = swap_chain->Present(0, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->OMSetRenderTargets(0, nullptr, depth_view);
    context->OMSetRenderTargets(1, &color_view, depth_view);
    SendMessageW(window, WM_KEYDOWN, 'A', 0);
    const bool hidden_input_passed = forwarded_keys == 1;
    SendMessageW(window, WM_KEYDOWN, VK_INSERT, 0);
    const bool insert_passed = forwarded_insert == 1 && !menu_visible();
    SendMessageW(window, WM_KEYDOWN, VK_F5, 0);
    const bool opened = menu_visible();
    SendMessageW(window, WM_KEYDOWN, VK_F5, 1LL << 30);
    const bool f5_repeat_ignored = menu_visible();
    const HRESULT overlay_present = swap_chain->Present(0, 0);
    const bool sequence_requested = request_bind_sequence();
    const HRESULT arm_present = swap_chain->Present(0, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->OMSetRenderTargets(0, nullptr, depth_view);
    const HRESULT sequence_present = swap_chain->Present(0, 0);
    const bool snapshot_requested = request_depth_snapshot(0);
    const HRESULT snapshot_arm_present = swap_chain->Present(0, 0);
    context->ClearDepthStencilView(depth_view, D3D11_CLEAR_DEPTH, 0.25f, 0);
    context->OMSetRenderTargets(0, nullptr, depth_view);
    context->OMSetRenderTargets(0, nullptr, depth_view);
    context->VSSetConstantBuffers(1, 1, &probe_buffer);
    for (int i = 0; i < 50; ++i) context->VSSetConstantBuffers(1, 1, &probe_buffer);
    ID3D11RenderTargetView* scene_mrt[]{mrt0_view, color_view, mrt2_view, second_color_view};
    context->OMSetRenderTargets(4, scene_mrt, depth_view);
    constexpr char vertex_source[] =
        "float4 main(uint id : SV_VertexID) : SV_Position { "
        "return float4(id == 1 ? 1 : -1, id == 2 ? 1 : -1, 0, 1); }";
    ID3DBlob* vertex_bytecode = nullptr;
    if (FAILED(D3DCompile(vertex_source, sizeof(vertex_source) - 1, nullptr, nullptr,
            nullptr, "main", "vs_5_0", 0, 0, &vertex_bytecode, nullptr))) return 10;
    ID3D11VertexShader* vertex_shader = nullptr;
    const HRESULT vertex_result = device->CreateVertexShader(vertex_bytecode->GetBufferPointer(),
        vertex_bytecode->GetBufferSize(), nullptr, &vertex_shader);
    vertex_bytecode->Release();
    if (FAILED(vertex_result)) return 10;
    context->VSSetShader(vertex_shader, nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->Draw(3, 0);
    context->VSSetShader(nullptr, nullptr, 0);
    vertex_shader->Release();
    D3D11_MAPPED_SUBRESOURCE middle_probe{};
    if (FAILED(context->Map(probe_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &middle_probe))) return 10;
    probe_values[1080] = 1.75f;
    std::memcpy(middle_probe.pData, probe_values, sizeof(probe_values));
    context->Unmap(probe_buffer, 0);
    context->OMSetRenderTargets(4, scene_mrt, depth_view);
    context->OMSetRenderTargets(0, nullptr, depth_view);
    D3D11_MAPPED_SUBRESOURCE updated_probe{};
    if (FAILED(context->Map(probe_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &updated_probe))) return 10;
    probe_values[795] = 0.05f;
    probe_values[935] = 42.0f;
    probe_values[1094] = 0.05f;
    probe_values[1080] = 2.5f;
    std::memcpy(updated_probe.pData, probe_values, sizeof(probe_values));
    context->Unmap(probe_buffer, 0);
    context->OMSetRenderTargets(1, &second_color_view, depth_view);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    const HRESULT snapshot_present = swap_chain->Present(0, 0);
    const bool scene_candidate_found = scene_depth_candidate() == 0;
    const bool pair_requested = request_camera_pair(0);
    const HRESULT pair_arm_present = swap_chain->Present(0, 0);
    HRESULT pair_present = S_OK;
    for (int frame = 0; frame < 2; ++frame) {
        context->ClearDepthStencilView(depth_view, D3D11_CLEAR_DEPTH,
            frame ? 0.5f : 0.25f, 0);
        for (int bind = 0; bind < 3; ++bind)
            context->OMSetRenderTargets(4, scene_mrt, depth_view);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        pair_present = swap_chain->Present(0, 0);
        if (FAILED(pair_present)) break;
    }
    for (int i = 0; i < 4; ++i) swap_chain->Present(0, 0);
    probe_values[935] = 0;
    probe_values[1080] = probe_values[1085] = probe_values[1091] = 1;
    probe_values[1094] = .025f;
    D3D11_MAPPED_SUBRESOURCE motion_probe{};
    if (FAILED(context->Map(probe_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0,
            &motion_probe))) return 10;
    std::memcpy(motion_probe.pData, probe_values, sizeof(probe_values));
    context->Unmap(probe_buffer, 0);
    const bool motion_requested = request_motion_pair(0);
    const HRESULT motion_arm_present = swap_chain->Present(0, 0);
    HRESULT motion_present = S_OK;
    for (int frame = 0; frame < 2; ++frame) {
        if (frame) {
            probe_values[935] = .01f;
            if (FAILED(context->Map(probe_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0,
                    &motion_probe))) return 10;
            std::memcpy(motion_probe.pData, probe_values, sizeof(probe_values));
            context->Unmap(probe_buffer, 0);
        }
        context->ClearDepthStencilView(depth_view, D3D11_CLEAR_DEPTH, .5f, 0);
        for (int bind = 0; bind < 3; ++bind)
            context->OMSetRenderTargets(4, scene_mrt, depth_view);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        motion_present = swap_chain->Present(0, 0);
        if (FAILED(motion_present)) break;
    }
    for (int i = 0; i < 8; ++i) swap_chain->Present(0, 0);
    SendMessageW(window, WM_KEYDOWN, 'A', 0);
    const bool visible_input_blocked = forwarded_keys == 1;
    const HRESULT resize_result = swap_chain->ResizeBuffers(0, 128, 128, DXGI_FORMAT_UNKNOWN, 0);
    const HRESULT resized_present = SUCCEEDED(resize_result) ? swap_chain->Present(0, 0) : resize_result;
    for (int i = 0; i < 16; ++i) swap_chain->Present(0, 0);
    while (present_count() < 1024) swap_chain->Present(0, DXGI_PRESENT_TEST);
    const bool interval_observed = present_count() == 1024;
    SendMessageW(window, WM_KEYUP, VK_F5, 0);
    SendMessageW(window, WM_KEYDOWN, VK_F5, 0);
    const bool closed = !menu_visible();
    SendMessageW(window, WM_KEYUP, VK_F5, 0);
    SendMessageW(window, WM_KEYDOWN, 'A', 0);
    const bool hidden_input_restored = forwarded_keys == 2;
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11Buffer* no_buffer = nullptr;
    context->VSSetConstantBuffers(1, 1, &no_buffer);
    context->VSSetConstantBuffers(2, 1, &no_buffer);
    depth_pass_buffer->Release();
    probe_buffer->Release();
    color_view->Release();
    color_texture->Release();
    second_color_view->Release();
    second_color_texture->Release();
    mrt0_view->Release();
    mrt0_texture->Release();
    mrt2_view->Release();
    mrt2_texture->Release();
    depth_view->Release();
    depth_texture->Release();
    swap_chain->Release();
    const bool context_hook_restored =
        (*reinterpret_cast<void***>(context))[33] == original_om_set;
    if (factory) factory->Release();
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    context->Release();
    device->Release();
    FreeLibrary(dxgi_proxy);
    const HANDLE log = CreateFileW(argv[3], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) return 11;
    char contents[32768]{};
    DWORD bytes_read = 0;
    const BOOL read = ReadFile(log, contents, sizeof(contents) - 1, &bytes_read, nullptr);
    CloseHandle(log);
    const bool passed = SUCCEEDED(present_result) && SUCCEEDED(second_present_result) && observed &&
        SUCCEEDED(first_real_present) && SUCCEEDED(overlay_present) &&
        SUCCEEDED(arm_present) && SUCCEEDED(sequence_present) && sequence_requested &&
        SUCCEEDED(snapshot_arm_present) && SUCCEEDED(snapshot_present) && snapshot_requested &&
        scene_candidate_found && pair_requested && SUCCEEDED(pair_arm_present) &&
        SUCCEEDED(pair_present) && motion_requested &&
        SUCCEEDED(motion_arm_present) && SUCCEEDED(motion_present) &&
        SUCCEEDED(resize_result) && SUCCEEDED(resized_present) && opened && closed &&
        insert_passed && f5_repeat_ignored &&
        hidden_input_passed && visible_input_blocked && hidden_input_restored &&
        context_hook_restored &&
        interval_observed &&
        read && !std::strstr(contents, "OLD_SESSION") &&
        std::strstr(contents, "EDPE: D3D11 device created") &&
        std::strstr(contents, "EDPE: DXGI factory created") &&
        std::strstr(contents, "EDPE: Present swapchain=") &&
        std::strstr(contents, "EDPE: Present bindings") &&
        std::strstr(contents, "EDPE: context dispatch frame=1") &&
        std::strstr(contents, "EDPE: context dispatch frame=8") &&
        std::strstr(contents, "slot12=") && std::strstr(contents, "slot53=") &&
        std::strstr(contents, "EDPE: OMSetRenderTargets DSV census armed") &&
        std::strstr(contents, "EDPE: DSV bind #0 phase=first view=") &&
        std::strstr(contents, "EDPE: DSV bind #0 phase=first-color view=") &&
        std::strstr(contents, "color=64x64 colorFormat=28 colorBind=0x20") &&
        std::strstr(contents, "EDPE: DSV interval frame=1024 top=0:25") &&
        std::strstr(contents, "EDPE: DSV bind sequence frame=6 transitions=2 stored=2") &&
        std::strstr(contents, "EDPE: DSV bind sequence 0 target=-1") &&
        std::strstr(contents, "EDPE: DSV bind sequence 1 target=0") &&
        std::strstr(contents, "viewFormat=20 textureFormat=19 depth=64x64 bind=0x48") &&
        std::strstr(contents, "EDPE: depth snapshot timing #0 armedAfter=7 firstBindAfter=7 lastBindAfter=7 binds=6 handedAt=8") &&
        std::strstr(contents, "EDPE: DSV #0 bind=1 CB stage=VS slot=1 buffer=") &&
        std::strstr(contents, "EDPE: DSV #0 bind=2 CB stage=VS slot=1 buffer=") &&
        std::strstr(contents, "EDPE: DSV #0 bind=3 color target count=4") &&
        std::strstr(contents, "EDPE: DSV #0 bind=3 rtv0=") &&
        std::strstr(contents, "format=24 size=64x64 bind=0x20") &&
        std::strstr(contents, "EDPE: DSV #0 bind=3 rtv1=") &&
        std::strstr(contents, "EDPE: DSV #0 bind=3 rtv2=") &&
        std::strstr(contents, "EDPE: DSV #0 bind=3 rtv3=") &&
        std::strstr(contents, "format=26 size=64x64 bind=0x20") &&
        std::strstr(contents, "EDPE: DSV #0 bind=6 color target count=1") &&
        std::strstr(contents, "bytes=5376 usage=2 cpu=0x10000") &&
        std::strstr(contents, "EDPE: scene CB sample bind=3 projectionZ=(0,0.025000") &&
        std::strstr(contents, "projection2D=0.025000") &&
        std::strstr(contents, "rows0=(1,0,0,0)") &&
        std::strstr(contents, "EDPE: scene CB 2D xy bind=3 x=(1.25,0,0,0)") &&
        std::strstr(contents, "EDPE: scene CB 2D xy bind=4 x=(1.75,0,0,0)") &&
        std::strstr(contents, "EDPE: scene CB 2D xy bind=6 x=(2.5,0,0,0)") &&
        std::strstr(contents, "EDPE: depth-pass CB bind=2 slot=2 hex=3F8000004000000040400000") &&
        std::strstr(contents, "EDPE: scene CB 2D zw bind=6 z=(0,0,0,0) w=(0,0,0.0500000007,0)") &&
        std::strstr(contents, "EDPE: scene CB sample bind=6 projectionZ=(0,0.050000") &&
        std::strstr(contents, "projection2D=0.050000") &&
        std::strstr(contents, "rows0=(1,0,0,42)") &&
        std::strstr(contents, "EDPE: scene CB rows1 bind=6 (0,1,0,0) rows2=(0,0,1,0)") &&
        std::strstr(contents, "EDPE: scene pipeline afterPresent=7 bind=1 iaPrimitives=0 vsInvocations=0") &&
        std::strstr(contents, "EDPE: scene pipeline afterPresent=7 bind=2 iaPrimitives=0 vsInvocations=0") &&
        std::strstr(contents, "EDPE: depth-pass VS bindings calls=51 sceneBufferCalls=51 first=") &&
        std::strstr(contents, "switches=0 slotRestored=1") &&
        std::strstr(contents, "EDPE: scene CB sample bind=0 projectionZ=(0,0.025000") &&
        std::strstr(contents, "EDPE: scene CB sample bind=51 projectionZ=(0,0.025000") &&
        std::strstr(contents, "EDPE: scene CB hash bind=0 fnv64=") &&
        std::strstr(contents, "EDPE: scene CB hash bind=51 fnv64=A623605C522D5605 camera64=E22643B8B93F6294") &&
        std::strstr(contents, "EDPE: scene CB hash bind=3 fnv64=") &&
        std::strstr(contents, "EDPE: scene pipeline afterPresent=7 bind=3 iaPrimitives=1 vsInvocations=3") &&
        std::strstr(contents, "EDPE: scene pipeline afterPresent=7 bind=6 iaPrimitives=0 vsInvocations=0") &&
        std::strstr(contents, "EDPE: scene depth state afterPresent=7 bind=1 edge=start") &&
        std::strstr(contents, "EDPE: scene depth state afterPresent=7 bind=1 edge=end") &&
        std::strstr(contents, "EDPE: scene depth state afterPresent=7 bind=2 edge=start") &&
        std::strstr(contents, "EDPE: scene depth state afterPresent=7 bind=2 edge=end") &&
        std::strstr(contents, "EDPE: scene depth state afterPresent=7 bind=3 edge=start") &&
        std::strstr(contents, "EDPE: scene depth state afterPresent=7 bind=3 edge=end") &&
        std::strstr(contents, "EDPE: scene depth state afterPresent=7 bind=6 edge=start") &&
        std::strstr(contents, "EDPE: scene depth state afterPresent=7 bind=6 edge=end") &&
        std::strstr(contents, "EDPE: scene CB hex 0000 ") &&
        std::strstr(contents, "EDPE: scene CB hex 1312 ") &&
        std::strstr(contents, "EDPE: depth snapshot #0 copied 64x64 format=19") &&
        std::strstr(contents, "EDPE: scene color snapshot #0 copied 64x64 format=26") &&
        std::strstr(contents, "EDPE: scene color RTV unbound at Present") &&
        std::strstr(contents, "EDPE: scene color snapshot image submitted to ImGui") &&
        std::strstr(contents, "EDPE: depth snapshot image submitted to ImGui") &&
        std::strstr(contents, "EDPE: depth contrast shader active in ImGui") &&
        std::strstr(contents, "EDPE: depth sample grid valid=64 nonzero=64 min=0.25 max=0.25 centerValid=1 center=0.25") &&
        std::strstr(contents, "EDPE: adjacent camera probe armed DSV #0") &&
        std::strstr(contents, "EDPE: scene CB sample bind=100") &&
        std::strstr(contents, "EDPE: scene CB sample bind=101") &&
        std::strstr(contents, "EDPE: adjacent depth afterPresent=") &&
        std::strstr(contents, "centerValid=1 center=0.25 size=64x64") &&
        std::strstr(contents, "centerValid=1 center=0.5 size=64x64") &&
        std::strstr(contents, "EDPE: motion candidate depth retained on GPU") &&
        std::strstr(contents, "EDPE: motion candidate GPU pass completed") &&
        std::strstr(contents, "EDPE: motion candidate currentAfterPresent=17 center=(6.39844,0)") &&
        std::strstr(contents, "pixels finite=1") &&
        std::strstr(contents, "EDPE: Dear ImGui ready") &&
        std::strstr(contents, "EDPE: D3D11 context state available=1") &&
        std::strstr(contents, "EDPE: queued input routed to Dear ImGui") &&
        std::strstr(contents, "vtable=") && std::strstr(contents, "dsvMethod=");
    if (!passed) std::fprintf(stderr,
        "present=%08lx/%08lx real=%08lx overlay=%08lx resize=%08lx/%08lx observed=%d menu=%d/%d insert=%d repeat=%d input=%d/%d/%d read=%d\n",
        present_result, second_present_result, first_real_present, overlay_present,
        resize_result, resized_present, observed, opened, closed,
        insert_passed, f5_repeat_ignored,
        hidden_input_passed, visible_input_blocked, hidden_input_restored, read);
    return passed ? 0 : 12;
}
