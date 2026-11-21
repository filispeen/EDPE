#pragma once

#include "temporal_math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

#ifdef _WIN32
#include <d3d11_1.h>
#include <wrl/client.h>
#endif

namespace edpe {

inline constexpr uint32_t kMaxCelestialBodies = 16u;
inline constexpr uint32_t kStatusFlagSupercruise = 0x10u; // Status.json Flags bit 4
inline constexpr uint64_t kCelestialPatchVsHash = 0x72BDD292154158ADull;
inline constexpr float kCelestialSlackPx = 3.0f;

// GPU StructuredBuffer<CelestialRecord> element layout (96 bytes = 6 x float4).
struct CelestialRecord {
    float relRow0[4]{1.0f, 0.0f, 0.0f, 0.0f}; // view-space [R_rel_0 | t_rel_x] cur -> prev
    float relRow1[4]{0.0f, 1.0f, 0.0f, 0.0f}; // view-space [R_rel_1 | t_rel_y]
    float relRow2[4]{0.0f, 0.0f, 1.0f, 0.0f}; // view-space [R_rel_2 | t_rel_z]
    float box[4]{0.0f, 0.0f, 0.0f, 0.0f};     // screen AABB [minX, minY, maxX, maxY] in render px
    float span[4]{0.0f, 0.0f, 0.0f, 0.0f};    // x=depthMin, y=depthMax, z=rMinPx, w=rMaxPx
    float center[4]{0.0f, 0.0f, 0.0f, 0.0f};  // x=centerX, y=centerY (px), z=valid (1.0), w=0
};
static_assert(sizeof(CelestialRecord) == 96, "CelestialRecord must match HLSL layout");

// Parses Elite Dangerous Status.json for the Supercruise flag (Flags bit 4 = 0x10).
// Returns true if "Flags" was successfully parsed.
inline bool parseStatusJsonSupercruise(std::string_view json, bool* outSupercruise) {
    if (outSupercruise) *outSupercruise = false;
    const size_t keyPos = json.find("\"Flags\"");
    if (keyPos == std::string_view::npos) return false;
    const size_t colonPos = json.find(':', keyPos + 7);
    if (colonPos == std::string_view::npos) return false;
    size_t i = colonPos + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) {
        ++i;
    }
    if (i >= json.size() || json[i] < '0' || json[i] > '9') return false;
    uint64_t flags = 0;
    while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
        flags = flags * 10ull + static_cast<uint64_t>(json[i] - '0');
        ++i;
    }
    if (outSupercruise) *outSupercruise = (flags & kStatusFlagSupercruise) != 0ull;
    return true;
}

// Builds a CelestialRecord from current & previous view-space body poses [R|t]
// (row-major 3x4) plus screen-space bounding shell and depth span, adding 3px slack.
inline CelestialRecord buildCelestialRecord(
    const float (&bodyCurView)[12],
    const float (&bodyPrevView)[12],
    float minX, float minY, float maxX, float maxY,
    float depthMin, float depthMax,
    float centerX, float centerY,
    float rMinPx, float rMaxPx,
    float slackPx = kCelestialSlackPx)
{
    CelestialRecord rec{};
    // Relative transform mapping a point in current view space on the body
    // to its previous view-space position:
    //   p_local = R_cur^T * (p_cur - t_cur)
    //   p_prev  = R_prev * p_local + t_prev = (R_prev * R_cur^T) * p_cur + (t_prev - R_rel * t_cur)
    float rRel[9]{};
    for (unsigned r = 0; r < 3; ++r) {
        for (unsigned c = 0; c < 3; ++c) {
            rRel[r * 3 + c] =
                bodyPrevView[r * 4 + 0] * bodyCurView[c * 4 + 0] +
                bodyPrevView[r * 4 + 1] * bodyCurView[c * 4 + 1] +
                bodyPrevView[r * 4 + 2] * bodyCurView[c * 4 + 2];
        }
    }
    const float tCur[3] = {bodyCurView[3], bodyCurView[7], bodyCurView[11]};
    const float tPrev[3] = {bodyPrevView[3], bodyPrevView[7], bodyPrevView[11]};
    float* rows[3] = {rec.relRow0, rec.relRow1, rec.relRow2};
    for (unsigned r = 0; r < 3; ++r) {
        rows[r][0] = rRel[r * 3 + 0];
        rows[r][1] = rRel[r * 3 + 1];
        rows[r][2] = rRel[r * 3 + 2];
        rows[r][3] = tPrev[r] - (rRel[r * 3 + 0] * tCur[0] +
                                 rRel[r * 3 + 1] * tCur[1] +
                                 rRel[r * 3 + 2] * tCur[2]);
    }
    rec.box[0] = minX - slackPx;
    rec.box[1] = minY - slackPx;
    rec.box[2] = maxX + slackPx;
    rec.box[3] = maxY + slackPx;
    rec.span[0] = (std::max)(0.0f, depthMin);
    rec.span[1] = (std::min)(1.0f, depthMax);
    rec.span[2] = (std::max)(0.0f, rMinPx - slackPx);
    rec.span[3] = (std::max)(0.0f, rMaxPx + slackPx);
    rec.center[0] = centerX;
    rec.center[1] = centerY;
    rec.center[2] = 1.0f;
    rec.center[3] = 0.0f;
    return rec;
}

#ifdef _WIN32
// Supercruise planet/celestial body motion tracker (pure D3D11 + Status.json gate).
class CelestialMotionTracker {
public:
    bool initialize(ID3D11Device* device);
    void shutdown();

    // Non-blocking throttled poll of Elite Dangerous Status.json for Supercruise flag.
    void pollStatusJson();
    void setSupercruiseOverride(bool known, bool supercruise);
    bool isSupercruise() const { return statusKnown_ && inSupercruise_; }

    // Clear per-frame body records at frame start.
    void beginFrame();

    // Push a resolved body record (up to kMaxCelestialBodies = 16).
    bool pushRecord(const CelestialRecord& record);

    // Upload active records to GPU StructuredBuffer<CelestialRecord>.
    bool upload(ID3D11DeviceContext* context);

    ID3D11ShaderResourceView* recordsSrv() const { return count_ > 0 ? srv_.Get() : nullptr; }
    uint32_t recordCount() const { return count_; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;

    std::array<CelestialRecord, kMaxCelestialBodies> records_{};
    uint32_t count_ = 0;
    bool statusKnown_ = false;
    bool inSupercruise_ = false;
    uint64_t lastPollTickMs_ = 0;
    uint64_t lastFileWriteTime_ = 0;
};
#endif

} // namespace edpe
