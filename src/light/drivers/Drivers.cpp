/// The bodies of Drivers.h with logic beyond a line: the palette scripts, the controls, the relays, prepare and the encode worker's lifecycle, so a change to any of them rebuilds one object.

#include "light/drivers/Drivers.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>
#include <utility>

namespace mm {

Drivers::~Drivers() {
    stopEncodeTask();
    MoonLivePalette::clearActiveInstance(&paletteScriptModule_);
    // Free the script rather than detach: release() may be skipped, and the engine holds a block.
    paletteScriptModule_.release();
}

void Drivers::release() {
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

void Drivers::writeOutputs(JsonSink& sink) const {
    const nrOfLightsType n = source_ ? source_->count() : 0;
    bool first = true;
    for (uint8_t i = 0; i < childCount(); i++) {
        const MoonModule* c = child(i);
        if (c->role() != ModuleRole::Driver || !c->enabled()) continue;
        const auto* d = static_cast<const DriverBase*>(c);
        if (!d->drivesLights()) continue;
        sink.append(first ? "{\"driver\":" : ",{\"driver\":");
        sink.writeJsonString(c->typeName());
        // Frames a second: its own count where it keeps one, else every rendered frame, the device's rate.
        const std::optional<uint32_t> own = d->framesPerSecond();
        const uint32_t fps = own ? *own : (Scheduler::instance() ? Scheduler::instance()->fps() : 0);
        sink.appendf(",\"lights\":%u,\"fps\":%u", static_cast<unsigned>(d->windowLength(n)), static_cast<unsigned>(fps));
        sink.append(d->correction().hasMotion ? ",\"movingHeads\":true}" : "}");
        first = false;
    }
}

void Drivers::writeEffects(JsonSink& sink) const {
    if (!effects_) return;
    bool first = true;
    for (uint8_t i = 0; i < effects_->childCount(); i++) {
        const MoonModule* layer = effects_->child(i);
        if (layer->role() != ModuleRole::Layer || !layer->enabled()) continue;
        for (uint8_t j = 0; j < layer->childCount(); j++) {
            const MoonModule* e = layer->child(j);
            if (e->role() != ModuleRole::Effect || !e->enabled()) continue;
            if (!first) sink.append(",");
            sink.writeJsonString(e->name());
            first = false;
        }
    }
}

void Drivers::publishFixtureChannels() {
    const FixtureChannels fc = fixtureChannels();
    if (effects_) {
        for (uint8_t i = 0; i < effects_->childCount(); i++)
            if (effects_->child(i)->role() == ModuleRole::Layer)
                static_cast<Layer*>(effects_->child(i))->setFixtureChannels(fc);
    } else if (layer_) {
        layer_->setFixtureChannels(fc);
    }
}

FixtureChannels Drivers::fixtureChannels() {
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

void Drivers::attachPaletteScript() {
    MoonLivePalette::setActiveInstance(&paletteScriptModule_);
    paletteScriptModule_.setScript(paletteScript_);
    paletteScriptModule_.prepare(*this);
    demandPaletteAudio();
}

void Drivers::demandPaletteAudio() {
    const uint8_t d = paletteScript_[0] ? paletteScriptModule_.demands() : 0;
    paletteNotes_.set(d & AudioDemand::bitOf(AudioFeature::Notes));
    paletteAnalysis_.set(d & AudioDemand::bitOf(AudioFeature::Bands));
}

void Drivers::detachPaletteScript() {
    MoonLivePalette::setActiveInstance(nullptr);
    paletteScriptModule_.release();
    paletteScript_[0] = 0;
    demandPaletteAudio();
    // Only the script's own message: the card shares one status with the relay and encode warnings.
    if (status() == paletteScriptModule_.statusText()) clearStatus();
}

void Drivers::refreshLivePalettes() {
    live_.scanned = true;   // every scan counts, whichever caller ran it
    live_.count = 0;
    listCatalogPalettes();
    const uint8_t own = live_.count;
    // Then the user's own, from BOTH directories, user first, so an edited one appears once.
    const auto scan = [](const char* dir, void* ctx) {
        platform::fsList(dir, [](const char* name, bool isDir, uint32_t, void* c) {
            if (!isDir) static_cast<Drivers*>(c)->addUserPalette(name);
        }, ctx);
    };
    scan(moonlive::kScriptDir, this);
    scan(moonlive::kFactoryScriptDir, this);
    sortUserPalettes(own);
}

void Drivers::listCatalogPalettes() {
    // The catalog first, in its own order, so a palette's number is the same on every device of one firmware and a slot or a desk can pick it.
    for (size_t c = 0; c < moonlive::kPaletteCatalogCount && live_.count < LivePalettes::kMax; c++) {
        char path[96];
        live_.ptrs[live_.count] = moonlive::kPaletteCatalog[c];
        live_.tags[live_.count] = moonlive::kPaletteCatalogTags[c];
        live_.present[live_.count] = moonlive::resolveScript(moonlive::kPaletteCatalog[c], path, sizeof(path));
        live_.count++;
    }
}

void Drivers::addUserPalette(const char* name) {
    if (live_.count >= LivePalettes::kMax) return;
    const size_t n = std::strlen(name);
    if (n < 5 || std::strcmp(name + n - 4, moonlive::kPaletteExt) != 0) return;
    // A catalog palette, or one already listed, is not added twice.
    for (uint8_t i = 0; i < live_.count; i++)
        if (std::strcmp(live_.ptrs[i], name) == 0) return;
    // A bounded copy: a name longer than the slot truncates, which a picker wants.
    char* slot = live_.names[live_.count];
    const size_t cap = sizeof(live_.names[0]) - 1;
    const size_t copy = n < cap ? n : cap;
    std::memcpy(slot, name, copy);
    slot[copy] = '\0';
    live_.ptrs[live_.count] = slot;
    live_.tags[live_.count] = "";   // a script the catalog does not know is the user's own: no chips
    live_.present[live_.count] = true;
    live_.count++;
}

void Drivers::sortUserPalettes(uint8_t own) {
    // The user's own alphabetical among themselves, after the catalog.
    for (uint8_t i = static_cast<uint8_t>(own + 1); i < live_.count; i++)
        for (uint8_t j = i; j > own && LivePalettes::cmpName(live_.ptrs[j], live_.ptrs[j - 1]) < 0; j--) {
            std::swap(live_.ptrs[j], live_.ptrs[j - 1]);
            std::swap(live_.tags[j], live_.tags[j - 1]);
            std::swap(live_.present[j], live_.present[j - 1]);
        }
}

void Drivers::onFileChanged(const char* path) {
    if (!moonlive::touchesScripts(path)) return;
    paletteScriptModule_.fileChanged(path);
    if (paletteScript_[0]) { paletteScriptModule_.prepare(*this); demandPaletteAudio(); }
    refreshLivePalettes();
    LivePalettes::set(live_.ptrs, live_.tags, live_.count, live_.present);
    rebuildControls();
}

void Drivers::defineControls() {
    controls_.addControl("on", on);   // master power: first so it renders at the top of the card
    // A deviceModel fills this in, the same way it fills in the LED pins.
    controls_.addText("relayPins", relayPins, sizeof(relayPins));
    controls_.setAdvanced(controls_.count() - 1);   // wiring an installer sets once
    controls_.addControl("brightness", brightness, 0, 255);
    /// Blink the lights for five seconds, so this device is found by looking.
    /// @guide Dark and lit twice a second at their own brightness, never brighter. A peer's map presses it from a device's details.
    controls_.addButton("identify");
    // ONE picker for both kinds, so a `.mlp` is chosen exactly like a built-in; setup scans the folders and a file change rescans, and a brightness change leaves the list as it is.
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

void Drivers::onControlChanged(const char* controlName) {
    const auto is = [controlName](const char* name) { return std::strcmp(controlName, name) == 0; };
    if (is("identify")) {
        identifying_ = true;
        identifyAt_ = platform::millis();
        return;   // tick20ms blinks it
    }
    if (is("palette")) { onPaletteChanged(); return; }
    if (is("paletteScript")) { onPaletteScriptChanged(); return; }
    if (is("multicore")) {
        // Re-derive the schema, so the renderWait row appears and disappears with the switch.
        rebuildControls();
        return;
    }
    const bool power = is("on") || is("brightness");
    if (power) rebuildAllCorrections();
    // A state change, not a per-frame one: a mechanical relay would wear out at frame rate.
    if (power || is("relayPins")) applyRelay();
}

void Drivers::onPaletteChanged() {
    if (LivePalettes::isLive(palette)) {
        // The script fills the entries every frame, so no expansion is needed here.
        mm::formatTo(paletteScript_, sizeof(paletteScript_), "%s",
                      LivePalettes::nameAt(LivePalettes::sourceIndex(palette)));
        attachPaletteScript();
    } else {
        detachPaletteScript();
        Palettes::setActive(LivePalettes::sourceIndex(palette));
    }
    rebuildControls();   // the editor and the script's controls appear or disappear with it
}

void Drivers::onPaletteScriptChanged() {
    if (paletteScript_[0]) {
        attachPaletteScript();
    } else {
        // Clearing the name hands the palette back to the built-in select.
        detachPaletteScript();
        Palettes::setActive(palette);
    }
    rebuildControls();   // the compile re-derives the script's own controls
}

void Drivers::applyRelay() {
    // Each call judges the list afresh, so a warning a mid-edit list left goes once the list is fixed or cleared.
    if (showingRelayProblem_) { setStatus(""); showingRelayProblem_ = false; }
    // Release a pin the user cleared, or it stays asserted on a GPIO that no module owns.
    if (!relayPins[0]) { releaseRelays(); return; }
    uint16_t pins[kMaxRelays] = {};
    uint8_t n = 0;
    // Reporting the parse error is the difference between a typo'd list and a working one.
    if (const char* err = parsePinList(relayPins, pins, kMaxRelays, n)) {
        setStatus(err, Severity::Warning);
        showingRelayProblem_ = true;
        releaseRelays();   // what the OLD list held, or a typo mid-edit leaves the relays asserted
        return;
    }
    releaseLeavingRelays(pins, n);
    driveRelays(pins, n, on && brightness > 0);
}

void Drivers::releaseRelays() {
    for (uint8_t i = 0; i < lastRelayCount_; i++)
        platform::gpioWrite(lastRelayPins_[i], false);
    lastRelayCount_ = 0;
}

void Drivers::releaseLeavingRelays(const uint16_t* pins, uint8_t n) {
    // The pins LEAVING the list: shrinking "12,13" to "12" must not strand 13.
    for (uint8_t i = 0; i < lastRelayCount_; i++) {
        bool stillListed = false;
        for (uint8_t j = 0; j < n; j++)
            if (lastRelayPins_[i] == static_cast<uint8_t>(pins[j])) { stillListed = true; break; }
        if (!stillListed) platform::gpioWrite(lastRelayPins_[i], false);
    }
}

void Drivers::driveRelays(const uint16_t* pins, uint8_t n, bool closed) {
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

void Drivers::updateMotionHold() MM_NONBLOCKING {
    const bool movable = anyMovableDriver();
    if (movableNow_ != movable) {
        movableNow_ = movable;
        rebuildControls();
    }
    if (on) {
        offSeconds_ = 0;
    } else if (offSeconds_ < 0xFFFF) {
        offSeconds_++;
    }
    // 0 keeps tracking: the rig stays aimed however long the power is off, as a desk does.
    const bool held = !on && motionHold != kMotionHoldNever && offSeconds_ >= motionHold;
    for (uint8_t i = 0; i < childCount(); i++) {
        if (child(i)->role() != ModuleRole::Driver) continue;
        static_cast<DriverBase*>(child(i))->setMotionHeld(held);
    }
}

bool Drivers::anyMovableDriver() const MM_NONBLOCKING {
    // Read from each driver's ALREADY-RESOLVED correction: re-resolving costs a LUT per child.
    for (uint8_t i = 0; i < childCount(); i++) {
        if (child(i)->role() != ModuleRole::Driver || !child(i)->enabled()) continue;
        const Correction& c = static_cast<DriverBase*>(child(i))->correction();
        if (c.offPan != Correction::kAbsent || c.offTilt != Correction::kAbsent) return true;
    }
    return false;
}

void Drivers::rebuildAllCorrections() {
    for (uint8_t i = 0; i < childCount(); i++) {
        if (child(i)->role() != ModuleRole::Driver) continue;
        static_cast<DriverBase*>(child(i))->rebuildCorrection(effectiveBrightness());
    }
}

void Drivers::setup() {
    // The saved choice starts as picking it does, a script palette compiled, since at boot no handler reacts to the value.
    onControlChanged("palette");
    dark_ = MoonModule::safeMode();
    // Every lit device boot, while a host process or a test starts at the set level.
    softStarting_ = platform::bootRecord().booted && !dark_;
    softStartBegun_ = false;
    MoonModule::setup();
    passBufferToDrivers();           // seeds each driver's correction via rebuildCorrection()
    // HERE rather than in prepare: every setup runs first, and a Layer sizes its buffer there.
    publishFixtureChannels();
    // Without this a relay board comes up dark until something toggles the control.
    applyRelay();
}

void Drivers::prepare() {
    rescanLivePalettes();
    bindSource();
    // The split wants a buffer even in the identity case: core 1 reads it while core 0 renders.
    publishFixtureChannels();

    const SourceShape shape = sourceShape();
    const bool needOutput = needsOutput(shape);
    const bool splitWanted = multicore && anyDriver() && shape.lights > 0;
    const bool sameConfig = splitWanted == splitWanted_ && shape == preparedShape_;   // a retry of the configuration the last prepare saw
    splitWanted_ = splitWanted;

    // A wedged worker may still be READING the buffer, so tear the task down before freeing.
    if (!quiesceEncode()) stopEncodeTask();
    allocateOutput(shape, (needOutput && shape.lights > 0) || splitWanted);
    // Read before a failed worker gives an identity layer's buffer back, which a retry claims again; a configuration whose buffer fit keeps fitting through one refusal, so a passing shortage leaves the retry running.
    splitBufferFits_ = outputBuffer_.data() != nullptr || (sameConfig && splitBufferFits_);

    // Decided from the alloc OUTCOME, so a board that cannot claim the buffer runs inline.
    settleSplit(splitWanted && outputBuffer_.data(), needOutput);
    // One POD, overwritten in place, pulled by the domain-neutral consumers.
    summary_.lightCount = static_cast<uint32_t>(shape.lights);
    summary_.channelsPerLight = shape.out ? shape.channels : 3;
    seat_.claim();         // first live Drivers wins (claim-if-empty; one exists in practice)
    outputSeat_.claim();   // and is the light output core reads
    passBufferToDrivers();
    preparedShape_ = sourceShape();
}

void Drivers::rescanLivePalettes() {
    // A rescan here picks up a script written since the last one.
    const uint8_t hadLive = live_.count;
    refreshLivePalettes();
    LivePalettes::set(live_.ptrs, live_.tags, live_.count, live_.present);
    // A CHANGED count needs the control rebuilt: `palette`'s maximum is baked at define time.
    if (live_.count != hadLive) rebuildControls();
}

void Drivers::bindSource() {
    // The top-level Effects, found by type when no source was injected, so a tree built from a document needs no wiring.
    if (!effects_ && !layer_)
        if (Scheduler* s = Scheduler::instance()) effects_ = static_cast<Effects*>(s->topOfType("Effects"));
    // Re-resolved from the bound container, so an API-rebuilt Layer is picked up here.
    if (effects_) layer_ = effects_->activeLayer();
}

bool Drivers::needsOutput(const SourceShape& shape) {
    return shape.out && (shape.layers > 1 || shape.mapped);
}

void Drivers::allocateOutput(const SourceShape& shape, bool want) {
    if (!want) {
        outputBuffer_.free();
    } else if (!outputBuffer_.allocate(shape.lights, shape.channels)) {
        // A failed allocation leaves data_ null, which tick() checks before blending.
        std::printf("  DEGRADE  Drivers::outputBuffer_ allocate failed for %u lights\n",
                    static_cast<unsigned>(shape.lights));
        outputBuffer_.free();   // leaves data_=nullptr, bytes()=0
    }
    setDynamicBytes(outputBuffer_.heap());
    wiredLast_ = false;   // a reallocated output holds no frame yet
}

void Drivers::settleSplit(bool shouldSplit, bool needOutput) {
    if (shouldSplit && !renderSplitActive_) {
        renderSplitActive_ = true;
        startEncodeTask();                 // spawns the task (at boot it parks in waitNotify)
        // An identity layer needs the shared buffer only for the split, so the inline path gives its memory and its copy back.
        if (!renderSplitActive_ && !needOutput) {
            outputBuffer_.free();
            setDynamicBytes(outputBuffer_.heap());
        }
    } else if (!shouldSplit && renderSplitActive_) {
        stopEncodeTask();                  // drains core 1 before we leave split mode
        renderSplitActive_ = false;
        // Turning multicore off is a choice: the old split's stall warning must not outlive it.
        if (encodeStalled_) { encodeStalled_ = false; setStatus("", Severity::Status); }
    }
}

void Drivers::runEncodeLoop() {
    // The subscription is per-task: feeding another task's is rejected and floods the log.
    platform::taskWdtSubscribe();
    while (!encodeStop_.load(std::memory_order_acquire)) {
        if (!platform::waitNotify(encodeTask_, 100)) { platform::taskWdtReset(); continue; }
        if (encodeStop_.load(std::memory_order_acquire)) break;
        // Every Driver child, through core's one gate and timing loop, now on core 1.
        tickChildren(&MoonModule::tick, RoleFilter::Only, ModuleRole::Driver, false);   // core 1: core 0's own time leaves this out
        platform::taskWdtReset();
        encodeDone_.store(true, std::memory_order_release);
    }
    platform::taskWdtUnsubscribe();   // leave the WDT clean before the task exits (no dangling entry)
}

void Drivers::retrySplit() MM_NONBLOCKING {
    if (!splitWanted_ || !splitBufferFits_) { splitRetryDelayS_ = 1; splitRetryInS_ = 0; splitEngagedS_ = 0; return; }
    if (renderSplitActive_) {
        splitRetryInS_ = 0;
        if (splitRetryDelayS_ > 1 && ++splitEngagedS_ >= kSplitRetryCapS) { splitRetryDelayS_ = 1; splitEngagedS_ = 0; }
        return;
    }
    splitEngagedS_ = 0;
    if (++splitRetryInS_ < splitRetryDelayS_) return;
    splitRetryInS_ = 0;
    if (splitRetryDelayS_ < kSplitRetryCapS) splitRetryDelayS_ *= 2;
    if (Scheduler* s = Scheduler::instance()) s->requestPrepareSelf(this);   // prepare decides, at the frame boundary
}

bool Drivers::quiesceEncode() {
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
    // Clear a previous stall warning: a status that stays forever describes the past.
    if (encodeStalled_) {
        encodeStalled_ = false;
        setStatus("", Severity::Status);
    }
    return true;
}

bool Drivers::anyDriver() const {
    for (uint8_t i = 0; i < childCount(); i++) {
        MoonModule* c = child(i);
        if (c->role() != ModuleRole::Driver) continue;   // skips the fixture-profile library (Generic)
        if (c->respectsEnabled() && !c->enabled()) continue;
        return true;
    }
    return false;
}

void Drivers::startEncodeTask() {
    if (!renderSplitActive_ || encodeTask_.impl) return;
    encodeStop_.store(false, std::memory_order_release);
    encodeDone_.store(true, std::memory_order_release);
    // 5 KB: drivers own their DMA buffers and a network driver sends from its own task, so the encode loops stay light, measured at 1 to 1.4 KB.
    if (!platform::spawnPinnedTask(encodeTask_, "mmEncode", &encodeTrampoline, this, 5120, 5, 1))
        renderSplitActive_ = false;   // couldn't create the task → inline path
}

void Drivers::stopEncodeTask() {
    if (!encodeTask_.impl) return;
    encodeStop_.store(true, std::memory_order_release);
    platform::stopPinnedTask(encodeTask_);   // signals + wakes + joins (worker exits its loop)
}

Drivers::SourceShape Drivers::sourceShape() const {
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

void Drivers::passBufferToDrivers() {
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

} // namespace mm
