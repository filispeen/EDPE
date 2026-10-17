#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <array>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace edpe {

struct JitterFrameLog {
    unsigned long long frame = 0;
    float timeMs = 0.0f;
    float deltaMs = 0.0f;
    unsigned phase = 0;
    unsigned totalPhases = 0;
    float pixelX = 0.0f;
    float pixelY = 0.0f;
    float ndcX = 0.0f;
    float ndcY = 0.0f;
    unsigned injectedWrites = 0;
    unsigned rejections = 0;
    float evalJx = 0.0f;
    float evalJy = 0.0f;
    bool syncOk = true;
    bool cameraValid = false;
    bool heldJitter = false;
    bool sceneCut = false;
    float camMoveDist = 0.0f;
    float camRotDeg = 0.0f;
};

class JitterLogger {
public:
    static JitterLogger& instance();

    void setLoggingEnabled(bool enabled);
    bool isLoggingEnabled() const;

    void setOverlayVisible(bool visible);
    bool isOverlayVisible() const;

    // Fast push called on Present thread (< 20ns, never blocks on disk I/O)
    void pushFrame(const JitterFrameLog& log);

    // Flushes buffered entries to jitter.log on disk
    void flush();

    // Clears jitter.log file on disk and resets counters
    void clearFile();

    // Returns count of rows written to jitter.log so far
    uint32_t getWrittenRowCount() const;

    // Rolling history for UI plotting (last 240 samples)
    void getHistory(float* outPixelX, float* outPixelY, float* outDeltaMs,
                    float* outWrites, size_t maxCount, size_t* outCount);

    JitterFrameLog getLatest();

    // Dear ImGui UI Rendering
    void renderScopeWidget();
    void renderOverlay(bool isMenuOpen);

    void shutdown();

private:
    JitterLogger();
    ~JitterLogger();

    void workerThreadLoop();
    std::wstring getLogFilePath() const;

    static constexpr size_t kRingSize = 32768;
    std::array<JitterFrameLog, kRingSize> ring_{};
    std::atomic<uint32_t> writeIdx_{0};
    std::atomic<uint32_t> readIdx_{0};
    std::atomic<uint32_t> writtenRows_{0};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> overlayVisible_{false};
    std::atomic<bool> running_{true};
    std::atomic<bool> flushRequested_{false};

    // UI rolling history (up to 240 points)
    static constexpr size_t kHistorySize = 240;
    std::mutex historyMutex_;
    float historyPixelX_[kHistorySize]{};
    float historyPixelY_[kHistorySize]{};
    float historyDeltaMs_[kHistorySize]{};
    float historyWrites_[kHistorySize]{};
    size_t historyHead_{0};
    size_t historyCount_{0};

    std::mutex latestMutex_;
    JitterFrameLog latest_{};

    // Asynchronous background writer
    std::thread workerThread_;
    std::mutex cvMutex_;
    std::condition_variable cv_;
};

} // namespace edpe
