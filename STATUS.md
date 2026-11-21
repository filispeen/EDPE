# EDPE Status — Elite Dangerous Performance Enhanced

**Last updated:** 2026-11-20

This file documents **verified facts only**. Unverified claims, hypotheses, or speculative results are not recorded here. See `AGENTS.md` and `PLAN.md` for project governance and roadmap.

---

## Repository

- **Target version:** `v0.8.0` (branch `feat/phases-13-15-frame-generation`; build version string injected by CI from Git tag when released)
- **Git tags:** `v0.0.1` (initial roadmap), `v0.1.0` (verified in-game NVIDIA DLAA), `v0.2.1` (verified in-game DLSS SR, native UI protection, auto render scaling), `v0.3.5` (verified in-game AMD FSR 3.1.2 Native AA & SR, Performance HUD, input leak isolation), `v0.4.0` (verified in-game native D3D11 TAA reconstruction backend, HUD scaling and mouse dragging), `v0.4.1` (verified in-game native TAA stabilization, subpixel de-jittering, Karis clipping, cockpit depth stabilization), `v0.4.4` (verified in-game analytical skybox/star rotational motion vectors at infinity, elimination of star erasure during ship maneuvers, star bloom/corona motion coherence and artifact resolution across all reconstruction backends), `v0.4.6` (verified in-game canopy reflection sub-camera filtering, elimination of TAA docked station pulsing, adaptive stationary variance scaling, Performance HUD mouse cursor isolation when overlay is closed), `v0.4.7` (HOTAS/DirectInput isolation), `v0.4.8` (hot-path CPU overhead elimination), `v0.4.9` (crash logger and diagnostics subsystem), `v0.4.10` (AMD driver 26.10.1 FSR fix & UI scale slider), `v0.5.0` (cross-device scene rendering & DXGI Keyed Mutex, native UI composite pass, auxiliary camera/eye probe filtering, 2nd-order kinematic probe filter, pilot camera projection fallback, sharpening control), `v0.6.0` (real-time frametime graph in Performance HUD, multi-backend automated comparison benchmark with warmup & HUD stamp, 66.66ms telemetry & profiler rate limiting, DEV mode toggle in edpe.ini gating F8/F9 binds, sticky window footer with interactive author link), `v0.7.0-rc1`–`v0.7.0-rc3` / `v0.7.0` (cockpit holographic HUD live draw redirection & compute composite `UiLayer`, HUD debug view in F8 debug window, ImGui overlay refactor with ProggyForever vector font, cross-device Keyed Mutex synchronization, color source debounce, `CodeHook`/`VTableHook` D3D11 hook hot-path CPU optimization, EDO row-major deferred lighting/shadow/clustered CB2 jitter fixes, distant decal & on-foot hangar camera/HUD fixes), `v0.7.1` (hot-path CPU/GPU optimization, Preset E, `EnableProfiler=1` sub-pass profiling), `v0.7.2` (on-foot tool/rifle viewmodel sub-pass jitter coherence and 208B scaled `cb0` support, planetary ring HUD-layer isolation, on-foot helmet visor post-upscale delta composition, Smooth Motion auxiliary device isolation and context hook race hardening), `v0.7.3`–`v0.7.5-rc1` (Linux/Wine/Proton startup & ABI hardening, RenderDoc In-App API & debug annotations, linear-light HUD tonemapped composite `T(H) + F*T`, bloom/flare ghosting fixes, built-in FXAA/MLAA/SMAA blocking during reconstruction)
- **Build artifacts:** Consolidated `dxgi.dll` deployed beside `EliteDangerous64.exe`; staged in `build/stage/`
- **Source:** `src/` contains modular C++ modules (`capture_format.h`, `camera_delta.h`, `temporal_inputs.h`, `upscaler_backend.h/.cpp`, `motion_pass.cpp`, `ngx_context.cpp`, `ui_layer.cpp`, `ui_composite_pass.cpp`, `renderdoc_support.cpp`, `d3d12_bridge.cpp`, `d3d12_presenter.cpp`, `fg_backend.cpp`, `fg_ui_pack_pass.cpp`, `amd_fsr_fg_backend.cpp`, `nvidia_sl_fg_backend.cpp`, `fg_integration.cpp`, `vtable_hook.h`, `code_hook.h`, `crash_logger.cpp`, `version.cpp`, etc.); `tools/` contains standalone probes
- **Test suite:** 23 automated tests passing in CTest (`temporal_math`, `camera_delta`, `motion_stage2`, `capture_format`, `temporal_inputs`, `motion_gpu`, `ui_composite`, `hud_debug`, `dxbc_fanout`, `jitter_view_report`, `crash_logger`, `ui_menu_size`, `vtable_hook`, `renderdoc_support`, `d3d11_d3d12_bridge`, `d3d12_presenter`, `fg_backend`, `amd_fsr_fg`, `nvidia_sl_fg`, `fg_integration`, `proxy_smoke`, `proxy_shader_probe`, `proxy_alias_smoke`)

---

## Roadmap phase status

| Phase | Status | Current state |
|-------|--------|---------------|
| 0. Reconnaissance | **Substantially complete; ongoing** | Target is Elite Dangerous Odyssey's 2D D3D11 path. Executable imports, swapchain (2560×1440, R8G8B8A8_UNORM), proxy behavior, MRT layout, and render resources investigated and documented. |
| 1. D3D11 injection | **Working prototype; verified in-game** | Consolidated `dxgi.dll` proxies DXGI factories, swapchains, and `Present`, and hooks D3D11 device creation. F5 ImGui overlay fully operational in-game without black screens or crashes. |
| 2. Resource census | **Substantially complete for temporal path** | D3D11 context instrumentation tracks render/depth target binds, MRT patterns, 5376-byte dynamic constant buffers, and draw calls. Full engine inventory across multi-scene transitions remains ongoing. |
| 3. Scene depth | **Verified in-game (DSV #2)** | In-game capture (2026-10-15, build 332841) confirmed DSV #2 (`DXGI_FORMAT_R32G8X24_TYPELESS`) is the authoritative scene depth buffer with reversed-Z ($B=0.025$). Decoded BMP shows coherent full-frame geometry. |
| 4. Camera and projection | **Measured & verified on GPU** | 5376-byte dynamic constant buffer (CB0) parsed via `parseEliteCamera()`. Projection matrix words (1080–1095) isolated; primary scene camera latched once per frame on first valid CB0 map. |
| 5. Projection jitter | **Verified & active in runtime (fail-open)** | Halton (2,3) subpixel offsets injected into 5376-byte dynamic CB0 (words 1080–1095) during `Unmap`. Fail-open stand-down when upscaler inactive, with manual test override in ImGui. Verified in-game with DLAA reconstruction. |
| 6. Motion vectors | **Verified in-game (flight motion)** | GPU motion pass renders to `DXGI_FORMAT_R16G16_FLOAT`. In-game flight capture confirmed 64.8% horizontal and 63.5% vertical active motion across frame, matching reversed-Z depth and camera motion. |
| 7. Shared temporal input layer | **Implemented & verified** | Vendor-neutral `TemporalFrameInputs` handoff, `IUpscalerBackend`, and `UpscalerManager` coordinator implemented and unit-tested. |
| 8. NVIDIA DLAA | **Verified & working in-game** | Official NVIDIA NGX SDK linked. `NgxContext` evaluates DLAA at native 2560×1440 (`MVLowRes | DepthInverted`, Preset K, LDR mode) on the finished swapchain backbuffer at `Present` before ImGui overlay rendering. Verified in-game: clean, rock-solid temporal anti-aliasing (~0.3 ms GPU time) with zero image vibration. |
| 9. NVIDIA DLSS Super Resolution | **Verified & working in-game** | `NgxContext` and `NvidiaUpscalerBackend` reconstruct low-res LDR scene target (`scene_ldr_source`) into native backbuffer with verified temporal inputs and presets (`Auto`, `J`, `K`, `L`, `M`). Reconstructed 3D world is composited under native-resolution HUD. |
| 10. True render-scale control | **Verified & working in-game** | Cobra's native Supersampling (SSAA < 1.0x) delivers true rendering workload reduction. Automatically detected via scene depth dimensions with zero manual configuration. |
| 11. AMD FSR upscaling | **Verified & working in-game** | Native D3D11 FidelityFX FSR 3.1.2 backend integrated in `AmdUpscalerBackend`. Verified in-game: supports FSR Native AA (1.0x SSAA) and FSR Super Resolution (<1.0x SSAA) with auto-exposure, inverted reversed-Z, and fail-open lifecycle. |
| 12. Native-resolution HUD | **Verified in-game (Stage 2)** | Clean pre-HUD 3D scene color (`s_clean_hdr_scene_texture`) fed to upscaler. Cockpit HUD draws redirected live at native swapchain resolution (`UiLayer` / `ui_layer_composite_cs.hlsl`) with viewport subpixel projection unjittering and composited cleanly onto the reconstructed world (`evalStage=1`, `composite=2` verified in-game). |
| 13. D3D11/D3D12 interoperability | **Implemented & verified in-game (v0.8.0)** | `D3D11D3D12Bridge` (shared NT handles + `ID3D12Fence`/`ID3D11Fence` sync) and `D3D12ProxyPresenter` (child `WS_CHILD` Flip-model D3D12 swapchain overlay, resize recovery, RenderDoc safety guard). Measured in-game D3D12 copy overhead ~0.08 ms at 2560×1440. |
| 14. AMD Frame Generation | **Verified & working in-game (v0.8.0)** | `AmdFsrFgBackend` integrates official AMD FidelityFX Frame Generation DX12 (`ffxConfigure` + `ffxDispatch` PrepareV2/FG), premultiplied `RGBA16F` UI packing (`FgUiPackPass`), and `FgRuntime`. Verified in Elite Dangerous Odyssey on RTX 3060 with `AMD FSR Frame Generation (2x)` and automatic pause/menu stand-down. |
| 15. NVIDIA Streamline / DLSS Frame Generation | **Prototype implemented; capability-gated (v0.8.0)** | `NvidiaStreamlineFgBackend` integrates official Streamline v2.14.16 (`slInit`, `Reflex`, `PCL`, `DLSS-G`). Hardware capability probe verified on RTX 3060 (`Reflex=eOk`, `PCL=eOk`, `DLSS_G=eErrorNoSupportedAdapterFound`), failing open cleanly without black screens; live DLSS-G generation remains to be tested on RTX 40+ hardware. |
| 16. Intel XeSS upscaling | **Planned / Research** | Intel XeSS 1.3+ temporal upscaling (XeSS Native AA and Super Resolution). Dual-model support: XMX matrix acceleration on Intel Arc GPUs and universal DP4a fallback on NVIDIA/AMD GPUs. Investigating D3D11-to-D3D12 interop bridge (via Phase 13) vs proxy wrapper execution. |
| 17. Low-Latency Framework (NVIDIA Reflex & AMD Anti-Lag) | **Planned / Architecture defined** | Standalone and FG-coupled input latency minimization. NVIDIA Reflex SDK (Low Latency / Boost, marker insertion, sleep pacing) and AMD Anti-Lag / Anti-Lag 2 SDK. Pacing controls in edpe.ini and telemetry HUD integration. |
| 18. NVIDIA DLSS-NR | **Planned** | Research phase for G-buffer normal/roughness extraction and NGX neural rendering integration. |
| 19. Third-party mod compatibility (ReShade, EDHM, 3Dmigoto) | **Planned / Architecture defined** | Proxy DLL chaining architecture for transparent co-existence with ReShade and EDHM (3Dmigoto). Non-destructive HUD heuristics resilient to shader overrides, post-upscale ReShade injection order, and depth buffer alignment. |

---

## Available diagnostics and controls

- **F5 Overlay:** Toggles the Dear ImGui menu. Exposes Upscaler Backend (`Native`, `NVIDIA DLSS / DLAA`, `AMD FSR (Native AA / SR)`, `TAA`), Quality Mode (`Native AA`, `Quality`, `Balanced`, `Performance`, `Ultra Performance`), NVIDIA DLSS Preset (`Auto`, `E`, `J`, `K`, `L`, `M`), Frame Generation (`Off`, `AMD FSR Frame Generation`, `NVIDIA DLSS-G` with capability-driven availability/multiplier, Rendered vs Presented FPS, Total Generated, D3D12 Copy ms), RenderDoc controls, Elite `Custom.4.4.fxcfg` `<SSAAMultiplier>` auto-sync, asynchronous GPU timings, projection jitter status/controls, runtime temporal tuning overrides, and snapshot previews. Features a sticky bottom footer with hotkey status and clickable author link (`by FILISPEEN`). When hidden, passes through all inputs to the game without interference.
- **Performance Telemetry HUD (F7 / F5):** Real-time on-screen HUD displaying FPS, `Presented FPS (+FG)`, active Frame Gen backend/state, game frametime, upscaler frametime, total frametime, multi-curve rolling 120-sample frametime history graph (Game: White, Upscaler: Cyan, Total: Orange with 60 FPS / 30 FPS reference lines), current GPU adapter name, GPU usage %, and dedicated VRAM usage (GB & %). Numerical values are formatted with leading space padding for flicker-free reading, with 66.66 ms (~15 Hz) rate-limited display updates. Configurable anchor position (8 positions or manual drag), toggleable in F5 menu or via F7 hotkey, persisted in `edpe.ini`.
- **Multi-Backend Comparison Benchmark:** Automated sequential capture across all upscaler backends with 5-second warmup per backend. Saves uncompressed comparison frames to `edpe-captures/comparison/` with optional HUD overlay & GPU reconstruction time stamp.
- **Developer Mode Toggle (`DEV=0` / `DEV=1`):** Configurable in `edpe.ini`; defaults to `0` (clean gamer interface hiding diagnostic tools and gating F8/F9 binds).
- **Non-blocking readback:** Staging textures mapped with `D3D11_MAP_FLAG_DO_NOT_WAIT`, completely preventing render thread stalls.
- **Logging:** Detailed execution log written to `edpe.log` beside the executable.

---

## Next milestone

Phases 13 & 14 (D3D11/D3D12 Interop & AMD FSR Frame Generation) are **Verified In-Game (`v0.8.0`)**:
- `D3D11D3D12Bridge`, `D3D12ProxyPresenter`, and `FgRuntime` run live alongside `ContextCensusGetFgSnapshot()` on NVIDIA GeForce RTX 3060 (2560×1440, `0.08 ms` D3D12 bridge copy).
- `AmdFsrFgBackend` verified in-game with `AMD FSR Frame Generation (2x)` and automatic pause/menu stand-down.
- `NvidiaStreamlineFgBackend` initializes and reports hardware capabilities cleanly (`Reflex=eOk`, `PCL=eOk`, `DLSS-G` fails open with `eErrorNoSupportedAdapterFound` on RTX 3060).

The immediate next steps are:
1. Validate `NvidiaStreamlineFgBackend` live frame generation on **Ada Lovelace (RTX 40+) / Blackwell hardware** with HAGS / vkd3d-proton.
2. Proceed to **Phase 16 (Intel XeSS via D3D12 interop)** and **Phase 17 (Low Latency — standalone NVAPI Reflex & AMD Anti-Lag 2)**.

---

## Not yet demonstrated

- Live NVIDIA DLSS-G multi-frame generation on RTX 40+/50+ hardware (`eErrorNoSupportedAdapterFound` on RTX 3060 test GPU).
- Intel XeSS (Phase 16), standalone NVAPI Reflex / AMD Anti-Lag 2 (Phase 17), DLSS-NR (Phase 18), and ReShade/EDHM/3Dmigoto proxy chaining (Phase 19).

---

## D3D11 / DXGI Injection — VERIFIED

- `dxgi.dll` exports `EdpeCreateDXGIFactory`, `EdpeCreateDXGIFactory1`, `EdpeCreateDXGIFactory2`, and `EdpeD3D11CreateDevice`
- `dxgi.dll` shim observes `IDXGIFactory::CreateSwapChain` and `IDXGISwapChain::Present`
- Present hook logs first-Present (2560×1440, `R8G8B8A8_UNORM`), device/context pointers
- `Insert` key reaches original WndProc when menu hidden; `F5` toggles Dear ImGui overlay
- No black screen on game launch with EDPE loaded; 3D world and HUD display normally

---

## Dear ImGui Overlay — VERIFIED

- Win32 + DirectX 11 backends
- `F5` toggles menu visibility; `Insert` passes through when hidden
- Overlay title: `EDPE — Elite Dangerous Performance Enhanced`
- Exposes Upscaler selector (`Native`, `NVIDIA DLAA`), live GPU timing (`~0.3 ms`), jitter diagnostics, and runtime temporal convention tuning controls

---

## Depth — VERIFIED IN-GAME (DSV #2)

Per `docs/render-pipeline-observations.md` (2026-09-23 census, 2026-10-15 in-game decode):

- Swapchain: 2560×1440, `DXGI_FORMAT_R8G8B8A8_UNORM`, 1 sample
- **DSV #2 verified:** `depth-dsv2-*.bmp` decoded from `DXGI_FORMAT_R32G8X24_TYPELESS` via contrast mapping $z / (z + 0.001)$ yields pure monochrome geometry across the frame (values 35 to 177, average 90.7).
- **Z-direction:** Reversed-Z confirmed with clear value 0.0 and $B = 0.025$ ($z_{\text{view}} = B / z_{\text{raw}}$).
- **Clean sampling point:** Captured after scene-MRT pass before UI drawing.

---

## Emissive / Illumination Buffer (MRT RTV3 First Exit) — VERIFIED IN-GAME

Per `docs/render-pipeline-observations.md` (2026-10-15 in-game visual analysis & 2026-10-16 pipeline order verification):

- **Buffer:** MRT RTV3, `DXGI_FORMAT_R11G11B10_FLOAT`.
- **Pipeline timing finding (2026-10-16):** At its first unbind (`RTV3 first exit`), `RTV3` holds only the emissive/glow pass (black non-emissive background, bright lamps/instruments up to 254.0). Immediately *after* `first exit`, Cobra rebinds `RTV3` and executes the deferred lighting pass from the jittered G-buffer (`RTV0..2` + `Depth`), followed by tone-mapping into the `R8G8B8A8_UNORM` swapchain backbuffer.
- **Implication:** Full-scene reconstruction at native resolution must run after deferred lighting and tone-mapping complete (or on the final composited scene before UI), never at `RTV3 first exit`.

---

## Camera / Projection — VERIFIED IN-GAME

- Elite's 5376-byte constant buffer (CB0) parsed via `parseEliteCamera()` (`src/elite_camera.h`)
- Validated fields: `worldFromView[12]`, `scaleX`, `scaleY`, `depthB`
- Primary scene camera (`s_camera_now`) latched once per frame on the first valid CB0 `Unmap`, preventing later per-frame dynamic CB0 discards (~28 maps/frame) from overwriting the main 3D camera state.

---

## Projection Jitter — VERIFIED IN-GAME WITH DLAA (Phase 5)

- Halton sequence (base 2, base 3), frame-indexed: `jitter = (halton(frame+1,2)-0.5, halton(frame+1,3)-0.5)`
- Output: `pixelX, pixelY` in render-pixel units; `ndcX = 2*pixelX/width`, `ndcY = -2*pixelY/height`
- Target injection site: Words 1080–1095 of the 5376-byte dynamic CB0 intercepted during `Map`/`Unmap`.
- **Fail-open behavior:** Jitter stands down automatically whenever temporal reconstruction is inactive (`UpscalerBackend::None` or `UpscaleMode::Native`).

---

## Motion Vectors — VERIFIED IN-GAME (Phase 6)

- `MotionPass` renders to `DXGI_FORMAT_R16G16_FLOAT` render-target using current+previous camera constants and reversed-Z depth
- Output: per-pixel current→previous displacement in render-resolution pixels
- Consumed by NGX DLAA with `MVLowRes` and unit scale (`1.0f, 1.0f`).

---

## NGX / NVIDIA DLAA — VERIFIED IN-GAME (Phase 8)

- **SDK & Project ID:** Official NVIDIA NGX SDK initialized with EDPE-owned GUID `48d353f3-d07b-4048-876b-09f8622f5a27` via `NVSDK_NGX_D3D11_Init_with_ProjectID` (never EDVR's ID).
- **Feature Configuration:** 2560×1440 (`input == output`), `NVSDK_NGX_PerfQuality_Value_DLAA`, flags `MVLowRes | DepthInverted` (LDR backbuffer path), DLAA render preset hinted to Preset K (`11`).
- **Evaluation Site:** `ContextCensusOnPresent` copies the finished `R8G8B8A8_UNORM` swapchain backbuffer to persistent `s_dlaa_input`, evaluates NGX DLAA into `s_dlaa_output`, and copies the reconstructed frame back to `backbuffer` before `UiOnPresent` renders Dear ImGui.
- **Measured Performance & Quality:** ~0.3 ms GPU evaluation time at 2560×1440; stable anti-aliasing without subpixel jitter vibration.

---

## AMD FidelityFX FSR 3.1.2 — VERIFIED IN-GAME (Phase 11)

- **SDK:** AMD FidelityFX FSR 3.1.2 Direct3D 11 native backend (`FidelityFX-SDK-DX11`, pinned commit `9b04fa4`, dynamic CRT `/MD`). Fetched and built via `tools/fetch_ffx_dx11.py`.
- **Backend Architecture:** `AmdUpscalerBackend` implements `IUpscalerBackend` with PIMPL isolation (`Impl`) so FidelityFX host headers are not leaked to external compilation units.
- **Feature Configuration:** Supports both FSR Native AA (`input == output`) and Super Resolution (`input < output`). Configured with `FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE`, `FFX_FSR3UPSCALER_ENABLE_DEPTH_INVERTED`, `FFX_FSR3UPSCALER_ENABLE_DEPTH_INFINITE`, and `FFX_FSR3UPSCALER_ENABLE_AUTO_EXPOSURE`.
- **Execution & Dispatch:** Scratch memory managed via `ffxGetScratchMemorySizeDX11` and `ffxGetInterfaceDX11`. Dispatches compute pass to internal staging UAV texture, then copied to backbuffer before UI composite pass.
- **Fail-Open Behavior:** Reverts safely to native presentation if FFX initialization or dispatch fails. Cleanly resets history and destroys context on resolution resize or shutdown. Unit tested in `temporal_inputs_test`.

---

## Cockpit Holographic HUD Mask & UI Composition — IMPLEMENTED (Phase 12, Stage 1 & 2)

- **Root Cause Solved:** Deriving `uiMask` solely from `|gameBackbuffer - lowResWorld|` failed for the cockpit holographic HUD because Pass 35 draws directly into HDR color before bloom and tonemapping. The difference was ~0, leaving `uiMask = 0`, causing the HUD to be reconstructed through DLSS/FSR without motion vectors (causing blur and ghosting).
- **Verified Shader Pairs (Pass 35 / 50 in `gameplay.rdc`, 30 draws):**
  - `VS=0x81216C77F90DEDD6, PS=0xAB82734FE19EFB9D` (12 draws: icons, text, gauges)
  - `VS=0x2CECEC3065EF0D4A, PS=0xA4D03619D631B186` (16 draws: holographic panels, center radar)
  - `VS=0x5559BD94B6852E83, PS=0xEA02FAC2BD6C643C` (1 draw: target ship 3D hologram)
  - `VS=0x5559BD94B6852E83, PS=0xE95634B0F61D218F` (1 draw: player ship 3D hologram)
  - Canopy glass: `0xF512712C40D93C12 / 0x4A71EB0D34E9F2EF`, `0x8C091FFD08644E02 / 0x70F56E2EB7032E1A`
- **Per-Frame Draw Interception:**
  - Armed per-frame while reconstruction is active (`edpe::UpscalerManager::instance().isReconstructionActive()`).
  - Hooks remain installed across frames without vtable thrashing or per-frame hooking overhead.
  - Hot-path gated by `s_hud_mask_active_pass` and `motion_pair_active` (single CPU branch for non-HUD draws).
- **HUD Mask Generation:**
  - Matched HUD draws are replayed into `s_hud_mask_rtv` (`R11G11B10_FLOAT`, native output swapchain resolution) using the game's bound HUD shaders, an alpha/additive blend filter, and subpixel projection unjittering on the viewport (`TopLeftX = -jitterX`, `TopLeftY = +jitterY`).
  - Cleared once per frame on entry to RTV0 deferred lighting/HUD.
- **UI Composite Pass Integration:**
  - Mask SRV bound as 4th SRV (`t3`).
  - In `ui_composite_shader.h`, clean pre-HUD scene color (`s_clean_hdr_scene_texture`) is fed to DLSS/FSR, and native-resolution unjittered HUD (`tHudMask`) is composited directly onto the reconstructed frame via emissive addition without blurry `cGame` cutout blending (resolving Issue #7).
  - Non-blocking staging readback every 300 frames logs `EDPE: HUD mask coverage=<percent>% frame=<n>`.

---

## D3D11/D3D12 Interop & Frame Generation — VERIFIED IN-GAME (Phases 13–15, `v0.8.0`)

- **Phase 13 (Bridge & Child Flip Presenter):**
  - `D3D11D3D12Bridge` shares D3D11 textures (depth, `R16G16_FLOAT` motion vectors, HUD-less color, packed UI) into D3D12 using shared NT handles (`CreateSharedHandle`) and shared fences (`ID3D12Fence`/`ID3D11Fence`).
  - `D3D12ProxyPresenter` overlays a click-through `WS_CHILD` Flip-model D3D12 swapchain synced with the game's window (`DXGI_SWAP_EFFECT_FLIP_DISCARD`), recovering across `ResizeBuffers` and toggling cleanly back to native D3D11 `Present` when FG is off or bypassed.
  - Safe failure guards: bypasses cleanly under in-process Windows `renderdoc.dll` (cross-API present risk) and exclusive fullscreen, while supporting Wine/vkd3d-proton system `d3d12.dll` + Vulkan debug markers (`VK_EXT_debug_utils`).
- **Phase 14 (AMD FSR Frame Generation — Verified In-Game):**
  - `AmdFsrFgBackend` integrates official AMD FidelityFX Frame Generation DX12 (`ffxConfigure` + `ffxDispatch` PrepareV2/FG), coupled with `FgUiPackPass` (`src/fg_ui_pack_pass.cpp`) packing HDR/LDR/multiply UI layers into premultiplied `R16G16B16A16_FLOAT`.
  - `FgRuntime` (`src/fg_integration.cpp`) + `ContextCensusGetFgSnapshot()` synchronize completed rendered frames into `FrameGenerationManager` (`src/fg_backend.cpp`), resetting temporal FG history (`Stand down (menu/loading)`) during pause/menus/loading.
  - Verified live in *Elite Dangerous Odyssey* (NVIDIA GeForce RTX 3060, 2560×1440, alongside NVIDIA DLAA): active `AMD FSR Frame Generation (2x)` output scaling with `0.08 ms` D3D12 copy time, no F5 menu crash, and working menu stand-down.
- **Phase 15 (NVIDIA Streamline / DLSS-G — Capability-Gated Prototype):**
  - `NvidiaStreamlineFgBackend` (`src/nvidia_sl_fg_backend.cpp`) loads official NVIDIA Streamline v2.14.16 (`slInit`, `Reflex`, `PCL`, `DLSS-G`), queries adapter capabilities dynamically without hardcoded GPU lists, and surfaces unsupported reasons directly in the F5 menu.
  - Hardware capability probe verified on RTX 3060 (`Reflex=eOk`, `PCL=eOk`, `DLSS_G=eErrorNoSupportedAdapterFound (6)`), failing open cleanly.

---

## Failure Behavior — IMPLEMENTED & TESTED

- **Upscaler / Frame Generation failure:** disable affected backend, disable jitter/proxy swapchain, restore original frame, Present normally
- **Never:** black-screen Elite because one EDPE subsystem fails
- **Forbidden in hot path:** blocking GPU readback, `Map()` on full-resolution textures, blocking `GetData` loops, per-frame NGX feature recreation
- **Async timing:** disjoint timestamp queries (`D3D11_QUERY_TIMESTAMP_DISJOINT` + start/end timestamps) measure GPU upscale cost without blocking the render thread
- **Fail-open enforcement:** Automated unit tests in `temporal_inputs_test` (null resource fallback, invalid scale fallback) and `temporal_math_test` (corrupted matrix rejection fail-open) verify safe fallbacks.

---

## Build / Test Summary

| Target | Status | Notes |
|--------|--------|-------|
| `dxgi.dll` | ✅ Built and deployable | CMake + MSVC x64; staged in `build/stage/` |
| `temporal_math_test` | ✅ Unit test | Halton sequences, projection blocks, NDC offset estimator, rejection invariants, camera delta tolerance |
| `camera_delta_test` | ✅ Unit test | CPU delta math, event flags, rate limiting, and scene reset observation |
| `capture_format_test` | ✅ Unit test | Half-float, float11/10 unpacker, reversed-Z depth contrast, motion BGR, Reinhard tone mapping |
| `temporal_inputs_test` | ✅ Unit test | Scaling calculations, input validation, NVIDIA/AMD convention translations, fail-open manager |
| `ui_composite_test` | ✅ Unit test | Menu UI difference isolation and cockpit holographic HUD mask blending |
| `hud_debug_test` | ✅ Unit test | Cockpit HUD & UI Debug View modes (0–4) and null fallback safety |
| `jitter_view_report_test` | ✅ Unit test | Jitter diagnostics report generation |
| `crash_logger_test` | ✅ Unit test | SEH crash minidump generation and stack capture |
| `ui_menu_size_test` | ✅ Unit test | ImGui menu window size, auto-fit, and position persistence |
| `vtable_hook_test` | ✅ Unit test | `LiveCopy` vtable hook initialization, drift detection (`reclaimLive`), and unhook restoration |
| `renderdoc_support_test` | ✅ Unit test | RenderDoc In-App API loading, annotation state, and Wine/Vulkan debug markers |
| `d3d11_d3d12_bridge_test` | ✅ Requires D3D11+D3D12 | Shared NT handle textures and shared D3D11/D3D12 fence synchronization |
| `d3d12_presenter_test` | ✅ Requires D3D12 | Flip-model child HWND D3D12 proxy swapchain presentation and resize teardown |
| `fg_backend_test` | ✅ Unit test | `FrameGenerationManager` contracts, camera matrix math, menu stand-down, fail-open behavior |
| `amd_fsr_fg_test` | ✅ Requires D3D12+FFX | AMD FidelityFX Frame Generation DX12 lifecycle and evaluation |
| `nvidia_sl_fg_test` | ✅ Requires Streamline | Streamline initialization and capability probe (`Reflex`/`PCL`/`DLSS-G`) |
| `fg_integration_test` | ✅ Requires D3D11+D3D12 | End-to-end `FgRuntime` proxy swapchain, pause stand-down, and `ResizeBuffers` recovery |
| `motion_gpu_test` | ✅ Requires D3D11 | GPU motion pass test (WARP / hardware) |
| `dxbc_fanout_test` | ✅ DXBC fanout probe | Shader hash analysis |
| `proxy_smoke` | ✅ Integration test | Verifies DLL load, Present hook, and input pass-through |
| `proxy_shader_probe` | ✅ Integration test | Shader bytecode interception verification |
| `proxy_alias_smoke` | ✅ Integration test | Verifies staged `dxgi.dll` loading |
| In-game captures (2026-10-15) | ✅ MEASURED IN-GAME | Full 2560×1440 captures of depth (DSV #2), emissive (RTV3), motion, color, and clean frame |
| In-game NVIDIA DLAA (2026-10-16) | ✅ VERIFIED IN-GAME | Stable 2560×1440 DLAA evaluation (~0.3 ms GPU time) with Halton (2,3) jitter, depth, and motion |
| In-game AMD FSR FG (2026-11-20) | ✅ VERIFIED IN-GAME | Active `2x` Frame Generation on RTX 3060 + NVIDIA DLAA (2560×1440, `0.08 ms` D3D12 copy, clean F5 overlay & menu stand-down) |
| Standalone NGX probe | ✅ MEASURED | `tools/ngx_probe/` V1-V5 clean, 2 runs each |
| Overlay lifecycle in Elite | ✅ MEASURED | Clean init, non-interfering input, clean shutdown on swap chain destroy |

---

## Next Verified Milestones (per PLAN.md order)

1. **Phase 15 (Hardware Validation) — NVIDIA DLSS-G on Ada RTX 40+ / Blackwell:** Verify live DLSS Frame Generation & Multi Frame Generation on supported GPUs.
2. **Phase 16 — Intel XeSS:** Native AA and Super Resolution via Phase 13 D3D11/D3D12 interop.
3. **Phase 17 — Low-Latency Framework:** Standalone and FG-coupled input lag minimization via NVIDIA Reflex (NVAPI / Streamline) and AMD Anti-Lag / Anti-Lag 2 SDK integration.
4. **Phase 18–19 — NVIDIA DLSS-NR & Third-Party Mod Compatibility:** Neural rendering research and proxy DLL chaining for ReShade, EDHM, and 3Dmigoto.

---

## Design Principles (from AGENTS.md)

- Prefer graphics API observation over executable patching
- Keep hooks small; observe, update state, invoke isolated subsystem, call original
- Fail open: disable feature, restore original frame, Present normally
- Every subsystem answers: "Did it run? Which backend? What dimensions? What formats? What temporal conventions? Why did it fail? What did it cost?"
- Do not couple Elite hooks directly to NGX/FidelityFX unless unavoidable at small boundary
- Do not reuse EDVR's NGX Project ID
- Do not port EDVR's OpenVR/OpenXR runtime, HMD compositor, headset logic, VR foveation