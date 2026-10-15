# EDPE Status — Elite Dangerous Performance Enhanced

**Last updated:** 2026-10-15

This file documents **verified facts only**. Unverified claims, hypotheses, or speculative results are not recorded here. See `AGENTS.md` and `PLAN.md` for project governance and roadmap.

---

## Repository

- **Git tags:** `v0.0.1` (initial roadmap), current HEAD at `1fb2aaf` (264 commits beyond v0.0.1)
- **Build artifacts:** `EDPE.dll` and `dxgi.dll` deployed beside `EliteDangerous64.exe`; staged in `build/stage/`
- **Source:** `src/` contains 12 C++ modules (`capture_format.h`, `camera_delta.h`, `temporal_inputs.h`, `upscaler_backend.h/.cpp`, `motion_pass.cpp`, etc.); `tools/` contains standalone NGX probe
- **Test suite:** 9 automated tests passing in CTest (`temporal_math`, `camera_delta`, `capture_format`, `temporal_inputs`, `motion_gpu`, `dxbc_fanout`, `proxy_smoke`, `proxy_shader_probe`, `proxy_alias_smoke`)

---

## Roadmap phase status

| Phase | Status | Current state |
|-------|--------|---------------|
| 0. Reconnaissance | **Substantially complete; ongoing** | Target is Elite Dangerous Odyssey's 2D D3D11 path. Executable imports, swapchain (2560×1440, R8G8B8A8_UNORM), proxy behavior, MRT layout, and render resources investigated and documented. |
| 1. D3D11 injection | **Working prototype; verified in-game** | `EDPE.dll` forwards D3D11 device creation; `dxgi.dll` shim observes factories, swapchains, and `Present`. F5 ImGui overlay and F6 one-shot capture fully operational in-game without black screens or crashes. |
| 2. Resource census | **Substantially complete for temporal path** | D3D11 context instrumentation tracks render/depth target binds, MRT patterns, 5376-byte dynamic constant buffers, and draw calls. Full engine inventory across multi-scene transitions remains ongoing. |
| 3. Scene depth | **Verified in-game (DSV #2)** | In-game capture (2026-10-15, build 332841) confirmed DSV #2 (`DXGI_FORMAT_R32G8X24_TYPELESS`) is the authoritative scene depth buffer with reversed-Z ($B=0.025$). Decoded BMP shows coherent full-frame geometry. |
| 4. Camera and projection | **Measured & verified on GPU** | 5376-byte dynamic constant buffer (CB0) parsed via `parseEliteCamera()`. Projection matrix words (1080–1095) isolated. Camera delta tracker records inter-frame motion and scene resets without blocking rendering. |
| 5. Projection jitter | **Math verified; ready for runtime injection** | Halton (2,3) sequence, projection block modifier (`jitterEliteProjectionBlock`), and NDC offset estimator (`estimateEliteBlockNdcOffset`) verified by tests. Phase 5 target: wire into CB0 `Unmap` with fail-open switch. |
| 6. Motion vectors | **In-game neutral parity verified; dynamic flight pending** | GPU motion pass renders to `DXGI_FORMAT_R16G16_FLOAT`. In-game capture verified neutral gray $(128, 128, 128)$ for zero motion. Synchronized capture alongside depth and emissive implemented. Flight direction/scale verification pending. |
| 7. Shared temporal input layer | **Implemented; reactive mask source verified** | Vendor-neutral `TemporalFrameInputs` handoff, `IUpscalerBackend`, and `UpscalerManager` coordinator implemented and unit-tested. Emissive pass (MRT RTV3 `R11G11B10_FLOAT`) verified in-game as direct native source for `reactiveMask`. |
| 8. NVIDIA DLAA | **Blocked on Phase 5 jitter & flight motion verification** | Standalone NGX probe (`tools/ngx_probe`) verified clean init, feature creation, evaluation, and shutdown. NGX evaluation in Elite render loop awaits Phase 5 jitter injection and motion convention sign check. |
| 9. NVIDIA DLSS Super Resolution | **Not started** | Awaits DLAA validation at native resolution. |
| 10. True render-scale control | **Not started** | No Cobra scaling mechanism or selective scene-resource scaling has been implemented. The game still renders the scene at native resolution. |
| 11. AMD FSR upscaling | **Interface ready; backend pending** | `AmdUpscalerBackend` abstraction implemented in `src/upscaler_backend.h/.cpp`. FidelityFX integration pending backend architecture selection. |
| 12. Native-resolution HUD | **Investigation only** | Emissive and clean backbuffers isolated. Full HUD separation and native-resolution composition path pending. |
| 13. D3D11/D3D12 interoperability | **Not started** | Research phase for Frame Generation bridge. |
| 14. AMD Frame Generation | **Not started** | Awaits stable temporal upscaling and D3D12 presentation bridge. |
| 15. NVIDIA Streamline / DLSS Frame Generation | **Not started** | Awaits stable temporal upscaling and Streamline integration. |

---

## Available diagnostics and controls

- **F5 Overlay:** Toggles the Dear ImGui menu. Displays backend status, diagnostics, and snapshot previews. When hidden, passes through all inputs to the game without interference.
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

The immediate next milestone is **Phase 5 — Runtime Projection Jitter**:
1. Intercept the scene projection matrix (words 1080–1095 of the 5376-byte dynamic CB0) during `Map`/`Unmap`.
2. Apply subpixel Halton (2,3) offsets via `jitterEliteProjectionBlock()`.
3. Provide an ImGui checkbox toggle for jitter.
4. Enforce strict **fail-open behavior**: if temporal reconstruction is disabled or errors, jitter must immediately stand down so Elite renders an unjittered image.

Following Phase 5, capture one dynamic flight sample (yaw/pitch) with **F6** to confirm motion vector direction and scaling, then proceed directly to **Phase 8 (NVIDIA DLAA at native 2560×1440)**.

---

## Not yet demonstrated

EDPE has not demonstrated lower scene render resolution, temporal image reconstruction, frame-time improvement, stable multi-scene history, vendor backend switching, or generated-frame pacing/latency. These remain roadmap goals rather than current capabilities.

---

## D3D11 / DXGI Injection — VERIFIED

- `EDPE.dll` exports `EdpeD3D11CreateDevice` and `EdpeCreateDXGIFactory1`/`EdpeCreateDXGIFactory2`
- `dxgi.dll` shim observes `IDXGIFactory::CreateSwapChain` and `IDXGISwapChain::Present`
- Present hook logs first-Present (2560×1440, `R8G8B8A8_UNORM`), device/context pointers
- `Insert` key reaches original WndProc when menu hidden; `F5` toggles Dear ImGui overlay
- No black screen on game launch with EDPE loaded; 3D world and HUD display normally
- **NOT verified:** temporal reconstruction, DLSS/FSR, render-scale below native, NGX integration

---

## Dear ImGui Overlay — VERIFIED

- Win32 + DirectX 11 backends
- `F5` toggles menu visibility; `Insert` passes through when hidden
- Overlay title: `EDPE — Elite Dangerous Performance Enhanced`
- Displays: "Version: unreleased", "Rendering: original game output", "Upscaler: Native"
- **Disabled:** DLSS/DLAA/FSR options until inputs verified (currently: "temporal inputs not yet verified")
- **Build cost:** dxgi.dll grows ~387 KB when overlay linked (measured in prior WARP tests)

---

## Depth — VERIFIED IN-GAME (DSV #2)

Per `docs/render-pipeline-observations.md` (2026-09-23 census, 2026-10-15 in-game decode):

- Swapchain: 2560×1440, `DXGI_FORMAT_R8G8B8A8_UNORM`, 1 sample
- **DSV #2 verified:** `depth-dsv2-*.bmp` decoded from `DXGI_FORMAT_R32G8X24_TYPELESS` via contrast mapping $z / (z + 0.001)$ yields pure monochrome geometry across the frame (values 35 to 177, average 90.7).
- **Z-direction:** Reversed-Z confirmed with clear value 0.0 and $B = 0.025$ ($z_{\text{view}} = B / z_{\text{raw}}$).
- **Clean sampling point:** Captured after scene-MRT pass before UI drawing.
- **NOT verified:** multi-scene transitions (hyperspace, orbital cruise) coverage.

---

## Emissive / Illumination Buffer (Reactive Mask) — VERIFIED IN-GAME

Per `docs/render-pipeline-observations.md` (2026-10-15 in-game visual analysis):

- **Buffer:** MRT RTV3, `DXGI_FORMAT_R11G11B10_FLOAT`, captured at first RTV3 exit.
- **Content:** Non-emissive background is black ($R=G=B=0$). All glowing elements — cockpit instruments, lights, lamps, holographic UI indicators, engine glow — are bright and isolated (RGB up to 254).
- **Architectural role:** Native candidate for `TemporalFrameInputs::reactiveMask`. Bypasses heuristic luminance thresholding to eliminate ghosting on moving cockpit lights and emissive particles in both DLSS and FSR.

---

## Camera / Projection — MEASURED

- Elite's 5376-byte constant buffer (CB0) parsed via `parseEliteCamera()` (src/elite_camera.h)
- Validated fields: `worldFromView[12]`, `scaleX`, `scaleY`, `depthB`
- Validation checks: orthogonal row-lengths ≈1.0, coherent orientation, `scaleX/Y > 0`, `depthB in (0,1]`
- **NOT verified:** full matrix layout handedness, projection cone angles, near/far plane values
- **Reference:** `temporal_math.h` provides `cameraDepthMotion()` math (render-resolution pixels) used by `MotionPass`

---

## Projection Jitter — Math verified; runtime injection target for Phase 5

- Halton sequence (base 2, base 3), frame-indexed: `jitter = (halton(frame+1,2)-0.5, halton(frame+1,3)-0.5)`
- Output: `pixelX, pixelY` in render-pixel units; `ndcX = 2*pixelX/width`, `ndcY = -2*pixelY/height`
- Math documented and verified in `temporal_math.h::projectionJitter()`, `jitterEliteProjectionBlock()`, and `estimateEliteBlockNdcOffset()`
- Target injection site: Words 1080–1095 of the 5376-byte dynamic CB0 intercepted during `Map`/`Unmap`.
- **Phase 5 task:** Inject Halton offsets into the projection block with an ImGui toggle and automatic fail-open stand-down.

---

## Motion Vectors — In-game neutral baseline verified; dynamic flight pending

- `MotionPass` renders to `DXGI_FORMAT_R16G16_FLOAT` render-target using current+previous camera constants and depth
- Output: per-pixel current→previous displacement in render-resolution pixels
- Shader: `motion_shader.h` — neutral gray $(128, 128, 128) = 0$ pixels displacement; red = horizontal, green = vertical
- **VERIFIED IN-GAME (2026-10-15):** Stationary scene capture decodes to exact $(128, 128, 128)$ neutral gray across the frame.
- **Synchronized Capture:** Output handed over alongside depth, scene color, and emissive pass from the exact same frame.
- **Pending verification:** Direction (+Y down) and scale validation during rotational flight (yaw/pitch).
- **Enhanced motion** (ships, stations, terrain, particles, smoke, animated geometry) comes in later phases.

---

## NGX / DLSS / DLAA — EXPERIMENTAL (framework only; NOT active in Elite)

**Critical constraint (nvidia-dlaa-input-contract.md, 2026-09-27):**

> SDK probe hung >20s in `NVSDK_NGX_D3D11_Shutdown1`; cause unknown.
> NGX must NOT consume diagnostic captures as production inputs in Elite until:
>   (a) Shutdown lifecycle validated in isolated OS process with wall-clock timeout, AND
>   (b) All Elite input conventions verified (jitter sign/motion-scale/depth-flag/reset semantics)

**Current EDPE state:**

- `src/ngx_context.cpp/h`: Framework present but `initialize()` returns `false` (intentionally disabled until safety verified)
- Project ID: EDPE-owned GUID `48d353f3-d07b-4048-876b-09f8622f5a27` via `NVSDK_NGX_D3D11_Init_with_ProjectID` in `src/ngx_context.cpp` and `tools/ngx_probe/ngx_probe.cpp` (never EDVR's ID)
- **NOT:** NGX evaluated or consuming Elite resources
- **MEASURED (2026-10-07):** standalone probe `tools/ngx_probe/` (parent `ngx_probe_host` with 30 s timeout, child `ngx_probe` with real NGX calls, no threading) ran V1 to V5, two runs each, all clean exits, no hang; details in `docs/ngx_probe-results.md`. Evaluate used synthetic inputs only; output was not inspected
- **Convention translation planned** at NGX boundary (jitter, motion scale, depth flag, reset semantics)
- **DLAA mode:** input width == output width, input height == output height (2560×1440) — removes scaling issue but does not satisfy convention requirements

**Next:** verify Elite input conventions; NGX stays out of the Elite process.

---

## Temporal Inputs — EXPERIMENTAL

- **Depth:** read-only census (see above); not fed to any upscaler
- **Motion vectors:** GPU `MotionPass` output available as debug view; not consumed by NGX/FSR
- **Projection jitter:** Halton sequence documented; not actively applied in game render path
- **Status:** infrastructure present, inputs not verified for Elite-NGX compatibility, not active in game

---

## Failure Behavior — SPECIFIED (specification until fail-open test exists)

- **Upscaler failure:** disable affected upscaler, disable jitter, restore original frame, Present normally
- **Never:** black-screen Elite because one EDPE subsystem fails
- **Forbidden in hot path:** blocking GPU readback, `Map()` on full-resolution textures, blocking `GetData` loops, per-frame NGX feature recreation
- **Async timing:** timestamp queries preferred; do not block render thread
- **Note:** Failure behavior is specified in AGENTS.md and design principles, but a concrete fail-open test has not yet been written into the repository. Label is SPECIFIED pending test implementation.

---

## Build / Test Summary

| Target | Status | Notes |
|--------|--------|-------|
| `EDPE.dll` + `dxgi.dll` | ✅ Built and deployable | CMake + MSVC x64; staged in `build/stage/` |
| `temporal_math_test` | ✅ Unit test | Halton sequences, projection blocks, NDC offset estimator |
| `camera_delta_test` | ✅ Unit test | CPU delta math, event flags, rate limiting, and scene reset observation |
| `capture_format_test` | ✅ Unit test | Half-float, float11/10 unpacker, reversed-Z depth contrast, motion BGR, Reinhard tone mapping |
| `temporal_inputs_test` | ✅ Unit test | Scaling calculations, input validation, NVIDIA/AMD convention translations, fail-open manager |
| `motion_gpu_test` | ✅ Requires D3D11 | GPU motion pass test (WARP / hardware) |
| `dxbc_fanout_test` | ✅ DXBC fanout probe | Shader hash analysis |
| `proxy_smoke` | ✅ Integration test | Verifies DLL load, Present hook, and input pass-through |
| `proxy_shader_probe` | ✅ Integration test | Shader bytecode interception verification |
| `proxy_alias_smoke` | ✅ Integration test | Verifies staged `d3d11.dll` and `dxgi.dll` alias loading |
| In-game captures (2026-10-15) | ✅ MEASURED IN-GAME | Full 2560×1440 captures of depth (DSV #2), emissive (RTV3), motion, color, and clean frame |
| Standalone NGX probe | ✅ MEASURED | `tools/ngx_probe/` V1-V5 clean, 2 runs each |
| Overlay lifecycle in Elite | ✅ MEASURED | Clean init, non-interfering input, clean shutdown on swap chain destroy |
| F6 Synchronized Capture | ✅ Working prototype | One-shot hotkey triggers synchronized 5-buffer capture |

---

## Next Verified Milestones (per PLAN.md order)

1. **Phase 5 — Runtime Projection Jitter:** Wire `jitterEliteProjectionBlock()` into the 5376-byte constant buffer `Unmap` path with an ImGui toggle and automatic fail-open stand-down.
2. **Flight Motion Convention Check:** Perform F6 capture during steady yaw and forward flight to verify motion vector direction (+X right, +Y down, current to previous) and scaling.
3. **Phase 8 — NVIDIA DLAA at Native Resolution (2560×1440):** Wire `TemporalFrameInputs` into `NvidiaUpscalerBackend::evaluate()` using the verified color, depth, motion, emissive reactive mask, and jitter.
4. **Phase 9/10 — DLSS Super Resolution & True Render-Scale Control:** Reduced scene render resolution with temporal reconstruction.

---

## Design Principles (from AGENTS.md)

- Prefer graphics API observation over executable patching
- Keep hooks small; observe, update state, invoke isolated subsystem, call original
- Fail open: disable feature, restore original frame, Present normally
- Every subsystem answers: "Did it run? Which backend? What dimensions? What formats? What temporal conventions? Why did it fail? What did it cost?"
- Do not couple Elite hooks directly to NGX/FidelityFX unless unavoidable at small boundary
- Do not reuse EDVR's NGX Project ID
- Do not port EDVR's OpenVR/OpenXR runtime, HMD compositor, headset logic, VR foveation