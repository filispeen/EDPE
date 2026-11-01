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

    FrametimeSample sample;
    sample.gameMs = telemetry_.gameFrametimeMs;
    sample.upscalerMs = telemetry_.upscalerFrametimeMs;
    sample.totalMs = telemetry_.totalFrametimeMs;
    frametimeHistory_[historyIndex_] = sample;
    historyIndex_ = (historyIndex_ + 1) % kFrametimeHistorySize;
    if (historyCount_ < kFrametimeHistorySize) {
        historyCount_++;
    }

    // Rate-limit display telemetry & profiler updates to ~66.66 ms (~15 FPS) to keep numbers readable and reduce UI overhead
    auto updateDisplaySnapshots = [this]() {
        displayTelemetry_ = telemetry_;
        if (Profiler::instance().isEnabled()) {
            profilerDisplay_.mapMs = Profiler::instance().getAvgMs(ProfileZone::MapUnmap);
            profilerDisplay_.omMs = Profiler::instance().getAvgMs(ProfileZone::OMSetRenderTargets);
            profilerDisplay_.deltaMs = Profiler::instance().getAvgMs(ProfileZone::CameraDeltaCopy);
            profilerDisplay_.drawMs = Profiler::instance().getAvgMs(ProfileZone::DrawProbe);
            profilerDisplay_.upscaleMs = Profiler::instance().getAvgMs(ProfileZone::TemporalUpscale);
            const float measuredSum = profilerDisplay_.mapMs + profilerDisplay_.omMs + profilerDisplay_.deltaMs + profilerDisplay_.drawMs;
            profilerDisplay_.unaccountedMs = (std::max)(0.0f, displayTelemetry_.gameFrametimeMs - measuredSum);
        }
    };

    if (perfFreq_.QuadPart > 0) {
        const double elapsedDisplayMs = static_cast<double>(now.QuadPart - lastDisplayUpdate_.QuadPart) * 1000.0 / static_cast<double>(perfFreq_.QuadPart);
        if (lastDisplayUpdate_.QuadPart == 0 || elapsedDisplayMs >= 66.66) {
            updateDisplaySnapshots();
            lastDisplayUpdate_ = now;
        }
    } else {
        updateDisplaySnapshots();
    }

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

void PerfTracker::renderFrametimeGraph(float scale, const PerfOverlayOverride* override) {
    ImGui::Spacing();

    const float availW = ImGui::GetContentRegionAvail().x;
    const float graphWidth = (availW > 180.0f * scale) ? availW : 240.0f * scale;
    const float graphHeight = 42.0f * scale;
    const ImVec2 graphSize(graphWidth, graphHeight);

    const ImVec2 pMin = ImGui::GetCursorScreenPos();
    const ImVec2 pMax = ImVec2(pMin.x + graphSize.x, pMin.y + graphSize.y);
    ImGui::Dummy(graphSize);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (!drawList) return;

    // Background & border
    drawList->AddRectFilled(pMin, pMax, IM_COL32(20, 22, 27, 230), 2.0f * scale);
    drawList->AddRect(pMin, pMax, IM_COL32(70, 75, 85, 180), 2.0f * scale);

    const size_t count = historyCount_;
    const size_t startIndex = (count < kFrametimeHistorySize) ? 0 : historyIndex_;

    // Calculate vertical scaling with headroom
    float maxMs = 20.0f;
    for (size_t i = 0; i < count; ++i) {
        const auto& s = frametimeHistory_[(startIndex + i) % kFrametimeHistorySize];
        if (s.totalMs > maxMs) maxMs = s.totalMs;
        if (s.gameMs > maxMs) maxMs = s.gameMs;
    }
    maxMs = (std::min)(maxMs * 1.15f, 120.0f);

    const float innerH = graphHeight - 4.0f * scale;
    const float bottomY = pMax.y - 2.0f * scale;

    // Reference grid line at 16.67ms (60 FPS)
    if (maxMs >= 16.67f) {
        const float y60 = bottomY - (16.67f / maxMs) * innerH;
        drawList->AddLine(ImVec2(pMin.x, y60), ImVec2(pMax.x, y60), IM_COL32(90, 95, 105, 120), 1.0f);
    }
    // Reference grid line at 33.33ms (30 FPS)
    if (maxMs >= 33.33f) {
        const float y30 = bottomY - (33.33f / maxMs) * innerH;
        drawList->AddLine(ImVec2(pMin.x, y30), ImVec2(pMax.x, y30), IM_COL32(90, 95, 105, 120), 1.0f);
    }

    if (count < 2) return;

    // 1. Total Frametime: Orange
    for (size_t i = 0; i < count - 1; ++i) {
        const auto& s0 = frametimeHistory_[(startIndex + i) % kFrametimeHistorySize];
        const auto& s1 = frametimeHistory_[(startIndex + i + 1) % kFrametimeHistorySize];
        const float x0 = pMin.x + (static_cast<float>(i) / static_cast<float>(count - 1)) * graphWidth;
        const float x1 = pMin.x + (static_cast<float>(i + 1) / static_cast<float>(count - 1)) * graphWidth;
        const float y0 = bottomY - ((std::clamp)(s0.totalMs, 0.0f, maxMs) / maxMs) * innerH;
        const float y1 = bottomY - ((std::clamp)(s1.totalMs, 0.0f, maxMs) / maxMs) * innerH;
        drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 150, 20, 255), 1.6f * scale);
    }

    // 2. Game Frametime: White
    for (size_t i = 0; i < count - 1; ++i) {
        const auto& s0 = frametimeHistory_[(startIndex + i) % kFrametimeHistorySize];
        const auto& s1 = frametimeHistory_[(startIndex + i + 1) % kFrametimeHistorySize];
        const float x0 = pMin.x + (static_cast<float>(i) / static_cast<float>(count - 1)) * graphWidth;
        const float x1 = pMin.x + (static_cast<float>(i + 1) / static_cast<float>(count - 1)) * graphWidth;
        const float y0 = bottomY - ((std::clamp)(s0.gameMs, 0.0f, maxMs) / maxMs) * innerH;
        const float y1 = bottomY - ((std::clamp)(s1.gameMs, 0.0f, maxMs) / maxMs) * innerH;
        drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(255, 255, 255, 240), 1.2f * scale);
    }

    // 3. Upscaler Frametime: Cyan
    for (size_t i = 0; i < count - 1; ++i) {
        const auto& s0 = frametimeHistory_[(startIndex + i) % kFrametimeHistorySize];
        const auto& s1 = frametimeHistory_[(startIndex + i + 1) % kFrametimeHistorySize];
        const float x0 = pMin.x + (static_cast<float>(i) / static_cast<float>(count - 1)) * graphWidth;
        const float x1 = pMin.x + (static_cast<float>(i + 1) / static_cast<float>(count - 1)) * graphWidth;
        const float y0 = bottomY - ((std::clamp)(s0.upscalerMs, 0.0f, maxMs) / maxMs) * innerH;
        const float y1 = bottomY - ((std::clamp)(s1.upscalerMs, 0.0f, maxMs) / maxMs) * innerH;
        drawList->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 240, 240, 255), 1.2f * scale);
    }
}

void PerfTracker::renderOverlay(bool isMenuOpen, int corner, float scale, const PerfOverlayOverride* override) {
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
        const float shownFps = override ? telemetry_.fps : displayTelemetry_.fps;
        const ImVec4 fpsColor = (shownFps >= 58.0f) ? ImVec4(0.2f, 1.0f, 0.2f, 1.0f) :
                                (shownFps >= 30.0f) ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) :
                                                      ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        ImGui::Text("FPS:                ");
        ImGui::SameLine();
        ImGui::TextColored(fpsColor, "%.1f FPS", shownFps);

        // 2. Frametimes
        const float shownGameMs = override ? (std::max)(0.0f, telemetry_.totalFrametimeMs - override->upscalerMs)
                                           : displayTelemetry_.gameFrametimeMs;
        ImGui::Text("Game Frametime:     ");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%6.2f ms", shownGameMs);

        auto& upscaler = UpscalerManager::instance();
        UpscalerBackend shownBackend = upscaler.getBackend();
        bool upscalerActive = upscaler.isReconstructionActive();
        float shownUpscalerMs = override ? override->upscalerMs : displayTelemetry_.upscalerFrametimeMs;
        if (override) {
            shownBackend = (override->backendId == 1) ? UpscalerBackend::NvidiaDlss :
                           (override->backendId == 2) ? UpscalerBackend::AmdFsr :
                           (override->backendId == 3) ? UpscalerBackend::Taa : UpscalerBackend::None;
            upscalerActive = (shownBackend != UpscalerBackend::None);
        }

        ImGui::Text("Upscaler Frametime: ");
        ImGui::SameLine();
        if (upscalerActive && shownUpscalerMs > 0.0f) {
            const char* backendShort = (shownBackend == UpscalerBackend::Taa) ? "TAA" :
                                       (shownBackend == UpscalerBackend::AmdFsr) ? "FSR" : "DLSS/DLAA";
            ImGui::TextColored(ImVec4(0.0f, 0.95f, 0.95f, 1.0f), "%6.2f ms (%s)",
                shownUpscalerMs, backendShort);
        } else {
            ImGui::TextColored(ImVec4(0.0f, 0.95f, 0.95f, 1.0f), "%6.2f ms (Bypass)", 0.0f);
        }

        const float shownTotalMs = override ? telemetry_.totalFrametimeMs : displayTelemetry_.totalFrametimeMs;
        ImGui::Text("Total Frametime:    ");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.60f, 0.05f, 1.0f), "%6.2f ms", shownTotalMs);

        renderFrametimeGraph(scale, override);

        ImGui::Separator();

        // 3. Upscaler & Resolution Info
        if (upscalerActive) {
            const bool isAmd = (shownBackend == UpscalerBackend::AmdFsr);
            const bool isTaa = (shownBackend == UpscalerBackend::Taa);
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
            ImGui::Text(override ? "Upscaler:           Off (Before reconstruction)" : "Upscaler:           Bypass (Native)");
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
            ImGui::Text("  Map/Unmap (CB/Jitter): %6.3f ms", profilerDisplay_.mapMs);
            ImGui::Text("  OMSetRenderTargets:    %6.3f ms", profilerDisplay_.omMs);
            ImGui::Text("  Camera Delta Copy:     %6.3f ms", profilerDisplay_.deltaMs);
            ImGui::Text("  HUD Draw Probe:        %6.3f ms", profilerDisplay_.drawMs);
            ImGui::Text("  Upscale Prep & Eval:   %6.3f ms", profilerDisplay_.upscaleMs);
            ImGui::Text("  Unaccounted (Game):    %6.3f ms", profilerDisplay_.unaccountedMs);
        }
    }
    ImGui::End();

    if (customScale) {
        ImGui::PopStyleVar(2);
        ImGui::PopFont();
    }
}

} // namespace edpe
