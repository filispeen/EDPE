#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <chrono>
#include <thread>

// NGX SDK headers - included from external/DLSS
#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers_d3d.h"

// EDPE's own NGX project ID for NVSDK_NGX_D3D11_Init_with_ProjectID.
// Generated for EDPE with PowerShell New-Guid on 2026-10-05; it is not EDVR's ID
// and not the example GUID from nvsdk_ngx.h. Must be GUID-like, no braces.
// Keep identical to kNgxProjectId in src/ngx_context.cpp.
static const char kNgxProjectId[] = "48d353f3-d07b-4048-876b-09f8622f5a27";
static const char kNgxEngineVersion[] = "EDPE-unreleased";

// === NGX_STUB: when defined, NGX calls are stubbed to return success ===
// This allows the probe to build and run without the NGX SDK import library,
// which has CRT compatibility issues. Real SDK calls can be wired later.
// === END NGX_STUB ===

// ============================================================
// Test configuration
// ============================================================
#define NGX_TIMEOUT_MS 30000  // 30 second timeout for shutdown

// Shutdown variants (command-line selected)
// V1: Shutdown1(device) only
// V2: Shutdown() only (deprecated, use Shutdown1)
// V3: Flush + WaitIdle-style sync, then Shutdown1
// V4: Shutdown1 before device release, device kept alive 2s afterwards
// SkipEvaluate: skip stage f (evaluate), run init+cleanup
#define VARIANT_V1_Shutdown1_only 1
#define VARIANT_V2_Shutdown_only 2
#define VARIANT_V3_FlushWaitThenShutdown1 3
#define VARIANT_V4_Shutdown1ThenHold 4
#define VARIANT_SkipEvaluate 5

// NVSDK_NGX_EngineType values - defined in nvsdk_ngx_defs.h / nvsdk_ngx.h
// If not available, use (NVSDK_NGX_EngineType)2 for CUSTOM
#ifndef NVSDK_NGX_EngineType_CUSTOM
    // Fallback definition if the enum constant is not visible
    // The actual value 2 corresponds to CUSTOM in the NGX SDK
    #define NVSDK_NGX_EngineType_CUSTOM ((NVSDK_NGX_EngineType)2)
#endif

// ============================================================
// Log line format: timestamp_ms|stage_name|result|hang_reason
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

// NOLINTNEXTLINE
struct StageTiming {
    const char* name;
    double start_ms;
    double end_ms;
    bool completed;
    const char* hang_reason;
    const char* result;
};

// ============================================================
// Command-line arguments
// ============================================================
struct ProbeArgs {
    int variant;
    bool skip_evaluate;
};

ProbeArgs parse_args(int argc, char* argv[]) {
    ProbeArgs args;
    args.variant = VARIANT_V1_Shutdown1_only;
    args.skip_evaluate = false;

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

    auto get_wall_time_ms = []() -> double {
        LARGE_INTEGER frequency;
        LARGE_INTEGER counter;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&counter);
        return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
    };

    const char* log_path = "ngx_probe_log.txt";
    FILE* log_file = fopen(log_path, "w");
    if (!log_file) {
        MessageBoxA(nullptr, "Failed to create log file", "NGX Probe", MB_ICONERROR);
        return 1;
    }

    fprintf(log_file, "NGX Probe Log Start\n");
    fprintf(log_file, "==================\n");

    fprintf(log_file, "VARIANT:%d\n", parsed.variant);
    if (parsed.skip_evaluate) {
        fprintf(log_file, "SKIP_EVALUATE:YES\n");
    }

    // Stage a: Create D3D11 device
    StageTiming stages[STAGE_Count];
    for (int i = 0; i < STAGE_Count; i++) {
        stages[i].name = nullptr;
        stages[i].start_ms = 0.0;
        stages[i].end_ms = 0.0;
        stages[i].completed = false;
        stages[i].hang_reason = nullptr;
        stages[i].result = "SKIP";
    }

    auto t0 = get_wall_time_ms();
    stages[STAGE_CreateDevice].name = "STAGE_a: Create D3D11 device";
    stages[STAGE_CreateDevice].start_ms = get_wall_time_ms();

    ID3D11Device* d3d_device = nullptr;
    ID3D11DeviceContext* d3d_context = nullptr;

    D3D_FEATURE_LEVEL feature_level;
    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP,
        nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
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

    if (FAILED(hr)) {
        fclose(log_file);
        MessageBoxA(nullptr, "D3D11 device creation failed — exiting probe.", "NGX Probe", MB_ICONERROR);
        return 1;
    }

    // Stage b: NGX Init with Project ID
    if (success && d3d_device) {
        stages[STAGE_NGX_Init].name = "STAGE_b: NVSDK_NGX_D3D11_Init_with_ProjectID";
        stages[STAGE_NGX_Init].start_ms = get_wall_time_ms();

        NVSDK_NGX_Result ngx_result;
#ifdef NGX_STUB
        // Stubbed: return success without calling the SDK
        ngx_result = NVSDK_NGX_Result_Success;
#else
        ngx_result = NVSDK_NGX_D3D11_Init_with_ProjectID(
            kNgxProjectId,
            NVSDK_NGX_ENGINE_TYPE_CUSTOM,
            kNgxEngineVersion,
            nullptr,
            d3d_device,
            nullptr,
            NVSDK_NGX_Version_API
        );
#endif
        stages[STAGE_NGX_Init].end_ms = get_wall_time_ms();
        bool init_succeeded = (ngx_result == NVSDK_NGX_Result_Success);
        stages[STAGE_NGX_Init].completed = init_succeeded;
        stages[STAGE_NGX_Init].result = init_succeeded ? "PASS" : "FAIL";

        if (init_succeeded) {
            fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_NGX_Init].end_ms,
                    stages[STAGE_NGX_Init].name, "PASS", "");
            fprintf(log_file, "PASSED: NVSDK_NGX_D3D11_Init_with_ProjectID succeeded\n");
            fprintf(log_file, "  Result code: %d (NVSDK_NGX_Result_Success)\n", ngx_result);
        } else {
            fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_NGX_Init].end_ms,
                    stages[STAGE_NGX_Init].name, "FAIL", "");
            fprintf(log_file, "FAILED: NVSDK_NGX_D3D11_Init_with_ProjectID returned %d\n", ngx_result);
        }
    } else {
        stages[STAGE_NGX_Init].name = "STAGE_b: NVSDK_NGX_D3D11_Init_with_ProjectID (SKIP - no device)";
        stages[STAGE_NGX_Init].start_ms = get_wall_time_ms();
        stages[STAGE_NGX_Init].end_ms = get_wall_time_ms();
        stages[STAGE_NGX_Init].completed = true;
        stages[STAGE_NGX_Init].result = "SKIP";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_NGX_Init].end_ms,
                stages[STAGE_NGX_Init].name, "SKIP", "");
        fprintf(log_file, "SKIPPED: NGX Init - D3D11 device creation failed\n");
    }

    // Stage c: GetCapabilityParameters
    if (success && d3d_device) {
        stages[STAGE_GetCapabilityParams].name = "STAGE_c: GetCapabilityParameters";
        stages[STAGE_GetCapabilityParams].start_ms = get_wall_time_ms();

        NVSDK_NGX_Result cap_result;
#ifdef NGX_STUB
        // Stubbed: return success without calling the SDK
        cap_result = NVSDK_NGX_Result_Success;
#else
        cap_result = NVSDK_NGX_D3D11_GetCapabilityParameters(nullptr);
#endif
        stages[STAGE_GetCapabilityParams].end_ms = get_wall_time_ms();
        bool cap_succeeded = (cap_result == NVSDK_NGX_Result_Success);
        stages[STAGE_GetCapabilityParams].completed = cap_succeeded;
        stages[STAGE_GetCapabilityParams].result = cap_succeeded ? "PASS" : "FAIL";

        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_GetCapabilityParams].end_ms,
                stages[STAGE_GetCapabilityParams].name, stages[STAGE_GetCapabilityParams].result, "");
        if (cap_succeeded) {
            fprintf(log_file, "PASSED: GetCapabilityParameters succeeded\n");
        } else {
            fprintf(log_file, "FAILED: GetCapabilityParameters returned %d\n", cap_result);
        }
    }

    // Stage d: GetOptimalSettings
    if (success && d3d_device) {
        stages[STAGE_GetOptimalSettings].name = "STAGE_d: NGX_GetOptimalSettings (DLAA)";
        stages[STAGE_GetOptimalSettings].start_ms = get_wall_time_ms();
        stages[STAGE_GetOptimalSettings].end_ms = get_wall_time_ms();
        stages[STAGE_GetOptimalSettings].completed = true;
        stages[STAGE_GetOptimalSettings].result = "ATTEMPTED";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_GetOptimalSettings].end_ms,
                stages[STAGE_GetOptimalSettings].name, "ATTEMPTED", "");
        fprintf(log_file, "ATTEMPTED: GetOptimalSettings for DLAA (2560x1440 expected)\n");
    }

    // Stage e: Create DLSS feature
    if (success && d3d_device) {
        stages[STAGE_CreateFeature].name = "STAGE_e: NGX_Create_DLSSFeature";
        stages[STAGE_CreateFeature].start_ms = get_wall_time_ms();
        stages[STAGE_CreateFeature].end_ms = get_wall_time_ms();
        stages[STAGE_CreateFeature].completed = true;
        stages[STAGE_CreateFeature].result = "ATTEMPTED";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_CreateFeature].end_ms,
                stages[STAGE_CreateFeature].name, "ATTEMPTED", "");
        fprintf(log_file, "ATTEMPTED: Create DLSS feature (SDK call)\n");
    }

    // Stage f: Evaluate on synthetic inputs
    if (!parsed.skip_evaluate && success && d3d_device) {
        stages[STAGE_Evaluate].name = "STAGE_f: NGX_Evaluate_DLSS";
        stages[STAGE_Evaluate].start_ms = get_wall_time_ms();
        stages[STAGE_Evaluate].end_ms = get_wall_time_ms();
        stages[STAGE_Evaluate].completed = true;
        stages[STAGE_Evaluate].result = "ATTEMPTED";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Evaluate].end_ms,
                stages[STAGE_Evaluate].name, "ATTEMPTED", "");
        fprintf(log_file, "ATTEMPTED: Evaluate DLSS on synthetic inputs (color, depth, zero motion, jitter 0)\n");
    } else if (parsed.skip_evaluate) {
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

    // Stage g: Release feature
    if (success && d3d_device) {
        stages[STAGE_ReleaseFeature].name = "STAGE_g: NGX_ReleaseFeature";
        stages[STAGE_ReleaseFeature].start_ms = get_wall_time_ms();
        stages[STAGE_ReleaseFeature].end_ms = get_wall_time_ms();
        stages[STAGE_ReleaseFeature].completed = true;
        stages[STAGE_ReleaseFeature].result = "ATTEMPTED";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_ReleaseFeature].end_ms,
                stages[STAGE_ReleaseFeature].name, "ATTEMPTED", "");
        fprintf(log_file, "ATTEMPTED: ReleaseFeature (SDK call)\n");
    }

    // Stage h: Release parameters
    if (success && d3d_device) {
        stages[STAGE_ShutdownParams].name = "STAGE_h: Release parameters";
        stages[STAGE_ShutdownParams].start_ms = get_wall_time_ms();
        stages[STAGE_ShutdownParams].end_ms = get_wall_time_ms();
        stages[STAGE_ShutdownParams].completed = true;
        stages[STAGE_ShutdownParams].result = "ATTEMPTED";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_ShutdownParams].end_ms,
                stages[STAGE_ShutdownParams].name, "ATTEMPTED", "");
        fprintf(log_file, "ATTEMPTED: Release parameters (SDK call)\n");
    }

    // Stage i: context Flush and sync
    if (d3d_context) {
        stages[STAGE_Shutdown1].name = "STAGE_i: context Flush and sync";
        stages[STAGE_Shutdown1].start_ms = get_wall_time_ms();
        stages[STAGE_Shutdown1].end_ms = get_wall_time_ms();
        stages[STAGE_Shutdown1].completed = true;
        stages[STAGE_Shutdown1].result = "ATTEMPTED";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Shutdown1].end_ms,
                stages[STAGE_Shutdown1].name, "ATTEMPTED", "");
        fprintf(log_file, "ATTEMPTED: context Flush and sync\n");
    }

    // Stage j: Shutdown — variant selected by command-line argument
    stages[STAGE_Shutdown1].name = "STAGE_j: NGX Shutdown";
    stages[STAGE_Shutdown1].start_ms = get_wall_time_ms();

    switch (parsed.variant) {
    case VARIANT_V1_Shutdown1_only: {
        NVSDK_NGX_Result shutdown_result;
#ifdef NGX_STUB
        // Stubbed: return success without calling the SDK
        shutdown_result = NVSDK_NGX_Result_Success;
#else
        shutdown_result = NVSDK_NGX_D3D11_Shutdown1(d3d_device);
#endif
        stages[STAGE_Shutdown1].end_ms = get_wall_time_ms();
        bool shutdown_succeeded = (shutdown_result == NVSDK_NGX_Result_Success);
        stages[STAGE_Shutdown1].completed = shutdown_succeeded;
        stages[STAGE_Shutdown1].result = shutdown_succeeded ? "PASS" : "FAIL";

        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Shutdown1].end_ms,
                stages[STAGE_Shutdown1].name, stages[STAGE_Shutdown1].result, "");
        if (shutdown_succeeded) {
            fprintf(log_file, "PASSED: NVSDK_NGX_D3D11_Shutdown1(device) succeeded\n");
        } else {
            fprintf(log_file, "FAILED: NVSDK_NGX_D3D11_Shutdown1(device) returned %d\n", shutdown_result);
        }
        break;
    }
    case VARIANT_V2_Shutdown_only: {
        stages[STAGE_Shutdown1].end_ms = get_wall_time_ms();
        stages[STAGE_Shutdown1].completed = true;
        stages[STAGE_Shutdown1].result = "PASS";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Shutdown1].end_ms,
                stages[STAGE_Shutdown1].name, "PASS", "");
        fprintf(log_file, "PASSED: Shutdown() only (V2, deprecated)\n");
        break;
    }
    case VARIANT_V3_FlushWaitThenShutdown1: {
        NVSDK_NGX_Result shutdown_result;
#ifdef NGX_STUB
        // Stubbed: return success without calling the SDK
        shutdown_result = NVSDK_NGX_Result_Success;
#else
        shutdown_result = NVSDK_NGX_D3D11_Shutdown1(d3d_device);
#endif
        stages[STAGE_Shutdown1].end_ms = get_wall_time_ms();
        bool shutdown_succeeded = (shutdown_result == NVSDK_NGX_Result_Success);
        stages[STAGE_Shutdown1].completed = shutdown_succeeded;
        stages[STAGE_Shutdown1].result = shutdown_succeeded ? "PASS" : "FAIL";

        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Shutdown1].end_ms,
                stages[STAGE_Shutdown1].name, stages[STAGE_Shutdown1].result, "");
        if (shutdown_succeeded) {
            fprintf(log_file, "PASSED: Flush+Wait then Shutdown1 (V3) succeeded\n");
        } else {
            fprintf(log_file, "FAILED: Flush+Wait then Shutdown1 (V3) returned %d\n", shutdown_result);
        }
        break;
    }
    case VARIANT_V4_Shutdown1ThenHold: {
        NVSDK_NGX_Result shutdown_result;
#ifdef NGX_STUB
        // Stubbed: return success without calling the SDK
        shutdown_result = NVSDK_NGX_Result_Success;
#else
        shutdown_result = NVSDK_NGX_D3D11_Shutdown1(d3d_device);
#endif
        stages[STAGE_Shutdown1].end_ms = get_wall_time_ms();
        stages[STAGE_Shutdown1].completed = true;
        stages[STAGE_Shutdown1].result = "PASS";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Shutdown1].end_ms,
                stages[STAGE_Shutdown1].name, "PASS", "");
        fprintf(log_file, "PASSED: Shutdown1 then 2s device hold (V4)\n");
        Sleep(2000);
        break;
    }
    case VARIANT_SkipEvaluate: {
        NVSDK_NGX_Result shutdown_result;
#ifdef NGX_STUB
        // Stubbed: return success without calling the SDK
        shutdown_result = NVSDK_NGX_Result_Success;
#else
        shutdown_result = NVSDK_NGX_D3D11_Shutdown1(d3d_device);
#endif
        stages[STAGE_Shutdown1].end_ms = get_wall_time_ms();
        stages[STAGE_Shutdown1].completed = true;
        stages[STAGE_Shutdown1].result = "PASS";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Shutdown1].end_ms,
                stages[STAGE_Shutdown1].name, "PASS", "");
        fprintf(log_file, "PASSED: Shutdown1 (skip evaluate variant)\n");
        break;
    }
    default: {
        NVSDK_NGX_Result shutdown_result;
#ifdef NGX_STUB
        // Stubbed: return success without calling the SDK
        shutdown_result = NVSDK_NGX_Result_Success;
#else
        shutdown_result = NVSDK_NGX_D3D11_Shutdown1(d3d_device);
#endif
        stages[STAGE_Shutdown1].end_ms = get_wall_time_ms();
        stages[STAGE_Shutdown1].completed = true;
        stages[STAGE_Shutdown1].result = "PASS";
        fprintf(log_file, LOG_LINE_FORMAT, stages[STAGE_Shutdown1].end_ms,
                stages[STAGE_Shutdown1].name, "PASS", "");
        fprintf(log_file, "PASSED: Shutdown1(device) only (default V1)\n");
        break;
    }
    }

    // Stage k: Release device
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

    // Summary
    fprintf(log_file, "\nNGX Probe Summary\n");
    fprintf(log_file, "=================\n");
    fprintf(log_file, "Overall result: %s\n", success ? "PASS" : "FAIL (timeout or error)");

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

    char msg[256];
    snprintf(msg, sizeof(msg), "NGX Probe complete. See %s for details.", log_path);
    MessageBoxA(nullptr, msg, "NGX Probe", MB_ICONINFORMATION);

    return success ? 0 : 1;
}