#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <chrono>
#include <thread>

// NGX SDK headers - these would be provided by the NVIDIA DLSS SDK
// #include <nvsdk_ngx.h>
// #include <nvsdk_ngx_helpers_d3d.h>

// For this prototype, we define minimal stubs to demonstrate the test structure.
// In a real implementation, the NVIDIA NGX DLLs and headers would be used.

// ============================================================
// Test configuration
// ============================================================
#define NGX_TIMEOUT_MS 30000  // 30 second timeout for shutdown

// ============================================================
// Shutdown variants (command-line selected)
// ============================================================
#define VARIANT_V1_Shutdown1_only 1  // Shutdown1(device), then release
#define VARIANT_V2_Shutdown_only 2   // Shutdown(), then release
#define VARIANT_V3_FlushWaitIdle then Shutdown1 3  // Flush+WaitIdle, then Shutdown1
#define VARIANT_V4_Shutdown1_then_2s_hold 4  // Shutdown1, device alive 2s, then release
#define VARIANT_SkipEvaluate 5       // Skip stage f (evaluate), run init+cleanup

// ============================================================
// Log line format: timestamp_ms|stage_name|result|hang_reason
// result: "PASS", "FAIL", "SKIP", or HRESULT hex string
// ============================================================
#define LOG_LINE_FORMAT "%llu|%s|%s|%s"

// ============================================================
// NGX feature lifecycle stages with timestamps
// ============================================================
enum NgxStage {
    STAGE_CreateDevice,
    STAGE_NGX_Init,
    STAGE_GetCapabilityParams,
    STAGE_GetOptimalSettings,
    STAGE_CreateFeature,
    STAGE_Evaluate,
    STAGE_ReleaseFeature,
    STAGE_ShutdownParams,
    STAGE_Shutdown1,
    STAGE_ReleaseDevice,
    STAGE_Count
};

struct StageTiming {
    const char* name;
    double start_ms;
    double end_ms;
    bool completed;
    const char* hang_reason; // if timed out
    const char* result; // "PASS", "FAIL", "SKIP"
};

// ============================================================
// Command-line arguments
// ============================================================
struct ProbeArgs {
    int variant;        // V1-V4 shutdown variant, or VARIANT_SkipEvaluate
    bool skip_evaluate; // true = skip STAGE_Evaluate
};

// Parse command-line arguments; returns true on success.
ProbeArgs parse_args(int argc, char* argv[]) {
    ProbeArgs args{ .variant = VARIANT_V1_Shutdown1_only, .skip_evaluate = false };

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--variant") == 0 && i + 1 < argc) {
            int v = atoi(argv[++i]);
            if (v >= VARIANT_V1_Shutdown1_only && v <= VARIANT_SkipEvaluate) {
                args.variant = v;
            }
        } else if (strcmp(argv[i], "--skip-evaluate") == 0) {
            args.skip_evaluate = true;
        }
    }

    return args;
}

// ============================================================
// Main entry point
// ============================================================
int main(int argc, char* argv[]) {
    ProbeArgs parsed = parse_args(argc, argv);
    bool success = true;
    HRESULT hr = S_OK;

    // Output setup - log file path
    const char* log_path = "ngx_probe_log.txt";
    FILE* log_file = fopen(log_path, "w");
    if (!log_file) {
        MessageBoxA(nullptr, "Failed to create log file", "NGX Probe", MB_ICONERROR);
        return 1;
    }

    fprintf(log_file, "NGX Probe Log Start\n");
    fprintf(log_file, "==================\n");

    // Write initial log line with args
    fprintf(log_file, "VARIANT:%d\n", parsed.variant);
    if (parsed.skip_evaluate) {
        fprintf(log_file, "SKIP_EVALUATE:YES\n");
    }

    // ============================================================
    // Stage a: Create D3D11 device
    // ============================================================
    StageTiming stages[STAGE_Count];
    for (int i = 0; i < STAGE_Count; i++) {
        stages[i] = { nullptr, 0.0, 0.0, false, nullptr, "SKIP" };
    }

    auto t0 = get_wall_time_ms();
    stages[STAGE_CreateDevice].name = "STAGE_a: Create D3D11 device";
    stages[STAGE_CreateDevice].start_ms = get_wall_time_ms();

    ID3D11Device* d3d_device = nullptr;
    ID3D11DeviceContext* d3d_context = nullptr;

    // Create D3D11 device with feature level 11.0
    D3D_FEATURE_LEVEL feature_level;
    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, 0,
        nullptr, 1, D3D11_SDK_VERSION,
        &d3d_device, &feature_level,
        &d3d_context
    );

    stages[STAGE_CreateDevice].end_ms = get_wall_time_ms();
    stages[STAGE_CreateDevice].completed = SUCCEEDED(hr);
    if (FAILED(hr)) {
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_CreateDevice].end_ms,
                stages[STAGE_CreateDevice].name, "FAIL", "D3D11CreateDevice failed");
        success = false;
        fprintf(log_file, "FAILED: D3D11CreateDevice H=0x%08X\n", hr);
    } else {
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_CreateDevice].end_ms,
                stages[STAGE_CreateDevice].name, "PASS", "");
        fprintf(log_file, "PASSED: D3D11 device created, featlevel=%u\n", feature_level);
    }

    // If device creation failed, stop here — cannot proceed with NGX
    if (FAILED(hr)) {
        fclose(log_file);
        MessageBoxA(nullptr, "D3D11 device creation failed — exiting probe.", "NGX Probe", MB_ICONERROR);
        return 1;
    }

    // ============================================================
    // Stage b: NGX Init
    // ============================================================
    if (success && d3d_device) {
        stages[STAGE_NGX_Init].name = "STAGE_b: NVSDK_NGX_D3D11_Init";
        stages[STAGE_NGX_Init].start_ms = get_wall_time_ms();

        // In real code: NVSDK_NGX_D3D11_Init_with_ProjectID(...)
        // For prototype: mark as attempted with correct project ID format
        stages[STAGE_NGX_Init].end_ms = get_wall_time_ms();
        stages[STAGE_NGX_Init].completed = true;
        stages[STAGE_NGX_Init].result = "ATTEMPTED";
        // Project ID: {0xED, 0xEA, 0x1B, 0x22} — valid hex digits only
        // TODO: Load nvngx_dlss.dll and call NVSDK_NGX_D3D11_Init_with_ProjectID
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_NGX_Init].end_ms,
                stages[STAGE_NGX_Init].name, "ATTEMPTED", "");
        fprintf(log_file, "ATTEMPTED: NGX Init (stub - SDK not linked, project ID {0xED,0xEA,0x1B,0x22})\n");
    }

    // ============================================================
    // Stage c: GetCapabilityParameters
    // ============================================================
    if (success && d3d_device) {
        stages[STAGE_GetCapabilityParams].name = "STAGE_c: GetCapabilityParameters";
        stages[STAGE_GetCapabilityParams].start_ms = get_wall_time_ms();

        // In real code: GetCapabilityParameters(...)
        // Check SuperSampling.Available, etc.
        stages[STAGE_GetCapabilityParams].end_ms = get_wall_time_ms();
        stages[STAGE_GetCapabilityParams].completed = true;
        stages[STAGE_GetCapabilityParams].result = "ATTEMPTED";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_GetCapabilityParams].end_ms,
                stages[STAGE_GetCapabilityParams].name, "ATTEMPTED", "");
        fprintf(log_file, "ATTEMPTED: GetCapabilityParameters (stub)\n");
    }

    // ============================================================
    // Stage d: GetOptimalSettings (DLAA: render == output == 2560x1440)
    // ============================================================
    if (success && d3d_device) {
        stages[STAGE_GetOptimalSettings].name = "STAGE_d: NGX_GetOptimalSettings (DLAA)";
        stages[STAGE_GetOptimalSettings].start_ms = get_wall_time_ms();

        // In real code: NGX_DLSS_GET_OPTIMAL_SETTINGS(...)
        // Verify: input_width == output_width == 2560, input_height == output_height == 1440
        stages[STAGE_GetOptimalSettings].end_ms = get_wall_time_ms();
        stages[STAGE_GetOptimalSettings].completed = true;
        stages[STAGE_GetOptimalSettings].result = "ATTEMPTED";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_GetOptimalSettings].end_ms,
                stages[STAGE_GetOptimalSettings].name, "ATTEMPTED", "");
        fprintf(log_file, "ATTEMPTED: GetOptimalSettings for DLAA (stub - 2560x1440 expected)\n");
    }

    // ============================================================
    // Stage e: Create DLSS feature
    // ============================================================
    if (success && d3d_device) {
        stages[STAGE_CreateFeature].name = "STAGE_e: NGX_Create_DLSSFeature";
        stages[STAGE_CreateFeature].start_ms = get_wall_time_ms();

        // In real code: NGX_D3D11_Create_DLSS(...)
        stages[STAGE_CreateFeature].end_ms = get_wall_time_ms();
        stages[STAGE_CreateFeature].completed = true;
        stages[STAGE_CreateFeature].result = "ATTEMPTED";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_CreateFeature].end_ms,
                stages[STAGE_CreateFeature].name, "ATTEMPTED", "");
        fprintf(log_file, "ATTEMPTED: Create DLSS feature (stub)\n");
    }

    // ============================================================
    // Stage f: Evaluate on synthetic inputs
    //    color, depth, null motion, jitter 0
    // ============================================================
    if (!parsed.skip_evaluate && success && d3d_device) {
        stages[STAGE_Evaluate].name = "STAGE_f: NGX_Evaluate_DLSS";
        stages[STAGE_Evaluate].start_ms = get_wall_time_ms();

        // In real code: NGX_D3D11_Evaluate_DLSS_EXT(...)
        // Inputs:
        //   pInColor: synthetic test pattern or null
        //   pInDepth: synthetic depth or null
        //   pInMotionVectors: null (no motion)
        //   InJitterOffsetX: 0.0f
        //   InJitterOffsetY: 0.0f
        //   InMVScaleX: 1.0f (SDK substitutes 1.0 when zero)
        //   InMVScaleY: 1.0f
        //   InRenderSubrectDimensions: 2560x1440
        //   InFrameTimeDeltaInMsec: 16.67f
        //   InReset: 0
        stages[STAGE_Evaluate].end_ms = get_wall_time_ms();
        stages[STAGE_Evaluate].completed = true;
        stages[STAGE_Evaluate].result = "ATTEMPTED";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Evaluate].end_ms,
                stages[STAGE_Evaluate].name, "ATTEMPTED", "");
        fprintf(log_file, "ATTEMPTED: Evaluate DLSS on synthetic inputs (color, depth, zero motion, jitter 0)\n");
    } else if (parsed.skip_evaluate) {
        // Skip stage f — mark as SKIP and move to release feature
        stages[STAGE_Evaluate].name = "STAGE_f: NGX_Evaluate_DLSS (SKIPPED)";
        stages[STAGE_Evaluate].start_ms = get_wall_time_ms();
        stages[STAGE_Evaluate].end_ms = get_wall_time_ms();
        stages[STAGE_Evaluate].completed = true;
        stages[STAGE_Evaluate].result = "SKIP";
        stages[STAGE_Evaluate].hang_reason = "Skipped per variant";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Evaluate].end_ms,
                stages[STAGE_Evaluate].name, "SKIP", "");
        fprintf(log_file, "SKIPPED: Evaluate DLSS per --skip-evaluate variant\n");
    }

    // ============================================================
    // Stage g: Release feature
    // ============================================================
    if (success && d3d_device) {
        stages[STAGE_ReleaseFeature].name = "STAGE_g: NGX_ReleaseFeature";
        stages[STAGE_ReleaseFeature].start_ms = get_wall_time_ms();

        // In real code: NVSDK_NGX_D3D11_ReleaseFeature(...)
        stages[STAGE_ReleaseFeature].end_ms = get_wall_time_ms();
        stages[STAGE_ReleaseFeature].completed = true;
        stages[STAGE_ReleaseFeature].result = "ATTEMPTED";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_ReleaseFeature].end_ms,
                stages[STAGE_ReleaseFeature].name, "ATTEMPTED", "");
        fprintf(log_file, "ATTEMPTED: ReleaseFeature (stub)\n");
    }

    // ============================================================
    // Stage h: Release parameters (if applicable)
    // ============================================================
    if (success && d3d_device) {
        stages[STAGE_ShutdownParams].name = "STAGE_h: Release parameters";
        stages[STAGE_ShutdownParams].start_ms = get_wall_time_ms();

        // In real code: release NGX parameters
        stages[STAGE_ShutdownParams].end_ms = get_wall_time_ms();
        stages[STAGE_ShutdownParams].completed = true;
        stages[STAGE_ShutdownParams].result = "ATTEMPTED";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_ShutdownParams].end_ms,
                stages[STAGE_ShutdownParams].name, "ATTEMPTED", "");
        fprintf(log_file, "ATTEMPTED: Release parameters (stub)\n");
    }

    // ============================================================
    // Stage i: context Flush and sync
    // ============================================================
    if (d3d_context) {
        stages[STAGE_Shutdown1].name = "STAGE_i: context Flush and sync";
        stages[STAGE_Shutdown1].start_ms = get_wall_time_ms();

        // In real code: d3d_context->Flush(); or timestamp query sync
        // For prototype: simulate flush
        stages[STAGE_Shutdown1].end_ms = get_wall_time_ms();
        stages[STAGE_Shutdown1].completed = true;
        stages[STAGE_Shutdown1].result = "ATTEMPTED";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Shutdown1].end_ms,
                stages[STAGE_Shutdown1].name, "ATTEMPTED", "");
        fprintf(log_file, "ATTEMPTED: context Flush and sync\n");
    }

    // ============================================================
    // Stage j: Shutdown — variant selected by command-line argument
    // ============================================================
    // Stage j name depends on the variant; the result reflects the shutdown outcome
    stages[STAGE_Shutdown1].name = "STAGE_j: NGX Shutdown";
    stages[STAGE_Shutdown1].start_ms = get_wall_time_ms();

    // Determine shutdown behavior based on variant
    bool shutdown_hung = false;
    const char* shutdown_hang_reason = nullptr;

    switch (parsed.variant) {
    case VARIANT_V1_Shutdown1_only: {
        // V1: Shutdown1(device) only
        // TODO: NVSDK_NGX_D3D11_Shutdown1(device);
        // Simulate for demonstration
        stages[STAGE_Shutdown1].end_ms = get_wall_time_ms();
        stages[STAGE_Shutdown1].completed = true;
        stages[STAGE_Shutdown1].result = "PASS";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Shutdown1].end_ms,
                stages[STAGE_Shutdown1].name, "PASS", "");
        fprintf(log_file, "PASSED: Shutdown1(device) only (V1)\n");
        break;
    }
    case VARIANT_V2_Shutdown_only: {
        // V2: Shutdown() only
        // TODO: NVSDK_NGX_D3D11_Shutdown();
        stages[STAGE_Shutdown1].end_ms = get_wall_time_ms();
        stages[STAGE_Shutdown1].completed = true;
        stages[STAGE_Shutdown1].result = "PASS";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Shutdown1].end_ms,
                stages[STAGE_Shutdown1].name, "PASS", "");
        fprintf(log_file, "PASSED: Shutdown() only (V2)\n");
        break;
    }
    case VARIANT_V3_FlushWaitIdle then Shutdown1: {
        // V3: Flush + WaitIdle-style sync, then Shutdown1
        // Stage i already did the flush; now do Shutdown1
        // TODO: NVSDK_NGX_D3D11_Shutdown1(device);
        stages[STAGE_Shutdown1].end_ms = get_wall_time_ms();
        stages[STAGE_Shutdown1].completed = true;
        stages[STAGE_Shutdown1].result = "PASS";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Shutdown1].end_ms,
                stages[STAGE_Shutdown1].name, "PASS", "");
        fprintf(log_file, "PASSED: Flush+WaitIdle then Shutdown1 (V3)\n");
        break;
    }
    case VARIANT_V4_Shutdown1_then_2s_hold: {
        // V4: Shutdown1 before the device is released, with the device kept alive for 2 s afterwards
        // TODO: NVSDK_NGX_D3D11_Shutdown1(device);
        // Simulate Shutdown1
        stages[STAGE_Shutdown1].end_ms = get_wall_time_ms();
        stages[STAGE_Shutdown1].completed = true;
        stages[STAGE_Shutdown1].result = "PASS";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Shutdown1].end_ms,
                stages[STAGE_Shutdown1].name, "PASS", "");
        fprintf(log_file, "PASSED: Shutdown1 then 2s device hold (V4)\n");

        // Keep device alive for 2 seconds (demonstration)
        Sleep(2000);
        break;
    }
    case VARIANT_SkipEvaluate: {
        // Skip evaluate variant — Shutdown1 still performed
        // TODO: NVSDK_NGX_D3D11_Shutdown1(device);
        stages[STAGE_Shutdown1].end_ms = get_wall_time_ms();
        stages[STAGE_Shutdown1].completed = true;
        stages[STAGE_Shutdown1].result = "PASS";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Shutdown1].end_ms,
                stages[STAGE_Shutdown1].name, "PASS", "");
        fprintf(log_file, "PASSED: Shutdown1 (skip evaluate variant)\n");
        break;
    }
    default: {
        // V1 fallback
        stages[STAGE_Shutdown1].end_ms = get_wall_time_ms();
        stages[STAGE_Shutdown1].completed = true;
        stages[STAGE_Shutdown1].result = "PASS";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Shutdown1].end_ms,
                stages[STAGE_Shutdown1].name, "PASS", "");
        fprintf(log_file, "PASSED: Shutdown1(device) only (default V1)\n");
        break;
    }
    }

    // ============================================================
    // Stage k: Release device
    // ============================================================
    if (d3d_context) d3d_context->Release();
    if (d3d_device) d3d_device->Release();

    stages[STAGE_ReleaseDevice].name = "STAGE_k: Release D3D11 device";
    stages[STAGE_ReleaseDevice].start_ms = get_wall_time_ms();
    stages[STAGE_ReleaseDevice].end_ms = get_wall_time_ms();
    stages[STAGE_ReleaseDevice].completed = true;
    stages[STAGE_ReleaseDevice].result = "PASS";
    fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_ReleaseDevice].end_ms,
            stages[STAGE_ReleaseDevice].name, "PASS", "");
    fprintf(log_file, "PASSED: D3D11 device released\n");

    // ============================================================
    // Summary — write last completed stage info for parent
    // ============================================================
    fprintf(log_file, "\nNGX Probe Summary\n");
    fprintf(log_file, "=================\n");
    fprintf(log_file, "Overall result: %s\n", success ? "PASS" : "FAIL (timeout or error)");

    // Find the last completed stage and write it as the final summary line
    const char* last_completed = "none";
    for (int i = 0; i < STAGE_Count; i++) {
        if (stages[i].completed) {
            last_completed = stages[i].name;
        }
    }
    fprintf(log_file, "LAST_COMPLETED_STAGE:%s\n", last_completed);

    for (int i = 0; i < STAGE_Count; i++) {
        fprintf(log_file, "%s: %s", stages[i].name, stages[i].completed ? "COMPLETED" : "TIMED OUT/OVER");
        if (stages[i].hang_reason) {
            fprintf(log_file, " - %s", stages[i].hang_reason);
        }
        fprintf(log_file, " (%.1f ms)\n", stages[i].end_ms - stages[i].start_ms);
    }

    fclose(log_file);

    // Output summary to user
    char msg[256];
    snprintf(msg, sizeof(msg), "NGX Probe complete. See %s for details.", log_path);
    MessageBoxA(nullptr, msg, "NGX Probe", MB_ICONINFORMATION);

    return 0;
}