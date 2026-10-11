#pragma once

#include "core/module/MoonModule.h"

#include <cstdint>

namespace mm {

/// A board's microSD slot: pins, a usage gauge and a format button. Its files appear under `/sd` in the one File Manager tree, not a panel of their own.
///
/// A Services child, added only to a board's catalog entry, the way AudioService's mic pins are.
/// @card SdCardModule.png
///
/// @moreinfo
///
/// ## The pins are runtime configuration, like AudioService's mic
///
/// `clkPin`/`cmdPin`/`d0Pin`-`d3Pin`/`pwrPin` default to -1, unset; a board's catalog entry sets the ones it wires.
/// `platform::hasSdCard` reports whether this chip has SDMMC hardware at all, and hides the pin controls where it does not.
/// `d1Pin`-`d3Pin` are optional: leaving them unset drops the bus to 1-bit mode instead of 4-bit.
///
/// ## The mount re-attempts on every pin change, like the bus it rides
///
/// `prepare()`, not `setup()`: a pin edit unmounts and remounts on the new wiring, the same live-reconfiguration contract I2cBusModule's sda/scl have.
/// A missing or unreadable card shows as its own status rather than retried every tick; reinsert and touch a pin field (or reboot) to retry.
///
/// ## No panel, no view toggle
///
/// The card mounts at `/sd`, so the existing File Manager tree already shows it; this module owns only pins, the gauge and format.
///
/// ## Why KiB, not the shared Progress control's bytes
///
/// A real card's byte count overruns the `uint32_t` the shared Progress control carries past 4 GiB.
/// `platform::sdCardUsed`/`Total` are `uint64_t` for exactly that, and this module narrows to KiB before handing the gauge its numbers, well inside range for any card size in practice.
///
/// Prior art: I2cBusModule's live pin reconfiguration; FileManagerModule's usage gauge.
class SdCardModule : public MoonModule {
public:
    /// A Services child, same as AudioService: the catalog entry attaches it under "Services".
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }
    /// Declare the pins, the usage gauge and the format button.
    void defineControls() override;
    /// A pin edit remounts the card on its new wiring, same as the bus it rides.
    bool affectsPrepare(const char* name) const override;
    /// Mount the card, if this chip has the hardware and the pins are set.
    void prepare() override;
    /// Unmount this instance's card, if it mounted one.
    void release() override;
    /// Run a format when the button is pressed.
    void onControlChanged(const char* controlName) override;
    /// Refresh the usage gauge, which walks the card on a slow cadence.
    void tick1s() MM_NONBLOCKING override;

private:
    bool mounted_ = false;            ///< true once platform::sdMount() has succeeded
    uint32_t usedKiB_ = 0;             ///< card usage, in KiB: @xref{why-kib-not-the-shared-progress-controls-bytes}
    uint32_t totalKiB_ = 0;            ///< the card's capacity, in KiB, read once on mount
    uint8_t  secondsSinceScan_ = 0;    ///< paces the usage scan

    // Unused by default, so a chip with no card wired claims no GPIO; a board's catalog entry sets the ones it wires.
    int8_t clkPin_ = -1;   ///< SDMMC clock
    int8_t cmdPin_ = -1;   ///< SDMMC command line
    int8_t d0Pin_  = -1;   ///< SDMMC data line 0, required
    int8_t d1Pin_  = -1;   ///< SDMMC data line 1; unset alongside d2/d3 drops the bus to 1-bit mode
    int8_t d2Pin_  = -1;   ///< SDMMC data line 2
    int8_t d3Pin_  = -1;   ///< SDMMC data line 3
    int8_t pwrPin_ = -1;   ///< a slot's card-power switch, or -1 where the slot has none

    /// Refresh the gauge fields from the mounted card's real usage.
    void refreshGauge();
};

} // namespace mm
