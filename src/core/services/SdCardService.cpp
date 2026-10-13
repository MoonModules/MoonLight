/// @defgroup sd_card_impl SD Card implementation
/// The microSD slot's pins, mount and format button; the files it holds are browsed through the File Manager's existing routes, not a surface of this module's own. Public surface and class layout live in SdCardService.h.
/// @{
#include "core/services/SdCardService.h"

#include "platform/platform.h"   // sdMount/sdUnmount/sdFormat/sdCardUsed/sdCardTotal, hasSdCard

#include <cstring>   // strcmp
#include <iterator>  // size

namespace mm {

namespace {
// In platform::SdBus order, since the control stores the index.
constexpr const char* kBusOptions[] = {"SDMMC", "SPI"};
}  // namespace

void SdCardService::defineControls() {
    buildPresetOptions();
    // A selection that MOVED writes its map, since a restore fires no onControlChanged; otherwise an un-chosen Custom is read back off the pins.
    if (presetSel_ != presetApplied_) applyPreset();
    else if (presetIsCustom()) seedPresetFromPins();
    presetApplied_ = presetSel_;
    controls_.addSelect("board", presetSel_, presetOptions_, presetCount_);
    controls_.setPersistLabel(controls_.count() - 1);   // by label: the list is filtered per build, so an index would name a different board
    controls_.setHidden(controls_.count() - 1, presetCount_ == 1);   // Custom alone is no choice
    // Hidden under a preset and still bound, so the values the preset wrote drive the mount and persist.
    const bool custom = presetIsCustom();
    controls_.addSelect("bus", bus_, kBusOptions, static_cast<uint8_t>(std::size(kBusOptions)));
    controls_.setHidden(controls_.count() - 1, !custom);
    const bool spi = bus_ == static_cast<uint8_t>(platform::SdBus::Spi);
    const auto addPinRow = [&](const char* name, int8_t& pin, bool inSpi) {
        controls_.addPin(name, pin);
        controls_.setHidden(controls_.count() - 1, !custom || (spi && !inSpi));
    };
    addPinRow("clkPin", clkPin_, true);
    addPinRow("cmdPin", cmdPin_, true);
    addPinRow("d0Pin", d0Pin_, true);
    addPinRow("d1Pin", d1Pin_, false);   // SPI mode carries data on D0 and D3 alone
    addPinRow("d2Pin", d2Pin_, false);
    addPinRow("d3Pin", d3Pin_, true);
    addPinRow("pwrPin", pwrPin_, true);

    // Neither is hidden: there is no panel of this module's own, so the generic card is the only place either shows. Press-twice confirmed generically, the same contract every other confirm-gated button already has.
    controls_.addButton("format", /*confirm=*/true);
    // Usage gauge, bound only once mounted: an absent/unreadable card has nothing to show.
    if (mounted_ && totalKiB_ > 0) controls_.addProgress("card", usedKiB_, totalKiB_, ProgressUnit::KiB);
    MoonModule::defineControls();
}

bool SdCardService::affectsPrepare(const char* name) const {
    return std::strcmp(name, "board") == 0 || std::strcmp(name, "bus") == 0 || std::strcmp(name, "clkPin") == 0 || std::strcmp(name, "cmdPin") == 0 ||
           std::strcmp(name, "d0Pin") == 0  || std::strcmp(name, "d1Pin") == 0 ||
           std::strcmp(name, "d2Pin") == 0  || std::strcmp(name, "d3Pin") == 0 ||
           std::strcmp(name, "pwrPin") == 0;
}

namespace {
// The one way a card inserted later mounts, without a timed retry on the render thread.
constexpr const char* kRetryHint = "card removed: reinsert it, then switch this module off and on";
constexpr const char* kFormatting = "formatting: the card is back when it finishes";

// What a failed mount says, by the reason the platform recorded, with the one step that fixes it.
const char* mountProblem() {
    switch (platform::sdMountError()) {
        case platform::SdMountError::Pins:         return "mount failed: a pin cannot drive the card, check the pins";
        case platform::SdMountError::Unreadable:   return "the card answers but does not read: try another card";
        case platform::SdMountError::NoFilesystem: return "the card has no FAT filesystem: press format to make one";
        default:                                   return "no card answered: insert one, then switch this module off and on";
    }
}

bool samePins(const platform::SdPinConfig& a, const platform::SdPinConfig& b) {
    return a.clk == b.clk && a.cmd == b.cmd && a.d0 == b.d0 &&
           a.d1 == b.d1 && a.d2 == b.d2 && a.d3 == b.d3 && a.pwr == b.pwr && a.bus == b.bus;
}
}  // namespace

void SdCardService::prepare() {
    // A running format owns the card until it publishes its result, which tick1s picks up.
    if (platform::sdFormatState() == platform::SdFormatState::Running) { formatting_ = true; setStatus(kFormatting, Severity::Status); return; }
    platform::SdPinConfig pins = pinConfig();
    // Most prepares concern another module, so pins already tried are left alone: a remount tears down open streams, and a retry on an empty slot stalls rendering.
    if (tried_ && samePins(pins, triedPins_)) return;

    const bool wasMounted = mounted_;
    if (mounted_) {
        if (!platform::sdUnmount()) { setStatus("busy: transfer in progress", Severity::Warning); return; }
        mounted_ = false;
    }

    tried_ = true;
    triedPins_ = pins;
    mountOn(pins);
    // defineControls() saw the old mounted_ in its gauge gate, so one rebuild adds or drops the gauge now that the mount state moved.
    if (mounted_ != wasMounted) rebuildControls();
}

void SdCardService::mountOn(const platform::SdPinConfig& pins) {
    if (pins.clk < 0 || pins.cmd < 0 || pins.d0 < 0) {
        // Unset is this module's idle state (nothing is wrong yet, nothing was attempted), not a warning.
        setStatus("set the SD card pins first", Severity::Status);
        return;
    }
    mounted_ = platform::sdMount(pins);
    if (mounted_) { refreshGauge(); showMounted(); }
    else setStatus(mountProblem(), Severity::Error);
}

void SdCardService::release() {
    if (mounted_ && platform::sdUnmount()) mounted_ = false;
    tried_ = false;   // switching the module back on tries the slot again, which is how a card inserted later mounts
    MoonModule::release();
}

void SdCardService::onControlChanged(const char* controlName) {
    if (std::strcmp(controlName, "format") != 0 || formatting_) return;
    // A card without a filesystem is the one unmounted card a format applies to: it formats and mounts it.
    const bool unformatted = !mounted_ && platform::sdMountError() == platform::SdMountError::NoFilesystem;
    if (!mounted_ && !unformatted) { setStatus("mount a card before formatting", Severity::Warning); markDirty(); return; }
    if (platform::sdCardBusy()) { setStatus("busy: transfer in progress", Severity::Warning); markDirty(); return; }
    if (!platform::sdFormat()) { setStatus("format failed", Severity::Error); markDirty(); return; }
    formatting_ = true;
    setStatus(kFormatting, Severity::Status);
    if (mounted_) { mounted_ = false; rebuildControls(); }   // the card is unmounted while it formats, so the gauge goes
    finishFormat();   // a backend that formats within the call is done already
    markDirty();
}

void SdCardService::finishFormat() {
    const platform::SdFormatState state = platform::sdFormatState();
    if (state == platform::SdFormatState::Running) return;
    formatting_ = false;
    mounted_ = platform::sdCardMounted();
    if (state == platform::SdFormatState::Succeeded) { refreshGauge(); showMounted(); }
    else setStatus("format failed: switch this module off and on to try the card again", Severity::Error);
    rebuildControls();
    markDirty();
}

void SdCardService::tick1s() MM_NONBLOCKING {
    if (formatting_) finishFormat();
    else pollCard();
}

void SdCardService::pollCard() {
    if (!mounted_) return;
    // A cheap status probe, once a second: cheap enough to poll, unlike a full mount. A card pulled mid-transfer leaves its stream to the existing stall-timeout/I/O-failure cleanup, so sdUnmount's busy guard is retried here rather than bypassed.
    if (!platform::sdCardPresent()) {
        if (!platform::sdUnmount()) return;
        mounted_ = false;
        setStatus(kRetryHint, Severity::Warning);
        rebuildControls();
        return;
    }
    // Once a minute, not once a second: the same reasoning as FileManagerModule's own scan.
    if (++secondsSinceScan_ < 60) return;
    secondsSinceScan_ = 0;
    refreshGauge();
}

void SdCardService::showMounted() {
    if (bus_ == static_cast<uint8_t>(platform::SdBus::Spi)) setStatus("SPI: writes over 50 KB can stop the card until a power cycle", Severity::Warning);
    else clearStatus();
}

void SdCardService::refreshGauge() {
    totalKiB_ = static_cast<uint32_t>(platform::sdCardTotal() / 1024);
    usedKiB_ = static_cast<uint32_t>(platform::sdCardUsed() / 1024);
}

void SdCardService::buildPresetOptions() {
    const char* current = presetSel_ < presetCount_ ? presetOptions_[presetSel_] : nullptr;
    presetCount_ = 0;
    uint8_t sel = 0;
    for (uint8_t i = 0; i < kSdPresetCount; i++) {
        if (!presetFits(kSdPresets[i].chip)) continue;
        // The default: Custom, unless this chip has exactly one real preset, which is then its board.
        const bool isDefault = opensOnSolePreset() ? !kSdPresets[i].editable : kSdPresets[i].editable;
        if (isDefault) sel = presetCount_;
        presetOptions_[presetCount_] = kSdPresets[i].label;
        presetIndex_[presetCount_++] = i;
    }
    for (uint8_t k = 0; current && k < presetCount_; k++)
        if (std::strcmp(presetOptions_[k], current) == 0) sel = k;
    presetSel_ = sel;
}

bool SdCardService::presetIsCustom() const {
    return presetSel_ >= presetCount_ || kSdPresets[presetIndex_[presetSel_]].editable;
}

void SdCardService::applyPreset() {
    if (presetIsCustom()) return;
    const SdPreset& p = kSdPresets[presetIndex_[presetSel_]];
    bus_ = static_cast<uint8_t>(p.bus);
    clkPin_ = p.clk; cmdPin_ = p.cmd; d0Pin_ = p.d0; d1Pin_ = p.d1; d2Pin_ = p.d2; d3Pin_ = p.d3; pwrPin_ = p.pwr;
}

platform::SdPinConfig SdCardService::pinsOf(const SdPreset& p) {
    platform::SdPinConfig pins{};
    pins.clk = p.clk; pins.cmd = p.cmd; pins.d0 = p.d0; pins.d1 = p.d1; pins.d2 = p.d2; pins.d3 = p.d3; pins.pwr = p.pwr;
    pins.bus = p.bus;
    return pins;
}

void SdCardService::seedPresetFromPins() {
    const platform::SdPinConfig pins = pinConfig();
    for (uint8_t k = 0; k < presetCount_; k++) {
        const SdPreset& p = kSdPresets[presetIndex_[k]];
        if (!p.editable && samePins(pinsOf(p), pins)) { presetSel_ = k; return; }
    }
}

platform::SdPinConfig SdCardService::pinConfig() const {
    platform::SdPinConfig pins{};
    pins.clk = clkPin_;
    pins.cmd = cmdPin_;
    pins.d0 = d0Pin_;
    pins.d1 = d1Pin_;
    pins.d2 = d2Pin_;
    pins.d3 = d3Pin_;
    pins.pwr = pwrPin_;
    pins.bus = static_cast<platform::SdBus>(bus_);
    return pins;
}

// mkdir/delete/read/write are the File Manager's existing HTTP endpoints, against paths under /sd; this module holds no op state and persists nothing itself.

} // namespace mm

/// @}
