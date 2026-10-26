# EDPE — Elite Dangerous Performance Enhanced

<div align="center">

[![Latest Release](https://img.shields.io/github/v/release/filispeen/EDPE?color=blue&label=version)](https://github.com/filispeen/EDPE/releases/latest)
[![CI](https://github.com/filispeen/EDPE/actions/workflows/CI.yml/badge.svg)](https://github.com/filispeen/EDPE/actions/workflows/CI.yml)
[![Status](https://img.shields.io/badge/status-Hot%20Path%20CPU%20Optimized%20%26%20Input%20Isolated-success.svg)](STATUS.md)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64%20|%20Direct3D%2011-lightgrey.svg)]()
[![Tests](https://img.shields.io/badge/tests-11%2F11%20Passing-brightgreen.svg)]()

</div>

**EDPE (Elite Dangerous Performance Enhanced)** is a standalone graphics enhancement proxy mod targeting the **2D Direct3D 11 renderer of Elite Dangerous** (Cobra engine).

Elite Dangerous natively lacks modern temporal reconstruction technologies in its standard 2D rendering path. EDPE injects temporal anti-aliasing and super-resolution reconstruction into the engine pipeline via non-invasive graphics API observation—without modifying game binaries, game logic, or network code.

---

## Current Status: Milestone `v0.4.8` (Hot-Path CPU Optimization, Input Isolation & Consolidated DXGI Proxy)

As of **October 2026 (`v0.4.8`)**, EDPE has achieved:

* **Consolidated DXGI Proxy (`dxgi.dll`):** Single drop-in proxy library handling DXGI swapchain observation, D3D11 device creation hooks, temporal reconstruction, and runtime overlay management without black screens, crashes, or rendering artifacts.
* **Hot-Path CPU Overhead Elimination:** Eliminated large FPS drops during active temporal reconstruction (TAA, DLAA, DLSS). Constant buffer discovery in `Map()` terminates immediately upon identifying the scene camera; `Unmap()` skips untracked resources without stale buffer checks; HUD draw probes are armed once rather than re-armed each frame.
* **Background NVML Hardware Polling:** GPU utilization and VRAM statistics are polled asynchronously on a dedicated worker thread with a 500 ms cached value, preventing NVML driver stalls on the render thread.
* **Overlay Input Isolation & HOTAS Protection:** Robust input isolation using DirectInput proxying and window message filtering (`WM_INPUT`, cursor clipping, and focus resets), preventing mouse and keyboard interference with flight controls and HOTAS peripherals (addressing Issue #1).
* **Canopy Reflection Sub-Camera Filter:** Depth/luma filtering that isolates and rejects cockpit canopy reflection passes, eliminating station pulsing, jitter artifacts, and edge halos in native TAA and temporal upscalers.
* **Analytical Skybox / Star Motion Vectors:** Rotational motion vectors at infinity for stars and skybox, preventing star erasure, smearing, and corona artifacts during high-speed ship maneuvers.
* **Scene Depth Discovery:** Successfully isolated authoritative reversed-Z scene depth buffer (`DSV #2`, `DXGI_FORMAT_R32G8X24_TYPELESS`, $B=0.025$).
* **Dynamic Camera Matrix Tracking:** Parses the 5376-byte dynamic constant buffer (`CB0`), extracting view, projection, and depth parameters each frame.
* **Projection Jitter Injection:** Generates Halton (2,3) subpixel projection jitter and injects it into dynamic CB0 projection matrix words (1080–1095) with **automatic fail-open stand-down** when reconstruction is disabled.
* **Camera-Depth Motion Vectors:** Real-time GPU motion pass generates per-pixel current→previous screen-space motion vectors (`DXGI_FORMAT_R16G16_FLOAT`).
* **Vendor-Neutral Temporal Input Layer:** Abstracted `TemporalFrameInputs` structure decoupling engine state from vendor reconstruction SDKs.
* **Native D3D11 TAA (Temporal Anti-Aliasing):** Vendor-neutral native temporal anti-aliasing reconstruction pass utilizing Halton (2,3) projection jitter accumulation, camera-depth motion reprojection, and 3x3 YCoCg variance clipping with zero external SDK dependencies.
* **NVIDIA DLAA Evaluation:** Official NVIDIA NGX SDK integrated with EDPE-owned Project ID (`48d353f3-d07b-4048-876b-09f8622f5a27`). DLAA runs on the finished swapchain backbuffer at `Present` prior to overlay rendering, achieving **rock-solid temporal anti-aliasing with ~0.3 ms GPU overhead** and zero subpixel image vibration.
* **NVIDIA DLSS Super Resolution:** Reconstructs true sub-native 3D scenes (via Cobra in-game Supersampling < 1.0x) into native resolution, supporting Quality, Balanced, Performance, and Ultra Performance with DLSS Presets (Auto, J, K, L, M).
* **AMD FSR 3.1.2 Verified In-Game:** Verified in Elite Dangerous Odyssey for both FSR Native AA at 1.0x SSAA and FSR Super Resolution at sub-native render scales (< 1.0x SSAA) via native D3D11 compute path.
* **Real-Time Performance HUD Overlay (`F7` / `F5`):** Built-in heads-up display showing live FPS, game frametime, upscaler frametime, total frametime, active GPU adapter name, GPU usage %, and dedicated VRAM usage (used/total and %). Features scaling slider (0.5x–2.0x), interactive mouse dragging in F5 menu, persistent screen positioning in `edpe.ini`, and complete transparent click-through protection against input leaks.
* **Integrated High-Precision QPC Profiler:** Microsecond-precision breakdown of engine, motion pass, upscaler execution, UI composition, and overhead with toggleable in-game display.
* **Subpixel Jitter Debug View & Log Recorder (`F8` / `F9`):** Real-time on-screen jitter visualization and frame-indexed logging to `jitter_view.log` for tracking subpixel jitter offsets, luma correlation, and frame stability.
* **Automatic Scaling Detection:** Zero-configuration render scaling detection comparing scene depth dimensions against window resolution, automatically choosing DLAA / FSR Native AA / TAA (at 1.0x) or DLSS / FSR Super Resolution (at < 1.0x).
* **Native UI Protection & Composite:** Dedicated shader pass (`UiCompositePass`) preserves native-resolution HUD, text, cockpits, and menus, seamlessly compositing them over reconstructed DLSS/FSR frames.
* **Synchronized 5-Buffer GPU Capture (`F6`):** One-shot capture of all 5 engine buffers (`color`, `motion`, `depth`, `early-color` emissive, `frame`) from the exact same frame using non-blocking asynchronous GPU readback (`D3D11_MAP_FLAG_DO_NOT_WAIT`), saved to `edpe-captures/`.
* **Automated Test Suite:** 11 automated tests passing in CTest covering math, camera deltas, pixel formats, proxy hooks, GPU motion passes, jitter reports, and UI composition.

---

## Roadmap & Technology Matrix

| Phase | Technology / Milestone | Status | Description |
|:---:|:---|:---:|:---|
| **0–2** | **Reconnaissance & Resource Census** | ✅ **Verified** | Identified D3D11 context calls, swapchain layout (2560×1440 R8G8B8A8), MRT bindings, and CB0 lifecycle. |
| **3** | **Scene Depth Extraction** | ✅ **Verified** | DSV #2 reversed-Z depth buffer confirmed ($B=0.025$) with coherent full-frame geometry. |
| **4** | **Camera & Projection Tracking** | ✅ **Verified** | 5376-byte dynamic CB0 parsed; primary 3D camera latched once per frame. |
| **5** | **Projection Jitter Injection** | ✅ **Verified** | Halton (2,3) injected into CB0 with fail-open bypass when upscaler is inactive. |
| **6** | **Motion Vectors Pass** | ✅ **Verified** | Screen-space camera-depth motion vectors rendered to `R16G16_FLOAT`. |
| **7** | **Shared Temporal Input Layer** | ✅ **Verified** | Backend-neutral `TemporalFrameInputs` and `IUpscalerBackend` architecture implemented. |
| **8** | **NVIDIA DLAA** | ✅ **Verified** | Official NGX DLAA evaluated at native resolution; ~0.3 ms GPU time, pristine anti-aliasing. |
| **9–10** | **True Render Scale & NVIDIA DLSS SR** | ✅ **Verified** | Lower 3D scene render resolution via Cobra SSAA (< 1.0x) with full DLSS SR reconstruction and zero-configuration auto-scaling. |
| **11** | **AMD FSR Super Resolution & Native AA** | ✅ **Verified** | Native D3D11 FidelityFX FSR 3.1.2 backend integrated and verified in-game; supports FSR Native AA & FSR Super Resolution with fail-open fallback. |
| **12** | **Native-Resolution HUD Separation** | 🟡 **Partial** (Works with menus; cockpit UI refined) | Native-res HUD composited over upscaled DLSS/FSR 3D scene (`UiCompositePass`), protecting cockpit/UI crispness. |
| **13–15** | **D3D11/D3D12 Interop & Frame Generation** | 🔬 **Research** | NVIDIA DLSS Frame Generation (Streamline) & AMD FSR Frame Generation via interop bridge. |
| **16** | **NVIDIA DLSS-NR (Ray Reconstruction / Neural Denoising)** | 🔬 **Research** | AI-driven Ray Reconstruction / neural denoising replacing hand-tuned temporal filters for reflections and lighting via NGX Ray Reconstruction. |

See [STATUS.md](STATUS.md) for verified facts and [PLAN.md](PLAN.md) for the complete roadmap.

---

## Architecture

EDPE enforces a strict separation: Elite-specific engine reverse-engineering belongs to EDPE; vendor-specific reconstruction logic lives behind modular backend interfaces.

```text
               Elite Dangerous (Cobra Engine D3D11)
                                |
                                v
                      EDPE Proxy Injection
                           (dxgi.dll)
                                |
        +-----------------------+-----------------------+
        |                                               |
        v                                               v
 Context Census & CB0 Tracking                 Swapchain Observer
  - Reverse-Z Depth (DSV #2)                    - Backbuffer tracking
  - Camera matrices (CB0 Map/Unmap)             - First-Present hook
  - Emissive glow pass (RTV3)                   - WndProc message handler
        |                                               |
        v                                               |
 Temporal Input Generation                              |
  - Halton (2,3) projection jitter                      |
  - GPU camera-depth motion vectors                     |
        |                                               |
        +-----------------------+-----------------------+
                                |
                                v
                  Shared TemporalFrameInputs
           (color, depth, motion, jitter, camera)
                                |
        +-----------------------+-----------------------+
        |                       |                       |
        v                       v                       v
  NVIDIA Backend           AMD Backend             Native TAA
(NGX DLAA / DLSS SR)   (FSR Native AA / SR)  (D3D11 Reconstruction)
        |                       |                       |
        +-----------------------+-----------------------+
                                |
                                v
                     Reconstructed 3D Scene
                                |
                                v
                     Dear ImGui Overlay (F5)
                                |
                                v
                       IDXGISwapChain::Present
```

### Core Architectural Principles
1. **API Observation over Binary Patching:** Intercepts D3D11/DXGI COM interfaces and buffer unmaps instead of patching game executable memory.
2. **Fail-Open Design:** If any upscaler or temporal input fails, EDPE automatically stands down, disables projection jitter, restores original rendering, and presents the native frame safely.
3. **No Duplicate Pipelines:** Common temporal inputs (`TemporalFrameInputs`) are calculated once and translated into vendor-specific conventions (NVIDIA/AMD/TAA) at the backend boundary.
4. **Fair Play Guarantee:** Rendering enhancements only. No gameplay automation, no networking hooks, no security bypasses.

---

## In-Game Controls & Hotkeys

| Hotkey | Action | Description |
|:---:|:---|:---|
| **`F5`** | **Toggle EDPE Menu** | Opens/closes the Dear ImGui control panel. When closed, all mouse and keyboard inputs pass freely to the game. |
| **`F6`** | **One-Shot Synchronized Capture** | Captures all 5 rendering buffers (`color`, `motion`, `depth`, `early-color`, `frame`) from the exact same frame to `edpe-captures/`. |
| **`F7`** | **Toggle Performance HUD** | Toggles the real-time telemetry overlay (FPS, frametimes, GPU adapter, GPU usage %, dedicated VRAM). |
| **`F8`** | **Toggle Jitter Debug View** | Toggles the real-time visual overlay for subpixel jitter inspection. |
| **`F9`** | **Record Jitter Diagnostics** | Starts recording subpixel jitter, luma, and camera motion telemetry to `jitter_view.log`. |
| **`Insert`** | **Game Pass-Through** | Configured to pass transparently to the original game window procedure. |

### Dear ImGui Control Panel Features (`F5`)
* **Upscaler Backend Selector:** Switch seamlessly between `Native (Bypass)`, `NVIDIA DLSS / DLAA`, `AMD FSR (Native AA / SR)`, and `Native D3D11 TAA`.
* **Live GPU Reconstruction Time & Profiler:** Asynchronous GPU timestamp queries and optional QPC microsecond profiler detailing exact upscaler overhead (~0.3 ms for DLAA).
* **Jitter Controls & Diagnostics:** Real-time toggle, subpixel vibration test override, phase count cycle slider (default: 32), X/Y inversion, and axis swap.
* **Motion Vector Tuning:** Motion scale slider, X/Y inversion toggles, zero-motion force mode, and skybox rotational motion toggle.
* **Interactive Buffer Viewers:** Dedicated preview windows for Scene Depth, Scene Color, Emissive Glow (Early HDR), and Motion Vectors.
* **Configuration Persistence:** Save runtime tuning adjustments directly to `edpe.ini` via the UI button.

### Synchronized GPU Frame Capture (`F6`)
Pressing **`F6`** saves five synchronized 32-bit BMP images in `edpe-captures/` beside `EliteDangerous64.exe`:
* `color-*.bmp`: Tone-mapped scene color HDR (`DXGI_FORMAT_R11G11B10_FLOAT`).
* `motion-*.bmp`: Visualized screen-space motion displacement (`DXGI_FORMAT_R16G16_FLOAT`).
* `depth-dsv2-*.bmp`: Contrast-adjusted reversed-Z depth buffer (`DXGI_FORMAT_R32G8X24_TYPELESS`).
* `early-color-*.bmp`: Emissive / illumination pass isolating glowing cockpit dials, indicators, and lamps (reactive mask candidate).
* `frame-*.bmp`: Pristine native swapchain backbuffer captured immediately before ImGui overlay rendering.

*Note: GPU readbacks are mapped with `D3D11_MAP_FLAG_DO_NOT_WAIT` and polled asynchronously, completely preventing game hitching or render thread stalls.*

---

## Configuration (`edpe.ini`)

EDPE automatically reads and writes configuration parameters from `edpe.ini` located in the game directory:

```ini
[Upscaler]
; Upscaler backend: 0 = Native (Bypass), 1 = NVIDIA DLSS/DLAA, 2 = AMD FSR, 3 = Native D3D11 TAA
Backend=1
; Upscale mode: 0 = Native, 1 = NativeAA (DLAA), 2 = Quality, 3 = Balanced, 4 = Performance, 5 = Ultra Performance
Mode=1
; DLSS Preset: 0 = Auto, 1 = J, 2 = K, 3 = L, 4 = M
Preset=0
AutoSyncEliteConfig=1
PreserveNativeUi=1
HdrInput=0
Sharpness=0.0

[Jitter]
InvertX=0           ; Invert horizontal jitter passed to upscaler
InvertY=0           ; Invert vertical jitter passed to upscaler
SwapXY=0            ; Swap jitter X and Y axes
Scale=1.00          ; Multiplier for jitter offset (0.0 = disable)
PhaseCount=32       ; Sequence phase repeat count (DLAA standard: 32)

[Motion]
ZeroMotion=0        ; Force stationary motion vectors (0,0) for debugging
InvertX=0           ; Invert horizontal motion vector direction
InvertY=0           ; Invert vertical motion vector direction
Scale=1.00          ; Scale multiplier for motion vectors

[Overlay]
ShowPerfOverlay=1   ; Toggle real-time performance telemetry HUD (F7)
EnableProfiler=0    ; High-precision QPC per-pass execution profiler
Corner=1            ; 0 = Top-Left, 1 = Top-Right, 2 = Bottom-Left, 3 = Bottom-Right
Scale=1.50          ; HUD scale multiplier (0.5x - 2.0x)
PosX=1900.0         ; Custom dragged X position (-1.0 = use corner default)
PosY=496.0          ; Custom dragged Y position (-1.0 = use corner default)
```

Runtime diagnostics and initialization events are written to `edpe.log` beside the game executable.

---

## Building EDPE

### Prerequisites
* Windows 10/11 x64.
* Visual Studio 2022 with **Desktop development with C++** (MSVC x64 toolchain, Windows 10/11 SDK).
* CMake 3.25 or newer.
* Python 3 (for fetching AMD FidelityFX DX11 SDK).
* Dear ImGui Git submodule.
* NVIDIA NGX SDK (for DLAA/DLSS; placed under `external/DLSS`).
* AMD FidelityFX DX11 SDK (fetched via `python tools/fetch_ffx_dx11.py`).

### 1. Clone & Initialize Submodules
```powershell
git clone https://github.com/filispeen/EDPE.git
cd EDPE
git submodule update --init --recursive
```

*(Optional)* You can run `install-build-requirements.ps1` from an elevated PowerShell to automatically configure dependencies:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install-build-requirements.ps1
```

### 2. Compile via x64 Developer PowerShell
Open **x64 Native Tools Command Prompt for VS 2022** (or x64 Developer PowerShell) and run:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

The build produces:
* `build/dxgi.dll`: Consolidated proxy library (DXGI swapchain observer, D3D11 device hooks, upscalers, ImGui overlay).
* `build/stage/`: Pre-staged directory containing:
  * `dxgi.dll`
  * `nvngx_dlss.dll` (if NGX SDK is present)

### 3. Run Automated Tests
```powershell
ctest --test-dir build -C Release --output-on-failure
```
All 11 automated unit and integration tests should pass:
* `temporal_math`: Halton sequence generation, projection math, matrix rejection bounds.
* `camera_delta`: Camera displacement and scene reset detection.
* `capture_format`: Half-float, float11/10 unpackers, depth contrast, Reinhard tone mapping.
* `temporal_inputs`: Temporal input validation, vendor translation conventions, fail-open logic.
* `motion_gpu`: Direct3D 11 GPU motion vector compute/pixel pass.
* `ui_composite`: Native-resolution UI preservation and alpha blending pass.
* `dxbc_fanout`: Shader bytecode interception and hash validation.
* `jitter_view_report`: Subpixel jitter diagnostics and luma correlation verification.
* `proxy_smoke`: DLL proxy loading and swapchain hook smoke checks.
* `proxy_shader_probe`: Shader bytecode interception verification.
* `proxy_alias_smoke`: Consolidated `dxgi.dll` staged loading smoke checks.

---

## Installation & Deployment

1. Build the project or obtain the latest release package from [Releases](https://github.com/filispeen/EDPE/releases/latest).
2. Locate your Elite Dangerous installation folder (the directory containing `EliteDangerous64.exe`, typically under `Products\elite-dangerous-odyssey-64` or `Products\elite-dangerous-64`).
3. Copy the staged proxy libraries into the game folder:
   * Copy `build/stage/dxgi.dll` $\rightarrow$ `EliteDangerous64.exe` directory
   * *(For NVIDIA DLAA/DLSS)* Copy `build/stage/nvngx_dlss.dll` $\rightarrow$ `EliteDangerous64.exe` directory
   * If an older `d3d11.dll` from previous EDPE versions remains in the game folder, make sure to delete it (EDPE now runs exclusively as `dxgi.dll`).
4. Launch Elite Dangerous normally through the Frontier Launcher or Steam.
5. In game, press **`F5`** to toggle the EDPE overlay and verify that your desired upscaler or TAA backend is active.
6. Check `edpe.log` beside `EliteDangerous64.exe` for detailed diagnostic logs.

### Uninstallation
To remove EDPE, simply delete or rename `dxgi.dll` and `nvngx_dlss.dll` from the game directory. Elite Dangerous will immediately revert to standard engine rendering.

---

## Project Structure

```text
EDPE/
├── external/                # Third-party dependencies (imgui, DLSS SDK, FidelityFX SDK)
├── src/
│   ├── config.h             # Central configuration (kVersion, edpe.ini management)
│   ├── context_census.cpp   # Direct3D 11 pipeline observation, CB0 parsing, jitter injection
│   ├── d3d11_proxy.cpp      # D3D11CreateDevice hook & system DLL forwarding
│   ├── dxgi_proxy.cpp       # DXGI factory hooks
│   ├── dxgi_observer.cpp    # Swapchain creation & Present hooks
│   ├── motion_pass.cpp      # GPU camera-depth motion vector generation
│   ├── ngx_context.cpp      # NVIDIA NGX DLAA/DLSS evaluation wrapper
│   ├── amd_upscaler_backend.cpp # AMD FidelityFX FSR 3.1.2 backend
│   ├── taa_backend.cpp      # Native Direct3D 11 TAA reconstruction pass
│   ├── upscaler_backend.cpp # Vendor-neutral upscaler manager & backend abstraction
│   ├── temporal_inputs.h    # Shared TemporalFrameInputs data contract
│   ├── profiler.h / .cpp    # High-precision QPC per-pass execution profiler
│   └── ui.cpp               # Dear ImGui overlay, telemetry HUD, F5-F9 hotkeys, snapshot visualizers
├── tests/                   # Automated unit & integration test suites
├── tools/
│   ├── fetch_ffx_dx11.py    # Automation script for fetching and building FidelityFX DX11 SDK
│   └── ngx_probe/           # Standalone diagnostic probe for hardware NGX verification
├── docs/                    # Technical research, reverse-engineering notes & input contracts
├── PLAN.md                  # Authoritative architectural roadmap
├── STATUS.md                # Verified empirical findings and implementation log
└── AGENTS.md                # Engineering guidelines, governance, and architecture rules
```

---

## Important Documentation References

* [STATUS.md](STATUS.md): Up-to-date verified empirical facts, test results, and milestone logs.
* [PLAN.md](PLAN.md): Detailed 15-phase architectural roadmap.
* [AGENTS.md](AGENTS.md): Project governance, architecture constraints, and contribution rules.
* [render-pipeline-observations.md](docs/render-pipeline-observations.md): Reverse-engineering findings on Elite's MRT layout, depth buffers, and lighting passes.
* [nvidia-dlaa-input-contract.md](docs/nvidia-dlaa-input-contract.md): Specifications for NVIDIA NGX DLAA integration.
* [elite-input-conventions.md](docs/elite-input-conventions.md): Camera, coordinate system, and projection conventions.

---

## Acknowledgments & Research

* **EDVR Unofficial Patch** ([characterecho-sean/edvr-unofficial-patch](https://github.com/characterecho-sean/edvr-unofficial-patch)): Conceptual reference for depth discovery and motion injection in the Cobra engine.
* **Dear ImGui** ([ocornut/imgui](https://github.com/ocornut/imgui)): Runtime debugging interface.
* **NVIDIA NGX SDK**: Temporal anti-aliasing and deep learning reconstruction technology.
* **AMD FidelityFX SDK**: High-quality open-source temporal upscaling technology.

---

світла пам'ять соні, моїй любій кішці яка померла 24 січня 26-го року около 1:00 AM
