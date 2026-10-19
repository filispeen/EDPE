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

} // namespace edpe
