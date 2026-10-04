#include <windows.h>
#include <stdio.h>
#include <chrono>
#include <thread>
#include <string>

// Timeout for NGX Shutdown1 in milliseconds
#define NGX_SHUTDOWN_TIMEOUT_MS 30000

// Child process command line
// Runs ngx_probe.exe and exits with appropriate code
int main(int argc, char* argv[]) {
    // Launch the child probe process
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    
    // Build command line for child probe
    char cmd_line[MAX_PATH];
    snprintf(cmd_line, MAX_PATH, "\"%s\\tools\\ngx_probe\\ngx_probe.exe\"", 
             "F:/Code/С++/EDPE");
    
    // Create the child process
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
    
    // Report results
    bool success = (exit_code == 0);
    
    // Write result file for parent to read
    char result_file[MAX_PATH];
    snprintf(result_file, MAX_PATH, "%s\\tools\\ngx_probe\\result.txt", 
             "F:/Code/С++/EDPE");
    
    FILE* f = fopen(result_file, "w");
    if (f) {
        fprintf(f, "success=%d\nexit_code=%d\n", success, exit_code);
        fclose(f);
    }
    
    // Display results
    std::string msg = success ? 
        "NGX Probe: PASS" : 
        "NGX Probe: FAIL (timeout or error)";
    
    MessageBoxA(nullptr, msg.c_str(), "NGX Probe Host", success ? MB_ICONINFORMATION : MB_ICONERROR);
    
    return success ? 0 : 1;
}