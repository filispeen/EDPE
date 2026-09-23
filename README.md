# EDPE — Elite Dangerous Performance Enhanced

EDPE targets the 2D Direct3D 11 renderer of Elite Dangerous. The current DLL forwards `D3D11CreateDevice` to the system DLL and reports creation through debugger output. It does not hook `Present` or change rendered output.

Build from an x64 MSVC developer PowerShell:

```powershell
cmake -S . -B build -G "NMake Makefiles" "-DCMAKE_MAKE_PROGRAM=$((Get-Command nmake).Source)"
cmake --build build
ctest --test-dir build --output-on-failure
```

The output is `build/EDPE.dll`. There is currently no supported game installation package. Do not rename or place the DLL beside the game yet: the proxy only implements the executable's observed D3D11 import, and compatibility with other modules is untested.

See [renderer observations](docs/render-pipeline-observations.md) for verified findings and the next Phase 1 checks.
