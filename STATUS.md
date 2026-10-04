# EDPE Project Status

**Status reviewed:** 2026-10-05

**Roadmap:** [`PLAN.md`](PLAN.md)

**Evidence:** [`docs/render-pipeline-observations.md`](docs/render-pipeline-observations.md) and the linked technical notes.

## Summary

EDPE has a working, experimentally deployed D3D11/DXGI proxy and an in-game Dear ImGui diagnostics overlay. The game has been observed launching and rendering normally with the proxy loaded. Resource, depth, camera, projection, and motion investigations have produced useful captures and prototype code.

**No temporal upscaler is currently operational.** Elite is still rendered and presented through its original native-resolution path. Projection jitter is not applied in-game; generated motion is diagnostic and manually triggered; NGX is deliberately disabled. There is no FSR backend, render-scale control, HUD separation, or Frame Generation path.

The stage labels below describe implementation and evidence in the repository. They do not imply that every item in a roadmap phase is complete.

## Roadmap phase status

| Phase | Status | Current state |
|---|---|---|
| 0. Reconnaissance | **Substantially complete; ongoing** | The target is Elite Dangerous Odyssey's 2D D3D11 path. The target executable imports, swapchain, backbuffer format, proxy behavior, and several renderer resources have been investigated and documented. Other graphics configurations, complete resource lifetime coverage, and long-session behavior remain unverified. |
| 1. D3D11 injection | **Working prototype; limited coverage** | `EDPE.dll` forwards D3D11 device creation; the `dxgi.dll` shim observes factories, swapchains, and `Present`; logging and the F5 Win32/DX11 ImGui overlay work in the documented local game test. Only the observed exports are covered. Coexistence with other graphics mods and long-session stability are unverified. |
| 2. Resource census | **Partial** | D3D11 context instrumentation records render/depth target binds, draw-related state, shader information, and selected captures. The roadmap's full inventory of resource creation, copies, dispatches, viewports, lifetimes, and call relationships is not complete. |
| 3. Scene depth | **Candidate identified; not production-validated** | Depth candidates have been captured and inspected alongside scene output. Per-pixel association, clear/far behavior, Z direction, near/far behavior, safe sampling point, and validity across scenes are not established well enough for a vendor backend. |
| 4. Camera and projection | **Experimental** | Camera constant-buffer layouts and projection usage have been inspected, with CPU parsing and GPU-copy prototypes. The chosen camera/depth association is based on observed passes and sparse samples; it is not verified for every depth-writing draw, scene, or transition. |
| 5. Projection jitter | **Math prototype only; runtime use blocked** | Halton jitter and projection-block transformation helpers exist. Jitter is intentionally not applied in Elite because full shader/pass coverage and a guaranteed fail-open path that removes jitter have not been proven. |
| 6. Motion vectors | **Diagnostic prototype; not backend-ready** | A D3D11 GPU motion pass and CPU math have synthetic/WARP evidence, and sparse in-game diagnostic captures show finite output during camera movement. Nearby geometry, per-pixel depth association, moving objects, scene cuts, stable camera selection, and vendor-specific direction/scale conventions still need validation. The capture path is explicitly experimental/manual. |
| 7. Shared temporal input layer | **Not implemented** | There is no backend-neutral `TemporalFrameInputs` handoff or production history/reset/resource-lifetime manager. Current color, depth, camera, and motion observations are separate diagnostics. |
| 8. NVIDIA DLAA | **Blocked / not implemented** | `NgxContext` is a disabled stub. A standalone SDK probe reportedly initialized and queried capabilities, but hung during NGX shutdown; NGX is therefore not run in the game. The Elite input conventions and safe jitter rollback are also unresolved. |
| 9. NVIDIA DLSS Super Resolution | **Not started** | No evaluation path is implemented. |
| 10. True render-scale control | **Not started** | No Cobra scaling mechanism or selective scene-resource scaling has been implemented. The game still renders the expensive scene at native resolution. |
| 11. AMD FSR upscaling | **Not started** | No FidelityFX backend is implemented. The project notes compare the D3D11 community route with an official API/interoperability route, but no production choice or evaluation exists. |
| 12. Native-resolution HUD | **Investigation only** | Captured scene color includes HUD content. HUD/world classification and a native-resolution composition path are not implemented. |
| 13. D3D11/D3D12 interoperability | **Not started** | No shared-resource/fence experiment or overhead measurements are documented. |
| 14. AMD Frame Generation | **Not started** | No AMD FG or presentation integration exists. |
| 15. NVIDIA Streamline / DLSS Frame Generation | **Not started** | No Streamline, DLSS-G, or Reflex integration exists. |

## Available diagnostics and controls

- The overlay is toggled with **F5** and reports the original rendering path. Temporal options remain disabled.
- Experimental F5 actions can capture selected scene/depth/camera/motion candidates. Captures are diagnostic evidence, not production temporal inputs.
- The proxy writes `edpe.log` beside the host executable. Manual captures may also save visual BMP screenshots under `edpe-captures`; those images are not raw depth or motion data.
- The repository defines CPU math, WARP motion, DXBC fanout, and proxy smoke tests. Earlier project notes record successful runs, but tests were not rerun for this status review.
- Current source contains `0xPE` in `src/ngx_context.cpp` as an initializer. It is not a valid C++ numeric literal and may prevent a fresh build. Existing binaries or earlier test results do not establish that the current checkout builds; correct and rebuild before treating the current revision as build-verified.

## Next milestone

The next roadmap milestone is a safe, native-resolution **NVIDIA DLAA** vertical slice, but implementation should wait for two prerequisites:

1. Resolve and document the NGX shutdown/lifecycle behavior so the game process can initialize and exit safely.
2. Validate the selected color, depth, camera, and motion resources and conventions in Elite, and prove that any applied jitter can always be removed when evaluation fails.

Then formalize the shared temporal input boundary, implement DLAA with input and output dimensions equal, and validate cockpit, station, space, camera movement, HUD, moving ships, reset behavior, fallback, and GPU cost. Only after that should reduced-resolution rendering and DLSS SR begin.

## Not yet demonstrated

EDPE has not demonstrated lower scene render resolution, temporal image reconstruction, frame-time improvement, stable multi-scene history, vendor backend switching, or generated-frame pacing/latency. These remain roadmap goals rather than current capabilities.
