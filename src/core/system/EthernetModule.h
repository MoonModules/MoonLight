#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/module/MoonModule.h"
#include "core/system/IpSettings.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace mm {

/// The wired interface: which board's wiring it uses, its pins, and how it gets an address.
///
/// A Network child: the network module runs the cascade, and this one holds what Ethernet needs to join it.
/// @card EthernetModule.png
///
/// @moreinfo
///
/// ## Why one preset defaults to itself
///
/// The preset list is filtered per chip, and the default is Custom rather than row 0.
/// Where the filter leaves exactly ONE real preset that reasoning inverts, because the single survivor IS this chip's board.
/// A P4 offers P4-NANO and an S31 offers S31 CoreBoard, so Custom there ships an unconfigured interface on a board whose wiring is known.
/// A classic keeps Custom, where several survive and only the catalog knows the board: an Olimex takes Classic RMII, a QuinLED Dig-Octa the no-reset variant.
///
/// The applied-tracker starts on a sentinel rather than row 0, or a virgin P4 would select P4-NANO and still boot with the interface at ethNone.
class EthernetModule : public MoonModule {
public:
    /// Keep the interface configured whatever the toggle says, or the device drops off the network.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    /// The interface comes up once at boot, so a restored config applies at the next boot.
    bool appliesConfigLive() const override { return false; }

    /// The controller's data pads while Ethernet runs, which the chip fixed and nobody sets.
    uint8_t fixedPins(FixedPin* out, uint8_t max) const override {
        // The applied type rather than the pending control.
        if (!out || appliedEthType_ == static_cast<uint8_t>(platform::ethNone)) return 0;
        uint8_t n = 0;
        for (uint8_t i = 0; i < platform::ethFixedPadCount && n < max; i++)
            out[n++] = FixedPin{platform::ethFixedPads[i].gpio, platform::ethFixedPads[i].name};
        return n;
    }

    /// Declare the board, the PHY and its pins, then the IP settings.
    void defineControls() override {
        MoonModule::defineControls();
        // Where a driver is compiled in, the type selecting which pin rows apply; a desktop builds the same rows with no interface behind them, to exercise the presets.
        if constexpr (platform::hasEthernet || platform::previewsEthernetControls) {
            // A preview configures nothing, so it is developer-mode only and says so on the card.
            constexpr bool preview = !platform::hasEthernet;
            const uint8_t firstEthControl = controls_.count();
            buildEthPresetOptions();
            // A selection MOVED, so write its map. The restore path fires no onControlChanged, and this is what makes a saved `ethBoard` reach the pins; keyed on the move, or a rebuild would undo a Custom edit.
            if (ethPresetSel_ != ethPresetApplied_) applyEthPreset();
            // Otherwise read the preset back off the pins, while the selection is still the un-chosen Custom: restore overlays pins before `ethBoard` survives a rebuild, so seeding once read defaults instead.
            else if (ethPresetIsUnset()) seedEthPresetFromPins();
            ethPresetApplied_ = ethPresetSel_;
            controls_.addSelect("ethBoard", ethPresetSel_, ethPresetOptions_, ethPresetCount_);
            // By label: the list is filtered per build, so an index would name a different board.
            controls_.setPersistLabel(controls_.count() - 1);
            // Hidden on a known board, and hidden stays BOUND: the values still drive the interface.
            const bool editable = ethPinsEditable();
            controls_.addSelect("ethType", ethType_, ethTypeOptions_, 5);
            controls_.setHidden(controls_.count() - 1, !editable);
            const bool isRmii  = (ethType_ == 1 || ethType_ == 2);
            const bool isSpi   = (ethType_ == 3);
            const bool isRgmii = (ethType_ == 4);
            const bool isEth   = isRmii || isSpi || isRgmii;
            // An address rather than a GPIO, and signed for the auto-detect sentinel.
            controls_.addControl("ethPhyAddr", ethPhyAddr_, -1, 31);
            controls_.setNumberField(controls_.count() - 1);   // an identity, not a magnitude
            controls_.setHidden(controls_.count() - 1, !editable || !isEth);
            controls_.addPin("ethRstGpio", ethRstGpio_);
            controls_.setHidden(controls_.count() - 1, !editable || !isEth);
            // Every wired interface needs them, and showing them is what the pin map counts.
            controls_.addPin("ethMdcGpio", ethMdcGpio_);
            controls_.setHidden(controls_.count() - 1, !editable || (!isRmii && !isRgmii));
            controls_.addPin("ethMdioGpio", ethMdioGpio_);
            controls_.setHidden(controls_.count() - 1, !editable || (!isRmii && !isRgmii));
            controls_.addPin("ethClockGpio", ethClockGpio_);
            controls_.setHidden(controls_.count() - 1, !editable || !isRmii);
            // A direction, so a toggle rather than a range.
            controls_.addControl("ethClockExtIn", ethClockExtIn_);
            controls_.setHidden(controls_.count() - 1, !editable || !isRmii);
            controls_.addPin("ethSpiMiso", ethSpiMiso_);
            controls_.setHidden(controls_.count() - 1, !editable || !isSpi);
            controls_.addPin("ethSpiMosi", ethSpiMosi_);
            controls_.setHidden(controls_.count() - 1, !editable || !isSpi);
            controls_.addPin("ethSpiSck", ethSpiSck_);
            controls_.setHidden(controls_.count() - 1, !editable || !isSpi);
            controls_.addPin("ethSpiCs", ethSpiCs_);
            controls_.setHidden(controls_.count() - 1, !editable || !isSpi);
            controls_.addPin("ethSpiIrq", ethSpiIrq_);
            controls_.setHidden(controls_.count() - 1, !editable || !isSpi);
            // Immediately before the fields it conditions, so the two stay adjacent.
            const uint8_t firstIpControl = controls_.count();
            controls_.addSelect("ipSettings", ipSettings_, ipsettings::kOptions, 2);
            // Always bound so persistence can load them, with only their visibility conditional.
            const bool hideStatic = !isStatic();
            controls_.addIPv4(ipsettings::kFields[0], staticIp_);
            controls_.setHidden(controls_.count() - 1, hideStatic);
            controls_.addIPv4(ipsettings::kFields[1], staticGateway_);
            controls_.setHidden(controls_.count() - 1, hideStatic);
            controls_.addIPv4(ipsettings::kFields[2], staticSubnet_);
            controls_.setHidden(controls_.count() - 1, hideStatic);
            controls_.addIPv4(ipsettings::kFields[3], staticDns_);
            controls_.setHidden(controls_.count() - 1, hideStatic);
            // One loop rather than a call beside every row: the wiring carries one tag, while the IP settings stay in view.
            if constexpr (preview) {
                for (uint8_t i = firstEthControl; i < firstIpControl; i++) controls_.setDeveloper(i);
            }
        }
    }

    // What the network module's cascade asks of the wired interface.

    /// Push the interface config to the platform, before bring-up reads it, and baseline the IP settings.
    void syncConfig() {
        if constexpr (platform::hasEthernet) {
            platform::EthPinConfig cfg{};
            // Where the platform fixes the interface, its own type wins.
            cfg.phyType        = platform::ethPhyIsFixed
                               ? static_cast<uint8_t>(platform::ethConfigDefault.phyType) : ethType_;
            cfg.phyAddr        = ethPhyAddr_;
            cfg.mdcGpio        = ethMdcGpio_;
            cfg.mdioGpio       = ethMdioGpio_;
            cfg.rstGpio        = ethRstGpio_;
            cfg.rmiiClockGpio  = ethClockGpio_;
            cfg.rmiiClockExtIn = ethClockExtIn_;
            cfg.spiMiso        = ethSpiMiso_;
            cfg.spiMosi        = ethSpiMosi_;
            cfg.spiSck         = ethSpiSck_;
            cfg.spiCs          = ethSpiCs_;
            cfg.spiIrq         = ethSpiIrq_;
            platform::setEthConfig(cfg);
            appliedEthSig_ = ethSig();
            appliedEthType_ = ethType_;   // what holds the pads now
            ethSigApplied_ = true;
        }
        appliedIpSig_ = ipSig();
        ipSigApplied_ = true;
    }

    /// Apply an interface change live where the hardware allows it, returning true when the interface restarted.
    bool syncLive() {
        if constexpr (platform::hasEthernet) {
            if (ethSigApplied_ && ethSig() == appliedEthSig_) return false;   // nothing changed
            // Only for the SPI device, since elsewhere a restart would strand the device.
            const bool hotReinit = (ethType_ == 3) && platform::hasEthW5500;
            if (hotReinit) {
                platform::ethStop();
                syncConfig();
                if (platform::ethInit()) {
                    std::printf("EthernetModule: W5500 re-init (live config change)\n");
                    setStatus("");
                    return true;
                }
                setStatus("W5500 re-init failed, check pins", Severity::Error);
            } else {
                // Record it for the next boot without disturbing the running interface.
                syncConfig();
                setStatus("saved, restart to apply");
            }
        }
        return false;
    }

    /// Apply an IP settings change live when Ethernet carries the device, returning true when one was applied.
    bool syncIpLive(bool connected) {
        const uint32_t sig = ipSig();
        if (ipSigApplied_ && sig == appliedIpSig_) return false;   // nothing changed
        appliedIpSig_ = sig;
        ipSigApplied_ = true;
        if (!connected) return false;   // applied on the next connect
        if (isStatic()) applyStatic();
        else platform::netSetDhcp(platform::NetIface::Eth);   // Static → DHCP: re-lease live
        return true;
    }

    /// Whether the user pinned a static address.
    bool isStatic() const MM_NONBLOCKING { return ipSettings_ == ipsettings::kStatic; }

    /// The address the user set, or null where a DHCP client runs.
    const uint8_t* configuredIp() const MM_NONBLOCKING { return isStatic() ? staticIp_ : nullptr; }

    /// Whether a wired interface is set up, which is what lets the access point stay closed.
    bool configured() const MM_NONBLOCKING {
        if constexpr (!platform::hasEthernet) return false;
        const uint8_t type = platform::ethPhyIsFixed ? static_cast<uint8_t>(platform::ethConfigDefault.phyType) : ethType_;
        return type != static_cast<uint8_t>(platform::ethNone);
    }
    /// Pin the configured address onto the wired interface, or do nothing where a client runs.
    void applyStatic() {
        if (!isStatic()) return;   // leave the client running
        platform::netSetStaticIPv4(platform::NetIface::Eth, staticIp_, staticGateway_, staticSubnet_, staticDns_);
    }

private:
    uint8_t ipSettings_ = ipsettings::kDhcp;   ///< DHCP or Static, the words routers and phones use
    // Octets rather than strings, always bound, with only their visibility conditional.
    uint8_t staticIp_[4]      = {0, 0, 0, 0};
    uint8_t staticGateway_[4] = {0, 0, 0, 0};
    uint8_t staticSubnet_[4]  = {255, 255, 255, 0};
    uint8_t staticDns_[4]     = {0, 0, 0, 0};
    // The IP settings last applied, with a flag for "never" since any value is a valid hash.
    uint32_t appliedIpSig_ = 0;
    bool ipSigApplied_ = false;

    /// A hash over the mode and the static octets, so an edit to any of them re-applies.
    uint32_t ipSig() const {
        return ipsettings::sig(ipSettings_, staticIp_, staticGateway_, staticSubnet_, staticDns_);
    }

    // The order must match the platform's own enum, since the control stores an index.
    static constexpr const char* ethTypeOptions_[] = {"None", "LAN8720", "IP101", "W5500", "YT8531"};

    // Seeded per chip and overridden by the catalog, the type defaulting to none.
    uint8_t ethType_       = static_cast<uint8_t>(platform::ethNone);
    // An address rather than a GPIO, and signed so the auto-detect sentinel round-trips.
    int16_t ethPhyAddr_    = static_cast<int16_t>(platform::ethConfigDefault.phyAddr);
    int8_t  ethMdcGpio_    = static_cast<int8_t>(platform::ethConfigDefault.mdcGpio);
    int8_t  ethMdioGpio_   = static_cast<int8_t>(platform::ethConfigDefault.mdioGpio);
    int8_t  ethRstGpio_    = static_cast<int8_t>(platform::ethConfigDefault.rstGpio);
    // Mirrored so they can be published: the controls are the registry the pin map reads.
    int8_t  ethClockGpio_  = static_cast<int8_t>(platform::ethConfigDefault.rmiiClockGpio);
    bool    ethClockExtIn_ = platform::ethConfigDefault.rmiiClockExtIn;
    int8_t  ethSpiMiso_    = static_cast<int8_t>(platform::ethConfigDefault.spiMiso);
    int8_t  ethSpiMosi_    = static_cast<int8_t>(platform::ethConfigDefault.spiMosi);
    int8_t  ethSpiSck_     = static_cast<int8_t>(platform::ethConfigDefault.spiSck);
    int8_t  ethSpiCs_      = static_cast<int8_t>(platform::ethConfigDefault.spiCs);
    int8_t  ethSpiIrq_     = static_cast<int8_t>(platform::ethConfigDefault.spiIrq);
    // The signature last applied, with a flag for "never" since any value is a valid hash.
    uint32_t appliedEthSig_ = 0;
    bool ethSigApplied_ = false;
    // What the driver is running, which the fixed-pin report must read.
    uint8_t appliedEthType_ = static_cast<uint8_t>(platform::ethNone);

    // One board's Ethernet wiring, the fields matching EthPinConfig; -1 leaves a line unused.
    struct EthPreset {
        const char* label;
        int8_t type;          // an EthPhyType
        int8_t phyAddr;
        int8_t mdc, mdio, rst;
        int8_t rmiiClock;
        bool   rmiiClockExtIn;
        int8_t miso, mosi, sck, cs, irq;
        bool   editable;      // false on a soldered map: those lines are not the user's to set
    };

    // The presets this module knows, each a board family rather than one product; Custom keeps whatever is in the fields, for a hand-wired board.
    static constexpr EthPreset kEthPresets[] = {
        // The LAN8720 reference wiring most classic boards follow, which is also the chip default.
        {"Classic RMII",            1,  0, 23, 18,  5, 17, false, -1, -1, -1, -1, -1, false},
        // The same wiring with no reset, for a board using GPIO 5 as an LED lane: the PHY resets by jumper.
        {"Classic RMII (no reset)", 1,  0, 23, 18, -1, 17, false, -1, -1, -1, -1, -1, false},
        // Waveshare P4-NANO and the boards following its shield pinout: IP101, the clock fed in.
        {"P4-NANO",           2,  1, 31, 52, 51, 50, true,  -1, -1, -1, -1, -1, false},
        // The S31's 1 Gb PHY, addressed by scan rather than by a fixed address.
        {"S31 CoreBoard",     4, -1,  5,  6,  7, -1, false, -1, -1, -1, -1, -1, false},
        {"Custom",            0, -1, -1, -1, -1, -1, false, -1, -1, -1, -1, -1, true},
    };
    static constexpr uint8_t kEthPresetCount = sizeof(kEthPresets) / sizeof(kEthPresets[0]);

    // Which board wiring is selected, the pins following from it unless it is Custom.
    uint8_t ethPresetSel_ = 0;
    const char* ethPresetOptions_[kEthPresetCount] = {};
    uint8_t ethPresetIndex_[kEthPresetCount] = {};
    uint8_t ethPresetCount_ = 0;
    // Never a valid row, so the first build applies whatever the selection resolved to: @xref{why-one-preset-defaults-to-itself}.
    static constexpr uint8_t kEthPresetNone = 0xFF;
    uint8_t ethPresetApplied_ = kEthPresetNone;

    // A preset naming a PHY this build cannot drive would offer pins that reach nothing.
    /// Does this firmware carry a driver for the preset's PHY?
    static bool presetBuildable(const EthPreset& p) {
        if (p.type == 0) return true;                       // Custom, which names no PHY
        if (p.type == 3) return platform::hasEthW5500;      // SPI, a separate driver
        if (!platform::hasEthW5500) {
            // A preset carries a PIN MAP as well as a PHY, so one the chip cannot wire persists pins reaching nothing; the desktop previews all.
            constexpr bool knownChip = platform::isEsp32P4 || platform::isEsp32S31;
            if (p.type == 2) return !knownChip || platform::isEsp32P4;    // IP101: the P4's
            if (p.type == 4) return !knownChip || platform::isEsp32S31;   // YT8531 RGMII: the S31's
            return !platform::isEsp32P4 && !platform::isEsp32S31;         // LAN8720: classic RMII
        }
        return false;
    }

    /// Offer the presets this build can drive, re-pointing the selection by label.
    void buildEthPresetOptions() {
        const char* current = (ethPresetSel_ < ethPresetCount_) ? ethPresetOptions_[ethPresetSel_] : nullptr;
        ethPresetCount_ = 0;
        for (uint8_t i = 0; i < kEthPresetCount; i++) {
            if (!presetBuildable(kEthPresets[i])) continue;
            ethPresetOptions_[ethPresetCount_] = kEthPresets[i].label;
            ethPresetIndex_[ethPresetCount_] = i;
            ethPresetCount_++;
        }
        // By LABEL, so a filtered list cannot select a different board, falling back to CUSTOM rather than row 0: a real preset whose map would overwrite the chip's defaults.
        uint8_t sel = 0;
        for (uint8_t k = 0; k < ethPresetCount_; k++) {
            if (kEthPresets[ethPresetIndex_[k]].editable) { sel = k; break; }
        }
        // Unless the chip's filter left exactly ONE real preset: @xref{why-one-preset-defaults-to-itself}.
        uint8_t real = 0, onlyReal = 0;
        for (uint8_t k = 0; k < ethPresetCount_; k++) {
            if (!kEthPresets[ethPresetIndex_[k]].editable) { real++; onlyReal = k; }
        }
        if (real == 1) sel = onlyReal;
        if (current) {
            for (uint8_t k = 0; k < ethPresetCount_; k++) {
                if (std::strcmp(ethPresetOptions_[k], current) == 0) { sel = k; break; }
            }
        }
        ethPresetSel_ = sel;
    }

    /// Has nothing chosen a preset yet? True while the selection is the editable Custom row.
    bool ethPresetIsUnset() const {
        if (ethPresetSel_ >= ethPresetCount_) return true;
        return kEthPresets[ethPresetIndex_[ethPresetSel_]].editable;
    }

    /// Are the pin controls the user's to edit, for the preset currently selected?
    bool ethPinsEditable() const {
        if (ethPresetSel_ >= ethPresetCount_) return true;   // nothing resolved: never hide
        return kEthPresets[ethPresetIndex_[ethPresetSel_]].editable;
    }

    // Custom writes nothing, so switching to it after an edit keeps the edit.
    /// Write the chosen preset's map into the pin controls.
    void applyEthPreset() {
        if (ethPresetSel_ >= ethPresetCount_) return;
        const EthPreset& p = kEthPresets[ethPresetIndex_[ethPresetSel_]];
        if (p.editable) return;
        ethType_       = static_cast<uint8_t>(p.type);
        ethPhyAddr_    = p.phyAddr;
        ethMdcGpio_    = p.mdc;
        ethMdioGpio_   = p.mdio;
        ethRstGpio_    = p.rst;
        ethClockGpio_  = p.rmiiClock;
        ethClockExtIn_ = p.rmiiClockExtIn;
        ethSpiMiso_    = p.miso;
        ethSpiMosi_    = p.mosi;
        ethSpiSck_     = p.sck;
        ethSpiCs_      = p.cs;
        ethSpiIrq_     = p.irq;
    }

    // The board a set of pins came from, so a provisioned device opens on its own name.
    /// The preset whose map these pin values already are, or Custom when none matches.
    void seedEthPresetFromPins() {
        for (uint8_t k = 0; k < ethPresetCount_; k++) {
            const EthPreset& p = kEthPresets[ethPresetIndex_[k]];
            if (p.editable) continue;
            if (p.type == static_cast<int8_t>(ethType_) && p.phyAddr == ethPhyAddr_ &&
                p.mdc == ethMdcGpio_ && p.mdio == ethMdioGpio_ && p.rst == ethRstGpio_ &&
                p.rmiiClock == ethClockGpio_ && p.rmiiClockExtIn == ethClockExtIn_ &&
                p.miso == ethSpiMiso_ && p.mosi == ethSpiMosi_ && p.sck == ethSpiSck_ &&
                p.cs == ethSpiCs_ && p.irq == ethSpiIrq_) {
                ethPresetSel_ = k;
                return;
            }
        }
        // Custom is the last row, and the only editable one.
        for (uint8_t k = 0; k < ethPresetCount_; k++) {
            if (kEthPresets[ethPresetIndex_[k]].editable) { ethPresetSel_ = k; return; }
        }
    }

    /// A cheap hash over the interface controls, so a live change is detected.
    uint32_t ethSig() const {
        uint32_t h = ethType_;
        for (int16_t v : {ethRstGpio_, ethMdcGpio_, ethMdioGpio_,
                          ethClockGpio_, ethSpiMiso_, ethSpiMosi_,
                          ethSpiSck_, ethSpiCs_, ethSpiIrq_}) {
            h = h * 131u + static_cast<uint32_t>(v);
        }
        h = h * 131u + static_cast<uint32_t>(ethPhyAddr_ & 0xFF);   // folded in separately
        h = h * 131u + (ethClockExtIn_ ? 1u : 0u);                  // and likewise
        return h;
    }
};

} // namespace mm
