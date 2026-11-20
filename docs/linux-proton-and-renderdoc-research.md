# Linux (Wine / Proton) & Simultaneous RenderDoc Research (Current D3D11 Debugging + Phases 13–15)

**Date:** 2026-11-20  
**Scope:** Wine/Proton compatibility (DXVK, vkd3d-proton, dxvk-nvapi) and simultaneous RenderDoc capture/debugging for both current D3D11 issues (Pass 53 / reconstruction) and Phases 13–15 (D3D11↔D3D12 interop, D3D12 proxy swapchain, AMD FSR FG, NVIDIA Streamline DLSS-G).  
**Baseline:** EDPE `feat/phases-13-15-frame-generation` (`bef3669`, built on `master` with `9-Add-Linux-Support` and `feat/renderdoc-support` merged).

---

## 1. Evidence Classification (`PLAN.md` §50)

* **`VERIFIED`**: Directly inspected in EDPE source files (`src/`) or git history (`9-Add-Linux-Support`, `feat/renderdoc-support`).
* **`SDK-DOCUMENTED`**: Documented behavior of DXVK, vkd3d-proton, Wine/Proton, RenderDoc (`baldurk/renderdoc`), FidelityFX SDK, and NVIDIA Streamline.
* **`EXPERIMENTAL`**: Proposed EDPE fixes and test/verification steps requiring live Linux/Proton + RenderDoc validation.

---

## 2. Current Baseline Audit (`VERIFIED`)

### 2.1 Wine / Proton support already in `master`
* `src/config.h`: `isRunningUnderWine()` checks `GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr` and defaults `Input.WrapDirectInputKeyboard` to `0` under Wine so remapped/virtual keys work reliably.
* `src/context_census.cpp`:
  * Pass 53 scans PS slots `t0..t7` (`9341019`) and accepts post-tonemap backbuffer blits when an intermediate pass runs (`272d65c`).
  * `allowBackbufferWrite` (`10976`) preserves backbuffer UI when Pass 53 is missed under Proton/Wine (`stage=2` Present fallback, `2e1919d`).
  * Built-in FXAA/MLAA/SMAA passes are blocked during reconstruction (`0fa9498`).
* `src/system_dll.h`: `LoadSystemDll` loads `GetSystemDirectoryW` + `LOAD_LIBRARY_SEARCH_SYSTEM32` (`C:\windows\system32\...`), which under Wine/Proton with `WINEDLLOVERRIDES="dxgi=n,b"` resolves to DXVK's `dxgi.dll` and `d3d11.dll` rather than looping back into EDPE's local `dxgi.dll`.

### 2.2 RenderDoc support already in `master`
* `src/renderdoc_support.h` / `.cpp`:
  * Probes `GetModuleHandleW(L"renderdoc.dll")` and binds `RENDERDOC_GetAPI` (`1.0.0`–`1.7.0`).
  * Emits `ID3DUserDefinedAnnotation` events (`EDPE: HudBloom`, `ColorPrep`, `DepthCopy`, `MotionPass`, `UpscaleEval`, `UiComposite`, `ImGuiOverlay`) and `WKPDID_D3DDebugObjectName` labels when `areAnnotationsActive()` is true.
  * Automatically attaches `EDPE` metadata comments (`version`, `upscaler`, `mode`, `render/output` dimensions) to newly captured `.rdc` files via `SetCaptureFileComments`.
* `src/dxgi_proxy.cpp` & `src/d3d11_proxy.cpp`:
  * `IsAddressInRenderDocModule(_ReturnAddress())` + `ResolvePeExportRaw` prevents EDPE from wrapping RenderDoc's own internal `CreateDXGIFactory*` and `D3D11CreateDevice*` calls.
  * `installD3D11Hooks()` skips IAT patching inside `renderdoc.dll` and `renderdocshim64.dll`.
* `src/dxgi_observer.cpp`:
  * `EnsureRenderDocFactoryHooked()` hooks RenderDoc's `WrappedIDXGIFactory` slots `10` (`CreateSwapChain`) and `15` (`CreateSwapChainForHwnd`) so `observeSwapChain` attaches to RenderDoc's wrapped swapchain.
* `src/ngx_context.cpp`:
  * `NVSDK_NGX_D3D11_Init` explicitly logs when blocked under `renderdoc.dll` (`"use FSR or TAA while capturing"`).

---

## 3. Wine / Proton Findings & Gaps (Current D3D11 + Phases 13–15)

### 3.1 Proxy Loading & DLL Resolution (`VERIFIED` + `SDK-DOCUMENTED`)
* **D3D11 / DXGI (`WINEDLLOVERRIDES="dxgi=n,b"`):**
  * `n,b` tells Wine to load EDPE's local `dxgi.dll` (`native`) next to `EliteDangerous64.exe`, while `LoadSystemDll(L"dxgi.dll")` (`LOAD_LIBRARY_SEARCH_SYSTEM32`) resolves DXVK's `dxgi.dll` in `system32`.
  * **Important gotcha (`SDK-DOCUMENTED`):** If `WINEDLLOVERRIDES` sets `dxgi=n` *without* `,b` or if Proton uses builtin/DXVK symlink layout where `system32\dxgi.dll` is treated as `builtin` in Wine's loader policy, `LoadSystemDll(L"dxgi.dll")` succeeds only when `system32` contains DXVK's PE/builtin DLL. `WINEDLLOVERRIDES="dxgi=n,b"` is mandatory.
* **D3D12 Bridge Gap (`VERIFIED` in `src/d3d12_bridge.cpp:53`):**
  * `D3D11D3D12Bridge::initialize()` currently calls `LoadLibraryW(L"d3d12.dll")` instead of `LoadSystemDll(L"d3d12.dll")`.
  * Under Wine/Proton or Windows mod setups (ReShade/Special K/chaining), `LoadLibraryW(L"d3d12.dll")` searches the game directory first. Switching to `LoadSystemDll(L"d3d12.dll")` guarantees `system32\d3d12.dll` (`vkd3d-proton` on Proton, Windows `d3d12.dll` on Windows) is loaded cleanly.

### 3.2 D3D11 ↔ D3D12 Shared Handles & Fences under DXVK + vkd3d-proton (`SDK-DOCUMENTED`)
* **Mechanism:**
  * DXVK implements `ID3D11Device5::OpenSharedFence` via Vulkan timeline semaphores (`VK_KHR_external_semaphore_win32`) and `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` via `VK_KHR_external_memory_win32`.
  * `vkd3d-proton` implements `D3D12_FENCE_FLAG_SHARED`, `D3D12_HEAP_FLAG_SHARED`, and `ID3D12Device::OpenSharedHandle`, sharing DXGI infrastructure with DXVK (`dxvk` 2.1+ / `vkd3d-proton`).
* **Failure modes to handle in `D3D11D3D12Bridge` (`EXPERIMENTAL`):**
  1. **Older Wine / missing host extensions:** Non-Proton Wine or drivers lacking Win32 external memory/semaphore support fail `CreateSharedHandle` / `OpenSharedFence` / `OpenSharedHandle` with `E_NOTIMPL`, `E_INVALIDARG`, or `DXGI_ERROR_UNSUPPORTED` (`"Shared fences not supported by Vulkan host"`). `D3D11D3D12Bridge` must fail open cleanly and log `wine_get_version()` + failing stage.
  2. **Allocation direction (`D3D11 -> D3D12` vs `D3D12 -> D3D11`):** Currently Phase 13A allocates on D3D11 (`CreateTexture2D(SHARED_NTHANDLE)`) and opens on D3D12 (`OpenSharedHandle`). Under DXVK + vkd3d-proton, both directions should be tested in `d3d11_d3d12_bridge_test.exe` under Proton; if DXVK `CreateSharedHandle` has format/flag quirks (e.g., `D3D11_BIND_UNORDERED_ACCESS` on `R32_FLOAT`), allocating the shared heap/resource on D3D12 (`CreateCommittedResource(..., D3D12_HEAP_FLAG_SHARED)`) and opening via `ID3D11Device1::OpenSharedResource1` is an equivalent zero-copy fallback.
  3. **D3D11 `Flush()` & async query behavior under DXVK:** DXVK batches command submissions until `Flush()` or threshold; `ID3D11DeviceContext4::Signal()` triggers command submission, and `D3D11_ASYNC_GETDATA_DONOTFLUSH` behaves cleanly across `Flush()` boundaries.

### 3.3 Phases 13B, 14, and 15 under Wine / Proton (`SDK-DOCUMENTED`)
* **Phase 13B (`ProxySwapChainD3D12`):**
  * Creating a hidden 1×1 dummy `HWND` for D3D11 while binding `IDXGISwapChain4` (`DXGI_SWAP_EFFECT_FLIP_DISCARD`, `ALLOW_TEARING`) to the real game `HWND` must use `WS_POPUP | WS_DISABLED` (`ShowWindow(SW_HIDE)`, non-activating) so Wine/X11/Wayland window managers do not steal focus or misroute fullscreen/resize events.
* **Phase 14 (AMD FSR 3.1 / 4 Frame Generation):**
  * Supported by `vkd3d-proton` (2.13+ for FSR 3.x FG; `vkd3d-proton` 3.0+ adds `VK_KHR_cooperative_matrix` / `VK_KHR_shader_float8` for FSR 4 on RDNA4).
  * Note `PROTON_LOG=1` / `VKD3D_DEBUG=warn` are essential when testing `FrameInterpolationSwapChain` WSI interactions on Linux.
* **Phase 15 (NVIDIA Streamline `sl.dlss_g`):**
  * Requires `PROTON_ENABLE_NVAPI=1` (`dxvk-nvapi`), `vkd3d-proton`, and Proton Experimental / custom Proton HAGS support (`WINEHAGS=1` where applicable) plus Optical Flow support in the Linux NVIDIA driver.
  * **Signature verification hazard (`EXPERIMENTAL`):** `sl::security::verifyEmbeddedSignature()` relies on `WinVerifyTrust`, which can fail in minimal Wine prefixes missing root CA certificates. Verify early in a standalone Streamline probe under Proton and fail open to `Off` if `slInit` or signature verification fails.

---

## 4. Simultaneous RenderDoc Support: Critical Gaps & Debugging Workflows

### 4.1 Gap 1 — RenderDoc under Linux/Proton (`ENABLE_VULKAN_RENDERDOC_CAPTURE=1`) is invisible to `RenderDocBridge` (`VERIFIED`)
* **Why:** Under Proton/Wine, Windows `renderdoc.dll` injection does **not** capture DXVK/vkd3d-proton's Vulkan internals reliably (and is generally unsupported across DXVK). Instead, Linux RenderDoc attaches as a **host Vulkan layer** (`librenderdoc.so`) via:
  ```bash
  ENABLE_VULKAN_RENDERDOC_CAPTURE=1 DXVK_DEBUG=markers %command%
  ```
* **Impact on current EDPE debugging (`VERIFIED` in `src/renderdoc_support.cpp:71,111`):**
  1. `GetModuleHandleW(L"renderdoc.dll")` returns `nullptr` when using `ENABLE_VULKAN_RENDERDOC_CAPTURE=1`.
  2. Consequently `RenderDocBridge::isAvailable()` and `areAnnotationsActive()` return `false`.
  3. **All** EDPE `ScopedRenderDocEvent` markers (`EDPE: ColorPrep`, `DepthCopy`, `MotionPass`, `UpscaleEval`, `UiComposite`, `HudBloom`, `ImGuiOverlay`) and `SetD3D11DebugName` labels are **skipped**!
  4. Furthermore, even if `ID3DUserDefinedAnnotation` is called, **DXVK stubs it out by default** unless `DXVK_DEBUG=markers` (or `dxvk.debug = markers` in `dxvk.conf`) is set — only then does DXVK translate `BeginEvent`/`EndEvent`/`SetMarker` to `VK_EXT_debug_utils` (`vkCmdBeginDebugUtilsLabelEXT`), making EDPE passes visible in Linux `qrenderdoc`.
* **Recommended fix (`EXPERIMENTAL`):**
  * Add an environment-based annotation/probe check in `RenderDocBridge` (or `Config`): when `isRunningUnderWine()` and (`ENABLE_VULKAN_RENDERDOC_CAPTURE` is non-empty/`1`, OR `DXVK_DEBUG` contains `markers`, OR a `[RenderDoc] ForceAnnotations=1` INI override is set), allow `areAnnotationsActive()` to return `true` so EDPE emits `ID3DUserDefinedAnnotation` + `WKPDID_D3DDebugObjectName` even without `renderdoc.dll`!
  * Clearly separate `hasInAppApi()` (`renderdoc.dll` loaded -> `TriggerCapture`, `SetCaptureFileComments`, `MaskOverlayBits`) from `areAnnotationsActive()` (annotations emitted to D3D11/D3D12/DXVK/vkd3d-proton).

### 4.2 Gap 2 — Debugging Current Issue (Pass 53 / Reconstruction) with Simultaneous RenderDoc (`VERIFIED`)
* **On Windows (Native D3D11):**
  1. Launch `EliteDangerous64.exe` from RenderDoc (`renderdoc.dll` injected).
  2. **Do not select NVIDIA DLSS/DLAA** while capturing: `NVSDK_NGX_D3D11_Init` is blocked under `renderdoc.dll` (`src/ngx_context.cpp:179`). Select **AMD FSR** or **Native TAA** (`Upscaler.Backend=2` or `3`) so `UpscaleEval`, `Pass 53`, and `UiComposite` all execute inside the captured frame!
  3. Keep `RenderDoc.EnableAnnotations=1` and `Overlay.DEV=1` in `edpe.ini`.
* **On Linux (Wine / Proton + DXVK):**
  1. Launch with:
     ```bash
     WINEDLLOVERRIDES="dxgi=n,b" ENABLE_VULKAN_RENDERDOC_CAPTURE=1 DXVK_DEBUG=markers %command%
     ```
  2. Note that under Proton Vulkan capture, RenderDoc sits *below* DXVK (so NGX/FSR run in Vulkan — FSR 3.1.2 and Native TAA work cleanly during Vulkan capture; check `.rdc` output in `/tmp/RenderDoc/`).
  3. Trigger capture via `F12` (or `PrintScreen`). If manual hotkey triggers a crash or timing race, use `VKD3D_AUTO_CAPTURE_COUNTS` (for D3D12) or `renderdoccmd` inside `PRESSURE_VESSEL_SHELL=instead`.
  4. Remember `DXVK_ASYNC=0` (or disabling GPL/async shader hacks if needed) prevents missing pipelines during capture.

### 4.3 Gap 3 — Simultaneous RenderDoc with Phases 13–15 (`D3D11 ↔ D3D12` Interop) (`SDK-DOCUMENTED` + `VERIFIED`)
* **Windows `renderdoc.dll` Limitation (`SDK-DOCUMENTED` in `baldurk/renderdoc`):**
  * RenderDoc cannot track mid-frame GPU writes across `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` + `ID3D12Device::OpenSharedHandle` + `ID3D11Device5::OpenSharedFence`. Replaying a Windows `.rdc` capture with live cross-API shared handles typically produces **blank/uninitialized textures** or crashes in `WrappedID3D12Device` / `WrappedID3D11Device`.
  * **Required EDPE Guardrails for Phase 13B–15:**
    1. **Fail-open under Windows `renderdoc.dll`:** When `GetModuleHandleW(L"renderdoc.dll") != nullptr`, either disable D3D12 swapchain proxying / FG cleanly with an explicit log + ImGui status (`"Blocked under RenderDoc (cross-API shared handles unsupported)"`), **or** add an explicit debug-only staging-copy fallback (never used in hot path) when inspecting isolated D3D12 compute/UI shaders.
    2. **D3D12 RenderDoc Hygiene in `src/d3d12_bridge.cpp`:**
       * Switch `LoadLibraryW(L"d3d12.dll")` to `LoadSystemDll(L"d3d12.dll")`.
       * Add `SetD3D12DebugName(ID3D12Object*, const char*)` (`WKPDID_D3DDebugObjectName` / `SetName`) and `ScopedD3D12RenderDocEvent` (`ID3D12GraphicsCommandList::BeginEvent` / `EndEvent` with `PIX_EVENT_UNICODE_VERSION = 0` or WinPixEventRuntime-compatible encoding) so D3D12 work is labeled in both Windows RenderDoc (standalone D3D12 harness) and Linux `vkd3d-proton` (`VK_EXT_debug_utils`).
       * When Phase 13B hooks `IDXGIFactory` / `CreateSwapChainForHwnd` for D3D12, ensure `IsAddressInRenderDocModule(_ReturnAddress())` is checked on any `D3D12CreateDevice` / `CreateSwapChain` path so RenderDoc's internal probe devices are never intercepted.
* **Linux Proton (`ENABLE_VULKAN_RENDERDOC_CAPTURE=1`):**
  * Because both DXVK (D3D11) and `vkd3d-proton` (D3D12) submit to the **same Vulkan driver** inside the process, `librenderdoc.so` captures the unified Vulkan command stream (`VK_EXT_debug_utils` labels from both `DXVK_DEBUG=markers` and `vkd3d-proton`).
  * However, `VK_KHR_external_memory_win32` / `VK_KHR_external_semaphore_win32` imports inside a single-process Vulkan capture can still confuse Vulkan replay if RenderDoc treats imported memory as opaque external memory; test `d3d11_d3d12_bridge_test.exe` under Wine + `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` before relying on it for FG frame captures.

---

## 5. Actionable Recommendations (Next Steps)

1. **Immediate (for debugging current Pass 53 / Linux issue with RenderDoc):**
   * Add `ForceAnnotations` / `ENABLE_VULKAN_RENDERDOC_CAPTURE` + `DXVK_DEBUG=markers` detection in `RenderDocBridge` so EDPE emits `ID3DUserDefinedAnnotation` events (`EDPE: ColorPrep`, `DepthCopy`, `MotionPass`, `UpscaleEval`, `UiComposite`, `HudBloom`) under Proton/DXVK even when `renderdoc.dll` is absent.
   * Keep using **AMD FSR** or **Native TAA** when capturing on Windows RenderDoc (`NGX` is blocked by driver under `renderdoc.dll`).
2. **Phase 13A/13B Hardening:**
   * Update `src/d3d12_bridge.cpp` to use `LoadSystemDll(L"d3d12.dll")` instead of `LoadLibraryW(L"d3d12.dll")`.
   * Add `SetD3D12DebugName` + `ID3D12GraphicsCommandList::BeginEvent/EndEvent` helpers gated on `areAnnotationsActive()`.
   * Log `wine_get_version()` if `D3D11D3D12Bridge::initialize` or `createSharedTexture` fails under Wine/Proton, and test `d3d11_d3d12_bridge_test.exe` under Proton (validating DXVK ↔ vkd3d-proton `OpenSharedFence` / `OpenSharedHandle` and testing `D3D12 -> D3D11` allocation direction if needed).
   * In Phase 13B (`ProxySwapChainD3D12`), bypass D3D12 swapchain takeover / FG when Windows `renderdoc.dll` is attached (fail-open to D3D11 swapchain so D3D11 RenderDoc capture remains functional).
