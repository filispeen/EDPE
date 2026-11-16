# Phase 16 Research: Intel XeSS Upscaling (Native AA & Super Resolution)

**Date:** 2026-11-14  
**Scope:** Phase 16 ([PLAN.md](../PLAN.md) §§4, 13, 19–22, 43–45; `AGENTS.md` Intel references)  
**Prerequisites:** Phase 7 (`TemporalFrameInputs`, `IUpscalerBackend`); **Phase 13A** (Shared-Handle D3D11↔D3D12 Compute Bridge for non-Intel GPUs)  
**Status:** RESEARCH COMPLETE — Ready for dual-path implementation (`libxess_dx11.dll` + `libxess.dll`).

---

## 1. Authoritative SDK Discovery: Dual-DLL Architecture (`SDK-DOCUMENTED`)

Official repository: **`intel/xess`** (`inc/xess/xess.h`, `inc/xess/xess_d3d11.h`, `inc/xess/xess_d3d12.h`, **MIT headers**, proprietary prebuilt runtime DLLs).

A critical architectural fact verified directly in `xess_d3d11.h` vs. `xess_d3d12.h` governs EDPE's Phase 16 design:

| Runtime Binary | Header / API Family | GPU Vendor Support | Acceleration Kernel | D3D12 Interop Needed? |
|:---|:---|:---|:---|:---:|
| **`libxess_dx11.dll`** | `xess_d3d11.h` (`xessD3D11CreateContext`, `xessD3D11Init`, `xessD3D11Execute`) | **Intel Arc only** (`XESS_RESULT_ERROR_UNSUPPORTED_DEVICE` on NVIDIA/AMD!) | Native Intel DX11 / XMX path | **No** (native D3D11) |
| **`libxess.dll`** | `xess_d3d12.h` (`xessD3D12CreateContext`, `xessD3D12BuildPipelines`, `xessD3D12Init`, `xessD3D12Execute`) | **All DX12 GPUs** (Intel Arc XMX, Iris Xe DP4a, **NVIDIA RTX/GTX DP4a**, **AMD RDNA/GCN DP4a**) | XMX on Arc; SM 6.4 DP4a (`ID3D12Device`) everywhere else | **Yes** (Phase 13A NT-handle bridge) |

> **`VERIFIED` Engineering Consequence (`REFERENCE-CODE` in `OptiScaler` & `EDAA`):**
> * `OptiScaler` maintains two completely separate classes: `XeSSFeature_Dx11.cpp` (`libxess_dx11.dll` for Intel Arc native) and `XeSSFeature_Dx11on12.cpp` (`libxess.dll` via D3D11↔D3D12 interop for cross-vendor GPUs).
> * `Siegfried-Origin/EDAA` (`addon/src/xess.cpp`, **MIT License**) proves in live Elite Dangerous gameplay that **Phase 13A's `D3D11 ↔ D3D12` shared-handle compute bridge works with `libxess.dll`** on NVIDIA, AMD, and Intel GPUs without touching DXGI swapchain ownership.
> * Therefore, **EDPE should support `libxess_dx11.dll` as an optional fast path when `DXGI_ADAPTER_DESC1::VendorId == 0x8086` (Intel Arc), and `libxess.dll` over Phase 13A `D3D11D3D12ComputeBridge` as the universal cross-vendor XeSS backend.**

---

## 2. Existing Production Reference in Elite Dangerous (`REFERENCE-CODE`)

### 2.1 `Siegfried-Origin/EDAA` (`addon/src/xess.cpp`, MIT License)
Inspecting `EDAA`'s Elite Dangerous implementation confirms every practical detail required for EDPE's Phase 16B D3D12-bridged path:
1. **Dynamic Loading & Fail-Open:** Delay-loads `libxess.dll` via `LoadLibraryW` (never static `.lib` import linkage that could prevent `dxgi.dll` from loading when `libxess.dll` is absent!).
2. **D3D12 Device Initialization (`xess_d3d12.h`):**
   * `xessD3D12CreateContext(d3d12Device, &hContext)`
   * `xessD3D12GetInitParams` / **`xessD3D12BuildPipelines(hContext, pipelineLibrary, blocking, initFlags)`**: compiles XeSS neural-network PSOs up front; **must run asynchronously (`blocking = false` on a worker thread or deferred background pass)** so Elite does not stutter 1–3 seconds at cockpit spawn.
   * Ring of **8 `ID3D12CommandAllocator` instances** tied to monotonic `ID3D12Fence` values so in-flight XeSS dispatches never overwrite active command lists.
3. **Zero-Copy Texture Sharing:**
   * Color, Depth (`R32_FLOAT` copy extracted from `DSV #2` specifically for WDDM D3D11↔D3D12 `SHARED_NTHANDLE` sharing), Motion Vectors (`R16G16_FLOAT`), Output, and Responsive Mask (`R8_UNORM`) are created on `ID3D11Device` with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED`, exported via `IDXGIResource1::CreateSharedHandle`, and opened once on `ID3D12Device::OpenSharedHandle`.
   * Synchronized via `ID3D12Device::CreateFence(..., D3D12_FENCE_FLAG_SHARED)` → `ID3D11Device5::OpenSharedFence`, bracketing execution with `ID3D11DeviceContext4::Signal` → D3D12 `Wait` → `xessD3D12Execute` → D3D12 `Signal` → `ID3D11DeviceContext4::Wait`.
4. **Native D3D11 Path (`libxess_dx11.dll`, `xess_d3d11.h`):**
   * On Intel Arc (`xessD3D11CreateContext(d3d11Device, &hContext)`), XeSS requires **no** external descriptor heaps, **no** `BuildPipelines` step, and **no** D3D12 fence synchronization — only `xessD3D11Init(hContext, &d3d11InitParams)` and `xessD3D11Execute(hContext, &d3d11ExecParams)`.
   * Per *Intel XeSS-SR Developer Guide 2.0* (*Resource Formats*), native D3D11 (`libxess_dx11.dll`) directly accepts `DXGI_FORMAT_R32G8X24_TYPELESS` / `DXGI_FORMAT_D32_FLOAT_S8X24_UINT` (`DSV #2`), interpreting it internally as `R32_FLOAT_X8X24_TYPELESS` with **no** intermediate `R32_FLOAT` depth copy needed on Intel Arc!

---

## 3. Exact SDK Structures & EDPE Convention Mapping (`SDK-DOCUMENTED`)

### 3.1 Quality Settings, Legacy Scale Alignment & `<SSAAMultiplier>` (`inc/xess/xess.h`)
| XeSS Enum | Value | Default XeSS 1.3+/2.0 Scale | With `xessForceLegacyScaleFactors(true)` | EDPE `UpscaleMode` ([`src/temporal_inputs.h`](../src/temporal_inputs.h#L13-L28)) & `<SSAAMultiplier>` ([`src/config.h`](../src/config.h#L206-L275)) |
|:---|---:|---:|---:|:---|
| `XESS_QUALITY_SETTING_ULTRA_PERFORMANCE` | `100` | `3.0x` | **`3.0x`** | `UpscaleMode::UltraPerformance` (`0.333333`) |
| `XESS_QUALITY_SETTING_PERFORMANCE` | `101` | `2.3x` | **`2.0x`** | `UpscaleMode::Performance` (`0.500000`) |
| `XESS_QUALITY_SETTING_BALANCED` | `102` | `2.0x` | **`1.7x`** | `UpscaleMode::Balanced` (`0.580000`) |
| `XESS_QUALITY_SETTING_QUALITY` | `103` | `1.7x` | **`1.5x`** | `UpscaleMode::Quality` (`0.666667`) |
| `XESS_QUALITY_SETTING_ULTRA_QUALITY` | `104` | `1.5x` | `1.3x` | *(Optional fallback mapping if legacy flags omitted)* |
| `XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS` | `105` | `1.3x` | `1.3x` | *(Unused unless sub-tier exposed)* |
| `XESS_QUALITY_SETTING_AA` | `106` | `1.0x` | **`1.0x`** | `UpscaleMode::NativeAA` (`1.000000`) |

> **`SDK-DOCUMENTED` Preset Scale Conflict Fix (`xessForceLegacyScaleFactors`):**
> Starting with XeSS 1.3+, Intel shifted preset scaling upward (`Quality=1.7x`, `Balanced=2.0x`, `Performance=2.3x`), which conflicts with EDPE's [`getTargetSsaaMultiplier`](../src/config.h#L206-L215) (`1.5x / 1.7x / 2.0x / 3.0x`). Calling **`xessForceLegacyScaleFactors(xessContext_, true)`** immediately after `xessD3D11CreateContext` / `xessD3D12CreateContext` restores exact `1.5x / 1.7x / 2.0x / 3.0x` alignment with Cobra's `<SSAAMultiplier>` and guarantees `xessGetOptimalInputResolution()` matches Cobra's actual render buffer dimensions!
> ```cpp
> xess_result_t status = xessD3D12CreateContext(d3d12Device, &xessContext_);
> if (status == XESS_RESULT_SUCCESS) {
>     // Align XeSS 1.3+/2.0 presets with EDPE's <SSAAMultiplier> grid (1.5x / 1.7x / 2.0x / 3.0x):
>     xessForceLegacyScaleFactors(xessContext_, true);
> }
> ```

### 3.2 Critical Halton Jitter Phase-Count Discovery (`SDK-DOCUMENTED` vs. EDPE `src/config.h`)
The official Intel XeSS Programming Guide mandates:
$$\text{jitterPhaseCount} \ge \left\lceil 8 \times \left(\frac{\text{outputWidth}}{\text{renderWidth}}\right)^2 \right\rceil$$

| Mode / Scale Factor (with Legacy Grid) | Min Halton Phases Required by XeSS | EDPE `v0.7.0` Default `jitterPhaseCount` ([`src/config.h#L298`](../src/config.h#L298)) |
|:---|:---:|:---:|
| `NativeAA` (`1.0x`) | **8** | `32` (OK) |
| `Quality` (`1.5x`) | **18** | `32` (OK) |
| `Balanced` (`1.7x`) | **24** | `32` (OK) |
| `Performance` (`2.0x`; `2.3x` if non-legacy) | **32** (`43` at `2.3x`) | `32` (OK with `xessForceLegacyScaleFactors(true)`) |
| `Ultra Performance` (`3.0x`) | **72** | `32` (**Undersampled — causes shimmering/Moire!**) |

> **Required EDPE Fix:** Add automatic Halton period sizing in `TemporalInputBuilder` / `Config` whenever `jitterPhaseCount` is in Auto mode (`0` or below the mode minimum): `std::max(32u, static_cast<unsigned>(std::ceil(8.0f * scale * scale)))` (yielding `72` for Ultra Performance `3.0x`, while keeping `Quality/Balanced/Performance` at `32`). Verify Halton `(2,3)` period extension does not disrupt `NvidiaUpscalerBackend` or `AmdUpscalerBackend`.

### 3.3 `translateToIntel()` Convention Mapping (`TemporalFrameInputs` $\rightarrow$ `xess.h`)
All flags verified directly in `inc/xess/xess.h` (`_xess_init_flags_t`):

| `TemporalFrameInputs` Property | XeSS Parameter / Init Flag | Exact Value & Rationale |
|:---|:---|:---|
| `depthInverted = true` | `XESS_INIT_FLAG_INVERTED_DEPTH` | **Enabled.** Cobra `DSV #2` is reversed-Z ($B=0.025$, far clear `0.0`). On `libxess_dx11.dll`, pass `DSV #2` (`R32G8X24_TYPELESS`) natively; on `libxess.dll` (D3D12 bridge), pass `R32_FLOAT` copy. |
| `motionIsLowRes = true` | `XESS_INIT_FLAG_HIGH_RES_MV` | **Unset (`0`).** EDPE `MotionPass` emits render-resolution MVs. |
| `motionVectors` (`R16G16_FLOAT`) | `XESS_INIT_FLAG_USE_NDC_VELOCITY` | **Unset (`0`).** EDPE writes **render-pixel displacement**, not NDC velocity. |
| MV units & sign (`current -> prev`) | `xessSetVelocityScale(hContext, x, y)` *(init)* or `inputWidth / outputWidth` check | Per `xess.h`, pixel-space velocities (`HIGH_RES_MV` unset) expect **current-to-previous pixel displacement** (`+x` right, `+y` down in texture UV space). Must be validated deterministically via `motion_gpu_test` & cockpit yaw/pitch test (`AGENTS.md`), configurable via `motionScaleX/Y`. |
| `motionJittered = false` | `XESS_INIT_FLAG_JITTERED_MV` | **Unset (`0`).** EDPE motion vectors already exclude Halton jitter. |
| `colorIsHdr = false` (or `true` when linear HDR fed) | `XESS_INIT_FLAG_LDR_INPUT_COLOR` vs. `XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE` | When `colorIsHdr == false` (LDR tonemapped `scene_ldr_source`), set **`XESS_INIT_FLAG_LDR_INPUT_COLOR`**. When linear `s_clean_hdr_scene_texture` (`R11G11B10_FLOAT`) is fed (`colorIsHdr == true`), omit `LDR_INPUT_COLOR` and set **`XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE`** (no need for `EXPOSURE_SCALE_TEXTURE` initially). |
| `(jitterX, jitterY)` | `execParams.jitterOffsetX`, `execParams.jitterOffsetY` | In **render pixels**, `[-0.5, 0.5]`. Validate X/Y sign empirically via EDPE `F8` Jitter Debug View (same convention as FSR/DLSS `translateToNvidia` / `translateToAmd`). |
| `exposure` (`nullptr` initially) | `XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE` | Optional if `ENABLE_AUTOEXPOSURE` is used. |
| Reactive / Responsive Mask (`R8_UNORM`) | `XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK` + `pResponsivePixelMaskTexture` | Ideal optional hook for emissive `RTV3` thrusters/lasers/canopy sparks (`docs/render-pipeline-observations.md` lines 107–110), **excluding** native HUD (already separated by `UiLayer`). |
| `inputs.reset` | `execParams.resetHistory = inputs.reset ? 1 : 0` | Triggered on `CameraDelta` jump/hyperjump/station cut. |
| Texture Mip LOD Bias (`src/d3d11_proxy.cpp`) | Negative bias on `PSSetSamplers` / `CreateSamplerState` | XeSS programming guide uses standard $\text{mipBias} = \log_2(\text{renderWidth} / \text{outputWidth}) - 1.0$ (matching EDPE's existing `observedCreateSamplerState` bias in `src/d3d11_proxy.cpp#L99-L106`). |

---

## 4. Integration Architecture in EDPE (`EXPERIMENTAL`)

### 4.1 Extension of `src/temporal_inputs.h` and `src/upscaler_backend.h`
1. Add `IntelXess = 4` to `enum class UpscalerBackend` (`src/temporal_inputs.h#L13-L18`) and align `Config::upscalerBackend` (`src/config.h#L285`).
2. Add `IntelXessInputParams` and `translateToIntel(const TemporalFrameInputs&, IntelXessInputParams*, const wchar_t**)` matching [`translateToNvidia`](../src/temporal_inputs.h#L216-L243) and [`translateToAmd`](../src/temporal_inputs.h#L259-L283).
3. Update `UpscalerManager::kBackendCount` ([`src/upscaler_backend.h#L261`](../src/upscaler_backend.h#L261)) from `4` to `5` (or `6` if Phase 18 adds `NvidiaDlssRR`) and register `IntelUpscalerBackend intelBackend_;` alongside the existing backend members ([`src/upscaler_backend.h#L242-L246`](../src/upscaler_backend.h#L242-L246)).
4. Implement `IntelUpscalerBackend` via PIMPL (`struct Impl; std::unique_ptr<Impl> impl_;`, identical to `AmdUpscalerBackend`) so XeSS headers never leak into non-XeSS translation units:

```cpp
// Conceptual IntelUpscalerBackend dual-path dispatch
struct IntelUpscalerBackend::Impl {
    enum class ExecutionPath { None, NativeDx11Arc, BridgedDx12 };
    ExecutionPath activePath = ExecutionPath::None;

    // Native Intel Arc path (libxess_dx11.dll)
    HMODULE dllDx11 = nullptr;
    xess_context_handle_t ctxDx11 = nullptr;

    // Cross-vendor fallback/DP4a path (libxess.dll via Phase 13A bridge)
    HMODULE dllDx12 = nullptr;
    xess_context_handle_t ctxDx12 = nullptr;
    D3D11D3D12ComputeBridge bridge; // Phase 13A NT-handle + shared-fence bridge
    std::atomic<bool> pipelinesBuilt{false};

    bool detectAndLoad(ID3D11Device* device) {
        // 1. Inspect IDXGIAdapter DXGI_ADAPTER_DESC1::VendorId.
        // 2. If VendorId == 0x8086 (Intel), try LoadLibraryW(L"libxess_dx11.dll") + xessD3D11CreateContext first.
        // 3. Otherwise (NVIDIA/AMD/Arc fallback), try LoadLibraryW(L"libxess.dll") + bridge.initialize(device).
        // 4. If neither DLL loads or context creation fails, populate unavailableReason_ and fail open!
        return activePath != ExecutionPath::None;
    }
};
```

### 4.2 External Descriptor Heap vs. Internal Heap (`xess_d3d12.h`)
* Setting `XESS_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP` is **unnecessary** on Phase 13A's isolated secondary `ID3D12Device` when XeSS is the sole user of that compute allocator; omitting it lets `libxess.dll` manage its own descriptor heap internally (`pDescriptorHeap = nullptr`, `pTempBufferHeap = nullptr`, `pTempTextureHeap = nullptr`), avoiding descriptor fragmentation bugs (`VERIFIED` in `EDAA`).
* Only set `XESS_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP` if benchmarking shows heap allocation churn across resize events; pre-query `xessGetProperties` heap sizes and suballocate from persistent `D3D12_HEAP_TYPE_DEFAULT` heaps if needed.

---

## 5. Testing, Verification & Failure Plan (`EXPERIMENTAL`)

1. **CPU Unit Tests (`tests/temporal_inputs_test.cpp`):**
   * Verify `translateToIntel()` flag matrix (`INVERTED_DEPTH`, `LDR_INPUT_COLOR` vs `ENABLE_AUTOEXPOSURE`, `!HIGH_RES_MV`, `!USE_NDC_VELOCITY`).
   * Verify dynamic Halton `phaseCount` scaling formula ($\ge 8 \cdot s^2 \rightarrow 72$ at 3.0x).
   * Verify graceful `isAvailable() == false` & null-input fail-open when `libxess.dll` / `libxess_dx11.dll` are missing from disk.
2. **Standalone Probe (`tools/xess_probe/`):**
   * Mirror `tools/ngx_probe/`: create D3D11 device + synthetic reversed-Z frame, verify `libxess_dx11.dll` (Intel) or `libxess.dll` via Phase 13A NT handles (NVIDIA/AMD), assert clean `xessDestroyContext` without shutdown hangs.
3. **In-Game Verification Gates:**
   * Verify `Status` panel in `F5` ImGui UI reports `"Intel XeSS (XMX)"` vs `"Intel XeSS (DP4a via D3D12 Bridge)"` vs `"Unavailable: libxess.dll not found"`.
   * Run EDPE's Multi-Backend Comparison Benchmark (`STATUS.md`) across `Native`, `DLSS`, `FSR`, `XeSS`, and `TAA` checking GPU cost (expect ~0.5–1.2 ms for DP4a on RTX at 1440p vs ~0.3 ms for DLSS/XMX) and zero ghosting on cockpit holo-HUD / distant stars.
4. **Licensing Compliance (`AGENTS.md`):**
   * `intel/xess` SDK headers are **MIT** (`inc/xess/*.h`).
   * Do **not** bundle proprietary `libxess.dll` / `libxess_dx11.dll` in Git source or violate Intel's redistributable EULA — follow the exact same user/fetch-script staging model EDPE uses for `nvngx_dlss.dll` (`tools/fetch_*` or user-supplied DLL beside `dxgi.dll`).
