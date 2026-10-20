#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
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
    uint64_t droppedFrames = 0;
};

struct JitterViewFrameMeta {
    unsigned long long frame = 0;
    unsigned long long renderedFrameId = 0;
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
    uint32_t inputFormat = 0;
    uint32_t texW = 0;
    uint32_t texH = 0;
    uint32_t inW = 0;
    uint32_t inH = 0;
    bool historyRecreated = false;
    bool performReset = false;
    bool motionNull = false;
    bool depthNull = false;
    float sensitivity = 0.0f;
    float threshold = 0.0f;
    int mode = 0;
    uint32_t sceneW = 0;
    uint32_t sceneH = 0;
    uint64_t lumaSumQ10 = 0;
    double lumaMean = -1.0;
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

inline uint32_t computeDominantPeriod(const std::vector<double>& series, uint32_t minLag = 2, uint32_t maxLag = 240) {
    const size_t n = series.size();
    if (n < minLag + 2) return 0;

    double sum = 0.0;
    for (double v : series) sum += v;
    const double mean = sum / static_cast<double>(n);

    double var = 0.0;
    for (double v : series) {
        const double d = v - mean;
        var += d * d;
    }
    if (var <= 1e-12) return 0;

    const uint32_t upperLag = static_cast<uint32_t>(std::min<size_t>(maxLag, n - 2));
    if (upperLag < minLag) return 0;

    std::vector<double> r(upperLag + 2, 0.0);
    for (uint32_t k = 1; k <= upperLag + 1; ++k) {
        double c = 0.0;
        for (size_t i = 0; i < n - k; ++i) {
            c += (series[i] - mean) * (series[i + k] - mean);
        }
        r[k] = c / (static_cast<double>(n - k) * (var / static_cast<double>(n)));
    }

    struct Peak {
        double rVal;
        uint32_t lag;
    };
    std::vector<Peak> peaks;
    for (uint32_t k = minLag; k <= upperLag; ++k) {
        if (r[k] > r[k - 1] && r[k] >= r[k + 1] && r[k] > 0.0) {
            peaks.push_back({ r[k], k });
        }
    }
    if (peaks.empty()) return 0;

    double maxR = peaks[0].rVal;
    for (const auto& p : peaks) {
        if (p.rVal > maxR) maxR = p.rVal;
    }

    std::vector<Peak> prominent;
    for (const auto& p : peaks) {
        if (p.rVal >= 0.7 * maxR) {
            prominent.push_back(p);
        }
    }
    std::sort(prominent.begin(), prominent.end(), [](const Peak& a, const Peak& b) {
        return a.lag < b.lag;
    });

    return prominent.empty() ? 0 : prominent[0].lag;
}

inline const char* getDxgiFormatName(uint32_t fmt) {
    switch (fmt) {
    case 10: return "R16G16B16A16_FLOAT";
    case 24: return "R10G10B10A2_UNORM";
    case 26: return "R11G11B10_FLOAT";
    case 28: return "R8G8B8A8_UNORM";
    case 29: return "R8G8B8A8_UNORM_SRGB";
    case 87: return "B8G8R8A8_UNORM";
    case 91: return "B8G8R8A8_UNORM_SRGB";
    default: return nullptr;
    }
}

inline double pearson(const std::vector<double>& x, const std::vector<double>& y) {
    const size_t n = x.size();
    if (n < 2 || n != y.size()) return 0.0;

    double sumX = 0.0;
    double sumY = 0.0;
    for (size_t i = 0; i < n; ++i) {
        sumX += x[i];
        sumY += y[i];
    }
    const double meanX = sumX / static_cast<double>(n);
    const double meanY = sumY / static_cast<double>(n);

    double varX = 0.0;
    double varY = 0.0;
    double covXY = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double dx = x[i] - meanX;
        const double dy = y[i] - meanY;
        varX += dx * dx;
        varY += dy * dy;
        covXY += dx * dy;
    }

    if (varX <= 1e-12 || varY <= 1e-12) return 0.0;

    const double r = covXY / std::sqrt(varX * varY);
    return std::clamp(r, -1.0, 1.0);
}

inline std::string generateJitterViewReport(
    const JitterViewHeaderInfo& header,
    const std::vector<JitterViewFrameData>& frames) {

    std::string out;
    out.reserve(90000);

    char buf[1024];
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
        "# green_rule: g - max(r, b) > 0.04\n"
        "# luma_rule: dot(norm(c), float3(0.299, 0.587, 0.114)), HDR: c/(1+c), LDR: saturate, Q10 clamped sum\n",
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

    out.append("# frame, rendered_frame_id, t_ms, delta_ms, phase, phases, jx, jy, green_pct, green_px, centroid_x, centroid_y, bbox_x0, bbox_y0, bbox_x1, bbox_y1, tiles_active, tiles_flip, cam_move_m, cam_rot_deg, cam_valid, held, scene_cut, luma_mean, in_fmt, scene_w, scene_h, mv_null, depth_null, hist_realloc, sens, thr, mode\n");

    std::vector<JitterViewFrameStats> allStats;
    allStats.reserve(frames.size());

    std::vector<std::array<bool, kJitterViewGridTiles>> frameActive(frames.size());
    std::array<uint32_t, kJitterViewGridTiles> tileFlipsTotal{};
    std::array<double, kJitterViewGridTiles> tileFractionSum{};

    std::vector<double> greenPctSeries;
    greenPctSeries.reserve(frames.size());

    for (size_t i = 0; i < frames.size(); ++i) {
        const auto* prev = (i > 0) ? &frameActive[i - 1] : nullptr;
        JitterViewFrameStats s = computeFrameStats(frames[i], header.renderWidth, header.renderHeight, prev, frameActive[i]);
        allStats.push_back(s);
        greenPctSeries.push_back(s.greenPct);

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
            "%llu, %llu, %.2f, %.2f, %u, %u, %.4f, %.4f, %.6f, %u, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %u, %u, %.4f, %.3f, %d, %d, %d, %.6f, %u, %u, %u, %d, %d, %d, %.2f, %.3f, %d\n",
            m.frame, m.renderedFrameId, m.timeMs, m.deltaMs, m.phase, m.totalPhases, m.pixelX, m.pixelY,
            s.greenPct, s.greenPx, s.centroidX, s.centroidY,
            s.bboxX0, s.bboxY0, s.bboxX1, s.bboxY1,
            s.tilesActive, s.tilesFlip,
            m.camMoveDist, m.camRotDeg,
            m.cameraValid ? 1 : 0, m.heldJitter ? 1 : 0, m.sceneCut ? 1 : 0,
            m.lumaMean, m.inputFormat, m.sceneW, m.sceneH,
            m.motionNull ? 1 : 0, m.depthNull ? 1 : 0, m.historyRecreated ? 1 : 0,
            m.sensitivity, m.threshold, m.mode);
        out.append(buf);
    }

    for (size_t i = 0; i < frames.size(); i += 30) {
        const unsigned long long fid = frames[i].meta.renderedFrameId ? frames[i].meta.renderedFrameId : frames[i].meta.frame;
        std::snprintf(buf, sizeof(buf), "# MAP frame=%llu\n", fid);
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

    std::set<unsigned long long> uniqueFrameIds;
    size_t duplicateFrames = 0;
    std::set<unsigned> phasesSeen;
    unsigned long long frameIdMin = 0;
    unsigned long long frameIdMax = 0;

    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& s = allStats[i];
        const auto& m = frames[i].meta;

        const unsigned long long fid = m.renderedFrameId ? m.renderedFrameId : m.frame;
        if (uniqueFrameIds.count(fid)) {
            duplicateFrames++;
        } else {
            uniqueFrameIds.insert(fid);
        }
        phasesSeen.insert(m.phase);

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

    if (!uniqueFrameIds.empty()) {
        frameIdMin = *uniqueFrameIds.begin();
        frameIdMax = *uniqueFrameIds.rbegin();
    }
    const size_t uniqueFrames = uniqueFrameIds.size();

    const double greenAvg = allStats.empty() ? 0.0 : (greenSum / static_cast<double>(allStats.size()));
    const double flipsAvg = allStats.empty() ? 0.0 : (flipsSum / static_cast<double>(allStats.size()));

    double varSum = 0.0;
    for (const auto& s : allStats) {
        const double diff = s.greenPct - greenAvg;
        varSum += diff * diff;
    }
    const double greenStddev = allStats.empty() ? 0.0 : std::sqrt(varSum / static_cast<double>(allStats.size()));
    const double spikeThreshold = greenAvg + 2.0 * greenStddev;

    out.append("# SPIKES\n");
    out.append("# t_ms, frame, phase, green_pct\n");
    size_t spikeCount = 0;
    for (size_t i = 0; i < frames.size() && spikeCount < 100; ++i) {
        if (allStats[i].greenPct > spikeThreshold) {
            const unsigned long long fid = frames[i].meta.renderedFrameId ? frames[i].meta.renderedFrameId : frames[i].meta.frame;
            std::snprintf(buf, sizeof(buf), "%.2f, %llu, %u, %.6f\n",
                frames[i].meta.timeMs, fid, frames[i].meta.phase, allStats[i].greenPct);
            out.append(buf);
            spikeCount++;
        }
    }

    float maxTMs = 0.0f;
    for (const auto& f : frames) {
        if (f.meta.timeMs > maxTMs) maxTMs = f.meta.timeMs;
    }
    const size_t numBuckets = (frames.empty()) ? 0 : (static_cast<size_t>(maxTMs / 500.0f) + 1);

    struct SecondBucket {
        double greenSum = 0.0;
        double greenMax = 0.0;
        size_t count = 0;
    };
    std::vector<SecondBucket> buckets(numBuckets);
    for (size_t i = 0; i < frames.size(); ++i) {
        const float t = frames[i].meta.timeMs;
        size_t bIdx = static_cast<size_t>(t / 500.0f);
        if (bIdx >= numBuckets) bIdx = numBuckets - 1;
        buckets[bIdx].greenSum += allStats[i].greenPct;
        if (buckets[bIdx].count == 0 || allStats[i].greenPct > buckets[bIdx].greenMax) {
            buckets[bIdx].greenMax = allStats[i].greenPct;
        }
        buckets[bIdx].count++;
    }

    out.append("# SECOND BUCKETS\n");
    out.append("# bucket_start_s, bucket_end_s, avg_green_pct, max_green_pct, frames\n");
    for (size_t b = 0; b < buckets.size(); ++b) {
        const double bStart = static_cast<double>(b) * 0.5;
        const double bEnd = static_cast<double>(b + 1) * 0.5;
        const double bAvg = (buckets[b].count > 0) ? (buckets[b].greenSum / static_cast<double>(buckets[b].count)) : 0.0;
        std::snprintf(buf, sizeof(buf), "%.1f, %.1f, %.6f, %.6f, %zu\n",
            bStart, bEnd, bAvg, buckets[b].greenMax, buckets[b].count);
        out.append(buf);
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

    const uint32_t dominantPeriodFrames = computeDominantPeriod(greenPctSeries, 2, 240);
    const double dominantPeriodS = (avgFps > 0.0 && dominantPeriodFrames > 0) ? (static_cast<double>(dominantPeriodFrames) / avgFps) : 0.0;
    const double dominantFreqHz = (dominantPeriodS > 0.0) ? (1.0 / dominantPeriodS) : 0.0;

    std::snprintf(buf, sizeof(buf),
        "# frames: %zu\n"
        "# duration_s: %.3f\n"
        "# avg_fps: %.2f\n"
        "# unique_frames: %zu\n"
        "# duplicate_frames: %zu\n"
        "# dropped_frames: %llu\n"
        "# frame_id_min: %llu\n"
        "# frame_id_max: %llu\n"
        "# phases_seen: %zu\n"
        "# dominant_period_frames: %u\n"
        "# dominant_period_s: %.3f\n"
        "# dominant_freq_hz: %.2f\n"
        "# green_pct_min: %.6f\n"
        "# green_pct_avg: %.6f\n"
        "# green_pct_max: %.6f\n"
        "# tiles_flip_avg: %.2f\n"
        "# tiles_flip_max: %u\n",
        frames.size(),
        durationS,
        avgFps,
        uniqueFrames,
        duplicateFrames,
        header.droppedFrames,
        frameIdMin,
        frameIdMax,
        phasesSeen.size(),
        dominantPeriodFrames,
        dominantPeriodS,
        dominantFreqHz,
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

    size_t mvNullCount = 0;
    size_t depthNullCount = 0;
    size_t histReallocCount = 0;
    std::set<uint32_t> uniqueFormats;

    std::vector<double> validGreen;
    std::vector<double> validLuma;
    validGreen.reserve(frames.size());
    validLuma.reserve(frames.size());

    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& m = frames[i].meta;
        if (m.motionNull) mvNullCount++;
        if (m.depthNull) depthNullCount++;
        if (m.historyRecreated) histReallocCount++;
        uniqueFormats.insert(m.inputFormat);

        if (m.lumaMean >= 0.0) {
            validGreen.push_back(allStats[i].greenPct);
            validLuma.push_back(m.lumaMean);
        }
    }

    std::vector<double> pairGreen;
    std::vector<double> pairAbsDluma;
    for (size_t i = 1; i < frames.size(); ++i) {
        if (frames[i].meta.lumaMean >= 0.0 && frames[i - 1].meta.lumaMean >= 0.0) {
            pairGreen.push_back(allStats[i].greenPct);
            pairAbsDluma.push_back(std::abs(frames[i].meta.lumaMean - frames[i - 1].meta.lumaMean));
        }
    }

    double lumaMin = -1.0;
    double lumaMax = -1.0;
    double lumaAvg = -1.0;
    if (!validLuma.empty()) {
        lumaMin = validLuma[0];
        lumaMax = validLuma[0];
        double lumaSum = 0.0;
        for (double v : validLuma) {
            if (v < lumaMin) lumaMin = v;
            if (v > lumaMax) lumaMax = v;
            lumaSum += v;
        }
        lumaAvg = lumaSum / static_cast<double>(validLuma.size());
    }

    const uint32_t lumaDominantPeriodFrames = (!validLuma.empty()) ? computeDominantPeriod(validLuma, 2, 240) : 0;
    const double lumaDominantPeriodS = (avgFps > 0.0 && lumaDominantPeriodFrames > 0) ? (static_cast<double>(lumaDominantPeriodFrames) / avgFps) : 0.0;
    const double lumaDominantFreqHz = (lumaDominantPeriodS > 0.0) ? (1.0 / lumaDominantPeriodS) : 0.0;

    const double pearsonGreenLuma = pearson(validGreen, validLuma);
    const double pearsonGreenAbsDluma = pearson(pairGreen, pairAbsDluma);

    std::string uniqueInFmtStr;
    for (uint32_t fmt : uniqueFormats) {
        if (!uniqueInFmtStr.empty()) uniqueInFmtStr += ", ";
        const char* name = getDxgiFormatName(fmt);
        if (name) {
            uniqueInFmtStr += std::to_string(fmt) + " (" + name + ")";
        } else {
            uniqueInFmtStr += std::to_string(fmt);
        }
    }
    if (uniqueInFmtStr.empty()) uniqueInFmtStr = "none";

    std::snprintf(buf, sizeof(buf),
        "# reset_frames: %zu\n"
        "# scene_cut_frames: %zu\n"
        "# held_jitter_frames: %zu\n"
        "# mv_null_frames: %zu\n"
        "# depth_null_frames: %zu\n"
        "# hist_realloc_frames: %zu\n"
        "# unique_in_fmt: %s\n"
        "# luma_min: %.6f\n"
        "# luma_avg: %.6f\n"
        "# luma_max: %.6f\n"
        "# luma_dominant_period_frames: %u\n"
        "# luma_dominant_freq_hz: %.2f\n"
        "# pearson_green_vs_luma: %.4f\n"
        "# pearson_green_vs_abs_dluma: %.4f\n",
        resetCount,
        sceneCutCount,
        heldCount,
        mvNullCount,
        depthNullCount,
        histReallocCount,
        uniqueInFmtStr.c_str(),
        lumaMin,
        lumaAvg,
        lumaMax,
        lumaDominantPeriodFrames,
        lumaDominantFreqHz,
        pearsonGreenLuma,
        pearsonGreenAbsDluma);
    out.append(buf);

    return out;
}

}
