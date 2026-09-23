# EDPE — Elite Dangerous Performance Enhanced

EDPE targets the 2D Direct3D 11 renderer of Elite Dangerous. `EDPE.dll` forwards `D3D11CreateDevice`; the experimental `dxgi.dll` shim observes factories, swapchains, and `Present`. Observation reports the first swapchain's dimensions, format, device, and immediate context. The game, 3D world, HUD, and F5 menu have been verified to display normally with the current read-only diagnostics.

The shims also append creation and first-Present diagnostics to `edpe.log` beside the host executable. File logging failure leaves the original graphics calls intact.

The experimental Dear ImGui status window toggles with `F5`. When hidden, other keyboard and mouse messages, including `Insert`, pass to the original window procedure. Temporal options are disabled until their inputs are verified.

Build from an x64 MSVC developer PowerShell:

```powershell
cmake -S . -B build -G "NMake Makefiles" "-DCMAKE_MAKE_PROGRAM=$((Get-Command nmake).Source)"
cmake --build build
ctest --test-dir build --output-on-failure
```

The outputs are `build/EDPE.dll` and `build/dxgi.dll`. For the current local game test, copy `EDPE.dll` as `d3d11.dll` and copy `dxgi.dll` beside `EliteDangerous64.exe` after each successful build. The proxy covers the executable's observed static imports and the system D3D11 dependency on `CreateDXGIFactory2`. Long-session stability and other graphics configurations remain unverified.

See [renderer observations](docs/render-pipeline-observations.md) for verified findings and the next Phase 1 checks.
