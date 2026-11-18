#include "discord_rpc.h"
#include "config.h"
#include "log.h"

#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace edpe {

namespace {

// Optional compile-time fallback if [Discord] ClientId is not set in edpe.ini.
constexpr const char* kDefaultDiscordClientId = "";
constexpr const char* kEdpeRepoUrl = "https://github.com/filispeen/EDPE";
constexpr uint32_t kOpHandshake = 0;
constexpr uint32_t kOpFrame = 1;
constexpr uint32_t kOpClose = 2;

#pragma pack(push, 1)
struct IpcHeader {
    uint32_t opcode;
    uint32_t length;
};
#pragma pack(pop)

const char* upscaleModeLabel(UpscaleMode mode) {
    switch (mode) {
    case UpscaleMode::Native:           return "Native AA";
    case UpscaleMode::Quality:          return "Quality";
    case UpscaleMode::Balanced:         return "Balanced";
    case UpscaleMode::Performance:      return "Performance";
    case UpscaleMode::UltraPerformance: return "Ultra Performance";
    default:                            return "Native";
    }
}

std::string resolveClientId() {
    const std::string& configured = Config::instance().discordClientId;
    if (!configured.empty()) {
        return configured;
    }
    return kDefaultDiscordClientId;
}

} // namespace

std::string escapeJsonString(const std::string& input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char c : input) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8]{};
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

std::string formatDiscordDetails(const DiscordPresenceInfo& info) {
    std::string text;
    if (info.mode == UpscaleMode::Native || info.backend == UpscalerBackend::None) {
        text = "Native (Bypass)";
    } else {
        switch (info.backend) {
        case UpscalerBackend::NvidiaDlss:
            if (info.mode == UpscaleMode::NativeAA) {
                text = "NVIDIA DLAA";
            } else {
                text = "NVIDIA DLSS ";
                text += upscaleModeLabel(info.mode);
            }
            break;
        case UpscalerBackend::AmdFsr:
            if (info.mode == UpscaleMode::NativeAA) {
                text = "AMD FSR Native AA";
            } else {
                text = "AMD FSR 3 ";
                text += upscaleModeLabel(info.mode);
            }
            break;
        case UpscalerBackend::Taa:
            text = "Native TAA";
            break;
        case UpscalerBackend::None:
        default:
            text = "Native (Bypass)";
            break;
        }
    }

    if (info.outputWidth > 0 && info.outputHeight > 0) {
        char resBuf[64]{};
        if (info.renderWidth > 0 && info.renderHeight > 0 &&
            (info.renderWidth != info.outputWidth || info.renderHeight != info.outputHeight)) {
            std::snprintf(resBuf, sizeof(resBuf), " (%ux%u -> %ux%u)",
                info.renderWidth, info.renderHeight, info.outputWidth, info.outputHeight);
        } else {
            std::snprintf(resBuf, sizeof(resBuf), " (%ux%u)",
                info.outputWidth, info.outputHeight);
        }
        text += resBuf;
    }

    return text;
}

std::string formatDiscordState(const char* version) {
    if (!version || version[0] == '\0' || std::strcmp(version, "dev") == 0) {
        return "Elite Dangerous with EDPE";
    }
    std::string out = "Elite Dangerous with EDPE ";
    if (version[0] != 'v' && version[0] != 'V') {
        out.push_back('v');
    }
    out += version;
    return out;
}

std::string buildSetActivityPayload(uint32_t pid,
    uint64_t startTimestamp,
    uint64_t nonce,
    const std::string& details,
    const std::string& state) {
    char prefix[128]{};
    std::snprintf(prefix, sizeof(prefix),
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%u,\"activity\":{", pid);

    std::string json = prefix;
    json += "\"details\":\"";
    json += escapeJsonString(details);
    json += "\",\"state\":\"";
    json += escapeJsonString(state);
    json += "\",\"timestamps\":{\"start\":";
    json += std::to_string(startTimestamp);
    json += "},\"assets\":{\"large_image\":\"edpe_logo\",\"large_text\":\"EDPE - Elite Dangerous Performance Enhanced\"}";
    json += ",\"buttons\":[{\"label\":\"Get EDPE on GitHub\",\"url\":\"";
    json += kEdpeRepoUrl;
    json += "\"}]}},\"nonce\":\"";
    json += std::to_string(nonce);
    json += "\"}";
    return json;
}

std::string buildClearActivityPayload(uint32_t pid, uint64_t nonce) {
    char buf[160]{};
    std::snprintf(buf, sizeof(buf),
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%u,\"activity\":null},\"nonce\":\"%llu\"}",
        pid, static_cast<unsigned long long>(nonce));
    return std::string(buf);
}

DiscordRpc& DiscordRpc::instance() {
    static DiscordRpc s_instance;
    return s_instance;
}

DiscordRpc::~DiscordRpc() {
    shutdown(false);
}

void DiscordRpc::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return;
    started_ = true;
    stopRequested_.store(false, std::memory_order_release);
    startTimestamp_ = static_cast<uint64_t>(std::time(nullptr));
    desiredInfo_.enabled = Config::instance().discordRpcEnabled;
    dirty_ = true;
    worker_ = std::thread(&DiscordRpc::workerLoop, this);
}

void DiscordRpc::shutdown(bool waitForThread) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return;
        stopRequested_.store(true, std::memory_order_release);
    }
    cv_.notify_all();

    if (waitForThread && worker_.joinable()) {
        worker_.join();
    } else if (worker_.joinable()) {
        worker_.detach();
    }
}

void DiscordRpc::update(const DiscordPresenceInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return;
    if (info == desiredInfo_) return;
    desiredInfo_ = info;
    dirty_ = true;
    cv_.notify_one();
}

DiscordRpcStatus DiscordRpc::status() const {
    return status_.load(std::memory_order_relaxed);
}

const char* DiscordRpc::statusText() const {
    switch (status_.load(std::memory_order_relaxed)) {
    case DiscordRpcStatus::Disabled:          return "Disabled";
    case DiscordRpcStatus::NoClientId:        return "Missing Client ID";
    case DiscordRpcStatus::WaitingForDiscord: return "Waiting for Discord";
    case DiscordRpcStatus::Connected:         return "Connected";
    default:                                  return "Unknown";
    }
}

void DiscordRpc::workerLoop() {
    auto lastConnectAttempt = std::chrono::steady_clock::time_point{};
    bool hadLoggedClientIdMissing = false;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        DiscordPresenceInfo currentInfo;
        bool shouldUpdate = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(2), [&] {
                return stopRequested_.load(std::memory_order_acquire) || dirty_;
            });
            if (stopRequested_.load(std::memory_order_acquire)) break;
            currentInfo = desiredInfo_;
            shouldUpdate = dirty_;
            dirty_ = false;
        }

        if (!currentInfo.enabled) {
            if (pipeHandle_ != nullptr) {
                disconnect(true);
            }
            status_.store(DiscordRpcStatus::Disabled, std::memory_order_relaxed);
            continue;
        }

        const std::string clientId = resolveClientId();
        if (clientId.empty()) {
            if (pipeHandle_ != nullptr) {
                disconnect(true);
            }
            status_.store(DiscordRpcStatus::NoClientId, std::memory_order_relaxed);
            if (!hadLoggedClientIdMissing) {
                EdpeLog(L"EDPE: Discord RPC enabled but [Discord] ClientId is not set in edpe.ini");
                hadLoggedClientIdMissing = true;
            }
            continue;
        }
        hadLoggedClientIdMissing = false;

        if (pipeHandle_ == nullptr) {
            const auto now = std::chrono::steady_clock::now();
            if (lastConnectAttempt == std::chrono::steady_clock::time_point{} ||
                now - lastConnectAttempt >= std::chrono::seconds(8)) {
                lastConnectAttempt = now;
                if (tryConnect(clientId)) {
                    status_.store(DiscordRpcStatus::Connected, std::memory_order_relaxed);
                    shouldUpdate = true;
                    EdpeLog(L"EDPE: Discord RPC connected");
                } else {
                    status_.store(DiscordRpcStatus::WaitingForDiscord, std::memory_order_relaxed);
                }
            } else {
                status_.store(DiscordRpcStatus::WaitingForDiscord, std::memory_order_relaxed);
            }
        }

        if (pipeHandle_ != nullptr) {
            if (!drainIncoming()) {
                disconnect(false);
                status_.store(DiscordRpcStatus::WaitingForDiscord, std::memory_order_relaxed);
                continue;
            }

            if (shouldUpdate) {
                if (!sendActivity(currentInfo)) {
                    disconnect(false);
                    status_.store(DiscordRpcStatus::WaitingForDiscord, std::memory_order_relaxed);
                }
            }
        }
    }

    if (pipeHandle_ != nullptr) {
        disconnect(true);
    }
}

bool DiscordRpc::tryConnect(const std::string& clientId) {
    for (int i = 0; i < 10; ++i) {
        wchar_t pipePath[64]{};
        swprintf_s(pipePath, L"\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileW(pipePath, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }

        pipeHandle_ = h;
        const std::string handshake = "{\"v\":1,\"client_id\":\"" + escapeJsonString(clientId) + "\"}";
        if (!sendFrame(kOpHandshake, handshake)) {
            CloseHandle(h);
            pipeHandle_ = nullptr;
            continue;
        }

        // Wait briefly for Discord's READY frame so the pipe is fully established.
        for (int attempt = 0; attempt < 10; ++attempt) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
                CloseHandle(h);
                pipeHandle_ = nullptr;
                break;
            }
            if (avail >= sizeof(IpcHeader)) {
                if (!drainIncoming()) {
                    pipeHandle_ = nullptr;
                }
                break;
            }
            Sleep(25);
        }

        if (pipeHandle_ != nullptr) {
            return true;
        }
    }
    return false;
}

void DiscordRpc::disconnect(bool sendClear) {
    HANDLE h = static_cast<HANDLE>(pipeHandle_);
    if (!h) return;

    if (sendClear) {
        sendFrame(kOpFrame, buildClearActivityPayload(GetCurrentProcessId(), ++nonceCounter_));
        sendFrame(kOpClose, "{}");
    }
    CloseHandle(h);
    pipeHandle_ = nullptr;
}

bool DiscordRpc::drainIncoming() {
    HANDLE h = static_cast<HANDLE>(pipeHandle_);
    if (!h) return false;

    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) {
            return false;
        }
        if (avail < sizeof(IpcHeader)) {
            return true;
        }

        IpcHeader hdr{};
        DWORD bytesRead = 0;
        if (!ReadFile(h, &hdr, sizeof(hdr), &bytesRead, nullptr) || bytesRead != sizeof(hdr)) {
            return false;
        }

        if (hdr.length > 0) {
            if (hdr.length > 1024 * 1024) {
                return false;
            }
            std::string discard(hdr.length, '\0');
            DWORD totalRead = 0;
            while (totalRead < hdr.length) {
                DWORD chunk = 0;
                if (!ReadFile(h, discard.data() + totalRead, hdr.length - totalRead, &chunk, nullptr) || chunk == 0) {
                    return false;
                }
                totalRead += chunk;
            }
        }

        if (hdr.opcode == kOpClose) {
            return false;
        }
    }
}

bool DiscordRpc::sendFrame(uint32_t opcode, const std::string& json) {
    HANDLE h = static_cast<HANDLE>(pipeHandle_);
    if (!h) return false;

    IpcHeader hdr{opcode, static_cast<uint32_t>(json.size())};
    DWORD written = 0;
    if (!WriteFile(h, &hdr, sizeof(hdr), &written, nullptr) || written != sizeof(hdr)) {
        return false;
    }
    if (!json.empty()) {
        DWORD totalWritten = 0;
        const DWORD target = static_cast<DWORD>(json.size());
        while (totalWritten < target) {
            DWORD chunk = 0;
            if (!WriteFile(h, json.data() + totalWritten, target - totalWritten, &chunk, nullptr) || chunk == 0) {
                return false;
            }
            totalWritten += chunk;
        }
    }
    return true;
}

bool DiscordRpc::sendActivity(const DiscordPresenceInfo& info) {
    const std::string details = formatDiscordDetails(info);
    const std::string state = formatDiscordState(kVersion);
    const std::string payload = buildSetActivityPayload(
        GetCurrentProcessId(), startTimestamp_, ++nonceCounter_, details, state);
    return sendFrame(kOpFrame, payload);
}

} // namespace edpe
