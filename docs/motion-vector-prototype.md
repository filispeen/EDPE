# GPU motion-vector prototype

**Date:** 2026-09-26

**Game build:** Not involved; standalone D3D11 WARP test.

**Status:** EXPERIMENTAL.

## Method and evidence

EDPE's first GPU motion shader reconstructs a world point from reversed-Z
depth and the current row-major view-to-world camera, projects it through
the previous camera, and writes current-to-previous motion in render pixels.
Positive Y points down. It targets `DXGI_FORMAT_R16G16_FLOAT` through a
render-target view; Microsoft lists render-target support for this format
among [required DXGI formats](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/required-dxgi-formats).
The test checks support at runtime with `CheckFormatSupport` as well.

**VERIFIED (Release WARP test):** A synthetic 4×4 depth texture uses Elite's
observed `R32G8X24_TYPELESS` resource format and an
`R32_FLOAT_X8X24_TYPELESS` shader-resource view. With `depthB=0.025`, raw
depth `0.0025` represents view Z=10. Moving the
current camera +10 units in X and Y while leaving the previous camera at
the origin produced exactly `(2,-2)` in the half-float output. A zero-depth
pixel produced `(0,0)`. All four project tests passed.

**VERIFIED (Release WARP state test):** The motion draw runs under a separate
`ID3DDeviceContextState`. After switching back, the test observes its prior
vertex shader, line topology, and viewport unchanged. Microsoft documents
[`SwapDeviceContextState`](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-swapdevicecontextstate)
as a way for plug-ins to save and restore application state on the immediate
context. The runtime path must check D3D11.1 availability and fail open.

**EXPERIMENTAL game capability probe:** On overlay initialization, EDPE now
queries the game's D3D11 device and immediate context for the 11.1 interfaces,
tries to create a same-feature-level context state, logs availability, and
releases the probe object. It does not activate that state or draw motion.
The WARP proxy smoke tests observe `available=1`; an Elite result is pending.

## Limits

The shader is used only by the standalone test. It does not run inside Elite,
read its DSV, modify its output, or establish the correct camera for every
depth-writing draw. Jitter is excluded. The in-game depth/camera pairing and
moving-cockpit measurements are in
[camera-reconstruction.md](camera-reconstruction.md). Runtime integration
still needs GPU-side camera capture, state-safe execution, a debug view, and
visual validation before any upscaler receives motion.
