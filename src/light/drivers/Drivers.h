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
#include "core/moonlive/script_catalog.h"       // the tags each factory palette declares
#include "core/util/LightOutput.h"   // the seam the domain-neutral WLED, MQTT and MoonStats consumers read
#include "platform/platform.h"

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
    using LightOutput::nearestPalette;   // the RGB form, beside the override

    /// Release the children, then vacate every static seat this container published, since each seat references this object's members.
    void release() override {
        // BEFORE the children release: the worker ticks them, so it must stop before they free.
        stopEncodeTask();
        renderSplitActive_ = false;
        seat_.vacate();
        outputSeat_.vacate();
        // Effects::tick runs this every frame, so a dead Drivers would keep executing its script.
        MoonLivePalette::clearActiveInstance(&paletteScriptModule_);
        paletteScriptModule_.release();
        MoonModule::release();
    }

    /// Stop the worker and free the palette script, for a tree torn down without release(); a destructor runs where release() may not.
    ~Drivers() override {
        stopEncodeTask();
        MoonLivePalette::clearActiveInstance(&paletteScriptModule_);
        // Free the script rather than detach: release() may never run, and the engine holds a block.
        paletteScriptModule_.release();
    }

    /// Stop the core-1 worker so a structural tree mutation can free nodes safely, since a driver walks the whole tree.
    void quiesceRenderSplit() {
        stopEncodeTask();
        renderSplitActive_ = false;
    }
    /// Reach the live Drivers (the one that owns the encode worker) to quiesce it around a mutation.
    static Drivers* active() { return ActiveInstance<Drivers>::active(); }

    /// Where this rig's fixtures keep their motion channels, as LAYER slots, callable before prepare so Effects sizes its buffer first.
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

    /// Global output brightness, multiplied with each driver's `localBrightness`.
    /// @guide It starts low, so a fresh device on USB 5 V never browns out at full white.
    uint8_t brightness = 20;
    /// Master power.
    /// @guide Off scales the output to black and keeps `brightness`, so switching on restores the level. The UI, infrared, the WLED app, Home Assistant and MQTT all drive it.
    bool on = true;

    /// GPIOs switching the LED power supply, closed while `on` and brightness is above 0.
    char relayPins[24] = "";

    /// Run the output stage on the second core.
    /// @guide It falls back to one core by itself when the extra frame buffer does not fit. The switch exists to compare the two.
    bool multicore = true;
    /// What effects sample from: a built-in palette or a scripted `.mlp` one.
    uint8_t palette = 0;

    /// Run the script `paletteScript_` names, compiling it, whose size or error the card shows.
    void attachPaletteScript() {
        MoonLivePalette::setActiveInstance(&paletteScriptModule_);
        paletteScriptModule_.setScript(paletteScript_);
        paletteScriptModule_.prepare(*this);
    }

    /// Stop and release the palette script, so neither the program nor its status outlives the choice. Otherwise it keeps overwriting the gradient every frame.
    void detachPaletteScript() {
        MoonLivePalette::setActiveInstance(nullptr);
        paletteScriptModule_.release();
        paletteScript_[0] = 0;
        // Only the script's own message: the card shares one status with the relay and encode warnings.
        if (status() == paletteScriptModule_.statusText()) clearStatus();
    }

    /// List every factory palette at its place in the catalog, held or not, then the user's own `.mlp` files, and publish them to the picker. The names live in member arrays, as the seam holds pointers.
    void refreshLivePalettes() {
        live_.scanned = true;   // every scan counts, whichever caller ran it
        live_.count = 0;
        // The catalog first, in its own order, so a palette's number is the same on every device of one firmware and a slot or a desk can pick it.
        for (size_t c = 0; c < moonlive::kPaletteCatalogCount && live_.count < LivePalettes::kMax; c++) {
            char path[96];
            live_.ptrs[live_.count] = moonlive::kPaletteCatalog[c];
            live_.tags[live_.count] = moonlive::kPaletteCatalogTags[c];
            live_.present[live_.count] = moonlive::resolveScript(moonlive::kPaletteCatalog[c], path, sizeof(path));
            live_.count++;
        }
        const uint8_t own = live_.count;
        // Then the user's own, from BOTH directories, user first, so an edited one appears once.
        const auto scan = [](const char* dir, void* ctx) {
            platform::fsList(dir, [](const char* name, bool isDir, uint32_t, void* c) {
                auto* self = static_cast<Drivers*>(c);
                if (isDir || self->live_.count >= LivePalettes::kMax) return;
                const size_t n = std::strlen(name);
                if (n < 5 || std::strcmp(name + n - 4, moonlive::kPaletteExt) != 0) return;
                // A catalog palette, or one already listed, is not added twice.
                for (uint8_t i = 0; i < self->live_.count; i++)
                    if (std::strcmp(self->live_.ptrs[i], name) == 0) return;
                // A bounded copy: a name longer than the slot truncates, which a picker wants.
                char* slot = self->live_.names[self->live_.count];
                const size_t cap = sizeof(self->live_.names[0]) - 1;
                const size_t copy = n < cap ? n : cap;
                std::memcpy(slot, name, copy);
                slot[copy] = '\0';
                self->live_.ptrs[self->live_.count] = slot;
                self->live_.tags[self->live_.count] = "";   // a script the catalog does not know is the user's own: no chips
                self->live_.present[self->live_.count] = true;
                self->live_.count++;
            }, ctx);
        };
        scan(moonlive::kScriptDir, this);
        scan(moonlive::kFactoryScriptDir, this);
        // The user's own alphabetical among themselves, after the catalog.
        for (uint8_t i = static_cast<uint8_t>(own + 1); i < live_.count; i++)
            for (uint8_t j = i; j > own && LivePalettes::cmpName(live_.ptrs[j], live_.ptrs[j - 1]) < 0; j--) {
                std::swap(live_.ptrs[j], live_.ptrs[j - 1]);
                std::swap(live_.tags[j], live_.tags[j - 1]);
                std::swap(live_.present[j], live_.present[j - 1]);
            }
    }

    /// A palette script added, removed or restored joins the picker: any path in or above a script folder rescans them. The running one compiles again at once, an edit or a download landing with no control change.
    void onFileChanged(const char* path) override {
        if (!moonlive::touchesScripts(path)) return;
        paletteScriptModule_.fileChanged(path);
        if (paletteScript_[0]) paletteScriptModule_.prepare(*this);
        refreshLivePalettes();
        LivePalettes::set(live_.ptrs, live_.tags, live_.count, live_.present);
        rebuildControls();
    }

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
    const Buffer* source_ = nullptr;            ///< the frame the drivers read, which the rise waits to see lit
    uint32_t    softStartAt_ = 0;               ///< when they started rising

    /// The scripted palette's file, which `palette` picks.
    char paletteScript_[moonlive::kMaxScriptName + 1] = {};
    /// The engine that runs the selected scripted palette, one per Drivers.
    MoonLivePalette paletteScriptModule_;

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
        if (!on || dark_) return 0;
        if (!softStarting_) return brightness;
        if (!softStartBegun_) return 0;   // dark until the first frame, where the rise starts
        const uint32_t up = platform::millis() - softStartAt_;
        return up < kSoftStartMs ? static_cast<uint8_t>(brightness * up / kSoftStartMs) : brightness;
    }

    /// How long the lights take to rise to their brightness after the device boots, so a supply that cannot hold the level fails within the boot record's minute.
    static constexpr uint32_t kSoftStartMs = 5000;

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
    void defineControls() override {
        controls_.addControl("on", on);   // master power: first so it renders at the top of the card
        // A deviceModel fills this in, the same way it fills in the LED pins.
        controls_.addText("relayPins", relayPins, sizeof(relayPins));
        controls_.setAdvanced(controls_.count() - 1);   // wiring an installer sets once
        controls_.addControl("brightness", brightness, 0, 255);
        // ONE picker for both kinds, so a `.mlp` is chosen exactly like a built-in; setup scans the folders and a file change rescans, never a brightness change.
        if (!live_.scanned) refreshLivePalettes();
        // Published here too, so a saved palette name resolves when the file is applied, before setup.
        LivePalettes::set(live_.ptrs, live_.tags, live_.count, live_.present);
        // Sized from THIS instance's scan: defineControls also runs before prepare has published.
        controls_.addPalette("palette", palette, mm::paletteOptions,
                             static_cast<uint8_t>(live_.count + mm::palettes::kCount));
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

    /// React to a control change: re-bake the LUTs (no pipeline realloc, so the slider stays fluent), resolve a palette, drive the relay.
    void onControlChanged(const char* controlName) override {
        if (std::strcmp(controlName, "palette") == 0) {
            if (LivePalettes::isLive(palette)) {
                // The script fills the entries every frame, so there is nothing to expand here.
                mm::formatTo(paletteScript_, sizeof(paletteScript_), "%s",
                              LivePalettes::nameAt(LivePalettes::sourceIndex(palette)));
                attachPaletteScript();
            } else {
                detachPaletteScript();
                Palettes::setActive(LivePalettes::sourceIndex(palette));
            }
            rebuildControls();   // the editor and the script's controls appear or disappear with it
            return;
        }
        if (std::strcmp(controlName, "paletteScript") == 0) {
            if (paletteScript_[0]) {
                attachPaletteScript();
            } else {
                // Clearing the name hands the palette back to the built-in select.
                detachPaletteScript();
                Palettes::setActive(palette);
            }
            rebuildControls();              // the compile re-derives the script's own controls
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

    /// Drive the power relay: closed while `on` and brightness is above zero, open otherwise, so a WLED-style slider at 0 reads as off.
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
        MoonModule::tick1s();
    }

    /// Rebake the brightness tables through the soft start, the last time at the set level.
    void tick20ms() MM_NONBLOCKING override {
        if (softStarting_) {
            // From the first frame with a light lit, so the whole rise is seen, or once the boot counts as good, so a dark show stops scanning.
            if (!softStartBegun_) {
                if (!anyLit() && platform::millis() < platform::kBootStableMs) return MoonModule::tick20ms();
                softStartBegun_ = true;
                softStartAt_ = platform::millis();
            }
            const uint8_t level = effectiveBrightness();
            if (platform::millis() - softStartAt_ >= kSoftStartMs) softStarting_ = false;
            for (uint8_t i = 0; i < childCount(); i++)
                if (child(i)->role() == ModuleRole::Driver) static_cast<DriverBase*>(child(i))->rampBrightness(level);
        }
        MoonModule::tick20ms();
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

    /// Re-resolve every driver's correction without re-preparing the tree, since prepare() would tear down an RMT and blank the strip for a tick.
    void rebuildAllCorrections() {
        for (uint8_t i = 0; i < childCount(); i++) {
            if (child(i)->role() != ModuleRole::Driver) continue;
            static_cast<DriverBase*>(child(i))->rebuildCorrection(effectiveBrightness());
        }
    }

    /// Publish the fixture layout and the scripted palettes, and close the relay for `on`.
    void setup() override {
        // The filesystem module mounts first, so this scan is the one that sees every script; from here a file change rescans.
        refreshLivePalettes();
        LivePalettes::set(live_.ptrs, live_.tags, live_.count, live_.present);
        // The saved choice starts as picking it does, a script palette compiled, since at boot nothing reacts to the value.
        onControlChanged("palette");
        dark_ = MoonModule::safeMode();
        // Every lit device boot, and never a host process or a test, which start at the set level.
        softStarting_ = platform::bootRecord().booted && !dark_;
        softStartBegun_ = false;
        MoonModule::setup();
        passBufferToDrivers();           // seeds each driver's correction via rebuildCorrection()
        // HERE rather than in prepare: every setup runs first, and a Layer sizes its buffer there.
        publishFixtureChannels();
        // Without this a relay board comes up dark until something toggles the control.
        applyRelay();
    }

    /// Size the composition buffer, engage or drop the core-1 split, publish the summary.
    void prepare() override {
        // A rescan here picks up a script written since the last one.
        const uint8_t hadLive = live_.count;
        refreshLivePalettes();
        LivePalettes::set(live_.ptrs, live_.tags, live_.count, live_.present);
        // A CHANGED count needs the control rebuilt: `palette`'s maximum is baked at define time.
        if (live_.count != hadLive) rebuildControls();
        // The top-level Effects, found by type when nothing injected a source, so a tree built from a document needs no wiring.
        if (!effects_ && !layer_)
            if (Scheduler* s = Scheduler::instance()) effects_ = static_cast<Effects*>(s->topOfType("Effects"));
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
        wiredLast_ = false;   // a reallocated output holds no frame yet

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
        preparedShape_ = sourceShape();
    }

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
        }
        // Resolved ONCE: both single-layer branches need it, and an if-init shadowed the outer one.
        Layer* srcLayer = effects_ ? effects_->firstEnabledLayer() : layer_;
        // A frame already in wiring order, which goes to the output as it is: one copy, whatever the layout.
        EffectBase* const wired = outputBuffer_.data() && srcLayer ? srcLayer->wiringSource() : nullptr;
        composite(srcLayer, wired);
        wiredLast_ = wired != nullptr;
        // A socket write still lands in lwIP on core 0, so core 1 builds and core 0 sends.
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
            // The bottom layer overwrites; each one above blends per its own mode and opacity.
            effects_->forEachEnabledLayer([&](Layer* L, bool first) { blendLayer(*L, first); });
            return;
        }
        if (wired) {
            // Whole the first time, since the output held the mapped frame or a fresh allocation.
            wired->copyWiringFrame(outputBuffer_.data(), outputBuffer_.bytes(), !wiredLast_);
            return;
        }
        // A mapped layer goes through its LUT; an identity one is copied only for the split, as core 1 must not read a buffer core 0 is mutating.
        if (srcLayer && (srcLayer->lut().hasLUT() || renderSplitActive_))
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
    std::atomic<bool> encodeDone_{true};   // core 1 sets true when its encode finishes; core 0 waits it
    std::atomic<bool> encodeStop_{false};  // stop flag the worker observes via a woken waitNotify; atomic, not volatile, which is no thread primitive
    bool renderSplitActive_ = false;   // the split is engaged (task spawned, boundary in effect)
    bool wiredLast_ = false;   ///< the last frame went out as a wiring-order copy, so the output already holds it
    bool encodeStalled_ = false;       // a stall warning is showing, so recovery can clear it
    uint32_t renderWaitUs_ = 0;                 // last frame's core-0 wait at the boundary (the tick-line KPI)
    uint32_t renderWaitPeakUs_ = 0;             // worst wait in the current 1 s window (what the control shows)
    /// How long core 0 waited for core 1 at the frame boundary.
    /// @guide Near zero means render and output overlap well. A large value means core 0 idles on a slow output stage.
    char renderWaitStr_[32] = {};

    /// The core-1 body: wait for a notify, tick every driver, signal done. One rule with no per-driver opt-out: the whole output stage lives on core 1 while it is on.
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

    /// How long to wait for a wedged worker before disengaging the split, slower but lit; far above any real encode (~50 ms at 16K lights).
    static constexpr uint32_t kQuiesceTimeoutMs = 500;
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

    /// Whether any enabled driver child exists, which is what the split engages for; with none there is nothing for core 1 to do, so no task and no buffer.
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
    /// The worst core-0 wait at the frame boundary in the current one-second window, in µs; a large value means core 0 idles, which a second handoff buffer would recover.
    uint32_t renderWaitPeakUs() const { return renderWaitPeakUs_; }
    /// Test-only: the frame-boundary wait alone, without the fallback inline tick that follows a timeout; false means it timed out and disengaged.
    bool quiesceEncodeForTest() { return quiesceEncode(); }

private:
    /// Spawn the core-1 encode task, degrading to inline when it cannot be created; private, so the task never changes while renderSplitActive_ disagrees.
    void startEncodeTask() {
        if (!renderSplitActive_ || encodeTask_.impl) return;
        encodeStop_.store(false, std::memory_order_release);
        encodeDone_.store(true, std::memory_order_release);
        // 5 KB: drivers own their DMA buffers and a network driver sends from its own task, so the encode loops stay light, measured at 1 to 1.4 KB.
        if (!platform::spawnPinnedTask(encodeTask_, "mmEncode", &encodeTrampoline, this, 5120, 5, 1))
            renderSplitActive_ = false;   // couldn't create the task → inline path
    }
    /// Stop and join the core-1 task, draining its encode before any buffer it reads is freed.
    void stopEncodeTask() {
        if (!encodeTask_.impl) return;
        encodeStop_.store(true, std::memory_order_release);
        platform::stopPinnedTask(encodeTask_);   // signals + wakes + joins (worker exits its loop)
    }

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
    SourceShape sourceShape() const {
        SourceShape s;
        s.out = effects_ ? effects_->firstEnabledLayer() : layer_;
        s.layers = effects_ ? effects_->enabledLayerCount() : (layer_ ? 1 : 0);
        if (s.out) {
            s.mapped = s.out->lut().hasLUT();
            s.lights = s.out->physicalLightCount();
            s.count = outputBuffer_.data() ? 0 : s.out->buffer().count();   // the drivers size from it only when they read it directly
            s.channels = s.out->channelsPerLight();
        }
        return s;
    }

    void passBufferToDrivers() {
        // Keyed off the buffer prepare() allocated, so ONE decision rather than two that disagree.
        Layer* const out = effects_ ? effects_->firstEnabledLayer() : layer_;
        Buffer* buf = out ? (outputBuffer_.data() ? &outputBuffer_ : &out->buffer())
                          : nullptr;
        source_ = buf;
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
