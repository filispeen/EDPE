#include <d3d11.h>
#include <dxgi.h>
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
    if (context) context->Release();
    if (device) device->Release();
    FreeLibrary(proxy);
    if (FAILED(result) || !device || !context) return 4;

    const HMODULE dxgi_proxy = LoadLibraryW(argv[2]);
    if (!dxgi_proxy) return 5;
    const auto create_factory = reinterpret_cast<decltype(&CreateDXGIFactory1)>(
        GetProcAddress(dxgi_proxy, "CreateDXGIFactory1"));
    if (!create_factory) return 6;
    IDXGIFactory1* factory = nullptr;
    const HRESULT factory_result = create_factory(__uuidof(IDXGIFactory1),
        reinterpret_cast<void**>(&factory));
    if (factory) factory->Release();
    FreeLibrary(dxgi_proxy);
    return SUCCEEDED(factory_result) && factory ? 0 : 7;
}
