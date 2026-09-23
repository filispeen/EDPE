#include <d3d11.h>
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
    DeleteFileW(argv[3]);
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
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* depth_texture = nullptr;
    if (FAILED(device->CreateTexture2D(&depth_desc, nullptr, &depth_texture))) return 10;
    ID3D11DepthStencilView* depth_view = nullptr;
    if (FAILED(device->CreateDepthStencilView(depth_texture, nullptr, &depth_view))) return 10;
    D3D11_TEXTURE2D_DESC color_desc = depth_desc;
    color_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* color_texture = nullptr;
    if (FAILED(device->CreateTexture2D(&color_desc, nullptr, &color_texture))) return 10;
    ID3D11RenderTargetView* color_view = nullptr;
    if (FAILED(device->CreateRenderTargetView(color_texture, nullptr, &color_view))) return 10;
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
    SendMessageW(window, WM_KEYDOWN, 'A', 0);
    const bool visible_input_blocked = forwarded_keys == 1;
    const HRESULT resize_result = swap_chain->ResizeBuffers(0, 128, 128, DXGI_FORMAT_UNKNOWN, 0);
    const HRESULT resized_present = SUCCEEDED(resize_result) ? swap_chain->Present(0, 0) : resize_result;
    for (int i = 7; i < 1024; ++i) swap_chain->Present(0, DXGI_PRESENT_TEST);
    const bool interval_observed = present_count() == 1024;
    SendMessageW(window, WM_KEYUP, VK_F5, 0);
    SendMessageW(window, WM_KEYDOWN, VK_F5, 0);
    const bool closed = !menu_visible();
    SendMessageW(window, WM_KEYUP, VK_F5, 0);
    SendMessageW(window, WM_KEYDOWN, 'A', 0);
    const bool hidden_input_restored = forwarded_keys == 2;
    context->OMSetRenderTargets(0, nullptr, nullptr);
    color_view->Release();
    color_texture->Release();
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
    char contents[8192]{};
    DWORD bytes_read = 0;
    const BOOL read = ReadFile(log, contents, sizeof(contents) - 1, &bytes_read, nullptr);
    CloseHandle(log);
    const bool passed = SUCCEEDED(present_result) && SUCCEEDED(second_present_result) && observed &&
        SUCCEEDED(first_real_present) && SUCCEEDED(overlay_present) &&
        SUCCEEDED(arm_present) && SUCCEEDED(sequence_present) && sequence_requested &&
        SUCCEEDED(resize_result) && SUCCEEDED(resized_present) && opened && closed &&
        insert_passed && f5_repeat_ignored &&
        hidden_input_passed && visible_input_blocked && hidden_input_restored &&
        context_hook_restored &&
        interval_observed &&
        read && std::strstr(contents, "EDPE: Present swapchain=") &&
        std::strstr(contents, "EDPE: Present bindings") &&
        std::strstr(contents, "EDPE: context dispatch frame=1") &&
        std::strstr(contents, "EDPE: context dispatch frame=8") &&
        std::strstr(contents, "slot12=") && std::strstr(contents, "slot53=") &&
        std::strstr(contents, "EDPE: OMSetRenderTargets DSV census armed") &&
        std::strstr(contents, "EDPE: DSV bind #0 phase=first view=") &&
        std::strstr(contents, "EDPE: DSV bind #0 phase=first-color view=") &&
        std::strstr(contents, "color=64x64 colorFormat=28 colorBind=0x20") &&
        std::strstr(contents, "EDPE: DSV interval frame=1024 top=0:7") &&
        std::strstr(contents, "EDPE: DSV bind sequence frame=6 transitions=2 stored=2") &&
        std::strstr(contents, "EDPE: DSV bind sequence 0 target=-1") &&
        std::strstr(contents, "EDPE: DSV bind sequence 1 target=0") &&
        std::strstr(contents, "viewFormat=45 textureFormat=45 depth=64x64 bind=0x40") &&
        std::strstr(contents, "EDPE: Dear ImGui ready") &&
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
