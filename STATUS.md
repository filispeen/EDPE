# EDPE Status — Elite Dangerous Performance Enhanced

**Last updated:** 2026-10-05

This file documents **verified facts only**. Unverified claims, hypotheses, or speculative results are not recorded here. See `AGENTS.md` and `PLAN.md` for project governance and roadmap.

---

## Repository

- **Git tags:** `v0.0.1` (initial roadmap), current HEAD at `6683ff7` (216+ commits beyond v0.0.1)
- **Build artifacts:** `EDPE.dll` and `dxgi.dll` deployed beside `EliteDangerous64.exe`; builds succeed with CMake + NMake MSVC x64
- **Source:** `src/` contains 10 C++ modules; `tools/` added for NGX probe infrastructure

---

## Roadmap phase status

| Phase | Status | Current state |
|-------|--------|---------------|
| 0. Reconnaissance | **Substantially complete; ongoing** | The target is Elite Dangerous Odyssey's 2D D3D11 path. The target executable imports, swapchain, backbuffer format, proxy behavior, and several renderer resources have been investigated and documented. Other graphics configurations, complete resource lifetime coverage, and long-session behavior remain unverified. |
| 1. D3D11 injection | **Working prototype; limited coverage** | `EDPE.dll` forwards D3D11 device creation; the `dxgi.dll` shim observes factories, swapchains, and `Present`; logging and the F5 Win32/DX11 ImGui overlay work in the documented local game test. Only the observed exports are covered. Coexistence with other graphics mods and long-session stability are unverified. |
| 2. Resource census | **Partial** | D3D11 context instrumentation records render/depth target binds, draw-related state, shader information, and selected captures. The roadmap's full inventory of resource creation, copies, dispatches, viewports, lifetimes, and call relationships is not complete. |
| 3. Scene depth | **Candidate identified; not production-validated** | Depth candidates have been captured and inspected alongside scene output. Per-pixel association, clear/far behavior, Z direction, near/far behavior, safe sampling point, and validity across scenes are not established well enough for a vendor backend. |
| 4. Camera and projection | **Experimental** | Camera constant-buffer layouts and projection usage have been inspected, with CPU parsing and GPU-copy prototypes. The chosen camera/depth association is based on observed passes and sparse samples; it is not verified for every depth-writing draw, scene, or transition. |
| 5. Projection jitter | **Math prototype only; runtime use blocked** | Halton jitter and projection-block transformation helpers exist. Jitter is intentionally not applied in Elite because full shader/pass coverage and a guaranteed fail-open path that removes jitter have not been proven. |
| 6. Motion vectors | **Diagnostic prototype; not backend-ready** | A D3D11 GPU motion pass and CPU math have synthetic/WARP evidence, and sparse in-game diagnostic captures show finite output during camera movement. Nearby geometry, per-pixel depth association, moving objects, scene cuts, stable camera selection, and vendor-specific direction/scale conventions still need validation. The capture path is explicitly experimental/manual. |
| 7. Shared temporal input layer | **Not implemented** | There is no backend-neutral `TemporalFrameInputs` handoff or production history/reset/resource-lifetime manager. Current color, depth, camera, and motion observations are separate diagnostics. |
| 8. NVIDIA DLAA | **Blocked / not implemented** | `NgxContext` is a disabled stub. MEASURED (2026-10-07): the standalone probe (`tools/ngx_probe`, RTX 3060, driver 617.14, `nvngx_dlss.dll` 310.9.1.0) completed init, capability query, DLAA create and evaluate on synthetic inputs, and shutdown with clean exits in variants V1 to V5, two runs each; the earlier reported shutdown hang was not reproduced. NGX is not run in the game. The Elite input conventions and safe jitter rollback are unresolved. |
| 9. NVIDIA DLSS Super Resolution | **Not started** | No evaluation path is implemented. |
| 10. True render-scale control | **Not started** | No Cobra scaling mechanism or selective scene-resource scaling has been implemented. The game still renders the expensive scene at native resolution. |
| 11. AMD FSR upscaling | **Not started** | No FidelityFX backend is implemented. The project notes compare the D3D11 community route with an official API/interoperability route, but no production choice or evaluation exists. |
| 12. Native-resolution HUD | **Investigation only** | Captured scene color includes HUD content. HUD/world classification and a native-resolution composition path are not implemented. |
| 13. D3D11/D3D12 interoperability | **Not started** | No shared-resource/fence experiment or overhead measurements are documented. |
| 14. AMD Frame Generation | **Not started** | No AMD FG or presentation integration exists. |
| 15. NVIDIA Streamline / DLSS Frame Generation | **Not started** | No Streamline, DLSS-G, or Reflex integration exists. |

---

## Available diagnostics and controls

- The overlay is toggled with **F5** and reports the original rendering path. Temporal options remain disabled.
- Experimental F5 actions can capture selected scene/depth/camera/motion candidates. Captures are diagnostic evidence, not production temporal inputs.
- The proxy writes `edpe.log` beside the host executable. Manual captures may also save visual BMP screenshots under `edpe-captures`; those images are not raw depth or motion data.
- The repository defines CPU math, WARP motion, DXBC fanout, and proxy smoke tests. Earlier project notes record successful runs, but tests were not rerun for this status review.

---

## Next milestone

The next roadmap milestone is a safe, native-resolution **NVIDIA DLAA** vertical slice, but implementation should wait for two prerequisites:

1. Resolve and document the NGX shutdown/lifecycle behavior so the game process can initialize and exit safely.
2. Validate the selected color, depth, camera, and motion resources and conventions in Elite, and prove that any applied jitter can always be removed when evaluation fails.

Then formalize the shared temporal input boundary, implement DLAA with input and output dimensions equal, and validate cockpit, station, space, camera movement, HUD, moving ships, reset behavior, fallback, and GPU cost. Only after that should reduced-resolution rendering and DLSS SR begin.

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

## Depth — MEASURED (read-only census)

Per `docs/render-pipeline-observations.md` (2026-09-23 game build `2026.09.03.332841`):

- Swapchain: 2560×1440, `DXGI_FORMAT_R8G8B8A8_UNORM`, 1 sample
- DSV census through `OMSetRenderTargets` hook (slot 33):
  - 13 distinct DSV pointers observed at full resolution
  - First three 2560×1440 DSVs: `R32G8X24_TYPELESS`/`D32_FLOAT_S8X24_UINT` (bind-flag 0x48, 1 sample)
  - Later 2560×1440 DSVs: `R24G8_TYPELESS`/`D24_UNORM_S8_UINT` (bind-flag 0x48, 1 sample)
  - Smaller targets: 256×256, 512×512, 3072×3072; formats include `R16_TYPELESS`/`D16_UNORM`, `R11G11B10_FLOAT`
  - Every logged target had bind flags `0x48` (`DEPTH_STENCIL | SHADER_RESOURCE`) and 1 sample
  - No DSV bound at first `Present`; depth identification requires earlier frame observation
- **NOT verified:** standard vs reversed Z, near/far planes, clear values, draw-to-DSV association, safe sampling point
- **Design principle:** No EDVR source code copied; EDPE measures 2D path independently

---

## Camera / Projection — MEASURED

- Elite's 5376-byte constant buffer (CB0) parsed via `parseEliteCamera()` (src/elite_camera.h)
- Validated fields: `worldFromView[12]`, `scaleX`, `scaleY`, `depthB`
- Validation checks: orthogonal row-lengths ≈1.0, coherent orientation, `scaleX/Y > 0`, `depthB in (0,1]`
- **NOT verified:** full matrix layout handedness, projection cone angles, near/far plane values
- **Reference:** `temporal_math.h` provides `cameraDepthMotion()` math (render-resolution pixels) used by `MotionPass`

---

## Projection Jitter — math prototype only; runtime use blocked

- Halton sequence (base 2, base 3), frame-indexed: `jitter = (halton(frame+1,2)-0.5, halton(frame+1,3)-0.5)`
- Output: `pixelX, pixelY` in render-pixel units; `ndcX = 2*pixelX/width`, `ndcY = -2*pixelY/height`
- Math documented in `temporal_math.h::projectionJitter()` and `jitterEliteProjectionBlock()`
- **NOT verified in Elite:** jitter actually supplied to game/NGX, sign/convention match, automatic disable when reconstruction stands down
- **Note:** Per `nvidia-dlaa-input-contract.md` (2026-09-27), NGX expects jitter in "input/render pixels"; EDPE uses Halton in render pixels. Convention translation required at NGX boundary.
- **Important:** Jitter is intentionally not applied in the Elite render path until runtime conventions are validated and a fail-open disable path is proven.

---

## Motion Vectors — diagnostic prototype; not backend-ready

- `MotionPass` renders to `DXGI_FORMAT_R16G16_FLOAT` render-target using current+previous camera constants
- Output: per-pixel current→previous displacement in render-resolution pixels
- Shader: `motion_shader.h` — neutral gray = 0 pixels; red = horizontal, green = vertical
- **NOT verified:** correctness against NGX conventions, pixel/normalized units, Y-axis orientation, jitter inclusion/exclusion
- **Enhanced motion** (ships, stations, terrain, particles, smoke, animated geometry) comes later
- **Reference:** EDVR's `dlMv` implementation and temporal math used as reference only; not copied
- **Important:** Diagnostic prototype available for inspection; not yet ready for vendor backend consumption.

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
| `EDPE.dll` + `dxgi.dll` | ✅ Built and deployable | Pre-built; CMake + NMake MSVC x64 |
| `temporal_math_test` | ✅ Unit test | Runs CPU math verification |
| `motion_gpu_test` | ✅ Requires D3D11 | GPU motion pass test |
| `dxbc_fanout_test` | ✅ DXBC fanout probe | Shader hash analysis |
| `proxy_smoke` | ✅ DLL load + Present observation | Requires EDPE.dll + dxgi.dll beside game |
| NGX runtime in Elite | ❌ Blocked | Conventions unvalidated; NGX not run in game |
| Standalone NGX probe | ✅ MEASURED | `tools/ngx_probe/` V1-V5 clean, 2 runs each |
| Overlay lifecycle in Elite (4d8e157) | ✅ MEASURED | Game run to 3D world past frame 15360: one "Dear ImGui ready", no per-Present re-init, clean exit with "swap chain destroyed; shutting overlay down" (earlier 295ff46 build: 326 re-inits, then game exit) |

---

## Next Verified Milestones (per PLAN.md order)

1. **NGX shutdown lifecycle** - MEASURED clean in an isolated process (V1 to V5, 2 runs each); repeated init/shutdown cycles and in-game behavior remain unverified
2. **Elite input conventions** — jitter sign/motion-scale/depth-flag/reset semantics
3. **DLAA at native resolution** — input == output (2560×1440), conventions verified
4. **DLSS Super Resolution** — after DLAA validated, reduced render resolution

---

## Design Principles (from AGENTS.md)

- Prefer graphics API observation over executable patching
- Keep hooks small; observe, update state, invoke isolated subsystem, call original
- Fail open: disable feature, restore original frame, Present normally
- Every subsystem answers: "Did it run? Which backend? What dimensions? What formats? What temporal conventions? Why did it fail? What did it cost?"
- Do not couple Elite hooks directly to NGX/FidelityFX unless unavoidable at small boundary
- Do not reuse EDVR's NGX Project ID
- Do not port EDVR's OpenVR/OpenXR runtime, HMD compositor, headset logic, VR foveation