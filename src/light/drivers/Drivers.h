#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/util/PinList.h"        // parsePinList: the relay list, same parser the LED drivers use
#include "light/drivers/DriverBase.h"  // DriverBase: the Drivers container casts its children to it
#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"   // topOfType: the Effects it finds when nothing injected a source
#include "core/util/ActiveInstance.h"  // the summary-seat election (the seat + its RAII vacate)
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/layers/Effects.h"
#include "light/layers/BlendMap.h"
#include "light/drivers/Correction.h"
#include "light/util/Palette.h"   // the global active palette + its select control
#include "light/moonlive/MoonLivePalette.h"   // a palette computed per frame by a script
#include "core/services/AudioDemand.h"         // what the palette script reads of the audio, as Demand.h describes
#include "core/moonlive/script_catalog.h"       // the tags each factory palette declares
#include "core/util/LightOutput.h"   // the seam the domain-neutral WLED, MQTT and MoonStats consumers read
#include "platform/platform_clock.h"   // millis, micros
#include "platform/platform_tasks.h"   // notifyTask, WorkerTask, taskWdtSubscribe
#include "platform/platform_system.h"   // kBootStableMs, bootRecord
#include "platform/platform_fs.h"   // fsList
#include "platform/platform_io.h"   // gpioWrite

#include <cstring>  // std::strcmp in onControlChanged
#include <atomic>   // encodeDone_: the render↔encode cross-core handoff flag
#include <utility>  // std::swap: sorting the user's own palettes

namespace mm {

/// The container of driver modules, owning the shared buffer and the per-light output correction every driver applies before sending.
/// @card Drivers.png
///
/// Every driver under it reads its own slice of that buffer, as the [drivers catalog](https://moonmodules.org/MoonLight/moonmodules/light/drivers.html) shows.
///
/// @technical
///
/// **Interface here, bodies in `Drivers.cpp`.** About 21 files include this container, so its cold-path bodies live in the `.cpp` and an edit there rebuilds one object; the ticks, `composite` and `anyLit` stay here, where the render path inlines them (coding-standards.md, File shape).
///
/// ## The shared output buffer
///
/// Blend and map write to arbitrary physical positions through a LUT, so the output is readable only once whole. One enabled layer with a 1:1 unshuffled mapping is the exception: drivers read that layer's buffer directly, giving up parallelism.
///
/// Two or more enabled layers composite in Effects order, bottom to top. Drivers owns that because only it sees both the stack order and the output buffer.
class Drivers : public MoonModule, public LightOutput {
public:
    /// Which child roles the "+ add" picker offers under Drivers, so it is not buried under every generic system module.
    const char* acceptsChildRoles() const override { return "driver"; }

    /// The live light-pipeline summary.
    const LightSummary& summary() const override { return summary_; }
    /// The built-in palette nearest a color wheel's hue and saturation.
    uint8_t nearestPalette(uint16_t hue, uint8_t sat) const override { return Palettes::nearestForHue(hue, sat); }
    /// A palette's representative hue and saturation, built-in or scripted; a scripted index reads the active palette, which is the one a caller asks about.
    void paletteHueSat(uint8_t index, uint16_t& hue, uint16_t& sat) const override {
        if (LivePalettes::isLive(index)) Palettes::hueSatOf(*Palettes::active(), hue, sat);
        else Palettes::representativeHueSat(index, hue, sat);
    }
    /// The built-ins and the scripted tail.
    uint8_t paletteCount() const override { return mm::paletteCount(); }
    /// Every palette name, in picker order.
    void writePaletteNames(JsonSink& sink) const override { mm::paletteNames(sink); }
    /// Each enabled driver that lights something, by type, with the lights its window sends from the frame the drivers read, its frames a second, and whether its fixtures are moving heads.
    void writeOutputs(JsonSink& sink) const override;
    /// Each enabled effect on an enabled layer, by name, in stack order.
    void writeEffects(JsonSink& sink) const override;
    using LightOutput::nearestPalette;   // the RGB form, beside the override

    /// Release the children, then vacate every static seat this container published, since each seat references this object's members.
    void release() override;

    /// Stop the worker and free the palette script, for a tree torn down without release(); a destructor runs where release() may not.
    ~Drivers() override;

    /// Stop the core-1 worker so a structural tree mutation can free nodes safely, since a driver walks the whole tree.
    void quiesceRenderSplit() {
        stopEncodeTask();
        renderSplitActive_ = false;
    }
    /// Reach the live Drivers (the one that owns the encode worker) to quiesce it around a mutation.
    static Drivers* active() { return ActiveInstance<Drivers>::active(); }

    /// Where this rig's fixtures keep their motion channels, as LAYER slots, callable before prepare so Effects sizes its buffer first.
    void publishFixtureChannels();

    /// Resolve every enabled driver's fixture profile and fold their motion channels into one map.
    FixtureChannels fixtureChannels();

    /// Global output brightness, multiplied with each driver's `localBrightness`.
    /// @guide It starts low, so a fresh device on USB 5 V never browns out at full white.
    uint8_t brightness = 20;
    /// Master power.
    /// @guide Off scales the output to black and keeps `brightness`, so switching on restores the level. The UI, infrared, the WLED app, Home Assistant and MQTT all drive it.
    bool on = true;

    /// GPIOs switching the LED power supply, closed while `on` and brightness is above 0.
    char relayPins[24] = "";

    /// Run the output stage on the second core.
    /// @guide It falls back to one core by itself when the extra frame buffer does not fit or the second core stalls, and tries two again, waiting at most about a minute between tries. The switch exists to compare the two.
    bool multicore = true;
    /// What effects sample from: a built-in palette or a scripted `.mlp` one.
    uint8_t palette = 0;

    /// Run the script `paletteScript_` names, compiling it, whose size or error the card shows.
    void attachPaletteScript();

    /// Need what the palette script reads of the audio, the notes or the analysis, while it runs, as an effect script does.
    void demandPaletteAudio();

    /// Stop and release the palette script, so neither the program nor its status outlives the choice. Otherwise it keeps overwriting the gradient every frame.
    void detachPaletteScript();

    /// List every factory palette at its place in the catalog, held or not, then the user's own `.mlp` files, and publish them to the picker. The names live in member arrays, as the seam holds pointers.
    void refreshLivePalettes();
    /// Fill the picker's first entries with the factory catalog, in its own order.
    void listCatalogPalettes();
    /// Append one user `.mlp` file to the picker, unless the list is full, the name is not a palette or it is listed already; the caller skips directories.
    void addUserPalette(const char* name);
    /// Order the user's own palettes alphabetically, after the first `own` catalog entries.
    void sortUserPalettes(uint8_t own);

    /// A palette script added, removed or restored joins the picker: any path in or above a script folder rescans them. The running one compiles again at once, an edit or a download landing with no control change.
    void onFileChanged(const char* path) override;

    /// The device's scripted palettes, as the last scan found them; static storage starts them zeroed.
    struct LiveScan {
        char        names[LivePalettes::kMax][moonlive::kMaxScriptName + 1];   ///< the user's own scripts' names, which `ptrs` points at
        const char* ptrs[LivePalettes::kMax];   ///< each palette's name
        const char* tags[LivePalettes::kMax];   ///< each palette's catalog chips, empty for the user's own
        bool        present[LivePalettes::kMax];   ///< whether the device holds the file, which the UI downloads on a pick when it does not
        uint8_t     count;     ///< how many the scan found
        bool        scanned;   ///< so a probe reads the list rather than rescanning it
    };
    /// Static, so the one published list outlives any instance, a probe's included.
    static inline LiveScan live_{};
    bool        softStarting_ = false;          ///< the lights are still rising after this boot
    bool        dark_ = false;                  ///< safe mode: every driver sends black, so no LED holds a bright frame
    bool        softStartBegun_ = false;        ///< the first lit frame has started the rise
    bool        identifying_ = false;           ///< `identify` was pressed and its blink runs
    bool        identifyDark_ = false;          ///< the blink's dark half, which effectiveBrightness reads
    uint32_t    identifyAt_ = 0;                ///< when `identify` was pressed
    const Buffer* source_ = nullptr;            ///< the frame the drivers read, which the rise waits to see lit
    uint32_t    softStartAt_ = 0;               ///< when they started rising

    /// The scripted palette's file, which `palette` picks.
    char paletteScript_[moonlive::kMaxScriptName + 1] = {};
    /// The engine that runs the selected scripted palette, one per Drivers.
    MoonLivePalette paletteScriptModule_;
    AudioDemand paletteNotes_{*this, AudioFeature::Notes};      ///< the notes, while the palette script calls a note builtin
    AudioDemand paletteAnalysis_{*this, AudioFeature::Bands};   ///< the analysis, while it calls a band, onset or peak builtin

    /// Bind the Effects container, which is self-healing: the source Layer re-resolves on every prepareTree.
    void setEffects(Effects* layers) {
        effects_ = layers;
        if (effects_) layer_ = effects_->activeLayer();
    }
    /// Pin one Layer directly, for a rig built outside an Effects container.
    void setLayer(Layer* layer) {
        effects_ = nullptr;  // explicit pin overrides container resolution
        layer_ = layer;
    }

    /// The brightness the LUT is built from: 0 when powered off, else the set level, rising through the soft start; `on` stays independent, so off never clobbers the level.
    uint8_t effectiveBrightness() const MM_NONBLOCKING {
        if (!on || dark_ || identifyDark_) return 0;
        if (!softStarting_) return brightness;
        if (!softStartBegun_) return 0;   // dark until the first frame, where the rise starts
        const uint32_t up = platform::millis() - softStartAt_;
        return up < kSoftStartMs ? static_cast<uint8_t>(brightness * up / kSoftStartMs) : brightness;
    }

    /// How long the lights take to rise to their brightness after the device boots, so a supply that cannot hold the level fails within the boot record's minute.
    static constexpr uint32_t kSoftStartMs = 5000;
    /// How long `identify` blinks the lights, and each half of a blink: twice a second, for five seconds.
    static constexpr uint32_t kIdentifyMs = 5000, kIdentifyHalfMs = 250;

    /// How long a powered-off rig keeps tracking before its heads go still, in seconds.
    /// @guide A short hold is a blackout between cues, a long one a park between sets.
    uint8_t motionHold = 30;
    static constexpr uint8_t kMotionHoldNever = 0;   ///< 0: keep tracking, the desk behavior

    /// Seconds the rig has been off, counted on tick1s; stops climbing once the hold expires.
    uint16_t offSeconds_ = 0;
    /// Whether any enabled driver was aimable last check, so the list rebuilds on the transition.
    bool     movableNow_ = false;

    /// A palette script's own controls appear once it compiles, so their saved values wait for that.
    bool declaresControlsAtPrepare() const override { return true; }

    /// Bind the global controls: power, brightness, relays, palette and the multicore split.
    void defineControls() override;

    /// React to a control change: re-bake the LUTs (no pipeline realloc, so the slider stays fluent), resolve a palette, drive the relay.
    void onControlChanged(const char* controlName) override;
    /// The `palette` select changed: start the chosen script or hand back to the built-in gradient.
    void onPaletteChanged();
    /// The `paletteScript` editor changed: compile the named script, or an empty name hands back to the select.
    void onPaletteScriptChanged();

    /// Drive the power relay: closed while `on` and brightness is above zero, open otherwise, so a WLED-style slider at 0 reads as off.
    void applyRelay();
    /// Open every pin the last list held and forget the list.
    void releaseRelays();
    /// Open the pins of the last list that the new list `pins` drops.
    void releaseLeavingRelays(std::span<const uint16_t> pins);
    /// Write `closed` to each of `pins`, and remember them as the list in force.
    void driveRelays(std::span<const uint16_t> pins, bool closed);

    /// Relays one device can carry; four is the most any board in the catalog wires.
    static constexpr uint8_t kMaxRelays = 8;

    /// The pins driven last time, so clearing the list can still release them.
    uint8_t lastRelayPins_[kMaxRelays] = {};
    /// How many relay pins were driven last time, so a shrinking list can release the rest.
    uint8_t lastRelayCount_ = 0;
    /// Whether the status is a relay warning, so fixing the list clears it and nothing else.
    bool showingRelayProblem_ = false;

    /// Which controls route through the prepare sweep rather than the cheap correction tier: `multicore` alone, as it decides the handoff buffer and the core-1 task.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "multicore") == 0;
    }

    /// Refresh the read-only `renderWait` KPI once a second, off the hot path, from the peak over the second, since one sample reads ~0 even when core 0 idles.
    void tick1s() MM_NONBLOCKING override {
        if (renderSplitActive_) mm::formatTo(renderWaitStr_, sizeof(renderWaitStr_), "%u µs",
                                              static_cast<unsigned>(renderWaitPeakUs_));
        else                    mm::formatTo(renderWaitStr_, sizeof(renderWaitStr_), "—");
        renderWaitPeakUs_ = 0;   // start a fresh window
        updateMotionHold();
        retrySplit();
        MoonModule::tick1s();
    }

    /// Rebake the brightness tables through the soft start, the last time at the set level.
    void tick20ms() MM_NONBLOCKING override {
        if (identifying_) {
            const uint32_t t = platform::millis() - identifyAt_;
            if (t >= kIdentifyMs) identifying_ = false;
            const bool dark = identifying_ && ((t / kIdentifyHalfMs) & 1u);
            if (dark != identifyDark_) {
                identifyDark_ = dark;
                rampDrivers(effectiveBrightness());
            }
        }
        if (softStarting_) {
            // From the first frame with a light lit, so the whole rise is seen, or once the boot counts as good, so a dark show stops scanning.
            if (!softStartBegun_) {
                if (!anyLit() && platform::millis() < platform::kBootStableMs) return MoonModule::tick20ms();
                softStartBegun_ = true;
                softStartAt_ = platform::millis();
            }
            const uint8_t level = effectiveBrightness();
            if (platform::millis() - softStartAt_ >= kSoftStartMs) softStarting_ = false;
            rampDrivers(level);
        }
        MoonModule::tick20ms();
    }

    /// Rebake every driver's brightness table at `level`, the one step the soft start and the identify blink share.
    void rampDrivers(uint8_t level) MM_NONBLOCKING {
        for (uint8_t i = 0; i < childCount(); i++)
            if (child(i)->role() == ModuleRole::Driver) static_cast<DriverBase*>(child(i))->rampBrightness(level);
    }

    /// Whether the frame the drivers read has any light on: its color slots, since an aim alone emits nothing.
    bool anyLit() const MM_NONBLOCKING {
        if (!source_ || !source_->data()) return false;
        const uint8_t* d = source_->data();
        const uint8_t cpl = source_->channelsPerLight();
        if (cpl == 0) return false;
        const uint8_t colors = cpl < FixtureChannels::kMotionBase ? cpl : FixtureChannels::kMotionBase;
        for (size_t at = 0; at + cpl <= source_->bytes(); at += cpl)
            for (uint8_t c = 0; c < colors; c++)
                if (d[at + c]) return true;
        return false;
    }

    /// Count the rig's time powered off, and park it once the hold expires; the hold changes what is transmitted, so it writes the flag rather than rebuilding.
    void updateMotionHold() MM_NONBLOCKING;
    /// Whether any enabled driver's resolved correction has a pan or tilt offset.
    bool anyMovableDriver() const MM_NONBLOCKING;

    /// Re-resolve every driver's correction without re-preparing the tree, since prepare() would tear down an RMT and blank the strip for a tick.
    void rebuildAllCorrections();

    /// Start the saved palette, publish the fixture layout, and close the relay for `on`; the scripted palettes were listed when the controls were bound.
    void setup() override;

    /// Size the composition buffer, engage or drop the core-1 split, publish the summary.
    void prepare() override;
    /// Rescan the palette scripts, publish them, and rebuild the controls when their count changed.
    void rescanLivePalettes();
    /// Find the Effects container when none was bound, and re-resolve the source Layer from it.
    void bindSource();

    /// Prepare again when a rebuild of one layer changed what this container sized from; a rebuild of the tree already did.
    void onTreePrepared() override {
        if (effectivelyEnabled() && sourceShape() != preparedShape_) applyState();
    }

    /// Composite the layers into the output buffer, then hand it to the drivers, on the encode core when the render is split.
    void tick() MM_NONBLOCKING override {
        // Core 1 is encoding the PREVIOUS frame, so wait it out before overwriting the buffer.
        if (renderSplitActive_) {
            uint32_t s0 = platform::micros();
            // JOIN a wedged worker: two cores inside one driver would double-transmit.
            if (!quiesceEncode()) stopEncodeTask();
            renderWaitUs_ = static_cast<uint32_t>(platform::micros() - s0);
            if (renderWaitUs_ > renderWaitPeakUs_) renderWaitPeakUs_ = renderWaitUs_;   // the 1 s window's worst, for the KPI
            renderWaitSumUs_ += renderWaitUs_;
        }
        // Resolved ONCE: both single-layer branches need it, and an if-init shadowed the outer one.
        Layer* srcLayer = effects_ ? effects_->firstEnabledLayer() : layer_;
        // A frame already in wiring order, which goes to the output as it is: one copy, whatever the layout.
        EffectBase* const wired = outputBuffer_.data() && srcLayer ? srcLayer->wiringSource() : nullptr;
        composite(srcLayer, wired);
        wiredLast_ = wired != nullptr;
        // Split: the encode task ticks the drivers, and the other children tick here.
        if (renderSplitActive_) {
            encodeDone_.store(false, std::memory_order_release);
            platform::notifyTask(encodeTask_);
            tickNonDriverChildren();
        } else {
            MoonModule::tick();   // parent already composited; base ticks all children
        }
    }

    /// Blend one layer into the shared output, each per its own mode and opacity, an invisible one above the bottom skipped. The bottom one lies over black: a plain overwrite at full opacity, faded by alpha below it, so any look can be the one faded out.
    void blendLayer(const Layer& L, bool first) {
        if (!first && !L.contributes()) return;
        const BlendOp op = !first ? L.blendOp() : L.opacity == 255 ? BlendOp::Overwrite : BlendOp::Alpha;
        blendMap(L.buffer(), outputBuffer_, L.lut(), L.channelsPerLight(), op, L.opacity, /*clearFirst=*/first);
        // Opacity fades color; the bottom layer's moving heads keep their aim however far it is faded.
        if (first && op != BlendOp::Overwrite)
            copyChannelsFrom(L.buffer(), outputBuffer_, L.lut(), L.channelsPerLight(), FixtureChannels::kMotionBase);
    }

    /// Fill the shared output from the layers: blended when several, a wiring-order frame as it is, else the one layer mapped. @xref{the-shared-output-buffer}
    void composite(Layer* srcLayer, EffectBase* wired) MM_NONBLOCKING {
        if (!outputBuffer_.data()) return;   // the drivers read the layer's buffer directly
        if (effects_ && effects_->enabledLayerCount() > 1) {
            // The bottom layer lies over black, faded by its opacity; each one above blends per its own mode and opacity.
            effects_->forEachEnabledLayer([&](Layer* L, bool first) { blendLayer(*L, first); });
            return;
        }
        if (wired) {
            // Whole the first time, since the output held the mapped frame or a fresh allocation.
            wired->copyWiringFrame(outputBuffer_.data(), outputBuffer_.bytes(), !wiredLast_);
            return;
        }
        // The drivers read this buffer whenever it exists, so it is filled whenever it exists: through the LUT, or copied for the split and for a split a stalled worker dropped to inline.
        if (srcLayer)
            blendMap(srcLayer->buffer(), outputBuffer_, srcLayer->lut(), srcLayer->channelsPerLight());
    }

    /// Tick the children that are not drivers, which core 1 never touches; this container picks the side and core's tickChildren applies the rule.
    void tickNonDriverChildren() { tickChildren(&MoonModule::tick, RoleFilter::Except, ModuleRole::Driver); }

    /// Bring core 1 to a stop, tearing the worker down if it will not come back; core calls it before every structural mutation, which makes a live delete safe.
    void quiesce() override { if (!quiesceEncode()) stopEncodeTask(); }

private:
    Effects* effects_ = nullptr;  // bound container; layer_ re-resolved from it at prepareTree
    Layer* layer_ = nullptr;
    Buffer outputBuffer_;

    /// The live summary and the seats that publish this container. The RAII vacate is the same dangling-static guard the mic and registry seats use.
    LightSummary summary_;
    ActiveInstance<Drivers> seat_{*this};
    ActiveInstance<LightOutput> outputSeat_{*this};

    /// The core-1 encode worker; the boundary is one shared buffer, and core 0 waits on encodeDone_ before overwriting it.
    platform::WorkerTask encodeTask_{};
    platform::Signal encodeDoneSignal_{};   // the encode task gives it with encodeDone_, so the render loop sleeps rather than spins
    std::atomic<bool> encodeDone_{true};   // core 1 sets true when its encode finishes; core 0 waits it
    std::atomic<bool> encodeStop_{false};  // stop flag the worker observes via a woken waitNotify; atomic, not volatile, which is no thread primitive
    bool renderSplitActive_ = false;   // the split is engaged (task spawned, boundary in effect)
    bool wiredLast_ = false;   ///< the last frame went out as a wiring-order copy, so the output already holds it
    bool encodeStalled_ = false;       // a stall warning is showing, so recovery can clear it
    bool splitWanted_ = false;         // the last prepare wanted the split, so tick1s retries one that is off
    bool splitBufferFits_ = false;     // this configuration has claimed the split's buffer: one that never could stays inline and the retry rests
    uint8_t splitRetryDelayS_ = 1;     // seconds between attempts, doubling up to kSplitRetryCapS
    uint8_t splitRetryInS_ = 0;        // seconds off since the last attempt
    uint8_t splitEngagedS_ = 0;        // seconds engaged since the last attempt, which resets the delay at the cap
    uint32_t renderWaitUs_ = 0;                 // last frame's core-0 wait at the boundary (the tick-line KPI)
    uint32_t renderWaitPeakUs_ = 0;             // worst wait in the current 1 s window (what the control shows)
    uint32_t renderWaitSumUs_ = 0;              // the waits of the current timing window, which publishTiming averages
    uint32_t renderWaitAvgUs_ = 0;              // the average wait per frame over the last window
    /// How long core 0 waited for core 1 at the frame boundary.
    /// @guide Near zero means render and output overlap well. A large value means core 0 idles on a slow output stage.
    char renderWaitStr_[32] = {};

    /// The core-1 body: wait for a notify, tick every driver, signal done. One rule with no per-driver opt-out: the whole output stage lives on core 1 while it is on.
    void runEncodeLoop();
    static void encodeTrampoline(void* self) { static_cast<Drivers*>(self)->runEncodeLoop(); }

    /// The longest wait between two attempts to re-engage the split, and how long it stays engaged before the next stall retries after 1 s again.
    static constexpr uint8_t kSplitRetryCapS = 64;
    /// Re-engage a split that is off while its buffer fits, through Drivers' own prepare on a back-off, so one stall or one refused worker costs the second core only until the next attempt.
    void retrySplit() MM_NONBLOCKING;

    /// How long to wait for a wedged worker before disengaging the split, slower but lit; far above any real encode (~50 ms at 16K lights).
    static constexpr uint32_t kQuiesceTimeoutMs = 500;
    bool quiesceEncode();

    /// Whether any enabled driver child exists, which is what the split engages for; with none there is nothing for core 1 to do, so no task and no buffer.
    bool anyDriver() const;

public:
    /// True while the render and encode split is engaged, for diagnostics and tests.
    bool renderSplitActive() const { return renderSplitActive_; }
    /// The worst core-0 wait at the frame boundary in the current one-second window, in µs; a large value means core 0 idles, which a second handoff buffer would recover.
    uint32_t renderWaitPeakUs() const { return renderWaitPeakUs_; }
    /// The average core-0 wait per frame over the last second, in µs, the tick time balance sheet's "waiting for encoder".
    uint32_t renderWaitAvgUs() const { return renderWaitAvgUs_; }
    /// Average the boundary wait with the module times, over the same frames.
    void publishTiming(uint32_t frameCount) MM_NONBLOCKING override {
        MoonModule::publishTiming(frameCount);
        renderWaitAvgUs_ = frameCount ? renderWaitSumUs_ / frameCount : 0;
        renderWaitSumUs_ = 0;
    }
    /// Test-only: the frame-boundary wait alone, without the fallback inline tick that follows a timeout; false means it timed out and disengaged.
    bool quiesceEncodeForTest() { return quiesceEncode(); }
    /// Test-only: stop and join the worker, as quiesce() does after a boundary wait times out.
    void stopEncodeForTest() { stopEncodeTask(); }

private:
    /// Spawn the core-1 encode task, degrading to inline when it cannot be created; private, so the task never changes while renderSplitActive_ disagrees.
    void startEncodeTask();
    /// Stop and join the core-1 task, draining its encode before any buffer it reads is freed.
    void stopEncodeTask();

    /// What prepare() decides the output buffer and the drivers' windows from: the first layer, how many layers composite, and that layer's size and mapping.
    struct SourceShape {
        const Layer* out = nullptr;
        uint8_t layers = 0;
        bool mapped = false;
        nrOfLightsType lights = 0;
        nrOfLightsType count = 0;
        uint8_t channels = 0;
        bool operator==(const SourceShape&) const = default;
    };
    SourceShape preparedShape_;
    SourceShape sourceShape() const;
    /// Whether the composition needs its own output buffer: several layers composite, or the first one is mapped.
    static bool needsOutput(const SourceShape& shape);
    /// Size the output buffer for `shape` when `want`, free it otherwise, and degrade visibly when the allocation fails.
    void allocateOutput(const SourceShape& shape, bool want);
    /// Engage or drop the core-1 split to match `shouldSplit`, giving an identity layer's buffer back when the inline path needs none.
    void settleSplit(bool shouldSplit, bool needOutput);

    void passBufferToDrivers();
};

} // namespace mm
