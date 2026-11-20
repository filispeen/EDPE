#include "fg_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>

namespace edpe {

namespace {

bool isFiniteMatrix(const float* m, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (!std::isfinite(m[i])) return false;
    }
    return true;
}

void mulMat4RowMajor(const float (&a)[16], const float (&b)[16], float (&out)[16]) {
    float tmp[16]{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k) {
                sum += static_cast<double>(a[r * 4 + k]) * static_cast<double>(b[k * 4 + c]);
            }
            tmp[r * 4 + c] = static_cast<float>(sum);
        }
    }
    std::copy_n(tmp, 16, out);
}

// Builds worldFromView (4x4 row-vector: p_world = p_view * W) and viewFromWorld (orthonormal inverse).
void buildCameraRigidPair(const CameraProjection& cam, float (&worldFromView)[16], float (&viewFromWorld)[16]) {
    const float* w = cam.worldFromView;
    // worldFromView stores column vectors in rows 0..2, col 3 = translation.
    // For row-vector convention p_world = [x y z 1] * M:
    worldFromView[0] = w[0];  worldFromView[1] = w[4];  worldFromView[2] = w[8];   worldFromView[3] = 0.0f;
    worldFromView[4] = w[1];  worldFromView[5] = w[5];  worldFromView[6] = w[9];   worldFromView[7] = 0.0f;
    worldFromView[8] = w[2];  worldFromView[9] = w[6];  worldFromView[10] = w[10]; worldFromView[11] = 0.0f;
    worldFromView[12] = w[3]; worldFromView[13] = w[7]; worldFromView[14] = w[11]; worldFromView[15] = 1.0f;

    // Orthonormal rotation transpose = R^T; translation = -C * R^T.
    const float tx = -(w[3] * w[0] + w[7] * w[4] + w[11] * w[8]);
    const float ty = -(w[3] * w[1] + w[7] * w[5] + w[11] * w[9]);
    const float tz = -(w[3] * w[2] + w[7] * w[6] + w[11] * w[10]);

    viewFromWorld[0] = w[0];  viewFromWorld[1] = w[1];  viewFromWorld[2] = w[2];   viewFromWorld[3] = 0.0f;
    viewFromWorld[4] = w[4];  viewFromWorld[5] = w[5];  viewFromWorld[6] = w[6];   viewFromWorld[7] = 0.0f;
    viewFromWorld[8] = w[8];  viewFromWorld[9] = w[9];  viewFromWorld[10] = w[10]; viewFromWorld[11] = 0.0f;
    viewFromWorld[12] = tx;   viewFromWorld[13] = ty;   viewFromWorld[14] = tz;    viewFromWorld[15] = 1.0f;
}

void buildProjAndInvProj(const CameraProjection& cam, float (&viewToClip)[16], float (&clipToView)[16]) {
    // Row-vector infinite reversed-Z +Z forward:
    // [x y z 1] * P = [x*sx, y*sy, depthB, z] -> after /w (= z): (x*sx/z, y*sy/z, depthB/z)
    std::fill_n(viewToClip, 16, 0.0f);
    viewToClip[0] = cam.scaleX;
    viewToClip[5] = cam.scaleY;
    viewToClip[11] = 1.0f;       // w_clip = z_view
    viewToClip[14] = cam.depthB; // z_clip = depthB

    // Inverse: [nx, ny, nz, 1] * invP = [nx/sx, ny/sy, 1, nz/depthB] -> after /w (= nz/depthB):
    // view = [nx*depthB/(sx*nz), ny*depthB/(sy*nz), depthB/nz]
    std::fill_n(clipToView, 16, 0.0f);
    clipToView[0] = 1.0f / cam.scaleX;
    clipToView[5] = 1.0f / cam.scaleY;
    clipToView[11] = 1.0f / cam.depthB;
    clipToView[14] = 1.0f;
}

class StubUnavailableFgBackend : public IFrameGenerationBackend {
public:
    StubUnavailableFgBackend(FrameGenerationBackend type, const wchar_t* label, const wchar_t* reason)
        : type_(type), label_(label), reason_(reason) {}

    FrameGenerationBackend backendType() const override { return type_; }
    const wchar_t* name() const override { return label_; }
    bool isAvailable() const override { return false; }
    const wchar_t* unavailableReason() const override { return reason_; }
    uint32_t maxFramesToGenerate() const override { return 0; }
    bool isDynamicMfgSupported() const override { return false; }
    bool initialize(D3D12ProxyPresenter* /*presenter*/) override { return false; }
    void shutdown() override {}
    bool evaluate(ID3D12GraphicsCommandList* /*cmdList*/,
                  const FrameGenerationInputs& /*inputs*/,
                  bool /*enabled*/,
                  uint32_t /*requestedExtraFrames*/,
                  uint32_t* outGeneratedFrames) override {
        if (outGeneratedFrames) *outGeneratedFrames = 0;
        return false;
    }
    void resetHistory() override {}

private:
    FrameGenerationBackend type_;
    const wchar_t* label_;
    const wchar_t* reason_;
};

} // namespace

bool FrameGenerationInputs::isValid(const wchar_t** outReason) const {
    if (!renderWidth || !renderHeight || !outputWidth || !outputHeight) {
        if (outReason) *outReason = L"FG dimensions must be non-zero";
        return false;
    }
    if (renderWidth > outputWidth || renderHeight > outputHeight) {
        if (outReason) *outReason = L"FG render dimensions exceed output dimensions";
        return false;
    }
    if (!hudlessColor) {
        if (outReason) *outReason = L"FG hudlessColor resource missing";
        return false;
    }
    if (!depth) {
        if (outReason) *outReason = L"FG depth resource missing";
        return false;
    }
    if (!motionVectors) {
        if (outReason) *outReason = L"FG motionVectors resource missing";
        return false;
    }
    if (!std::isfinite(jitterX) || !std::isfinite(jitterY)) {
        if (outReason) *outReason = L"FG jitter must be finite";
        return false;
    }
    if (!std::isfinite(frameTimeMs) || frameTimeMs < 0.0f) {
        if (outReason) *outReason = L"FG frameTimeMs must be non-negative and finite";
        return false;
    }
    if (frameIndex == 0) {
        if (outReason) *outReason = L"FG frameIndex must be >= 1";
        return false;
    }
    if (!(cameraNow.depthB > 0.0f && cameraNow.scaleX > 0.0f && cameraNow.scaleY > 0.0f) ||
        !(cameraPrevious.depthB > 0.0f && cameraPrevious.scaleX > 0.0f && cameraPrevious.scaleY > 0.0f) ||
        !isFiniteMatrix(cameraNow.worldFromView, 12) ||
        !isFiniteMatrix(cameraPrevious.worldFromView, 12)) {
        if (outReason) *outReason = L"FG camera state invalid";
        return false;
    }
    return true;
}

bool computeFgCameraMatrices(const CameraProjection& now,
                             const CameraProjection& previous,
                             FgCameraMatrices* outMatrices) {
    if (!outMatrices) return false;
    if (!(now.depthB > 0.0f && now.scaleX > 0.0f && now.scaleY > 0.0f) ||
        !(previous.depthB > 0.0f && previous.scaleX > 0.0f && previous.scaleY > 0.0f) ||
        !isFiniteMatrix(now.worldFromView, 12) ||
        !isFiniteMatrix(previous.worldFromView, 12)) {
        return false;
    }

    float nowProj[16]{}, nowInvProj[16]{};
    float prevProj[16]{}, prevInvProj[16]{};
    buildProjAndInvProj(now, nowProj, nowInvProj);
    buildProjAndInvProj(previous, prevProj, prevInvProj);

    float nowWorldFromView[16]{}, nowViewFromWorld[16]{};
    float prevWorldFromView[16]{}, prevViewFromWorld[16]{};
    buildCameraRigidPair(now, nowWorldFromView, nowViewFromWorld);
    buildCameraRigidPair(previous, prevWorldFromView, prevViewFromWorld);

    // clipNow -> viewNow -> world -> viewPrev -> clipPrev
    float t1[16]{}, t2[16]{};
    mulMat4RowMajor(nowInvProj, nowWorldFromView, t1);
    mulMat4RowMajor(t1, prevViewFromWorld, t2);
    mulMat4RowMajor(t2, prevProj, outMatrices->clipToPrevClip);

    // clipPrev -> viewPrev -> world -> viewNow -> clipNow
    mulMat4RowMajor(prevInvProj, prevWorldFromView, t1);
    mulMat4RowMajor(t1, nowViewFromWorld, t2);
    mulMat4RowMajor(t2, nowProj, outMatrices->prevClipToClip);

    std::copy_n(nowProj, 16, outMatrices->viewToClip);
    std::copy_n(nowInvProj, 16, outMatrices->clipToView);

    outMatrices->position = cameraPosition(now);
    outMatrices->right = cameraRight(now);
    outMatrices->up = cameraUp(now);
    outMatrices->forward = cameraForward(now);
    outMatrices->verticalFovRad = 2.0f * std::atan(1.0f / now.scaleY);
    outMatrices->aspectRatio = now.scaleY / now.scaleX;
    outMatrices->nearPlane = now.depthB;
    return true;
}

FrameGenerationManager::FrameGenerationManager() {
    backends_[static_cast<size_t>(FrameGenerationBackend::Off)] =
        std::make_unique<NullFrameGenBackend>();
    backends_[static_cast<size_t>(FrameGenerationBackend::AmdFsrFg)] =
        std::make_unique<StubUnavailableFgBackend>(
            FrameGenerationBackend::AmdFsrFg,
            L"AMD FSR Frame Generation",
            L"FidelityFX FG SDK (amd_fidelityfx_framegeneration.dll) not staged yet (Phase 14)");
    backends_[static_cast<size_t>(FrameGenerationBackend::NvidiaDlssG)] =
        std::make_unique<StubUnavailableFgBackend>(
            FrameGenerationBackend::NvidiaDlssG,
            L"NVIDIA DLSS Frame Generation",
            L"NVIDIA Streamline SDK (sl.interposer.dll / sl.dlss_g.dll) not staged yet (Phase 15)");
}

void FrameGenerationManager::registerBackend(std::unique_ptr<IFrameGenerationBackend> backend) {
    if (!backend) return;
    const size_t idx = static_cast<size_t>(backend->backendType());
    if (idx < std::size(backends_)) {
        if (selected_ == backend->backendType() && backends_[idx]) {
            backends_[idx]->shutdown();
        }
        backends_[idx] = std::move(backend);
    }
}

bool FrameGenerationManager::selectBackend(FrameGenerationBackend backend, D3D12ProxyPresenter* presenter) {
    const size_t nextIdx = static_cast<size_t>(backend);
    if (nextIdx >= std::size(backends_) || !backends_[nextIdx]) {
        selected_ = FrameGenerationBackend::Off;
        return false;
    }
    if (selected_ != backend) {
        const size_t curIdx = static_cast<size_t>(selected_);
        if (curIdx < std::size(backends_) && backends_[curIdx]) {
            backends_[curIdx]->shutdown();
        }
    }
    if (backend == FrameGenerationBackend::Off) {
        selected_ = FrameGenerationBackend::Off;
        hasLastFrameIndex_ = false;
        return true;
    }
    if (D3D12ProxyPresenter::isCrossApiPresentBlockedByRenderDoc()) {
        std::swprintf(diagnostics_.statusMessage, 256,
                      L"Blocked under Windows RenderDoc (cross-API shared handles unsupported)");
        selected_ = FrameGenerationBackend::Off;
        return false;
    }
    if (!backends_[nextIdx]->isAvailable() || !presenter || !backends_[nextIdx]->initialize(presenter)) {
        std::swprintf(diagnostics_.statusMessage, 256, L"%ls",
                      backends_[nextIdx]->unavailableReason());
        selected_ = FrameGenerationBackend::Off;
        return false;
    }
    selected_ = backend;
    hasLastFrameIndex_ = false;
    return true;
}

void FrameGenerationManager::shutdown() {
    for (auto& b : backends_) {
        if (b) b->shutdown();
    }
    selected_ = FrameGenerationBackend::Off;
    hasLastFrameIndex_ = false;
}

bool FrameGenerationManager::evaluate(ID3D12GraphicsCommandList* cmdList,
                                      const FrameGenerationInputs& inputs,
                                      bool userEnabled) {
    diagnostics_.didRun = false;
    diagnostics_.backend = selected_;
    diagnostics_.enabled = false;
    diagnostics_.generatedFramesThisCycle = 0;
    diagnostics_.success = false;
    diagnostics_.renderWidth = inputs.renderWidth;
    diagnostics_.renderHeight = inputs.renderHeight;
    diagnostics_.outputWidth = inputs.outputWidth;
    diagnostics_.outputHeight = inputs.outputHeight;
    diagnostics_.frameIndex = inputs.frameIndex;
    diagnostics_.renderedFps = (inputs.frameTimeMs > 0.0f) ? (1000.0f / inputs.frameTimeMs) : 0.0f;
    diagnostics_.presentedFps = diagnostics_.renderedFps;

    if (!userEnabled || selected_ == FrameGenerationBackend::Off) {
        std::swprintf(diagnostics_.statusMessage, 256, L"Off");
        diagnostics_.success = true;
        return true;
    }

    if (D3D12ProxyPresenter::isCrossApiPresentBlockedByRenderDoc()) {
        std::swprintf(diagnostics_.statusMessage, 256,
                      L"Bypassed under Windows RenderDoc");
        return false;
    }

    if (inputs.isPauseOrMenu) {
        auto* backend = backends_[static_cast<size_t>(selected_)].get();
        if (backend && backend->isAvailable() && cmdList) {
            uint32_t dummyGen = 0;
            backend->evaluate(cmdList, inputs, false, 0, &dummyGen);
        }
        hasLastFrameIndex_ = false;
        std::swprintf(diagnostics_.statusMessage, 256, L"Stand down (menu/loading)");
        diagnostics_.success = true;
        return true;
    }

    const wchar_t* reason = nullptr;
    if (!inputs.isValid(&reason)) {
        std::swprintf(diagnostics_.statusMessage, 256, L"Bypassed: %ls",
                      reason ? reason : L"invalid inputs");
        hasLastFrameIndex_ = false;
        return false;
    }

    // FidelityFX & Streamline require monotonic +1 frame IDs; gaps force history reset.
    FrameGenerationInputs sanitized = inputs;
    if (!hasLastFrameIndex_ || inputs.frameIndex != lastFrameIndex_ + 1) {
        sanitized.reset = true;
    }
    lastFrameIndex_ = inputs.frameIndex;
    hasLastFrameIndex_ = true;
    diagnostics_.reset = sanitized.reset;

    auto* backend = backends_[static_cast<size_t>(selected_)].get();
    if (!backend || !backend->isAvailable()) {
        std::swprintf(diagnostics_.statusMessage, 256, L"Unavailable");
        return false;
    }

    uint32_t generated = 0;
    const bool ok = backend->evaluate(cmdList, sanitized, true, requestedExtraFrames_, &generated);
    diagnostics_.didRun = true;
    diagnostics_.success = ok;
    if (!ok) {
        generated = 0;
        std::swprintf(diagnostics_.statusMessage, 256, L"FG backend evaluate failed (fail-open)");
        return false;
    }

    diagnostics_.enabled = true;
    diagnostics_.generatedFramesThisCycle = generated;
    totalGeneratedFrames_ += generated;
    diagnostics_.totalGeneratedFrames = totalGeneratedFrames_;
    diagnostics_.presentedFps = diagnostics_.renderedFps * static_cast<float>(1u + generated);
    std::swprintf(diagnostics_.statusMessage, 256, L"%ls (%ux)",
                  backend->name(), 1u + generated);
    return true;
}

const IFrameGenerationBackend* FrameGenerationManager::activeBackend() const {
    return getBackend(selected_);
}

const IFrameGenerationBackend* FrameGenerationManager::getBackend(FrameGenerationBackend backend) const {
    const size_t idx = static_cast<size_t>(backend);
    return (idx < std::size(backends_)) ? backends_[idx].get() : nullptr;
}

} // namespace edpe
