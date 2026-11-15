# Phase 14 Research: AMD FidelityFX Frame Generation Prototype

**Date:** 2026-11-14  
**Scope:** Phase 14 ([PLAN.md](../PLAN.md) §§5, 14–16, 18, 35, 43; `AGENTS.md` AMD FG & independence rules)  
**Prerequisites:** Phase 13B (D3D12 Flip-model Proxy Swapchain), Phase 17 (AMD Anti-Lag 2 SDK)  
**Status:** RESEARCH COMPLETE — Ready for prototype after Phase 13B & Phase 17.

---

## 1. Authoritative SDK Status (`SDK-DOCUMENTED`)

Official repository: **`GPUOpen-LibrariesAndSDKs/FidelityFX-SDK`** (FidelityFX API `v2.x` / FSR SDK 2.4.0 "Redstone", **MIT License**).

Unlike EDPE's Phase 11 upscaler (`external/ffx-dx11` native D3D11 FSR 3.1.2 port), official AMD Frame Generation is exposed through the **unified `ffx_api` ABI** (`ffx_api.h`, `ffx_framegeneration.h`, `dx12/ffx_api_framegeneration_dx12.h`) and requires **Direct3D 12** (or Vulkan):

1. **FSR 3.1.6 Frame Generation (`amd_fidelityfx_framegeneration.dll`):**
   * Cross-vendor compute optical-flow + interpolation pipeline. Runs on **AMD RDNA, NVIDIA RTX/GTX, and Intel Arc** (`VERIFIED` in official AMD docs & OptiScaler).
2. **FSR 4.0.x Frame Generation (`amd_fidelityfx_framegeneration.dll` / ML path):**
   * Machine-learning Frame Generation (same `ffx_framegeneration.h` / `ffx_api` entry points, selectable/auto-dispatched when `FFX_API_EFFECT_ID_FRAMEGENERATION` & RDNA 4+ ML model are available, plus `ffxOverrideVersion`).
3. **Architecture Independence Rule (`AGENTS.md` & `PLAN.md` §5):**
   * `AmdFsrFrameGenBackend` implements `IFrameGenerationBackend` cleanly separated from `IUpscalerBackend`.
   * Because `TemporalFrameInputs` + `FrameGenerationInputs` are backend-neutral, **NVIDIA DLSS SR + AMD FSR Frame Generation** (e.g., on RTX 20/30 series GPUs that lack hardware DLSS-G) works naturally with zero special-casing!

---

## 2. Exact `ffx_api` Frame Generation Lifecycle & Contracts (`SDK-DOCUMENTED`)

Inspecting `Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h` and `dx12/ffx_api_framegeneration_dx12.h` reveals four distinct constructs:

### 2.1 Swapchain Wrapping vs Swapchain Creation
At DXGI swapchain interception (Phase 13B):
* Either wrap EDPE's existing D3D12 Flip-model `IDXGISwapChain4` via `ffxCreateContextDescFrameGenerationSwapChainWrapDX12` (`*swapchain` is replaced in-place with AMD's `FrameInterpolationSwapChain`), **or** create it directly via `ffxCreateContextDescFrameGenerationSwapChainForHwndDX12` (`hwnd`, `desc`, `fullscreenDesc`, `dxgiFactory`, `gameQueue`).
* Wrap/create is paired with `ffxCreateContextDescFrameGenerationSwapChainVersionDX12` (`version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION`).

### 2.2 Frame Generation Context Creation
Create context via `ffxCreateContext(&fgContext, nullptr, ...)`, chaining:
* `ffxCreateContextDescFrameGeneration`:
  * `displaySize` = `{outputWidth, outputHeight}`
  * `maxRenderSize` = `{outputWidth, outputHeight}` (or dynamic max)
  * `flags`: `FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE` (matches Cobra `DSV #2`!) + `FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE` (when `HUDLessColor` is FP16/R11G11B10) + optional `FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT`
  * `backBufferFormat`: `FfxApiSurfaceFormat` matching the swapchain.
* `ffxCreateContextDescFrameGenerationVersion` (`version = FFX_FRAMEGENERATION_VERSION`, mandatory since FidelityFX SDK 2.1!).
* `ffxCreateContextDescFrameGenerationHudless`: **Essential for EDPE!** Allows `HUDLessColor` to use a **different format** (e.g. `R11G11B10_FLOAT` or `R16G16B16A16_FLOAT` from `NvidiaUpscalerBackend` / `AmdUpscalerBackend`) than `backBufferFormat` (`R8G8B8A8_UNORM`).
* `ffxCreateContextDescBackendDX12` (`device = bridge.d3d12Device()`).

### 2.3 Per-Frame Configure & Prepare (`ffxDispatchDescFrameGenerationPrepareV2`)
> **`SDK-DOCUMENTED` Deprecation Warning:** In FidelityFX SDK 2.1+, `ffxDispatchDescFrameGenerationPrepare` and `ffxDispatchDescFrameGenerationPrepareCameraInfo` are **deprecated**. Use `ffxDispatchDescFrameGenerationPrepareV2`, which embeds camera vectors (`cameraPosition`, `cameraUp`, `cameraRight`, `cameraForward`) and restores an active `reset` field.

Every frame before presenting:
1. **`ffxConfigureDescFrameGeneration`**:
   * `swapChain` = wrapped D3D12 swapchain
   * `frameID` = monotonic `uint64_t` counter. **Critical invariant (`ffx_framegeneration.h`):** *"Must increment by exactly one (1) for each frame. Any non-exactly-one difference will reset the frame generation logic."*
   * `frameGenerationEnabled` = `true/false`
   * `allowAsyncWorkloads` = configurable (allows compute optical flow overlap)
   * `HUDLessColor` = shared D3D12 texture containing **upscaled world before `UiCompositePass`**
   * `presentCallback` = UI composition callback (or `RegisterUiResourceDX12` — see Section 3!).
2. **`ffxDispatchDescFrameGenerationPrepareV2`**:
   * `commandList` = D3D12 command list on `bridge`
   * `frameID` = same `frameID`
   * `flags`: `FFX_FRAMEGENERATION_FLAG_NO_UI` or `FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES` / `DRAW_DEBUG_VIEW` (invaluable for diagnostics!)
   * `renderSize` = `{renderWidth, renderHeight}`
   * `jitterOffset` = `{jitterX, jitterY}`
   * `motionVectorScale` = `{motionScaleX, motionScaleY}` (EDPE MVs are in render pixels $\rightarrow$ `(1.0f, 1.0f)`, or flip Y if sign test dictates; verify with `DRAW_DEBUG_VIEW`!)
   * `frameTimeDelta` = `frameTimeMs`
   * `reset` = `inputs.reset` (from `CameraDelta` cut/jump detection)
   * `cameraNear` = Cobra near plane (note reversed-Z infinite convention `depthB = 0.025f`!), `cameraFovAngleVertical` (extracted from `CB0` projection `[1][1]`: $2 \arctan(1 / P_{11})$), `cameraPosition/Up/Right/Forward` (from `CB0` view matrix!).
   * `depth` = shared `R32_FLOAT` copy of `DSV #2`, `motionVectors` = shared `R16G16_FLOAT` from `MotionPass`.
3. **Registering UI (`ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12`) OR `presentCallback`:**
   * If `uiResource` is provided with `FFX_UI_COMPOSITION_FLAG_USE_PREMULTIPLIED_ALPHA`, FidelityFX automatically composites crisp UI onto **both** real and interpolated frames.

---

## 3. How EDPE Solves HUD Ghosting Cleanly (`UiLayer` Advantage)

In third-party injectors like `OptiScaler` (`OptiFG + HUDFix`) or `Nukem9/dlssg-to-fsr3`, games without native FG do not expose a HUD-less buffer, forcing brittle runtime heuristics (`HUDFix limit`, `Immediate Capture`) that break overlays and ghost on cockpit glass.

**EDPE already has full HUD separation in `v0.7.0` (`VERIFIED` in [src/ui_layer.cpp](../src/ui_layer.cpp)):**
* `worldReconstruction` (output of `NvidiaUpscalerBackend`, `AmdUpscalerBackend`, `IntelUpscalerBackend`, or `s_clean_hdr_scene_texture`) is **100% HUD-less**.
* `UiLayer` holds the exact isolated HUD textures at native output resolution:
  * `hdrLayerTex_` (`R16G16B16A16_FLOAT`: `.rgb` linear UI emission, `.a` transmittance $T = \prod(1-\alpha)$)
  * `ldrLayerTex_` (`R8G8B8A8_UNORM`: tonemapped cockpit/menu UI)
  * `multLayerTex_` (`R8G8B8A8_UNORM`: vignettes/drop shadows).

### Recommended EDPE UI Handoff to FSR FG
Instead of trying to force EDPE's 3-texture transmittance/multiply math into a single `RegisterUiResourceDX12` texture, use AMD's **`presentCallback` (`FfxApiPresentCallbackFunc`)**:

```cpp
// Conceptual callback executed by FidelityFX for BOTH real and generated frames:
ffxReturnCode_t edpeFfxPresentCallback(ffxCallbackDescFrameGenerationPresent* params, void* userCtx) {
    auto* fg = static_cast<AmdFsrFrameGenBackend*>(userCtx);
    // params->commandList: ID3D12GraphicsCommandList*
    // params->currentBackBuffer: frame to present (real OR interpolated)
    // params->outputSwapChainBuffer: final swapchain RTV target
    // params->isGeneratedFrame: distinguishes interpolated vs real frame!
    //
    // 1. Copy/composite EDPE's shared native UI layers (ldrLayer + hdrLayer.a transmittance)
    //    via a lightweight D3D12 HLSL port of kUiLayerCompositePsHlsl onto outputSwapChainBuffer.
    // 2. If Dear ImGui overlay (F5 / F7 Perf HUD) is visible, draw ImGui on top (DX12 backend)
    //    ONLY here so ImGui itself is crisp and never ghosted or fed into optical flow!
    return FFX_API_RETURN_OK;
}
```
* **Why `presentCallback` wins for EDPE:**
  1. Preserves EDPE's exact linear transmittance & tone-mapped UI fidelity (`kUiLayerCompositePsHlsl`) on **both real and interpolated frames**.
  2. Keeps EDPE's Dear ImGui (`F5` menu / `F7` FPS graph) out of `HUDLessColor` so UI text never distorts optical flow vectors.
  3. Provides `params->isGeneratedFrame` directly for accurate FG diagnostics (`renderedFPS` vs `presentedFPS` vs `generatedFrames`, `AGENTS.md`).

---

## 4. Mandatory AMD Anti-Lag 2 Coupling (`SDK-DOCUMENTED`)

Per `GPUOpen-LibrariesAndSDKs/AntiLag2-SDK` README and `fakenvapi` (`REFERENCE-CODE`):
* Without Anti-Lag 2, Frame Generation queues extra buffered frames, spiking flight-stick/mouse latency.
* When AMD FSR FG is enabled alongside Anti-Lag 2 (Phase 17):
  1. Call `AMD::AntiLag2DX12::MarkEndOfFrameRendering(&al2Dx12Ctx)` right after game rendering finishes / before FG Present.
  2. Frame type (`bInterpolatedFrame`) must be signaled per presented frame via `SetFrameGenFrameType` or the DXGI swapchain private data GUID used by FSR 3.1.1+ (`SDK-DOCUMENTED` in `AntiLag2-SDK`).
* **Cross-vendor note:** On NVIDIA GPUs running FSR FG (e.g., RTX 3080), Anti-Lag 2 will not initialize (`amdxc64.dll` absent); EDPE's `ILatencyBackend` automatically pairs **NVIDIA NVAPI Reflex + AMD FSR FG**! Wait: verify that NVAPI Reflex does not conflict with FidelityFX swapchain pacing (`Streamline` isn't loaded in that mode, so NVAPI Reflex `NvAPI_D3D_SetLatencyMarker` + `Sleep` is safe).

---

## 5. Failure Modes & Fail-Open Guarantees (`PLAN.md` §16, §18)

| Failure Scenario | Required Fail-Open Behavior |
|:---|:---|
| Missing `amd_fidelityfx_framegeneration.dll` or D3D12 bridge init fails | Grey out `AMD FSR Frame Generation` in F5 menu with reason; render & present via standard D3D11 path. |
| Alt+Tab / resolution change (`ResizeBuffers`) | Synchronize with `PresentResizeMutex` + `ffxConfigure(frameGenerationEnabled=false)` / drain D3D12 queue before freeing `HUDLessColor`. |
| `frameID` skips (`> +1`) or `reset == true` (jump/SC transition) | Reset FG history cleanly for 1 frame (`frameGenerationEnabled` stays on, no ghosting streaks across hyperspace exits). |
| Per-frame FG dispatch returns error | Disable FG, stop calling `presentCallback` interpolation, pass `finalBackbuffer` directly to D3D12 swapchain Present, keep SR (`DLSS`/`FSR`/`XeSS`) running! |

---

## 6. Reference Provenance & Implementation Checklist

* **Official SDK:** `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK` (`MIT`), headers `ffx_api.h`, `ffx_framegeneration.h`, `dx12/ffx_api_framegeneration_dx12.h`.
* **Ecosystem References:** `OptiScaler` (`OptiFG` + `Dx11wDx12SC`, GPL-3.0 study only — pay close attention to `PrepareFGResize` and `FGHUDFix` caveats), `Nukem9/dlssg-to-fsr3` (optical flow & swapchain pacing reference).
* **Prototype Tasks:**
  1. Add `FfxApi` loader (`ffxCreateContext`/`Dispatch`/`Configure`/`Query`/`DestroyContext`) for `amd_fidelityfx_framegeneration.dll`.
  2. Port `kUiLayerCompositePsHlsl` (25 lines HLSL) to a D3D12 PSO invoked inside `presentCallback`.
  3. Wire `FrameGenerationInputs` from `ContextCensus` (`s_clean_hdr_scene_texture`, `DSV #2` `R32_FLOAT`, `MotionPass`, `CB0` camera vectors/FOV).
  4. Validate motion-vector polarity & tear-free pacing with `FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW` and `FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES`.
