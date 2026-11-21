#include "celestial_motion.h"
#include "renderdoc_support.h"

#include <shlobj.h>
#include <fstream>
#include <string>

namespace edpe {

namespace {
std::wstring getStatusJsonPath() {
    PWSTR savedGames = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &savedGames)) || !savedGames) {
        return {};
    }
    std::wstring path(savedGames);
    CoTaskMemFree(savedGames);
    path += L"\\Frontier Developments\\Elite Dangerous\\Status.json";
    return path;
}
} // namespace

bool CelestialMotionTracker::initialize(ID3D11Device* device) {
    if (!device || device_) return false;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = kMaxCelestialBodies * sizeof(CelestialRecord);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(CelestialRecord);
    if (FAILED(device->CreateBuffer(&desc, nullptr, &buffer_))) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = kMaxCelestialBodies;
    if (FAILED(device->CreateShaderResourceView(buffer_.Get(), &srvDesc, &srv_))) {
        buffer_.Reset();
        return false;
    }
    SetD3D11DebugName(buffer_.Get(), "EDPE_CelestialRecords");
    device_ = device;
    return true;
}

void CelestialMotionTracker::shutdown() {
    srv_.Reset();
    buffer_.Reset();
    device_.Reset();
    count_ = 0;
    statusKnown_ = false;
    inSupercruise_ = false;
}

void CelestialMotionTracker::setSupercruiseOverride(bool known, bool supercruise) {
    statusKnown_ = known;
    inSupercruise_ = supercruise;
    if (!isSupercruise()) {
        count_ = 0;
    }
}

void CelestialMotionTracker::pollStatusJson() {
    const uint64_t nowMs = GetTickCount64();
    if (lastPollTickMs_ != 0 && (nowMs - lastPollTickMs_) < 250ull) {
        return;
    }
    lastPollTickMs_ = nowMs;

    static const std::wstring kPath = getStatusJsonPath();
    if (kPath.empty()) return;

    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(kPath.c_str(), GetFileExInfoStandard, &attr)) {
        return;
    }
    const uint64_t writeTime =
        (static_cast<uint64_t>(attr.ftLastWriteTime.dwHighDateTime) << 32) |
        static_cast<uint64_t>(attr.ftLastWriteTime.dwLowDateTime);
    if (writeTime == lastFileWriteTime_ && statusKnown_) {
        return;
    }
    lastFileWriteTime_ = writeTime;

    std::ifstream f(kPath, std::ios::binary);
    if (!f) return;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    bool sc = false;
    if (parseStatusJsonSupercruise(content, &sc)) {
        statusKnown_ = true;
        inSupercruise_ = sc;
    }
}

void CelestialMotionTracker::beginFrame() {
    count_ = 0;
}

bool CelestialMotionTracker::pushRecord(const CelestialRecord& record) {
    if (!isSupercruise() || count_ >= kMaxCelestialBodies) return false;
    records_[count_++] = record;
    return true;
}

bool CelestialMotionTracker::upload(ID3D11DeviceContext* context) {
    if (!context || !buffer_ || count_ == 0 || !isSupercruise()) return false;
    context->UpdateSubresource(buffer_.Get(), 0, nullptr, records_.data(), 0, 0);
    return true;
}

} // namespace edpe
