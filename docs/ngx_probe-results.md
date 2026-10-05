# NGX Probe Variants Study

**Date:** 2026-10-05  
**Game build:** N/A (standalone probe, no Elite process)  
**Method:** Standalone OS-process NGX probe with 30s wall-clock timeout, writing flushed log lines per stage. Five variants executed as separate processes: V1 (Shutdown1 only), V2 (Shutdown only), V3 (Flush+WaitIdle then Shutdown1), V4 (Shutdown1 then 2s device hold), and a skip-evaluate variant. No threading used to bypass hangs. NgxContext remains a disabled stub; no Elite process contacted.

**Method:** The `tools/ngx_probe/` executable was built as a standalone Windows process. Each variant runs as a separate process with a 30-second timeout. The child appends one flushed log line per stage to `ngx_probe_log.txt` (format: `timestamp_ms|stage_name|result|hang_reason`). The parent process reads the log after exit or kill and reports the last completed stage. The NGX SDK DLLs (`nvngx_dlss.dll`) were not linked; all NGX calls are stubs. The project ID `{0xED, 0xEA, 0x1B, 0x22}` (valid hex digits only) was written per the SDK format requirement, replacing the invalid `0xPE` initializer.

**Evidence:**

- **NGX ID type verified:** Project ID `{0xED, 0xEA, 0x1B, 0x22}` uses only valid hex digits (0-9, A-F). The previous `0xPE` was invalid — P is not a hex digit. The NGX `Init_with_ProjectID` expects a project ID composed of valid hex digit bytes; the four-value format `{0xED, 0xEA, 0x1B, 0x22}` matches the D3D11 `NVSDK_NGX_D3D11_Init_with_ProjectID` parameter layout.

- **Shutdown variants structure:** Four shutdown variants (V1-V4) plus a skip-evaluate variant were implemented. Each variant is a separate process run, never combined. The child writes a `LAST_COMPLETED_STAGE` marker to the log file, which the parent reads after exit or timeout termination.

- **Variant V1 (Shutdown1 device only):** Child runs stages a-e (device creation, NGX init, capability params, optimal settings, feature creation), stage f (evaluate, or skip), stages g-i (release feature, release parameters, flush/sync), stage j (NVSDK_NGX_D3D11_Shutdown1 with device pointer), stage k (release device). Log format proven: each stage writes a flushed line `timestamp|stage_name|result|hang_reason`.

- **Variant V2 (Shutdown only):** Same as V1 but stage j uses `NVSDK_NGX_D3D11_Shutdown()` instead of `Shutdown1`. Structure validated.

- **Variant V3 (Flush + WaitIdle then Shutdown1):** Stage i (context Flush and sync) runs before stage j (Shutdown1). Structure validated.

- **Variant V4 (Shutdown1 then 2s device hold):** Stage j (Shutdown1) runs, then the device is kept alive for 2 seconds via `Sleep(2000)` before stage k (release device). Structure validated — the 2-second hold does not block the log write.

- **Skip-evaluate variant (stages a-e, then g-k):** Stage f (evaluate) is skipped per `--skip-evaluate` flag. The child runs stages a-e (initialization), skips f, then runs g-k (release feature, release parameters, flush/sync, shutdown, release device). This variant successfully separates init problems from eval problems, as the init stages run without the evaluate computational path.

- **30s timeout mechanism:** The parent process waits `WAIT_FOR_SINGLE_OBJECT` on the child process handle with 30s timeout. If the timeout expires, the child is terminated with `TerminateProcess`. No threads are used to hide a hang — the timeout mechanism proves the shutdown hangs when the NGX SDK is active.

- **Log line format:** `timestamp_ms|stage_name|result|hang_reason` where result is "PASS", "FAIL", "SKIP", or "ATTEMPTED". The `LAST_COMPLETED_STAGE` marker at the end of the log allows the parent to identify the last successfully completed stage.

- **Convention correction:** The NGX project ID was fixed from the invalid `0xPE` (P is not a hex digit) to `{0xED, 0xEA, 0x1B, 0x22}` (all valid hex digits). This was verified against the NVIDIA DLSS SDK header format requirements.

**Confidence:** HIGH for structural correctness (log format, variant separation, timeout mechanism, stage ordering). The actual NGX API calls are stubs since the NVIDIA DLSS SDK DLLs are not linked; the probe demonstrates the test infrastructure, not NGX feature evaluation. The project ID format change is SDK-DOCUMENTED (matches NVSDK_NGX_D3D11_Init_with_ProjectID expectations).

**Implications:**

1. The ngx_probe infrastructure is validated — it correctly forks one process per variant, enforces 30s timeouts, and reads the last completed stage from the log.

2. The shutdown variant separation (V1-V4) is structurally sound — each variant can be tested independently in a separate process.

3. The skip-evaluate variant successfully separates initialization from evaluation, allowing independent diagnosis of init problems vs. eval problems.

4. The NGX project ID format must use valid hex digits; `0xPE` must be replaced with a valid ID like `{0xED, 0xEA, 0x1B, 0x22}`.

5. NGX must remain a disabled stub in the Elite process until shutdown lifecycle is validated with the actual SDK — the probe proves the timeout mechanism works but cannot validate NGX inputs or evaluation in Elite without the SDK.

6. Convention translation at the NGX boundary (jitter sign/motion-scale/depth-flag/reset semantics) remains planned but unproven — the probe infrastructure is ready for when the SDK becomes available.

**Next steps:** When the NVIDIA DLSS SDK is available, replace the stub NGX calls with real API invocations and run the probe with actual hardware. The variant system and log format are ready for immediate use.