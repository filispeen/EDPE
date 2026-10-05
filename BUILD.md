# Building EDPE

## Requirements

- Windows 10/11 x64.
- Visual Studio 2022 with the **Desktop development with C++** workload, including the x64 MSVC toolchain and Windows SDK.
- CMake 3.25 or newer.
- NMake, included with Visual Studio. Run the commands below in **x64 Native Tools PowerShell for VS 2022** or Developer PowerShell with the x64 MSVC environment enabled.
- The `external/imgui` Git submodule (Dear ImGui) initialized.

The project uses C++20 and the Direct3D 11/DXGI headers and libraries from the Windows SDK. CMake also links the system `d3dcompiler` library. The project does not currently download or link the NVIDIA NGX or AMD FidelityFX SDK; NGX integration is still a stub.

## Initialize the dependency

After cloning the repository, fetch Dear ImGui:

```powershell
git submodule update --init --recursive
```

Alternatively, use the repository setup script from PowerShell to install Git, CMake 3.25+, Visual Studio 2022 Build Tools with the C++ workload/recommended Windows SDK components, and initialize the ImGui submodule:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-build-requirements.ps1
```

The script requires `winget` (App Installer) and may request administrator approval during installation. It skips Git and CMake installation when Git is available and CMake 3.25+ is already on `PATH`. It adds the C++ workload to an existing Visual Studio 2022 installation when found. After it finishes, open a new x64 Developer PowerShell so the MSVC and NMake environment is loaded.

## Build

From the repository root in an x64 Developer PowerShell:

```powershell
cmake -S . -B build -G "NMake Makefiles" "-DCMAKE_MAKE_PROGRAM=$((Get-Command nmake).Source)"
cmake --build build
```

The main DLLs are:

```text
build/EDPE.dll
build/dxgi.dll
```

The `proxy_stage` target is built along with the other default targets and stages runtime files here:

```text
build/stage/d3d11.dll       # copy of EDPE.dll under the proxy filename
build/stage/dxgi.dll
build/stage/proxy_smoke.exe
```

For subsequent builds using the configured `build` directory, run `cmake --build build` from the same x64 Developer PowerShell.

## Tests

After a successful build, run:

```powershell
ctest --test-dir build --output-on-failure
```

The current tests cover temporal math, the GPU motion pass, DXBC fanout, and proxy smoke checks. The GPU test requires Windows and an available D3D11 device.

## NGX project ID

`src/ngx_context.cpp` and `tools/ngx_probe/ngx_probe.cpp` each define `kNgxProjectId`, an EDPE-owned GUID for `NVSDK_NGX_D3D11_Init_with_ProjectID` (not EDVR's ID). `NgxContext` remains a disabled stub: it makes no NGX calls.

## Staging proxies for a local game run

The README describes a manual test that copies `EDPE.dll` as `d3d11.dll` and places it alongside `dxgi.dll` next to `EliteDangerous64.exe`. The build and tests do not deploy these files automatically. Use only DLLs produced by a successful build.
