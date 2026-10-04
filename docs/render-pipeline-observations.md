# EDPE render-pipeline observations

Date: 2026-10-05. Target: Elite Dangerous 2D; installed Odyssey build `2026.09.03.332841`.

## Repository state

- **VERIFIED (workspace inspection):** This repository contains `AGENTS.md`, `PLAN.md`, `sPROMPT.md`, and substantial C++ source. Build system: CMake + NMake, MSVC 14.44 x64, Windows SDK 10.0.26100.0. Git: 216 commits beyond initial `v0.0.1`, current HEAD at `6683ff7`. EDPE.dll and dxgi.dll built and deployable.
- **VERIFIED (local toolchain):** CMake 4.4.2, MSVC 14.44 x64, Windows SDK 10.0.26100.0 installed. ELITE executable tested via WARP and local harness.
- **VERIFIED (in-game load):** EDPE dxgi.dll + d3d11.dll deployed beside `EliteDangerous64.exe`. Game launches with DXGI factory creation, D3D11 device creation, first `Present` on 2560×1440 swapchain `R8G8B8A8_UNORM`, and Dear ImGui overlay (`F5` toggles). 3D world and HUD display normally; no black screen.
- **UNVERIFIED:** Full temporal reconstruction pipeline (DLSS/FSR), render-scale control below native, NGX/DLSS Super Resolution integration, HUD separation at native resolution, Frame Generation. Depth Z-convention (standard vs reversed) not independently verified for 2D scene. Motion-vector correctness at render resolution not validated against NGX conventions.

## Repository history

- **v0.0.1** (initial): Added `AGENTS.md` and `PLAN.md` project roadmap.
- **216 commits** beyond v0.0.1 spanning: D3D11 proxy/hook, Present observation, depth bind census, motion-vector generation, projection jitter, ImGui overlay, NGX DLL presence, and extensive diagnostics.
- **Current HEAD** (`6683ff7`): Probe Elite world shader fanout, retain pixel shader bytecode, isolate EDVR DXBC color fanout, document post-HUD world draws, count post-HUD draws, document in-game glass replay and snapshot layout, arrange diagnostic snapshot windows, accept idempotent back-face glass stencil replay, save one-shot diagnostic frames as BMP, log rejected glass depth state.

## Reference findings

- **REFERENCE-CODE:** [EDVR's depth probe](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/d3d11/depth_probe.cpp) treats its observed VR depth as reversed Z and uses a depth copy for inspection. That observation is specific to EDVR's measured VR path; EDPE must measure the 2D path independently.
- **REFERENCE-CODE:** [EDVR's temporal pass](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/d3d11/temporal_pass.cpp), [temporal math](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/common/temporal_math.h), [DLAA implementation](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/d3d11/dlaa.cpp), [native temporal path](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/d3d11/native_temporal.cpp), and [per-object motion notes](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/docs/per-object-motion.md) provide candidates for depth, motion, jitter, history, and fallback design. Their eye-specific rendering and runtime code do not transfer to 2D.
- **REFERENCE-CODE:** [EDVR's FSR implementation](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/d3d11/fsr3_engine.cpp) identifies its D3D11 route as the metarutaiga community port, hardened by OptiScaler, with MIT provenance stated there. Confirm exact source and license before any reuse.
- **SDK-DOCUMENTED:** [NVIDIA's DLSS SDK](https://github.com/NVIDIA/DLSS) includes D3D11 NGX support. Its [current definitions](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_defs.h) list presets J, K, L, and M. EDPE must verify input conventions against the exact SDK chosen for implementation.
- **SDK-DOCUMENTED:** [AMD's FidelityFX API guide](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/getting-started/ffx-api.md) describes the current effect DLLs and a D3D12 backend. [FSR 3.1 migration guidance](https://gpuopen.com/manuals/fidelityfx_sdk/getting-started/migrating-to-fsr-3-1/) separates upscaling from frame generation and lists D3D12/Vulkan backends. The D3D11 port and an official D3D12 bridge remain candidates to measure later.
- **SDK-DOCUMENTED:** [Dear ImGui](https://github.com/ocornut/imgui) `v1.92.9b` is pinned as a Git submodule including Win32 and DX11 backends. [Streamline's FG guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md) requires capability queries and Reflex integration; FG is outside Phase 1.
- **NVIDIA-DOCUMENTED (2026-09-27):** [NVIDIA D3D11 DLAA input contract](docs/nvidia-dlaa-input-contract.md) measured NGX initialization success, capability queries, and identified input conventions. **BLOCKED FOR RUNTIME:** NGX shutdown hangs >20s; SDK logging confirms `DestroyParameters` success but shutdown stall cause unknown. NGX must not consume diagnostic captures as production inputs until input conventions are verified in Elite.

## First vertical slice — current status

The project has completed Phase 1 (D3D11 injection, Present hook, Dear ImGui) and Phase 2-5 (resource census, depth analysis, camera/projection, projection jitter, motion vectors). The next smallest coherent step is **NGX/DLSS Super Resolution integration**, beginning with **NVIDIA DLAA at native resolution** (input == output) to validate temporal inputs without debugging render-scale behavior.

Per `PLAN.md` expected order: `stable D3D11 injection -> temporal inputs -> DLAA -> DLSS SR -> FSR SR -> HUD separation -> D3D11/D3D12 interop -> Frame Generation`.

**First vertical slice goal:** Initialize NGX, validate DLAA at native resolution, and document input conventions at the NGX boundary.

## Installed game inspection — 2026-10-05

Game build: `2026-09-03.332841` (`EliteDangerous64.exe` file version `332841`, product version `4.4.1.1`). Executable SHA-256: `E6BE8BBE04E6A7AE226D4318945AF7F367DE13DC5A007A261964D9BA8144E988`.

- **VERIFIED (PE imports):** The x64 executable imports `D3D11CreateDevice` from `d3d11.dll` and `CreateDXGIFactory1` from `dxgi.dll`. It also delay-loads `openvr_api.dll`.
- **VERIFIED (directory inspection):** Local `d3d11.dll` and `dxgi.dll` copied beside executable. No active 3Dmigoto or EDVR binary present at deployment.
- **VERIFIED (in-game observation):** EDPE dxgi.dll + d3d11.dll deployed. Game launches with DXGI factory creation, D3D11 device creation, first `Present` on 2560×1440 swapchain `DXGI_FORMAT_R8G8B8A8_UNORM`. Dear ImGui overlay visible and toggles with `F5`. 3D world and HUD display normally; no black screen or visible rendering fault.

## Local proxy check — current

- **VERIFIED:** `EDPE.dll` exports `EdpeD3D11CreateDevice` and `EdpeCreateDXGIFactory1` / `EdpeCreateDXGIFactory2`. Exports forward to system32 DLLs. Proxy observes `CreateSwapChain` and `Present` via DXGI vtable hooks. `Insert` key reaches original WndProc when menu hidden.
- **LIMIT:** Export set covers static imports only. Other modules may request additional D3D11 exports not yet hooked. Present observation and normal 2D rendering verified in-game; long-session stability unverified.

## DXGI factory check — current

- **VERIFIED:** `dxgi.dll` shim exports `CreateDXGIFactory1` and `CreateDXGIFactory2` (forwarded to system32). Factory observation working; both factory exports create valid factory objects.
- **LIMIT:** Additional DXGI exports and coexistence with other graphics mods remain untested.

## Dear ImGui dependency — current

- **SDK-DOCUMENTED:** Dear ImGui `v1.92.9b` MIT-licensed, Win32 + DX11 backends included.
- **MEASURED (binary size):** Release `dxgi.dll` grew from 17,920 to 404,992 bytes after linking Win32/DX11 overlay (387,072-byte increase). GPU cost and in-game frame impact measured after integration.
- **LIMIT:** In-game F5 interaction verified; long-session stability not measured.

## NVIDIA DLAA input contract — measured conventions (2026-09-27)

Per `docs/nvidia-dlaa-input-contract.md`, the following was measured in an isolated SDK probe (not in Elite):

- **NGX initialization:** `NVSDK_NGX_D3D11_Init_with_ProjectID` with custom GUID-like project ID and `NVSDK_NGX_ENGINE_TYPE_CUSTOM` returned success (`0x00000001`).
- **Capability query:** `GetCapabilityParameters` returned success; `SuperSampling.Available` returned 1 when feature DLL search path pointed at SDK `Windows_x86_64/rel` directory, 0 otherwise.
- **Shutdown:** SDK probe hung >20s in `NVSDK_NGX_D3D11_Shutdown1`; cause unknown. **NGX must not be used in Elite runtime until lifecycle is understood.**
- **Input conventions (SDK-documented):** 
  - `NGX_D3D11_CREATE_DLSS_EXT`: sets input width/height, output width/height, quality value, and creation flags. Native-resolution DLAA must use equal input and output dimensions.
  - `NGX_D3D11_EVALUATE_DLSS_EXT`: binds distinct color, output, depth, and motion resources. Jitter described in **input/render pixels**. Motion-vector scale converts stored values to pixel space; when either scale is zero, helper substitutes `1.0`.
  - `InReset=1`: requests a history reset when the scene changes completely. Structure also accepts exposure texture and current-color bias mask; transparency-mask field unused/reserved.
  - Creation flags: `IsHDR`, `MVLowRes`, `MVJittered`, `DepthInverted`, `AutoExposure`. Each flag needs evidence from the resources and conventions actually submitted. The definitions alone do not establish motion direction, Y sign, or the correct flag combination for Elite.

## EDPE readiness — verified / measured

- **VERIFIED / MEASURED:** A one-shot `2560×1440` D3D11 capture associated depth, HDR color, and GPU camera-depth motion with the same observed `Present` interval. Copied HDR color visibly includes the HUD. WARP tests check the EDPE shader's current-to-previous motion in render pixels; in-game snapshot shows finite motion during camera movement.
- **EXPERIMENTAL:** The depth/camera match is based on observed bind order and sparse samples. Per-pixel color/depth correspondence, scene resets, exposure, moving-object motion, and the optimal evaluation point remain unverified. The motion preview omits HUD shapes while HDR color contains them, so the current pair is not a complete temporal description of HUD.
- **BLOCKED FOR RUNTIME DLAA:** Read-only captures identify a camera-buffer write before the main scene pass. A one-frame shader census found that 248/251 draws in its heavy pass used the observed projection block for clip X/Y/W, while three used a separate, transposed copy in VS slot 0. **Other scenes and pass orders, plus a guaranteed way to remove jitter if evaluation fails, remain unverified. The current Halton sequence must remain inactive in Elite. NGX must not consume diagnostic captures as production inputs until that safety condition and the input conventions are verified.**
- **Implication:** Keep EDPE's scene observations and motion generation backend-neutral. Translate measured EDPE conventions at the future NGX boundary; do not infer them from EDVR or from a visually plausible snapshot.

## NGX integration plan — next coherent step

Following the nvidia-dlaa-input-contract.md implications and PLAN.md expected order, the next smallest milestone is:

1. **NGX initialization** - Use `NVSDK_NGX_D3D11_Init_with_ProjectID` with a custom project ID (not EDVR's). Verify capability parameters.
2. **DLAA at native resolution** - Input width == output width, input height == output height (2560×1440). This validates depth, motion, jitter, and history without debugging render-scale.
3. **Convention translation** - At the NGX boundary, translate EDPE's measured conventions:
   - Jitter: EDPE uses Halton sequence in **render pixels**; NGX expects jitter in **input/render pixels** (per SDK helper). Must match sign and scale.
   - Motion vectors: EDPE generates camera+depth motion in **render-resolution pixels** (DXGI_FORMAT_R16G16_FLOAT). NGX expects MV scale conversion (scale X/Y may be zero, substituted as 1.0).
   - Depth: EDPE depth format observed as `R32G8X24_TYPELESS` / `D32_FLOAT_S8X24_UINT` or `R24G8_TYPELESS` / `D24_UNORM_S8_UINT`. NGX expects depth convention evidence (`DepthInverted` flag).
   - Reset: EDPE camera/projection changes detected via constant-buffer inspection. NGX `InReset=1` requests history reset on complete scene change.
4. **Safe fallback** - If any convention cannot be validated, disable DLSS/DLAA, disable jitter, restore original frame, Present normally.

**Do not proceed to reduced render resolution** until DLAA at native resolution is visually stable across: cockpit, station, space, yaw, pitch, fast rotation, HUD, moving external ships.

## Documentation to create/update

- `docs/nvidia-dlaa-input-contract.md` - Already exists with measured conventions; keep updated as Elite inputs are verified.
- `docs/render-pipeline-observations.md` - This file (currently being updated).
- Consider: `docs/dlss-implementation-plan.md` - High-level plan for DLSS SR after DLAA validation.

## Performance & failure behavior

- **Forbidden in hot path:** blocking GPU readback, `Map()` on full-resolution textures, blocking `GetData` loops, per-frame NGX feature recreation.
- **Fail open:** If NGX/DLAA fails, disable upscaler, disable jitter, restore original frame, Present normally. Never black-screen Elite.
- **Async timing:** Use asynchronous D3D11 timestamp queries for NGX DLSS, motion-vector generation, and total EDPE GPU cost.