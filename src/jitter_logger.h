#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <array>
#include <atomic>
#include <thread>
#include <mutex>
#include <windows.h>

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

    // Controls how often frames are sampled into jitter.log (1 = every frame, N = every Nth frame)
    void setSampleInterval(uint32_t everyNFrames);
    uint32_t getSampleInterval() const;

    // Controls how often the background thread flushes buffered entries to disk (ms)
    void setFlushIntervalMs(uint32_t intervalMs);
    uint32_t getFlushIntervalMs() const;

    // Fast lock-free check before building JitterFrameLog on the Present thread
    bool shouldSampleFrame(unsigned long long frame, bool forceOnAnomaly = false) const;

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

    void shutdown();

private:
    JitterLogger();
    ~JitterLogger();

    void ensureWorkerStarted();
    void workerThreadLoop();
    std::wstring getLogFilePath() const;

    static constexpr size_t kRingSize = 32768;
    std::array<JitterFrameLog, kRingSize> ring_{};
    std::atomic<uint32_t> writeIdx_{0};
    std::atomic<uint32_t> readIdx_{0};
    std::atomic<uint32_t> writtenRows_{0};
    std::atomic<bool> enabled_{false};
    std::atomic<uint32_t> sampleInterval_{10};
    std::atomic<uint32_t> flushIntervalMs_{1000};
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

    // Asynchronous background writer (started lazily only when DEV=1 and logging is enabled)
    std::mutex workerStartMutex_;
    bool workerStarted_{false};
    std::thread workerThread_;
    SRWLOCK cvLock_ = SRWLOCK_INIT;
    CONDITION_VARIABLE cv_ = CONDITION_VARIABLE_INIT;
};

} // namespace edpe
