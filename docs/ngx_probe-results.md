# NGX probe

Standalone probe of the NGX D3D11 lifecycle (Init, capability query, DLAA feature create/evaluate, shutdown). It runs in its own processes, never inside Elite, and never inside `EDPE.dll` or the proxy. `NgxContext` stays a disabled stub.

## Design

- `tools/ngx_probe/ngx_probe_host.cpp` (parent): launches `ngx_probe.exe` for one variant with a 30 s wall-clock timeout, terminates it on timeout, reads the child's log and prints a `SUMMARY` line with the last completed stage, exit state and exit code. No threads are used to hide a hang.
- `tools/ngx_probe/ngx_probe.cpp` (child): real NGX SDK calls only. It writes one flushed log line per completed stage: `elapsed_ms|stage|status|result_hex|detail`. A stage is `ok` only when the real call returned `NVSDK_NGX_Result_Success` (`0x1`); otherwise the actual code is logged as `fail` and the child stops.
- No stubs and no software devices. The first non-software DXGI adapter is used; with none, the probe aborts.
- `ngx_probe` links `nvsdk_ngx_d.lib` (x64, `/MD`) and nothing else links NGX. A post-build step copies `nvngx_dlss.dll` next to the exe, and the exe folder is passed to NGX through `PathListInfo`.
- Project ID: `NVSDK_NGX_D3D11_Init_with_ProjectID` (`nvsdk_ngx.h:246`) with an EDPE-owned GUID, `NVSDK_NGX_ENGINE_TYPE_CUSTOM` and the version string `EDPE-unreleased`. `NVSDK_NGX_D3D11_Init` (`nvsdk_ngx.h:150`) needs an NVIDIA-assigned `unsigned long long ApplicationId`, which EDPE does not have.
- Logs go to `ngx_probe_results/` next to the exe (under `build/`, not committed).

Stages: a create device, b NGX init, c capability parameters and DLSS availability, d optimal settings (DLAA, 2560x1440, render == output), e create feature, f evaluate on synthetic inputs, g release feature, h release parameters, i flush and sync, j shutdown, k release device.

Synthetic inputs for f: flat gray R8G8B8A8 color, constant R32_FLOAT depth 0.5, zero R16G16_FLOAT motion, zero jitter, `InReset=1`. Create flags: `MVLowRes | AutoExposure`.

Variants, one per process run, never combined:

- V1: `Shutdown1(device)` only
- V2: `Shutdown()` only (`NGX_ENABLE_DEPRECATED_SHUTDOWN`)
- V3: Flush and sync (event query, 5 s limit), then `Shutdown1`
- V4: `Shutdown1` before device release, device kept alive 2 s afterwards
- V5: skip stage f, otherwise V1

Stage i runs only in V3 and is logged `skip` in the other variants.

Run: `ngx_probe_host.exe --variant N --run K`.

## Measured results

Date: 2026-10-07.

Method: V1 to V5, each run twice as separate processes through `ngx_probe_host.exe` (Release build, MSVC, x64, `/MD`). Hardware: NVIDIA GeForce RTX 3060, driver 617.14 (UMD 32.0.16.1714), `nvngx_dlss.dll` 310.9.1.0, SDK API version `0x0000015`, D3D11 feature level 11.1.

Evidence (console output of this session; per-run logs `V<N>_run<K>.log` in `ngx_probe_results/`):

- V1 (2 runs): last completed k_release_device, clean exit, code 0x0, NGX codes b,c,d,e,f,g,h,j = 0x1, reproducible yes
- V2 (2 runs): last completed k_release_device, clean exit, code 0x0, NGX codes b,c,d,e,f,g,h,j = 0x1, reproducible yes
- V3 (2 runs): last completed k_release_device, clean exit, code 0x0, NGX codes b,c,d,e,f,g,h,j = 0x1, reproducible yes
- V4 (2 runs): last completed k_release_device, clean exit, code 0x0, NGX codes b,c,d,e,f,g,h,j = 0x1, reproducible yes
- V5 (2 runs): last completed k_release_device, clean exit, code 0x0, NGX codes b,c,d,e,g,h,j = 0x1 (f skipped), reproducible yes

Details seen in the V1 run 1 log: capability query reported `SuperSampling.Available=1` and `FeatureInitResult=0x1`; DLAA optimal settings for 2560x1440 returned optimal 2560x1440, max 2560x1440, min 2534x1426; NGX init took about 1.9 s; shutdown took about 27 ms; device release left 0 references. Every run finished in 1.8 to 4.5 s, far below the 30 s limit.

Confidence: MEASURED on one machine, two runs per variant. All five variants exited cleanly; no variant hung or crashed.

Implications:

- The earlier report of a hang of more than 20 s in `NVSDK_NGX_D3D11_Shutdown1` was not reproduced by this probe. No data from that earlier probe exists in the repo, so the cause of the difference is unknown.
- Stage f success shows that the evaluate call returned success on synthetic inputs. Output pixels were not inspected, and only V3 synchronized the GPU, so this does not validate image quality or Elite inputs.
- Nothing here shows that NGX is safe inside the Elite process. NGX is still not called there and `NgxContext` remains a disabled stub.
- Not yet tested: other drivers or GPUs, repeated init/shutdown cycles in one process, a device that has real swapchain and rendering load.
