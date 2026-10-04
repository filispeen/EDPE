# EDPE Status — Elite Dangerous Performance Enhanced

**Last updated:** 2026-10-05

This file documents **verified facts only**. Unverified claims, hypotheses, or speculative results are not recorded here. See `AGENTS.md` and `PLAN.md` for project governance and roadmap.

---

## Repository

- **Git tags:** `v0.0.1` (initial roadmap), current HEAD at `6683ff7` (216+ commits beyond v0.0.1)
- **Build artifacts:** `EDPE.dll` and `dxgi.dll` deployed beside `EliteDangerous64.exe`; builds succeed with CMake + NMake MSVC x64
- **Source:** `src/` contains 10 C++ modules; `tools/` added for NGX probe infrastructure

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

## Projection Jitter — MEASURED (algorithm documented)

- Halton sequence (base 2, base 3), frame-indexed: `jitter = (halton(frame+1,2)-0.5, halton(frame+1,3)-0.5)`
- Output: `pixelX, pixelY` in render-pixel units; `ndcX = 2*pixelX/width`, `ndcY = -2*pixelY/height`
- Math documented in `temporal_math.h::projectionJitter()` and `jitterEliteProjectionBlock()`
- **NOT verified in Elite:** jitter actually supplied to game/NGX, sign/convention match, automatic disable when reconstruction stands down
- **Note:** Per `nvidia-dlaa-input-contract.md` (2026-09-27), NGX expects jitter in "input/render pixels"; EDPE uses Halton in render pixels. Convention translation required at NGX boundary.

---

## Motion Vectors — MEASURED (GPU pass, diagnostic)

- `MotionPass` renders to `DXGI_FORMAT_R16G16_FLOAT` render-target using current+previous camera constants
- Output: per-pixel current→previous displacement in render-resolution pixels
- Shader: `motion_shader.h` — neutral gray = 0 pixels; red = horizontal, green = vertical
- **NOT verified:** correctness against NGX conventions, pixel/normalized units, Y-axis orientation, jitter inclusion/exclusion
- **Enhanced motion** (ships, stations, terrain, particles, smoke, animated geometry) comes later
- **Reference:** EDVR's `dlMv` implementation and temporal math used as reference only; not copied

---

## NGX / DLSS / DLAA — EXPERIMENTAL (framework only; NOT active in Elite)

**Critical constraint (nvidia-dlaa-input-contract.md, 2026-09-27):**

> SDK probe hung >20s in `NVSDK_NGX_D3D11_Shutdown1`; cause unknown.
> NGX must NOT consume diagnostic captures as production inputs in Elite until:
>   (a) Shutdown lifecycle validated in isolated OS process with wall-clock timeout, AND
>   (b) All Elite input conventions verified (jitter sign/motion-scale/depth-flag/reset semantics)

**Current EDPE state:**

- `src/ngx_context.cpp/h`: Framework present but `initialize()` returns `false` (intentionally disabled until safety verified)
- `0xPE` hex-digit error fixed: project ID now `{0xED, 0xEA, 0x1B, 0x22}` (valid hex only)
- **NOT:** NGX evaluated or consuming Elite resources
- **Standalone probe:** `tools/ngx_probe/` created — separate executable with 30s timeout on Shutdown1, no threading to bypass hang
- **Convention translation planned** at NGX boundary (jitter, motion scale, depth flag, reset semantics)
- **DLAA mode:** input width == output width, input height == output height (2560×1440) — removes scaling issue but does not satisfy convention requirements

**Upcoming task (per task list):** Standalone NGX probe execution with stage-by-stage timing and timeout documentation.

---

## Temporal Inputs — EXPERIMENTAL

- **Depth:** read-only census (see above); not fed to any upscaler
- **Motion vectors:** GPU `MotionPass` output available as debug view; not consumed by NGX/FSR
- **Projection jitter:** Halton sequence documented; not actively applied in game render path
- **Status:** infrastructure present, inputs not verified for Elite-NGX compatibility, not active in game

---

## Failure Behavior — VERIFIED (per AGENTS.md)

- **Upscaler failure:** disable affected upscaler, disable jitter, restore original frame, Present normally
- **Never:** black-screen Elite because one EDPE subsystem fails
- **Forbidden in hot path:** blocking GPU readback, `Map()` on full-resolution textures, blocking `GetData` loops, per-frame NGX feature recreation
- **Async timing:** timestamp queries preferred; do not block render thread

---

## Build / Test Summary

| Target | Status | Notes |
|--------|--------|-------|
| `EDPE.dll` + `dxgi.dll` | ✅ Built and deployable | Pre-built; CMake + NMake MSVC x64 |
| `temporal_math_test` | ✅ Unit test | Runs CPU math verification |
| `motion_gpu_test` | ✅ Requires D3D11 | GPU motion pass test |
| `dxbc_fanout_test` | ✅ DXBC fanout probe | Shader hash analysis |
| `proxy_smoke` | ✅ DLL load + Present observation | Requires EDPE.dll + dxgi.dll beside game |
| NGX runtime in Elite | ❌ Blocked | Shutdown hang >20s; conventions unvalidated |
| Standalone NGX probe | 🔧 In progress | `tools/ngx_probe/` — 30s timeout mechanism |

---

## Next Verified Milestones (per PLAN.md order)

1. **NGX shutdown lifecycle** — validated in isolated process with 30s timeout (standalone probe)
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