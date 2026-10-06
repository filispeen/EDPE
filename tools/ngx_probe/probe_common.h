#pragma once
// Shared between ngx_probe (child, links the NGX SDK) and ngx_probe_host
// (parent, no SDK dependency). Constants only.

enum ProbeStage {
    StageCreateDevice,     // a
    StageNgxInit,          // b
    StageCapability,       // c
    StageOptimalSettings,  // d
    StageCreateFeature,    // e
    StageEvaluate,         // f
    StageReleaseFeature,   // g
    StageReleaseParams,    // h
    StageFlushSync,        // i
    StageShutdown,         // j
    StageReleaseDevice,    // k
    StageCount
};

inline const char* const kStageNames[StageCount] = {
    "a_create_device",
    "b_ngx_init",
    "c_capability",
    "d_optimal_settings",
    "e_create_feature",
    "f_evaluate",
    "g_release_feature",
    "h_release_params",
    "i_flush_sync",
    "j_shutdown",
    "k_release_device",
};

// One variant per process run, never combined.
enum ProbeVariant {
    VariantV1 = 1,  // Shutdown1(device) only
    VariantV2 = 2,  // Shutdown() only
    VariantV3 = 3,  // context Flush and sync, then Shutdown1(device)
    VariantV4 = 4,  // Shutdown1 before device release, device kept alive 2 s afterwards
    VariantV5 = 5,  // skip stage f (evaluate), otherwise V1
};

inline const char* const kVariantNames[] = {
    "",
    "V1 Shutdown1 only",
    "V2 Shutdown only",
    "V3 Flush+sync then Shutdown1",
    "V4 Shutdown1 then 2s device hold",
    "V5 skip evaluate",
};

constexpr unsigned kProbeTimeoutMs = 30000;  // parent wall-clock limit per child

// Child exit codes (anything else is a crash or a kill).
constexpr int kExitOk = 0;
constexpr int kExitStageFailed = 10;  // a stage returned a non-success code
constexpr int kExitSetupError = 11;   // bad arguments or no hardware adapter

// Child log line: elapsed_ms|stage|status|result_hex|detail
// status is "ok", "fail" or "skip". "info" lines use stage "info".
