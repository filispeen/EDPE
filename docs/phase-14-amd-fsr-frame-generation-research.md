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

### 2.3 Per-Frame Configure, Prepare & Dispatch Sequence (`ffxDispatchDescFrameGenerationPrepareV2`)
> **`SDK-DOCUMENTED` Deprecation & Ordering Warning (`FSR SDK 2.4.0`):**
> * In FidelityFX SDK 2.1+, `ffxDispatchDescFrameGenerationPrepare` and `ffxDispatchDescFrameGenerationPrepareCameraInfo` are **deprecated**. Use `ffxDispatchDescFrameGenerationPrepareV2`, which embeds camera vectors (`cameraPosition`, `cameraUp`, `cameraRight`, `cameraForward`) and restores an active `reset` field.
> * **Strict Per-Frame Execution Order (`FSR SDK 2.4`):** Always execute in exact sequence **(1)** `ffxConfigure(&fgContext, &configDesc.header)` $\rightarrow$ **(2)** `ffxDispatch(&fgContext, &prepareV2Desc.header)` $\rightarrow$ **(3)** `ffxDispatch(&fgContext, &fgDispatchDesc.header)` (or `FrameInterpolationSwapChain::Present`). Violating this order breaks FSR 4.0.x ML Frame Generation driver dispatch!

Every frame before presenting:
1. **`ffxConfigureDescFrameGeneration`**:
   * `swapChain` = wrapped D3D12 swapchain
   * `frameID` = monotonic `uint64_t` counter. **Critical invariant (`ffx_framegeneration.h`):** *"Must increment by exactly one (1) for each frame. Any non-exactly-one difference will reset the frame generation logic."*
   * `frameGenerationEnabled` = `true/false`
   * `allowAsyncWorkloads` = configurable (allows compute optical flow overlap)
   * `HUDLessColor` = shared D3D12 texture containing **upscaled world before `UiCompositePass`**
   * `presentCallback` = UI / Dear ImGui composition callback (or paired with `RegisterUiResourceDX12` — see Section 3!).
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
   * Note that `RegisterUiResourceDX12` accepts only a **single** premultiplied texture (`FFX_UI_COMPOSITION_FLAG_USE_PREMULTIPLIED_ALPHA`), whereas EDPE's `UiLayer` produces **three** layers (`multLayerTex_`, `hdrLayerTex_`, `ldrLayerTex_`). Use one of the two exact patterns in Section 3.

---

## 3. How EDPE Solves HUD Ghosting Cleanly (`UiLayer` Three-Layer Handoff)

In third-party injectors like `OptiScaler` (`OptiFG + HUDFix`) or `Nukem9/dlssg-to-fsr3`, games without native FG do not expose a HUD-less buffer, forcing brittle runtime heuristics (`HUDFix limit`, `Immediate Capture`) that break overlays and ghost on cockpit glass.

**EDPE already separates all UI in `v0.7.0` (`VERIFIED` in [src/ui_layer.cpp](../src/ui_layer.cpp#L15-L83), [#L496-L538](../src/ui_layer.cpp#L496-L538)):**
* `worldReconstruction` (output of `NvidiaUpscalerBackend`, `AmdUpscalerBackend`, `IntelUpscalerBackend`, or `s_clean_hdr_scene_texture`) is **100% HUD-less**.
* `UiLayer` holds **three** separated native-resolution HUD textures composited by `kUiLayerCompositeCsHlsl` / `kUiLayerCompositePsHlsl`:
  * `hdrLayerTex_` (`R16G16B16A16_FLOAT`: `.rgb` linear emission, `.a` transmittance $T = \prod(1-\alpha)$)
  * `ldrLayerTex_` (`R8G8B8A8_UNORM`: tonemapped HUD + menu overlay emission)
  * `multLayerTex_` (`R8G8B8A8_UNORM`: multiplicative backdrop darkening under cockpit panels/vignettes).

### Two Correct UI Handoff Options to FSR FG (Preserving `multLayerTex_`)

1. **Option A (Unified Premultiplied Pack `packPremultipliedUiForFgCs` → `RegisterUiResourceDX12` — shares pipeline with Phase 15):**
   * Run a lightweight D3D11 compute pass before copying to the Phase 13B bridge, packing all three layers (`hdrLayerTex_`, `ldrLayerTex_`, `multLayerTex_`) into a single `DXGI_FORMAT_R8G8B8A8_UNORM` premultiplied-alpha texture `fgPackedUiTex_`:
     * Subtract black floor per `kUiLayerCompositeCsHlsl`: $\text{UI}_{rgb} = \max(L_{rgb} - \text{blackFloor} \cdot T, 0)$
     * Combine linear transmittance and backdrop darkening into scalar premultiplied alpha: $\text{UI}_a = 1 - \text{saturate}(T) \cdot \text{luma}(M_{rgb})$ (where $M_{rgb} = (1,1,1)$ when `!hasMultiply`).
     * Standard FG premultiplied composition $C_{\text{final}} = C_{\text{scene}} \cdot (1 - \text{UI}_a) + \text{UI}_{rgb} = C_{\text{scene}} \cdot T \cdot \text{luma}(M_{rgb}) + \text{UI}_{rgb}$ retains dark panel backdrops (`multLayerTex_`), HUD emission, and crisp menus in one shared buffer!
   * Register `fgPackedUiTex_` via `ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12` (`FFX_UI_COMPOSITION_FLAG_USE_PREMULTIPLIED_ALPHA`), and draw only Dear ImGui (`F5`/`F7`) inside `presentCallback`.
2. **Option B (Exact 3-Layer D3D12 `presentCallback` — highest color fidelity):**
   * Bridge **all three** native UI textures (`hdrLayerTex_`, `ldrLayerTex_`, `multLayerTex_`) as shared D3D12 SRVs (note: `hdrLayerTex_` and `multLayerTex_` only change on HUD-draw / multiply frames) and execute an exact D3D12 HLSL port of `kUiLayerCompositePsHlsl` (`srgbToLinear` $\cdot T \cdot M_{rgb}$ per-channel + `linearToSrgb` + `uiRgb`) inside `edpeFfxPresentCallback`:

```cpp
// Conceptual callback executed by FidelityFX for BOTH real and generated frames (Option B):
ffxReturnCode_t edpeFfxPresentCallback(ffxCallbackDescFrameGenerationPresent* params, void* userCtx) {
    auto* fg = static_cast<AmdFsrFrameGenBackend*>(userCtx);
    // params->commandList: ID3D12GraphicsCommandList*
    // params->currentBackBuffer: frame to present (real OR interpolated)
    // params->outputSwapChainBuffer: final swapchain RTV target
    // params->isGeneratedFrame: distinguishes interpolated vs real frame!
    //
    // 1. Composite ALL THREE shared EDPE UI layers (ldrLayer + multLayer + hdrLayer.a transmittance)
    //    via an exact D3D12 HLSL port of kUiLayerCompositePsHlsl onto outputSwapChainBuffer.
    // 2. If Dear ImGui overlay (F5 / F7 Perf HUD) is visible, draw ImGui on top (DX12 backend)
    //    ONLY here so ImGui itself is crisp and never ghosted or fed into optical flow!
    return FFX_API_RETURN_OK;
}
```
* **Why `presentCallback` (+ Option A/B) wins for EDPE:**
  1. Preserves EDPE's backdrop darkening (`multLayerTex_`), linear transmittance, and tone-mapped UI fidelity on **both real and interpolated frames**.
  2. Keeps EDPE's Dear ImGui (`F5` menu / `F7` FPS graph) out of `HUDLessColor` so UI text never distorts optical flow vectors.
  3. Provides `params->isGeneratedFrame` directly for accurate FG diagnostics (`renderedFPS` vs `presentedFPS` vs `generatedFrames`, `AGENTS.md`).

---

## 4. Mandatory AMD Anti-Lag 2 Coupling (`SDK-DOCUMENTED`)

Per `GPUOpen-LibrariesAndSDKs/AntiLag2-SDK` README and FSR SDK 2.4 (`SDK-DOCUMENTED`):
* Without Anti-Lag 2, Frame Generation queues extra buffered frames, spiking flight-stick/mouse latency.
* When AMD FSR FG is enabled alongside Anti-Lag 2 (Phase 17B):
  1. Call `AMD::AntiLag2DX12::MarkEndOfFrameRendering(&al2Dx12Ctx)` right after game rendering finishes / before FG Present.
  2. **FSR SDK 2.4 Frame-Type Ownership:** Starting with FSR 3.1.1+ / FSR SDK 2.4.0, FidelityFX's `FrameInterpolationSwapChain` **automatically calls `SetFrameGenFrameType` internally** once the Anti-Lag 2 context is associated with the swapchain via DXGI private data / swapchain configuration (`GPUOpen-LibrariesAndSDKs/AntiLag2-SDK`). Do **not** manually invoke `AMD::AntiLag2DX12::SetFrameGenFrameType` simultaneously with FSR 2.4's internal handler!
  3. **FPS Cap Caveat (`AntiLag2-SDK#3`):** `maxFPS` in `AMD::AntiLag2DX12::Update` is ignored while FSR FG is active; use swapchain/external pacing if capping FG output rate.
* **Cross-vendor note:** On NVIDIA GPUs running FSR FG (e.g., RTX 3080), Anti-Lag 2 will not initialize (`amdxc64.dll` absent); EDPE's `ILatencyBackend` automatically pairs **NVIDIA NVAPI Reflex + AMD FSR FG** (`Streamline` is not loaded in this mode, so standalone NVAPI Reflex `NvAPI_D3D_SetLatencyMarker` + `Sleep` is safe).

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
