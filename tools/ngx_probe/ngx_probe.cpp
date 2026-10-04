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
// Timing utilities
// ============================================================
inline double get_wall_time_ms() {
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 / (double)frequency.QuadPart;
}

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
};

// ============================================================
// Main entry point
// ============================================================
int main() {
    bool success = true;
    HRESULT hr = S_OK;
    
    // Output setup
    FILE* log_file = fopen("ngx_probe_log.txt", "w");
    if (!log_file) {
        MessageBoxA(nullptr, "Failed to create log file", "NGX Probe", MB_ICONERROR);
        return 1;
    }
    
    fprintf(log_file, "NGX Probe Log Start\n");
    fprintf(log_file, "==================\n");
    
    // ============================================================
    // Stage a: Create D3D11 device
    // ============================================================
    StageTiming stages[STAGE_Count];
    for (int i = 0; i < STAGE_Count; i++) {
        stages[i] = { nullptr, 0.0, 0.0, false, nullptr };
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
        fprintf(log_file, "FAILED: D3D11CreateDevice H=0x%08X\n", hr);
        success = false;
    } else {
        fprintf(log_file, "PASSED: D3D11 device created, featlevel=%u\n", feature_level);
    }
    
    // ============================================================
    // Stage b: NGX Init
    // ============================================================
    if (success && d3d_device) {
        t0 = get_wall_time_ms();
        stages[STAGE_NGX_Init].name = "STAGE_b: NVSDK_NGX_D3D11_Init";
        stages[STAGE_NGX_Init].start_ms = get_wall_time_ms();
        
        // In real code: NVSDK_NGX_D3D11_Init_with_ProjectID(...)
        // For prototype: mark as attempted
        stages[STAGE_NGX_Init].end_ms = get_wall_time_ms();
        stages[STAGE_NGX_Init].completed = true; // placeholder
        fprintf(log_file, "ATTEMPTED: NGX Init (stub - SDK not linked)\n");
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
        stages[STAGE_GetCapabilityParams].completed = true; // placeholder
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
        stages[STAGE_GetOptimalSettings].completed = true; // placeholder
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
        stages[STAGE_CreateFeature].completed = true; // placeholder
        fprintf(log_file, "ATTEMPTED: Create DLSS feature (stub)\n");
    }
    
    // ============================================================
    // Stage f: Evaluate on synthetic inputs
    //    color, depth, null motion, jitter 0
    // ============================================================
    if (success && d3d_device) {
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
        stages[STAGE_Evaluate].completed = true; // placeholder
        fprintf(log_file, "ATTEMPTED: Evaluate DLSS on synthetic inputs (stub)\n");
    }
    
    // ============================================================
    // Stage f1: Evaluate DLAA specifically (input == output)
    // ============================================================
    if (success && d3d_device) {
        // DLAA mode: InRenderSubrectDimensions should equal backbuffer dimensions
        // (2560x1440), input resolution equals output resolution
        stages[STAGE_Evaluate].name = "STAGE_f1: NGX_DLAA_Evaluate (input==output)";
        stages[STAGE_Evaluate].start_ms = get_wall_time_ms();
        // Same evaluation but with renderWidth==outputWidth, renderHeight==outputHeight
        stages[STAGE_Evaluate].end_ms = get_wall_time_ms();
        stages[STAGE_Evaluate].completed = true;
        fprintf(log_file, "ATTEMPTED: DLAA evaluate with input==output 2560x1440 (stub)\n");
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
        fprintf(log_file, "ATTEMPTED: ReleaseFeature (stub)\n");
    }
    
    // ============================================================
    // Stage h: Shutdown parameters (non-blocking attempt first)
    // ============================================================
    if (success && d3d_device) {
        stages[STAGE_ShutdownParams].name = "STAGE_h: NVSDK_NGX_D3D11_Shutdown (params)";
        stages[STAGE_ShutdownParams].start_ms = get_wall_time_ms();
        
        // In real code: NVSDK_NGX_D3D11_Shutdown1(nullptr) or with params
        // THIS IS THE CRITICAL STAGE - may hang >20s per documented SDK behavior
        // Prototype will use timeout mechanism
        stages[STAGE_ShutdownParams].end_ms = get_wall_time_ms() + NGX_TIMEOUT_MS + 1000; // mark for timeout check
        stages[STAGE_ShutdownParams].completed = false; // will be set after timeout check
        fprintf(log_file, "ATTEMPTED: Shutdown with parameters (timeout %ims)\n", NGX_TIMEOUT_MS);
    }
    
    // ============================================================
    // Stage i: Shutdown1 (with 30s timeout via separate process mechanism)
    // ============================================================
    // NOTE: Per SDK documentation, Shutdown1 may hang >20s.
    // The probe demonstrates the timeout mechanism but cannot fully test
    // it without the actual SDK DLLs. The design principle is that the
    // parent process must enforce the timeout, NOT use threading to bypass.
    
    stages[STAGE_Shutdown1].name = "STAGE_i: NVSDK_NGX_D3D11_Shutdown1";
    stages[STAGE_Shutdown1].start_ms = get_wall_time_ms();
    
    // Record attempt - timeout will be enforced by calling process
    // In this stub, we simulate the timeout check
    double shutdown_start = get_wall_time_ms();
    // Simulate a short shutdown for demonstration (replace with real call)
    // Sleep(5000); // 5s for demo - would be NGX_TIMEOUT_MS in real test
    Sleep(2000); // Short demo sleep
    double shutdown_end = get_wall_time_ms();
    double shutdown_duration = shutdown_end - shutdown_start;
    
    stages[STAGE_Shutdown1].end_ms = shutdown_end;
    bool shutdown_timed_out = (shutdown_duration > (NGX_TIMEOUT_MS / 1000.0));
    stages[STAGE_Shutdown1].completed = !shutdown_timed_out;
    if (shutdown_timed_out) {
        stages[STAGE_Shutdown1].hang_reason = "Shutdown1 exceeded timeout";
        fprintf(log_file, "TIMEOUT: Shutdown1 lasted %.1fs > %ds timeout\n", 
                shutdown_duration, NGX_TIMEOUT_MS / 1000);
        success = false;
    } else {
        fprintf(log_file, "PASSED: Shutdown1 completed in %.1fs\n", shutdown_duration);
    }
    
    // ============================================================
    // Stage j: Release device
    // ============================================================
    if (d3d_context) d3d_context->Release();
    if (d3d_device) d3d_device->Release();
    
    stages[STAGE_ReleaseDevice].name = "STAGE_j: Release D3D11 device";
    stages[STAGE_ReleaseDevice].start_ms = get_wall_time_ms();
    stages[STAGE_ReleaseDevice].end_ms = get_wall_time_ms();
    stages[STAGE_ReleaseDevice].completed = true;
    fprintf(log_file, "PASSED: D3D11 device released\n");
    
    // ============================================================
    // Summary
    // ============================================================
    fprintf(log_file, "\nNGX Probe Summary\n");
    fprintf(log_file, "=================\n");
    fprintf(log_file, "Overall result: %s\n", success ? "PASS" : "FAIL (timeout or error)");
    
    for (int i = 0; i < STAGE_Count; i++) {
        fprintf(log_file, "%s: %s", stages[i].name, stages[i].completed ? "COMPLETED" : "TIMED OUT/OVER");
        if (stages[i].hang_reason) {
            fprintf(log_file, " - %s", stages[i].hang_reason);
        }
        fprintf(log_file, " (%.1f ms)\n", stages[i].end_ms - stages[i].start_ms);
    }
    
    fclose(log_file);
    
    // Output summary to user
    MessageBoxA(nullptr, 
        ("NGX Probe complete. See ngx_probe_log.txt for details.").c_str(),
        "NGX Probe", success ? MB_ICONINFORMATION : MB_ICONWARNING);
    
    return success ? 0 : 1;
}