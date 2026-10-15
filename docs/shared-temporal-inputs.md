# Shared temporal input layer & upscaler backend architecture

**Date:** 2026-10-15  
**Scope:** Phase 7 (Roadmap PLAN.md section 4 and 27; AGENTS.md core architecture rule)  
**Status:** IMPLEMENTED & TESTED (unit tests and WARP build passing).

---

## 1. Architectural principle

Per `AGENTS.md` and `PLAN.md`:

```text
Elite Dangerous / Cobra
        │
        ▼
EDPE renderer observation (color, depth, camera)
        │
        ▼
EDPE temporal input generation (jitter, motion vectors, reset detection)
        │
        ▼
shared TemporalFrameInputs
        │
        ├──────────────────────┬──────────────────────┐
        ▼                      ▼                      ▼
NullUpscalerBackend    NvidiaUpscalerBackend   AmdUpscalerBackend
     (Bypass)               (DLSS / DLAA)           (FSR SR / NativeAA)
```

1. **EDPE owns temporal reconstruction inputs:**
   - Depth discovery, projection jitter, camera matrices, motion vector generation, and reset detection belong exclusively to EDPE.
2. **Vendors live behind backend abstractions:**
   - Neither NGX nor FidelityFX types leak into high-level EDPE renderer hooks or swapchain presentation logic.
   - Each backend translates EDPE's unified conventions into vendor-specific parameters and flags.
3. **Fail-open contract:**
   - If temporal evaluation fails or inputs are invalid, the upscaler coordinator disables temporal reconstruction and presents the untouched original frame. Elite never black-screens.

---

## 2. Shared structures (`src/temporal_inputs.h`)

### `TemporalFrameInputs`
* Resource pointers: `color`, `depth`, `motionVectors` (with optional `reactiveMask`, `transparencyMask`, `exposure`).
* Dimension metadata: `renderWidth`, `renderHeight`, `outputWidth`, `outputHeight`.
* Temporal offsets: `jitterX`, `jitterY` (in render pixel units, Halton base 2,3).
* Frame pacing: `frameTimeMs`.
* Camera & history state: `reset`, `cameraNow`, `cameraPrevious`.
* Depth conventions: `depthInverted = true` (reversed-Z), `depthInfinite = true`.
* Motion conventions: `motionJittered = false` (excludes jitter), `motionIsLowRes = true` (render resolution).

### `calculateRenderResolution`
Computes integer render dimensions for any target `UpscaleMode` (Quality = 1.5x, Balanced = 1.7x, Performance = 2.0x, UltraPerformance = 3.0x).  
Enforces even-number dimensions (`(rw + 1) & ~1u`) and caps render dimensions so they never exceed presentation output dimensions.

---

## 3. Convention translation contracts

### NVIDIA DLSS / DLAA (`translateToNvidia`)
* **Jitter:** Passed directly in render pixels (NGX evaluation helper expectation).
* **Motion Vectors:** Scale set to `(1.0, 1.0)` (EDPE motion is in render pixels; NGX treats 0.0 or 1.0 as 1:1 pixel displacement).
* **Depth:** Flag `NVSDK_NGX_DLSS_Feature_Flags_DepthInverted` set to `true`.
* **Flags:** `MVLowRes = true`, `MVJittered = false`, `IsHDR = true`.
* **Reset:** `InReset = 1` when `inputs.reset == true`.

### AMD FidelityFX FSR (`translateToAmd`)
* **Jitter:** Render pixel offsets.
* **Motion Vectors:** Scale set to `(1.0, 1.0)`.
* **Depth:** Flags `FFX_FSR_ENABLE_DEPTH_INVERTED` and `FFX_FSR_ENABLE_DEPTH_INFINITE`.
* **Reset:** `reset = true` on camera cuts.

---

## 4. Upscaler manager & backends (`src/upscaler_backend.h` / `.cpp`)

* **`IUpscalerBackend`:** Pure virtual interface defining `initialize`, `shutdown`, `evaluate`, `isAvailable`, `isModeSupported`, `resetHistory`.
* **`NullUpscalerBackend`:** Always available; bypass mode that leaves original frame intact.
* **`NvidiaUpscalerBackend`:** Wraps `NgxContext`; enforces safety constraints (`nvidia-dlaa-input-contract.md`), translates conventions via `translateToNvidia`, and fails open.
* **`AmdUpscalerBackend`:** Framework for FidelityFX FSR; currently reports unavailable with documented reason pending D3D11/D3D12 interop investigation.
* **`UpscalerManager`:** Singleton coordinator managing active backend, mode, input validation, fail-open dispatch, and `UpscalerDiagnostics`.
* **Dear ImGui integration ([src/ui.cpp](file:///F:/Code/С++/EDPE/src/ui.cpp)):**
  - Displays current upscaler, mode, and diagnostics status message.
  - Automatically queries and displays vendor unavailability reasons when features are disabled.

---

## 5. Verification evidence

Tested via `tests/temporal_inputs.cpp` (`ctest -R temporal_inputs`):
1. **Resolution math:** Verified 1:1 for Native/NativeAA, 1708×960 for 1440p Quality, 1280×720 for 1440p Performance, 1920×1080 for 4K Performance, zero/null input rejection, and even-pixel rounding.
2. **Input validation:** Verified rejection of missing color, depth, motion vectors, inverted dimension bounds, NaN/inf jitter, and negative frame times.
3. **Convention translation:** Verified exact mapping of Halton jitter, motion vector 1.0 scale, reversed-Z depth flags, and reset propagation for both NVIDIA and AMD contracts.
4. **Manager fail-open:** Verified safe bypass in None mode, rejected selection of unvalidated vendor backends, and guaranteed fallback to original frame upon evaluation failure.
