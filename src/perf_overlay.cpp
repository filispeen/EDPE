#include "perf_overlay.h"
#include "upscaler_backend.h"
#include "context_census.h"
#include "ngx_context.h"
#include "config.h"
#include "profiler.h"
#include "log.h"

#include <imgui.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <windows.h>

namespace edpe {

PerfTracker& PerfTracker::instance() {
    static PerfTracker s_tracker;
    return s_tracker;
}

PerfTracker::PerfTracker() {
    QueryPerformanceFrequency(&perfFreq_);
    initNvml();
}

PerfTracker::~PerfTracker() {
    shutdown();
}

void PerfTracker::shutdown() {
    {
        std::lock_guard<std::mutex> lock(nvmlMutex_);
        nvmlRunning_.store(false, std::memory_order_release);
    }
    nvmlCv_.notify_all();
    if (nvmlWorkerThread_.joinable()) {
        nvmlWorkerThread_.join();
    }
    if (nvmlModule_) {
        auto nvmlShutdown = reinterpret_cast<NvmlShutdown_t>(GetProcAddress(nvmlModule_, "nvmlShutdown"));
        if (nvmlShutdown) {
            nvmlShutdown();
        }
        FreeLibrary(nvmlModule_);
        nvmlModule_ = nullptr;
    }
    nvmlAvailable_ = false;
    nvmlDevice_ = nullptr;
    nvmlGetUtil_ = nullptr;
    nvmlGetMem_ = nullptr;
}

void PerfTracker::initNvml() {
    if (nvmlModule_) return;
    nvmlModule_ = LoadLibraryW(L"nvml.dll");
    if (!nvmlModule_) return;

    auto nvmlInit = reinterpret_cast<NvmlInit_t>(GetProcAddress(nvmlModule_, "nvmlInit_v2"));
    auto nvmlGetHandle = reinterpret_cast<NvmlGetHandle_t>(GetProcAddress(nvmlModule_, "nvmlDeviceGetHandleByIndex_v2"));
    nvmlGetUtil_ = reinterpret_cast<NvmlGetUtil_t>(GetProcAddress(nvmlModule_, "nvmlDeviceGetUtilizationRates"));
    nvmlGetMem_ = reinterpret_cast<NvmlGetMem_t>(GetProcAddress(nvmlModule_, "nvmlDeviceGetMemoryInfo"));

    if (nvmlInit && nvmlGetHandle && nvmlGetUtil_ && nvmlInit() == 0) {
        if (nvmlGetHandle(0, &nvmlDevice_) == 0) {
            nvmlAvailable_ = true;
            nvmlRunning_.store(true, std::memory_order_relaxed);
            nvmlWorkerThread_ = std::thread([this]() {
                while (true) {
                    {
                        std::unique_lock<std::mutex> lock(nvmlMutex_);
                        nvmlCv_.wait_for(lock, std::chrono::milliseconds(500), [this]() {
                            return !nvmlRunning_.load(std::memory_order_acquire);
                        });
                        if (!nvmlRunning_.load(std::memory_order_acquire)) break;
                    }
                    NvmlUtil_t util{};
                    if (nvmlGetUtil_ && nvmlDevice_ && nvmlGetUtil_(nvmlDevice_, &util) == 0) {
                        cachedGpuUsage_.store(util.gpu, std::memory_order_relaxed);
                    }
                }
            });
            EdpeLog(L"EDPE: NVML hardware telemetry initialized successfully");
        }
    }
}

void PerfTracker::onPresentStart(IDXGISwapChain* swapChain) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    if (lastPresentStart_.QuadPart > 0 && perfFreq_.QuadPart > 0) {
        const double deltaMs = static_cast<double>(now.QuadPart - lastPresentStart_.QuadPart) * 1000.0 / static_cast<double>(perfFreq_.QuadPart);
        if (deltaMs >= 1.5) {
            lastPresentStart_ = now;
            if (deltaMs < 1000.0) {
                telemetry_.totalFrametimeMs = static_cast<float>(deltaMs);
                smoothedFrametimeMs_ = (smoothedFrametimeMs_ > 0.0f)
                    ? (smoothedFrametimeMs_ * 0.90f + telemetry_.totalFrametimeMs * 0.10f)
                    : telemetry_.totalFrametimeMs;
                telemetry_.fps = (smoothedFrametimeMs_ > 0.0f) ? (1000.0f / smoothedFrametimeMs_) : 0.0f;
            }
        }
    } else {
        lastPresentStart_ = now;
    }
    currentPresentStart_ = now;

    // Retrieve active upscaler timing
    auto& upscaler = UpscalerManager::instance();
    if (upscaler.isReconstructionActive()) {
        const auto& diag = upscaler.getDiagnostics();
        telemetry_.upscalerFrametimeMs = diag.gpuTimeMs;
    } else {
        telemetry_.upscalerFrametimeMs = 0.0f;
    }

    // Resolution dimensions
    UINT outW = 0, outH = 0;
    if (swapChain) {
        DXGI_SWAP_CHAIN_DESC scDesc{};
        if (SUCCEEDED(swapChain->GetDesc(&scDesc))) {
            outW = scDesc.BufferDesc.Width;
            outH = scDesc.BufferDesc.Height;
        }
    }
    UINT sceneW = 0, sceneH = 0;
    ContextCensusGetSceneRenderSize(&sceneW, &sceneH);
    if (!sceneW || !sceneH) {
        sceneW = outW;
        sceneH = outH;
    }
    telemetry_.renderWidth = sceneW;
    telemetry_.renderHeight = sceneH;
    telemetry_.outputWidth = outW;
    telemetry_.outputHeight = outH;

    // Game frametime is time spent by game engine before upscaler / composite
    telemetry_.gameFrametimeMs = (std::max)(0.0f, telemetry_.totalFrametimeMs - telemetry_.upscalerFrametimeMs);

    // Rate-limit hardware telemetry to ~4 times per second (250 ms)
    if (perfFreq_.QuadPart > 0) {
        const double elapsedSinceUpdateMs = static_cast<double>(now.QuadPart - lastTelemetryUpdate_.QuadPart) * 1000.0 / static_cast<double>(perfFreq_.QuadPart);
        if (lastTelemetryUpdate_.QuadPart == 0 || elapsedSinceUpdateMs >= 250.0) {
            updateTelemetry(swapChain);
            lastTelemetryUpdate_ = now;
        }
    }
}

void PerfTracker::onPresentEnd() {
    QueryPerformanceCounter(&lastPresentEnd_);
}

void PerfTracker::updateTelemetry(IDXGISwapChain* swapChain) {
    if (!swapChain) return;

    // 1. Static GPU adapter information
    if (!gpuInfoCached_) {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device)))) {
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
            if (SUCCEEDED(device.As(&dxgiDevice))) {
                Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
                if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                    DXGI_ADAPTER_DESC desc{};
                    if (SUCCEEDED(adapter->GetDesc(&desc))) {
                        telemetry_.gpuName = desc.Description;
                        telemetry_.vramTotalBytes = desc.DedicatedVideoMemory;
                        gpuInfoCached_ = true;
                    }
                }
            }
        }
    }

    // 2. Video Memory (VRAM) Usage
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device)))) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        if (SUCCEEDED(device.As(&dxgiDevice))) {
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
                if (SUCCEEDED(adapter.As(&adapter3))) {
                    DXGI_QUERY_VIDEO_MEMORY_INFO memInfo{};
                    if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo))) {
                        telemetry_.vramUsedBytes = memInfo.CurrentUsage;
                    }
                }
            }
        }
    }

    // 3. GPU Utilization Percentage (%)
    if (nvmlAvailable_ && nvmlDevice_) {
        telemetry_.gpuUsagePct = cachedGpuUsage_.load(std::memory_order_relaxed);
    } else {
        telemetry_.gpuUsagePct = (std::min)(100u, static_cast<uint32_t>((telemetry_.totalFrametimeMs / 16.67f) * 60.0f));
    }
}

void PerfTracker::renderOverlay(bool isMenuOpen, int corner, float scale) {
    auto& cfg = edpe::Config::instance();
    const float PAD = 10.0f;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 work_pos = viewport->WorkPos;
    const ImVec2 work_size = viewport->WorkSize;
    ImVec2 window_pos, window_pos_pivot;

    switch (corner) {
    case 1: // Top-Right
        window_pos = ImVec2(work_pos.x + work_size.x - PAD, work_pos.y + PAD);
        window_pos_pivot = ImVec2(1.0f, 0.0f);
        break;
    case 2: // Bottom-Left
        window_pos = ImVec2(work_pos.x + PAD, work_pos.y + work_size.y - PAD);
        window_pos_pivot = ImVec2(0.0f, 1.0f);
        break;
    case 3: // Bottom-Right
        window_pos = ImVec2(work_pos.x + work_size.x - PAD, work_pos.y + work_size.y - PAD);
        window_pos_pivot = ImVec2(1.0f, 1.0f);
        break;
    case 0: // Top-Left
    default:
        window_pos = ImVec2(work_pos.x + PAD, work_pos.y + PAD);
        window_pos_pivot = ImVec2(0.0f, 0.0f);
        break;
    }

    if (cfg.perfOverlayPosX >= 0.0f && cfg.perfOverlayPosY >= 0.0f) {
        ImGui::SetNextWindowPos(ImVec2(cfg.perfOverlayPosX, cfg.perfOverlayPosY), isMenuOpen ? ImGuiCond_Appearing : ImGuiCond_Always);
    } else {
        ImGui::SetNextWindowPos(window_pos, isMenuOpen ? ImGuiCond_Appearing : ImGuiCond_Always, window_pos_pivot);
    }
    ImGui::SetNextWindowBgAlpha(0.70f);

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDecoration |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav;
    if (!isMenuOpen) {
        window_flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
    }

    const bool customScale = (scale > 0.49f && std::fabs(scale - 1.0f) > 0.01f);
    if (customScale) {
        ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * scale, 8.0f * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f * scale, 3.0f * scale));
    }

    if (ImGui::Begin("##EDPE_PerfOverlay", nullptr, window_flags)) {
        if (isMenuOpen) {
            const ImVec2 curPos = ImGui::GetWindowPos();
            if ((ImGui::IsWindowHovered() || ImGui::IsWindowFocused()) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                if (std::fabs(curPos.x - cfg.perfOverlayPosX) > 0.5f || std::fabs(curPos.y - cfg.perfOverlayPosY) > 0.5f) {
                    cfg.perfOverlayPosX = curPos.x;
                    cfg.perfOverlayPosY = curPos.y;
                    cfg.save();
                }
            } else if (cfg.perfOverlayPosX >= 0.0f && cfg.perfOverlayPosY >= 0.0f) {
                if (std::fabs(curPos.x - cfg.perfOverlayPosX) > 0.5f || std::fabs(curPos.y - cfg.perfOverlayPosY) > 0.5f) {
                    cfg.perfOverlayPosX = curPos.x;
                    cfg.perfOverlayPosY = curPos.y;
                    cfg.save();
                }
            }
        }
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "EDPE Performance HUD");
        ImGui::Separator();

        // 1. FPS with color coding
        const ImVec4 fpsColor = (telemetry_.fps >= 58.0f) ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f) :
                                (telemetry_.fps >= 30.0f) ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) :
                                                            ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        ImGui::Text("FPS:                ");
        ImGui::SameLine();
        ImGui::TextColored(fpsColor, "%.1f FPS", telemetry_.fps);

        // 2. Frametimes
        ImGui::Text("Game Frametime:     %.2f ms", telemetry_.gameFrametimeMs);

        auto& upscaler = UpscalerManager::instance();
        const bool upscalerActive = upscaler.isReconstructionActive();

        if (upscalerActive && telemetry_.upscalerFrametimeMs > 0.0f) {
            const char* backendShort = (upscaler.getBackend() == UpscalerBackend::Taa) ? "TAA" :
                                       (upscaler.getBackend() == UpscalerBackend::AmdFsr) ? "FSR" : "DLSS/DLAA";
            ImGui::Text("Upscaler Frametime: ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.85f, 1.0f), "%.2f ms (%s)",
                telemetry_.upscalerFrametimeMs, backendShort);
        } else {
            ImGui::Text("Upscaler Frametime: 0.00 ms (Bypass)");
        }

        ImGui::Text("Total Frametime:    %.2f ms", telemetry_.totalFrametimeMs);

        ImGui::Separator();

        // 3. Upscaler & Resolution Info
        if (upscalerActive) {
            const bool isAmd = (upscaler.getBackend() == UpscalerBackend::AmdFsr);
            const bool isTaa = (upscaler.getBackend() == UpscalerBackend::Taa);
            const bool isSr = (telemetry_.renderWidth < telemetry_.outputWidth || telemetry_.renderHeight < telemetry_.outputHeight);

            const char* upscalerName = isTaa ? "TAA" :
                                       isAmd ? (isSr ? "AMD FSR SR" : "AMD FSR Native AA")
                                             : (isSr ? "NVIDIA DLSS" : "NVIDIA DLAA");

            char presetBuf[64]{};
            if (isTaa) {
                snprintf(presetBuf, sizeof(presetBuf), "(%s)", isSr ? "Super Resolution" : "Native AA");
            } else if (!isAmd) {
                const uint32_t p = NgxContext::get_preset();
                const char* pName = (p == 10) ? "Preset J" :
                                    (p == 11) ? "Preset K" :
                                    (p == 12) ? "Preset L" :
                                    (p == 13) ? "Preset M" : "Auto (Preset K)";
                snprintf(presetBuf, sizeof(presetBuf), "(%s)", pName);
            } else {
                snprintf(presetBuf, sizeof(presetBuf), "(%s)", isSr ? "Super Resolution" : "Native AA");
            }

            ImGui::Text("Upscaler:           ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s %s", upscalerName, presetBuf);

            const float scalePct = telemetry_.outputWidth > 0
                ? (100.0f * static_cast<float>(telemetry_.renderWidth) / static_cast<float>(telemetry_.outputWidth))
                : 100.0f;

            ImGui::Text("Resolution:         %ux%u -> %ux%u (%.1f%%)",
                telemetry_.renderWidth, telemetry_.renderHeight,
                telemetry_.outputWidth, telemetry_.outputHeight, scalePct);
        } else {
            ImGui::Text("Upscaler:           Bypass (Native)");
            ImGui::Text("Resolution:         %ux%u -> %ux%u (100.0%%)",
                telemetry_.outputWidth, telemetry_.outputHeight,
                telemetry_.outputWidth, telemetry_.outputHeight);
        }

        ImGui::Separator();

        // 4. GPU Info
        char gpuNameUtf8[128]{};
        WideCharToMultiByte(CP_UTF8, 0, telemetry_.gpuName.c_str(), -1, gpuNameUtf8, sizeof(gpuNameUtf8), nullptr, nullptr);
        ImGui::Text("Current GPU:        %s", gpuNameUtf8[0] ? gpuNameUtf8 : "Direct3D 11 Adapter");

        // 5. GPU Usage
        ImGui::Text("GPU Usage:          %u %%", telemetry_.gpuUsagePct);

        // 6. VRAM Usage
        if (telemetry_.vramTotalBytes > 0) {
            const double usedGb = static_cast<double>(telemetry_.vramUsedBytes) / (1024.0 * 1024.0 * 1024.0);
            const double totalGb = static_cast<double>(telemetry_.vramTotalBytes) / (1024.0 * 1024.0 * 1024.0);
            const float pct = static_cast<float>(100.0 * static_cast<double>(telemetry_.vramUsedBytes) / static_cast<double>(telemetry_.vramTotalBytes));
            ImGui::Text("VRAM Usage:         %.2f / %.2f GB (%u %%)", usedGb, totalGb, static_cast<unsigned>(pct));
        } else if (telemetry_.vramUsedBytes > 0) {
            const double usedMb = static_cast<double>(telemetry_.vramUsedBytes) / (1024.0 * 1024.0);
            ImGui::Text("VRAM Usage:         %.1f MB", usedMb);
        }

        if (cfg.enableProfiler) {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Profiler (120f avg):");
            const float mapMs = Profiler::instance().getAvgMs(ProfileZone::MapUnmap);
            const float omMs = Profiler::instance().getAvgMs(ProfileZone::OMSetRenderTargets);
            const float deltaMs = Profiler::instance().getAvgMs(ProfileZone::CameraDeltaCopy);
            const float drawMs = Profiler::instance().getAvgMs(ProfileZone::DrawProbe);
            const float upscaleMs = Profiler::instance().getAvgMs(ProfileZone::TemporalUpscale);
            const float measuredSum = mapMs + omMs + deltaMs + drawMs;
            const float unaccountedMs = (std::max)(0.0f, telemetry_.gameFrametimeMs - measuredSum);
            ImGui::Text("  Map/Unmap (CB/Jitter): %.3f ms", mapMs);
            ImGui::Text("  OMSetRenderTargets:    %.3f ms", omMs);
            ImGui::Text("  Camera Delta Copy:     %.3f ms", deltaMs);
            ImGui::Text("  HUD Draw Probe:        %.3f ms", drawMs);
            ImGui::Text("  Upscale Prep & Eval:   %.3f ms", upscaleMs);
            ImGui::Text("  Unaccounted (Game):    %.3f ms", unaccountedMs);
        }
    }
    ImGui::End();

    if (customScale) {
        ImGui::PopStyleVar(2);
        ImGui::PopFont();
    }
}

} // namespace edpe
