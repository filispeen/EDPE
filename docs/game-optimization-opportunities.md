# Additional Game Optimization Ideas (Outside PLAN.md)

Date: 2026-11-15

> Note: True Render Scale (Phase 10), D3D11/D3D12 interop (Phase 13), and Low Latency / Frame Pacing (Phase 17, sections 43 and 44) are already planned in [PLAN.md](../PLAN.md), so they are excluded here.

These ideas target the Elite Dangerous graphics pipeline through `dxgi.dll` and D3D11 hooks and are not currently on the roadmap.

## 1. Ideas outside the current plan

### Shader hash replacement and simplification

Identify expensive Cobra pixel or compute passes by shader hash from RenderDoc captures (such as `gameplay.rdc`) when those effects cannot be turned off cleanly in the game settings, including volumetric fog, shadows, SSAO, heavy bloom or blur, and reflections. Replace them with simpler or faster HLSL shaders.

### D3D11 state caching and redundant call filtering

In `ID3D11DeviceContext` hooks, skip duplicate `VSSetShader`, `PSSetShader`, `PSSetConstantBuffers`, `OMSetRenderTargets`, and sampler calls when the state has not changed. Drop unnecessary `CopyResource`, `ResolveSubresource`, and repeated `ClearRenderTargetView` or `ClearDepthStencilView` calls within a frame.

### Resource and streaming optimization (`CreateTexture2D` / `UpdateSubresource` / `Map`)

Intercept texture and buffer creation or updates to spot and remove CPU-GPU synchronization bottlenecks, such as `D3D11_MAP_READ` or suboptimal usage and staging copies in the game.

### Geometry and draw call reduction (experimental)

If profiling with RenderDoc or GPU timers shows a CPU bottleneck around stations or planetary settlements, test culling or simplifying small rendering passes at the D3D11 context level.

## 2. Constraints

- Do not optimize game logic, physics, networking, or system generation in `EliteDangerous64.exe` through the D3D11 proxy, as that risks instability and anti-cheat issues.
- Only implement an optimization after confirming the bottleneck in RenderDoc or a profiler, and keep fail-open fallback behavior.
