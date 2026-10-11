/// @defgroup sd_card_impl SD Card implementation
/// The microSD slot's pins, mount and format button; the files it holds are browsed through the File Manager's existing routes, not a surface of this module's own.
/// Public surface and class layout live in SdCardModule.h.
/// @{
#include "core/system/SdCardModule.h"

#include "platform/platform.h"   // sdMount/sdUnmount/sdFormat/sdCardUsed/sdCardTotal, hasSdCard

#include <cstring>   // strcmp

namespace mm {

void SdCardModule::defineControls() {
    // Hidden on a chip with no SDMMC hardware, the same gate AudioService uses for its own mic pins; hidden stays bound, so a value survives a board change.
    controls_.addPin("clkPin", clkPin_);
    controls_.setHidden(controls_.count() - 1, !platform::hasSdCard);
    controls_.addPin("cmdPin", cmdPin_);
    controls_.setHidden(controls_.count() - 1, !platform::hasSdCard);
    controls_.addPin("d0Pin", d0Pin_);
    controls_.setHidden(controls_.count() - 1, !platform::hasSdCard);
    controls_.addPin("d1Pin", d1Pin_);
    controls_.setHidden(controls_.count() - 1, !platform::hasSdCard);
    controls_.addPin("d2Pin", d2Pin_);
    controls_.setHidden(controls_.count() - 1, !platform::hasSdCard);
    controls_.addPin("d3Pin", d3Pin_);
    controls_.setHidden(controls_.count() - 1, !platform::hasSdCard);
    controls_.addPin("pwrPin", pwrPin_);
    controls_.setHidden(controls_.count() - 1, !platform::hasSdCard);

    // Neither is hidden: there is no panel of this module's own, so the generic card is the only place either shows. Press-twice confirmed generically, the same contract every other confirm-gated button already has.
    controls_.addButton("format", /*confirm=*/true);
    // Usage gauge, bound only once mounted: an absent/unreadable card has nothing to show; KiB is in the name since the shared Progress control's own label assumes bytes.
    if (mounted_ && totalKiB_ > 0) controls_.addProgress("card (KiB)", usedKiB_, totalKiB_, /*bytes=*/false);
    MoonModule::defineControls();
}

bool SdCardModule::affectsPrepare(const char* name) const {
    return std::strcmp(name, "clkPin") == 0 || std::strcmp(name, "cmdPin") == 0 ||
           std::strcmp(name, "d0Pin") == 0  || std::strcmp(name, "d1Pin") == 0 ||
           std::strcmp(name, "d2Pin") == 0  || std::strcmp(name, "d3Pin") == 0 ||
           std::strcmp(name, "pwrPin") == 0;
}

namespace {
bool samePins(const platform::SdPinConfig& a, const platform::SdPinConfig& b) {
    return a.clk == b.clk && a.cmd == b.cmd && a.d0 == b.d0 &&
           a.d1 == b.d1 && a.d2 == b.d2 && a.d3 == b.d3 && a.pwr == b.pwr;
}
}  // namespace

void SdCardModule::prepare() {
    platform::SdPinConfig pins{};
    pins.clk = clkPin_;
    pins.cmd = cmdPin_;
    pins.d0 = d0Pin_;
    pins.d1 = d1Pin_;
    pins.d2 = d2Pin_;
    pins.d3 = d3Pin_;
    pins.pwr = pwrPin_;
    // prepare() runs on any affectsPrepare change anywhere in the tree, so most calls are about some other module entirely; skip the unmount/remount round trip when this instance's own wiring has not actually changed, rather than tearing down a mount (and any stream open on it) for nothing.
    if (mounted_ && samePins(pins, mountedPins_)) return;

    const bool wasMounted = mounted_;
    if (mounted_) {
        if (!platform::sdUnmount()) { setStatus("busy: transfer in progress", Severity::Warning); return; }
        mounted_ = false;
    }

    if constexpr (!platform::hasSdCard) {
        setStatus("no SD hardware on this build", Severity::Status);
    } else if (clkPin_ < 0 || cmdPin_ < 0 || d0Pin_ < 0) {
        setStatus("set the SD card pins first", Severity::Warning);
    } else {
        mounted_ = platform::sdMount(pins);
        if (mounted_) { mountedPins_ = pins; refreshGauge(); clearStatus(); }
        else setStatus("mount failed: check the card is inserted and formatted FAT32", Severity::Error);
    }
    // defineControls() ran with the OLD mounted_, so its "mounted_ && totalKiB_ > 0" gate above saw a stale value either way the state just moved; one rebuild picks up or drops the gauge.
    if (mounted_ != wasMounted) rebuildControls();
}

void SdCardModule::release() {
    if (mounted_ && platform::sdUnmount()) mounted_ = false;
    MoonModule::release();
}

void SdCardModule::onControlChanged(const char* controlName) {
    if (std::strcmp(controlName, "format") != 0) return;
    if (!mounted_) { setStatus("mount a card before formatting", Severity::Warning); markDirty(); return; }
    if (platform::sdCardBusy()) { setStatus("busy: transfer in progress", Severity::Warning); markDirty(); return; }
    if (!platform::sdFormat()) { setStatus("format failed", Severity::Error); markDirty(); return; }
    refreshGauge();
    clearStatus();
    markDirty();   // push the fresh usage figures to the UI
}

void SdCardModule::tick1s() MM_NONBLOCKING {
    // Once a minute, not once a second: the same reasoning as FileManagerModule's own scan.
    if (!mounted_) return;
    if (++secondsSinceScan_ < 60) return;
    secondsSinceScan_ = 0;
    refreshGauge();
}

void SdCardModule::refreshGauge() {
    totalKiB_ = static_cast<uint32_t>(platform::sdCardTotal() / 1024);
    usedKiB_ = static_cast<uint32_t>(platform::sdCardUsed() / 1024);
}

// mkdir/delete/read/write are the File Manager's existing HTTP endpoints, against paths under /sd; this module holds no op state and persists nothing itself.

} // namespace mm

/// @}
