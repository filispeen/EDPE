#pragma once

#include "temporal_inputs.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace edpe {

struct DiscordPresenceInfo {
    bool enabled = true;
    UpscalerBackend backend = UpscalerBackend::None;
    UpscaleMode mode = UpscaleMode::Native;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;

    bool operator==(const DiscordPresenceInfo& other) const = default;
};

enum class DiscordRpcStatus {
    Disabled,
    NoClientId,
    WaitingForDiscord,
    Connected
};

// Pure formatting helpers (exposed for deterministic unit testing).
std::string escapeJsonString(const std::string& input);
std::string formatDiscordDetails(const DiscordPresenceInfo& info);
std::string formatDiscordState(const char* version);
std::string buildSetActivityPayload(uint32_t pid,
    uint64_t startTimestamp,
    uint64_t nonce,
    const std::string& details,
    const std::string& state);
std::string buildClearActivityPayload(uint32_t pid, uint64_t nonce);

class DiscordRpc {
public:
    static DiscordRpc& instance();

    void start();
    void shutdown(bool waitForThread);
    void update(const DiscordPresenceInfo& info);

    DiscordRpcStatus status() const;
    const char* statusText() const;

private:
    DiscordRpc() = default;
    ~DiscordRpc();

    DiscordRpc(const DiscordRpc&) = delete;
    DiscordRpc& operator=(const DiscordRpc&) = delete;

    void workerLoop();
    bool tryConnect(const std::string& clientId);
    void disconnect(bool sendClear);
    bool drainIncoming();
    bool sendFrame(uint32_t opcode, const std::string& json);
    bool sendActivity(const DiscordPresenceInfo& info);

    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool started_ = false;
    std::atomic<bool> stopRequested_{false};
    std::atomic<DiscordRpcStatus> status_{DiscordRpcStatus::Disabled};

    DiscordPresenceInfo desiredInfo_{};
    bool dirty_ = false;
    uint64_t startTimestamp_ = 0;
    uint64_t nonceCounter_ = 0;

    void* pipeHandle_ = nullptr; // HANDLE (avoid <windows.h> in header)
};

} // namespace edpe
