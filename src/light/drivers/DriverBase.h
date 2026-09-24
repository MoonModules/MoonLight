#pragma once

#include "core/module/MoonModule.h"
#include "core/util/ScratchBuffer.h"
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/drivers/Correction.h"
#include "light/drivers/LedPeripheral.h"         // LedHwBlock: the peripheral-block claim guard's vocabulary
#include "light/drivers/LightPresetsModule.h"   // the shared preset library a driver references by id
#include "platform/platform.h"

#include <cstdio>       // std::snprintf for status strings
#include <cstring>     // std::strcmp (onControlChanged) / memset (buffer clears)
#include <cstdint>    // fixed-width ints
#include <algorithm> // std::min / max / clamp (chunk loops, size clamps)

namespace mm {

/// Base class for one driver: a consumer that reads the shared source buffer and emits it. The destination is a physical LED output, a network sink, or the preview.
///
/// A driver reads dimensions from an active Layer, applies the shared output correction, and may restrict its output to a window of the source buffer. The zero-state role EffectBase plays for effects.
///
/// @moreinfo
///
/// ## One include writes a driver
///
/// This file brings `DriverBase` plus the buffer, correction and platform pieces every driver needs.
/// A peripheral seam or a packet header stays per-driver.
class DriverBase : public MoonModule {
public:
    // The OWNER must release before destroying: a base destructor cannot prevent the vptr race.
    /// This module's role, which is what the container filters its children by.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Driver; }
    virtual void setSourceBuffer(Buffer* buf) = 0;

    // Virtual rather than RTTI: ESP32 builds compile without it, so the guard never casts.
    /// The hardware block this driver claims, so two drivers cannot corrupt one peripheral.
    virtual LedHwBlock hwBlock() const { return LedHwBlock::None; }

    // A driver overrides defineDriverControls, not this, so the placement is never re-implemented.
    /// Lead every driver card with the correction block, then the driver's own controls.
    void defineControls() final {
        if (hasCorrectionControls()) defineCorrectionControls();
        defineDriverControls();
    }

    /// A driver's own controls, added after the correction block; this is what a driver overrides.
    virtual void defineDriverControls() {}

    /// Whether this driver exposes the correction controls, which a raw-RGB sink opts out of.
    virtual bool hasCorrectionControls() const { return true; }
    // The ACTIVE layer for dimension queries, not a wiring constraint on how many layers feed it.
    /// Set the Layer this driver reads its dimensions from.
    void setLayer(Layer* layer) { layer_ = layer; }

    /// The first light of the configured window.
    uint16_t windowStart() const { return start_; }
    /// Number of lights in the configured window (0 = to end of buffer).
    uint16_t windowCount() const { return count_; }
    /// Set the window directly, which takes effect on the next parse as a control edit does.
    void setWindow(uint16_t start, uint16_t count) { start_ = start; count_ = count; }
    /// Test-only: resolve the window against a buffer of `bufN` lights into its length.
    nrOfLightsType resolveWindowLenForTest(nrOfLightsType bufN) const {
        nrOfLightsType outStart = 0, outLen = 0;
        windowSlice(bufN, outStart, outLen);
        return outLen;
    }
    /// The active Layer, which is null when none is wired, so a driver must tolerate that.
    Layer* layer() const { return layer_; }

    // The multiply happens once here, so the hot path stays one LUT lookup per channel.
    /// Rebuild this driver's correction, baking global times local brightness into one LUT.
    void rebuildCorrection(uint8_t globalBrightness) {
        lastGlobalBrightness_ = globalBrightness;   // remembered for self-triggered rebuilds
        // Applied unconditionally, so brightness works even before the preset library is up.
        const uint8_t effective =
            static_cast<uint8_t>((globalBrightness * localBrightness_) / 255);
        correction_.whiteMode = static_cast<WhiteMode>(whiteMode_);
        correction_.curve = static_cast<Correction::Curve>(curveSel_);
        correction_.balRed = balRed_;
        correction_.balGreen = balGreen_;
        correction_.balBlue = balBlue_;
        correction_.whiteLevel = whiteLevel_;
        correction_.budgetMa = budgetMa_;
        correction_.mAColor = mAColor_;
        correction_.mAWhite = mAWhite_;
        correction_.mAYellow = mAYellow_;
        correction_.mAUV = mAUV_;
        // A missing id falls back to the default, so a driver degrades rather than crashing.
        if (auto* lib = LightPresetsModule::active()) {
            if (presetId_ == 0) presetId_ = lib->defaultId();
            if (!lib->deriveCorrection(presetId_, effective, correction_)) {
                presetId_ = lib->defaultId();                       // dangling → re-point to default
                lib->deriveCorrection(presetId_, effective, correction_);
            }
        } else {
            // No library yet, so apply brightness here, as deriveCorrection would have.
            correction_.rebuildBrightness(effective);
        }
        onCorrectionChanged();      // let a driver resize its correction-applied buffer
    }

    /// Rebuild the correction when one of its own correction controls changed.
    void onControlChanged(const char* name) override {
        // The chosen INDEX maps to a stable id, so the reference survives a later reorder.
        if (std::strcmp(name, "lightPreset") == 0) {
            if (auto* lib = LightPresetsModule::active()) {
                presetId_ = lib->idAt(presetSel_);
                std::snprintf(presetRef_, sizeof(presetRef_), "%s", lib->nameAt(presetSel_));  // persist the name
            }
        }
        if (isCorrectionControl(name)) rebuildCorrection(lastGlobalBrightness_);
        MoonModule::onControlChanged(name);
    }

    /// Notified when the output channel count may have changed without a structural rebuild.
    virtual void onCorrectionChanged() {}

    /// Clear every shared status string, so a stopped driver leaves nothing behind.
    void release() override {
        freeWire();          // the shared correction scratch: owned here, so no driver re-frees it
        clearFailBuf();
        clearConfigErr();
        setConfigWarn(nullptr);
        MoonModule::release();
    }

    /// Test-only: the driver's own Correction, mutable so a test can set a wiring directly.
    Correction& correctionForTest() { return correction_; }

    /// The resolved correction, read-only, since the driver owns and rebuilds it.
    const Correction& correction() const { return correction_; }

    // The ONE correction field a container sets: everything else is derived and would be overwritten.
    /// Park or release this driver's motion.
    void setMotionHeld(bool held) { correction_.motionHeld = held; }
    /// Whether this driver's motion is currently parked.
    bool motionHeld() const { return correction_.motionHeld; }

protected:
    Layer* layer_ = nullptr;

    // The size differs per driver, so the caller passes the byte count; the lifecycle lives here.
    uint8_t* wire_ = nullptr;
    size_t   wireCap_ = 0;   // bytes the allocation returned (0 when it failed)

    // Internal RAM first: this is the encoder's hottest data, written and read back per light.
    /// Grow the correction scratch to at least `bytes`, keeping a big-enough existing block.
    void ensureWire(size_t bytes) {
        if (wire_ && wireCap_ >= bytes) return;
        freeWire();
        wire_ = static_cast<uint8_t*>(platform::allocInternal(bytes));
        if (!wire_) wire_ = static_cast<uint8_t*>(platform::alloc(bytes));
        wireCap_ = wire_ ? bytes : 0;
        publishHeapBytes();   // the scratch grew: refresh the memory readout
    }
    /// Release the scratch (on the true teardown: release(), not a mid-life reinit).
    void freeWire() {
        if (wire_) { platform::free(wire_); wire_ = nullptr; wireCap_ = 0; publishHeapBytes(); }
    }

    // One hook rather than setDynamicBytes at every alloc site, where one omission drifts the readout.
    virtual size_t driverHeapBytes() const { return wireCap_; }
    void publishHeapBytes() { setDynamicBytes(driverHeapBytes()); }

    // The wiring comes from a named preset by stable id; the render loop never reads the library.
    Correction correction_;
    uint32_t presetId_ = 0;          // stable id into the LightPresets library (0 → resolve to default)
    uint8_t presetSel_ = 0;          // the preset Select's chosen INDEX (mapped to an id in onControlChanged)
    uint8_t whiteMode_ = static_cast<uint8_t>(WhiteMode::Min);  // index into kWhiteModeOptions
    uint8_t localBrightness_ = 255;  // per-driver dim, multiplied with the global brightness
    /// Per-channel white balance; trims the stronger dies DOWN to match the weakest.
    uint8_t balRed_ = 255, balGreen_ = 255, balBlue_ = 255;
    /// The white die's own trim; the RGB trims do not reach it.
    uint8_t whiteLevel_ = 255;
    /// The current budget and the per-channel draw it is priced with.
    uint16_t budgetMa_ = 0;   // 0 = no current limiting
    uint8_t mAColor_ = 8;     // measured on SK6812 RGBW: R 7.98, G 8.11, B 7.98
    uint8_t mAWhite_ = 16;    // measured: W 16.11
    uint8_t mAYellow_ = 8;    // assumed: an amber die sits near red
    uint8_t mAUV_ = 8;        // assumed: a UV die usually draws more
    /// Whether this driver calls Correction::measure() before its emit loop.
    virtual bool limitsCurrent() const { return false; }

    /// Which perceptual curve the output LUT is built through; CIE lightness by default.
    uint8_t curveSel_ = 0;           // index into kCurveOptions; 0 = CIE
    static constexpr uint8_t kCurveCount = 4;
    static constexpr const char* kCurveOptions[kCurveCount] = {
        "CIE 1931",        // perceptual, the standard
        "gamma 2.2",       // the DISPLAY convention (sRGB's effective exponent)
        "gamma 2.8",       // the STAGE convention: emulates a tungsten dimmer's feel
        "linear"           // no curve: for a downstream device that corrects its own output
    };
    uint8_t lastGlobalBrightness_ = 0;  // last global brightness the container pushed (for self-rebuilds)
    // A preset id is a runtime handle, so what survives a reboot is the preset NAME.
    char presetRef_[16] = {};        // referenced preset's name (the durable reference)

    /// Add the correction controls: brightness, the preset selector, the curve and the white mode.
    void defineCorrectionControls() {
        controls_.addControl("localBrightness", localBrightness_, 0, 255);
        // Per driver, not global: a fixture that corrects its own pixels needs Linear here.
        controls_.addSelect("curve", curveSel_, kCurveOptions, kCurveCount);
        buildPresetOptions();                        // fill presetOptions_ from the library, sync id/sel/ref
        controls_.addSelect("lightPreset", presetSel_, presetOptions_, presetOptionCount_);
        controls_.addSelect("whiteMode", whiteMode_, kWhiteModeOptions, kWhiteModeCount);
        // Hidden unless the referenced preset carries a channel there is something to synthesise for.
        auto* lib = LightPresetsModule::active();
        controls_.setHidden(controls_.count() - 1, !(lib && lib->presetHasSynthChannel(presetId_)));
        controls_.addControl("balanceRed", balRed_, 0, 255);
        controls_.addControl("balanceGreen", balGreen_, 0, 255);
        controls_.addControl("balanceBlue", balBlue_, 0, 255);
        // Narrower than whiteMode's gate, which also counts amber and UV: this trims the white dies alone, so on a fixture with only those the slider would reach nothing.
        const bool hasWhite = lib && (lib->presetHasRole(presetId_, ChannelRole::White) ||
                                      lib->presetHasRole(presetId_, ChannelRole::WarmWhite));
        controls_.addControl("whiteLevel", whiteLevel_, 0, 255);
        controls_.setHidden(controls_.count() - 1, !hasWhite);
        // Only offered by the drivers that measure: a network sender feeds another board's supply.
        const bool limits = limitsCurrent();
        controls_.addControl("maxCurrentMa", budgetMa_, 0, 60000);
        controls_.setHidden(controls_.count() - 1, !limits);
        controls_.addControl("mAPerColorChannel", mAColor_, 1, 60);
        controls_.setHidden(controls_.count() - 1, !limits);
        controls_.addControl("mAPerWhiteChannel", mAWhite_, 1, 60);
        controls_.setHidden(controls_.count() - 1, !limits);
        // Only where the fixture carries them.
        const bool wide = lib && (lib->presetHasRole(presetId_, ChannelRole::Yellow) ||
                                  lib->presetHasRole(presetId_, ChannelRole::UV));
        controls_.addControl("mAPerYellowChannel", mAYellow_, 1, 60);
        controls_.setHidden(controls_.count() - 1, !(limits && wide));
        controls_.addControl("mAPerUvChannel", mAUV_, 1, 60);
        controls_.setHidden(controls_.count() - 1, !(limits && wide));
        // Persisted but not shown: the selector above is what the user sees.
        controls_.addText("presetRef", presetRef_, sizeof(presetRef_));
        controls_.setHidden(controls_.count() - 1, true);
    }

    /// Set this driver's default referenced preset by name, from its constructor.
    void setDefaultPresetName(const char* name) { std::snprintf(presetRef_, sizeof(presetRef_), "%s", name); }

    /// Whether `name` is one of the correction controls, for a driver's own prepare test.
    static bool isCorrectionControl(const char* name) {
        return std::strcmp(name, "lightPreset") == 0 || std::strcmp(name, "localBrightness") == 0 ||
               std::strcmp(name, "whiteMode") == 0 || std::strcmp(name, "curve") == 0 ||
               std::strcmp(name, "balanceRed") == 0 || std::strcmp(name, "balanceGreen") == 0 ||
               std::strcmp(name, "balanceBlue") == 0 || std::strcmp(name, "whiteLevel") == 0 ||
               std::strcmp(name, "maxCurrentMa") == 0 || std::strcmp(name, "mAPerColorChannel") == 0 ||
               std::strcmp(name, "mAPerWhiteChannel") == 0 || std::strcmp(name, "mAPerYellowChannel") == 0 ||
               std::strcmp(name, "mAPerUvChannel") == 0;
    }

private:
    // Borrowed pointers into the library's own name storage, which outlives the control list.
    const char* presetOptions_[LightPresetsModule::kMaxPresets] = {};
    uint8_t presetOptionCount_ = 0;
    void buildPresetOptions() {
        presetOptionCount_ = 0;
        auto* lib = LightPresetsModule::active();
        if (!lib) { presetOptions_[0] = "(none)"; presetOptionCount_ = 1; presetSel_ = 0; return; }
        const uint8_t n = lib->presetCount();
        for (uint8_t i = 0; i < n; i++) presetOptions_[i] = lib->nameAt(i);
        presetOptionCount_ = n;
        // Reconciled freshest-first: a user pick, then the persisted name, then the current id.
        if (presetSel_ < n && presetId_ != 0 && lib->idAt(presetSel_) != presetId_) {
            presetId_ = lib->idAt(presetSel_);                 // user picked a different preset
        } else if (presetRef_[0]) {
            for (uint8_t i = 0; i < n; i++)
                if (std::strcmp(lib->nameAt(i), presetRef_) == 0) { presetId_ = lib->idAt(i); break; }
        }
        if (presetId_ == 0) presetId_ = lib->defaultId();
        presetSel_ = lib->indexOfId(presetId_);
        std::snprintf(presetRef_, sizeof(presetRef_), "%s", lib->nameAt(presetSel_));
    }

protected:

    // Each driver names its own slice, rather than the buffer being split by driver order.
    static constexpr uint16_t kWindowAll = 65535;  ///< count default: clamped to buffer length = all lights
    uint16_t start_ = 0;   ///< First source-buffer light this driver outputs (default 0).
    uint16_t count_ = kWindowAll;   ///< Lights from start_; default kWindowAll (clamped to buffer = all).

    /// Add the two window controls, which a driver calls where its own controls go.
    void addWindowControls() {
        controls_.addControl("start", start_);
        controls_.addControl("count", count_);
    }

    /// Whether `name` is one of the window controls, for a driver's own prepare test.
    static bool isWindowControl(const char* name) {
        return std::strcmp(name, "start") == 0 || std::strcmp(name, "count") == 0;
    }

    /// Resolve the window against a buffer, writing the clamped first light and the length.
    void windowSlice(nrOfLightsType bufN, nrOfLightsType& outStart,
                     nrOfLightsType& outLen) const {
        outStart = start_ < bufN ? start_ : bufN;
        const nrOfLightsType avail = static_cast<nrOfLightsType>(bufN - outStart);
        // Matched EXPLICITLY: a buffer can exceed 65535 lights, which a literal count would cap.
        outLen = (count_ == 0 || count_ == kWindowAll || count_ > avail)
                     ? avail
                     : static_cast<nrOfLightsType>(count_);
    }

    // Both follow the same rule: clear the status only when it is the one this driver set.
    const char* configErr_ = nullptr;
    const char* configWarn_ = nullptr;
    char* failBuf_ = nullptr;
    // 64: the widest verdict is the loopback's worst case, which a narrower buffer clips.
    static constexpr size_t kFailBufLen = 64;

    // Remembered, so clearConfigErr can later retract exactly this one.
    void setConfigErr(const char* err) {
        configErr_ = err;
        setStatus(err, Severity::Error);
    }
    void clearConfigErr() {
        if (configErr_) {
            if (status() == configErr_) clearStatus();
            configErr_ = nullptr;
        }
    }

    // Called unconditionally each parse, so the warning tracks the live state.
    void setConfigWarn(const char* warn) {
        if (warn) {
            configWarn_ = warn;
            setStatus(warn, Severity::Warning);
        } else if (configWarn_) {
            if (status() == configWarn_) clearStatus();
            configWarn_ = nullptr;
        }
    }

    // Null on an allocation failure, where the caller falls back to a literal status.
    char* failBufEnsure() {
        if (!failBuf_) failBuf_ = static_cast<char*>(platform::alloc(kFailBufLen));
        return failBuf_;
    }
    void clearFailBuf() {
        if (failBuf_) {
            if (status() == failBuf_) clearStatus();
            platform::free(failBuf_);
            failBuf_ = nullptr;
        }
    }

    // Set LAST, so an error or warning from earlier in the same parse still wins.
    void setDrivingInfo(unsigned driven, unsigned total, unsigned channels = 1,
                        const char* mode = nullptr) {
        if (char* buf = failBufEnsure()) {
            int n;
            if (channels > 1)
                n = std::snprintf(buf, kFailBufLen, "driving %u of %u lights (%u channels)",
                                  driven, total, driven * channels);
            else
                n = std::snprintf(buf, kFailBufLen, "driving %u of %u lights", driven, total);
            if (mode && mode[0] && n > 0 && static_cast<size_t>(n) < kFailBufLen)
                std::snprintf(buf + n, kFailBufLen - static_cast<size_t>(n), ", %s", mode);
            setStatus(buf, Severity::Status);
        }
    }
};

} // namespace mm
