# EDPE render-pipeline observations

Date: 2026-09-23. Target: Elite Dangerous 2D; installed Odyssey build `2026.09.03.332841`.

## Repository state

- **VERIFIED (workspace inspection):** This repository contains `AGENTS.md`, `PLAN.md`, and `sPROMPT.md` only. There is no source, README, build configuration, bundled SDK, injection mechanism, Git commit, or Git tag. `.codegraph/` is absent.
- **VERIFIED (local toolchain):** CMake 4.4.2, MSVC 14.44 x64, and Windows SDK 10.0.26100.0 are installed. No Elite executable or capture is in this workspace.
- **UNVERIFIED:** The 2D game's swapchain, backbuffer, depth, camera, projection, render scale, presentation mode, and any compatible DLL loading path. No game process or graphics capture was observed.

## Reference findings

- **REFERENCE-CODE:** [EDVR's depth probe](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/d3d11/depth_probe.cpp) treats its observed VR depth as reversed Z and uses a depth copy for inspection. That observation is specific to EDVR's measured VR path; EDPE must measure the 2D path independently.
- **REFERENCE-CODE:** [EDVR's temporal pass](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/d3d11/temporal_pass.cpp), [temporal math](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/common/temporal_math.h), [DLAA implementation](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/d3d11/dlaa.cpp), [native temporal path](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/d3d11/native_temporal.cpp), and [per-object motion notes](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/docs/per-object-motion.md) provide candidates for depth, motion, jitter, history, and fallback design. Their eye-specific rendering and runtime code do not transfer to 2D.
- **REFERENCE-CODE:** [EDVR's FSR implementation](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/src/d3d11/fsr3_engine.cpp) identifies its D3D11 route as the metarutaiga community port, hardened by OptiScaler, with MIT provenance stated there. Confirm exact source and license before any reuse.
- **SDK-DOCUMENTED:** [NVIDIA's DLSS SDK](https://github.com/NVIDIA/DLSS) includes D3D11 NGX support. Its [current definitions](https://github.com/NVIDIA/DLSS/blob/main/include/nvsdk_ngx_defs.h) list presets J, K, L, and M. EDPE must verify input conventions against the exact SDK chosen for implementation.
- **SDK-DOCUMENTED:** [AMD's FidelityFX API guide](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/getting-started/ffx-api.md) describes the current effect DLLs and a D3D12 backend. [FSR 3.1 migration guidance](https://gpuopen.com/manuals/fidelityfx_sdk/getting-started/migrating-to-fsr-3-1/) separates upscaling from frame generation and lists D3D12/Vulkan backends. The D3D11 port and an official D3D12 bridge remain candidates to measure later.
- **SDK-DOCUMENTED:** [Dear ImGui](https://github.com/ocornut/imgui/blob/master/docs/EXAMPLES.md) supplies Win32 and DX11 backends. [Streamline's FG guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md) requires capability queries and Reflex integration; FG is outside Phase 1.

## First vertical slice

1. Build an x64 `EDPE.dll` with a minimal, safe process-attach entry point. This proves the toolchain and establishes the project artifact only.
2. Determine a compatible loading route before replacing a game DLL. EDVR's [installation notes](https://github.com/characterecho-sean/edvr-unofficial-patch) show that another mod may already own `d3d11.dll`; overwriting it would be unsafe.
3. Observe D3D11 device and swapchain creation, then log `Present`, backbuffer description, and device/context identity without changing output.
4. Add Dear ImGui with Win32/DX11 after the pass-through path is verified. Confirm hidden input behavior and safe resize/shutdown.

The `v0.0.1` tag requires a tested in-game load, normal rendering, Present observation, device/context discovery, ImGui toggle, and logging. No current measurement meets that gate.

## Installed game inspection — 2026-09-23

Game build: `2026.09.03.332841` (`EliteDangerous64.exe` file version `332841`, product version `4.4.1.1`). Executable SHA-256: `E6BE8BBE04E6A7AE226D4318945AF7F367DE13DC5A007A261964D9BA8144E988`.

- **VERIFIED (PE imports):** The x64 executable imports `D3D11CreateDevice` from `d3d11.dll` and `CreateDXGIFactory1` from `dxgi.dll`. It also delay-loads `openvr_api.dll`. This identifies possible observation entry points, not when or how the 2D scene presents.
- **VERIFIED (directory inspection):** Neither an active local `d3d11.dll` nor `dxgi.dll` is present. A `d3d11.dll.disabled` file is 3Dmigoto 1.3.16, and backup folders contain both 3Dmigoto and EDVR D3D11 proxies. The existing `edvr.ini` names `d3d11_edhm.dll` as a chained real DLL, but that file is not currently beside the executable.
- **VERIFIED (existing EDVR diagnostic):** An EDVR breadcrumb records its D3D11 proxy loading, a chained proxy call, hook arming, frames, and process exit in an earlier session. This supports the local-proxy route for that session only. It does not validate EDPE or the 2D renderer.
- **IMPLICATION:** Build and test EDPE's proxy in isolation before installation. Never overwrite the disabled 3Dmigoto binary or EDVR backups. Recheck the live directory immediately before any installation because mod tools can change it.

Still unknown: which swapchain corresponds to the 2D scene, actual `Present` timing, device/context identity, backbuffer description, and normal-rendering compatibility with EDPE.

## Local proxy check — 2026-09-23

- **VERIFIED (local test):** `EDPE.dll` exports `D3D11CreateDevice` and forwards it to the system D3D11 DLL loaded from System32. A WARP device and immediate context were created through the export, released, and the test passed.
- **LIMIT:** The export set currently covers only the executable's static D3D11 import. The DLL has not been placed beside the game; other modules may request additional D3D11 exports. Present observation and normal 2D rendering remain unverified.

## DXGI factory check — 2026-09-23

- **VERIFIED (local test):** A separate `dxgi.dll` shim exports the executable's observed `CreateDXGIFactory1` import and forwards it to System32. A test created and released an `IDXGIFactory1` through the shim.
- **LIMIT:** Additional DXGI exports and coexistence with other graphics mods remain untested; the shim is not ready to install.

## Local Present observation — 2026-09-23

- **VERIFIED (local WARP test):** The DXGI shim observes `IDXGIFactory::CreateSwapChain` and `IDXGISwapChain::Present` through instance-local COM vtables. A 64×64 WARP swapchain was created, two `Present(DXGI_PRESENT_TEST)` calls reached the observer, and the swapchain, factory, context, device, and test window were released. The original DXGI methods returned successfully.
- **VERIFIED (failure and correction):** The first test crashed during swapchain `Release` when the replacement vtable remained installed inside the original `Release` call. Restoring the original vtable for that call fixed the local crash. This does not establish safety under all multithreaded or mod-chained use.
- **LIMIT:** The current path observes `CreateSwapChain` and `Present`, not `CreateSwapChainForHwnd` or `Present1`. The DLL exports only the executable's static `CreateDXGIFactory1` import. In-game loading, other module imports, concurrency, resize, fullscreen transitions, and normal 2D rendering remain unverified. Do not install this prototype yet.

## Local logging check — 2026-09-23

- **VERIFIED (Release test):** The WARP harness produced `edpe.log` beside its executable with D3D11 creation, DXGI factory creation, and one first-Present line. The line contained the 64×64 swapchain, DXGI format 28 (`R8G8B8A8_UNORM`), and non-null D3D11 device/context pointers. The test reads the file and checks the Present entry.
- **LIMIT:** The file is opened only for infrequent diagnostics; no per-frame file write was added. The game path has not been tested for log write permission. Debugger output remains available if file creation fails.

## Dear ImGui dependency — 2026-09-23

- **SDK-DOCUMENTED:** [Dear ImGui](https://github.com/ocornut/imgui) `v1.92.9b` is pinned as a Git submodule at `f1cc2ae`, including its [MIT license](https://github.com/ocornut/imgui/blob/v1.92.9b/LICENSE.txt). It is required for the runtime control surface specified by `PLAN.md`; the Win32 and DX11 backends are included.
- **BUILD COST:** Six ImGui translation units add compile time. Linking the overlay will add binary size and per-visible-frame UI work; actual DXGI DLL size and GPU cost must be measured after integration. The pinned tag avoids silent SDK drift, but updates require deliberate compatibility testing.

## Local ImGui overlay check — 2026-09-23

- **VERIFIED (Release WARP test):** A hidden test window created a 64×64 swapchain. `Present(0, 0)` initialized Win32/DX11 ImGui and returned `DXGI_STATUS_OCCLUDED`; `Insert` opened and closed the EDPE status window. The test verified ordinary keyboard input reaches the original WndProc while hidden and is withheld while visible. `ResizeBuffers` to 128×128 succeeded with the overlay open, then another `Present` completed. The original graphics calls and resource release completed. Visible compositing was not measured.
- **MEASURED (binary size):** The Release `dxgi.dll` grew from 17,920 to 404,992 bytes after linking the Win32/DX11 overlay, a 387,072-byte increase. GPU cost and in-game frame impact are not yet measured.
- **LIMIT:** The test proves a local WARP path only. The game may use another swapchain-creation or Present variant, other threads, raw input handling, or graphics mods. The current DLL is still an experimental prototype and has not been installed beside Elite.

## Proxy filename regression — 2026-09-23

- **VERIFIED (local failure):** Renaming `EDPE.dll` to `d3d11.dll` in a test package caused recursive self-loading and stack overflow. Loading a short DLL name with `LOAD_LIBRARY_SEARCH_SYSTEM32` was insufficient once the proxy with the same name was already loaded.
- **VERIFIED (local fix):** Both proxy exports now build an absolute System32 path and use `LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_SYSTEM32` for dependent DLLs. The package test creates local `d3d11.dll` and `dxgi.dll` copies, then creates a WARP device, DXGI factory, swapchain, Present observations, overlay, resize, and clean release. This is still a local harness, not a game validation.

## Menu hotkey — 2026-09-23

- **VERIFIED (Release WARP test):** `F5` now opens and closes the EDPE menu. A repeated keydown while F5 is held does not toggle it. `Insert` no longer toggles the menu and reaches the original window procedure while the menu is hidden.

## Local game deployment — 2026-09-23

- **VERIFIED (file hashes):** The Release-stage `d3d11.dll` and `dxgi.dll` were copied beside the supplied Odyssey `EliteDangerous64.exe`. SHA-256 of each destination matched its build-stage source immediately after copying. No pre-existing active DLL with either name was overwritten.
- **LIMIT:** Copying confirms deployment only. Game load, normal rendering, Present observation, ImGui visibility, and input handling still require an in-game test. The DLLs are experimental and their export sets remain limited.

## In-game loader failure — 2026-09-23

- **VERIFIED (user report and game log):** Game startup reported missing `CreateDXGIFactory2` while loading the system `d3d11.dll`. The game-side `edpe.log` contained `EDPE: DXGI factory created`, but no Present observation.
- **VERIFIED (PE imports):** Windows `System32\d3d11.dll` imports `CreateDXGIFactory2` from `dxgi.dll`. EDPE's installed `dxgi.dll` exported only `CreateDXGIFactory1`, which explains the loader failure.
- **VERIFIED (Release WARP test):** The DXGI proxy now forwards `CreateDXGIFactory2` to System32 and observes the returned factory. The test loads the local DXGI proxy before D3D11, creates a WARP device, calls both factory exports, presents, resizes, and releases resources. Both normal and filename-alias tests pass.
- **LIMIT (at fix time):** This local test corrected the identified loader error; the subsequent in-game check is recorded below.

## First in-game load — 2026-09-23

- **VERIFIED (game log):** Two startup sessions after the `CreateDXGIFactory2` fix recorded DXGI factory creation, D3D11 device creation, and a first `Present` on a 2560×1440 swapchain with format 28 (`DXGI_FORMAT_R8G8B8A8_UNORM`). Both sessions recorded non-null device and immediate-context pointers and `EDPE: Dear ImGui ready; F5 toggles menu`.
- **VERIFIED (user observation):** The missing-entry-point error is gone. The Dear ImGui overlay is visible and toggles with F5 in the game.
- **VERIFIED (user observation):** The 3D world and HUD displayed normally with the overlay; no black screen or visible rendering fault was reported in this session.
- **LIMIT:** The log contains only first-Present observations, not a frame count or GPU timing. Scene rendering quality, input interactions beyond F5, resize/fullscreen transitions, and long-session stability are not yet independently measured.

## Rejected depth-view hook — 2026-09-23

- **VERIFIED (Release WARP test):** A device-vtable hook for `CreateDepthStencilView` recorded a 32×32 D24S8 test view and passed both proxy smoke tests.
- **VERIFIED (game log and user observation):** With that hook installed, two game launches logged DXGI factory and D3D11 device creation but no `Present` or DSV entry. The user reported that the game closed immediately after launch.
- **ACTION:** Commit `8f31352` reverted the hook; the earlier pass-through DLLs were rebuilt and restored to the game's relocated installation. The exact failure point inside the device hook remains unverified. No depth candidate from the game was obtained.

## Rejected revised depth-view hook — 2026-09-23

- **VERIFIED (Release local tests):** A second version left `ID3D11Device::Release` untouched and retained the replacement vtable for the device lifetime. Two WARP tests and one hardware D3D11 test created a DSV, reached `Present`, resized, and released successfully.
- **VERIFIED (game log and user observation):** Two further game launches logged successful installation of the revised hook on 69-method `ID3D11Device5` pointers, then exited before any DSV entry or `Present`. The user confirmed that the game briefly opened and closed.
- **VERIFIED (comparison):** The `Release` wrapper alone did not cause the failure. Instance-vtable replacement on the D3D11 device is unsafe in this game under the tested conditions; the specific mechanism is unknown. Commit `46f0598` reverted the second attempt, and the known-working DLLs were restored before rebuilding and redeploying the pass-through version. Do not retry this hook pattern without evidence that explains the early exit.

## Read-only D3D11 dispatch check — 2026-09-23

- **EXPERIMENTAL:** Log the device pointer, vtable pointer, and `CreateDepthStencilView` slot pointer at device creation and first `Present`, without replacing either pointer. Matching device pointers will allow a direct comparison of dispatch state between those points. A changed address would be evidence of runtime table replacement; unchanged addresses would not explain the earlier failure.
- **REFERENCE-CODE:** [EDVR's current README](https://github.com/characterecho-sean/edvr-unofficial-patch) reports a separate context-vtable failure when a frozen copy fell out of sync with Windows D3D11's changing dispatch table. That is context evidence from EDVR's VR path, not proof that EDPE's device hook failed for the same reason.
- **VERIFIED (game log, build `2026.09.03.332841`):** The output device at creation and at first `Present` was the same pointer (`00000286AAA5B9A0`). Its vtable pointer (`00000286ABF1D550`) and slot-10 method pointer (`00000286ABDBB280`) were unchanged between those observations. One earlier successful `D3D11CreateDevice` call returned no output device. This rules out a dispatch-pointer change in that interval for the observed device; it does not identify why cloned-vtable attempts ended before `Present`.
- **VERIFIED (user observation):** The game and F5 menu worked normally with the read-only diagnostics.
- **SDK-DOCUMENTED:** [RenderDoc's resource inspector](https://github.com/baldurk/renderdoc/blob/v1.x/docs/window/resource_inspector.rst) and [texture viewer](https://github.com/baldurk/renderdoc/blob/v1.x/docs/window/texture_viewer.rst) can inspect resource relationships and depth contents from a D3D11 frame capture. RenderDoc is not currently installed in this environment; a game capture has not been made. [PIX requirements](https://devblogs.microsoft.com/pix/requirements/) do not offer direct D3D11 analysis without an 11-on-12 path.

## Present-bound targets — 2026-09-23

- **EXPERIMENTAL:** At the first observed `Present`, query `OMGetRenderTargets` and log the currently bound render-target and depth-stencil view with the depth texture's format, dimensions, bind flags, and sample count. This reads existing COM state and leaves the rendering path unchanged. A bound DSV at `Present` would be a candidate only; an unbound DSV would not prove the scene lacked depth earlier in the frame.
- **VERIFIED (game log, build `2026.09.03.332841`):** On a 2560×1440 first `Present`, `OMGetRenderTargets` returned no RTV and no DSV. The query logged zero formats and dimensions. Neither target was bound at this frame boundary; depth identification requires observation earlier in the frame.
- **VERIFIED (user observation):** The game and F5 menu still displayed normally with this read-only query.

## Next depth observation — 2026-09-23

- **SDK-DOCUMENTED:** [RenderDoc's capture guide](https://github.com/baldurk/renderdoc/blob/v1.x/docs/getting_started/quick_start.rst) describes D3D11 frame capture. Its [resource inspector](https://github.com/baldurk/renderdoc/blob/v1.x/docs/window/resource_inspector.rst) and [texture viewer](https://github.com/baldurk/renderdoc/blob/v1.x/docs/window/texture_viewer.rst) can show depth resources and their use earlier than `Present`.
- **VERIFIED (local environment):** RenderDoc is not installed. The official portable download could not be reached from the command-line network path. A frame capture has not been made.
- **EXPERIMENTAL PLAN:** Capture one normal 2D gameplay frame, locate DSV binds and clears alongside 3D draws, then inspect candidate textures, formats, dimensions, and view relationships. Record the game build and capture evidence before identifying scene depth. Keep the capture out of Git.

## RenderDoc launch check — 2026-09-23

- **VERIFIED (local tool):** The signed RenderDoc v1.46 portable ZIP was extracted under ignored `build/tools`. `renderdoccmd capture --wait-for-exit` ran the EDPE WARP smoke executable successfully (exit 0).
- **VERIFIED (game process and user observation):** Launching `EliteDangerous64.exe` directly through RenderDoc created a process with `renderdoc.dll` and both EDPE proxies loaded, but no main window, new EDPE log entry, or frame capture. The process stayed at roughly 0.23 seconds of CPU until EDPE stopped the two processes started for this experiment. The user did not see the game open normally.
- **LIMIT:** This does not show that RenderDoc is incompatible with the game or identify a depth resource. Direct executable launch may bypass the game's usual Steam/launcher path. `EDLaunch.exe` and `MinEdLauncher.exe` exist in the installation, but neither has been tested with RenderDoc. Determine the user's normal launch route before another capture attempt.

## Steam launcher capture check — 2026-09-23

- **VERIFIED (Steam configuration and user observation):** The normal launch chain is Steam → MinEdLauncher → Elite. Its launch option is `cmd /c "MinEdLauncher.exe %command% /edh4 /autorun /autoquit"`. With that option restored, the game and F5 menu work; EDPE again logs a 2560×1440 `Present`.
- **VERIFIED (capture experiment):** A temporary Steam launch option placed RenderDoc before MinEdLauncher and enabled `--opt-hook-children`. MinEdLauncher logged an attempt to launch Elite, and an `EliteDangerous64.exe` process appeared briefly, then exited without a new EDPE log entry or frame capture. The user reported that the game did not open. The original Steam option was restored immediately afterward.
- **LIMIT:** Neither RenderDoc experiment identified a depth resource. The reason this game launch fails under RenderDoc is unknown. Further capture attempts need a specific compatibility hypothesis; the ordinary Steam path must remain usable.

## Later-Present binding sample — 2026-09-23

- **EXPERIMENTAL:** The first observed `Present` may occur before normal gameplay. Log the same read-only binding query at frame 1 and every 1024th observed `Present` through frame 8192. This uses the existing, game-verified swapchain hook and performs no output modification. A later bound DSV would still be only a candidate, not verified scene depth.
- **MEASURED (game log, build `2026.09.03.332841`):** Nine samples at frames 1, 1024, 2048, 3072, 4096, 5120, 6144, 7168, and 8192 returned null RTV and DSV. The swapchain, D3D11 device, context, device vtable, and DSV method addresses were stable in all nine entries. The game process remained responsive during collection.
- **LIMIT:** The log does not mark when the player entered the 3D world. It proves only that these sampled `Present` boundaries had no bound targets. The scene-depth resource and its earlier binding point remain unidentified.
- **VERIFIED (user observation):** After loading from the main menu into the 3D world, pressing F5 caused the game to close with this sampling build. The log ended at frame 8192 and contains no failure reason. The earlier DLL backup also closed on F5 in the 3D menu. With both EDPE DLLs removed, F5 did not close the game. Thus the later sampling alone is not an established cause; the active EDPE overlay path is implicated.
- **DECISION:** The sampling code was reverted, and both EDPE DLLs were moved out of the game directory pending a focused UI fix. Do not retry D3D11 state queries at later `Present` frames until the F5 failure is understood.

## F5 overlay threading hypothesis — 2026-09-23

- **EXPERIMENTAL:** EDPE's window procedure called the ImGui Win32 message handler while `Present` built and rendered ImGui frames. These callbacks may execute on different threads in Elite; no thread IDs have yet been measured. This is a possible race, not a verified crash cause.
- **IMPLEMENTATION CHECK:** For the current informational menu, F5 can toggle an atomic visibility flag and the window procedure can block game input without calling ImGui. All ImGui API use then stays in the `Present` path. The WARP input test must still pass, followed by an in-game F5 check in the 3D menu.
- **VERIFIED (user observation):** With this isolation, F5 opened the EDPE menu in the 3D menu without closing the game. The menu could not be interacted with because the Win32 message handler was no longer receiving mouse and keyboard events.
- **EXPERIMENTAL NEXT STEP:** Queue ordinary Win32 input messages in the window procedure and deliver them to ImGui's Win32 handler on the `Present` thread before `NewFrame`. Bound the queue and coalesce mouse movement. Keep raw `WM_INPUT` cleanup in the window procedure. Restore the ImGui close button and verify interaction in game.
- **VERIFIED (Release WARP tests and game log):** Both proxy smoke tests passed with queued input. The game log recorded `EDPE: queued input routed to Dear ImGui` after F5 opened the overlay.
- **VERIFIED (user observation):** With the queued-input DLLs installed, the menu responded to the mouse and the game remained open. This confirms interactive input for the tested session; it does not identify the earlier crash mechanism or establish long-session stability.
- **IMPLICATION:** The F5 overlay is usable again. Scene depth remains unidentified because the sampled `Present` boundaries had no bound DSV.

## Read-only context dispatch timeline — 2026-09-23

- **REFERENCE-CODE:** [EDVR's current README](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/main/README.md#L126-L181) describes a context dispatch table that Windows may rewrite while the game runs. This is a reason to measure EDPE's context, not evidence that it behaves the same way.
- **SDK-DOCUMENTED:** [Microsoft's D3D11 depth-stencil guide](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-depth-stencil) binds a DSV through `OMSetRenderTargets`; [the API reference](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetrendertargets) states that a null DSV unbinds it. The null DSV observed at `Present` therefore does not identify an earlier depth binding.
- **EXPERIMENTAL:** Log the immediate-context pointer, vtable pointer, and slot-33 method pointer at frames 1 and every 1024th `Present` through 8192. This reads COM dispatch state without replacing a table or changing rendering. Compare game values before selecting a context-hook strategy; local WARP results alone cannot establish game compatibility.
- **MEASURED (game log, build `2026.09.03.332841`):** All nine samples from frames 1 through 8192 showed the same immediate-context pointer (`0000021BF52C10D8`), vtable pointer (`0000021BF52C10E0`), and slot-33 method pointer (`0000021BF4DC9950`). The log recorded queued ImGui input after the samples.
- **VERIFIED (user observation):** The 3D scene and F5 menu worked normally during this session.
- **LIMIT:** These samples rule out persistent changes to those three pointers at the sampled `Present` boundaries. They cannot rule out changes between samples or explain the earlier device-hook failures. No DSV binding was observed in this experiment.

## EDVR depth implementation review — 2026-09-23

- **REFERENCE-CODE (cloned EDVR revision `96df07575af124f77f0f7acc0e3953dbb8a9529d`, MIT):** [EDVR's device hook](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/96df07575af124f77f0f7acc0e3953dbb8a9529d/src/d3d11/device_hook.cpp) patches the device vtable in place by default. Its `CreateDepthStencilView` slot-10 hook primarily reports failed game calls; it does not identify scene depth from creation alone. EDPE's failed attempts instead replaced the device object's vtable pointer with a frozen copy. This difference is real, but it does not prove why EDPE exited before `Present`.
- **REFERENCE-CODE:** [EDVR's context hook](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/96df07575af124f77f0f7acc0e3953dbb8a9529d/src/d3d11/vscreen.cpp) observes both `OMSetRenderTargets` (slot 33) and `OMSetRenderTargetsAndUnorderedAccessViews` (slot 34), plus depth clears and draw calls. The [depth probe](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/96df07575af124f77f0f7acc0e3953dbb8a9529d/src/d3d11/depth_probe.cpp) describes DSV textures on first observed use, counts draws per target, records clear values, and samples candidates after they are unbound. A direct SRV and a same-family copy are checked because an SRV over a bound depth target can read as null.
- **REFERENCE-CODE:** EDVR chooses its context-hook mode by the module implementing the vtable entries: a live forwarding private table for a context backed by system D3D11, or an in-place shared-table patch for a wrapper. It also has hook-loss checks and a crash sentinel. EDPE has not measured method ownership or installed either context-hook strategy; stable slot addresses at sampled `Present` calls do not establish safe interception.
- **REFERENCE-CODE (VR only):** [EDVR's anti-aliasing measurements](https://github.com/characterecho-sean/edvr-unofficial-patch/blob/96df07575af124f77f0f7acc0e3953dbb8a9529d/docs/anti-aliasing.md) found a busy per-eye depth pair with `R32G8X24_TYPELESS` textures, `D32_FLOAT_S8X24_UINT` views, bind flags `0x48`, and clear value `0.0`; sampled cockpit values supported reversed Z. Its pair selection uses eye dimensions, draw counts, and first-bind order. These VR-specific identities and near/far measurements cannot be assigned to the 2D renderer without a separate EDPE census.
- **IMPLICATION:** The next EDPE depth step is a limited, read-only context binding and draw census. Identify 2D candidates by format, bind flags, viewport/color association, draw count, clear value, and frame timing before sampling or changing any texture. No EDVR source was copied into EDPE.

## Initial 2D DSV bind census — 2026-09-23

- **EXPERIMENTAL:** After the first observed `Present`, patch only immediate-context `OMSetRenderTargets` (slot 33) in place. Forward the original call unchanged, then describe at most 32 distinct non-null DSV pointers by texture size/format, view format, bind flags, and sample count. Log bind totals at every 1024th `Present`. Restore the slot when the observed swapchain is finally released, but only if it still points at EDPE's hook.
- **LIMIT:** This first pass does not observe slot 34 (`OMSetRenderTargetsAndUnorderedAccessViews`), clears, draw counts, or binds before the first `Present`. Pointer reuse can make the bounded distinct-view count incomplete. Local WARP tests must pass before an in-game attempt; any game result remains experimental until observed.
- **VERIFIED (Release WARP tests and user observation, Odyssey executable file version `332841` / product version `4.4.1.1`):** Both proxy smoke tests passed, including restoration of slot 33 after swapchain release. In the game, the user reported normal 3D world, HUD, and F5-menu behavior during the DSV-bind session.
- **MEASURED (game log):** The slot-33 hook remained active at sampled `Present` boundaries through frame 24576. By then it had observed 120239 non-null DSV bind calls and 26 distinct view pointers. No non-null bind was observed through frame 4096; three 2560×1440 views appeared before frame 5120, and more formats and sizes appeared after frame 13312. The log does not mark the exact menu-to-world transition.
- **MEASURED (game log and Windows SDK `dxgiformat.h`):** The first three 2560×1440 views used `R32G8X24_TYPELESS` textures (19) and `D32_FLOAT_S8X24_UINT` DSVs (20). Later 2560×1440 views used `R24G8_TYPELESS` textures (44) and `D24_UNORM_S8_UINT` DSVs (45). Smaller targets included 256×256, 512×512, and 3072×3072 views; some used `R16_TYPELESS` (53) / `D16_UNORM` (55). Every logged target had bind flags `0x48` (`DEPTH_STENCIL | SHADER_RESOURCE`) and one sample.
- **LIMIT:** A bound DSV is a candidate, not verified scene depth. This run did not measure draw association, clear value, reversed/standard Z, valid depth contents, or a safe sampling point. The fixed 32-pointer table and possible pointer reuse also limit the census.

## Rejected depth-clear hook experiment — 2026-09-23

- **EXPERIMENTAL (Release WARP harness):** A temporary in-place `ClearDepthStencilView` (slot 53) hook reported successful installation at the first `DXGI_PRESENT_TEST`. A later `ClearDepthStencilView` call after a real `Present` never reached it. The harness measured slot 53 back at its original address by then, while the existing slot-33 DSV-bind hook still worked and was restored on swapchain release. Two test runs reproduced this; the first test also mistakenly placed the clear before the hook was installed and was corrected before the second run.
- **DECISION:** Revert the uncommitted clear-hook extension; it was never installed in Elite. The WARP result shows that a one-time in-place patch of slot 53 is insufficient in this harness. It does not establish that Elite rewrites the same slot, or who wrote the WARP slot. Further context hooks need explicit hook-loss handling or another validated dispatch strategy before deployment.

## DSV and color-target association — 2026-09-23

- **REFERENCE-CODE (EDVR revision `96df075`):** EDVR's `VTableHook::reclaim` distinguishes a runtime rewrite from a third-party hook that chains through its own callback. It does not blindly patch a displaced slot: doing so can create a recursive call loop. Its fast per-frame repair is limited to slots re-pointed by the module that implements them; other slots need call-traffic evidence. This is EDVR's design, not proof of the cause of EDPE's WARP slot-53 loss.
- **EXPERIMENTAL:** Extend the existing, game-tested slot-33 DSV-bind census to log the first RTV bound with each observed DSV. Log the first DSV bind separately if it has no RTV. Capture RTV count, first RTV pointer, texture dimensions, view format, and bind flags. Keep the 32-DSV limit and continue forwarding the game's call unchanged. No new context slot is patched.
- **VERIFIED (Release WARP tests):** Both proxy smoke tests passed. The test binds a DSV alone, then with a 64×64 RGBA RTV, and checks that both first-bind and first-color records are emitted.
- **LIMIT:** An RTV association is evidence of a render pass, not proof of the 3D scene depth. The first RTV may be absent or one of several color targets; the census still lacks clears, draws, depth contents, and a validated sampling point. No game observation has yet been made with this extension.
