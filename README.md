# EDPE — Elite Dangerous Performance Enhanced

[![Version](https://img.shields.io/badge/version-v0.1.0-blue.svg)](STATUS.md)
[![Status](https://img.shields.io/badge/status-DLAA%20Verified%20In--Game-success.svg)](STATUS.md)
[![Platform](https://img.shields.io/badge/platform-Windows%20x64%20|%20Direct3D%2011-lightgrey.svg)]()
[![Tests](https://img.shields.io/badge/tests-9%2F9%20Passing-brightgreen.svg)]()

**EDPE (Elite Dangerous Performance Enhanced)** is a standalone graphics enhancement proxy mod targeting the **2D Direct3D 11 renderer of Elite Dangerous** (Cobra engine).

Elite Dangerous natively lacks modern temporal reconstruction technologies in its standard 2D rendering path. EDPE injects temporal anti-aliasing and super-resolution reconstruction into the engine pipeline via non-invasive graphics API observation—without modifying game binaries, game logic, or network code.

---

## Current Status: Milestone `v0.1.0` (In-Game NVIDIA DLAA Verified)

As of **October 2026 (`v0.1.0`)**, EDPE has achieved **full in-game verification of NVIDIA DLAA**:

* **Non-invasive D3D11 / DXGI Proxy:** Hooks device creation and swapchain presentation without black screens, crashes, or rendering artifacts.
* **Scene Depth Discovery:** Successfully isolated authoritative reversed-Z scene depth buffer (`DSV #2`, `DXGI_FORMAT_R32G8X24_TYPELESS`, $B=0.025$).
* **Dynamic Camera Matrix Tracking:** Parses the 5376-byte dynamic constant buffer (`CB0`), extracting view, projection, and depth parameters each frame.
* **Projection Jitter Injection:** Generates Halton (2,3) subpixel projection jitter and injects it into dynamic CB0 projection matrix words (1080–1095) with **automatic fail-open stand-down** when reconstruction is disabled.
* **Camera-Depth Motion Vectors:** Real-time GPU motion pass generates per-pixel current→previous screen-space motion vectors (`DXGI_FORMAT_R16G16_FLOAT`).
* **Vendor-Neutral Temporal Input Layer:** Abstracted `TemporalFrameInputs` structure decoupling engine state from vendor reconstruction SDKs.
* **NVIDIA DLAA Evaluation:** Official NVIDIA NGX SDK integrated with EDPE-owned Project ID (`48d353f3-d07b-4048-876b-09f8622f5a27`). DLAA runs on the finished swapchain backbuffer at `Present` prior to overlay rendering, achieving **rock-solid temporal anti-aliasing with ~0.3 ms GPU overhead** and zero subpixel image vibration.
* **Dear ImGui In-Game Overlay (`F5`):** Runtime control surface displaying live GPU timings, jitter/motion diagnostics, and temporal tuning controls. Passes all inputs through to the game when closed.
* **Synchronized 5-Buffer GPU Capture (`F6`):** One-shot capture of all 5 engine buffers (`color`, `motion`, `depth`, `early-color` emissive, `frame`) from the exact same frame using non-blocking asynchronous GPU readback (`D3D11_MAP_FLAG_DO_NOT_WAIT`), saved to `edpe-captures/`.
* **Automated Test Suite:** 9 automated tests passing in CTest covering math, camera deltas, pixel formats, proxy hooks, and GPU motion passes.

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
| **9–10** | **True Render Scale & NVIDIA DLSS SR** | 🟡 **Next Up** | Lower 3D scene render resolution below output resolution (`input < output`) with DLSS reconstruction. |
| **11** | **AMD FSR Super Resolution & Native AA** | 🟡 **In Progress** | `AmdUpscalerBackend` interface defined; FidelityFX integration pending backend selection. |
| **12** | **Native-Resolution HUD Separation** | 📋 **Planned** | Composite native-res UI over upscaled 3D world (candidate HUD shaders identified). |
| **13–15** | **D3D11/D3D12 Interop & Frame Generation** | 🔬 **Research** | NVIDIA DLSS Frame Generation (Streamline) & AMD FSR Frame Generation via interop bridge. |

See [STATUS.md](STATUS.md) for verified facts and [PLAN.md](PLAN.md) for the complete roadmap.

---

## Architecture

EDPE enforces a strict separation: Elite-specific engine reverse-engineering belongs to EDPE; vendor-specific reconstruction logic lives behind modular backend interfaces.

```text
               Elite Dangerous (Cobra Engine D3D11)
                                |
                                v
                      EDPE Proxy Injection
                     (d3d11.dll / dxgi.dll)
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
        |                                               |
        v                                               v
  NVIDIA Backend                                   AMD Backend
 (NGX DLAA / DLSS SR)                         (FSR Native AA / SR)
        |                                               |
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
3. **No Duplicate Pipelines:** Common temporal inputs (`TemporalFrameInputs`) are calculated once and translated into vendor-specific conventions (NVIDIA/AMD) at the backend boundary.
4. **Fair Play Guarantee:** Rendering enhancements only. No gameplay automation, no networking hooks, no security bypasses.

---

## In-Game Controls & Hotkeys

| Hotkey | Action | Description |
|:---:|:---|:---|
| **`F5`** | **Toggle EDPE Menu** | Opens/closes the Dear ImGui control panel. When closed, all mouse and keyboard inputs pass freely to the game. |
| **`F6`** | **One-Shot Synchronized Capture** | Captures all 5 rendering buffers from the **exact same frame** without GPU stalling, saving BMPs to `edpe-captures/`. |
| **`Insert`** | **Game Pass-Through** | Configured to pass transparently to the original game window procedure. |

### Dear ImGui Control Panel Features
* **Upscaler Backend Selector:** Switch seamlessly between `Native (Bypass)` and `NVIDIA DLSS / DLAA`.
* **Live GPU Reconstruction Time:** Asynchronous GPU timestamp query displaying exact upscaler overhead (~0.3 ms for DLAA).
* **Jitter Controls & Diagnostics:** Real-time toggle, subpixel vibration test override, phase count cycle slider (default: 32), X/Y inversion, and axis swap.
* **Motion Vector Tuning:** Motion scale slider, X/Y inversion toggles, and zero-motion force mode.
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
Backend=1           ; 0 = Native (Bypass), 1 = NVIDIA DLSS/DLAA, 2 = AMD FSR
Mode=1              ; 0 = Native, 1 = NativeAA (DLAA), 2 = Quality, 3 = Balanced, 4 = Performance, 5 = Ultra Performance

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
```

Runtime diagnostics and initialization events are written to `edpe.log` beside the game executable.

---

## Building EDPE

### Prerequisites
* Windows 10/11 x64.
* Visual Studio 2022 with **Desktop development with C++** (MSVC x64 toolchain, Windows 10/11 SDK).
* CMake 3.25 or newer.
* NMake (included with Visual Studio).
* Dear ImGui Git submodule.
* NVIDIA NGX SDK (for DLAA/DLSS; placed under `external/DLSS`).

### 1. Clone & Initialize Submodules
```powershell
git clone https://github.com/username/EDPE.git
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
cmake -S . -B build -G "NMake Makefiles" "-DCMAKE_MAKE_PROGRAM=$((Get-Command nmake).Source)"
cmake --build build --config Release
```

The build produces:
* `build/EDPE.dll`: Core D3D11 device hook library.
* `build/dxgi.dll`: DXGI swapchain observer and Present hook library.
* `build/stage/`: Pre-staged directory containing:
  * `d3d11.dll` (copy of `EDPE.dll` ready for game injection)
  * `dxgi.dll`
  * `nvngx_dlss.dll` (if NGX SDK is present)

### 3. Run Automated Tests
```powershell
ctest --test-dir build -C Release --output-on-failure
```
All 9 automated unit and integration tests should pass:
* `temporal_math`: Halton sequence generation, projection math, matrix rejection bounds.
* `camera_delta`: Camera displacement and scene reset detection.
* `capture_format`: Half-float, float11/10 unpackers, depth contrast, Reinhard tone mapping.
* `temporal_inputs`: Temporal input validation, vendor translation conventions, fail-open logic.
* `motion_gpu`: Direct3D 11 GPU motion vector compute/pixel pass.
* `dxbc_fanout`: Shader bytecode interception and hash validation.
* `proxy_smoke`, `proxy_shader_probe`, `proxy_alias_smoke`: DLL proxy loading and swapchain hook smoke checks.

---

## Installation & Deployment

1. Build the project or obtain a release package.
2. Locate your Elite Dangerous installation folder (the directory containing `EliteDangerous64.exe`, typically under `Products\elite-dangerous-odyssey-64` or `Products\elite-dangerous-64`).
3. Copy the staged proxy libraries into the game folder:
   * Copy `build/stage/d3d11.dll` $\rightarrow$ `EliteDangerous64.exe` directory
   * Copy `build/stage/dxgi.dll` $\rightarrow$ `EliteDangerous64.exe` directory
   * *(For NVIDIA DLAA/DLSS)* Copy `build/stage/nvngx_dlss.dll` $\rightarrow$ `EliteDangerous64.exe` directory
4. Launch Elite Dangerous normally through the Frontier Launcher or Steam.
5. In game, press **`F5`** to toggle the EDPE overlay and verify that NVIDIA DLAA is running.
6. Check `edpe.log` beside `EliteDangerous64.exe` for detailed diagnostic logs.

### Uninstallation
To remove EDPE, simply delete or rename `d3d11.dll`, `dxgi.dll`, and `nvngx_dlss.dll` from the game directory. Elite Dangerous will immediately revert to standard engine rendering.

---

## Project Structure

```text
EDPE/
├── external/                # Third-party dependencies (imgui, DLSS SDK)
├── src/
│   ├── config.h             # Central configuration (kVersion, edpe.ini management)
│   ├── context_census.cpp   # Direct3D 11 pipeline observation, CB0 parsing, jitter injection
│   ├── d3d11_proxy.cpp      # D3D11CreateDevice hook & system DLL forwarding
│   ├── dxgi_proxy.cpp       # DXGI factory hooks
│   ├── dxgi_observer.cpp    # Swapchain creation & Present hooks
│   ├── motion_pass.cpp      # GPU camera-depth motion vector generation
│   ├── ngx_context.cpp      # NVIDIA NGX DLAA/DLSS evaluation wrapper
│   ├── upscaler_backend.cpp # Vendor-neutral upscaler manager & backend abstraction
│   ├── temporal_inputs.h    # Shared TemporalFrameInputs data contract
│   └── ui.cpp               # Dear ImGui overlay, F5/F6 hotkeys, snapshot visualizers
├── tests/                   # Automated unit & integration test suites
├── tools/
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

---

світла пам'ять соні, моїй любій кішці яка померла 24 січня 26-го року около 1:00 AM
