# AGENTS.md

## Project & Scope
- **EDPE — Elite Dangerous Performance Enhanced**: rendering-performance enhancement mod for the **2D Direct3D 11 renderer of Elite Dangerous** (`EliteDangerous64.exe`).
- **Rendering-only scope**: never implement gameplay cheats, automation, network modifications, or anti-cheat/security bypasses.
- **Naming & files**: use `EDPE` consistently across code, Dear ImGui, logs, config, docs, versions, and tags. Owned runtime filenames: `dxgi.dll`, `edpe.ini`, `edpe.log`.
- **Authoritative docs**:
  - `PLAN.md` — full roadmap, backend designs, and phase specifications. Read before architectural changes.
  - `STATUS.md` — verified current implementation status and test inventory.
  - `docs/` — verified reverse-engineering findings and backend input contracts.

## Build, Test & Layout
- **Stack**: CMake, MSVC x64, C++20, HLSL, Dear ImGui (`external/imgui`).
- **Build & test** (from x64 VS Developer shell):
  - Ninja: `cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-ninja && ctest --test-dir build-ninja --output-on-failure`
  - NMake: `cmake -S . -B build -G "NMake Makefiles" && cmake --build build && ctest --test-dir build --output-on-failure`
- **Repository map**:
  - `src/`: DXGI/D3D11 proxy & hooks (`dxgi_proxy.cpp`, `d3d11_proxy.cpp`, `context_census.cpp`, `vtable_hook.h`, `code_hook.h`), temporal inputs & motion (`temporal_inputs.h`, `temporal_math.h`, `elite_camera.h`, `motion_pass.cpp`), upscalers (`upscaler_backend.cpp`, `ngx_context.cpp`, `taa_pass.cpp`), HUD/UI (`ui.cpp`, `ui_layer.cpp`, `ui_composite_pass.cpp`, `perf_overlay.cpp`), D3D12 interop & Frame Generation (`d3d12_bridge.cpp`, `d3d12_presenter.cpp`, `fg_backend.cpp`, `fg_integration.cpp`, `amd_fsr_fg_backend.cpp`, `nvidia_sl_fg_backend.cpp`), central version (`version.cpp`).
  - `tests/`: CTest CPU unit tests and standalone D3D11/D3D12 harnesses (do not make every test require launching Elite Dangerous).
  - `tools/`: standalone probes and SDK fetch helpers.

## Architecture Invariants
- **Shared temporal inputs**: Elite-specific observation and input generation (scene color, depth, camera/projection, jitter, motion vectors, HUD isolation, history resets) belong to EDPE (`TemporalFrameInputs`). Do not build duplicate per-vendor Elite pipelines.
- **Vendor isolation**: keep NGX, FidelityFX, Streamline, XeSS, and low-latency SDKs behind backend interfaces (`IUpscalerBackend`, `IFrameGenerationBackend`). Do not leak vendor types or headers into Cobra hooks.
- **Explicit convention translation**: each backend must explicitly translate EDPE conventions (motion direction/scale, jitter sign, reversed-Z depth, exposure, reactive/UI masks) to vendor conventions. Validate independently via deterministic tests, never by visual guessing.
- **Independent Frame Generation**: FG is a separate subsystem from temporal upscaling. Do not hardwire vendor pairings (e.g., DLSS -> NVIDIA FG). Gate all modes, GPU support, and FG multipliers through vendor capability APIs — never hardcode GPU generations or fixed multipliers.
- **True Super Resolution**: only treat SR as a performance win when Cobra renders the 3D scene below output resolution (`renderWidth < outputWidth`); never call native rendering + downsampling "Super Resolution".
- **HUD & UI**: Dear ImGui is the runtime control surface (`F5` toggles menu, `F7` toggles performance HUD; no separate launcher unless requested). When hidden, ImGui must not intercept game input. Unsupported features must be greyed out with an explanation. Keep native-res HUD/UI separated from temporal reconstruction and FG (`UiLayer`, `UiCompositePass`, `FgUiPackPass`).
- **Low Latency (Reflex / Anti-Lag 2)**: operates both standalone and coupled with FG. Distinguish simulation/rendered FPS from presented FPS when FG is active; never claim FG reduces latency merely because presented FPS increases.
- **Fail open**: on any upscaler or FG failure, disable the affected backend, stand down jitter/proxy swapchain as needed, restore original presentation, and `Present` normally. Never black-screen Elite. Never leave projection jitter active when temporal reconstruction is bypassed.

## Hooking, Reverse Engineering & Interop
- **Small COM hooks**: hooks only observe, update lightweight state, call an isolated EDPE subsystem, and forward to the original method. No heavy rendering algorithms inline.
- **Decision priority**: (1) existing Elite/Cobra capability -> (2) documented D3D11 behavior -> (3) documented vendor API -> (4) stable graphics-resource interception -> (5) isolated reverse engineering -> (6) D3D11/D3D12 interop -> (7) executable patching only as a last resort (must be isolated, validated, version-gated, fail-safe, and documented).
- **Resource classification**: never identify or modify a texture solely by resolution. Use multi-signal evidence (format, bind flags, usage, sample count, creation order, draw frequency, depth/viewport association, shader hashes, copies, frame position).
- **D3D11/D3D12 interop**: keep interop isolated, prefer zero-copy shared NT handles and shared fences, and measure copy/sync/GPU/CPU/VRAM/frame-pacing overhead.
- **Document RE findings**: record verified renderer facts in `docs/` with date, game build (if known), method, evidence, confidence, and implications. Never leave important verified findings only in chat or terminal output.

## Performance, Diagnostics & Code Style
- **Forbidden on hot path**: blocking full-frame GPU readback, `Map()` on full-resolution GPU textures, blocking `GetData` loops, per-frame shader compilation, per-frame texture or backend-feature recreation, unnecessary D3D11/D3D12 copies.
- **Prefer**: persistent/pooled GPU resources, compute passes, asynchronous timestamp queries (`D3D11_QUERY_TIMESTAMP_DISJOINT`), small focused modules, RAII for COM objects, explicit ownership/formats/states, enums over magic numbers, minimal global state.
- **Math documentation**: matrix and reprojection code must document row/column-major layout, handedness, clip-space convention, multiplication order, motion-vector direction, and jitter convention.
- **Diagnostics**: every subsystem must expose/log whether it ran, active backend, resources/dimensions/formats, temporal conventions, GPU timing cost, and exact failure/fallback reasons.

## References, Dependencies & Licensing
- **Vendor sources of truth**: official NVIDIA (NGX/DLSS, Streamline, Reflex), AMD (FidelityFX/GPUOpen, Anti-Lag 2), and Intel (XeSS / `libxess`) SDKs and documentation.
- **EDVR (`characterecho-sean/edvr-unofficial-patch`)**: technical reference only — do not port its VR stack (OpenVR/OpenXR/HMD/foveation), do not assume EDVR details match current SDKs, and **never reuse EDVR's NGX Project ID**.
- **Provenance & dependencies**: inspect licenses and record provenance before adapting external code (EDVR, OptiScaler, community ports, vendor samples). Never redistribute proprietary SDK material contrary to license terms. Before adding any new dependency, document why it is needed, license, runtime cost, binary-size impact, and maintenance burden.

## Git, Versioning & Hygiene
- **Commits**: commit after every coherent change/milestone (`inspect -> implement -> build -> test -> review diff -> commit`). Use concise descriptive messages; avoid vague messages (`wip`, `update`, `fix`, `misc`). Do not start another major subsystem with substantial uncommitted work.
- **Pre-commit**: build affected targets, run `ctest`, and review `git diff` + `git status`.
- **Never commit**: secrets, SDK credentials, personal machine paths, `.rdc` captures, texture dumps, build caches, or accidental binaries.
- **Versioning & tags**: keep the version string centralized in `src/version.cpp` (exposed to UI, logs, diagnostics, release metadata). Use annotated `vMAJOR.MINOR.PATCH` tags for stable milestones only (verify clean working tree, build, and tests first). Never overwrite or force-move an existing release tag without explicit user approval.
