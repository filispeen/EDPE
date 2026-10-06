// ngx_probe_host: parent process for ngx_probe.
// Runs one variant in a child process with a 30 s wall-clock timeout and kills the
// child on timeout. No threads are used to hide a hang. Reports the last completed
// stage from the child's flushed log.
//
// Usage: ngx_probe_host.exe --variant N [--run K] [--results-dir <dir>]

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "probe_common.h"

static std::wstring exe_dir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

static std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t pos = s.find(sep, start);
        if (pos == std::string::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
}

static int stage_index(const std::string& name) {
    for (int i = 0; i < StageCount; ++i)
        if (name == kStageNames[i]) return i;
    return -1;
}

int wmain(int argc, wchar_t** argv) {
    int variant = 0;
    int run = 1;
    std::wstring results = exe_dir() + L"\\ngx_probe_results";
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--variant") && i + 1 < argc) variant = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--run") && i + 1 < argc) run = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--results-dir") && i + 1 < argc) results = argv[++i];
    }
    if (variant < VariantV1 || variant > VariantV5) {
        fprintf(stderr, "usage: ngx_probe_host --variant 1..5 [--run K] [--results-dir <dir>]\n");
        return 2;
    }

    CreateDirectoryW(results.c_str(), nullptr);
    const std::wstring appdata = results + L"\\ngx_appdata";
    CreateDirectoryW(appdata.c_str(), nullptr);
    wchar_t logname[64];
    swprintf(logname, 64, L"\\V%d_run%d.log", variant, run);
    const std::wstring log_path = results + logname;
    DeleteFileW(log_path.c_str());

    const std::wstring child_exe = exe_dir() + L"\\ngx_probe.exe";
    std::wstring cmd = L"\"" + child_exe + L"\" --variant " + std::to_wstring(variant) + L" --log \"" + log_path +
                       L"\" --appdata \"" + appdata + L"\"";
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    const std::wstring dir = exe_dir();
    const ULONGLONG t0 = GetTickCount64();
    if (!CreateProcessW(child_exe.c_str(), cmd_buf.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si,
                        &pi)) {
        fprintf(stderr, "cannot start ngx_probe.exe (error %lu)\n", GetLastError());
        return 2;
    }

    bool timed_out = false;
    DWORD w = WaitForSingleObject(pi.hProcess, kProbeTimeoutMs);
    if (w == WAIT_TIMEOUT) {
        timed_out = true;
        TerminateProcess(pi.hProcess, 0xDEAD);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);  // read before closing the handle
    const ULONGLONG elapsed = GetTickCount64() - t0;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Parse the child's log.
    std::ifstream in(log_path);
    std::string line;
    int last_completed = -1;
    int failed = -1;
    std::string codes;
    std::string ngx_stages = "b_ngx_init c_capability d_optimal_settings e_create_feature f_evaluate "
                             "g_release_feature h_release_params j_shutdown";
    while (std::getline(in, line)) {
        printf("%s\n", line.c_str());
        auto f = split(line, '|');
        if (f.size() < 4) continue;
        int idx = stage_index(f[1]);
        if (idx < 0) continue;
        if (f[2] == "ok" || f[2] == "skip") last_completed = idx;
        if (f[2] == "fail") failed = idx;
        if (ngx_stages.find(f[1]) != std::string::npos) codes += f[1].substr(0, 1) + ":" + f[3] + " ";
    }

    std::string state;
    if (timed_out) {
        state = "hang killed at 30s";
    } else if (exit_code == (DWORD)kExitOk) {
        state = "clean";
    } else if (exit_code == (DWORD)kExitStageFailed) {
        state = "stage failed";
    } else if (exit_code == (DWORD)kExitSetupError) {
        state = "setup error";
    } else if ((exit_code & 0xC0000000u) == 0xC0000000u) {
        state = "crash";
    } else {
        state = "exit " + std::to_string(exit_code);
    }

    const char* last_name = last_completed >= 0 ? kStageNames[last_completed] : "none";
    std::string where;
    if (failed >= 0) {
        where = std::string("failed_at=") + kStageNames[failed];
    } else if (state != "clean" && last_completed + 1 < StageCount) {
        where = std::string("stopped_in=") + kStageNames[last_completed + 1];
    } else {
        where = "stopped_in=-";
    }
    printf("SUMMARY|variant=%s|run=%d|last_completed=%s|%s|exit_state=%s|exit_code=0x%08lX|ngx_codes=%s|elapsed_ms=%llu\n",
           kVariantNames[variant], run, last_name, where.c_str(), state.c_str(), (unsigned long)exit_code,
           codes.c_str(), (unsigned long long)elapsed);
    return (state == "clean") ? 0 : 1;
}
