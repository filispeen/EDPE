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

- `docs/render-passes.md` - Complete reference of all observed engine passes and custom EDPE render passes.
- `docs/nvidia-dlaa-input-contract.md` - Already exists with measured conventions; keep updated as Elite inputs are verified.
- `docs/render-pipeline-observations.md` - This file (currently being updated).
- Consider: `docs/dlss-implementation-plan.md` - High-level plan for DLSS SR after DLAA validation.

## In-game capture findings (2026-10-15)

- **Date:** 2026-10-15. Target: Elite Dangerous Odyssey 2D, build `2026.09.03.332841`.
- **Method:** Non-blocking D3D11 staging readback (`D3D11_MAP_FLAG_DO_NOT_WAIT`) saving 2560×1440 32-bit BMP snapshots to `edpe-captures/`.
- **VERIFIED (Depth DSV #2):** `depth-dsv2-*.bmp` decoded from `DXGI_FORMAT_R32G8X24_TYPELESS` with contrast mapping $z / (z + 0.001)$ yields pure monochrome geometry across the frame (values 35 to 177, average 90.7), confirming DSV #2 as the primary scene depth buffer.
- **VERIFIED (Emissive / Glow buffer — `early-color`):** In-game visual inspection of `early-color-*.bmp` (captured from MRT RTV3 `DXGI_FORMAT_R11G11B10_FLOAT` at first RTV3 exit) reveals it is **not space**, but the engine's **emissive / illumination pass**. Non-emissive scene background and dark areas are zero (RGB = 0, 0, 0), while lamps, emissive cockpit indicators, lights, and glowing elements are isolated and bright (RGB up to 254).
- **Implication for Temporal Upscaling:**
  - This pass provides a direct, native candidate for constructing the **Reactive Mask** (`TemporalFrameInputs::reactiveMask`) required by both AMD FSR (Native AA / SR) and NVIDIA DLSS.
  - Feeding this emissive information to the upscaler allows high reactivity on cockpit lights, laser effects, and ship lamps, directly preventing ghosting and smearing on glowing elements without expensive per-pixel luminance heuristics.
- **VERIFIED (Clean Backbuffer):** `frame-*.bmp` captured immediately before `ImGui_ImplDX11_RenderDrawData` provides an overlay-free native frame capture.
- **VERIFIED (Motion Neutral):** `motion-*.bmp` decoded from `DXGI_FORMAT_R16G16_FLOAT` verifies $(128, 128, 128)$ neutral gray encoding during zero camera motion.

## Performance & failure behavior

- **Forbidden in hot path:** blocking GPU readback, `Map()` on full-resolution textures, blocking `GetData` loops, per-frame NGX feature recreation.
- **Fail open:** If NGX/DLAA fails, disable upscaler, disable jitter, restore original frame, Present normally. Never black-screen Elite.
- **Async timing:** Use asynchronous D3D11 timestamp queries for NGX DLSS, motion-vector generation, and total EDPE GPU cost.

## On-foot viewmodel & 208B CB0 findings (2026-11-16, Issue #11)

- **Date:** 2026-11-16. Source: vanilla Odyssey RenderDoc capture `edo(issue11).rdc` (2560×1440, 8642 actions).
- **Method:** RenderDoc `pixel_history`, `get_buffer_data`, `get_shader_bindings`, and `disassemble_shader` across Z-prepass (`6652–7126`), G-buffer (`7158–14617`), and sky/planet/atmosphere passes (`15736–16208`).
- **VERIFIED (On-foot viewmodel ordering & Z-prepass vs G-buffer):**
  - Event `6652` (on-foot weapon/tool Z-prepass, `depth = 0.248466` at `(2000, 1100)`) runs **before** the main world Z-prepass (`6796`, `scaleX=1.0525, scaleY=1.871, depthB=0.025`) using a viewmodel sub-pass projection (`scaleX=1.2935, scaleY=2.300, depthB=0.0675` in `cb1[270..273]`).
  - G-buffer draws `7189` and `14617` (`scaleX=1.2935, depthB=0.0675`) shade against Z-prepass depth (`6652`) with `DepthFunc == EQUAL`; `16208` (post-deferred viewmodel atmosphere reading linearized depth `8724`) also uses `scaleX=1.2935, depthB=0.0675` in both `cb0` (`208` bytes) and `cb1`.
  - **Confidence:** High (directly observed in capture `cb0`/`cb1` buffers and pixel history).
  - **Implication:** Per-frame jitter decisions for viewmodel sub-passes must lock on first encounter (so Z-prepass, G-buffer, and post-deferred agree), and sub-pass writes must never overwrite `s_last_unjittered_forward_block` / `s_last_clean_cb1_data` used by world `depthB=0.025` deferred `CB2` repair and HUD replay.
- **VERIFIED (208-byte `cb0` celestial/scaled ModelViewProj):**
  - Planet (`15736`), scaled planet atmosphere (`16044`, model scale `492878.0`), and viewmodel atmosphere (`16208`) all bind `cb0[4..7]` (`words[16..31]` of 208-byte `cb0`) as `transpose(Model * ViewProj)` with `cb0[6]` (`words[24..27]`) equal to `(0, 0, 0, depthB)` and non-zero translation/scale.
  - **Confidence:** High (verified via shader disassembly `8628fdf1` / `5ea854d6` and buffer contents).
  - **Implication:** Validate `cb0[6]` = `(0, 0, 0, depthB)`, normalized FOV scales `len0/lenW` and `len1/lenW`, orthogonality, and positive right-handed determinant rather than requiring unit `lenW == 1` and zero translation.
- **VERIFIED (Planetary rings pass order & Preserve Native UI isolation):**
  - Planetary rings (`15837, 15851, 15866, 15880, 15935, 15949, 15964, 15978`, `5334` vertices with `4096x1` ring LUTs, plus planet passes `15897` / `15913` / `15919`) render into linear HDR `8728` against depth `8723` **before** atmospheric transmittance (`16044`) and sky scattering (`16137`).
  - Diverting any pre-sky depth-tested world shader (`0xB12F7A618E1BDE98 / 0x42AC0CACC9CDF72B`, `0x5559BD94B6852E83`, or unlocked space dust) into `HdrLayer` traps its low linear HDR color and `< 1` transmittance in `HdrLayer` while `16137` brightens the sky behind it, darkening the ring band over sky and planet when `UiLayer::composite` runs.
  - **Confidence:** High (verified via `edo(issue11).rdc` `pixel_history` at `(1660, 280)` and `(1715, 340)`, shader disassembly, and `shot1`/`shot2`/`shot3`).
  - **Implication:** Exclude `0x5559BD94B6852E83` and `0xB12F7A618E1BDE98` from `isHudShaderPair` / `UiLayer::shouldInterceptDraw`, require confirmed ship-cockpit holo shaders (`isConfirmedShipCockpitHudPair`) before unlocking cockpit-only shaders (`kSpaceDust` / `kNormalSpaceDust` / `0xB7790CBF`), and guard `captureEarlyMotionColor` against overwriting an already-latched `from_hud` snapshot in the same frame.


## On-foot Helmet Visor Fullscreen Pass (`EID 18188`, `edo(issue11).rdc`, 2026-11-16)
- **Method:** RenderDoc pipeline inspection, `pixel_history`, and DXBC disassembly of `F:\Code\C++\EDPE\edo(issue11).rdc` (`2560x1440`, Odyssey on-foot settle).
- **Evidence:**
  - `EID 18098`: `CopySubresourceRegion` copies clean pre-visor HDR `8728` (`R11G11B10_FLOAT`) into `8731`.
  - `EID 18120..18162`: `DrawInstanced(3, 1)` builds downsampled blurred HDR `8698` (`320x180`).
  - `EID 18188`: `DrawInstanced(3, 1)` (`TriangleStrip`, `SV_VERTEXID`, no vertex buffer) renders the curved helmet visor rim streaks, frost/droplets, and visor tint onto `RTV0 = 8728` (`DSV = 8723`, `stencil: 0 -> 4`, dual-source outputs `o0` + `o1`):
    - `VS ResourceId::110929` (`b678d36d-5d404fd2-dd954269-c11c2adf`, `cb1[91]`, `cb2[2]`, `t0`): outputs screen-locked unjittered clip coords (`o3.zw = (0.99, 1.0)`) and screen UV `o1.xy`.
    - `PS ResourceId::110930` (`c7167686-a5850b4a-f73bbcd8-24903a6a`, `cb1[333]`, `cb2[9]`, `t0..t4`): samples curved visor streaks `t3` using unjittered `v3.xy * cb1[332].zw`, droplets/normals `t2`/`t4` using unjittered `v1.xy`, and already-jittered pre-visor HDR background `t1 = 8731` twice.
  - Because `8728` and `8731` carry camera Halton subpixel jitter `+J_N` while `EID 18188` draws at unjittered screen coordinates, temporal unjitter (`-J_N` in DLAA/DLSS/FSR/XeSS) shifts the static visor curves/droplets across Halton frames, producing subpixel shimmer and doubled visor streaks.
- **Confidence:** High (verified via shader disassembly, `pixel_history` at `(180, 400)` and `(100, 100)`, and WARP D3D11 + `D3DDisassemble` test in `tests/dxbc_fanout.cpp`).
- **Implication:** Patch `VS 110929` (`o1.xy += -J_N / res`) and `PS 110930` (`v3.xy += -J_N` for visor terms while restoring `+J_N / res` right before `t1` background samples) in `edpe::patchVisorVertexShader` / `edpe::patchVisorPixelShader` (`src/dxbc_fanout.h`), invoked lazily via `tryVisorJitterAlignDraw` in `observedDrawInstanced` (`src/context_census.cpp`) whenever projection jitter is active, failing open on unsupported shaders.
- **VERIFIED Follow-up — Camera-rotation ghosting (`шлейфи`) when keeping `EID 18188` inside `8728`:**
  - Confirmed in live Odyssey (`edpe.log` `VS=B553BB479B7C0B97 PS=68ABCB9FEF6CA66C`, `2560x1440`, DLAA + Jitter Debug View): even when `EID 18188` is subpixel-jitter aligned inside `8728`, `EID 18188` has `DepthWriteMask = ZERO` and is camera-locked (`MV_real = 0`) over sky/distant pixels that have non-zero camera-rotation motion vectors (`MV_world != 0`).
  - Keeping `EID 18188` baked inside `scene_ldr_source` causes temporal upscalers (and `JitterDebugPass`) to reproject rotating sky pixels from history pixels that held the previous frame's static visor curves, producing bright smeared trails (`шлейфи`) across the sky whenever panning the camera.
  - Because `PS 110930` adds faint ambient HDR visor glare (~`0.001..0.023`) over the whole screen and refracts `t1 = 8731`, tonemapping `HdrLayer` against black causes gamma washout and loses refraction. Instead, when `Preserve Native UI` or `Subpixel Jitter` debug view is active:
    1. Copy clean pre-visor `8728` -> `s_visor_pre_hdr_tex` and `s_visor_hdr_tex`; draw unjittered `EID 18188` onto `s_visor_hdr_rtv` (read-only depth/stencil), then replay `EID 18188` with `RenderTargetWriteMask = 0` onto `8728+8723` so `DSV 8723` keeps `stencil = 0 -> 4` while `8728` remains 100% visor-free.
    2. At Pass 52 Tonemap (`observePostHudDrawAndTonemap`), re-issue the exact bound game tonemapper (`LUT t0`, `Bloom t2`, exposure `cb2`, viewport) with `PS hdrSlot = s_visor_hdr_srv` into `s_visor_ldr_rtv` (plus `s_visor_pre_hdr_srv` into `s_visor_clean_ldr_rtv` if HDR was modified after `EID 18188` or redirected by post-process).
    3. Post-upscale in `UiLayer::composite` / `compositeToRtv`, add `(tVisorLdr - tCleanLdr) * atten` onto the reconstructed world (`t0`), keeping `in-scene cb13` jitter alignment as fallback when `Preserve Native UI` is disabled.

## Planetary surface `cb1` & On-foot Aiming Down Sights (ADS) findings (`EDO1.rdc` & `EDO2.rdc`, 2026-11-18)

- **Date:** 2026-11-18. Sources: RenderDoc captures `EDO1.rdc` (on-foot planetary surface hip-fire, `2560x1440`) and `EDO2.rdc` (on-foot planetary surface Aiming Down Sights, `2560x1440`), cross-checked with `co-pilot-crew.rdc`.
- **Method:** RenderDoc `get_frame_overview`, `analyze_render_passes`, `pixel_history`, `get_shader_bindings`, and `get_buffer_data` on the 5376-byte scene constant buffer `cb1` (`ResourceId::10174`) and 192/208-byte `cb0` transpose blocks.
- **VERIFIED (Planetary surface `cb1` rotation split — `failReason = 7`):**
  - On planetary surfaces, `cb1[233..235]` (`words[932..943]`) carries the planetary local-horizon / cubemap reference rotation in columns `0..2`, paired with the true camera world position in column `3` (`words[935, 939, 943]`) duplicated at `cb1[241]` (`words[964..967]`, with `words[967] == 0.0f`).
  - Meanwhile, the active pass's true unscaled 3x3 view rotation (`R_right`, `R_up`, `R_forward`) is written transposed into `cb1[277..279]` (`words[1108..1119]`, `.w == 0.0f`).
  - **Confidence:** High (observed across `EDO1.rdc`, `EDO2.rdc`, and `co-pilot-crew.rdc`).
  - **Implication:** When `parseEliteCameraBlocks(&words[932], &words[1080])` fails with codes `7`, `8`, or `10` and `cb1[241]` confirms `words[935, 939, 943]` holds the live camera position, `parseEliteCameraWithCb1Fallback` reconstructs the view rotation from transposed `cb1[277..279]` while preserving `words[935, 939, 943]`.
- **VERIFIED (ADS viewmodel sub-pass shares `depthB = 0.025` with world & runs before world):**
  - **Hip-fire (`EDO1.rdc`):** Viewmodel runs first in Z-prepass (`EID 3367`) and G-buffer (`EID 4333`) with `scaleX = 1.2935, scaleY = 2.300, depthB = 0.0675`; Main World follows in Z-prepass (`EID 3409`) and G-buffer (`EID 4643`) with `scaleX = 1.0524, scaleY = 1.8709, depthB = 0.025`.
  - **ADS (`EDO2.rdc`):** Viewmodel switches `depthB` to `0.025` (`scaleX = 3.1901, scaleY = 5.6713`) across Z-prepass (`EID 7512..7568`), 4-MRT G-buffer (`EID 8495..8850`), and post-deferred sight/lens/reticle/trim draws (`EID 24040..24066`, including 192/208-byte `cb0` transpose blocks), still executing before Main World (`EID 7712..8000` Z-prepass, `EID 8921..15888` 4-MRT G-buffer, `scaleX = 1.2634, scaleY = 2.2460, depthB = 0.025`).
  - Inside 5376-byte `cb1`, `cb1[35..38]` (`words[140..155]`) persistently holds the Main World Camera inverse projection basis (`|cb1[35].xyz| = 1 / world_scaleX`, `|cb1[36].xyz| = 1 / world_scaleY`, `cb1[37] = (0, 0, 0, 1 / world_depthB)`, `|cb1[38].xyz| = 1.0`) even while `cb1` is unmapped for a viewmodel sub-pass.
  - **Confidence:** High (directly measured in `EDO1.rdc` and `EDO2.rdc` constant buffers).
  - **Implication:** Compare `parsed_camera` against `cb1[35..38]` (`isEliteSubpassVersusWorldCb1` / `matchesEliteWorldProjectionScales`) so same-`depthB` ADS viewmodel draws are routed to `SceneSubpassTracker` without triggering camera cuts or hijacking world camera staging/motion/deferred repair, while smooth world FOV zoom during ADS transitions is accepted without per-frame history resets (`projCut` > 25% or `depthB` change).

## On-foot ADS FOV ramp & spinning settlement beacon camera hijack (`issue #26`, 2026-11-22)

- **Date:** 2026-11-22. Sources: `issue #26` `edpe.log` (`v0.8.0-rc2`, Odyssey Surface Operations Tutorial, `3440x1440`) and DLSS-G debug-view video capture (`2026-11-22_10-31-31.mp4`, frames `634–2109`).
- **Method:** Frame-by-frame inspection of DLSS-G debug tiles (motion vectors, canonical depth, HUD-less, UI, FG output) correlated with `edpe.log` `commitMainCamera` / `viewmodel subpass decision` / `unmap candidate ignored` traces.
- **VERIFIED (Spinning settlement alarm beacon camera characteristics):**
  - During on-foot settlement combat/alarm states, Cobra submits pre-scene (`in_scene=0`) rotating spotlight/shadow-beacon cameras with `depthB = 0.1002`, 1:1 square aspect ratio, fixed world position ~29–50 m from the player, rotating continuously ~12°–17°/frame (`dist = 0.0000`, `deg = 12..17`).
  - **Confidence:** High (observed across thousands of consecutive frames in `edpe.log`, e.g. frames `63071–66166`, matching the ~15°/frame rotational MV saturation visible across skybox/buildings in DLSS-G debug frames `1098–2109`).
- **VERIFIED (Smooth ADS FOV ramp triggers `cb1[35..38]` lag & `commitMainCamera fallback`):**
  - Entering/exiting on-foot ADS smoothly animates `proj1080` (`scaleX` steps ~2–5%/frame, e.g. `0.7605 -> 0.7918 -> 0.8267 -> 0.8579 -> 0.8840` at `depthB = 0.0250`) while `cb1[35..38]` (`local_world140`) temporarily lags by >2%, causing `isEliteSubpassVersusWorldBlock16` to reject world G-buffer unmaps and fall into `commitMainCamera fallback` (`frames 60342, 60981, 62746, 63000, 66494, 88849..88851`).
  - **Confidence:** High (directly observed in `edpe.log` and matched to ADS-exit corruption timestamps `634–665`, `871`, `1098–2109`).
- **Implication:**
  - All camera qualification and fallback recovery paths must enforce `isMainCameraGeometryValid` (including screen-matching aspect ratio, non-square viewport, `wLenSq`, and `depthB`) plus `worldDepthB` matching when `cb1[35..38]` is valid.
  - `s_cut_rejection_streak` recovery must be gated to `(in_main_scene || matches_world_cb1 || s_consecutive_fallback_frames >= 3)` so pre-scene (`in_scene=0`) auxiliary cameras cannot hijack `s_camera_now`.
  - Smooth world FOV ramps (`isSmoothWorldFovTransitionCandidate`) must update `s_camera_now` via `s_world_zoom_fallback_camera` without falsely promoting ~2.5x ADS viewmodel sub-passes (`sx = 2.3740 / 3.1901`) or dropping into fallback.

## Cockpit Frosted HUD Panel Backdrops (Issue #34, `gameplay.rdc`, 2026-11-25)

- **Date:** 2026-11-25. Sources: `F:\Code\C++\EDPE\gameplay.rdc` (`2560x1440`, Pass 50 EIDs `3999..4125, 4236`) and GitHub issue #34 video capture (`2026-11-24 15-50-19.mp4`, `3440x1440`, 75% DLSS SR).
- **Method:** 2 fps video frame-by-frame inspection, RenderDoc pass/shader/binding/buffer inspection (`disassemble_shader`, `get_shader_bindings`, `get_buffer_data`, `save_render_target`), and Pass 52 filmic tonemap simulation.
- **VERIFIED (`VS 0x2CECEC3065EF0D4A / PS 0xA4D03619D631B186` renders frosted blurred-scene HUD backdrops, not just stencil setup):**
  - Across 16 draws (`EID 3999..4125, 4236`), `PS 17b40209` (`0xA4D03619D631B186`, `cb1[333]`, `cb2[5]`, `t0..t3`, `SV_Position`) renders the translucent frosted-glass backdrops behind all cockpit HUD panels (Comms, Info, Target Info, Target Hologram base, Radar/Starport Services, Own-Ship Shield `100%`, Fuel/Mass Locked).
  - When `cb1[49].x != 0`, it samples `t0` (`ResourceId::6291`, `320x180` blurred HDR scene) at `uv = SV_Position.xy * cb1[332].zw`, tints with `cb2[4].rgb` (crimson/orange, e.g. `(0.234, 0.044, 0.025)`), and blends onto the pre-tonemap HDR scene (`T ~ 0.15..0.50`).
  - Redirecting these color draws into `UiLayer::hdrLayerRtv_` at native output resolution both shifts `SV_Position.xy * cb1[332].zw` by `outputSize / renderSize` in Super Resolution (projecting nearby stars onto unrelated HUD panels over dark space) and splits `Tonemap(H_star * T + H_frosted)` into `Tonemap(H_star) * T + Tonemap(H_frosted)` (creating red glare over dark backgrounds and darkened boxes over bright stars).
- **Confidence:** High (directly observed in `gameplay.rdc` shader disassembly, buffer data, and `rt_3986` vs `rt_4125` render target diff).
- **Implication:** Keep stencil-only draws (`RenderTargetWriteMask == 0`) redirected for `s_hud_depth_stencil_dsv`, while passing frosted-backdrop color draws (`isFrostedBackdropPixelShader` / `0xA4D03619D631B186` / `0x0146ABCC53240479`) through to Cobra's pre-tonemap HDR scene target (synchronizing `s_hud_depth_stencil_dsv` stencil if written), preserving native crispness on all HUD text, icons, lines, and holograms.
