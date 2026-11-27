/// The bodies of DriverBase.h with logic beyond a line: the correction rebuild, the fixture profile resolution, the scratch lifecycle and the status verdicts, so a change to any of them rebuilds one object.

#include "light/drivers/DriverBase.h"
#include "core/module/Scheduler.h"   // the render rate, the pace of an output that keeps no count

#include <cstdio>
#include <cstring>

namespace mm {

uint32_t DriverBase::paceFps() const {
    const std::optional<uint32_t> own = framesPerSecond();
    return own ? *own : (Scheduler::instance() ? Scheduler::instance()->fps() : 0);
}

bool DriverBase::placeWindowed(LayoutPicture& picture, const Layer& layer, const Buffer* buffer) const {
    const lengthType w = layer.physicalWidth() > 0 ? layer.physicalWidth() : 1;
    const lengthType h = layer.physicalHeight() > 0 ? layer.physicalHeight() : 1;
    const nrOfLightsType total = buffer ? buffer->count() : static_cast<nrOfLightsType>(static_cast<size_t>(w) * h);
    nrOfLightsType from = 0, len = 0;
    windowSlice(total, from, len);
    const bool narrowed = from > 0 || len < total;
    return picture.place(layer, w, h, narrowed ? from : 0, narrowed ? len : 0);
}

void DriverBase::rebuildCorrection(uint8_t globalBrightness) {
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
    const bool firstGain = resolveGainTables();
    if (firstTable || firstGain) correction_.rebuildBrightness(tableBrightness);   // a table that existed was filled by the rebuild above
    showGainShortage();
    onCorrectionChanged();      // let a driver resize its correction-applied buffer
}

void DriverBase::onControlChanged(const char* name) {
    // The chosen row maps to a stable id, so the reference survives a later reorder.
    if (std::strcmp(name, "fixture") == 0) {
        if (auto* lib = FixtureProfilesModule::active()) profileId_ = lib->idAt(fixtureSel_);
    }
    if (isCorrectionControl(name)) rebuildCorrection(lastGlobalBrightness_);
    MoonModule::onControlChanged(name);
}

void DriverBase::onListChanged(const MoonModule& owner) {
    if (&owner == static_cast<const MoonModule*>(FixtureProfilesModule::active()))
        rebuildCorrection(lastGlobalBrightness_);
}

void DriverBase::release() {
    freeWire();          // the shared correction scratch: owned here, so no driver re-frees it
    correction_.lut16 = nullptr;   // the table goes with the module's scratch buffers below
    correction_.gainLut = nullptr;
    correction_.gainLut16 = nullptr;
    correction_.resolveTables();   // back onto the curve, so no row points into a freed table
    clearFailBuf();
    clearConfigErr();
    setConfigWarn(nullptr);
    MoonModule::release();
}

bool DriverBase::siblingClaims(LedHwBlock block) const {
    if (block == LedHwBlock::None) return false;
    const MoonModule* p = parent();
    if (!p) return false;
    for (uint8_t i = 0; i < p->childCount(); i++) {
        const MoonModule* sib = p->child(i);
        if (sib == this || !sib || !sib->enabled()) continue;
        // role() confirms it, so this static_cast needs no RTTI.
        if (sib->role() != ModuleRole::Driver) continue;
        if (static_cast<const DriverBase*>(sib)->hwBlock() == block) return true;
    }
    return false;
}

bool DriverBase::sizeLightScratch() {
    if (correction_.outChannels <= 3 || lightScratch_.resize(correction_.outChannels)) return true;
    setStatus(kNoLightMemory, Severity::Error);
    return false;
}

void DriverBase::ensureWire(size_t bytes) {
    if (wire_ && wireCap_ >= bytes) return;
    freeWire();
    wire_ = static_cast<uint8_t*>(platform::allocInternal(bytes));
    if (!wire_) wire_ = static_cast<uint8_t*>(platform::alloc(bytes));
    wireCap_ = wire_ ? bytes : 0;
    frameReplaced();
    publishHeapBytes();   // the scratch grew: refresh the memory readout
}

void DriverBase::freeWire() {
    if (wire_) { platform::free(wire_); wire_ = nullptr; wireCap_ = 0; publishHeapBytes(); }
}

void DriverBase::publishHeapBytes() {
    const HeapBytes now = driverHeapBytes();
    setDynamicBytes(heapBytes() - reportedHeapBytes_ + now);
    reportedHeapBytes_ = now;
}

void DriverBase::defineCorrectionControls() {
    controls_.addControl("localBrightness", localBrightness_, 0, 255);
    // Per driver, not global: a fixture that corrects its own pixels needs Linear here.
    controls_.addSelect("curve", curveSel_, kCurveOptions, kCurveCount);
    buildFixtureOptions();                        // fill fixtureOptions_ from the library, sync id and row
    controls_.addSelect("fixture", fixtureSel_, fixtureOptions_, fixtureOptionCount_);
    controls_.setPersistLabel(controls_.count() - 1);   // saved by name, so a longer or reordered library leaves it in place
    controls_.addSelect("whiteMode", whiteMode_, kWhiteModeOptions, kWhiteModeCount);
    // Hidden unless the referenced profile carries a channel there is something to synthesize for.
    auto* lib = FixtureProfilesModule::active();
    controls_.setHidden(controls_.count() - 1, !(lib && lib->profileHasSynthChannel(profileId_)));
    // Hidden, and only read: the name an older config saved.
    controls_.addText("fixtureRef", fixtureRef_, sizeof(fixtureRef_));
    controls_.setHidden(controls_.count() - 1, true);
}

bool DriverBase::resolveGainTables() {
    const bool wants = correction_.wantsGains();
    const bool wide = wants && correction_.wantsTable();
    const bool freshGain = fitGainTable(gainLut_, wants);
    const bool freshWide = fitGainTable(gainLut16_, wide);
    correction_.gainLut = wants ? gainLut_.data() : nullptr;
    correction_.gainLut16 = gainLut16_ ? gainLut16_.data() : nullptr;
    return freshGain || freshWide;
}

void DriverBase::showGainShortage() {
    const bool dropped = correction_.wantsGains() && (!gainLut_ || (correction_.wantsTable() && !gainLut16_));
    if (dropped) setStatus(kNoGainMemory, Severity::Warning);
    else if (gainsDropped_ && status() == kNoGainMemory) clearStatus();
    gainsDropped_ = dropped;
}

void DriverBase::addWindowControls() {
    controls_.addControl("start", start_, 0);
    controls_.setNumberField(controls_.count() - 1);   // typed: a light index is a number to read, not a position on a 2-billion slider
    controls_.addControl("count", count_, 0);
    controls_.setNumberField(controls_.count() - 1);
}

void DriverBase::buildFixtureOptions() {
    fixtureOptionCount_ = 0;
    auto* lib = FixtureProfilesModule::active();
    if (!lib) { fixtureOptions_[0] = "(none)"; fixtureOptionCount_ = 1; fixtureSel_ = 0; return; }
    const uint8_t n = lib->profileCount();
    for (uint8_t i = 0; i < n; i++) fixtureOptions_[i] = lib->nameAt(i);
    fixtureOptionCount_ = n;
    reconcileProfile(*lib, n);
    fixtureSel_ = lib->indexOfId(profileId_);
}

void DriverBase::reconcileProfile(const FixtureProfilesModule& lib, uint8_t n) {
    if (fixtureRef_[0]) {
        adoptLegacyName(lib);
    } else if (fixtureSel_ < n && profileId_ != 0 && lib.idAt(fixtureSel_) != profileId_) {
        profileId_ = lib.idAt(fixtureSel_);                 // a pick, by hand or by a saved name
    }
    if (profileId_ == 0 && defaultFixture_) profileId_ = profileNamed(lib, defaultFixture_);
    if (profileId_ == 0) profileId_ = lib.defaultId();
}

void DriverBase::adoptLegacyName(const FixtureProfilesModule& lib) {
    if (!fixtureRef_[0]) return;
    if (const uint32_t id = profileNamed(lib, fixtureRef_)) {
        profileId_ = id;
        fixtureSel_ = lib.indexOfId(id);
    }
    fixtureRef_[0] = '\0';
}

uint32_t DriverBase::profileNamed(const FixtureProfilesModule& lib, const char* name) {
    for (uint8_t i = 0; i < lib.profileCount(); i++)
        if (std::strcmp(lib.nameAt(i), name) == 0) return lib.idAt(i);
    return 0;
}

void DriverBase::setConfigErr(const char* err) {
    configErr_ = err;
    setStatus(err, Severity::Error);
}

void DriverBase::clearConfigErr() {
    if (configErr_) {
        if (status() == configErr_) clearStatus();
        configErr_ = nullptr;
    }
}

void DriverBase::setConfigWarn(const char* warn) {
    if (warn) {
        configWarn_ = warn;
        setStatus(warn, Severity::Warning);
    } else if (configWarn_) {
        if (status() == configWarn_) clearStatus();
        configWarn_ = nullptr;
    }
}

char* DriverBase::failBufEnsure() {
    if (!failBuf_) failBuf_ = static_cast<char*>(platform::alloc(kFailBufLen));
    return failBuf_;
}

void DriverBase::clearFailBuf() {
    if (failBuf_) {
        if (status() == failBuf_) clearStatus();
        platform::free(failBuf_);
        failBuf_ = nullptr;
    }
}

void DriverBase::setDrivingInfo(unsigned driven, unsigned total, unsigned channels, const char* mode) {
    if (gainsDropped_) { setStatus(kNoGainMemory, Severity::Warning); return; }
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

} // namespace mm
