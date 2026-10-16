# EDPE Status — Elite Dangerous Performance Enhanced

**Last updated:** 2026-10-16

This file documents **verified facts only**. Unverified claims, hypotheses, or speculative results are not recorded here. See `AGENTS.md` and `PLAN.md` for project governance and roadmap.

---

## Repository

- **Git tags:** `v0.0.1` (initial roadmap), `v0.1.0` (verified in-game NVIDIA DLAA), `v0.2.1` (verified in-game DLSS SR, native UI protection, auto render scaling), `v0.3.5` (verified in-game AMD FSR 3.1.2 Native AA & SR, Performance HUD, input leak isolation), `v0.4.0` (verified in-game native D3D11 TAA reconstruction backend, HUD scaling and mouse dragging), `v0.4.1` (verified in-game native TAA stabilization, subpixel de-jittering, Karis clipping, cockpit depth stabilization)
- **Build artifacts:** `EDPE.dll`, `dxgi.dll`, and `d3d11.dll` deployed beside `EliteDangerous64.exe`; staged in `build/stage/`
- **Source:** `src/` contains 12 C++ modules (`capture_format.h`, `camera_delta.h`, `temporal_inputs.h`, `upscaler_backend.h/.cpp`, `motion_pass.cpp`, `ngx_context.cpp`, etc.); `tools/` contains standalone NGX probe
- **Test suite:** 10 automated tests passing in CTest (`temporal_math`, `camera_delta`, `capture_format`, `temporal_inputs`, `motion_gpu`, `ui_composite`, `dxbc_fanout`, `proxy_smoke`, `proxy_shader_probe`, `proxy_alias_smoke`)

---

## Roadmap phase status

| Phase | Status | Current state |
|-------|--------|---------------|
| 0. Reconnaissance | **Substantially complete; ongoing** | Target is Elite Dangerous Odyssey's 2D D3D11 path. Executable imports, swapchain (2560×1440, R8G8B8A8_UNORM), proxy behavior, MRT layout, and render resources investigated and documented. |
| 1. D3D11 injection | **Working prototype; verified in-game** | `EDPE.dll` forwards D3D11 device creation; `dxgi.dll` shim observes factories, swapchains, and `Present`. F5 ImGui overlay and F6 one-shot capture fully operational in-game without black screens or crashes. |
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
| 12. Native-resolution HUD | **Operational for DLSS & FSR SR** | `UiCompositePass` composites native-resolution UI/cockpit elements from pre-upscale backbuffer copy over reconstructed DLSS/FSR frames. Reactive mask generation planned for fine holographic elements. |
| 13. D3D11/D3D12 interoperability | **Not started** | Research phase for Frame Generation bridge. |
| 14. AMD Frame Generation | **Not started** | Awaits stable temporal upscaling and D3D12 presentation bridge. |
| 15. NVIDIA Streamline / DLSS Frame Generation | **Not started** | Awaits stable temporal upscaling and Streamline integration. |
| 16. NVIDIA DLSS-NR | **Not started** | Research phase for G-buffer normal/roughness extraction and NGX Ray Reconstruction / neural denoising integration. |

---

## Available diagnostics and controls

- **F5 Overlay:** Toggles the Dear ImGui menu. Exposes Upscaler Backend (`Native`, `NVIDIA DLSS / DLAA`, `AMD FSR (Native AA / SR)`), Quality Mode (`Native AA`, `Quality`, `Balanced`, `Performance`, `Ultra Performance`), NVIDIA DLSS Preset (`Auto`, `J`, `K`, `L`, `M`), Elite `Custom.4.4.fxcfg` `<SSAAMultiplier>` auto-sync, asynchronous GPU timings, projection jitter status/controls, runtime temporal tuning overrides, and snapshot previews. When hidden, passes through all inputs to the game without interference.
- **Performance Telemetry HUD (F7 / F5):** Real-time on-screen HUD displaying FPS, game frametime, upscaler frametime, total frametime, current GPU adapter name, GPU usage %, and dedicated VRAM usage (GB & %). Configurable corner position (Top-Left, Top-Right, Bottom-Left, Bottom-Right), toggleable in F5 menu or via F7 hotkey, persisted in `edpe.ini`.
- **F6 One-Shot Synchronized Capture:** Captures all 5 rendering buffers from the **exact same frame** and writes non-blocking 32-bit BMP files to `edpe-captures/`:
  - `color-*.bmp`: Tone-mapped scene color HDR (`DXGI_FORMAT_R11G11B10_FLOAT`)
  - `motion-*.bmp`: Visualized motion displacement (`DXGI_FORMAT_R16G16_FLOAT`)
  - `depth-dsv2-*.bmp`: Contrast-adjusted reversed-Z depth (`DXGI_FORMAT_R32G8X24_TYPELESS`)
  - `early-color-*.bmp`: Emissive / illumination pass isolating cockpit dials, lamps, and glowing lights (reactive mask candidate)
  - `frame-*.bmp`: Clean native backbuffer frame captured before ImGui overlay drawing
- **Non-blocking readback:** Staging textures mapped with `D3D11_MAP_FLAG_DO_NOT_WAIT`, completely preventing render thread stalls.
- **Logging:** Detailed execution log written to `edpe.log` beside the executable.

---

## Next milestone

Phase 11 (AMD FSR 3.1.2 Upscaling) is **Verified In-Game**:
- `AmdUpscalerBackend` verified in Elite Dangerous Odyssey for both FSR Native AA (1.0x SSAA) and Super Resolution (< 1.0x SSAA).
- UI composites native HUD over the reconstructed FSR image.
- Performance telemetry HUD tracks frame pacing, engine/upscaler times, and GPU/VRAM load.

The immediate next steps are:
1. Proceed to **Phase 12 (Reactive Mask refinements for HUD/cockpit glow)**.
2. Proceed to **Phase 13 (D3D11/D3D12 presentation bridge research for Frame Generation)**.

---

## Not yet demonstrated

EDPE has implemented Phases 9 & 10 (`render < output` DLSS Super Resolution and Cobra `<SSAAMultiplier>` render-scale integration); in-game verification of DLSS SR performance gains, AMD FSR reconstruction, native-resolution HUD separation, and Frame Generation remain to be demonstrated.

---

## D3D11 / DXGI Injection — VERIFIED

- `EDPE.dll` exports `EdpeD3D11CreateDevice` and `EdpeCreateDXGIFactory1`/`EdpeCreateDXGIFactory2`
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

## Failure Behavior — IMPLEMENTED & TESTED

- **Upscaler failure:** disable affected upscaler, disable jitter, restore original frame, Present normally
- **Never:** black-screen Elite because one EDPE subsystem fails
- **Forbidden in hot path:** blocking GPU readback, `Map()` on full-resolution textures, blocking `GetData` loops, per-frame NGX feature recreation
- **Async timing:** disjoint timestamp queries (`D3D11_QUERY_TIMESTAMP_DISJOINT` + start/end timestamps) measure GPU upscale cost without blocking the render thread
- **Fail-open enforcement:** Automated unit tests in `temporal_inputs_test` (null resource fallback, invalid scale fallback) and `temporal_math_test` (corrupted matrix rejection fail-open) verify safe fallbacks.

---

## Build / Test Summary

| Target | Status | Notes |
|--------|--------|-------|
| `EDPE.dll` + `dxgi.dll` | ✅ Built and deployable | CMake + MSVC x64; staged in `build/stage/` |
| `temporal_math_test` | ✅ Unit test | Halton sequences, projection blocks, NDC offset estimator, rejection invariants, camera delta tolerance |
| `camera_delta_test` | ✅ Unit test | CPU delta math, event flags, rate limiting, and scene reset observation |
| `capture_format_test` | ✅ Unit test | Half-float, float11/10 unpacker, reversed-Z depth contrast, motion BGR, Reinhard tone mapping |
| `temporal_inputs_test` | ✅ Unit test | Scaling calculations, input validation, NVIDIA/AMD convention translations, fail-open manager |
| `motion_gpu_test` | ✅ Requires D3D11 | GPU motion pass test (WARP / hardware) |
| `dxbc_fanout_test` | ✅ DXBC fanout probe | Shader hash analysis |
| `proxy_smoke` | ✅ Integration test | Verifies DLL load, Present hook, and input pass-through |
| `proxy_shader_probe` | ✅ Integration test | Shader bytecode interception verification |
| `proxy_alias_smoke` | ✅ Integration test | Verifies staged `d3d11.dll` and `dxgi.dll` alias loading |
| In-game captures (2026-10-15) | ✅ MEASURED IN-GAME | Full 2560×1440 captures of depth (DSV #2), emissive (RTV3), motion, color, and clean frame |
| In-game NVIDIA DLAA (2026-10-16) | ✅ VERIFIED IN-GAME | Stable 2560×1440 DLAA evaluation (~0.3 ms GPU time) with Halton (2,3) jitter, depth, and motion |
| Standalone NGX probe | ✅ MEASURED | `tools/ngx_probe/` V1-V5 clean, 2 runs each |
| Overlay lifecycle in Elite | ✅ MEASURED | Clean init, non-interfering input, clean shutdown on swap chain destroy |
| F6 Synchronized Capture | ✅ Working prototype | One-shot hotkey triggers synchronized 5-buffer capture |

---

## Next Verified Milestones (per PLAN.md order)

1. **Phase 12 — Native-Resolution HUD Separation:** Fine reactive mask generation and refinement for cockpit holographic elements and emissive UI.
2. **Phase 13–15 — D3D11/D3D12 Interop & Frame Generation:** Presentation bridge prototyping for AMD FSR & NVIDIA DLSS Frame Generation.

---

## Design Principles (from AGENTS.md)

- Prefer graphics API observation over executable patching
- Keep hooks small; observe, update state, invoke isolated subsystem, call original
- Fail open: disable feature, restore original frame, Present normally
- Every subsystem answers: "Did it run? Which backend? What dimensions? What formats? What temporal conventions? Why did it fail? What did it cost?"
- Do not couple Elite hooks directly to NGX/FidelityFX unless unavoidable at small boundary
- Do not reuse EDVR's NGX Project ID
- Do not port EDVR's OpenVR/OpenXR runtime, HMD compositor, headset logic, VR foveation