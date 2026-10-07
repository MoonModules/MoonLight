#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/util/PinList.h"        // parsePinList: the relay list, same parser the LED drivers use
#include "light/drivers/DriverBase.h"  // DriverBase: the Drivers container casts its children to it
#include "core/module/MoonModule.h"
#include "core/util/ActiveInstance.h"  // the summary-seat election (the seat + its RAII vacate)
#include "light/layers/Buffer.h"
#include "light/layers/Layer.h"
#include "light/layers/Effects.h"
#include "light/layers/BlendMap.h"
#include "light/drivers/Correction.h"
#include "light/util/Palette.h"   // the global active palette + its select control
#include "light/moonlive/MoonLivePalette.h"   // a palette computed per frame by a script
#include "core/moonlive/script_catalog.h"       // the tags each factory palette declares
#include "core/util/LightOutput.h"   // the seam the domain-neutral WLED, MQTT and MoonStats consumers read
#include "platform/platform.h"

#include <cstring>  // std::strcmp in onControlChanged
#include <atomic>   // encodeDone_: the render↔encode cross-core handoff flag

namespace mm {

/// Top-level container for the drivers: the consumer side of the pipeline. Owns the shared output buffer, composites every enabled layer into it each frame, and holds the global power, brightness and palette each Correction multiplies with.
///
/// Prior art: MoonLight's PhysicalLayer, which owns the display buffer and maps virtual channels into it.
///
/// @moreinfo
///
/// ## The shared output buffer
///
/// Blend and map write to arbitrary physical positions through a LUT, so the output is readable only once whole. One enabled layer with a 1:1 unshuffled mapping is the exception: drivers read that layer's buffer directly, giving up parallelism.
///
/// Two or more enabled layers composite in Effects order, bottom to top. Drivers owns that because only it sees both the stack order and the output buffer.
///
/// ## Per-driver source window
///
/// A window-aware driver outputs a contiguous slice, so each driver names its own lights. Reordering drivers changes nothing but tick order.
///
/// ## Naming
///
/// Capital `Drivers` is this container; lowercase "driver" is one `DriverBase` child.
///
/// @card Drivers.png
class Drivers : public MoonModule, public LightOutput {
public:
    // Drivers only, so the picker is not buried under every generic system module.
    /// Which child roles the "+ add" picker offers under Drivers.
    const char* acceptsChildRoles() const override { return "driver"; }

    /// The live light-pipeline summary.
    const LightSummary& summary() const override { return summary_; }
    /// The built-in palette nearest a color wheel's hue and saturation.
    uint8_t nearestPalette(uint16_t hue, uint8_t sat) const override { return Palettes::nearestForHue(hue, sat); }
    // A script computes its entries only while it runs, so a scripted index reads the active palette, which is the one a caller asks about.
    /// A palette's representative hue and saturation, built-in or scripted.
    void paletteHueSat(uint8_t index, uint16_t& hue, uint16_t& sat) const override {
        if (LivePalettes::isLive(index)) Palettes::hueSatOf(*Palettes::active(), hue, sat);
        else Palettes::representativeHueSat(index, hue, sat);
    }
    /// The built-ins and the scripted tail.
    uint8_t paletteCount() const override { return mm::paletteCount(); }
    /// Every palette name, in picker order.
    void writePaletteNames(JsonSink& sink) const override { mm::paletteNames(sink); }
    using LightOutput::nearestPalette;   // the RGB form, beside the override

    // Each seat REFERENCES this object's members, so leaving one published dangles a live reader.
    /// Release the children, then vacate every static seat this container published.
    void release() override {
        // BEFORE the children release: the worker ticks them, so it must stop before they free.
        stopEncodeTask();
        renderSplitActive_ = false;
        seat_.vacate();
        outputSeat_.vacate();
        // The palette seam references this object's name arrays rather than copying them.
        LivePalettes::clear(livePtrs_);
        // Effects::tick runs this every frame, so a dead Drivers would keep executing its script.
        MoonLivePalette::clearActiveInstance(&paletteScriptModule_);
        paletteScriptModule_.release();
        MoonModule::release();
    }

    // A destructor cannot be skipped, where an explicit release() can.
    /// Stop the worker and free the palette script, for a tree torn down without release().
    ~Drivers() override {
        stopEncodeTask();
        LivePalettes::clear(livePtrs_);
        MoonLivePalette::clearActiveInstance(&paletteScriptModule_);
        // Free the script rather than detach: release() may never run, and the engine holds a block.
        paletteScriptModule_.release();
    }

    // A driver walks the whole tree, so freeing ANY node while core 1 runs is a use-after-free.
    /// Stop the core-1 worker so a structural tree mutation can free nodes safely.
    void quiesceRenderSplit() {
        stopEncodeTask();
        renderSplitActive_ = false;
    }
    /// Reach the live Drivers (the one that owns the encode worker) to quiesce it around a mutation.
    static Drivers* active() { return ActiveInstance<Drivers>::active(); }

    // Callable BEFORE prepare, which is the point: Effects sizes its buffer first.
    /// Where this rig's fixtures keep their motion channels, as LAYER slots.
    void publishFixtureChannels() {
        const FixtureChannels fc = fixtureChannels();
        if (effects_) {
            for (uint8_t i = 0; i < effects_->childCount(); i++)
                if (effects_->child(i)->role() == ModuleRole::Layer)
                    static_cast<Layer*>(effects_->child(i))->setFixtureChannels(fc);
        } else if (layer_) {
            layer_->setFixtureChannels(fc);
        }
    }

    /// Resolve every enabled driver's fixture profile and fold their motion channels into one map.
    FixtureChannels fixtureChannels() {
        FixtureChannels fc;   // every offset absent: a rig with no motion, the common case
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Driver || !child(i)->enabled()) continue;
            auto* d = static_cast<DriverBase*>(child(i));
            d->rebuildCorrection(brightness);        // resolve the profile if it has not been yet
            const Correction& c = d->correction();
            if (!c.hasMotion) continue;
            // Layer slots, not channel numbers: packed after RGBW so a pan write cannot collide.
            const bool present[5] = {c.offPan    != Correction::kAbsent,
                                     c.offTilt   != Correction::kAbsent,
                                     c.offZoom   != Correction::kAbsent,
                                     c.offRotate != Correction::kAbsent,
                                     c.offGobo   != Correction::kAbsent};
            uint8_t* const dst[5] = {&fc.pan, &fc.tilt, &fc.zoom, &fc.rotate, &fc.gobo};
            FixtureChannels::forEachMotionSlot(present,
                [&](uint8_t role, uint8_t slot) { *dst[role] = slot; });
            break;   // a chain is homogeneous (architecture.md), so one layout describes it
        }
        return fc;
    }

    // Default low, around 8%: a fresh device on USB 5 V must not brown out at full white.
    /// Global brightness, scaling every channel through a 256-entry LUT.
    uint8_t brightness = 20;
    // Black without touching `brightness`, so toggling back restores the level.
    /// Master power.
    bool on = true;

    // A LIST, and not per driver: a relay gates the supply, which several drivers share.
    /// The GPIOs that switch the LED power supply, comma-separated; empty on most boards.
    char relayPins[24] = "";

    // Owned here because this container owns the handoff buffer and the frame boundary.
    /// Run the drivers' encode on the second core, so a frame costs max(render, encode).
    bool multicore = true;
    // The wire format is per driver; this container owns only the global brightness above.
    /// The global active color palette, which every palette-driven effect reads live.
    uint8_t palette = 0;

    // The names live in a member array because the seam holds POINTERS, so a local would dangle.
    /// Discover the `.mlp` files this device carries and publish their names to the picker.
    void refreshLivePalettes() {
        liveCount_ = 0;
        // BOTH directories, user first, so an edited factory palette appears once as the user's.
        const auto scan = [](const char* dir, void* ctx) {
            platform::fsList(dir, [](const char* name, bool isDir, uint32_t, void* c) {
                auto* self = static_cast<Drivers*>(c);
                if (isDir || self->liveCount_ >= LivePalettes::kMax) return;
                const size_t n = std::strlen(name);
                if (n < 5 || std::strcmp(name + n - 4, moonlive::kPaletteExt) != 0) return;
                // The user copy SHADOWS this one, so the factory pass must not add a second row.
                for (uint8_t i = 0; i < self->liveCount_; i++)
                    if (std::strcmp(self->livePtrs_[i], name) == 0) return;
                // A bounded copy: a name longer than the slot truncates, which a picker wants.
                char* slot = self->liveNames_[self->liveCount_];
                const size_t cap = sizeof(self->liveNames_[0]) - 1;
                const size_t copy = n < cap ? n : cap;
                std::memcpy(slot, name, copy);
                slot[copy] = '\0';
                self->livePtrs_[self->liveCount_] = self->liveNames_[self->liveCount_];
                self->liveCount_++;
            }, ctx);
        };
        scan(moonlive::kScriptDir, this);
        scan(moonlive::kFactoryScriptDir, this);
        // Alphabetical, because the picker merges this with the built-ins by walking both in order.
        for (uint8_t i = 1; i < liveCount_; i++)
            for (uint8_t j = i; j > 0 && LivePalettes::cmpName(livePtrs_[j], livePtrs_[j - 1]) < 0; j--) {
                const char* tmp = livePtrs_[j]; livePtrs_[j] = livePtrs_[j - 1]; livePtrs_[j - 1] = tmp;
            }
        // A `.mlp` the catalog does not know is the user's own, so it lists with no chips.
        for (uint8_t i = 0; i < liveCount_; i++) {
            liveTags_[i] = "";
            for (size_t c = 0; c < moonlive::kPaletteCatalogCount; c++)
                if (std::strcmp(livePtrs_[i], moonlive::kPaletteCatalog[c]) == 0) {
                    liveTags_[i] = moonlive::kPaletteCatalogTags[c];
                    break;
                }
        }
    }

    char        liveNames_[LivePalettes::kMax][moonlive::kMaxScriptName + 1] = {};
    const char* livePtrs_[LivePalettes::kMax] = {};
    const char* liveTags_[LivePalettes::kMax] = {};
    /// How many scripted palettes the last scan found.
    uint8_t     liveCount_ = 0;

    // Reached through a static seam, because the layers sample the palette before this ticks.
    /// The scripted palette: a `.mlp` name, and the binding that runs it. Empty means built-in.
    char paletteScript_[moonlive::kMaxScriptName + 1] = {};
    /// The engine that runs the selected scripted palette, one per Drivers.
    MoonLivePalette paletteScriptModule_;

    // Binding the container is self-healing; setLayer below pins one, for a test rig.
    /// Bind the Effects container; the source Layer re-resolves on every prepareTree.
    void setEffects(Effects* layers) {
        effects_ = layers;
        if (effects_) layer_ = effects_->activeLayer();
    }
    /// Pin one Layer directly, for a rig built outside an Effects container.
    void setLayer(Layer* layer) {
        effects_ = nullptr;  // explicit pin overrides container resolution
        layer_ = layer;
    }

    // Keeping `on` and `brightness` independent means "off" never clobbers the chosen level.
    /// The brightness the LUT is built from: 0 when powered off, else the set level.
    uint8_t effectiveBrightness() const { return on ? brightness : 0; }

    // `on=false` is a blackout between cues and a park between sets; duration separates them.
    /// How long a powered-off rig keeps tracking before its heads go still, in seconds.
    uint8_t motionHold = 30;
    static constexpr uint8_t kMotionHoldNever = 0;   ///< 0: keep tracking, the desk behavior

    /// Seconds the rig has been off, counted on tick1s; stops climbing once the hold expires.
    uint16_t offSeconds_ = 0;
    /// Whether any enabled driver was aimable last check, so the list rebuilds on the transition.
    bool     movableNow_ = false;

    /// Bind the global controls: power, brightness, relays, palette and the multicore split.
    void defineControls() override {
        controls_.addControl("on", on);   // master power: first so it renders at the top of the card
        // A deviceModel fills this in, the same way it fills in the LED pins.
        controls_.addText("relayPins", relayPins, sizeof(relayPins));
        controls_.setAdvanced(controls_.count() - 1);   // wiring an installer sets once
        controls_.addControl("brightness", brightness, 0, 255);
        // ONE picker for both kinds, so a `.mlp` is chosen exactly like a built-in.
        refreshLivePalettes();
        // Sized from THIS instance's scan: defineControls also runs before prepare has published.
        controls_.addPalette("palette", palette, mm::paletteOptions,
                             static_cast<uint8_t>(liveCount_ + mm::palettes::kCount));
        controls_.setPersistLabel(controls_.count() - 1);   // saved by name, which survives a script added before it
        // An EDITOR, not a second selector: `palette` owns the choice, so the two cannot disagree.
        controls_.addFilePath("paletteScript", paletteScript_, sizeof(paletteScript_),
                              moonlive::kPalettePick);
        controls_.setHidden(controls_.count() - 1, !LivePalettes::isLive(palette));
        controls_.setReadOnly(controls_.count() - 1, true);   // the selector is `palette`, above
        // And the script's own controls, which only exist while one is running.
        if (LivePalettes::isLive(palette)) paletteScriptModule_.publishControls(controls_);
        // Only where it can do something: a rig of LED strips has no aim to hold.
        controls_.addControl("motionHold", motionHold, 0, 240);
        controls_.setHidden(controls_.count() - 1, !fixtureChannels().movable());
        controls_.addControl("multicore", multicore);   // render↔encode split on/off (see the member's doc)
        controls_.setAdvanced(controls_.count() - 1);   // a tuning knob, not a user setting
        // How long core 0 waited at the frame boundary; a large value means core 0 idles.
        controls_.addReadOnly("renderWait", renderWaitStr_, sizeof(renderWaitStr_));
        controls_.setHidden(controls_.count() - 1, !multicore);
        controls_.setAdvanced(controls_.count() - 1);
        MoonModule::defineControls();  // cascade to driver children (each owns its fixture/whiteMode)
    }

    // Re-baking a LUT needs no pipeline realloc, which keeps the brightness slider fluent.
    /// React to a control change: re-bake the LUTs, resolve a palette, drive the relay.
    void onControlChanged(const char* controlName) override {
        if (std::strcmp(controlName, "palette") == 0) {
            if (LivePalettes::isLive(palette)) {
                // The script fills the entries every frame, so there is nothing to expand here.
                mm::formatTo(paletteScript_, sizeof(paletteScript_), "%s",
                              LivePalettes::nameAt(LivePalettes::sourceIndex(palette)));
                MoonLivePalette::setActiveInstance(&paletteScriptModule_);
                paletteScriptModule_.setScript(paletteScript_);
                paletteScriptModule_.prepare(*this);
            } else {
                // Detach any script, or it would keep overwriting the gradient every frame.
                MoonLivePalette::setActiveInstance(nullptr);
                paletteScript_[0] = 0;
                Palettes::setActive(LivePalettes::sourceIndex(palette));
            }
            rebuildControls();   // the editor and the script's controls appear or disappear with it
            return;
        }
        if (std::strcmp(controlName, "paletteScript") == 0) {
            // Only while a name is set, so clearing the field detaches the script.
            MoonLivePalette::setActiveInstance(paletteScript_[0] ? &paletteScriptModule_ : nullptr);
            paletteScriptModule_.setScript(paletteScript_);
            paletteScriptModule_.prepare(*this);
            rebuildControls();              // the compile re-derives the script's own controls
            // Clearing the name hands the palette back to the built-in select.
            if (paletteScript_[0] == 0) Palettes::setActive(palette);
            return;
        }
        if (std::strcmp(controlName, "multicore") == 0) {
            // Re-derive the schema, so the renderWait row appears and disappears with the switch.
            rebuildControls();
            return;
        }
        if (std::strcmp(controlName, "on") == 0 ||
            std::strcmp(controlName, "brightness") == 0) {
            rebuildAllCorrections();
        }
        // A state change, not a per-frame one: a mechanical relay would wear out at frame rate.
        if (std::strcmp(controlName, "on") == 0 || std::strcmp(controlName, "brightness") == 0 ||
            std::strcmp(controlName, "relayPins") == 0) {
            applyRelay();
        }
    }

    // Brightness 0 opens it too: a slider at 0 is how a WLED-style client says "off".
    /// Drive the power relay: closed while `on` and brightness is above zero, open otherwise.
    void applyRelay() {
        const bool closed = on && brightness > 0;
        // Each call judges the list afresh, so a warning a mid-edit list left goes once the list is fixed or cleared.
        if (showingRelayProblem_) { setStatus(""); showingRelayProblem_ = false; }
        // Release a pin the user cleared, or it stays asserted on a GPIO nothing owns.
        if (!relayPins[0]) {
            for (uint8_t i = 0; i < lastRelayCount_; i++)
                platform::gpioWrite(static_cast<uint8_t>(lastRelayPins_[i]), false);
            lastRelayCount_ = 0;
            return;
        }
        uint16_t pins[kMaxRelays] = {};
        uint8_t n = 0;
        // Reporting the parse error is the difference between a typo'd list and a working one.
        if (const char* err = parsePinList(relayPins, pins, kMaxRelays, n)) {
            setStatus(err, Severity::Warning);
            showingRelayProblem_ = true;
            // Release what the OLD list held, or a typo mid-edit leaves the relays asserted.
            for (uint8_t i = 0; i < lastRelayCount_; i++)
                platform::gpioWrite(lastRelayPins_[i], false);
            lastRelayCount_ = 0;
            return;
        }
        // Release the pins LEAVING the list: shrinking "12,13" to "12" must not strand 13.
        for (uint8_t i = 0; i < lastRelayCount_; i++) {
            bool stillListed = false;
            for (uint8_t j = 0; j < n; j++)
                if (lastRelayPins_[i] == static_cast<uint8_t>(pins[j])) { stillListed = true; break; }
            if (!stillListed) platform::gpioWrite(lastRelayPins_[i], false);
        }
        for (uint8_t i = 0; i < n; i++) {
            // An input-only pin refuses the write, and the seam says so rather than going quiet.
            if (!platform::gpioWrite(static_cast<uint8_t>(pins[i]), closed)) {
                setStatus("relay pin cannot drive an output", Severity::Warning);
                showingRelayProblem_ = true;
            }
            lastRelayPins_[i] = static_cast<uint8_t>(pins[i]);
        }
        lastRelayCount_ = n;
    }

    /// Relays one device can carry; four is the most any board in the catalog wires.
    static constexpr uint8_t kMaxRelays = 8;

    /// The pins driven last time, so clearing the list can still release them.
    uint8_t lastRelayPins_[kMaxRelays] = {};
    /// How many relay pins were driven last time, so a shrinking list can release the rest.
    uint8_t lastRelayCount_ = 0;
    /// Whether the status is a relay warning, so fixing the list clears it and nothing else.
    bool showingRelayProblem_ = false;

    // `multicore` alone is structural: it decides the handoff buffer and the core-1 task.
    /// Which controls route through the prepare sweep rather than the cheap correction tier.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "multicore") == 0;
    }

    // The PEAK over the second, not one sample: a lone sample reads ~0 even when core 0 idles.
    /// Refresh the read-only `renderWait` KPI once a second, off the hot path.
    void tick1s() MM_NONBLOCKING override {
        if (renderSplitActive_) mm::formatTo(renderWaitStr_, sizeof(renderWaitStr_), "%u µs",
                                              static_cast<unsigned>(renderWaitPeakUs_));
        else                    mm::formatTo(renderWaitStr_, sizeof(renderWaitStr_), "—");
        renderWaitPeakUs_ = 0;   // start a fresh window
        updateMotionHold();
        MoonModule::tick1s();
    }

    // The hold changes what is TRANSMITTED, so it writes the flag rather than rebuilding.
    /// Count the rig's time powered off, and park it once the hold expires.
    void updateMotionHold() MM_NONBLOCKING {
        // Read from each driver's ALREADY-RESOLVED correction: re-resolving costs a LUT per child.
        bool movable = false;
        for (uint8_t i = 0; i < childCount() && !movable; i++) {
            if (child(i)->role() != ModuleRole::Driver || !child(i)->enabled()) continue;
            const Correction& c = static_cast<DriverBase*>(child(i))->correction();
            movable = c.offPan != Correction::kAbsent || c.offTilt != Correction::kAbsent;
        }
        if (movableNow_ != movable) {
            movableNow_ = movable;
            rebuildControls();
        }
        if (on) {
            offSeconds_ = 0;
        } else if (offSeconds_ < 0xFFFF) {
            offSeconds_++;
        }
        // 0 means never park: keep tracking however long the power is off, as a desk does.
        const bool held = !on && motionHold != kMotionHoldNever && offSeconds_ >= motionHold;
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Driver) continue;
            static_cast<DriverBase*>(child(i))->setMotionHeld(held);
        }
    }

    // Through prepare() instead, an RMT teardown would blank the strip for a tick.
    /// Re-resolve every driver's correction without re-preparing the tree.
    void rebuildAllCorrections() {
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Driver) continue;
            static_cast<DriverBase*>(child(i))->rebuildCorrection(effectiveBrightness());
        }
    }

    /// Publish the fixture layout and the scripted palettes, and close the relay for `on`.
    void setup() override {
        Palettes::setActive(palette);   // seed the global active palette from the persisted index
        MoonModule::setup();
        passBufferToDrivers();           // seeds each driver's correction via rebuildCorrection()
        // HERE rather than in prepare: every setup runs first, and a Layer sizes its buffer there.
        publishFixtureChannels();
        // Without this a relay board comes up dark until something toggles the control.
        applyRelay();
    }

    /// Size the composition buffer, engage or drop the core-1 split, publish the summary.
    void prepare() override {
        // Published HERE because prepare runs only on a mounted module, and a probe would empty it.
        const uint8_t hadLive = liveCount_;
        refreshLivePalettes();
        LivePalettes::set(livePtrs_, liveTags_, liveCount_);
        // A CHANGED count needs the control rebuilt: `palette`'s maximum is baked at define time.
        if (liveCount_ != hadLive) rebuildControls();
        // Re-resolved from the bound container, so an API-rebuilt Layer is picked up here.
        if (effects_) layer_ = effects_->activeLayer();
        // A failed allocation leaves data_ null, which tick() checks before blending.
        Layer* const out = effects_ ? effects_->firstEnabledLayer() : layer_;
        const uint8_t enabled = effects_ ? effects_->enabledLayerCount() : (layer_ ? 1 : 0);
        const bool needOutput = out && (enabled > 1 || out->lut().hasLUT());

        // The split wants a buffer even in the identity case: core 1 reads it while core 0 renders.
        publishFixtureChannels();

        const bool haveLights = out && out->physicalLightCount() > 0;
        const bool splitWanted = multicore && anyDriver() && haveLights;
        const bool wantOutput = (needOutput && haveLights) || splitWanted;

        // A wedged worker may still be READING the buffer, so tear the task down before freeing.
        if (!quiesceEncode()) stopEncodeTask();
        if (wantOutput) {
            if (!outputBuffer_.allocate(out->physicalLightCount(), out->channelsPerLight())) {
                std::printf("  DEGRADE  Drivers::outputBuffer_ allocate failed for %u lights\n",
                            static_cast<unsigned>(out->physicalLightCount()));
                outputBuffer_.free();   // leaves data_=nullptr, bytes()=0
            }
        } else {
            outputBuffer_.free();
        }
        setDynamicBytes(outputBuffer_.bytes());

        // Decided from the alloc OUTCOME, so a board that cannot claim the buffer runs inline.
        const bool shouldSplit = splitWanted && outputBuffer_.data();
        if (shouldSplit && !renderSplitActive_) {
            renderSplitActive_ = true;
            startEncodeTask();                 // spawns the task (at boot it parks in waitNotify)
            // Keeping a buffer nothing composites into would freeze every driver on the last frame.
            if (!renderSplitActive_ && !needOutput) {
                outputBuffer_.free();
                setDynamicBytes(outputBuffer_.bytes());
            }
        } else if (!shouldSplit && renderSplitActive_) {
            stopEncodeTask();                  // drains core 1 before we leave split mode
            renderSplitActive_ = false;
            // Turning multicore off is a choice: the old split's stall warning must not outlive it.
            if (encodeStalled_) { encodeStalled_ = false; setStatus("", Severity::Status); }
        }
        // One POD, overwritten in place, pulled by the domain-neutral consumers.
        summary_.lightCount = out ? static_cast<uint32_t>(out->physicalLightCount()) : 0;
        summary_.channelsPerLight = out ? out->channelsPerLight() : 3;
        seat_.claim();         // first live Drivers wins (claim-if-empty; one exists in practice)
        outputSeat_.claim();   // and is the light output core reads
        passBufferToDrivers();
    }

    void tick() MM_NONBLOCKING override {
        // Core 1 is encoding the PREVIOUS frame, so wait it out before overwriting the buffer.
        if (renderSplitActive_) {
            uint32_t s0 = platform::micros();
            // JOIN a wedged worker: two cores inside one driver would double-transmit.
            if (!quiesceEncode()) stopEncodeTask();
            renderWaitUs_ = static_cast<uint32_t>(platform::micros() - s0);
            if (renderWaitUs_ > renderWaitPeakUs_) renderWaitPeakUs_ = renderWaitUs_;   // the 1 s window's worst, for the KPI
        }
        // Resolved ONCE: both single-layer branches need it, and an if-init shadowed the outer one.
        Layer* srcLayer = effects_ ? effects_->firstEnabledLayer() : layer_;

        if (outputBuffer_.data() && effects_ && effects_->enabledLayerCount() > 1) {
            // The bottom layer overwrites; each one above blends per its own mode and opacity.
            effects_->forEachEnabledLayer([&](Layer* L, bool first) {
                BlendOp op = first ? BlendOp::Overwrite : L->blendOp();
                uint8_t op_opacity = first ? 255 : L->opacity;
                blendMap(L->buffer(), outputBuffer_, L->lut(), L->channelsPerLight(),
                         op, op_opacity, /*clearFirst=*/first);
            });
        } else if (outputBuffer_.data() && srcLayer && srcLayer->lut().hasLUT()) {
            // One layer with a LUT: map its logical buffer into physical space.
            blendMap(srcLayer->buffer(), outputBuffer_, srcLayer->lut(), srcLayer->channelsPerLight());
        } else if (renderSplitActive_ && outputBuffer_.data() && srcLayer) {
            // Copied even in the identity case: core 1 must not read a buffer core 0 is mutating.
            blendMap(srcLayer->buffer(), outputBuffer_, srcLayer->lut(), srcLayer->channelsPerLight());
        }
        // A socket write still lands in lwIP on core 0, so core 1 builds and core 0 sends.
        if (renderSplitActive_) {
            encodeDone_.store(false, std::memory_order_release);
            platform::notifyTask(encodeTask_);
            tickNonDriverChildren();
        } else {
            MoonModule::tick();   // parent already composited; base ticks all children
        }
    }

    // This container picks the SIDE; core's one tickChildren loop still applies the rule.
    /// Tick the children that are not drivers, which core 1 never touches.
    void tickNonDriverChildren() { tickChildren(&MoonModule::tick, RoleFilter::Except, ModuleRole::Driver); }

    // Core calls this before every structural mutation, which is what makes a live delete safe.
    /// Bring core 1 to a stop, tearing the worker down if it will not come back.
    void quiesce() override { if (!quiesceEncode()) stopEncodeTask(); }

private:
    Effects* effects_ = nullptr;  // bound container; layer_ re-resolved from it at prepareTree
    Layer* layer_ = nullptr;
    Buffer outputBuffer_;

    // The RAII vacate is the same dangling-static guard the mic and registry seats use.
    LightSummary summary_;
    ActiveInstance<Drivers> seat_{*this};
    ActiveInstance<LightOutput> outputSeat_{*this};

    // The boundary is one shared buffer: core 0 waits on encodeDone_ before overwriting it.
    platform::WorkerTask encodeTask_{};
    std::atomic<bool> encodeDone_{true};   // core 1 sets true when its encode finishes; core 0 waits it
    std::atomic<bool> encodeStop_{false};  // stop flag the worker fn observes via a woken waitNotify
                                           // atomic, not volatile: volatile is no thread primitive
    bool renderSplitActive_ = false;   // the split is engaged (task spawned, boundary in effect)
    bool encodeStalled_ = false;       // a stall warning is showing, so recovery can clear it
    uint32_t renderWaitUs_ = 0;                 // last frame's core-0 wait at the boundary (the tick-line KPI)
    uint32_t renderWaitPeakUs_ = 0;             // worst wait in the current 1 s window (what the control shows)
    char renderWaitStr_[32] = {};               // the `renderWait` read-only control's text (refreshed in tick1s)

    // One rule, no per-driver opt-out: the whole output stage lives on core 1 while it is on.
    /// The core-1 body: wait for a notify, tick every driver, signal done.
    void runEncodeLoop() {
        // The subscription is per-task: feeding another task's is rejected and floods the log.
        platform::taskWdtSubscribe();
        while (!encodeStop_.load(std::memory_order_acquire)) {
            if (!platform::waitNotify(encodeTask_, 100)) { platform::taskWdtReset(); continue; }
            if (encodeStop_.load(std::memory_order_acquire)) break;
            // Every Driver child, through core's one gate and timing loop, now on core 1.
            tickChildren(&MoonModule::tick, RoleFilter::Only, ModuleRole::Driver);
            platform::taskWdtReset();
            encodeDone_.store(true, std::memory_order_release);
        }
        platform::taskWdtUnsubscribe();   // leave the WDT clean before the task exits (no dangling entry)
    }
    static void encodeTrampoline(void* self) { static_cast<Drivers*>(self)->runEncodeLoop(); }

    // The timeout is the robustness floor: a wedged worker disengages the split, slower but lit.
    static constexpr uint32_t kQuiesceTimeoutMs = 500;   // ≫ any real encode (~50 ms at 16K lights)
    bool quiesceEncode() {
        if (!renderSplitActive_) return true;
        const uint32_t deadline = platform::millis() + kQuiesceTimeoutMs;
        while (!encodeDone_.load(std::memory_order_acquire)) {
            if (platform::millis() > deadline) {
                setStatus("encode worker stalled - running single-core", Severity::Warning);
                encodeStalled_ = true;        // remember, so recovery can clear the warning
                renderSplitActive_ = false;   // stop notifying it; drivers tick inline from here
                return false;
            }
            platform::yield();
        }
        // Clear a previous stall warning: a status that cannot go away says nothing about now.
        if (encodeStalled_) {
            encodeStalled_ = false;
            setStatus("", Severity::Status);
        }
        return true;
    }

    // With no driver there is nothing for core 1 to do, so no task and no buffer.
    /// Whether any enabled driver child exists, which is what the split engages for.
    bool anyDriver() const {
        for (uint8_t i = 0; i < childCount(); i++) {
            MoonModule* c = child(i);
            if (c->role() != ModuleRole::Driver) continue;   // skips FixtureProfilesModule (Generic)
            if (c->respectsEnabled() && !c->enabled()) continue;
            return true;
        }
        return false;
    }

public:
    /// True while the render and encode split is engaged, for diagnostics and tests.
    bool renderSplitActive() const { return renderSplitActive_; }
    // A large value means core 0 idles, which is what a second handoff buffer would recover.
    /// The worst core-0 wait at the frame boundary in the current one-second window, in µs.
    uint32_t renderWaitPeakUs() const { return renderWaitPeakUs_; }
    // A test needs the boundary WITHOUT the fallback inline tick that follows a timeout.
    /// Test-only: the frame-boundary wait alone; false means it timed out and disengaged.
    bool quiesceEncodeForTest() { return quiesceEncode(); }

private:
    // PRIVATE: an outside caller could stop the task while renderSplitActive_ stayed true.
    /// Spawn the core-1 encode task, degrading to inline when it cannot be created.
    void startEncodeTask() {
        if (!renderSplitActive_ || encodeTask_.impl) return;
        encodeStop_.store(false, std::memory_order_release);
        encodeDone_.store(true, std::memory_order_release);
        // 8 KB: drivers own their DMA buffers, so the task stack stays light.
        if (!platform::spawnPinnedTask(encodeTask_, "mmEncode", &encodeTrampoline, this, 8192, 5, 1))
            renderSplitActive_ = false;   // couldn't create the task → inline path
    }
    /// Stop and join the core-1 task, draining its encode before any buffer it reads is freed.
    void stopEncodeTask() {
        if (!encodeTask_.impl) return;
        encodeStop_.store(true, std::memory_order_release);
        platform::stopPinnedTask(encodeTask_);   // signals + wakes + joins (worker exits its loop)
    }

    void passBufferToDrivers() {
        // Keyed off the buffer prepare() allocated, so ONE decision rather than two that disagree.
        Layer* const out = effects_ ? effects_->firstEnabledLayer() : layer_;
        Buffer* buf = out ? (outputBuffer_.data() ? &outputBuffer_ : &out->buffer())
                          : nullptr;
        for (uint8_t i = 0; i < childCount(); i++) {
            // The non-driver child has no source buffer or correction to wire.
            if (child(i)->role() != ModuleRole::Driver) continue;
            auto* drv = static_cast<DriverBase*>(child(i));
            drv->setSourceBuffer(buf);
            // Geometry uses the fallback layer, so a preview keeps its coordinates while disabled.
            drv->setLayer(layer_);
            // Each driver bakes global × its own brightness into its own LUT.
            drv->rebuildCorrection(effectiveBrightness());
        }
    }
};

} // namespace mm
