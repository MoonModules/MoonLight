#pragma once

#include <optional>   // framesPerSecond: a count of its own, or none

#include "core/module/MoonModule.h"
#include "core/util/ScratchBuffer.h"
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/drivers/Correction.h"
#include "light/drivers/LayoutPicture.h"   // a screen or stream driver's picture, cropped to the window
#include "light/drivers/LedPeripheral.h"         // LedHwBlock: the peripheral-block claim guard's vocabulary
#include "light/drivers/FixtureProfilesModule.h"   // the shared fixture-profile library a driver references by id
#include "platform/platform_heap.h"   // allocInternal, alloc, free

#include <cstdio>       // std::snprintf for status strings
#include <cstring>     // std::strcmp (onControlChanged) / memset (buffer clears)
#include <cstdint>    // fixed-width ints
#include <algorithm> // std::min / max / clamp (chunk loops, size clamps)

namespace mm {

/// A driver consumes the light output and sends it on: to LEDs on a wire, a HUB75 panel, the network, a smart-light bridge or the web UI.
/// @video 07-drivers-host.webm Drivers on a host: preview, NDI, RTSP, HLS, network send and Hue.
/// @video 07-drivers-esp32.webm Drivers on a device: RMT and parallel LED pins, HUB75, panel cards, and the network senders.
///
/// Each reads its own slice of the @ref Drivers container's buffer through the shared window controls. A physical output corrects it through the shared correction controls, while @ref PreviewDriver shows it as it is. A card's emoji are its [tags](https://moonmodules.org/MoonLight/explanation/architecture/index.html#tag-emoji-legend).
///
/// @technical
///
/// **Interface here, bodies in `DriverBase.cpp`.** About 48 drivers include this base, so its bodies with real logic live in the `.cpp` and an edit there rebuilds one object; the brightness and color-order helpers each driver calls per frame stay here (coding-standards.md, File shape).
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
    /// Whether this driver reads a slice of the shared buffer, so its card shows start and count: every driver that lights something.
    virtual bool hasWindowControls() const { return drivesLights(); }
    /// Whether this driver lights something, which every output does and the preview, a picture of the output, does not.
    virtual bool drivesLights() const { return true; }
    /// Frames it sent last second where it counts them itself, a network output whose sends can fall behind the render; none means it sends every rendered frame.
    virtual std::optional<uint32_t> framesPerSecond() const { return std::nullopt; }
    /// Frames it dropped since it started, where it counts them: an output whose queue or wire falls behind; none means every frame it takes goes out.
    virtual std::optional<uint32_t> framesDropped() const { return std::nullopt; }
    /// The bytes its peripheral's DMA engine reads, a part of its heap shown on its own since that memory has to be DMA-capable and is scarce; 0 for an output with no DMA.
    virtual size_t dmaBytes() const { return 0; }
    /// The slack it had left over the last second before falling behind, in percent, 0 at the edge or behind; none where it measures none.
    virtual std::optional<uint8_t> headroom() const { return std::nullopt; }
    /// Its pace: its own frame count where it keeps one, else the render rate, since it sends every rendered frame.
    uint32_t paceFps() const;
    /// Set the Layer this driver reads its dimensions from: the active one, not a wiring constraint on how many layers feed it.
    void setLayer(Layer* layer) { layer_ = layer; }

    /// The first light of the configured window.
    int32_t windowStart() const { return start_; }
    /// Number of lights in the configured window (0 = to end of buffer).
    int32_t windowCount() const { return count_; }
    /// Set the window directly, which takes effect on the next parse as a control edit does.
    void setWindow(int32_t start, int32_t count) { start_ = start; count_ = count; }
    /// Place `picture` on `layer`'s physical front face, cropped to this driver's window of the `buffer`'s lights, else the whole face; false when its table finds no memory.
    bool placeWindowed(LayoutPicture& picture, const Layer& layer, const Buffer* buffer) const;
    /// How many lights this driver sends from a buffer of `bufN`: its window, clamped to the buffer.
    nrOfLightsType windowLength(nrOfLightsType bufN) const {
        nrOfLightsType outStart = 0, outLen = 0;
        windowSlice(bufN, outStart, outLen);
        return outLen;
    }
    /// The active Layer, which is null when none is wired, so a driver must tolerate that.
    Layer* layer() const { return layer_; }

    /// Rebuild this driver's correction, baking global times local brightness into one LUT, so the hot path stays one lookup per channel.
    void rebuildCorrection(uint8_t globalBrightness);

    /// Rebuild the correction when one of its own correction controls changed.
    void onControlChanged(const char* name) override;

    /// Re-resolve the fixture profile when the profile library's rows change, through the correction alone, since a prepare would reinitialize the output and blank it for a tick.
    void onListChanged(const MoonModule& owner) override;

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
    void release() override;

    /// Test-only: the driver's own Correction, mutable so a test can set a wiring directly.
    Correction& correctionForTest() { return correction_; }

    /// The resolved correction, read-only, since the driver owns and rebuilds it.
    const Correction& correction() const { return correction_; }

    /// Park or release this driver's motion; the one correction field a container sets, as everything else is derived and would be overwritten.
    void setMotionHeld(bool held) { correction_.motionHeld = held; }
    /// Whether this driver's motion is currently parked.
    bool motionHeld() const { return correction_.motionHeld; }

    /// The profile a new driver of this type starts on, by name, or null for none.
    const char* defaultFixtureName() const { return defaultFixture_; }

protected:
    Layer* layer_ = nullptr;

    /// Does an enabled sibling driver already claim `block`? RTTI-free: every driver answers hwBlock(), and one owning no block answers None.
    bool siblingClaims(LedHwBlock block) const;
    /// The status of a driver that stays idle because a sibling holds its block.
    static constexpr const char* kBlockInUse = "peripheral already in use by another driver: pick a different one";

    /// One light of a wiring wider than RGB, for correctToRgb, sized off the hot path by sizeLightScratch.
    ScratchBuffer<uint8_t> lightScratch_{*this};
    /// The status while one light of the wiring has no scratch to correct through.
    static constexpr const char* kNoLightMemory = "not enough memory for one light of this fixture";
    /// Whether the wiring is wider than RGB and the scratch for one light of it is missing or too small.
    bool lightScratchShort() const MM_NONBLOCKING {
        return correction_.outChannels > 3 && lightScratch_.bytes() < correction_.outChannels;
    }
    /// Size lightScratch_ to the wiring, saying so in the status when memory runs out; true when an RGB wiring needs none or the light fits.
    bool sizeLightScratch();

    /// Whether the wiring is plain RGB in the frame's own order, so a light corrects straight into an RGB frame; asked once per frame.
    bool rgbOrdered() const MM_NONBLOCKING {
        const Correction& c = correction_;
        return c.outChannels == 3 && c.offRed == 0 && c.offGreen == 1 && c.offBlue == 2;
    }

    /// Correct one light for an RGB-only wire: straight into `rgb` when `ordered`, else red, green and blue read where the profile puts them, 0 for a missing role; a wider wiring goes through lightScratch_, and without it the colors pass through.
    void correctToRgb(const uint8_t* src, uint8_t srcCh, uint8_t* rgb, bool ordered) MM_NONBLOCKING {
        if (ordered) { correction_.apply(src, rgb, srcCh); return; }
        const uint8_t outCh = correction_.outChannels;
        uint8_t three[3];
        uint8_t* light = outCh == 3 ? three : lightScratch_.count() >= outCh ? &lightScratch_[0] : nullptr;
        if (outCh < 3 || !light) { rgb[0] = src[0]; rgb[1] = src[1]; rgb[2] = src[2]; return; }
        correction_.apply(src, light, srcCh);
        rgb[0] = correction_.offRed != Correction::kAbsent ? light[correction_.offRed] : 0;
        rgb[1] = correction_.offGreen != Correction::kAbsent ? light[correction_.offGreen] : 0;
        rgb[2] = correction_.offBlue != Correction::kAbsent ? light[correction_.offBlue] : 0;
    }

    /// The same for `n` lights into a tight RGB frame at `dst`.
    void correctFrameToRgb(const uint8_t* src, uint8_t srcCh, uint8_t* dst, nrOfLightsType n) MM_NONBLOCKING {
        const bool ordered = rgbOrdered();
        for (nrOfLightsType i = 0; i < n; i++)
            correctToRgb(src + static_cast<size_t>(i) * srcCh, srcCh, dst + static_cast<size_t>(i) * 3, ordered);
    }

    /// The correction scratch; the size differs per driver, so the caller passes the byte count and the lifecycle lives here.
    uint8_t* wire_ = nullptr;
    size_t   wireCap_ = 0;   // bytes the allocation returned (0 when it failed)

    /// Grow the correction scratch to at least `bytes`, keeping a big-enough existing block, in internal RAM first as the encoder's hottest data.
    void ensureWire(size_t bytes);
    /// Release the scratch (on the true teardown: release(), not a mid-life reinit).
    void freeWire();

    /// The heap bytes this driver holds beside its ScratchBuffers, which count themselves; one hook rather than setDynamicBytes at every alloc site, where one omission drifts the readout.
    virtual HeapBytes driverHeapBytes() const { return heapOf(wire_, wireCap_); }
    /// Move the readout by the change in driverHeapBytes() since the last report, so every ScratchBuffer's own count stays in it. A driver reports through this alone, which keeps the total in step.
    void publishHeapBytes();
    HeapBytes reportedHeapBytes_;   ///< what driverHeapBytes() read at the last publish

    /// The resolved correction: the wiring comes from a named profile by stable id, and the render loop never reads the library.
    Correction correction_;

    /// Whether the frame a driver keeps between ticks is a fresh block or was written under another channel layout, once per change: the driver zeroes it first, so a channel with no role reads 0 rather than the last profile's byte.
    bool frameLayoutChanged() MM_NONBLOCKING {
        if (!correction_.layoutChanged) return false;
        correction_.layoutChanged = false;
        return true;
    }
    /// Say the kept frame is another block now, so the next tick zeroes it.
    void frameReplaced() { correction_.layoutChanged = true; }
    ScratchBuffer<uint16_t> lut16_{*this};   // the correction's 16-bit table, allocated the first time it is wanted
    ScratchBuffer<uint8_t> gainLut_{*this};    // the gained tables, only for a profile whose LED type carries a gain
    ScratchBuffer<uint16_t> gainLut16_{*this};   // the same to 16 bits, only where a channel is read wide
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
    void defineCorrectionControls();

    /// Set this driver's default referenced profile by name, from its constructor.
    void setDefaultFixtureName(const char* name) { defaultFixture_ = name; }
    /// Set this driver's default curve, from its constructor.
    void setDefaultCurve(Correction::Curve c) { curveSel_ = static_cast<uint8_t>(c); }

    /// Give the correction the gained tables its profile asks for, and take them back when it asks for none; true when one is new, which the rebuild before could not fill.
    bool resolveGainTables();

    /// Warn while a gain the profile asks for has no table to live in, so the strip shows ungained light visibly rather than silently; the next rebuild retries the allocation.
    void showGainShortage();

    /// Size one gained table to whether it is wanted, true when this call allocated it.
    template <typename T>
    static bool fitGainTable(ScratchBuffer<T>& table, bool wanted) {
        const bool fresh = wanted && !table;
        if (fresh) table.resize(Correction::kGainRows * 256);
        if (!wanted) table.resize(0);
        return fresh;
    }

    /// Whether `name` is one of the correction controls, for a driver's own prepare test.
    static bool isCorrectionControl(const char* name) {
        return std::strcmp(name, "fixture") == 0 || std::strcmp(name, "fixtureRef") == 0 || std::strcmp(name, "localBrightness") == 0
            || std::strcmp(name, "whiteMode") == 0 || std::strcmp(name, "curve") == 0;
    }

private:
    /// Add the two window controls, which defineControls places after the correction block.
    void addWindowControls();

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
    void buildFixtureOptions();

    /// Settle which profile the driver references: an older config's saved name, else the row picked, else the type's default.
    void reconcileProfile(const FixtureProfilesModule& lib, uint8_t n);

    /// Adopt the profile an older config named in `fixtureRef`, select its row, and empty the field so a save writes only `fixture`. The name wins over the saved row, which a longer library moved.
    void adoptLegacyName(const FixtureProfilesModule& lib);

    /// The id of the profile called `name`, or 0 when the library has none.
    static uint32_t profileNamed(const FixtureProfilesModule& lib, const char* name);

protected:

    /// The first light shown.
    int32_t start_ = 0;   // 32 bits, as is count: a wall with PSRAM holds more than 65535 lights
    /// How many, from `start`; 0 for the rest.
    /// @guide A status LED takes `start 0, count 1`, the strip `start 1`.
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
    /// The warning while a profile's gains have no table, which the driving line yields to.
    static constexpr const char* kNoGainMemory = "out of memory for the color balance, gains off";
    bool gainsDropped_ = false;
    char* failBuf_ = nullptr;
    /// The status buffer size: the widest verdict is the loopback's worst case, which a narrower buffer clips.
    static constexpr size_t kFailBufLen = 64;

    /// Show an error status and remember it, so clearConfigErr can later retract exactly this one.
    void setConfigErr(const char* err);
    void clearConfigErr();

    /// Show a warning, or retract the earlier one when null; called unconditionally each parse, so the warning tracks the live state.
    void setConfigWarn(const char* warn);

    /// The status buffer, allocated on first use; null on an allocation failure, where the caller falls back to a literal status.
    char* failBufEnsure();
    void clearFailBuf();

    /// Show how many lights are driven, set last so an error or warning from earlier in the same parse still wins.
    void setDrivingInfo(unsigned driven, unsigned total, unsigned channels = 1,
                        const char* mode = nullptr);
};

} // namespace mm
