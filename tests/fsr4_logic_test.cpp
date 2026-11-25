// CPU-only unit tests for src/fsr4_logic.h: the FSR 4 failure gate, the provider selection state (select, switch,
// retry, label text) and the mapping from EDPE's TemporalFrameInputs to FSR 4 context flags and dispatch values.
// No GPU, no FidelityFX SDK and no game needed.
#include "fsr4_logic.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

using namespace edpe;
using namespace edpe::fsr4;

namespace {

int g_failures = 0;
// A macro so a failure reports the text of the failing condition and its line.
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                        \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

bool approx(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) <= eps; }

void testFailureGate() {
    FailureGate g;
    for (int i = 1; i < kMaxConsecutiveFailures; ++i) CHECK(!g.onFailure());  // failures 1..119 keep FSR 4
    CHECK(g.onFailure());                                                      // the 120th disables it
    CHECK(kMaxConsecutiveFailures == 120);

    // A success in the middle restarts the count.
    FailureGate h;
    for (int i = 0; i < 100; ++i) h.onFailure();
    h.reset();
    CHECK(h.consecutive == 0);
    for (int i = 1; i < kMaxConsecutiveFailures; ++i) CHECK(!h.onFailure());
    CHECK(h.onFailure());

    h.reset();
    CHECK(h.consecutive == 0);
}

void testProviderState() {
    ProviderState s;
    CHECK(!s.running());
    CHECK(!s.needsInit());
    CHECK(s.label() == ProviderLabel::Fsr31);

    // Selecting FSR 4: needs a (lazy) init; a failed init falls back and is not retried by itself.
    CHECK(s.setSelected(true));
    CHECK(!s.setSelected(true));  // idempotent
    CHECK(s.needsInit());
    CHECK(s.label() == ProviderLabel::Fsr31Fsr4Unavailable);
    s.onInitResult(false);
    CHECK(!s.needsInit());
    CHECK(!s.running());
    CHECK(s.label() == ProviderLabel::Fsr31Fsr4Unavailable);

    // Explicit retry re-arms the init; a successful init runs FSR 4.
    CHECK(s.retry());
    CHECK(s.needsInit());
    s.onInitResult(true);
    CHECK(s.running());
    CHECK(s.label() == ProviderLabel::Fsr4);
    CHECK(!s.retry());  // nothing to retry while running

    // Back to FSR 3.1.2 and again to FSR 4: keeps the engine initialized (no new init needed).
    CHECK(s.setSelected(false));
    CHECK(!s.running());
    CHECK(s.label() == ProviderLabel::Fsr31);
    CHECK(!s.retry());  // not selected
    CHECK(s.setSelected(true));
    CHECK(s.running());          // still active from before
    CHECK(!s.needsInit());

    // Releasing the engine while FSR 4 is not selected keeps the capability; selecting FSR 4 again needs a new init.
    CHECK(s.setSelected(false));
    s.onEngineReleased();
    CHECK(!s.isEngineReady() && s.isCapable());
    CHECK(s.label() == ProviderLabel::Fsr31);
    CHECK(s.setSelected(true));
    CHECK(s.needsInit());
    CHECK(!s.running());
    s.onInitResult(true);
    CHECK(s.running());

    // Backend re-initialization forgets the previous engine.
    s.onBackendInitialize();
    CHECK(!s.isEngineReady() && !s.initTried());
    CHECK(s.isSelected());             // the selection itself survives
    CHECK(s.needsInit());

    s.onShutdown();
    CHECK(!s.isEngineReady() && !s.initTried());
}

void testProviderFailurePath() {
    ProviderState s;
    s.setSelected(true);
    s.onInitResult(true);
    CHECK(s.running());

    // The first failure asks for one log line; later failures do not.
    auto first = s.onEvaluateFailure();
    CHECK(first.logFirstFailure && !first.disabled);
    CHECK(first.startedStreak);  // FSR 3.1.2 takes over here: the caller resets its history once
    for (int i = 2; i < kMaxConsecutiveFailures; ++i) {
        auto o = s.onEvaluateFailure();
        CHECK(!o.logFirstFailure && !o.disabled && !o.startedStreak);
        CHECK(s.running());
    }
    auto last = s.onEvaluateFailure();  // 120th consecutive failure
    CHECK(last.disabled);
    CHECK(!s.running());
    CHECK(!s.needsInit());  // not retried on its own
    CHECK(s.label() == ProviderLabel::Fsr31Fsr4Unavailable);

    // Selecting FSR 3.1.2 and then FSR 4 again, or an explicit retry, starts over.
    CHECK(s.retry());
    CHECK(s.needsInit());
    s.onInitResult(true);
    CHECK(s.running());
    auto again = s.onEvaluateFailure();
    CHECK(again.logFirstFailure);  // the log-once flag was reset by the retry

    // A success clears the failure streak, so scattered failures never disable FSR 4.
    ProviderState t;
    t.setSelected(true);
    t.onInitResult(true);
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 100; ++i) {
            const auto o = t.onEvaluateFailure();
            CHECK(!o.disabled);
            CHECK(o.startedStreak == (i == 0));  // each streak after a success starts again
        }
        t.onEvaluateSuccess();
    }
    CHECK(t.running());
}

void testCapabilityState() {
    ProviderState s;
    CHECK(!s.isCapable());
    s.onInitResult(false);  // startup probe on a GPU or runtime without FSR 4
    CHECK(!s.isCapable() && !s.isEngineReady());
    s.onInitResult(true);   // a later successful initialization
    CHECK(s.isCapable() && s.isEngineReady());

    // Disabling FSR 4 after repeated failures keeps the capability: the menu entry stays available and a retry works.
    s.setSelected(true);
    for (int i = 0; i < kMaxConsecutiveFailures; ++i) s.onEvaluateFailure();
    CHECK(!s.isEngineReady() && s.isCapable());
    CHECK(s.retry());

    // A backend re-initialization or shutdown forgets the probe (the device may have changed).
    s.onBackendInitialize();
    CHECK(!s.isCapable());
    s.onInitResult(true);
    s.onShutdown();
    CHECK(!s.isCapable() && !s.isEngineReady());
}

std::vector<std::string> names(std::initializer_list<const char*> l) {
    std::vector<std::string> v;
    for (const char* s : l) v.emplace_back(s ? s : "");
    return v;
}

void testVersionParse() {
    Version v;
    CHECK(parseVersion("4.1.2", &v) && v.major == 4 && v.minor == 1 && v.patch == 2);
    CHECK(parseVersion("3.1.6", &v) && v.major == 3 && v.minor == 1 && v.patch == 6);
    CHECK(parseVersion("FSR 4.1", &v) && v.major == 4 && v.minor == 1 && v.patch == 0);
    CHECK(parseVersion("AMD FSR v4.1.1 (ML)", &v) && v.major == 4 && v.minor == 1 && v.patch == 1);
    CHECK(parseVersion("2.3.4", &v) && v.major == 2 && v.minor == 3 && v.patch == 4);
    CHECK(parseVersion("v12.34.567-beta", &v) && v.major == 12 && v.minor == 34 && v.patch == 567);

    // Not versions: false, and `out` is untouched.
    Version keep{9, 9, 9};
    CHECK(!parseVersion(nullptr, &keep));
    CHECK(!parseVersion("", &keep));
    CHECK(!parseVersion("unknown", &keep));
    CHECK(!parseVersion("4", &keep));
    CHECK(!parseVersion("1..2", &keep));
    CHECK(!parseVersion("1234567.1.1", &keep));  // a number that long is not a version
    CHECK(keep.major == 9 && keep.minor == 9 && keep.patch == 9);

    CHECK(compareVersions({4, 1, 2}, {4, 1, 2}) == 0);
    CHECK(compareVersions({4, 1, 2}, {4, 1, 10}) < 0);
    CHECK(compareVersions({4, 2, 0}, {4, 1, 99}) > 0);
    CHECK(compareVersions({3, 9, 9}, {4, 0, 0}) < 0);
}

void testProviderPick() {
    Version picked;
    // The usual offer on a Radeon RX 7000 or 9000: FSR 4 first.
    CHECK(pickFsr4Provider(names({"4.1.2", "3.1.6", "2.3.4"}), &picked) == 0);
    CHECK(picked.major == 4 && picked.minor == 1 && picked.patch == 2);
    // Order does not matter: the highest 4.x wins.
    CHECK(pickFsr4Provider(names({"3.1.6", "4.0.3", "4.1.2", "2.3.4"}), &picked) == 2);
    CHECK(picked.minor == 1 && picked.patch == 2);
    // Equal versions: the first wins.
    CHECK(pickFsr4Provider(names({"4.1.2", "4.1.2"})) == 0);
    // Garbage entries are ignored.
    CHECK(pickFsr4Provider(names({"", "unknown", "4.1.0"}), &picked) == 2);

    // No 4.x provider (RDNA 2, another vendor's GPU, an old runtime): FSR 4 must not run a 3.x one.
    Version untouched{7, 7, 7};
    CHECK(pickFsr4Provider(names({"3.1.5", "2.3.4"}), &untouched) == -1);
    CHECK(untouched.major == 7);
    CHECK(pickFsr4Provider(names({"3.1.6"})) == -1);
    CHECK(pickFsr4Provider(names({})) == -1);
    CHECK(pickFsr4Provider(names({"", "unknown"})) == -1);
}

void testProviderRefusalText() {
    CHECK(formatProviderList(names({"4.1.2", "3.1.6", "2.3.4"})) == "4.1.2, 3.1.6, 2.3.4");
    CHECK(formatProviderList(names({"FSR 3.1.5", "", "weird-name"})) == "3.1.5, ?, weird-name");
    CHECK(formatProviderList(names({"0123456789012345678901234567890"})) == "012345678901234567890123");  // 24 chars

    const std::string two = providerRefusalText(names({"3.1.5", "2.3.4"}));
    CHECK(two.find("only FSR 3.1.5, 2.3.4") != std::string::npos);
    CHECK(two.find("RX 7000 or 9000") != std::string::npos);
    const std::string none = providerRefusalText(names({}));
    CHECK(none.find("offers no FSR version") != std::string::npos);
    CHECK(none.find("RX 7000 or 9000") != std::string::npos);

    // The Agility hint appears only when the side-loaded runtime is not in use.
    CHECK(two.find("D3D12Core.dll") == std::string::npos);
    const std::string noAgility = providerRefusalText(names({"3.1.5"}), false);
    CHECK(noAgility.find("D3D12\\D3D12Core.dll") != std::string::npos);
    CHECK(providerRefusalText(names({"3.1.5"}), true).find("D3D12Core.dll") == std::string::npos);
}

void testHdrFallback() {
    ProviderState s;
    s.setSelected(true);
    s.onInitResult(true);
    CHECK(s.running() && s.label() == ProviderLabel::Fsr4);

    // HDR input starts: logged once, shown as FSR 3.1 with the reason, and not counted as failures.
    CHECK(s.setHdrFallback(true) == ProviderState::HdrTransition::Started);
    CHECK(s.setHdrFallback(true) == ProviderState::HdrTransition::None);
    CHECK(s.label() == ProviderLabel::Fsr31HdrInput);
    CHECK(s.running());
    for (int i = 0; i < 2 * kMaxConsecutiveFailures; ++i) s.setHdrFallback(true);
    CHECK(s.running() && s.consecutiveFailures() == 0);

    // LDR input again: FSR 4 resumes, and a later HDR episode logs again.
    CHECK(s.setHdrFallback(false) == ProviderState::HdrTransition::Ended);
    CHECK(s.setHdrFallback(false) == ProviderState::HdrTransition::None);
    CHECK(s.label() == ProviderLabel::Fsr4);
    CHECK(s.setHdrFallback(true) == ProviderState::HdrTransition::Started);

    // Not running (FSR 3.1.2 selected or FSR 4 unavailable): the label ignores the HDR flag.
    s.setSelected(false);
    CHECK(s.label() == ProviderLabel::Fsr31);
    ProviderState u;
    u.setSelected(true);
    u.onInitResult(false);
    u.setHdrFallback(true);
    CHECK(u.label() == ProviderLabel::Fsr31Fsr4Unavailable);
}

void testProviderLabelText() {
    CHECK(formatProviderLabel(ProviderLabel::Fsr4, "4.1.2") == "FSR 4.1.2");
    CHECK(formatProviderLabel(ProviderLabel::Fsr4, nullptr) == "FSR 4");
    CHECK(formatProviderLabel(ProviderLabel::Fsr4, "") == "FSR 4");
    CHECK(formatProviderLabel(ProviderLabel::Fsr31, "4.1.2") == "FSR 3.1.2");
    CHECK(formatProviderLabel(ProviderLabel::Fsr31Fsr4Unavailable, "4.1.2") == "FSR 3.1.2 (FSR 4 unavailable)");
    CHECK(formatProviderLabel(ProviderLabel::Fsr31HdrInput, "4.1.2") == "FSR 3.1.2 (FSR 4 needs HdrInput=0)");
}

void testContextFlags() {
    TemporalFrameInputs in{};
    in.colorIsHdr = false;
    in.depthInverted = true;
    in.depthInfinite = true;
    ContextFlagsPlan p = buildContextFlagsPlan(in);
    CHECK(p.autoExposure);
    CHECK(p.depthInverted && p.depthInfinite);
    CHECK(!p.hdr && p.nonLinearColorspace);  // tonemapped 8-bit input is declared non-linear

    in.colorIsHdr = true;
    p = buildContextFlagsPlan(in);
    CHECK(p.hdr && !p.nonLinearColorspace);  // linear pre-tonemap input uses the HDR flag
    CHECK(p.autoExposure);

    in.depthInverted = false;
    in.depthInfinite = false;
    p = buildContextFlagsPlan(in);
    CHECK(!p.depthInverted && !p.depthInfinite);
}

void testDispatchPlan() {
    TemporalFrameInputs in{};
    in.jitterX = 0.25f;
    in.jitterY = -0.375f;
    in.motionScaleX = 1.0f;
    in.motionScaleY = 1.0f;
    in.depthInverted = true;
    in.cameraNow.depthB = 0.025f;
    in.cameraNow.scaleY = 1.7320508f;  // cot(fov/2) = sqrt(3): a vertical field of view of 60 degrees
    in.frameTimeMs = 11.1f;
    in.colorIsHdr = false;

    DispatchPlan p = buildDispatchPlan(in, false);
    // Jitter is passed through unchanged: EDPE's projection jitter already uses the SDK recipe (2*jx/w, -2*jy/h).
    CHECK(approx(p.jitterX, 0.25f) && approx(p.jitterY, -0.375f));
    CHECK(approx(p.motionScaleX, 1.0f) && approx(p.motionScaleY, 1.0f));
    CHECK(approx(p.frameTimeMs, 11.1f));
    CHECK(approx(p.preExposure, 1.0f));
    CHECK(p.nonLinearSrgb);                                   // LDR input
    CHECK(!p.reset);
    CHECK(p.cameraNear == FLT_MAX && approx(p.cameraFar, 0.025f));  // reversed infinite depth
    CHECK(approx(p.fovVertical, 2.0f * std::atan(1.0f / 1.7320508f)));
    CHECK(approx(p.fovVertical, 1.0471976f, 1e-4f));            // 60 degrees in radians

    // Reset comes from either the frame or a pending reset (new context or recreated shared textures).
    in.reset = false;
    CHECK(buildDispatchPlan(in, true).reset);
    in.reset = true;
    CHECK(buildDispatchPlan(in, false).reset);

    // Missing or zero values take the documented defaults.
    TemporalFrameInputs d{};
    d.depthInverted = true;
    DispatchPlan dp = buildDispatchPlan(d, false);
    CHECK(approx(dp.frameTimeMs, 16.6f));        // no frame time: 16.6 ms
    CHECK(approx(dp.cameraFar, 0.025f));         // no depthB: the measured Elite value
    CHECK(approx(dp.fovVertical, 1.0f));         // no projection scale: 1 rad
    CHECK(!dp.enableSharpening && approx(dp.sharpness, 0.0f));

    // Non-inverted depth swaps near and far.
    d.depthInverted = false;
    d.cameraNow.depthB = 0.1f;
    dp = buildDispatchPlan(d, false);
    CHECK(approx(dp.cameraNear, 0.1f) && dp.cameraFar == FLT_MAX);

    // Sharpening: enabled above zero, clamped to 1.
    d.sharpness = 0.5f;
    dp = buildDispatchPlan(d, false);
    CHECK(dp.enableSharpening && approx(dp.sharpness, 0.5f));
    d.sharpness = 1.5f;
    CHECK(approx(buildDispatchPlan(d, false).sharpness, 1.0f));
    d.sharpness = -0.2f;
    dp = buildDispatchPlan(d, false);
    CHECK(!dp.enableSharpening && approx(dp.sharpness, 0.0f));

    // HDR input drops the non-linear sRGB dispatch flag.
    d.colorIsHdr = true;
    CHECK(!buildDispatchPlan(d, false).nonLinearSrgb);
}

} // namespace

int main() {
    testFailureGate();
    testProviderState();
    testProviderFailurePath();
    testProviderLabelText();
    testHdrFallback();
    testCapabilityState();
    testVersionParse();
    testProviderPick();
    testProviderRefusalText();
    testContextFlags();
    testDispatchPlan();
    if (g_failures) {
        std::printf("fsr4_logic_test: %d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("fsr4_logic_test PASSED\n");
    return 0;
}
