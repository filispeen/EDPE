#include <windows.h>
#include <stdio.h>
#include <chrono>
#include <thread>
#include <string>
#include <iostream>

// Timeout for NGX Shutdown1 in milliseconds
#define NGX_SHUTDOWN_TIMEOUT_MS 30000

// Shutdown variant constants - must match ngx_probe.cpp definitions
#define VARIANT_V1_Shutdown1_only 1
#define VARIANT_V2_Shutdown_only 2
#define VARIANT_V3_FlushWaitThenShutdown1 3
#define VARIANT_V4_Shutdown1ThenHold 4
#define VARIANT_SkipEvaluate 5

// Child process command line
// Runs ngx_probe.exe and exits with appropriate code
int main(int argc, char* argv[]) {
    // Parse command-line arguments for variant selection
    int variant = VARIANT_V1_Shutdown1_only; // default
    bool skip_evaluate = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--variant") == 0 && i + 1 < argc) {
            variant = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--skip-evaluate") == 0) {
            skip_evaluate = true;
        }
    }

    // Build command line for child probe
    char cmd_line[MAX_PATH];
    snprintf(cmd_line, MAX_PATH, "\"%s\\tools\\ngx_probe\\ngx_probe.exe\"", 
             "F:/Code/С++/EDPE");

    // Append variant and skip-evaluate flags to command line
    if (variant != VARIANT_V1_Shutdown1_only) {
        char variant_str[32];
        snprintf(variant_str, sizeof(variant_str), " --variant %d", variant);
        strncat_s(cmd_line, MAX_PATH - strlen(cmd_line) - 1, variant_str, _TRUNCATE);
    }
    if (skip_evaluate) {
        strncat_s(cmd_line, MAX_PATH - strlen(cmd_line) - 1, " --skip-evaluate", _TRUNCATE);
    }

    // Create the child process
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    if (!CreateProcessA(
            nullptr,                  // No module name (use command line)
            cmd_line,                 // Command line
            nullptr,                  // Process security attributes
            nullptr,                  // Primary thread security attributes
            FALSE,                    // Do not inherit handles
            CREATE_NEW_CONSOLE,       // Create new console
            nullptr,                  // Use parent's environment
            nullptr,                  // Use parent's current directory
            &si,                      // STARTUPINFO pointer
            &pi                       // PROCESS_INFORMATION pointer
        )) {
        MessageBoxA(nullptr, "Failed to create NGX probe child process", "NGX Probe Host", MB_ICONERROR);
        return 1;
    }

    // Wait for the child process with timeout
    HANDLE wait_handles[2] = { pi.hProcess, nullptr };
    DWORD wait_result = WaitForSingleObject(pi.hProcess, NGX_SHUTDOWN_TIMEOUT_MS);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    // Clean up process handles
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // If the process is still running after timeout, terminate it
    if (wait_result == WAIT_TIMEOUT) {
        fprintf(stderr, "NGX Probe timed out after %d ms, terminating child process\n", 
                NGX_SHUTDOWN_TIMEOUT_MS);
        TerminateProcess(pi.hProcess, 1);
        exit_code = 1; // timeout error
        // Note: Per the design principle, we DO NOT use threading to bypass the hang.
        // The shutdown hang is a known SDK issue; the timeout mechanism proves it.
    }

    // Read the log file to determine the last completed stage
    char log_file[MAX_PATH];
    snprintf(log_file, MAX_PATH, "%s\\tools\\ngx_probe\\ngx_probe_log.txt", 
             "F:/Code/С++/EDPE");

    FILE* f = fopen(log_file, "r");
    char* last_completed_stage = "unknown";  // Fixed: removed const to allow assignment
    double last_completed_time_ms = 0.0;

    if (f) {
        char line[512];
        // Read all lines; the last completed stage is tracked
        while (fgets(line, sizeof(line), f)) {
            // Look for LAST_COMPLETED_STAGE marker
            if (strncmp(line, "LAST_COMPLETED_STAGE:", 23) == 0) {
                last_completed_stage = line + 23;
                // Trim newline
                size_t len = strlen(last_completed_stage);
                while (len > 0 && (last_completed_stage[len-1] == '\n' || 
                    last_completed_stage[len-1] == '\r' || 
                    last_completed_stage[len-1] == ' ')) {
                    last_completed_stage[--len] = '\0';
                }
            }
            // Also look for the overall result
            // Try to find timestamp lines and track the last one with PASS/SKIP
        }
        fclose(f);
    }

    // Report results
    bool success = (exit_code == 0);

    // Determine result summary from log
    char result_buf[256] = "";
    if (!success) {
        snprintf(result_buf, sizeof(result_buf), "NGX Probe: FAIL (timeout or exit code %d)", exit_code);
    } else {
        // Read the log file for detailed info
        FILE* lf = fopen(log_file, "r");
        if (lf) {
            char line[512];
            bool found_last = false;
            while (fgets(line, sizeof(line), lf)) {
                // Check for last completed stage
                if (strncmp(line, "LAST_COMPLETED_STAGE:", 23) == 0) {
                    // Report it
                    found_last = true;
                }
                // Check for variant info
                if (strncmp(line, "VARIANT:", 8) == 0) {
                    // Found variant info
                }
                // Check for skip evaluate
                if (strncmp(line, "SKIP_EVALUATE:", 14) == 0) {
                    // Found skip info
                }
            }
            fclose(lf);
        }
        snprintf(result_buf, sizeof(result_buf), "NGX Probe: PASS (last stage: %s)", last_completed_stage);
    }

    std::string msg = success ? 
        "NGX Probe: PASS" : 
        result_buf;

    MessageBoxA(nullptr, msg.c_str(), "NGX Probe Host", success ? MB_ICONINFORMATION : MB_ICONERROR);

    return success ? 0 : 1;
}