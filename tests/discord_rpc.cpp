#include "discord_rpc.h"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
    using namespace edpe;

    // JSON escaping
    assert(escapeJsonString("hello \"world\"\\test\n") == "hello \\\"world\\\"\\\\test\\n");

    // State formatting
    assert(formatDiscordState(nullptr) == "dev");
    assert(formatDiscordState("") == "dev");
    assert(formatDiscordState("dev") == "dev");
    assert(formatDiscordState("0.5.0") == "v0.5.0");
    assert(formatDiscordState("v0.5.0") == "v0.5.0");

    // Details formatting
    {
        DiscordPresenceInfo info;
        info.backend = UpscalerBackend::NvidiaDlss;
        info.mode = UpscaleMode::Quality;
        info.renderWidth = 1707;
        info.renderHeight = 960;
        info.outputWidth = 2560;
        info.outputHeight = 1440;
        assert(formatDiscordDetails(info) == "NVIDIA DLSS Quality (1707x960 -> 2560x1440)");
    }
    {
        DiscordPresenceInfo info;
        info.backend = UpscalerBackend::NvidiaDlss;
        info.mode = UpscaleMode::NativeAA;
        info.renderWidth = 2560;
        info.renderHeight = 1440;
        info.outputWidth = 2560;
        info.outputHeight = 1440;
        assert(formatDiscordDetails(info) == "NVIDIA DLAA (2560x1440)");
    }
    {
        DiscordPresenceInfo info;
        info.backend = UpscalerBackend::AmdFsr;
        info.mode = UpscaleMode::Balanced;
        info.renderWidth = 1485;
        info.renderHeight = 835;
        info.outputWidth = 2560;
        info.outputHeight = 1440;
        assert(formatDiscordDetails(info) == "AMD FSR 3 Balanced (1485x835 -> 2560x1440)");
    }
    {
        DiscordPresenceInfo info;
        info.backend = UpscalerBackend::Taa;
        info.mode = UpscaleMode::NativeAA;
        info.outputWidth = 1920;
        info.outputHeight = 1080;
        assert(formatDiscordDetails(info) == "Native TAA (1920x1080)");
    }
    {
        DiscordPresenceInfo info;
        info.backend = UpscalerBackend::NvidiaDlss;
        info.mode = UpscaleMode::Native;
        info.outputWidth = 3840;
        info.outputHeight = 2160;
        assert(formatDiscordDetails(info) == "Native (Bypass) (3840x2160)");
    }

    // Payload structure checks
    {
        const std::string payload = buildSetActivityPayload(
            4242, 1700000000ULL, 7,
            "NVIDIA DLSS Quality (1707x960 -> 2560x1440)",
            "v0.5.0");
        assert(payload.find("\"cmd\":\"SET_ACTIVITY\"") != std::string::npos);
        assert(payload.find("\"pid\":4242") != std::string::npos);
        assert(payload.find("\"start\":1700000000") != std::string::npos);
        assert(payload.find("\"state\":\"v0.5.0\"") != std::string::npos);
        assert(payload.find("\"large_image\":\"https://cdn2.steamgriddb.com/icon/b353d984bb7fcc173edea010bd999b98/32/128x128.png\"") != std::string::npos);
        assert(payload.find("\"label\":\"Download from Nexus mods\"") != std::string::npos);
        assert(payload.find("\"url\":\"https://nexusmods.com/elitedangerous/mods/15\"") != std::string::npos);
        assert(payload.find("\"label\":\"Download from GitHub\"") != std::string::npos);
        assert(payload.find("\"url\":\"https://github.com/filispeen/EDPE\"") != std::string::npos);
        assert(payload.find("\"nonce\":\"7\"") != std::string::npos);
    }
    {
        const std::string clearPayload = buildClearActivityPayload(4242, 8);
        assert(clearPayload == "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":4242,\"activity\":null},\"nonce\":\"8\"}");
    }

    std::printf("discord_rpc_test passed\n");
    return 0;
}
