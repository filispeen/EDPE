#include <d3d11.h>
#include <dxgi1_6.h>
#include <windows.h>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) return 1;
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

    const HWND window = CreateWindowExW(0, L"STATIC", L"EDPE test", WS_OVERLAPPEDWINDOW,
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
    swap_chain->Release();
    if (factory) factory->Release();
    DestroyWindow(window);
    context->Release();
    device->Release();
    FreeLibrary(dxgi_proxy);
    return SUCCEEDED(present_result) && SUCCEEDED(second_present_result) && observed ? 0 : 11;
}
