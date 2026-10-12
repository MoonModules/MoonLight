#pragma once

#include "core/module/MoonModule.h"
#include "platform/platform.h"   // SdPinConfig

#include <cstdint>

namespace mm {

/// A board's microSD slot: pins, a usage gauge and a format button. Its files appear under `/sd`, within the existing File Manager tree.
///
/// A Services child, added only to a board's catalog entry, the way AudioService's mic pins are.
/// @card SdCardService.png
///
/// @moreinfo
///
/// ## The pins are runtime configuration, like AudioService's mic
///
/// The pins default to -1, unset; `d1Pin`-`d3Pin` are optional, and leaving them unset runs the bus 1-bit instead of 4-bit. `bus` picks SPI mode instead, with the SD spec's own pin mapping: `clkPin` is SCK, `cmdPin` MOSI, `d0Pin` MISO and `d3Pin` CS.
///
/// ## A board preset names a soldered wiring, as Ethernet's does
///
/// `board` picks a wiring boards follow, filtered to the ones this chip can wire, and hides the pins it sets; Custom shows them for a hand-wired slot. It persists by label, and a config holding only pins opens on the preset they match.
///
/// A card mounted over SPI carries a warning: on the Dig-Octa, reads hold but writes past about 50 KB stop the card until a power cycle.
///
/// The default is Custom, unless the filter leaves this chip one preset: every P4 board with a slot wires the same pins, so a P4 opens on it. A classic has two, so its catalog entry names one.
///
/// ## The mount re-attempts on every pin change, like the bus it rides
///
/// `prepare()` remounts on a pin edit, as I2cBusModule does for sda/scl. It runs on any `affectsPrepare` change in the tree, so it returns at once while the pins it last tried are unchanged.
///
/// A cheap probe in `tick1s()` notices a pull. An inserted card mounts on a pin edit, a module restart or a boot, since mounting an empty slot stalls the render thread.
///
/// An unmount or a format under an open stream is refused with "busy: transfer in progress", and a card pulled mid-transfer unmounts once the stall timeout has closed that stream.
///
/// ## Pins, gauge and format are this module's whole surface
///
/// The card mounts at `/sd`, so the existing File Manager tree already shows it; this module owns only pins, the gauge and format.
///
/// ## Why KiB, not the shared Progress control's bytes
///
/// A card's byte count overruns the `uint32_t` the shared Progress control carries, so `platform::sdCardUsed`/`Total` are `uint64_t` and this module narrows to KiB before handing the gauge its numbers.
///
/// Prior art: I2cBusModule's live pin reconfiguration; FileManagerModule's usage gauge.
class SdCardService : public MoonModule {
public:
    /// A Services child, same as AudioService: the catalog entry attaches it under "Services".
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }
    /// Declare the pins, the usage gauge and the format button: @xref{pins-gauge-and-format-are-this-modules-whole-surface}.
    void defineControls() override;
    /// A pin edit remounts the card on its new wiring: @xref{the-pins-are-runtime-configuration-like-audioservices-mic}.
    bool affectsPrepare(const char* name) const override;
    /// Mount the card on its pins, once per set of pins: @xref{the-mount-re-attempts-on-every-pin-change-like-the-bus-it-rides}.
    void prepare() override;
    /// Unmount this instance's card, if it mounted one.
    void release() override;
    /// Start a format when the button is pressed.
    void onControlChanged(const char* controlName) override;
    /// Wait out a running format, otherwise poll for a pull and refresh the usage gauge.
    void tick1s() MM_NONBLOCKING override;

    /// The chip a board preset is wired for; Custom is Any, which fits every chip.
    enum class SdPresetChip : uint8_t { Any, Classic, P4 };
    /// What a build can wire, so the filter answers for any chip; a classic is the chip that is none of the others.
    struct SdChip { bool previews, classic, p4; };
    static constexpr SdChip kThisChip{platform::previewsSdPresets, !platform::isEsp32P4 && !platform::isEsp32S3 && !platform::isEsp32S31, platform::isEsp32P4};
    /// Whether a preset for `chip` belongs on `on`: Custom everywhere, and every preset where the platform previews them.
    static constexpr bool presetFits(SdPresetChip chip, SdChip on = kThisChip) {
        return chip == SdPresetChip::Any || on.previews || (chip == SdPresetChip::Classic && on.classic) || (chip == SdPresetChip::P4 && on.p4);
    }
    /// Whether `on` opens on its one fitting preset rather than Custom: @xref{a-board-preset-names-a-soldered-wiring-as-ethernets-does}.
    static constexpr bool opensOnSolePreset(SdChip on = kThisChip) {
        uint8_t fitting = 0;
        for (const SdPreset& p : kSdPresets) fitting += !p.editable && presetFits(p.chip, on);
        return fitting == 1 && !on.previews;
    }

private:
    bool mounted_ = false;            ///< true once platform::sdMount() has succeeded
    bool formatting_ = false;         ///< a format runs in the background, which tick1s() waits out
    bool tried_ = false;              ///< whether prepare() has tried triedPins_ since the last release()
    platform::SdPinConfig triedPins_{};   ///< the pins prepare() last tried, mounted or not, so an unrelated prepare() is a no-op
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
    uint8_t bus_ = 0;      ///< a platform::SdBus: the SD bus, or SPI mode

    /// One board's slot wiring, the fields matching platform::SdPinConfig; -1 leaves a line unused.
    struct SdPreset {
        const char* label;
        SdPresetChip chip;
        platform::SdBus bus;
        int8_t clk, cmd, d0, d1, d2, d3, pwr;
        bool editable;   ///< Custom alone: a preset's lines are soldered, not the user's to set
    };
    /// The wirings boards follow rather than one product each; Custom keeps whatever is in the fields.
    static constexpr SdPreset kSdPresets[] = {
        // The P4's slot 0 IOMUX pins, which every P4 board with a slot wires; GPIO45 drives the card's power switch where the board fits its gate resistor.
        {"P4 SDMMC", SdPresetChip::P4, platform::SdBus::Sdmmc, 43, 44, 39, 40, 41, 42, 45, false},
        // The classic's slot 1 IOMUX pins in 1-bit mode, as the Olimex ESP32-Gateway wires them.
        {"Classic SDMMC (1-bit)", SdPresetChip::Classic, platform::SdBus::Sdmmc, 14, 15, 2, -1, -1, -1, -1, false},
        // SPI mode as the QuinLED Dig-Octa wires it: SCK 14, MOSI 15, MISO 36, CS 16.
        {"Classic SPI", SdPresetChip::Classic, platform::SdBus::Spi, 14, 15, 36, -1, -1, 16, -1, false},
        {"Custom", SdPresetChip::Any, platform::SdBus::Sdmmc, -1, -1, -1, -1, -1, -1, -1, true},
    };
    static constexpr uint8_t kSdPresetCount = sizeof(kSdPresets) / sizeof(kSdPresets[0]);
    static constexpr uint8_t kPresetNone = 0xFF;   ///< never a valid row, so the first build applies whatever the selection resolved to

    uint8_t presetSel_ = 0;                           ///< the selected row of presetOptions_
    const char* presetOptions_[kSdPresetCount] = {};  ///< the labels of the presets this build offers
    uint8_t presetIndex_[kSdPresetCount] = {};        ///< each offered row's index in kSdPresets
    uint8_t presetCount_ = 0;                         ///< how many presets this build offers
    uint8_t presetApplied_ = kPresetNone;             ///< the row whose map the pins were last written from

    /// Offer the presets this build can wire, re-pointing the selection by label and falling back to Custom.
    void buildPresetOptions();
    /// Whether the selection is Custom, whose pins are the user's to edit.
    bool presetIsCustom() const;
    /// Write the selected preset's map into the pins; Custom writes nothing, so an edit survives switching to it.
    void applyPreset();
    /// Select the preset whose map the pins already are, so a config holding a board's pins opens on its name.
    void seedPresetFromPins();
    /// A preset's wiring in the shape the platform mounts.
    static platform::SdPinConfig pinsOf(const SdPreset& p);
    /// Mount on `pins`, or say they are unset; the status names the outcome.
    void mountOn(const platform::SdPinConfig& pins);
    /// The status of a mounted card: clear, or the SPI write warning: @xref{a-board-preset-names-a-soldered-wiring-as-ethernets-does}.
    void showMounted();
    /// Pick up a finished format's result, a no-op while it still runs.
    void finishFormat();
    /// Refresh the gauge fields from the mounted card's real usage.
    void refreshGauge();
    /// The pin fields as platform::sdMount expects them.
    platform::SdPinConfig pinConfig() const;
    /// tick1s()'s actual body, named so its one blocking boundary reads as a single, well-understood site, the same shape refreshGauge() already gave the usage scan.
    void pollCard();
};

} // namespace mm
