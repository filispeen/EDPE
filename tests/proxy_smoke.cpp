#include <d3d11.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <cstring>
#include <cstdio>

int forwarded_keys = 0;

LRESULT CALLBACK testWndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_KEYDOWN && wparam == 'A') ++forwarded_keys;
    return DefWindowProcW(window, message, wparam, lparam);
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) return 1;
    DeleteFileW(argv[3]);
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

    const HMODULE dxgi_proxy = LoadLibraryW(argv[2]);
    if (!dxgi_proxy) return 5;
    const auto create_factory = reinterpret_cast<decltype(&CreateDXGIFactory1)>(
        GetProcAddress(dxgi_proxy, "CreateDXGIFactory1"));
    if (!create_factory) return 6;
    const auto present_count = reinterpret_cast<unsigned long long (WINAPI*)()>(
        GetProcAddress(dxgi_proxy, "EDPE_ObservedPresentCount"));
    if (!present_count) return 7;
    const auto menu_visible = reinterpret_cast<BOOL (WINAPI*)()>(
        GetProcAddress(dxgi_proxy, "EDPE_MenuVisible"));
    if (!menu_visible) return 7;
    IDXGIFactory1* factory = nullptr;
    const HRESULT factory_result = create_factory(__uuidof(IDXGIFactory1),
        reinterpret_cast<void**>(&factory));
    if (FAILED(factory_result) || !factory) return 8;
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
    SendMessageW(window, WM_KEYDOWN, 'A', 0);
    const bool hidden_input_passed = forwarded_keys == 1;
    SendMessageW(window, WM_KEYDOWN, VK_INSERT, 0);
    const bool opened = menu_visible();
    const HRESULT overlay_present = swap_chain->Present(0, 0);
    SendMessageW(window, WM_KEYDOWN, 'A', 0);
    const bool visible_input_blocked = forwarded_keys == 1;
    const HRESULT resize_result = swap_chain->ResizeBuffers(0, 128, 128, DXGI_FORMAT_UNKNOWN, 0);
    const HRESULT resized_present = SUCCEEDED(resize_result) ? swap_chain->Present(0, 0) : resize_result;
    SendMessageW(window, WM_KEYUP, VK_INSERT, 0);
    SendMessageW(window, WM_KEYDOWN, VK_INSERT, 0);
    const bool closed = !menu_visible();
    SendMessageW(window, WM_KEYUP, VK_INSERT, 0);
    SendMessageW(window, WM_KEYDOWN, 'A', 0);
    const bool hidden_input_restored = forwarded_keys == 2;
    SendMessageW(window, WM_KEYDOWN, VK_F5, 0);
    const bool f5_opened = menu_visible();
    SendMessageW(window, WM_KEYDOWN, VK_F5, 1LL << 30);
    const bool f5_repeat_ignored = menu_visible();
    SendMessageW(window, WM_KEYUP, VK_F5, 0);
    SendMessageW(window, WM_KEYDOWN, VK_F5, 0);
    const bool f5_closed = !menu_visible();
    SendMessageW(window, WM_KEYUP, VK_F5, 0);
    swap_chain->Release();
    if (factory) factory->Release();
    DestroyWindow(window);
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
    context->Release();
    device->Release();
    FreeLibrary(dxgi_proxy);
    const HANDLE log = CreateFileW(argv[3], GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) return 11;
    char contents[4096]{};
    DWORD bytes_read = 0;
    const BOOL read = ReadFile(log, contents, sizeof(contents) - 1, &bytes_read, nullptr);
    CloseHandle(log);
    const bool passed = SUCCEEDED(present_result) && SUCCEEDED(second_present_result) && observed &&
        SUCCEEDED(first_real_present) && SUCCEEDED(overlay_present) &&
        SUCCEEDED(resize_result) && SUCCEEDED(resized_present) && opened && closed &&
        f5_opened && f5_repeat_ignored && f5_closed &&
        hidden_input_passed && visible_input_blocked && hidden_input_restored &&
        read && std::strstr(contents, "EDPE: Present swapchain=") &&
        std::strstr(contents, "EDPE: Dear ImGui ready");
    if (!passed) std::fprintf(stderr,
        "present=%08lx/%08lx real=%08lx overlay=%08lx resize=%08lx/%08lx observed=%d menu=%d/%d f5=%d/%d/%d input=%d/%d/%d read=%d\n",
        present_result, second_present_result, first_real_present, overlay_present,
        resize_result, resized_present, observed, opened, closed,
        f5_opened, f5_repeat_ignored, f5_closed,
        hidden_input_passed, visible_input_blocked, hidden_input_restored, read);
    return passed ? 0 : 12;
}
