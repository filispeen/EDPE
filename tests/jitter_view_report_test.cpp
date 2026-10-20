#include "jitter_view_report.h"

#include <cmath>
#include <string>
#include <vector>

int main() {
    if (edpe::getJitterViewAsciiChar(0.0) != ' ') return 1;
    if (edpe::getJitterViewAsciiChar(-0.5) != ' ') return 2;
    if (edpe::getJitterViewAsciiChar(1.0) != '@') return 3;
    if (edpe::getJitterViewAsciiChar(2.0) != '@') return 4;
    if (edpe::getJitterViewAsciiChar(0.02) != '.') return 5;

    const uint32_t w = 2560;
    const uint32_t h = 1440;
    uint32_t totalPixelsSum = 0;
    for (uint32_t ty = 0; ty < edpe::kJitterViewGridRows; ++ty) {
        for (uint32_t tx = 0; tx < edpe::kJitterViewGridCols; ++tx) {
            totalPixelsSum += edpe::getTilePixelCount(tx, ty, w, h);
        }
    }
    if (totalPixelsSum != w * h) return 6;

    const uint32_t w2 = 1920;
    const uint32_t h2 = 1080;
    uint32_t totalPixelsSum2 = 0;
    for (uint32_t ty = 0; ty < edpe::kJitterViewGridRows; ++ty) {
        for (uint32_t tx = 0; tx < edpe::kJitterViewGridCols; ++tx) {
            totalPixelsSum2 += edpe::getTilePixelCount(tx, ty, w2, h2);
        }
    }
    if (totalPixelsSum2 != w2 * h2) return 7;

    edpe::JitterViewFrameData emptyFrame{};
    std::array<bool, edpe::kJitterViewGridTiles> active0{};
    edpe::JitterViewFrameStats s0 = edpe::computeFrameStats(emptyFrame, w, h, nullptr, active0);
    if (s0.greenPx != 0 || s0.greenPct != 0.0 || s0.tilesActive != 0 || s0.tilesFlip != 0) return 8;
    if (s0.centroidX != 0.0 || s0.centroidY != 0.0) return 9;
    if (s0.bboxX0 != 0.0 || s0.bboxY0 != 0.0 || s0.bboxX1 != 0.0 || s0.bboxY1 != 0.0) return 10;

    edpe::JitterViewFrameData frame1{};
    const uint32_t targetTx = 16;
    const uint32_t targetTy = 9;
    const uint32_t targetIdx = targetTy * edpe::kJitterViewGridCols + targetTx;
    const uint32_t tilePix = edpe::getTilePixelCount(targetTx, targetTy, w, h);
    const uint32_t greenCount = static_cast<uint32_t>(tilePix * 0.05);
    frame1.tiles[targetIdx] = greenCount;

    std::array<bool, edpe::kJitterViewGridTiles> active1{};
    edpe::JitterViewFrameStats s1 = edpe::computeFrameStats(frame1, w, h, nullptr, active1);
    if (s1.greenPx != greenCount) return 11;
    if (s1.tilesActive != 1) return 12;
    if (s1.tilesFlip != 0) return 13;
    if (!active1[targetIdx]) return 14;

    const double expectedCx = (targetTx + 0.5) * (100.0 / 64.0);
    const double expectedCy = (targetTy + 0.5) * (100.0 / 36.0);
    if (std::abs(s1.centroidX - expectedCx) > 1e-4) return 15;
    if (std::abs(s1.centroidY - expectedCy) > 1e-4) return 16;

    const double expectedX0 = targetTx * (100.0 / 64.0);
    const double expectedX1 = (targetTx + 1) * (100.0 / 64.0);
    const double expectedY0 = targetTy * (100.0 / 36.0);
    const double expectedY1 = (targetTy + 1) * (100.0 / 36.0);
    if (std::abs(s1.bboxX0 - expectedX0) > 1e-4) return 17;
    if (std::abs(s1.bboxX1 - expectedX1) > 1e-4) return 18;
    if (std::abs(s1.bboxY0 - expectedY0) > 1e-4) return 19;
    if (std::abs(s1.bboxY1 - expectedY1) > 1e-4) return 20;

    edpe::JitterViewFrameData frame2{};
    const uint32_t otherTx = 32;
    const uint32_t otherTy = 18;
    const uint32_t otherIdx = otherTy * edpe::kJitterViewGridCols + otherTx;
    const uint32_t otherPix = edpe::getTilePixelCount(otherTx, otherTy, w, h);
    frame2.tiles[otherIdx] = static_cast<uint32_t>(otherPix * 0.05);

    std::array<bool, edpe::kJitterViewGridTiles> active2{};
    edpe::JitterViewFrameStats s2 = edpe::computeFrameStats(frame2, w, h, &active1, active2);
    if (s2.tilesActive != 1) return 21;
    if (s2.tilesFlip != 2) return 22;

    edpe::JitterViewHeaderInfo header{};
    header.edpeVersion = "v0.4.6";
    header.date = "2026-10-20 17:00:00";
    header.mode = 0;
    header.modeName = "Hybrid";
    header.sensitivity = 1.5f;
    header.threshold = 0.02f;
    header.renderWidth = w;
    header.renderHeight = h;
    header.outputWidth = w;
    header.outputHeight = h;
    header.upscalerBackend = "None";
    header.upscaleMode = "Native";
    header.isHdr = false;
    header.droppedFrames = 0;

    std::vector<edpe::JitterViewFrameData> frames200(200);
    float prevT = -1.0f;
    for (size_t i = 0; i < frames200.size(); ++i) {
        frames200[i].meta.frame = 1000 + i;
        frames200[i].meta.renderedFrameId = 1000 + i;
        frames200[i].meta.timeMs = static_cast<float>(i * 16.666);
        frames200[i].meta.deltaMs = (i > 0) ? 16.666f : 0.0f;
        frames200[i].meta.phase = static_cast<unsigned>(i % 32);
        frames200[i].meta.totalPhases = 32;
        frames200[i].meta.cameraValid = true;

        if (frames200[i].meta.timeMs <= prevT) return 23;
        prevT = frames200[i].meta.timeMs;

        const double wave = (std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(i) / 50.0) + 1.0) * 0.5;
        const double frac = 0.01 + 0.05 * wave;
        frames200[i].tiles[targetIdx] = static_cast<uint32_t>(tilePix * frac);
    }

    const std::string report = edpe::generateJitterViewReport(header, frames200);
    if (report.find("# edpe_version: v0.4.6") == std::string::npos) return 24;
    if (report.find("# grid: 64x36") == std::string::npos) return 25;
    if (report.find("# green_rule: g - max(r, b) > 0.04") == std::string::npos) return 26;
    if (report.find("# frame, rendered_frame_id, t_ms, delta_ms") == std::string::npos) return 27;
    if (report.find("# MAP frame=1000\n") == std::string::npos) return 28;
    if (report.find("# MAP frame=1030\n") == std::string::npos) return 29;
    if (report.find("# PERSISTENCE MAP\n") == std::string::npos) return 30;
    if (report.find("# FLICKER MAP\n") == std::string::npos) return 31;
    if (report.find("# SPIKES\n") == std::string::npos) return 32;
    if (report.find("# SECOND BUCKETS\n") == std::string::npos) return 33;
    if (report.find("# SUMMARY\n") == std::string::npos) return 34;
    if (report.find("# unique_frames: 200") == std::string::npos) return 35;
    if (report.find("# duplicate_frames: 0") == std::string::npos) return 36;
    if (report.find("# dominant_period_frames: 50") == std::string::npos) return 37;
    if (report.find("# dominant_period_s:") == std::string::npos) return 38;
    if (report.find("# dominant_freq_hz:") == std::string::npos) return 39;
    if (report.find("# reset_frames: 0") == std::string::npos) return 40;
    if (report.find("# scene_cut_frames: 0") == std::string::npos) return 41;
    if (report.find("# held_jitter_frames: 0") == std::string::npos) return 42;
    if (report.find("# luma_rule:") == std::string::npos) return 43;
    if (report.find("luma_mean, in_fmt, scene_w, scene_h, mv_null, depth_null, hist_realloc, sens, thr, mode") == std::string::npos) return 44;
    if (report.find("-1.000000, 0, 0, 0, 0, 0, 0, 0.00, 0.000, 0") == std::string::npos) return 45;
    if (report.find("# mv_null_frames:") == std::string::npos) return 46;
    if (report.find("# depth_null_frames:") == std::string::npos) return 47;
    if (report.find("# hist_realloc_frames:") == std::string::npos) return 48;
    if (report.find("# unique_in_fmt:") == std::string::npos) return 49;
    if (report.find("# luma_min: -1.000000") == std::string::npos) return 50;
    if (report.find("# luma_avg: -1.000000") == std::string::npos) return 51;
    if (report.find("# luma_max: -1.000000") == std::string::npos) return 52;
    if (report.find("# luma_dominant_period_frames: 0") == std::string::npos) return 53;
    if (report.find("# luma_dominant_freq_hz: 0.00") == std::string::npos) return 54;
    if (report.find("# pearson_green_vs_luma: 0.0000") == std::string::npos) return 55;
    if (report.find("# pearson_green_vs_abs_dluma: 0.0000") == std::string::npos) return 56;

    std::vector<double> px = { 1.0, 2.0, 3.0, 4.0, 5.0 };
    std::vector<double> py = { 2.0, 4.0, 6.0, 8.0, 10.0 };
    if (std::abs(edpe::pearson(px, py) - 1.0) > 1e-4) return 57;

    std::vector<double> pny = { 10.0, 8.0, 6.0, 4.0, 2.0 };
    if (std::abs(edpe::pearson(px, pny) - (-1.0)) > 1e-4) return 58;

    std::vector<double> pconst = { 3.0, 3.0, 3.0, 3.0, 3.0 };
    if (edpe::pearson(px, pconst) != 0.0) return 59;
    if (edpe::pearson(pconst, py) != 0.0) return 60;

    std::vector<double> pempty;
    std::vector<double> pone = { 1.0 };
    if (edpe::pearson(pempty, pempty) != 0.0) return 61;
    if (edpe::pearson(pone, pone) != 0.0) return 62;

    std::vector<edpe::JitterViewFrameData> framesLuma(100);
    for (size_t i = 0; i < framesLuma.size(); ++i) {
        framesLuma[i].meta.frame = 2000 + i;
        framesLuma[i].meta.renderedFrameId = 2000 + i;
        framesLuma[i].meta.timeMs = static_cast<float>(i * 16.666);
        framesLuma[i].meta.deltaMs = (i > 0) ? 16.666f : 0.0f;
        framesLuma[i].meta.lumaMean = 0.2 + 0.1 * std::sin(2.0 * 3.14159265358979323846 * static_cast<double>(i) / 20.0);
        framesLuma[i].meta.inputFormat = 26;
        framesLuma[i].meta.sceneW = 1920;
        framesLuma[i].meta.sceneH = 1080;
        framesLuma[i].meta.sensitivity = 1.5f;
        framesLuma[i].meta.threshold = 0.02f;
        framesLuma[i].meta.mode = 1;
        framesLuma[i].meta.motionNull = (i % 10 == 0);
        framesLuma[i].meta.depthNull = false;
        framesLuma[i].meta.historyRecreated = (i == 0);
        framesLuma[i].tiles[targetIdx] = static_cast<uint32_t>(tilePix * (0.01 + 0.05 * std::abs(framesLuma[i].meta.lumaMean)));
    }

    const std::string repLuma = edpe::generateJitterViewReport(header, framesLuma);
    if (repLuma.find("26 (R11G11B10_FLOAT)") == std::string::npos) return 63;
    if (repLuma.find("# hist_realloc_frames: 1") == std::string::npos) return 64;
    if (repLuma.find("# mv_null_frames: 10") == std::string::npos) return 65;
    if (repLuma.find("# luma_dominant_period_frames: 20") == std::string::npos) return 66;
    if (repLuma.find(", 26, 1920, 1080, 0, 0, 0, 1.50, 0.020, 1") == std::string::npos) return 67;

    return 0;
}
