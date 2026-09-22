# EDPE — Elite Dangerous Performance Enhanced

EDPE targets the 2D Direct3D 11 renderer of Elite Dangerous. The current DLL is a build and loading smoke test; it does not intercept D3D11 or change rendered output.

Build from an x64 MSVC developer PowerShell:

```powershell
cmake -S . -B build -G "NMake Makefiles" "-DCMAKE_MAKE_PROGRAM=$((Get-Command nmake).Source)"
cmake --build build
```

The output is `build/EDPE.dll`. Loading it emits `EDPE loaded` to the Windows debugger output. There is currently no supported game loading or installation route.

See [renderer observations](docs/render-pipeline-observations.md) for verified findings and the next Phase 1 checks.
