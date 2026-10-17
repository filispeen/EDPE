#include "jitter_logger.h"
#include "temporal_math.h"
#include "context_census.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <vector>

#include <imgui.h>

namespace edpe {

JitterLogger& JitterLogger::instance() {
    static JitterLogger s_instance;
    return s_instance;
}

JitterLogger::JitterLogger() {
    workerThread_ = std::thread(&JitterLogger::workerThreadLoop, this);
}

JitterLogger::~JitterLogger() {
    shutdown();
}

void JitterLogger::shutdown() {
    if (running_.exchange(false)) {
        cv_.notify_all();
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
    }
}

void JitterLogger::setLoggingEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_relaxed);
}

bool JitterLogger::isLoggingEnabled() const {
    return enabled_.load(std::memory_order_relaxed);
}

void JitterLogger::setOverlayVisible(bool visible) {
    overlayVisible_.store(visible, std::memory_order_relaxed);
}

bool JitterLogger::isOverlayVisible() const {
    return overlayVisible_.load(std::memory_order_relaxed);
}

void JitterLogger::pushFrame(const JitterFrameLog& log) {
    // 1. Update UI rolling history (< 10ns)
    {
        std::lock_guard lock(historyMutex_);
        historyPixelX_[historyHead_] = log.pixelX;
        historyPixelY_[historyHead_] = log.pixelY;
        historyDeltaMs_[historyHead_] = log.deltaMs;
        historyWrites_[historyHead_] = static_cast<float>(log.injectedWrites);
        historyHead_ = (historyHead_ + 1) % kHistorySize;
        if (historyCount_ < kHistorySize) ++historyCount_;
    }

    {
        std::lock_guard lock(latestMutex_);
        latest_ = log;
    }

    // 2. Push to circular buffer for background logging (< 10ns)
    if (enabled_.load(std::memory_order_relaxed)) {
        const uint32_t w = writeIdx_.load(std::memory_order_relaxed);
        ring_[w % kRingSize] = log;
        writeIdx_.store(w + 1, std::memory_order_release);

        // Wake background thread if a batch accumulated
        if ((w % 256) == 0) {
            cv_.notify_one();
        }
    }
}

void JitterLogger::flush() {
    flushRequested_.store(true, std::memory_order_release);
    cv_.notify_one();
}

void JitterLogger::clearFile() {
    const std::wstring path = getLogFilePath();
    if (path.empty()) return;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") == 0 && f) {
        std::fprintf(f, "# EDPE Jitter Stability Log\n");
        std::fprintf(f, "# Frame, Time_ms, Delta_ms, Phase, TotalPhases, PixelX, PixelY, NdcX, NdcY, Writes, Rejections, EvalJx, EvalJy, Synced, CamValid, HeldJitter, SceneCut, CamMove_m, CamRot_deg\n");
        std::fclose(f);
    }
    writtenRows_.store(0, std::memory_order_relaxed);
}

uint32_t JitterLogger::getWrittenRowCount() const {
    return writtenRows_.load(std::memory_order_relaxed);
}

void JitterLogger::getHistory(float* outPixelX, float* outPixelY, float* outDeltaMs,
                             float* outWrites, size_t maxCount, size_t* outCount) {
    std::lock_guard lock(historyMutex_);
    const size_t n = (std::min)(historyCount_, maxCount);
    if (outCount) *outCount = n;
    if (!n) return;

    const size_t start = (historyHead_ + kHistorySize - n) % kHistorySize;
    for (size_t i = 0; i < n; ++i) {
        const size_t idx = (start + i) % kHistorySize;
        if (outPixelX) outPixelX[i] = historyPixelX_[idx];
        if (outPixelY) outPixelY[i] = historyPixelY_[idx];
        if (outDeltaMs) outDeltaMs[i] = historyDeltaMs_[idx];
        if (outWrites) outWrites[i] = historyWrites_[idx];
    }
}

JitterFrameLog JitterLogger::getLatest() {
    std::lock_guard lock(latestMutex_);
    return latest_;
}

std::wstring JitterLogger::getLogFilePath() const {
    wchar_t path[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!len || len >= MAX_PATH) return L"";
    wchar_t* name = wcsrchr(path, L'\\');
    if (!name) return L"";
    const size_t prefix = name + 1 - path;
    if (prefix + sizeof(L"jitter.log") / sizeof(wchar_t) > MAX_PATH) return L"";
    wcscpy_s(name + 1, MAX_PATH - prefix, L"jitter.log");
    return path;
}

void JitterLogger::workerThreadLoop() {
    const std::wstring path = getLogFilePath();
    if (path.empty()) return;

    // Check if file exists, if not write header
    {
        DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            FILE* f = nullptr;
            if (_wfopen_s(&f, path.c_str(), L"w") == 0 && f) {
                std::fprintf(f, "# EDPE Jitter Stability Log\n");
                std::fprintf(f, "# Frame, Time_ms, Delta_ms, Phase, TotalPhases, PixelX, PixelY, NdcX, NdcY, Writes, Rejections, EvalJx, EvalJy, Synced, CamValid, HeldJitter, SceneCut, CamMove_m, CamRot_deg\n");
                std::fclose(f);
            }
        }
    }

    constexpr size_t kChunkSize = 32768;
    std::vector<char> buffer(kChunkSize);

    while (running_.load(std::memory_order_relaxed)) {
        {
            std::unique_lock<std::mutex> lock(cvMutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(250), [this] {
                return !running_.load(std::memory_order_relaxed) ||
                       flushRequested_.load(std::memory_order_relaxed) ||
                       (writeIdx_.load(std::memory_order_relaxed) - readIdx_.load(std::memory_order_relaxed) >= 256);
            });
        }

        flushRequested_.store(false, std::memory_order_relaxed);

        const uint32_t w = writeIdx_.load(std::memory_order_acquire);
        uint32_t r = readIdx_.load(std::memory_order_relaxed);

        if (r == w) continue;

        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"a") != 0 || !f) {
            continue;
        }

        size_t bufUsed = 0;
        uint32_t batchWritten = 0;

        while (r != w) {
            const JitterFrameLog& entry = ring_[r % kRingSize];
            char line[256];
            const int lineLen = std::snprintf(line, sizeof(line),
                "%llu, %.2f, %.2f, %u, %u, %.4f, %.4f, %.6g, %.6g, %u, %u, %.4f, %.4f, %d, %d, %d, %d, %.4f, %.3f\n",
                entry.frame, entry.timeMs, entry.deltaMs, entry.phase, entry.totalPhases,
                entry.pixelX, entry.pixelY, entry.ndcX, entry.ndcY,
                entry.injectedWrites, entry.rejections,
                entry.evalJx, entry.evalJy, entry.syncOk ? 1 : 0,
                entry.cameraValid ? 1 : 0, entry.heldJitter ? 1 : 0, entry.sceneCut ? 1 : 0,
                entry.camMoveDist, entry.camRotDeg);

            if (lineLen > 0) {
                if (bufUsed + lineLen >= kChunkSize) {
                    std::fwrite(buffer.data(), 1, bufUsed, f);
                    bufUsed = 0;
                }
                std::memcpy(buffer.data() + bufUsed, line, lineLen);
                bufUsed += lineLen;
            }

            ++r;
            ++batchWritten;
            if (batchWritten >= 2048) {
                // Break batch to keep background thread responsive
                break;
            }
        }

        if (bufUsed > 0) {
            std::fwrite(buffer.data(), 1, bufUsed, f);
        }
        std::fflush(f);
        std::fclose(f);

        readIdx_.store(r, std::memory_order_release);
        writtenRows_.fetch_add(batchWritten, std::memory_order_relaxed);
    }

    // Flush remainder before thread exits
    const uint32_t w = writeIdx_.load(std::memory_order_acquire);
    uint32_t r = readIdx_.load(std::memory_order_relaxed);
    if (r != w) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"a") == 0 && f) {
            while (r != w) {
                const JitterFrameLog& entry = ring_[r % kRingSize];
                std::fprintf(f,
                    "%llu, %.2f, %.2f, %u, %u, %.4f, %.4f, %.6g, %.6g, %u, %u, %.4f, %.4f, %d, %d, %d, %d, %.4f, %.3f\n",
                    entry.frame, entry.timeMs, entry.deltaMs, entry.phase, entry.totalPhases,
                    entry.pixelX, entry.pixelY, entry.ndcX, entry.ndcY,
                    entry.injectedWrites, entry.rejections,
                    entry.evalJx, entry.evalJy, entry.syncOk ? 1 : 0,
                    entry.cameraValid ? 1 : 0, entry.heldJitter ? 1 : 0, entry.sceneCut ? 1 : 0,
                    entry.camMoveDist, entry.camRotDeg);
                ++r;
            }
            std::fclose(f);
        }
    }
}

// ----------------------------------------------------------------------------
// Dear ImGui Jitter Scope Visualization
// ----------------------------------------------------------------------------

static void drawSubpixelRadar(ImDrawList* draw, ImVec2 p0, float canvasSize, const JitterFrameLog& log) {
    ImVec2 p1 = ImVec2(p0.x + canvasSize, p0.y + canvasSize);

    // Background
    draw->AddRectFilled(p0, p1, IM_COL32(18, 22, 28, 240), 4.0f);
    draw->AddRect(p0, p1, IM_COL32(60, 70, 85, 255), 4.0f);

    const ImVec2 center = ImVec2(p0.x + canvasSize * 0.5f, p0.y + canvasSize * 0.5f);
    const float radius = canvasSize * 0.44f;

    // Center crosshairs
    draw->AddLine(ImVec2(center.x, p0.y + 4), ImVec2(center.x, p1.y - 4), IM_COL32(50, 62, 78, 220), 1.0f);
    draw->AddLine(ImVec2(p0.x + 4, center.y), ImVec2(p1.x - 4, center.y), IM_COL32(50, 62, 78, 220), 1.0f);

    // Reference [-0.5, +0.5] pixel box
    const float unitScale = radius / 0.707f;
    const ImVec2 b0 = ImVec2(center.x - 0.5f * unitScale, center.y - 0.5f * unitScale);
    const ImVec2 b1 = ImVec2(center.x + 0.5f * unitScale, center.y + 0.5f * unitScale);
    draw->AddRect(b0, b1, IM_COL32(80, 95, 115, 160), 0.0f, 0, 1.0f);

    // Reference Halton points for sequence
    const unsigned totalPhases = (log.totalPhases >= 2 && log.totalPhases <= 128) ? log.totalPhases : 8;
    std::vector<ImVec2> phasePoints(totalPhases);
    for (unsigned i = 0; i < totalPhases; ++i) {
        auto pJitter = edpe::projectionJitter(i, 2560, 1440, totalPhases);
        ImVec2 pt = ImVec2(center.x + pJitter.pixelX * unitScale, center.y + pJitter.pixelY * unitScale);
        phasePoints[i] = pt;
        draw->AddCircle(pt, 2.5f, IM_COL32(70, 110, 160, 180));
    }

    // Connect sequence in order
    for (unsigned i = 0; i < totalPhases; ++i) {
        unsigned next = (i + 1) % totalPhases;
        draw->AddLine(phasePoints[i], phasePoints[next], IM_COL32(40, 75, 110, 90), 1.0f);
    }

    // Current jitter dot
    ImVec2 currentPt = ImVec2(center.x + log.pixelX * unitScale, center.y + log.pixelY * unitScale);
    ImU32 dotColor = IM_COL32(40, 240, 80, 255); // Green = synced & active
    if (!log.cameraValid || (log.pixelX == 0.0f && log.pixelY == 0.0f)) {
        dotColor = IM_COL32(240, 50, 50, 255); // Red = zero / invalid
    } else if (log.heldJitter) {
        dotColor = IM_COL32(255, 190, 30, 255); // Yellow = held
    }

    // Glowing dot
    draw->AddCircleFilled(currentPt, 5.0f, dotColor);
    draw->AddCircle(currentPt, 8.0f, dotColor, 0, 1.5f);

    // Phase label
    char phaseBuf[16];
    std::snprintf(phaseBuf, sizeof(phaseBuf), "#%u", log.phase);
    draw->AddText(ImVec2(currentPt.x + 7.0f, currentPt.y - 7.0f), IM_COL32(240, 240, 240, 255), phaseBuf);

    ImGui::Dummy(ImVec2(canvasSize, canvasSize));
}

void JitterLogger::renderScopeWidget() {
    const JitterFrameLog log = getLatest();

    ImGui::TextColored(ImVec4(0.3f, 0.85f, 1.0f, 1.0f), "Subpixel Projection Jitter Scope & Diagnostics");
    ImGui::Separator();

    bool overlay_on = isOverlayVisible();
    if (ImGui::Checkbox("Show On-Screen Jitter Monitor HUD (Hotkey: F8)", &overlay_on)) {
        setOverlayVisible(overlay_on);
    }
    ImGui::SameLine();
    bool log_enabled = isLoggingEnabled();
    if (ImGui::Checkbox("Record to jitter.log (Async zero-overhead)", &log_enabled)) {
        setLoggingEnabled(log_enabled);
    }

    ImGui::Spacing();

    // Side-by-side: Radar on left, Metrics on right
    ImGui::BeginGroup();
    ImVec2 radarPos = ImGui::GetCursorScreenPos();
    drawSubpixelRadar(ImGui::GetWindowDrawList(), radarPos, 160.0f, log);
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::Text("Frame: %llu  |  Cycle: #%u / %u", log.frame, log.phase, log.totalPhases);

    if (log.syncOk && log.cameraValid) {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "[ACTIVE & SYNCED]");
    } else if (log.heldJitter) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[HELD - Camera Unchanged]");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "[DESYNC / ZERO JITTER]");
    }

    ImGui::Text("Injected Pixel: (%+.4f, %+.4f) px", log.pixelX, log.pixelY);
    ImGui::Text("Injected NDC:   (%+.6f, %+.6f)", log.ndcX, log.ndcY);
    ImGui::Text("Upscaler Input: (%+.4f, %+.4f) px", log.evalJx, log.evalJy);
    ImGui::Text("Writes this frame: %u  (Rejections: %u)", log.injectedWrites, log.rejections);
    ImGui::Text("Delta Frametime: %.2f ms", log.deltaMs);
    ImGui::Text("Camera: Move=%.4f m | Turn=%.3f deg", log.camMoveDist, log.camRotDeg);
    ImGui::EndGroup();

    ImGui::Spacing();

    // Waveform plots
    float historyX[240]{};
    float historyY[240]{};
    float historyDt[240]{};
    float historyWrites[240]{};
    size_t count = 0;
    getHistory(historyX, historyY, historyDt, historyWrites, 240, &count);

    if (count > 0) {
        char overlayX[64];
        std::snprintf(overlayX, sizeof(overlayX), "Current: %+.4f px", historyX[count - 1]);
        ImGui::PlotLines("Jitter X (Cyan)", historyX, static_cast<int>(count), 0, overlayX, -0.6f, 0.6f, ImVec2(0, 45));

        char overlayY[64];
        std::snprintf(overlayY, sizeof(overlayY), "Current: %+.4f px", historyY[count - 1]);
        ImGui::PlotLines("Jitter Y (Magenta)", historyY, static_cast<int>(count), 0, overlayY, -0.6f, 0.6f, ImVec2(0, 45));

        char overlayDt[64];
        std::snprintf(overlayDt, sizeof(overlayDt), "Frametime: %.1f ms", historyDt[count - 1]);
        ImGui::PlotLines("Frametime (Yellow)", historyDt, static_cast<int>(count), 0, overlayDt, 0.0f, 25.0f, ImVec2(0, 40));
    }

    ImGui::Spacing();
    ImGui::Text("jitter.log status: %u rows written to disk asynchronously", getWrittenRowCount());
    if (ImGui::Button("Flush to jitter.log now")) {
        flush();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear jitter.log")) {
        clearFile();
    }
}

void JitterLogger::renderOverlay(bool isMenuOpen) {
    if (!isOverlayVisible()) return;

    const JitterFrameLog log = getLatest();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 work_pos = viewport->WorkPos;
    const ImVec2 work_size = viewport->WorkSize;

    // Position at top-right
    ImGui::SetNextWindowPos(ImVec2(work_pos.x + work_size.x - 220.0f, work_pos.y + 40.0f),
                           isMenuOpen ? ImGuiCond_Appearing : ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.75f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav;
    if (!isMenuOpen) {
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
    }

    if (ImGui::Begin("##EDPE_JitterMonitorHUD", nullptr, flags)) {
        ImGui::TextColored(ImVec4(0.3f, 0.85f, 1.0f, 1.0f), "EDPE Jitter Monitor (F8)");
        ImGui::Separator();

        ImVec2 radarPos = ImGui::GetCursorScreenPos();
        drawSubpixelRadar(ImGui::GetWindowDrawList(), radarPos, 130.0f, log);

        ImGui::Spacing();
        ImGui::Text("#%u/%u  (%+.3f, %+.3f)", log.phase, log.totalPhases, log.pixelX, log.pixelY);
        ImGui::Text("Writes: %u  Dt: %.1fms", log.injectedWrites, log.deltaMs);
        if (log.syncOk && log.cameraValid) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "SYNC OK");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "DESYNC / HELD");
        }
    }
    ImGui::End();
}

} // namespace edpe
