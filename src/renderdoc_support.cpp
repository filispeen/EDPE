#include "renderdoc_support.h"
#include "log.h"
#include "renderdoc_app.h"

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <windows.h>

namespace edpe {

namespace {
std::atomic<bool> g_available{false};
std::atomic<bool> g_annotationsEnabled{true};
std::atomic<bool> g_loggedInit{false};
std::mutex g_rdocMutex;

const char* backendLabel(int backend) {
    switch (backend) {
    case 1: return "NVIDIA DLSS/DLAA";
    case 2: return "AMD FSR";
    case 3: return "Native TAA";
    default: return "Native (Bypass)";
    }
}

const char* modeLabel(int mode) {
    switch (mode) {
    case 1: return "NativeAA";
    case 2: return "Quality";
    case 3: return "Balanced";
    case 4: return "Performance";
    case 5: return "UltraPerformance";
    default: return "Native";
    }
}

RENDERDOC_API_1_7_0* queryRenderDocApi(HMODULE rdocMod) {
    if (!rdocMod) return nullptr;
    const auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(
        GetProcAddress(rdocMod, "RENDERDOC_GetAPI"));
    if (!getApi) return nullptr;

    static constexpr RENDERDOC_Version kVersions[] = {
        eRENDERDOC_API_Version_1_7_0,
        eRENDERDOC_API_Version_1_6_0,
        eRENDERDOC_API_Version_1_5_0,
        eRENDERDOC_API_Version_1_4_0,
        eRENDERDOC_API_Version_1_2_0,
        eRENDERDOC_API_Version_1_1_2,
        eRENDERDOC_API_Version_1_0_0,
    };
    for (RENDERDOC_Version ver : kVersions) {
        void* ptr = nullptr;
        if (getApi(ver, &ptr) == 1 && ptr) {
            return static_cast<RENDERDOC_API_1_7_0*>(ptr);
        }
    }
    return nullptr;
}
} // namespace

RenderDocBridge& RenderDocBridge::instance() {
    static RenderDocBridge s_bridge;
    return s_bridge;
}

bool RenderDocBridge::refresh() {
    std::lock_guard<std::mutex> lock(g_rdocMutex);
    const HMODULE rdocMod = GetModuleHandleW(L"renderdoc.dll");
    if (!rdocMod) {
        api_ = nullptr;
        apiMajor_ = apiMinor_ = apiPatch_ = 0;
        g_available.store(false, std::memory_order_release);
        return false;
    }
    if (!api_) {
        api_ = queryRenderDocApi(rdocMod);
        if (!api_) {
            g_available.store(false, std::memory_order_release);
            return false;
        }
        if (api_->GetAPIVersion) {
            api_->GetAPIVersion(&apiMajor_, &apiMinor_, &apiPatch_);
        }
        lastAnnotatedCaptureCount_ = api_->GetNumCaptures ? api_->GetNumCaptures() : 0;
        g_available.store(true, std::memory_order_release);
        if (!g_loggedInit.exchange(true, std::memory_order_acq_rel)) {
            wchar_t msg[128]{};
            swprintf_s(msg, L"EDPE: RenderDoc detected (API v%d.%d.%d)",
                apiMajor_, apiMinor_, apiPatch_);
            EdpeLog(msg);
        }
    }
    return true;
}

bool RenderDocBridge::isAvailable() const {
    return g_available.load(std::memory_order_acquire);
}

bool RenderDocBridge::areAnnotationsEnabled() const {
    return g_annotationsEnabled.load(std::memory_order_relaxed);
}

void RenderDocBridge::setAnnotationsEnabled(bool enabled) {
    g_annotationsEnabled.store(enabled, std::memory_order_relaxed);
}

bool RenderDocBridge::areAnnotationsActive() const {
    return g_available.load(std::memory_order_acquire) &&
           g_annotationsEnabled.load(std::memory_order_relaxed);
}

void RenderDocBridge::setActiveWindow(ID3D11Device* device, void* hwnd) {
    if (!device || !hwnd) return;
    std::lock_guard<std::mutex> lock(g_rdocMutex);
    if (api_ && api_->SetActiveWindow) {
        api_->SetActiveWindow(device, hwnd);
    }
}

bool RenderDocBridge::triggerCapture(uint32_t numFrames) {
    std::lock_guard<std::mutex> lock(g_rdocMutex);
    if (!api_) return false;
    if (numFrames > 1 && api_->TriggerMultiFrameCapture) {
        api_->TriggerMultiFrameCapture(numFrames);
        return true;
    }
    if (api_->TriggerCapture) {
        api_->TriggerCapture();
        return true;
    }
    return false;
}

bool RenderDocBridge::isFrameCapturing() const {
    std::lock_guard<std::mutex> lock(g_rdocMutex);
    return (api_ && api_->IsFrameCapturing) ? (api_->IsFrameCapturing() != 0) : false;
}

bool RenderDocBridge::isTargetControlConnected() const {
    std::lock_guard<std::mutex> lock(g_rdocMutex);
    return (api_ && api_->IsTargetControlConnected) ? (api_->IsTargetControlConnected() != 0) : false;
}

uint32_t RenderDocBridge::getNumCaptures() const {
    std::lock_guard<std::mutex> lock(g_rdocMutex);
    return (api_ && api_->GetNumCaptures) ? api_->GetNumCaptures() : 0u;
}

bool RenderDocBridge::getCapturePath(uint32_t index, std::string& outPath, uint64_t* outTimestamp) const {
    outPath.clear();
    if (outTimestamp) *outTimestamp = 0;
    std::lock_guard<std::mutex> lock(g_rdocMutex);
    if (!api_ || !api_->GetCapture) return false;
    uint32_t pathLen = 0;
    uint64_t ts = 0;
    if (api_->GetCapture(index, nullptr, &pathLen, &ts) == 0 || pathLen == 0) {
        return false;
    }
    std::string buf(pathLen, '\0');
    if (api_->GetCapture(index, buf.data(), &pathLen, &ts) == 0) {
        return false;
    }
    while (!buf.empty() && buf.back() == '\0') buf.pop_back();
    outPath = std::move(buf);
    if (outTimestamp) *outTimestamp = ts;
    return true;
}

void RenderDocBridge::setCaptureComments(const char* filePath, const char* comments) {
    if (!comments) return;
    std::lock_guard<std::mutex> lock(g_rdocMutex);
    if (api_ && api_->SetCaptureFileComments &&
        (apiMajor_ > 1 || (apiMajor_ == 1 && apiMinor_ >= 2))) {
        api_->SetCaptureFileComments(filePath, comments);
    }
}

void RenderDocBridge::setCaptureFilePathTemplate(const char* pathTemplate) {
    if (!pathTemplate) return;
    std::lock_guard<std::mutex> lock(g_rdocMutex);
    if (api_ && api_->SetCaptureFilePathTemplate) {
        api_->SetCaptureFilePathTemplate(pathTemplate);
    }
}

uint32_t RenderDocBridge::launchOrShowReplayUI() {
    std::lock_guard<std::mutex> lock(g_rdocMutex);
    if (!api_) return 0;
    if (api_->IsTargetControlConnected && api_->IsTargetControlConnected()) {
        if (api_->ShowReplayUI && (apiMajor_ > 1 || (apiMajor_ == 1 && apiMinor_ >= 5))) {
            return api_->ShowReplayUI();
        }
        return 0;
    }
    if (api_->LaunchReplayUI) {
        return api_->LaunchReplayUI(1, nullptr);
    }
    return 0;
}

bool RenderDocBridge::isOverlayVisible() const {
    std::lock_guard<std::mutex> lock(g_rdocMutex);
    if (!api_ || !api_->GetOverlayBits) return false;
    return (api_->GetOverlayBits() & eRENDERDOC_Overlay_Enabled) != 0;
}

void RenderDocBridge::setOverlayVisible(bool visible) {
    std::lock_guard<std::mutex> lock(g_rdocMutex);
    if (!api_ || !api_->MaskOverlayBits) return;
    if (visible) {
        api_->MaskOverlayBits(~0u, eRENDERDOC_Overlay_Enabled);
    } else {
        api_->MaskOverlayBits(~static_cast<uint32_t>(eRENDERDOC_Overlay_Enabled), 0u);
    }
}

void RenderDocBridge::onFramePresented(const char* edpeVersion,
                                       int upscalerBackend,
                                       int upscaleMode,
                                       uint32_t renderWidth,
                                       uint32_t renderHeight,
                                       uint32_t outputWidth,
                                       uint32_t outputHeight) {
    if (!g_available.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_rdocMutex);
    if (!api_ || !api_->GetNumCaptures) return;
    const uint32_t count = api_->GetNumCaptures();
    if (count > lastAnnotatedCaptureCount_) {
        lastAnnotatedCaptureCount_ = count;
        if (api_->SetCaptureFileComments &&
            (apiMajor_ > 1 || (apiMajor_ == 1 && apiMinor_ >= 2))) {
            char comments[512]{};
            std::snprintf(comments, sizeof(comments),
                "EDPE %s\nUpscaler: %s (%s)\nRender: %ux%u\nOutput: %ux%u",
                edpeVersion ? edpeVersion : "unknown",
                backendLabel(upscalerBackend),
                modeLabel(upscaleMode),
                renderWidth, renderHeight,
                outputWidth, outputHeight);
            api_->SetCaptureFileComments(nullptr, comments);
        }
    }
}

void RenderDocBridge::resetForTest() {
    std::lock_guard<std::mutex> lock(g_rdocMutex);
    api_ = nullptr;
    apiMajor_ = apiMinor_ = apiPatch_ = 0;
    lastAnnotatedCaptureCount_ = 0;
    g_available.store(false, std::memory_order_release);
    g_annotationsEnabled.store(true, std::memory_order_relaxed);
    g_loggedInit.store(false, std::memory_order_release);
}

} // namespace edpe
