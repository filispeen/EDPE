# EDPE render-pipeline observations

Date: 2026-09-23. Target: Elite Dangerous 2D, game build unknown.

## Repository state

- **VERIFIED (workspace inspection):** This repository contains `AGENTS.md`, `PLAN.md`, and `sPROMPT.md` only. There is no source, README, build configuration, bundled SDK, injection mechanism, Git commit, or Git tag. `.codegraph/` is absent.
- **VERIFIED (local toolchain):** CMake 4.4.2, MSVC 14.44 x64, and Windows SDK 10.0.26100.0 are installed. No Elite executable or capture is in this workspace.
- **UNVERIFIED:** The 2D game's swapchain, backbuffer, depth, camera, projection, render scale, presentation mode, and any compatible DLL loading path. No game process or graphics capture was observed.

## Reference findings

- **REFERENCE-CODE:** [EDVR's depth probe](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/d3d11/depth_probe.cpp) treats its observed VR depth as reversed Z and uses a depth copy for inspection. That observation is specific to EDVR's measured VR path; EDPE must measure the 2D path independently.
- **REFERENCE-CODE:** [EDVR's temporal pass](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/d3d11/temporal_pass.cpp), [temporal math](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/common/temporal_math.h), [DLAA implementation](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/d3d11/dlaa.cpp), [native temporal path](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/d3d11/native_temporal.cpp), and [per-object motion notes](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/docs/per-object-motion.md) provide candidates for depth, motion, jitter, history, and fallback design. Their eye-specific rendering and runtime code do not transfer to 2D.
- **REFERENCE-CODE:** [EDVR's FSR implementation](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/d3d11/fsr3_engine.cpp) identifies its D3D11 route as the metarutaiga community port, hardened by OptiScaler, with MIT provenance stated there. Confirm exact source and license before any reuse.
- **SDK-DOCUMENTED:** [NVIDIA's DLSS SDK](https://github.com/NVIDIA/DLSS) includes D3D11 NGX support. Its [current definitions](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_defs.h) list presets J, K, L, and M. EDPE must verify input conventions against the exact SDK chosen for implementation.
- **SDK-DOCUMENTED:** [AMD's FidelityFX API guide](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/getting-started/ffx-api.md) describes the current effect DLLs and a D3D12 backend. [FSR 3.1 migration guidance](https://gpuopen.com/manuals/fidelityfx_sdk/getting-started/migrating-to-fsr-3-1/) separates upscaling from frame generation and lists D3D12/Vulkan backends. The D3D11 port and an official D3D12 bridge remain candidates to measure later.
- **SDK-DOCUMENTED:** [Dear ImGui](https://github.com/ocornut/imgui/blob/master/docs/EXAMPLES.md) supplies Win32 and DX11 backends. [Streamline's FG guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md) requires capability queries and Reflex integration; FG is outside Phase 1.

## First vertical slice

1. Build an x64 `EDPE.dll` with a minimal, safe process-attach entry point. This proves the toolchain and establishes the project artifact only.
2. Determine a compatible loading route before replacing a game DLL. EDVR's [installation notes](https://github.com/characterecho-sean/edvr-unofficial-patch) show that another mod may already own `d3d11.dll`; overwriting it would be unsafe.
3. Observe D3D11 device and swapchain creation, then log `Present`, backbuffer description, and device/context identity without changing output.
4. Add Dear ImGui with Win32/DX11 after the pass-through path is verified. Confirm hidden input behavior and safe resize/shutdown.

The `v0.0.1` tag requires a tested in-game load, normal rendering, Present observation, device/context discovery, ImGui toggle, and logging. No current measurement meets that gate.

## Installed game inspection — 2026-09-23

Game build: `2026.09.03.332841` (`EliteDangerous64.exe` file version `332841`, product version `4.4.1.1`). Executable SHA-256: `E6BE8BBE04E6A7AE226D4318945AF7F367DE13DC5A007A261964D9BA8144E988`.

- **VERIFIED (PE imports):** The x64 executable imports `D3D11CreateDevice` from `d3d11.dll` and `CreateDXGIFactory1` from `dxgi.dll`. It also delay-loads `openvr_api.dll`. This identifies possible observation entry points, not when or how the 2D scene presents.
- **VERIFIED (directory inspection):** Neither an active local `d3d11.dll` nor `dxgi.dll` is present. A `d3d11.dll.disabled` file is 3Dmigoto 1.3.16, and backup folders contain both 3Dmigoto and EDVR D3D11 proxies. The existing `edvr.ini` names `d3d11_edhm.dll` as a chained real DLL, but that file is not currently beside the executable.
- **VERIFIED (existing EDVR diagnostic):** An EDVR breadcrumb records its D3D11 proxy loading, a chained proxy call, hook arming, frames, and process exit in an earlier session. This supports the local-proxy route for that session only. It does not validate EDPE or the 2D renderer.
- **IMPLICATION:** Build and test EDPE's proxy in isolation before installation. Never overwrite the disabled 3Dmigoto binary or EDVR backups. Recheck the live directory immediately before any installation because mod tools can change it.

Still unknown: which swapchain corresponds to the 2D scene, actual `Present` timing, device/context identity, backbuffer description, and normal-rendering compatibility with EDPE.

## Local proxy check — 2026-09-23

- **VERIFIED (local test):** `EDPE.dll` exports `D3D11CreateDevice` and forwards it to the system D3D11 DLL loaded from System32. A WARP device and immediate context were created through the export, released, and the test passed.
- **LIMIT:** The export set currently covers only the executable's static D3D11 import. The DLL has not been placed beside the game; other modules may request additional D3D11 exports. Present observation and normal 2D rendering remain unverified.

## DXGI factory check — 2026-09-23

- **VERIFIED (local test):** A separate `dxgi.dll` shim exports the executable's observed `CreateDXGIFactory1` import and forwards it to System32. A test created and released an `IDXGIFactory1` through the shim.
- **LIMIT:** The factory is currently returned unchanged. Neither `CreateSwapChain` nor `Present` is intercepted. Additional DXGI exports and coexistence with other graphics mods remain untested; the shim is not ready to install.
