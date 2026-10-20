#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace edpe {

inline constexpr uint32_t kJitterViewGridCols = 64;
inline constexpr uint32_t kJitterViewGridRows = 36;
inline constexpr uint32_t kJitterViewGridTiles = kJitterViewGridCols * kJitterViewGridRows;
inline constexpr const char* kJitterViewAsciiRamp = " .:-=+*#%@";
inline constexpr size_t kJitterViewAsciiRampLen = 10;

struct JitterViewHeaderInfo {
    std::string edpeVersion;
    std::string date;
    int mode = 0;
    std::string modeName;
    float sensitivity = 1.5f;
    float threshold = 0.02f;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t outputWidth = 0;
    uint32_t outputHeight = 0;
    std::string upscalerBackend;
    std::string upscaleMode;
    bool isHdr = false;
};

struct JitterViewFrameMeta {
    unsigned long long frame = 0;
    float timeMs = 0.0f;
    float deltaMs = 0.0f;
    unsigned phase = 0;
    unsigned totalPhases = 0;
    float pixelX = 0.0f;
    float pixelY = 0.0f;
    float camMoveDist = 0.0f;
    float camRotDeg = 0.0f;
    bool cameraValid = false;
    bool heldJitter = false;
    bool sceneCut = false;
    bool reset = false;
};

struct JitterViewFrameData {
    JitterViewFrameMeta meta;
    std::array<uint32_t, kJitterViewGridTiles> tiles{};
};

struct JitterViewFrameStats {
    double greenPct = 0.0;
    uint32_t greenPx = 0;
    double centroidX = 0.0;
    double centroidY = 0.0;
    double bboxX0 = 0.0;
    double bboxY0 = 0.0;
    double bboxX1 = 0.0;
    double bboxY1 = 0.0;
    uint32_t tilesActive = 0;
    uint32_t tilesFlip = 0;
};

inline char getJitterViewAsciiChar(double fraction) {
    if (fraction <= 0.0) return kJitterViewAsciiRamp[0];
    const int idx = std::min(9, std::max(0, static_cast<int>(std::sqrt(fraction) * 9.99)));
    return kJitterViewAsciiRamp[idx];
}

inline uint32_t getTilePixelCount(uint32_t tx, uint32_t ty, uint32_t width, uint32_t height) {
    const uint32_t x0 = tx * width / kJitterViewGridCols;
    const uint32_t x1 = (tx + 1) * width / kJitterViewGridCols;
    const uint32_t y0 = ty * height / kJitterViewGridRows;
    const uint32_t y1 = (ty + 1) * height / kJitterViewGridRows;
    return (x1 - x0) * (y1 - y0);
}

inline JitterViewFrameStats computeFrameStats(
    const JitterViewFrameData& frame,
    uint32_t renderWidth,
    uint32_t renderHeight,
    const std::array<bool, kJitterViewGridTiles>* prevActiveTiles,
    std::array<bool, kJitterViewGridTiles>& outActiveTiles) {

    JitterViewFrameStats stats{};
    const uint32_t totalPixels = renderWidth * renderHeight;
    uint32_t totalGreenPx = 0;
    double weightedCx = 0.0;
    double weightedCy = 0.0;
    uint32_t activeCount = 0;
    uint32_t flipCount = 0;

    uint32_t minActiveTx = kJitterViewGridCols;
    uint32_t maxActiveTx = 0;
    uint32_t minActiveTy = kJitterViewGridRows;
    uint32_t maxActiveTy = 0;

    uint32_t minGreenTx = kJitterViewGridCols;
    uint32_t maxGreenTx = 0;
    uint32_t minGreenTy = kJitterViewGridRows;
    uint32_t maxGreenTy = 0;

    for (uint32_t ty = 0; ty < kJitterViewGridRows; ++ty) {
        for (uint32_t tx = 0; tx < kJitterViewGridCols; ++tx) {
            const uint32_t idx = ty * kJitterViewGridCols + tx;
            const uint32_t count = frame.tiles[idx];
            const uint32_t tilePix = getTilePixelCount(tx, ty, renderWidth, renderHeight);
            const double fraction = (tilePix > 0) ? (static_cast<double>(count) / tilePix) : 0.0;
            const bool isActive = (fraction >= 0.02);

            outActiveTiles[idx] = isActive;
            if (isActive) {
                activeCount++;
                if (tx < minActiveTx) minActiveTx = tx;
                if (tx > maxActiveTx) maxActiveTx = tx;
                if (ty < minActiveTy) minActiveTy = ty;
                if (ty > maxActiveTy) maxActiveTy = ty;
            }

            if (prevActiveTiles && isActive != (*prevActiveTiles)[idx]) {
                flipCount++;
            }

            if (count > 0) {
                totalGreenPx += count;
                const double cx = (static_cast<double>(tx) + 0.5) * (100.0 / static_cast<double>(kJitterViewGridCols));
                const double cy = (static_cast<double>(ty) + 0.5) * (100.0 / static_cast<double>(kJitterViewGridRows));
                weightedCx += static_cast<double>(count) * cx;
                weightedCy += static_cast<double>(count) * cy;

                if (tx < minGreenTx) minGreenTx = tx;
                if (tx > maxGreenTx) maxGreenTx = tx;
                if (ty < minGreenTy) minGreenTy = ty;
                if (ty > maxGreenTy) maxGreenTy = ty;
            }
        }
    }

    stats.greenPx = totalGreenPx;
    stats.greenPct = (totalPixels > 0) ? (static_cast<double>(totalGreenPx) / totalPixels) : 0.0;
    stats.tilesActive = activeCount;
    stats.tilesFlip = flipCount;

    if (totalGreenPx > 0) {
        stats.centroidX = weightedCx / static_cast<double>(totalGreenPx);
        stats.centroidY = weightedCy / static_cast<double>(totalGreenPx);
    }

    if (activeCount > 0) {
        stats.bboxX0 = static_cast<double>(minActiveTx) * (100.0 / static_cast<double>(kJitterViewGridCols));
        stats.bboxX1 = static_cast<double>(maxActiveTx + 1) * (100.0 / static_cast<double>(kJitterViewGridCols));
        stats.bboxY0 = static_cast<double>(minActiveTy) * (100.0 / static_cast<double>(kJitterViewGridRows));
        stats.bboxY1 = static_cast<double>(maxActiveTy + 1) * (100.0 / static_cast<double>(kJitterViewGridRows));
    } else if (totalGreenPx > 0) {
        stats.bboxX0 = static_cast<double>(minGreenTx) * (100.0 / static_cast<double>(kJitterViewGridCols));
        stats.bboxX1 = static_cast<double>(maxGreenTx + 1) * (100.0 / static_cast<double>(kJitterViewGridCols));
        stats.bboxY0 = static_cast<double>(minGreenTy) * (100.0 / static_cast<double>(kJitterViewGridRows));
        stats.bboxY1 = static_cast<double>(maxGreenTy + 1) * (100.0 / static_cast<double>(kJitterViewGridRows));
    }

    return stats;
}

inline std::string generateJitterViewReport(
    const JitterViewHeaderInfo& header,
    const std::vector<JitterViewFrameData>& frames) {

    std::string out;
    out.reserve(70000);

    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "# edpe_version: %s\n"
        "# date: %s\n"
        "# mode: %d (%s)\n"
        "# sensitivity: %.2f\n"
        "# threshold: %.3f\n"
        "# render_resolution: %ux%u\n"
        "# output_resolution: %ux%u\n"
        "# upscaler_backend: %s\n"
        "# upscale_mode: %s\n"
        "# is_hdr: %s\n"
        "# grid: %ux%u\n"
        "# green_rule: g - max(r, b) > 0.04\n",
        header.edpeVersion.c_str(),
        header.date.c_str(),
        header.mode,
        header.modeName.c_str(),
        header.sensitivity,
        header.threshold,
        header.renderWidth,
        header.renderHeight,
        header.outputWidth,
        header.outputHeight,
        header.upscalerBackend.c_str(),
        header.upscaleMode.c_str(),
        header.isHdr ? "true" : "false",
        kJitterViewGridCols,
        kJitterViewGridRows);
    out.append(buf);

    out.append("# frame, t_ms, delta_ms, phase, phases, jx, jy, green_pct, green_px, centroid_x, centroid_y, bbox_x0, bbox_y0, bbox_x1, bbox_y1, tiles_active, tiles_flip, cam_move_m, cam_rot_deg, cam_valid, held, scene_cut\n");

    std::vector<JitterViewFrameStats> allStats;
    allStats.reserve(frames.size());

    std::vector<std::array<bool, kJitterViewGridTiles>> frameActive(frames.size());
    std::array<uint32_t, kJitterViewGridTiles> tileFlipsTotal{};
    std::array<double, kJitterViewGridTiles> tileFractionSum{};

    for (size_t i = 0; i < frames.size(); ++i) {
        const auto* prev = (i > 0) ? &frameActive[i - 1] : nullptr;
        JitterViewFrameStats s = computeFrameStats(frames[i], header.renderWidth, header.renderHeight, prev, frameActive[i]);
        allStats.push_back(s);

        for (uint32_t ty = 0; ty < kJitterViewGridRows; ++ty) {
            for (uint32_t tx = 0; tx < kJitterViewGridCols; ++tx) {
                const uint32_t idx = ty * kJitterViewGridCols + tx;
                const uint32_t pix = getTilePixelCount(tx, ty, header.renderWidth, header.renderHeight);
                const double fraction = (pix > 0) ? (static_cast<double>(frames[i].tiles[idx]) / pix) : 0.0;
                tileFractionSum[idx] += fraction;

                if (i > 0 && frameActive[i][idx] != frameActive[i - 1][idx]) {
                    tileFlipsTotal[idx]++;
                }
            }
        }

        const auto& m = frames[i].meta;
        std::snprintf(buf, sizeof(buf),
            "%llu, %.2f, %.2f, %u, %u, %.4f, %.4f, %.6f, %u, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %u, %u, %.4f, %.3f, %d, %d, %d\n",
            m.frame, m.timeMs, m.deltaMs, m.phase, m.totalPhases, m.pixelX, m.pixelY,
            s.greenPct, s.greenPx, s.centroidX, s.centroidY,
            s.bboxX0, s.bboxY0, s.bboxX1, s.bboxY1,
            s.tilesActive, s.tilesFlip,
            m.camMoveDist, m.camRotDeg,
            m.cameraValid ? 1 : 0, m.heldJitter ? 1 : 0, m.sceneCut ? 1 : 0);
        out.append(buf);
    }

    for (size_t i = 0; i < frames.size(); i += 30) {
        std::snprintf(buf, sizeof(buf), "# MAP frame=%llu\n", frames[i].meta.frame);
        out.append(buf);
        for (uint32_t ty = 0; ty < kJitterViewGridRows; ++ty) {
            char rowBuf[kJitterViewGridCols + 2];
            for (uint32_t tx = 0; tx < kJitterViewGridCols; ++tx) {
                const uint32_t idx = ty * kJitterViewGridCols + tx;
                const uint32_t pix = getTilePixelCount(tx, ty, header.renderWidth, header.renderHeight);
                const double fraction = (pix > 0) ? (static_cast<double>(frames[i].tiles[idx]) / pix) : 0.0;
                rowBuf[tx] = getJitterViewAsciiChar(fraction);
            }
            rowBuf[kJitterViewGridCols] = '\n';
            rowBuf[kJitterViewGridCols + 1] = '\0';
            out.append(rowBuf);
        }
    }

    out.append("# PERSISTENCE MAP\n");
    const double frameCountDbl = frames.empty() ? 1.0 : static_cast<double>(frames.size());
    for (uint32_t ty = 0; ty < kJitterViewGridRows; ++ty) {
        char rowBuf[kJitterViewGridCols + 2];
        for (uint32_t tx = 0; tx < kJitterViewGridCols; ++tx) {
            const uint32_t idx = ty * kJitterViewGridCols + tx;
            const double avgFrac = frames.empty() ? 0.0 : (tileFractionSum[idx] / frameCountDbl);
            rowBuf[tx] = getJitterViewAsciiChar(avgFrac);
        }
        rowBuf[kJitterViewGridCols] = '\n';
        rowBuf[kJitterViewGridCols + 1] = '\0';
        out.append(rowBuf);
    }

    out.append("# FLICKER MAP\n");
    const double flipDenom = (frames.size() > 1) ? static_cast<double>(frames.size() - 1) : 1.0;
    for (uint32_t ty = 0; ty < kJitterViewGridRows; ++ty) {
        char rowBuf[kJitterViewGridCols + 2];
        for (uint32_t tx = 0; tx < kJitterViewGridCols; ++tx) {
            const uint32_t idx = ty * kJitterViewGridCols + tx;
            const double flickerFrac = (frames.size() > 1) ? (static_cast<double>(tileFlipsTotal[idx]) / flipDenom) : 0.0;
            rowBuf[tx] = getJitterViewAsciiChar(flickerFrac);
        }
        rowBuf[kJitterViewGridCols] = '\n';
        rowBuf[kJitterViewGridCols + 1] = '\0';
        out.append(rowBuf);
    }

    out.append("# SUMMARY\n");
    double durationS = 0.0;
    if (frames.size() > 1) {
        const float t0 = frames.front().meta.timeMs;
        const float t1 = frames.back().meta.timeMs;
        if (t1 > t0) {
            durationS = (t1 - t0) * 0.001;
        } else {
            double totalDelta = 0.0;
            for (size_t i = 1; i < frames.size(); ++i) {
                totalDelta += frames[i].meta.deltaMs;
            }
            durationS = totalDelta * 0.001;
        }
    }
    const double avgFps = (durationS > 0.0) ? (static_cast<double>(frames.size()) / durationS) : 0.0;

    double greenMin = 0.0;
    double greenMax = 0.0;
    double greenSum = 0.0;
    double flipsSum = 0.0;
    uint32_t flipsMax = 0;
    size_t resetCount = 0;
    size_t sceneCutCount = 0;
    size_t heldCount = 0;

    struct PhaseAccum {
        double sumGreen = 0.0;
        size_t count = 0;
    };
    std::map<unsigned, PhaseAccum> phaseStats;

    if (!allStats.empty()) {
        greenMin = allStats[0].greenPct;
        greenMax = allStats[0].greenPct;
    }

    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& s = allStats[i];
        const auto& m = frames[i].meta;

        if (s.greenPct < greenMin) greenMin = s.greenPct;
        if (s.greenPct > greenMax) greenMax = s.greenPct;
        greenSum += s.greenPct;

        flipsSum += s.tilesFlip;
        if (s.tilesFlip > flipsMax) flipsMax = s.tilesFlip;

        if (m.reset) resetCount++;
        if (m.sceneCut) sceneCutCount++;
        if (m.heldJitter) heldCount++;

        auto& pa = phaseStats[m.phase];
        pa.sumGreen += s.greenPct;
        pa.count++;
    }

    const double greenAvg = allStats.empty() ? 0.0 : (greenSum / static_cast<double>(allStats.size()));
    const double flipsAvg = allStats.empty() ? 0.0 : (flipsSum / static_cast<double>(allStats.size()));

    std::snprintf(buf, sizeof(buf),
        "# frames: %zu\n"
        "# duration_s: %.3f\n"
        "# avg_fps: %.2f\n"
        "# green_pct_min: %.6f\n"
        "# green_pct_avg: %.6f\n"
        "# green_pct_max: %.6f\n"
        "# tiles_flip_avg: %.2f\n"
        "# tiles_flip_max: %u\n",
        frames.size(),
        durationS,
        avgFps,
        greenMin,
        greenAvg,
        greenMax,
        flipsAvg,
        flipsMax);
    out.append(buf);

    for (const auto& [phase, pa] : phaseStats) {
        const double avgPhaseGreen = (pa.count > 0) ? (pa.sumGreen / static_cast<double>(pa.count)) : 0.0;
        std::snprintf(buf, sizeof(buf), "phase=%u avg_green_pct=%.6f frames=%zu\n",
            phase, avgPhaseGreen, pa.count);
        out.append(buf);
    }

    std::snprintf(buf, sizeof(buf),
        "# reset_frames: %zu\n"
        "# scene_cut_frames: %zu\n"
        "# held_jitter_frames: %zu\n",
        resetCount,
        sceneCutCount,
        heldCount);
    out.append(buf);

    return out;
}

}
