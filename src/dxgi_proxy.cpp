#include <dxgi.h>
#include <windows.h>

void ObserveDxgiFactory(REFIID iid, void* factory);

extern "C" HRESULT WINAPI EdpeCreateDXGIFactory1(REFIID iid, void** factory) {
    static const HMODULE system_dxgi = LoadLibraryExW(
        L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!system_dxgi) return HRESULT_FROM_WIN32(GetLastError());

    static const auto create_factory = reinterpret_cast<decltype(&CreateDXGIFactory1)>(
        GetProcAddress(system_dxgi, "CreateDXGIFactory1"));
    if (!create_factory) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    const HRESULT result = create_factory(iid, factory);
    if (SUCCEEDED(result) && factory && *factory) ObserveDxgiFactory(iid, *factory);
    OutputDebugStringW(SUCCEEDED(result) ? L"EDPE: DXGI factory created\n"
                                        : L"EDPE: DXGI factory creation failed\n");
    return result;
}
