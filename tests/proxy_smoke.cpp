#include <d3d11.h>
#include <d3dcompiler.h>
#include "shader_probe.h"
#include <dxgi1_6.h>
#include <windows.h>
#include <cstring>
#include <cstdio>
#include <vector>

int forwarded_keys = 0;
int forwarded_insert = 0;

LRESULT CALLBACK testWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_KEYDOWN && wparam == 'A') ++forwarded_keys;
    if (message == WM_KEYDOWN && wparam == VK_INSERT) ++forwarded_insert;
    return DefWindowProcW(window, message, wparam, lparam);
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && argc != 4) return 1;
    const bool shader_probe = (argc == 4 && wcscmp(argv[3], L"shader-probe") == 0);
    if (argc == 4 && !shader_probe) return 1;

    const wchar_t* dxgi_path = argv[1];
    const wchar_t* log_path = argv[2];

    if (shader_probe) {
        wchar_t marker[MAX_PATH];
        const DWORD length = GetModuleFileNameW(nullptr, marker, MAX_PATH);
        if (!length || length >= MAX_PATH) return 12;
        wchar_t* name = wcsrchr(marker, L'\\');
        if (!name) return 12;
        wcscpy_s(name + 1, MAX_PATH - (name + 1 - marker), L"edpe_shader_probe.once");
        const HANDLE file = CreateFileW(marker, GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return 12;
        CloseHandle(file);
    }
    const HANDLE stale_log = CreateFileW(log_path, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (stale_log == INVALID_HANDLE_VALUE) return 11;
    constexpr char stale_line[] = "OLD_SESSION\n";
    DWORD seeded = 0;
    const BOOL seed_ok = WriteFile(stale_log, stale_line, sizeof(stale_line) - 1, &seeded, nullptr);
    CloseHandle(stale_log);
    if (!seed_ok || seeded != sizeof(stale_line) - 1) return 11;

    // Clean any stale *.bmp files from previous test runs so saved_bmp is deterministic
    wchar_t stale_captures[MAX_PATH]{};
    const DWORD exe_len = GetModuleFileNameW(nullptr, stale_captures, MAX_PATH);
    if (exe_len && exe_len < MAX_PATH) {
        wchar_t* nm = wcsrchr(stale_captures, L'\\');
        if (nm) {
            wcscpy_s(nm + 1, MAX_PATH - (nm + 1 - stale_captures), L"edpe-captures\\*.bmp");
            WIN32_FIND_DATAW ent{};
            HANDLE srch = FindFirstFileW(stale_captures, &ent);
            if (srch != INVALID_HANDLE_VALUE) {
                do {
                    wchar_t file_to_delete[MAX_PATH]{};
                    wcscpy_s(file_to_delete, stale_captures);
                    wchar_t* star = wcsrchr(file_to_delete, L'*');
                    if (star) {
                        *star = L'\0';
                        wcscat_s(file_to_delete, ent.cFileName);
                        DeleteFileW(file_to_delete);
                    }
                } while (FindNextFileW(srch, &ent));
                FindClose(srch);
            }
        }
    }

    auto inspect_bmp = []() -> bool {
        wchar_t capture_path[MAX_PATH]{};
        const DWORD executable_length = GetModuleFileNameW(nullptr, capture_path, MAX_PATH);
        if (!executable_length || executable_length >= MAX_PATH) return false;
        wchar_t* name = wcsrchr(capture_path, L'\\');
        if (!name) return false;
        wcscpy_s(name + 1, MAX_PATH - (name + 1 - capture_path), L"edpe-captures\\*.bmp");
        WIN32_FIND_DATAW entry{};
        const HANDLE search = FindFirstFileW(capture_path, &entry);
        if (search == INVALID_HANDLE_VALUE) return false;
        FindClose(search);
        wcscpy_s(name + 1, MAX_PATH - (name + 1 - capture_path), L"edpe-captures\\");
        wcscat_s(capture_path, entry.cFileName);
        const HANDLE image = CreateFileW(capture_path, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (image == INVALID_HANDLE_VALUE) return false;
        BITMAPFILEHEADER header{};
        BITMAPINFOHEADER info{};
        DWORD count = 0;
        const bool header_read = ReadFile(image, &header, sizeof(header), &count, nullptr) &&
            count == sizeof(header);
        const bool info_read = ReadFile(image, &info, sizeof(info), &count, nullptr) &&
            count == sizeof(info);
        CloseHandle(image);
        return header_read && info_read && header.bfType == 0x4D42 &&
            info.biWidth == 64 && info.biHeight == -64 && info.biBitCount == 32;
    };
    const HMODULE dxgi_proxy = LoadLibraryW(dxgi_path);
    if (!dxgi_proxy) return 5;
    const auto create_device = reinterpret_cast<decltype(&D3D11CreateDevice)>(
        GetProcAddress(dxgi_proxy, "D3D11CreateDevice"));
    if (!create_device) return 3;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const HRESULT result = create_device(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(result) || !device || !context) return 4;
    void* hooked_om_set = (*reinterpret_cast<void***>(context))[33];
    void* real_system_om_set = nullptr;
    HMODULE sys_d3d11 = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (sys_d3d11) {
        auto sys_create = reinterpret_cast<decltype(&D3D11CreateDevice)>(
            GetProcAddress(sys_d3d11, "D3D11CreateDevice"));
        if (sys_create) {
            ID3D11Device* d = nullptr;
            ID3D11DeviceContext* c = nullptr;
            if (SUCCEEDED(sys_create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, nullptr, &c)) && c) {
                real_system_om_set = (*reinterpret_cast<void***>(c))[33];
                c->Release();
                d->Release();
            }
        }
        FreeLibrary(sys_d3d11);
    }
    void** start_table = *reinterpret_cast<void***>(context);
    void* original_draws[]{start_table[12],
        start_table[13],
        start_table[20],
        start_table[21]};

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
    const auto set_jitter_test_override = reinterpret_cast<void (WINAPI*)(BOOL)>(
        GetProcAddress(dxgi_proxy, "EDPE_SetJitterTestOverride"));
    if (!set_jitter_test_override) return 7;
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
    ID3D11RenderTargetView* second_color_alias = nullptr;
    if (FAILED(device->CreateRenderTargetView(second_color_texture, nullptr, &second_color_alias))) return 10;
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
    ID3D11Buffer* no_scene_buffer = nullptr;
    context->VSSetConstantBuffers(1, 1, &no_scene_buffer);
    context->OMSetRenderTargets(0, nullptr, depth_view);
    D3D11_MAPPED_SUBRESOURCE early_probe{};
    if (FAILED(context->Map(probe_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0,
            &early_probe))) return 10;
    std::memcpy(early_probe.pData, probe_values, sizeof(probe_values));
    context->Unmap(probe_buffer, 0);
    context->VSSetConstantBuffers(1, 1, &probe_buffer);
    context->OMSetRenderTargets(0, nullptr, depth_view);
    ID3D11RenderTargetView* scene_mrt[]{mrt0_view, color_view, mrt2_view, second_color_view};
    context->OMSetRenderTargets(4, scene_mrt, depth_view);
    context->VSSetConstantBuffers(1, 1, &probe_buffer);
    for (int i = 0; i < 50; ++i) context->VSSetConstantBuffers(1, 1, &probe_buffer);
    context->DrawIndexed(0, 0, 0); // Exercise the one-shot draw observer without rendering.
    context->Draw(0, 0);
    context->DrawIndexedInstanced(0, 0, 0, 0, 0);
    context->DrawInstanced(0, 0, 0, 0);
    context->VSSetConstantBuffers(1, 1, &depth_pass_buffer);
    context->DrawIndexed(0, 0, 0);
    context->Draw(0, 0);
    context->DrawIndexedInstanced(0, 0, 0, 0, 0);
    context->DrawInstanced(0, 0, 0, 0);
    context->VSSetConstantBuffers(1, 1, &probe_buffer);
    constexpr char vertex_source[] =
        "float4 main(uint id : SV_VertexID) : SV_Position { "
        "return float4(id == 1 ? 1 : -1, id == 2 ? 1 : -1, 0, 1); }";
    ID3DBlob* vertex_bytecode = nullptr;
    if (FAILED(D3DCompile(vertex_source, sizeof(vertex_source) - 1, nullptr, nullptr,
            nullptr, "main", "vs_5_0", 0, 0, &vertex_bytecode, nullptr))) return 10;
    ID3D11VertexShader* vertex_shader = nullptr;
    const HRESULT vertex_result = device->CreateVertexShader(vertex_bytecode->GetBufferPointer(),
        vertex_bytecode->GetBufferSize(), nullptr, &vertex_shader);
    if (SUCCEEDED(vertex_result) && !vertex_shader) return 12;
    if (SUCCEEDED(vertex_result)) {
        std::uint64_t hash = 0;
        UINT bytes = sizeof(hash);
        if (FAILED(vertex_shader->GetPrivateData(kEdpeShaderHashGuid, &bytes, &hash)) ||
            bytes != sizeof(hash) ||
            hash != EdpeEdvrShaderHash(vertex_bytecode->GetBufferPointer(),
                vertex_bytecode->GetBufferSize()) ||
            EdpeEdvrShaderHash("abc", 3) != 0xe16801510db89efdull) return 12;
    }
    if (shader_probe && SUCCEEDED(vertex_result)) {
        unsigned char captured[8192]{};
        UINT bytes = sizeof(captured);
        UINT queried = 0;
        vertex_shader->GetPrivateData(kEdpeVertexBytecodeGuid, &queried, nullptr);
        if (vertex_bytecode->GetBufferSize() > sizeof(captured) ||
            queried != vertex_bytecode->GetBufferSize() ||
            FAILED(vertex_shader->GetPrivateData(kEdpeVertexBytecodeGuid, &bytes, captured)) ||
            bytes != vertex_bytecode->GetBufferSize() ||
            std::memcmp(captured, vertex_bytecode->GetBufferPointer(), bytes) != 0) return 12;
    }
    vertex_bytecode->Release();
    if (FAILED(vertex_result)) return 10;
    context->VSSetShader(vertex_shader, nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->Draw(3, 0);
    context->VSSetShader(nullptr, nullptr, 0);
    vertex_shader->Release();
    {
        constexpr char pixel_source[] =
            "float4 main() : SV_Target { return float4(1, 0, 0, 1); }";
        ID3DBlob* pixel_bytecode = nullptr;
        if (FAILED(D3DCompile(pixel_source, sizeof(pixel_source) - 1, nullptr,
                nullptr, nullptr, "main", "ps_5_0", 0, 0, &pixel_bytecode, nullptr))) return 12;
        ID3D11PixelShader* pixel_shader = nullptr;
        const HRESULT pixel_result = device->CreatePixelShader(
            pixel_bytecode->GetBufferPointer(), pixel_bytecode->GetBufferSize(),
            nullptr, &pixel_shader);
        std::uint64_t hash = 0;
        UINT bytes = sizeof(hash);
        const bool hash_ok = SUCCEEDED(pixel_result) && pixel_shader &&
            SUCCEEDED(pixel_shader->GetPrivateData(kEdpeShaderHashGuid, &bytes, &hash)) &&
            bytes == sizeof(hash) &&
            hash == EdpeEdvrShaderHash(pixel_bytecode->GetBufferPointer(),
                pixel_bytecode->GetBufferSize());
        UINT retained_size = 0;
        if (pixel_shader)
            pixel_shader->GetPrivateData(kEdpePixelBytecodeGuid, &retained_size, nullptr);
        const bool bytes_ok = retained_size == pixel_bytecode->GetBufferSize();
        std::vector<unsigned char> retained(retained_size);
        const bool contents_ok = bytes_ok &&
            SUCCEEDED(pixel_shader->GetPrivateData(kEdpePixelBytecodeGuid,
                &retained_size, retained.data())) &&
            retained_size == pixel_bytecode->GetBufferSize() &&
            std::memcmp(retained.data(), pixel_bytecode->GetBufferPointer(),
                retained_size) == 0;
        if (pixel_shader) pixel_shader->Release();
        pixel_bytecode->Release();
        if (!hash_ok || !contents_ok) return 12;
    }
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
    bool glass_target_restored = false;
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
        if (frame == 1) {
            // Emulate the runtime restoring Draw's vtable slot during a frame.
            void** table = *reinterpret_cast<void***>(context);
            DWORD protection = 0, ignored = 0;
            if (!VirtualProtect(table + 13, sizeof(void*), PAGE_READWRITE, &protection)) return 12;
            InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(table + 13),
                original_draws[1]);
            VirtualProtect(table + 13, sizeof(void*), protection, &ignored);
            ID3D11RenderTargetView* alias_mrt[4]{scene_mrt[0], scene_mrt[1],
                scene_mrt[2], second_color_alias};
            context->OMSetRenderTargets(4, alias_mrt, depth_view);
            // Synthetic EDVR HUD signature exercises the copy before the draw.
            ID3DBlob* vs_code = nullptr;
            ID3DBlob* ps_code = nullptr;
            constexpr char ps_source[] =
                "float4 main() : SV_Target { return float4(1, 0, 0, 1); }";
            if (FAILED(D3DCompile(vertex_source, sizeof(vertex_source) - 1,
                    nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0,
                    &vs_code, nullptr)) ||
                FAILED(D3DCompile(ps_source, sizeof(ps_source) - 1,
                    nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0,
                    &ps_code, nullptr))) return 12;
            ID3D11VertexShader* vs = nullptr;
            ID3D11PixelShader* ps = nullptr;
            if (FAILED(device->CreateVertexShader(vs_code->GetBufferPointer(),
                    vs_code->GetBufferSize(), nullptr, &vs)) ||
                FAILED(device->CreatePixelShader(ps_code->GetBufferPointer(),
                    ps_code->GetBufferSize(), nullptr, &ps))) return 12;
            const std::uint64_t hud_vs = 0xB7790CBFC6554097ull;
            const std::uint64_t hud_ps = 0x8DEF46452FA459F5ull;
            vs->SetPrivateData(kEdpeShaderHashGuid, sizeof(hud_vs), &hud_vs);
            ps->SetPrivateData(kEdpeShaderHashGuid, sizeof(hud_ps), &hud_ps);
            context->VSSetShader(vs, nullptr, 0);
            context->PSSetShader(ps, nullptr, 0);
            context->Draw(3, 0);
            const std::uint64_t world_vs = 0x9AEC596A2B036EA6ull;
            const std::uint64_t world_ps = 0x3789CA2062E196FBull;
            vs->SetPrivateData(kEdpeShaderHashGuid, sizeof(world_vs), &world_vs);
            ps->SetPrivateData(kEdpeShaderHashGuid, sizeof(world_ps), &world_ps);
            context->Draw(3, 0); // Diagnostic fanout creation; original PS stays bound.
            const std::uint64_t glass_vs = 0xF512712C40D93C12ull;
            const std::uint64_t glass_ps_hash = 0x4A71EB0D34E9F2EFull;
            constexpr char glass_source[] =
                "struct Out { float4 color : SV_Target0; float4 transmission : SV_Target1; }; "
                "Out main() { Out o; o.color = float4(.5, 0, 0, 1); "
                "o.transmission = float4(.5, .5, .5, .5); return o; }";
            ID3DBlob* glass_code = nullptr;
            ID3D11PixelShader* glass_ps = nullptr;
            if (FAILED(D3DCompile(glass_source, sizeof(glass_source) - 1,
                    nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0,
                    &glass_code, nullptr)) ||
                FAILED(device->CreatePixelShader(glass_code->GetBufferPointer(),
                    glass_code->GetBufferSize(), nullptr, &glass_ps))) return 12;
            vs->SetPrivateData(kEdpeShaderHashGuid, sizeof(glass_vs), &glass_vs);
            glass_ps->SetPrivateData(kEdpeShaderHashGuid,
                sizeof(glass_ps_hash), &glass_ps_hash);
            D3D11_BLEND_DESC glass_blend_desc{};
            auto& rt = glass_blend_desc.RenderTarget[0];
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D11_BLEND_ONE;
            rt.DestBlend = D3D11_BLEND_SRC1_COLOR;
            rt.BlendOp = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_SRC1_ALPHA;
            rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            ID3D11BlendState* glass_blend = nullptr;
            if (FAILED(device->CreateBlendState(&glass_blend_desc, &glass_blend))) return 12;
            D3D11_DEPTH_STENCIL_DESC glass_depth_desc{};
            glass_depth_desc.DepthEnable = TRUE;
            glass_depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            glass_depth_desc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
            glass_depth_desc.StencilEnable = TRUE;
            glass_depth_desc.StencilReadMask = 0;
            glass_depth_desc.StencilWriteMask = 4;
            glass_depth_desc.FrontFace = {D3D11_STENCIL_OP_KEEP,
                D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE,
                D3D11_COMPARISON_ALWAYS};
            glass_depth_desc.BackFace = {D3D11_STENCIL_OP_KEEP,
                D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE,
                D3D11_COMPARISON_ALWAYS};
            ID3D11DepthStencilState* glass_depth = nullptr;
            if (FAILED(device->CreateDepthStencilState(&glass_depth_desc, &glass_depth))) return 12;
            const UINT indices[3]{0, 1, 2};
            D3D11_BUFFER_DESC index_desc{};
            index_desc.ByteWidth = sizeof(indices);
            index_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            D3D11_SUBRESOURCE_DATA index_data{};
            index_data.pSysMem = indices;
            ID3D11Buffer* index_buffer = nullptr;
            if (FAILED(device->CreateBuffer(&index_desc, &index_data, &index_buffer))) return 12;
            D3D11_VIEWPORT viewport{0, 0, 64, 64, 0, 1};
            context->RSSetViewports(1, &viewport);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->IASetIndexBuffer(index_buffer, DXGI_FORMAT_R32_UINT, 0);
            context->OMSetRenderTargets(1, &second_color_alias, depth_view);
            context->OMSetBlendState(glass_blend, nullptr, ~0u);
            context->OMSetDepthStencilState(glass_depth, 4);
            context->ClearDepthStencilView(depth_view,
                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0, 0);
            context->PSSetShader(glass_ps, nullptr, 0);
            context->Draw(0, 0); // Same signature, unsupported draw shape: no replay.
            context->DrawIndexedInstanced(3, 1, 0, 0, 0);
            ID3D11RenderTargetView* restored_target = nullptr;
            context->OMGetRenderTargets(1, &restored_target, nullptr);
            glass_target_restored = restored_target == second_color_alias;
            if (restored_target) restored_target->Release();
            context->ClearDepthStencilView(depth_view,
                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, .5f, 0);
            context->OMSetBlendState(nullptr, nullptr, ~0u);
            context->OMSetDepthStencilState(nullptr, 0);
            context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
            context->VSSetShader(nullptr, nullptr, 0);
            context->PSSetShader(nullptr, nullptr, 0);
            index_buffer->Release();
            glass_depth->Release();
            glass_blend->Release();
            glass_ps->Release();
            glass_code->Release();
            vs->Release();
            ps->Release();
            vs_code->Release();
            ps_code->Release();
        }
        context->OMSetRenderTargets(0, nullptr, nullptr);
        motion_present = swap_chain->Present(0, 0);
        if (FAILED(motion_present)) break;
    }
    context->OMSetRenderTargets(0, nullptr, depth_view);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    context->Flush();
    for (int i = 0; i < 8; ++i) swap_chain->Present(0, 0);
    // Allow asynchronous motion grid readback to complete on slower/WARP CI runners
    for (int retry = 0; retry < 60 && scene_depth_candidate() != 0; ++retry) {
        context->Flush();
        swap_chain->Present(0, 0);
        Sleep(5);
    }
    const bool scene_candidate_held = scene_depth_candidate() == 0;
    SendMessageW(window, WM_KEYDOWN, 'A', 0);
    const bool visible_input_blocked = forwarded_keys == 1;
    const HRESULT resize_result = swap_chain->ResizeBuffers(0, 128, 128, DXGI_FORMAT_UNKNOWN, 0);
    const HRESULT resized_present = SUCCEEDED(resize_result) ? swap_chain->Present(0, 0) : resize_result;
    for (int i = 0; i < 40; ++i) swap_chain->Present(0, 0);
    const bool scene_candidate_expired = scene_depth_candidate() == -1;
    ID3D11DeviceContext* deferred_context = nullptr;
    const HRESULT def_hr = device->CreateDeferredContext(0, &deferred_context);
    bool def_executed = false;
    if (SUCCEEDED(def_hr) && deferred_context) {
        deferred_context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, depth_view, 0, 0, nullptr, nullptr);
        deferred_context->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D11CommandList* cmd_list = nullptr;
        if (SUCCEEDED(deferred_context->FinishCommandList(FALSE, &cmd_list)) && cmd_list) {
            context->ExecuteCommandList(cmd_list, FALSE);
            cmd_list->Release();
            def_executed = true;
        }
    }
    bool dev2_executed = false;
    bool dev2_candidate_found = false;
    ID3D11Device* device2 = nullptr;
    ID3D11DeviceContext* context2 = nullptr;
    ID3D11Texture2D* depth2_tex = nullptr;
    ID3D11DepthStencilView* depth2_view = nullptr;
    const HRESULT dev2_hr = create_device(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device2, nullptr, &context2);
    if (SUCCEEDED(dev2_hr) && device2 && context2) {
        const bool shared_runtime_vtable =
            (*reinterpret_cast<void***>(device) == *reinterpret_cast<void***>(device2)) &&
            ((*reinterpret_cast<void***>(context))[33] == (*reinterpret_cast<void***>(context2))[33]) &&
            ((*reinterpret_cast<void***>(context))[34] == (*reinterpret_cast<void***>(context2))[34]);
        D3D11_TEXTURE2D_DESC depth2_desc{};
        depth2_desc.Width = 256;
        depth2_desc.Height = 256;
        depth2_desc.MipLevels = 1;
        depth2_desc.ArraySize = 1;
        depth2_desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        depth2_desc.SampleDesc.Count = 1;
        depth2_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        HRESULT tex_hr = device2->CreateTexture2D(&depth2_desc, nullptr, &depth2_tex);
        if (SUCCEEDED(tex_hr)) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dsv2_desc{};
            dsv2_desc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
            dsv2_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            HRESULT dsv_hr = device2->CreateDepthStencilView(depth2_tex, &dsv2_desc, &depth2_view);
            if (SUCCEEDED(dsv_hr)) {
                auto make_dev2_rtv = [&](DXGI_FORMAT fmt, ID3D11Texture2D** out_tex, ID3D11RenderTargetView** out_rtv) {
                    D3D11_TEXTURE2D_DESC td{};
                    td.Width = 256;
                    td.Height = 256;
                    td.MipLevels = 1;
                    td.ArraySize = 1;
                    td.Format = fmt;
                    td.SampleDesc.Count = 1;
                    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                    if (SUCCEEDED(device2->CreateTexture2D(&td, nullptr, out_tex)) && *out_tex) {
                        device2->CreateRenderTargetView(*out_tex, nullptr, out_rtv);
                    }
                };
                ID3D11Texture2D* dev2_t0 = nullptr;
                ID3D11Texture2D* dev2_t1 = nullptr;
                ID3D11Texture2D* dev2_t2 = nullptr;
                ID3D11Texture2D* dev2_t3 = nullptr;
                ID3D11RenderTargetView* dev2_r0 = nullptr;
                ID3D11RenderTargetView* dev2_r1 = nullptr;
                ID3D11RenderTargetView* dev2_r2 = nullptr;
                ID3D11RenderTargetView* dev2_r3 = nullptr;
                make_dev2_rtv(DXGI_FORMAT_R10G10B10A2_UNORM, &dev2_t0, &dev2_r0);
                make_dev2_rtv(DXGI_FORMAT_R8G8B8A8_UNORM, &dev2_t1, &dev2_r1);
                make_dev2_rtv(DXGI_FORMAT_R8G8B8A8_UNORM, &dev2_t2, &dev2_r2);
                make_dev2_rtv(DXGI_FORMAT_R11G11B10_FLOAT, &dev2_t3, &dev2_r3);
                ID3D11RenderTargetView* dev2_mrts[4]{dev2_r0, dev2_r1, dev2_r2, dev2_r3};
                ID3D11Buffer* dev2_cb = nullptr;
                const HRESULT cb_hr = device2->CreateBuffer(&probe_desc, &probe_initial, &dev2_cb);
                set_jitter_test_override(TRUE);
                for (int i = 0; i < 32; ++i) {
                    context2->OMSetRenderTargets(4, dev2_mrts, depth2_view);
                    if (i == 0 && dev2_cb) {
                        D3D11_MAPPED_SUBRESOURCE dev2_mapped{};
                        const HRESULT map_hr = context2->Map(dev2_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &dev2_mapped);
                        if (SUCCEEDED(map_hr)) {
                            std::memcpy(dev2_mapped.pData, probe_values, sizeof(probe_values));
                            context2->Unmap(dev2_cb, 0);
                        }
                    }
                }
                set_jitter_test_override(FALSE);
                if (dev2_cb) dev2_cb->Release();
                context2->OMSetRenderTargets(1, &dev2_r3, depth2_view);
                context2->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, depth2_view, 0, 0, nullptr, nullptr);
                context2->OMSetRenderTargets(0, nullptr, nullptr);
                dev2_candidate_found = (scene_depth_candidate() == 0);
                dev2_executed = shared_runtime_vtable && dev2_candidate_found;
                if (dev2_r0) dev2_r0->Release();
                if (dev2_r1) dev2_r1->Release();
                if (dev2_r2) dev2_r2->Release();
                if (dev2_r3) dev2_r3->Release();
                if (dev2_t0) dev2_t0->Release();
                if (dev2_t1) dev2_t1->Release();
                if (dev2_t2) dev2_t2->Release();
                if (dev2_t3) dev2_t3->Release();
            } else {
                std::fprintf(stderr, "DSV2_FAIL: %08lx\n", dsv_hr);
            }
        } else {
            std::fprintf(stderr, "TEX2_FAIL: %08lx\n", tex_hr);
        }
    } else {
        std::fprintf(stderr, "DEV2_FAIL: %08lx dev=%p ctx=%p\n", dev2_hr, device2, context2);
    }
    bool saved_bmp = inspect_bmp();
    context->Flush();
    // Allow pending asynchronous staging texture readbacks to finish writing captures to disk
    for (int retry = 0; retry < 60 && !saved_bmp; ++retry) {
        context->Flush();
        swap_chain->Present(0, 0);
        saved_bmp = inspect_bmp();
        if (!saved_bmp) Sleep(5);
    }
    while (present_count() < 1024) swap_chain->Present(0, DXGI_PRESENT_TEST);
    const bool interval_observed = present_count() == 1024;
    SendMessageW(window, WM_KEYUP, VK_F5, 0);
    SendMessageW(window, WM_KEYDOWN, VK_F5, 0);
    const bool closed = !menu_visible();
    SendMessageW(window, WM_KEYUP, VK_F5, 0);
    SendMessageW(window, WM_KEYDOWN, 'A', 0);
    const bool hidden_input_restored = forwarded_keys == 2;
    // Exercise OMSetRenderTargets with NumViews=8 and null in unused slots (matching Cobra Odyssey lighting pass)
    ID3D11RenderTargetView* cobra_views[8]{second_color_view, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    context->OMSetRenderTargets(8, cobra_views, nullptr);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11Buffer* no_buffer = nullptr;
    context->VSSetConstantBuffers(1, 1, &no_buffer);
    context->VSSetConstantBuffers(2, 1, &no_buffer);
    depth_pass_buffer->Release();
    probe_buffer->Release();
    color_view->Release();
    color_texture->Release();
    second_color_view->Release();
    second_color_alias->Release();
    second_color_texture->Release();
    mrt0_view->Release();
    mrt0_texture->Release();
    mrt2_view->Release();
    mrt2_texture->Release();
    depth_view->Release();
    depth_texture->Release();
    swap_chain->Release();
    const bool context_hook_restored =
        real_system_om_set ? ((*reinterpret_cast<void***>(context))[33] == real_system_om_set &&
                              (*reinterpret_cast<void***>(context))[33] != hooked_om_set)
                           : ((*reinterpret_cast<void***>(context))[33] != hooked_om_set);
    auto is_d3d11_fn = [](void* fn) {
        if (!fn) return false;
        HMODULE mod = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(fn), &mod) || !mod) return false;
        wchar_t name[MAX_PATH]{};
        GetModuleFileNameW(mod, name, MAX_PATH);
        wchar_t* base = wcsrchr(name, L'\\');
        return base && _wcsicmp(base + 1, L"d3d11.dll") == 0;
    };
    const bool draw_hooks_restored =
        ((*reinterpret_cast<void***>(context))[12] == original_draws[0] || is_d3d11_fn((*reinterpret_cast<void***>(context))[12])) &&
        ((*reinterpret_cast<void***>(context))[13] == original_draws[1] || is_d3d11_fn((*reinterpret_cast<void***>(context))[13])) &&
        ((*reinterpret_cast<void***>(context))[20] == original_draws[2] || is_d3d11_fn((*reinterpret_cast<void***>(context))[20])) &&
        ((*reinterpret_cast<void***>(context))[21] == original_draws[3] || is_d3d11_fn((*reinterpret_cast<void***>(context))[21]));
    if (depth2_view) depth2_view->Release();
    if (depth2_tex) depth2_tex->Release();
    if (context2) context2->Release();
    if (device2) device2->Release();
    if (deferred_context) deferred_context->Release();
    if (factory) factory->Release();
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    context->Release();
    device->Release();
    FreeLibrary(dxgi_proxy);
    const HANDLE log = CreateFileW(log_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) return 11;
    char contents[65536]{};
    DWORD bytes_read = 0;
    const BOOL read = ReadFile(log, contents, sizeof(contents) - 1, &bytes_read, nullptr);
    CloseHandle(log);
    if (!saved_bmp) saved_bmp = inspect_bmp();
    char dev2_census_prefix[256]{};
    std::snprintf(dev2_census_prefix, sizeof(dev2_census_prefix),
        "EDPE: context census ctx=%p device=%p OMSetRenderTargets=34 (game=34 own=0) OMSetRenderTargetsAndUnorderedAccessViews=1 (game=1 own=0) omCalls=35 dsvCalls=34 (game=34 own=0) maxDsv=256x256",
        context2, device2);
    bool passed = true;
    auto check = [&](bool cond, const char* name) {
        if (!cond) {
            std::fprintf(stderr, "FAILED CHECK: %s\n", name);
            passed = false;
        }
    };
    check(SUCCEEDED(present_result), "SUCCEEDED(present_result)");
    check(SUCCEEDED(second_present_result), "SUCCEEDED(second_present_result)");
    check(observed, "observed");
    check(SUCCEEDED(first_real_present), "SUCCEEDED(first_real_present)");
    check(SUCCEEDED(overlay_present), "SUCCEEDED(overlay_present)");
    check(SUCCEEDED(arm_present), "SUCCEEDED(arm_present)");
    check(SUCCEEDED(sequence_present), "SUCCEEDED(sequence_present)");
    check(sequence_requested, "sequence_requested");
    check(SUCCEEDED(snapshot_arm_present), "SUCCEEDED(snapshot_arm_present)");
    check(SUCCEEDED(snapshot_present), "SUCCEEDED(snapshot_present)");
    check(snapshot_requested, "snapshot_requested");
    check(scene_candidate_found, "scene_candidate_found");
    check(scene_candidate_held, "scene_candidate_held");
    check(scene_candidate_expired, "scene_candidate_expired");
    check(def_executed, "def_executed");
    check(dev2_executed, "dev2_executed");
    check(pair_requested, "pair_requested");
    check(SUCCEEDED(pair_arm_present), "SUCCEEDED(pair_arm_present)");
    check(SUCCEEDED(pair_present), "SUCCEEDED(pair_present)");
    check(motion_requested, "motion_requested");
    check(SUCCEEDED(motion_arm_present), "SUCCEEDED(motion_arm_present)");
    check(SUCCEEDED(motion_present), "SUCCEEDED(motion_present)");
    check(glass_target_restored, "glass_target_restored");
    check(SUCCEEDED(resize_result), "SUCCEEDED(resize_result)");
    check(SUCCEEDED(resized_present), "SUCCEEDED(resized_present)");
    check(opened, "opened");
    check(closed, "closed");
    check(insert_passed, "insert_passed");
    check(f5_repeat_ignored, "f5_repeat_ignored");
    check(hidden_input_passed, "hidden_input_passed");
    check(visible_input_blocked, "visible_input_blocked");
    check(hidden_input_restored, "hidden_input_restored");
    check(context_hook_restored, "context_hook_restored");
    check(draw_hooks_restored, "draw_hooks_restored");
    check(interval_observed, "interval_observed");
    check(read, "read");
    check(saved_bmp, "saved_bmp");
    check(!std::strstr(contents, "OLD_SESSION"), "!strstr OLD_SESSION");
    check(std::strstr(contents, "EDPE: depth capture saved ") || std::strstr(contents, "EDPE: frame capture saved "), "capture saved");
    check(std::strstr(contents, "EDPE: D3D11 device created"), "device created");
    check(std::strstr(contents, "EDPE: DXGI factory created"), "factory created");
    check(std::strstr(contents, "EDPE: swapchain created"), "swapchain created");
    check(std::strstr(contents, "EDPE: Present swapchain="), "Present swapchain");
    check(std::strstr(contents, "matched known device"), "matched known device");
    check(std::strstr(contents, "EDPE: Present bindings"), "Present bindings");
    check(std::strstr(contents, "EDPE: context dispatch frame=1"), "dispatch frame=1");
    check(std::strstr(contents, "EDPE: context dispatch frame=8"), "dispatch frame=8");
    check(std::strstr(contents, "slot12=") && std::strstr(contents, "slot53="), "slot12 && slot53");
    check(std::strstr(contents, "EDPE: OMSetRenderTargets DSV census armed"), "DSV census armed");
    check(std::strstr(contents, "EDPE: HDR color copied before first matched HUD draw"), "HDR color copied before HUD");
    check(std::strstr(contents, "EDPE: clean HDR RTV ready for diagnostic world replay"), "clean HDR RTV ready");
    check(std::strstr(contents, "hudCopy=1 afterHud=3 glassAfterHud=2"), "hudCopy=1 afterHud=3 glassAfterHud=2");
    check(std::strstr(contents, "glassReplay=1 glassDeclined=1"), "glassReplay=1 glassDeclined=1");
    check(std::strstr(contents, "EDPE: glass replay declined: glass draw shape differs"), "glass replay declined");
    check(std::strstr(contents, "EDPE: world fanout probe VS=9AEC596A2B036EA6 PS=3789CA2062E196FB bytes="), "world fanout probe");
    check(std::strstr(contents, "result=ready"), "result=ready");
    check(std::strstr(contents, "EDPE: HDR draw shader 0 VS=B7790CBFC6554097 PS=8DEF46452FA459F5 draws=1 afterHud=0"), "HDR draw shader 0");
    check(std::strstr(contents, "EDPE: HDR draw shader 1 VS=9AEC596A2B036EA6 PS=3789CA2062E196FB draws=1 afterHud=1"), "HDR draw shader 1");
    check(std::strstr(contents, "EDPE: HDR draw shader 2 VS=F512712C40D93C12 PS=4A71EB0D34E9F2EF draws=2 afterHud=2"), "HDR draw shader 2");
    check(std::strstr(contents, "rearmed=0x2 restored=0xF"), "rearmed=0x2 restored=0xF");
    check(std::strstr(contents, "EDPE: DSV bind #0 phase=first view="), "DSV bind #0 phase=first");
    check(std::strstr(contents, "EDPE: DSV bind #0 phase=first-color view="), "DSV bind #0 phase=first-color");
    check(std::strstr(contents, "color=64x64 colorFormat=28 colorBind=0x20"), "color=64x64 colorFormat=28");
    check(std::strstr(contents, "EDPE: DSV interval frame=1024 top=0:29"), "DSV interval frame=1024 top=0:29");
    check(std::strstr(contents, "EDPE: DSV interval frame=1024 top=0:34"), "DSV interval frame=1024 top=0:34");
    check(std::strstr(contents, "EDPE: DSV bind sequence frame=6 transitions=2 stored=2"), "bind sequence frame=6 transitions=2");
    check(std::strstr(contents, "EDPE: DSV bind sequence 0 target=-1"), "bind sequence 0 target=-1");
    check(std::strstr(contents, "EDPE: DSV bind sequence 1 target=0"), "bind sequence 1 target=0");
    check(std::strstr(contents, "viewFormat=20 textureFormat=19 depth=64x64 bind=0x48"), "depth=64x64 bind=0x48");
    check(std::strstr(contents, "EDPE: depth snapshot timing #0 armedAfter=7 firstBindAfter=7 lastBindAfter=7 binds=6 handedAt=8"), "depth snapshot timing #0");
    check(std::strstr(contents, "EDPE: DSV #0 bind=2 CB stage=VS slot=1 buffer="), "DSV #0 bind=2 CB");
    check(std::strstr(contents, "EDPE: DSV #0 bind=3 color target count=4"), "bind=3 color target count=4");
    check(std::strstr(contents, "EDPE: DSV #0 bind=3 rtv0="), "bind=3 rtv0=");
    check(std::strstr(contents, "format=24 size=64x64 bind=0x20"), "format=24 size=64x64");
    check(std::strstr(contents, "EDPE: DSV #0 bind=3 rtv1="), "bind=3 rtv1=");
    check(std::strstr(contents, "EDPE: DSV #0 bind=3 rtv2="), "bind=3 rtv2=");
    check(std::strstr(contents, "EDPE: DSV #0 bind=3 rtv3="), "bind=3 rtv3=");
    check(std::strstr(contents, "format=26 size=64x64 bind=0x20"), "format=26 size=64x64");
    check(std::strstr(contents, "EDPE: DSV #0 bind=6 color target count=1"), "bind=6 color target count=1");
    check(std::strstr(contents, "bytes=5376 usage=2 cpu=0x10000"), "bytes=5376 usage=2 cpu=0x10000");
    check(std::strstr(contents, "EDPE: scene CB sample bind=3 projectionZ=(0,0.025000"), "projectionZ bind=3");
    check(std::strstr(contents, "projection2D=0.025000"), "projection2D=0.025000");
    check(std::strstr(contents, "rows0=(1,0,0,0)"), "rows0=(1,0,0,0)");
    check(std::strstr(contents, "EDPE: scene CB 2D xy bind=3 x=(1.25,0,0,0)"), "scene CB 2D xy bind=3");
    check(std::strstr(contents, "EDPE: scene CB 2D xy bind=4 x=(1.75,0,0,0)"), "scene CB 2D xy bind=4");
    check(std::strstr(contents, "EDPE: scene CB 2D xy bind=6 x=(2.5,0,0,0)"), "scene CB 2D xy bind=6");
    check(std::strstr(contents, "EDPE: depth-pass CB bind=2 slot=2 hex=3F8000004000000040400000"), "depth-pass CB bind=2 slot=2");
    check(std::strstr(contents, "EDPE: scene CB 2D zw bind=6 z=(0,0,0,0) w=(0,0,0.0500000007,0)"), "scene CB 2D zw bind=6");
    check(std::strstr(contents, "EDPE: scene CB sample bind=6 projectionZ=(0,0.050000"), "scene CB sample bind=6");
    check(std::strstr(contents, "projection2D=0.050000"), "projection2D=0.050000");
    check(std::strstr(contents, "rows0=(1,0,0,42)"), "rows0=(1,0,0,42)");
    check(std::strstr(contents, "EDPE: scene CB rows1 bind=6 (0,1,0,0) rows2=(0,0,1,0)"), "scene CB rows1 bind=6");
    check(std::strstr(contents, "EDPE: scene pipeline afterPresent=7 bind=1 iaPrimitives=0 vsInvocations=0"), "pipeline bind=1");
    check(std::strstr(contents, "EDPE: scene pipeline afterPresent=7 bind=2 iaPrimitives=0 vsInvocations=0"), "pipeline bind=2");
    check(std::strstr(contents, "EDPE: depth-pass VS bindings calls=53 sceneBufferCalls=52 first="), "depth-pass VS bindings calls=53");
    check(std::strstr(contents, "switches=0 slotRestored=1"), "switches=0 slotRestored=1");
    check(std::strstr(contents, "EDPE: depth-pass DrawIndexed calls=2 with5376VS1=1 slotRestored=1"), "depth-pass DrawIndexed calls=2");
    check(std::strstr(contents, "EDPE: scene draws Draw=3/2 DrawIndexedInstanced=2/1 DrawInstanced=2/1 restored=111"), "scene draws restored=111");
    check(std::strstr(contents, "EDPE: scene buffer maps=1 afterDraw=1 unmaps=1 restored=11"), "scene buffer maps=1 afterDraw=1");
    check(std::strstr(contents, "EDPE: scene buffer maps=1 afterDraw=0 unmaps=1 restored=11 bind=1 beforeBind=1 target="), "scene buffer maps=1 afterDraw=0");
    check(std::strstr(contents, "EDPE: scene buffer maps=0 afterDraw=0 unmaps=0 restored=11 bind=2"), "scene buffer maps=0 bind=2");
    check(std::strstr(contents, "EDPE: scene buffer maps=1 afterDraw=1 unmaps=1 restored=11 bind=3"), "scene buffer maps=1 bind=3");
    check(std::strstr(contents, "EDPE: scene write hashes sampled=1 cameraChanges=0 projectionChanges=0 camera="), "scene write hashes");
    check(std::strstr(contents, "EDPE: scene CB sample bind=0 projectionZ=(0,0.025000"), "scene CB sample bind=0");
    check(std::strstr(contents, "EDPE: scene CB sample bind=51 projectionZ=(0,0.025000"), "scene CB sample bind=51");
    check(std::strstr(contents, "EDPE: scene CB hash bind=0 fnv64="), "scene CB hash bind=0");
    check(std::strstr(contents, "EDPE: scene CB hash bind=51 fnv64=A623605C522D5605 camera64=E22643B8B93F6294"), "scene CB hash bind=51");
    check(std::strstr(contents, "EDPE: scene CB hash bind=3 fnv64="), "scene CB hash bind=3");
    check(std::strstr(contents, "EDPE: scene pipeline afterPresent=7 bind=3 iaPrimitives=1 vsInvocations=3"), "pipeline bind=3");
    check(std::strstr(contents, "EDPE: scene pipeline afterPresent=7 bind=6 iaPrimitives=0 vsInvocations=0"), "pipeline bind=6");
    check(std::strstr(contents, "EDPE: scene depth state afterPresent=7 bind=1 edge=start"), "depth state bind=1 start");
    check(std::strstr(contents, "EDPE: scene depth state afterPresent=7 bind=1 edge=end"), "depth state bind=1 end");
    check(std::strstr(contents, "EDPE: scene depth state afterPresent=7 bind=2 edge=start"), "depth state bind=2 start");
    check(std::strstr(contents, "EDPE: scene depth state afterPresent=7 bind=2 edge=end"), "depth state bind=2 end");
    check(std::strstr(contents, "EDPE: scene depth state afterPresent=7 bind=3 edge=start"), "depth state bind=3 start");
    check(std::strstr(contents, "EDPE: scene depth state afterPresent=7 bind=3 edge=end"), "depth state bind=3 end");
    check(std::strstr(contents, "EDPE: scene depth state afterPresent=7 bind=6 edge=start"), "depth state bind=6 start");
    check(std::strstr(contents, "EDPE: scene depth state afterPresent=7 bind=6 edge=end"), "depth state bind=6 end");
    check(std::strstr(contents, "EDPE: scene CB hex 0000 "), "scene CB hex 0000");
    check(std::strstr(contents, "EDPE: scene CB hex 1312 "), "scene CB hex 1312");
    check(std::strstr(contents, "EDPE: depth snapshot #0 copied 64x64 format=19"), "depth snapshot #0 copied");
    check(std::strstr(contents, "EDPE: scene color snapshot #0 copied 64x64 format=26"), "scene color snapshot #0 copied");
    check(std::strstr(contents, "EDPE: scene color RTV unbound at Present"), "scene color RTV unbound");
    check(std::strstr(contents, "EDPE: scene color snapshot image submitted to ImGui"), "color snapshot to ImGui");
    check(std::strstr(contents, "EDPE: depth snapshot image submitted to ImGui"), "depth snapshot to ImGui");
    check(std::strstr(contents, "EDPE: depth contrast shader active in ImGui"), "depth contrast active");
    check(std::strstr(contents, "EDPE: depth sample grid valid=64 nonzero=64 min=0.25 max=0.25 centerValid=1 center=0.25"), "depth sample grid");
    check(std::strstr(contents, "EDPE: adjacent camera probe armed DSV #0"), "adjacent camera probe armed");
    check(std::strstr(contents, "EDPE: scene CB sample bind=100"), "scene CB sample bind=100");
    check(std::strstr(contents, "EDPE: scene CB sample bind=101"), "scene CB sample bind=101");
    check(std::strstr(contents, "EDPE: adjacent depth afterPresent="), "adjacent depth afterPresent");
    check(std::strstr(contents, "centerValid=1 center=0.25 size=64x64"), "centerValid=1 center=0.25");
    check(std::strstr(contents, "centerValid=1 center=0.5 size=64x64"), "centerValid=1 center=0.5");
    check(std::strstr(contents, "EDPE: motion candidate depth retained on GPU"), "motion depth retained");
    check(std::strstr(contents, "EDPE: camera GPU copy queued bind=100 bytes=5376"), "camera copy bind=100");
    check(std::strstr(contents, "EDPE: camera GPU copy queued bind=101 bytes=5376"), "camera copy bind=101");
    check(std::strstr(contents, "EDPE: motion candidate GPU pass completed"), "motion GPU pass completed");
    check(std::strstr(contents, "EDPE: motion preview shader ready"), "motion preview shader ready");
    check(std::strstr(contents, "EDPE: motion snapshot handed to ImGui"), "motion snapshot to ImGui");
    check(std::strstr(contents, "EDPE: DSV observer restored after context-state swap"), "DSV observer restored");
    check(std::strstr(contents, "EDPE: motion candidate currentAfterPresent=17 center=(6.39844,0)"), "motion center=(6.39844,0)");
    check(std::strstr(contents, "EDPE: motion grid 5x5 finite=25"), "motion grid 5x5 finite=25");
    check(std::strstr(contents, "pixels finite=1"), "pixels finite=1");
    check(std::strstr(contents, "EDPE: DSV census frame=1024 binds=29 unique=1 slotActive=1"), "census binds=29");
    check(std::strstr(contents, "EDPE: DSV census frame=1024 binds=34 unique=1 slotActive=1"), "census binds=34");
    check(std::strstr(contents, "EDPE: WARNING: scene context differs from present context"), "WARNING scene context differs");
    check(std::strstr(contents, "EDPE: projection jitter applied #1 frame="), "jitter applied #1");
    check(std::strstr(contents, dev2_census_prefix), "dev2_census_prefix");
    check(std::strstr(contents, "EDPE: context census ctx="), "context census ctx=");
    check(std::strstr(contents, "maxDsv=256x256"), "maxDsv=256x256");
    check(std::strstr(contents, "EDPE: hook OMSetRenderTargets this="), "hook OMSetRenderTargets");
    check(std::strstr(contents, "type=immediate"), "type=immediate");
    check(std::strstr(contents, "type=deferred"), "type=deferred");
    check(std::strstr(contents, "EDPE: deferred context vtable hooked"), "deferred context hooked");
    check(std::strstr(contents, "EDPE: context calls frame=1024"), "context calls frame=1024");
    check(std::strstr(contents, "OMSetRenderTargetsAndUnorderedAccessViews="), "OMSetRenderTargetsAndUnorderedAccessViews=");
    check(std::strstr(contents, "FinishCommandList="), "FinishCommandList=");
    check(std::strstr(contents, "ExecuteCommandList="), "ExecuteCommandList=");
    check(std::strstr(contents, "vtable slot 33 (OMSetRenderTargets) match"), "slot 33 match");
    check(std::strstr(contents, "vtable slot 34 (OMSetRenderTargetsAndUAVs)"), "slot 34");
    check(std::strstr(contents, "EDPE: Dear ImGui ready"), "Dear ImGui ready");
    check(std::strstr(contents, "EDPE: D3D11 context state available=1"), "context state available");
    check(std::strstr(contents, "EDPE: queued input routed to Dear ImGui"), "queued input routed");
    check(std::strstr(contents, "vtable=") && std::strstr(contents, "dsvMethod="), "vtable && dsvMethod");
    if (!passed) std::fprintf(stderr,
        "present=%08lx/%08lx real=%08lx overlay=%08lx resize=%08lx/%08lx observed=%d menu=%d/%d insert=%d repeat=%d input=%d/%d/%d read=%d glassRestored=%d dev2=%d/%d candidate=%d/%d/%d interval=%d hooks=%d/%d requests=%d/%d/%d\n",
        present_result, second_present_result, first_real_present, overlay_present,
        resize_result, resized_present, observed, opened, closed,
        insert_passed, f5_repeat_ignored,
        hidden_input_passed, visible_input_blocked, hidden_input_restored, read,
        glass_target_restored, dev2_executed, dev2_candidate_found, scene_candidate_found, scene_candidate_held,
        scene_candidate_expired, interval_observed, context_hook_restored,
        draw_hooks_restored, sequence_requested, snapshot_requested, pair_requested);
    return passed ? 0 : 12;
}
