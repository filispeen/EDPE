#include <d3d11.h>
#include <windows.h>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
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
    return SUCCEEDED(result) && device && context ? 0 : 4;
}
