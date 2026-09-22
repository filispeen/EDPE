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
