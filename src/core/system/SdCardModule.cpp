/// @defgroup sd_card_impl SD Card implementation
/// The microSD slot's pins, mount and format button; the files it holds are browsed through the File Manager's existing routes, not a surface of this module's own.
/// Public surface and class layout live in SdCardModule.h.
/// @{
#include "core/system/SdCardModule.h"

#include "platform/platform.h"   // sdMount/sdUnmount/sdFormat/sdCardUsed/sdCardTotal, hasSdCard

#include <cstring>   // strcmp

namespace mm {

void SdCardModule::defineControls() {
    // Unhidden: a board without hasSdCard never boots this module at all (module_types.cpp gates on it), so no board is left for a hidden pin to guard against.
    controls_.addPin("clkPin", clkPin_);
    controls_.addPin("cmdPin", cmdPin_);
    controls_.addPin("d0Pin", d0Pin_);
    controls_.addPin("d1Pin", d1Pin_);
    controls_.addPin("d2Pin", d2Pin_);
    controls_.addPin("d3Pin", d3Pin_);
    controls_.addPin("pwrPin", pwrPin_);

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
    platform::SdPinConfig pins = pinConfig();
    // prepare() runs on any affectsPrepare change anywhere in the tree, so most calls are about some other module entirely; skip the unmount/remount round trip when this instance's own wiring has not actually changed, rather than tearing down a mount (and any stream open on it) for nothing.
    if (mounted_ && samePins(pins, mountedPins_)) return;

    const bool wasMounted = mounted_;
    if (mounted_) {
        if (!platform::sdUnmount()) { setStatus("busy: transfer in progress", Severity::Warning); return; }
        mounted_ = false;
    }

    if (clkPin_ < 0 || cmdPin_ < 0 || d0Pin_ < 0) {
        // Unset is this module's idle state (nothing is wrong yet, nothing was attempted), not a warning.
        setStatus("set the SD card pins first", Severity::Status);
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
    pollCard();
}

void SdCardModule::pollCard() {
    if (mounted_) {
        // A cheap status probe, once a second: cheap enough to poll, unlike a full mount. A card pulled mid-transfer leaves its stream to the existing stall-timeout/I/O-failure cleanup, so sdUnmount's busy guard is retried here rather than bypassed.
        if (!platform::sdCardPresent()) {
            if (!platform::sdUnmount()) return;
            mounted_ = false;
            setStatus("card removed", Severity::Warning);
            rebuildControls();
            return;
        }
        // Once a minute, not once a second: the same reasoning as FileManagerModule's own scan.
        if (++secondsSinceScan_ < 60) return;
        secondsSinceScan_ = 0;
        refreshGauge();
        return;
    }

    // Unmounted with pins set: retry periodically, so a reinsert is noticed without a pin touch or reboot. Slower than the presence poll above, since a retry drives real GPIO/power sequencing rather than a status probe.
    if (clkPin_ < 0 || cmdPin_ < 0 || d0Pin_ < 0) return;
    if (++secondsSinceRetry_ < kMountRetrySeconds) return;
    secondsSinceRetry_ = 0;
    platform::SdPinConfig pins = pinConfig();
    if (!platform::sdMount(pins)) return;
    mounted_ = true;
    mountedPins_ = pins;
    refreshGauge();
    clearStatus();
    rebuildControls();
}

void SdCardModule::refreshGauge() {
    totalKiB_ = static_cast<uint32_t>(platform::sdCardTotal() / 1024);
    usedKiB_ = static_cast<uint32_t>(platform::sdCardUsed() / 1024);
}

platform::SdPinConfig SdCardModule::pinConfig() const {
    platform::SdPinConfig pins{};
    pins.clk = clkPin_;
    pins.cmd = cmdPin_;
    pins.d0 = d0Pin_;
    pins.d1 = d1Pin_;
    pins.d2 = d2Pin_;
    pins.d3 = d3Pin_;
    pins.pwr = pwrPin_;
    return pins;
}

// mkdir/delete/read/write are the File Manager's existing HTTP endpoints, against paths under /sd; this module holds no op state and persists nothing itself.

} // namespace mm

/// @}
