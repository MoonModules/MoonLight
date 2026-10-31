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

/// A driver consumes the light output and sends it on: to LEDs on a wire, a HUB75 panel, the network, a smart-light bridge or the web UI.
/// @video 07-drivers-desktop.webm Drivers on a desktop: preview, NDI, RTSP, HLS, network send and Hue.
/// @video 07-drivers-esp32.webm Drivers on a device: RMT and parallel LED pins, HUB75, panel cards, and the network senders.
///
/// Each reads its own slice of the @ref Drivers container's buffer through the shared window controls. A physical output corrects it through the shared correction controls, while @ref PreviewDriver shows it as it is. A card's emoji are its [tags](https://moonmodules.org/MoonLight/explanation/architecture/index.html#tag-emoji-legend).
class DriverBase : public MoonModule {
public:
    /// This module's role, which is what the container filters its children by.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Driver; }
    /// Held from this many failed boots in a row, for a crash in the driver itself. Dark frames come first, since LEDs left without data hold their last frame and keep drawing current.
    static constexpr uint8_t kHeldAfter = 4;
    /// Held once safe mode has failed to stop the boots with dark frames.
    bool heldInSafeMode() const override { return MoonModule::failedBoots() >= kHeldAfter; }
    /// Point the driver at the buffer it reads its slice from.
    virtual void setSourceBuffer(Buffer* buf) = 0;

    /// The hardware block this driver claims, so two drivers cannot corrupt one peripheral; virtual rather than RTTI, which ESP32 builds compile without.
    virtual LedHwBlock hwBlock() const { return LedHwBlock::None; }

    /// Lead every driver card with the correction block, then the window, then the driver's own controls; a driver overrides defineDriverControls, so the placement is never re-implemented.
    void defineControls() final {
        if (hasCorrectionControls()) defineCorrectionControls();
        if (hasWindowControls()) addWindowControls();
        defineDriverControls();
    }

    /// A driver's own controls, added after the correction block and the window; this is what a driver overrides.
    virtual void defineDriverControls() {}

    /// Whether this driver exposes the correction controls, which a raw-RGB sink opts out of.
    virtual bool hasCorrectionControls() const { return true; }
    /// Whether this driver reads a slice of the shared buffer, so its card shows start and count.
    virtual bool hasWindowControls() const { return false; }
    /// Set the Layer this driver reads its dimensions from: the active one, not a wiring constraint on how many layers feed it.
    void setLayer(Layer* layer) { layer_ = layer; }

    /// The first light of the configured window.
    int32_t windowStart() const { return start_; }
    /// Number of lights in the configured window (0 = to end of buffer).
    int32_t windowCount() const { return count_; }
    /// Set the window directly, which takes effect on the next parse as a control edit does.
    void setWindow(int32_t start, int32_t count) { start_ = start; count_ = count; }
    /// Test-only: resolve the window against a buffer of `bufN` lights into its length.
    nrOfLightsType resolveWindowLenForTest(nrOfLightsType bufN) const {
        nrOfLightsType outStart = 0, outLen = 0;
        windowSlice(bufN, outStart, outLen);
        return outLen;
    }
    /// The active Layer, which is null when none is wired, so a driver must tolerate that.
    Layer* layer() const { return layer_; }

    /// Rebuild this driver's correction, baking global times local brightness into one LUT, so the hot path stays one lookup per channel.
    void rebuildCorrection(uint8_t globalBrightness) {
        // Applied unconditionally, so brightness works even before the fixture-profile library is up.
        const uint8_t tableBrightness = adoptBrightness(globalBrightness);
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
        // Allocated the first time the table is wanted and kept until release, since the encode task may be reading it while this rebuild runs.
        const bool firstTable = correction_.wantsTable() && !lut16_;
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

    /// Re-resolve the fixture profile when the profile library's rows change, through the correction alone, since a prepare would reinitialize the output and blank it for a tick.
    void onListChanged(const MoonModule& owner) override {
        if (&owner == static_cast<const MoonModule*>(FixtureProfilesModule::active()))
            rebuildCorrection(lastGlobalBrightness_);
    }

    /// Notified when the output channel count may have changed without a structural rebuild.
    virtual void onCorrectionChanged() {}

    /// Whether the output channel count changed since the last time a driver asked, so it re-lays its frame only then. A brightness change re-bakes the table and never resizes the frame.
    bool outChannelsChanged() {
        if (correction_.outChannels == laidOutChannels_) return false;
        laidOutChannels_ = correction_.outChannels;
        return true;
    }

    /// Whether this driver dims by how long it lights rather than by scaling the values it sends, as HUB75 does: its panel shows a level only as time.
    virtual bool dimsByTime() const MM_NONBLOCKING { return false; }
    /// Global times local brightness, as last rebuilt, for a driver that dims by time.
    uint8_t effectiveBrightness() const { return effectiveBrightness_; }
    /// Rebake only the brightness table for a level that moves every tick, the soft start, leaving the resolved profile as it is.
    void rampBrightness(uint8_t globalBrightness) MM_NONBLOCKING { correction_.rebuildBrightness(adoptBrightness(globalBrightness)); }

    /// Clear every shared status string, so a stopped driver leaves nothing behind; the owner calls it before destroying the driver, since a base destructor cannot prevent the vptr race.
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

    /// Park or release this driver's motion; the one correction field a container sets, as everything else is derived and would be overwritten.
    void setMotionHeld(bool held) { correction_.motionHeld = held; }
    /// Whether this driver's motion is currently parked.
    bool motionHeld() const { return correction_.motionHeld; }

protected:
    Layer* layer_ = nullptr;

    /// One light of a wiring wider than RGB, for correctToRgb, sized off the hot path by sizeLightScratch.
    ScratchBuffer<uint8_t> lightScratch_{*this};
    /// Size lightScratch_ to the wiring, saying so in the status when memory runs out; true when an RGB wiring needs none or the light fits.
    bool sizeLightScratch() {
        if (correction_.outChannels <= 3 || lightScratch_.resize(correction_.outChannels)) return true;
        setStatus("not enough memory for one light of this fixture", Severity::Error);
        return false;
    }

    /// Correct one light for a wire that carries RGB alone, reading red, green and blue where the profile puts them, 0 for a role it lacks; `scratch` holds one light of the wiring.
    void correctToRgb(const uint8_t* src, uint8_t srcCh, uint8_t* scratch, uint8_t* rgb) const MM_NONBLOCKING {
        correction_.apply(src, scratch, srcCh);
        rgb[0] = correction_.offRed != Correction::kAbsent ? scratch[correction_.offRed] : 0;
        rgb[1] = correction_.offGreen != Correction::kAbsent ? scratch[correction_.offGreen] : 0;
        rgb[2] = correction_.offBlue != Correction::kAbsent ? scratch[correction_.offBlue] : 0;
    }

    /// The correction scratch; the size differs per driver, so the caller passes the byte count and the lifecycle lives here.
    uint8_t* wire_ = nullptr;
    size_t   wireCap_ = 0;   // bytes the allocation returned (0 when it failed)

    /// Grow the correction scratch to at least `bytes`, keeping a big-enough existing block, in internal RAM first as the encoder's hottest data.
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

    /// The heap bytes this driver holds; one hook rather than setDynamicBytes at every alloc site, where one omission drifts the readout.
    virtual size_t driverHeapBytes() const { return wireCap_; }
    void publishHeapBytes() { setDynamicBytes(driverHeapBytes()); }

    /// The resolved correction: the wiring comes from a named profile by stable id, and the render loop never reads the library.
    Correction correction_;
    ScratchBuffer<uint16_t> lut16_{*this};   // the correction's 16-bit table, allocated the first time it is wanted
    uint32_t profileId_ = 0;          // stable id into the FixtureProfiles library (0 → resolve to default)
    /// The fixture profile applied per light, for channel order and white.
    /// @guide A reorder or a reboot keeps it. After renaming the profile, re-pick it: the next reboot falls back to the default.
    uint8_t fixtureSel_ = 0;
    /// How the white channel is derived, shown when the profile carries one.
    uint8_t whiteMode_ = static_cast<uint8_t>(WhiteMode::Min);
    /// This driver's dim, multiplied with the global brightness.
    uint8_t localBrightness_ = 255;
    /// How brightness maps to output: CIE 1931, a gamma, or linear.
    uint8_t curveSel_ = 0;
    static constexpr uint8_t kCurveCount = 4;
    static constexpr const char* kCurveOptions[kCurveCount] = {
        "CIE 1931",        // perceptual, the standard
        "gamma 2.2",       // the DISPLAY convention (sRGB's effective exponent)
        "gamma 2.8",       // the STAGE convention: emulates a tungsten dimmer's feel
        "linear"           // no curve: for a downstream device that corrects its own output
    };
    uint8_t lastGlobalBrightness_ = 0;  // last global brightness the container pushed (for self-rebuilds)
    uint8_t laidOutChannels_ = 0xFF;    // the channel count outChannelsChanged last saw, none yet
    uint8_t effectiveBrightness_ = 255; // global times local, for a driver that dims by time
    /// The profile name an older config saved, read once and then emptied.
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
    /// Add the two window controls, which defineControls places after the correction block.
    void addWindowControls() {
        controls_.addControl("start", start_, 0);
        controls_.setNumberField(controls_.count() - 1);   // typed: a light index is a number to read, not a position on a 2-billion slider
        controls_.addControl("count", count_, 0);
        controls_.setNumberField(controls_.count() - 1);
    }

    /// Take the global level, remembered for self-triggered rebuilds, and answer the level the table bakes.
    uint8_t adoptBrightness(uint8_t globalBrightness) MM_NONBLOCKING {
        lastGlobalBrightness_ = globalBrightness;
        effectiveBrightness_ = static_cast<uint8_t>((globalBrightness * localBrightness_) / 255);
        // A driver that dims by on-time keeps full values in its table, so the colors keep every level.
        return dimsByTime() ? 255 : effectiveBrightness_;
    }
    /// Borrowed pointers into the library's own name storage, which outlives the control list.
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

    /// Adopt the profile an older config named in `fixtureRef`, select its row, and empty the field so a save writes only `fixture`. The name wins over the saved row, which a longer library moved.
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

    /// The first light of the shared buffer this driver reads.
    int32_t start_ = 0;   // 32 bits, as is count: a wall with PSRAM holds more than 65535 lights
    /// How many lights it reads from `start`; 0 reads them all.
    /// @guide So drivers share one buffer: a status LED takes `start 0, count 1` and the strip `start 1`.
    int32_t count_ = 0;

    /// Whether `name` is one of the window controls, for a driver's own prepare test.
    static bool isWindowControl(const char* name) {
        return std::strcmp(name, "start") == 0 || std::strcmp(name, "count") == 0;
    }

    /// Resolve the window against a buffer, writing the clamped first light and the length.
    void windowSlice(nrOfLightsType bufN, nrOfLightsType& outStart,
                     nrOfLightsType& outLen) const {
        // The controls' lower bound keeps both at 0 or above, so they compare as unsigned.
        const uint32_t start = static_cast<uint32_t>(start_), count = static_cast<uint32_t>(count_);
        outStart = start < bufN ? static_cast<nrOfLightsType>(start) : bufN;
        const nrOfLightsType avail = static_cast<nrOfLightsType>(bufN - outStart);
        outLen = (count == 0 || count > avail) ? avail : static_cast<nrOfLightsType>(count);
    }

    /// The status verdicts this driver set; each is cleared only when the status still shows the one this driver set.
    const char* configErr_ = nullptr;
    const char* configWarn_ = nullptr;
    char* failBuf_ = nullptr;
    /// The status buffer size: the widest verdict is the loopback's worst case, which a narrower buffer clips.
    static constexpr size_t kFailBufLen = 64;

    /// Show an error status and remember it, so clearConfigErr can later retract exactly this one.
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

    /// Show a warning, or retract the earlier one when null; called unconditionally each parse, so the warning tracks the live state.
    void setConfigWarn(const char* warn) {
        if (warn) {
            configWarn_ = warn;
            setStatus(warn, Severity::Warning);
        } else if (configWarn_) {
            if (status() == configWarn_) clearStatus();
            configWarn_ = nullptr;
        }
    }

    /// The status buffer, allocated on first use; null on an allocation failure, where the caller falls back to a literal status.
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

    /// Show how many lights are driven, set last so an error or warning from earlier in the same parse still wins.
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
