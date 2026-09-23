# EDPE — Elite Dangerous Performance Enhanced

EDPE targets the 2D Direct3D 11 renderer of Elite Dangerous. `EDPE.dll` forwards `D3D11CreateDevice`; the experimental `dxgi.dll` shim forwards `CreateDXGIFactory1`. Both report creation through debugger output. Neither hooks `Present` or changes rendered output.

Build from an x64 MSVC developer PowerShell:

```powershell
cmake -S . -B build -G "NMake Makefiles" "-DCMAKE_MAKE_PROGRAM=$((Get-Command nmake).Source)"
cmake --build build
ctest --test-dir build --output-on-failure
```

The outputs are `build/EDPE.dll` and `build/dxgi.dll`. There is currently no supported game installation package. Do not place either DLL beside the game yet: each shim only implements the executable's observed static import, and compatibility with other modules is untested.

See [renderer observations](docs/render-pipeline-observations.md) for verified findings and the next Phase 1 checks.
