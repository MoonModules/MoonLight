#pragma once

#include "core/module/MoonModule.h"
#include "core/util/ScratchBuffer.h"
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/drivers/Correction.h"
#include "light/drivers/LedPeripheral.h"         // LedHwBlock: the peripheral-block claim guard's vocabulary
#include "light/drivers/FixtureProfilesModule.h"   // the shared fixture-profile library a driver references by id
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
        // Applied unconditionally, so brightness works even before the fixture-profile library is up.
        const uint8_t effective =
            static_cast<uint8_t>((globalBrightness * localBrightness_) / 255);
        effectiveBrightness_ = effective;
        // A driver that dims by on-time keeps full values in its table, so the colors keep every level.
        const uint8_t tableBrightness = dimsByTime() ? 255 : effective;
        correction_.whiteMode = static_cast<WhiteMode>(whiteMode_);
        correction_.curve = static_cast<Correction::Curve>(curveSel_);
        // A missing id falls back to the default, so a driver degrades rather than crashing.
        if (auto* lib = FixtureProfilesModule::active()) {
            adoptLegacyName(*lib);   // an older config's name, whichever path applied it: boot, re-apply or restore
            if (profileId_ == 0) profileId_ = lib->defaultId();
            if (!lib->deriveCorrection(profileId_, tableBrightness, correction_)) {
                profileId_ = lib->defaultId();                       // dangling → re-point to default
                lib->deriveCorrection(profileId_, tableBrightness, correction_);
            }
        } else {
            // No library yet, so apply brightness here, as deriveCorrection would have.
            correction_.rebuildBrightness(tableBrightness);
        }
        // Allocated the first time a profile has fine roles and kept until release, since the encode task may be reading it while this rebuild runs.
        const bool firstTable = correction_.hasFine && !lut16_;
        if (firstTable) lut16_.resize(256);
        correction_.lut16 = lut16_.data();
        if (firstTable) correction_.rebuildBrightness(tableBrightness);   // a table that existed was filled by the rebuild above
        onCorrectionChanged();      // let a driver resize its correction-applied buffer
    }

    /// Rebuild the correction when one of its own correction controls changed.
    void onControlChanged(const char* name) override {
        // The chosen row maps to a stable id, so the reference survives a later reorder.
        if (std::strcmp(name, "fixture") == 0) {
            if (auto* lib = FixtureProfilesModule::active()) profileId_ = lib->idAt(fixtureSel_);
        }
        if (isCorrectionControl(name)) rebuildCorrection(lastGlobalBrightness_);
        MoonModule::onControlChanged(name);
    }

    /// Notified when the output channel count may have changed without a structural rebuild.
    virtual void onCorrectionChanged() {}

    // HUB75 does: its panel shows a level only as time, so scaling values would cost levels.
    /// Whether this driver dims by how long it lights rather than by scaling the values it sends.
    virtual bool dimsByTime() const { return false; }
    /// Global times local brightness, as last rebuilt, for a driver that dims by time.
    uint8_t effectiveBrightness() const { return effectiveBrightness_; }

    /// Clear every shared status string, so a stopped driver leaves nothing behind.
    void release() override {
        freeWire();          // the shared correction scratch: owned here, so no driver re-frees it
        correction_.lut16 = nullptr;   // the table goes with the module's scratch buffers below
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

    // The wiring comes from a named profile by stable id; the render loop never reads the library.
    Correction correction_;
    ScratchBuffer<uint16_t> lut16_{*this};   // the correction's 16-bit table, allocated the first time a profile has fine roles
    uint32_t profileId_ = 0;          // stable id into the FixtureProfiles library (0 → resolve to default)
    uint8_t fixtureSel_ = 0;          // the fixture Select's chosen INDEX (mapped to an id in onControlChanged)
    uint8_t whiteMode_ = static_cast<uint8_t>(WhiteMode::Min);  // index into kWhiteModeOptions
    uint8_t localBrightness_ = 255;  // per-driver dim, multiplied with the global brightness
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
    uint8_t effectiveBrightness_ = 255; // global times local, for a driver that dims by time
    // A config saved before `fixture` held the profile's name kept it here; read for one release, then emptied.
    char fixtureRef_[16] = {};
    const char* defaultFixture_ = nullptr;   // the profile a new driver of this type starts on, by name

    /// Add the correction controls: brightness, the fixture selector, the curve and the white mode.
    void defineCorrectionControls() {
        controls_.addControl("localBrightness", localBrightness_, 0, 255);
        // Per driver, not global: a fixture that corrects its own pixels needs Linear here.
        controls_.addSelect("curve", curveSel_, kCurveOptions, kCurveCount);
        buildFixtureOptions();                        // fill fixtureOptions_ from the library, sync id and row
        controls_.addSelect("fixture", fixtureSel_, fixtureOptions_, fixtureOptionCount_);
        controls_.setPersistLabel(controls_.count() - 1);   // saved by name, so a longer or reordered library never moves it
        controls_.addSelect("whiteMode", whiteMode_, kWhiteModeOptions, kWhiteModeCount);
        // Hidden unless the referenced profile carries a channel there is something to synthesize for.
        auto* lib = FixtureProfilesModule::active();
        controls_.setHidden(controls_.count() - 1, !(lib && lib->profileHasSynthChannel(profileId_)));
        // Hidden, and only read: the name an older config saved.
        controls_.addText("fixtureRef", fixtureRef_, sizeof(fixtureRef_));
        controls_.setHidden(controls_.count() - 1, true);
    }

    /// Set this driver's default referenced profile by name, from its constructor.
    void setDefaultFixtureName(const char* name) { defaultFixture_ = name; }

    /// Whether `name` is one of the correction controls, for a driver's own prepare test.
    static bool isCorrectionControl(const char* name) {
        return std::strcmp(name, "fixture") == 0 || std::strcmp(name, "fixtureRef") == 0 || std::strcmp(name, "localBrightness") == 0
            || std::strcmp(name, "whiteMode") == 0 || std::strcmp(name, "curve") == 0;
    }

private:
    // Borrowed pointers into the library's own name storage, which outlives the control list.
    const char* fixtureOptions_[FixtureProfilesModule::kMaxProfiles] = {};
    uint8_t fixtureOptionCount_ = 0;
    void buildFixtureOptions() {
        fixtureOptionCount_ = 0;
        auto* lib = FixtureProfilesModule::active();
        if (!lib) { fixtureOptions_[0] = "(none)"; fixtureOptionCount_ = 1; fixtureSel_ = 0; return; }
        const uint8_t n = lib->profileCount();
        for (uint8_t i = 0; i < n; i++) fixtureOptions_[i] = lib->nameAt(i);
        fixtureOptionCount_ = n;
        reconcileProfile(*lib, n);
        fixtureSel_ = lib->indexOfId(profileId_);
    }

    /// Settle which profile the driver references: an older config's saved name, else the row picked, else the type's default.
    void reconcileProfile(const FixtureProfilesModule& lib, uint8_t n) {
        if (fixtureRef_[0]) {
            adoptLegacyName(lib);
        } else if (fixtureSel_ < n && profileId_ != 0 && lib.idAt(fixtureSel_) != profileId_) {
            profileId_ = lib.idAt(fixtureSel_);                 // a pick, by hand or by a saved name
        }
        if (profileId_ == 0 && defaultFixture_) profileId_ = profileNamed(lib, defaultFixture_);
        if (profileId_ == 0) profileId_ = lib.defaultId();
    }

    // An older config saved the name beside a row number that a longer library moved, so the name wins; read for one release.
    /// Adopt the profile an older config named in `fixtureRef`, select its row, and empty the field so a save writes only `fixture`.
    void adoptLegacyName(const FixtureProfilesModule& lib) {
        if (!fixtureRef_[0]) return;
        if (const uint32_t id = profileNamed(lib, fixtureRef_)) {
            profileId_ = id;
            fixtureSel_ = lib.indexOfId(id);
        }
        fixtureRef_[0] = '\0';
    }

    /// The id of the profile called `name`, or 0 when the library has none.
    static uint32_t profileNamed(const FixtureProfilesModule& lib, const char* name) {
        for (uint8_t i = 0; i < lib.profileCount(); i++)
            if (std::strcmp(lib.nameAt(i), name) == 0) return lib.idAt(i);
        return 0;
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
