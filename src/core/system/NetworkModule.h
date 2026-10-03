#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"
#include "core/system/SystemModule.h"
#include "core/system/FilesystemModule.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

/// All device connectivity, cascading from Ethernet through WiFi to an access point.
///
/// One module and one card: the user sees a network, not three technologies.
/// A desktop uses the operating system's own networking and loads none of this.
/// @card NetworkModule.png
///
/// @moreinfo
///
/// ## The cascade
///
/// Ethernet is preferred, then WiFi, then our own access point as a last resort.
/// A higher-priority link tears the lower ones down to reclaim memory.
/// Each is tried unconditionally, the platform failing fast where hardware is absent.
/// A state machine drives this from the slow tick, and a late interface is promoted live.
///
/// ## Why one preset defaults to itself
///
/// The preset list is filtered per chip, and the default is Custom rather than row 0.
/// A real preset writes a pin map, which on the wrong board overwrites the chip's own defaults.
/// Where the filter leaves exactly ONE real preset that reasoning inverts, because the single survivor IS this chip's board.
/// A P4 offers P4-NANO and an S31 offers S31 CoreBoard, so Custom there ships an unconfigured interface on a board whose wiring is known.
/// A classic keeps Custom, where several survive and only the catalog knows the board: an Olimex takes Classic RMII, a QuinLED Dig-Octa the no-reset variant.
///
/// The applied-tracker starts on a sentinel rather than row 0 for the same reason.
/// Zero would claim row 0 is already applied, and on a single-preset chip that row is now the board's own preset.
/// Its map then never reached the pin fields, so a virgin P4 selected P4-NANO and still booted with the interface at ethNone.
///
/// ## Configuration
///
/// Which driver is compiled in is per chip; which interface a board uses is runtime config.
/// An SPI change applies live, while the built-in controller applies at the next boot.
/// The addressing selector pins a static address or runs the client, live either way.
///
/// The device name belongs to the system module, and is the one identity behind every name.
/// It registers before the light pipeline, which then sees the real remaining heap.
class NetworkModule : public MoonModule {
public:
    /// Adopt the scheduler, which the tree rebuild after a mode change goes through.
    void setScheduler(Scheduler* s) { scheduler_ = s; }
    /// Adopt the system module, which owns the device name this one reads.
    void setSystemModule(SystemModule* s) { systemModule_ = s; }

    /// Persist and apply the power cap, before credentials arriving with it are used.
    void setTxPowerSetting(uint8_t dBm) {
        if (dBm > 21) return;
        txPowerSetting_ = dBm;
        markDirty();
        FilesystemModule::noteDirty();   // same persist arming as setWifiCredentials
        syncTxPower();                   // now if the radio is up, else on the next start
    }

    /// Set the credentials and drive a clean transition into connecting.
    void setWifiCredentials(const char* ssid, const char* password) {
        if (!ssid) return;
        // snprintf rather than a copy needing a second line to terminate, the classic bug.
        mm::formatTo(ssid_, sizeof(ssid_), "%s", ssid);
        mm::formatTo(password_, sizeof(password_), "%s", password ? password : "");
        markDirty();
        // Marking alone only sets the bit, so the save needs arming too.
        FilesystemModule::noteDirty();
        if constexpr (platform::hasWiFi) {
            // Tear down first: the platform would otherwise skip registering its handler.
            if (state_ == State::AP) {
                platform::wifiApStop();
                noteRadioStopped();
                apShutdownPending_ = false;
            }
            if (state_ == State::WaitingSta || state_ == State::ConnectedSta) {
                platform::wifiStaStop();
                noteRadioStopped();
            }
            if (platform::wifiStaInit(ssid_, password_)) {
                state_ = State::WaitingSta;
                stateChangeTime_ = platform::millis();
                // Before the radio's first burst, which is the window the cap protects.
                syncTxPower();
                std::snprintf(statusBuf_, sizeof(statusBuf_), "WiFi STA: %s", ssid_);
                setStatus(statusBuf_, Severity::Status);
                // Re-evaluate visibility, or a now-stale signal reading would stay rendered.
                rebuildControls();
            } else {
                // Recover through the access point, so credentials can be re-entered.
                startAP();
            }
        }
    }

    /// Keep the cascade ticking whatever the toggle says, or the device drops off the network.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    /// Bring-up runs once and is not re-entrant, so a restored config applies at the next boot.
    bool appliesConfigLive() const override { return false; }

    /// Set the hostname, push the interface config, then start the cascade.
    void setup() override {
        // Before any bring-up, so a router's client list shows the device's name.
        platform::setHostname(readDeviceName());
        // Before the interface reads it.
        syncEthConfig();
        // Baseline it, so a later change fires but the first tick does not restart a client.
        appliedAddressingSig_ = addressingSig();
        addressingSigApplied_ = true;
        // Ethernet first, without blocking.
        if (platform::ethInit()) {
            state_ = State::WaitingEth;
            std::printf("NetworkModule: Ethernet init started\n");
        } else if constexpr (platform::hasWiFi) {
            // No Ethernet, so fall back through WiFi to the access point.
            if (ssid_[0] != 0 && platform::wifiStaInit(ssid_, password_)) {
                state_ = State::WaitingSta;
                syncTxPower();  // see setWifiCredentials's syncTxPower comment
                std::printf("NetworkModule: WiFi STA init started, SSID: %s\n", ssid_);
            } else {
                startAP();
            }
        } else {
            // No fallback here, so stay idle until a cable appears.
            state_ = State::Idle;
            std::snprintf(statusBuf_, sizeof(statusBuf_), "No network (Ethernet only)"); setStatus(statusBuf_, Severity::Error);
        }

        stateChangeTime_ = platform::millis();

        // After we have claimed what we need, so a child sets up against it.
        MoonModule::setup();
    }

    /// The controller's data pads while Ethernet runs, which the chip fixed and nobody sets.
    uint8_t fixedPins(FixedPin* out, uint8_t max) const override {
        // The applied type rather than the pending control.
        if (!out || appliedEthType_ == static_cast<uint8_t>(platform::ethNone)) return 0;
        uint8_t n = 0;
        for (uint8_t i = 0; i < platform::ethFixedPadCount && n < max; i++)
            out[n++] = FixedPin{platform::ethFixedPads[i].gpio, platform::ethFixedPads[i].name};
        return n;
    }

    /// Declare the mode, the credentials, the addressing and the interface pins.
    void defineControls() override {
        // Chained first, so a child's controls land before this module's own.
        MoonModule::defineControls();

        setStatus(statusBuf_);

        // So a rebuild mid-transition shows current numbers.
        updateMetrics();

        // Always present, since every variant has a mode.
        controls_.addReadOnly("mode", modeStr_, sizeof(modeStr_));

        // Absent entirely in a build without WiFi.
        if constexpr (platform::hasWiFi) {
            controls_.addText("ssid", ssid_, sizeof(ssid_));
            controls_.addPassword("password", password_, sizeof(password_));
            // Meaningful only while associated, so hidden elsewhere rather than showing zero.
            controls_.addReadOnlyInt("rssi", rssi_, "dBm");
            controls_.setHidden(controls_.count() - 1, state_ != State::ConnectedSta);
            // Hidden where the radio is off, and expert-only as a tuning readout.
            controls_.addReadOnlyInt("txPower", txPower_, "dBm");
            const bool radioOn = (state_ == State::ConnectedSta
                                  || state_ == State::WaitingSta
                                  || state_ == State::AP);
            controls_.setHidden(controls_.count() - 1, !radioOn);
            controls_.setAdvanced(controls_.count() - 1);
            // Zero lifts any prior cap rather than leaving it sticky.
            controls_.addControl("txPowerSetting", txPowerSetting_, 0, 21);
            controls_.setHidden(controls_.count() - 1, !radioOn);
            controls_.setAdvanced(controls_.count() - 1);
        }
        // Expert-only: discovery works without it.
        controls_.addControl("mDNS", mdnsEnabled_);
        controls_.setAdvanced(controls_.count() - 1);

        // Immediately before the fields it conditions, so the two stay adjacent.
        controls_.addSelect("addressing", addressing_, addressingOptions_, 2);

        // Always bound so persistence can load them, with only their visibility conditional.
        const bool hideStatic = (addressing_ != kAddressingStatic);
        controls_.addIPv4("ip", staticIp_);
        controls_.setHidden(controls_.count() - 1, hideStatic);
        controls_.addIPv4("gateway", staticGateway_);
        controls_.setHidden(controls_.count() - 1, hideStatic);
        controls_.addIPv4("subnet", staticSubnet_);
        controls_.setHidden(controls_.count() - 1, hideStatic);
        controls_.addIPv4("dns", staticDns_);
        controls_.setHidden(controls_.count() - 1, hideStatic);

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
            // The data pads are fixed and reported separately; the management pair is not.
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
            // One loop rather than a call beside every row: the whole group carries one tag.
            if constexpr (preview) {
                for (uint8_t i = firstEthControl; i < controls_.count(); i++) controls_.setDeveloper(i);
            }
        }
    }

    /// Advance the cascade, then apply anything a control changed live.
    void tick1s() MM_NONBLOCKING override {
        uint32_t now = platform::millis();
        uint32_t elapsed = now - stateChangeTime_;

        // Held only while the leaseless cable is still in, so unplugging clears it.
        if (!platform::ethLinkUp() && (ethDegraded_ || ethLinkUpAt_ != 0)) {
            ethLinkUpAt_ = 0;
            if (ethDegraded_) {
                ethDegraded_ = false;
                updateStatusIP();   // reverts to the WiFi/AP IP line (or leaves prior status if none)
            }
        }

        switch (state_) {
            case State::WaitingEth:
                // A static address needs no lease, so pin it as soon as the link is up.
                if (addressing_ == kAddressingStatic && platform::ethLinkUp() && !platform::ethConnected())
                    applyStaticIfConfigured(platform::NetIface::Eth);
                if (platform::ethConnected()) {
                    onConnected("Ethernet");
                } else if ((elapsed > 3000 && !platform::ethLinkUp()) || elapsed > kEthDhcpWaitMs) {
                    // No link at all, or a link with no address, the second being remembered.
                    if (platform::ethLinkUp()) {
                        ethDegraded_ = true;
                        writeEthDegradedStatus();
                    } else {
                        ethDegraded_ = false;
                        mm::formatTo(statusBuf_, sizeof(statusBuf_), "Ethernet not detected: no cable/link");
                        setStatus(statusBuf_, Severity::Warning);
                    }
                    if constexpr (platform::hasWiFi) {
                        // No cable, or a link with no address: cascade onward.
                        std::printf("NetworkModule: Ethernet %s, cascading\n",
                                    platform::ethLinkUp() ? "no IP (DHCP timeout)" : "no link (no cable)");
                        if (ssid_[0] != 0 && platform::wifiStaInit(ssid_, password_)) {
                            state_ = State::WaitingSta;
                            stateChangeTime_ = now;
                            syncTxPower();  // see setWifiCredentials's syncTxPower comment
                        } else {
                            startAP();
                        }
                    } else {
                        // No fallback here, so keep polling for a cable.
                        mm::formatTo(statusBuf_, sizeof(statusBuf_), "No network (Ethernet only)"); setStatus(statusBuf_, Severity::Error);
                        stateChangeTime_ = now;
                    }
                }
                break;

            case State::WaitingSta:
                if constexpr (platform::hasWiFi) {
                    // Pinned during bring-up, since a network without a server fires no event.
                    if (addressing_ == kAddressingStatic && !platform::wifiStaConnected())
                        applyStaticIfConfigured(platform::NetIface::Sta);
                    if (platform::wifiStaConnected()) {
                        onConnected("WiFi STA");
                    } else if (elapsed > kStaGraceMs) {
                        // It did not connect in time, so fall back.
                        platform::wifiStaStop();
                        noteRadioStopped();
                        startAP();
                    }
                }
                break;

            case State::ConnectedEth:
                if (!platform::ethConnected()) {
                    if constexpr (platform::hasWiFi) {
                        std::printf("NetworkModule: Ethernet dropped, cascading\n");
                        platform::mdnsStop();
                        if (ssid_[0] != 0 && platform::wifiStaInit(ssid_, password_)) {
                            state_ = State::WaitingSta;
                            stateChangeTime_ = now;
                            syncTxPower();  // see setWifiCredentials's syncTxPower comment
                        } else {
                            startAP();
                        }
                    } else {
                        // Drop back to polling for the cable.
                        std::printf("NetworkModule: Ethernet dropped\n");
                        platform::mdnsStop();
                        mm::formatTo(statusBuf_, sizeof(statusBuf_), "No network (Ethernet only)"); setStatus(statusBuf_, Severity::Error);
                        state_ = State::WaitingEth;
                        stateChangeTime_ = now;
                    }
                }
                updateStatusIP();
                break;

            case State::ConnectedSta:
                if constexpr (platform::hasWiFi) {
                    // Ethernet outranks WiFi, but only once it works.
                    if (addressing_ == kAddressingStatic && platform::ethLinkUp() && !platform::ethConnected())
                        applyStaticIfConfigured(platform::NetIface::Eth);
                    if (platform::ethConnected()) {
                        std::printf("NetworkModule: Ethernet up, switching from WiFi STA\n");
                        platform::mdnsStop();
                        onConnected("Ethernet");
                    } else if (platform::ethLinkUp()) {
                        // Already being retried, so only a persistently addressless link is flagged.
                        if (ethLinkUpAt_ == 0) ethLinkUpAt_ = now;   // link just (re)appeared: start the clock
                        if (!ethDegraded_ && now - ethLinkUpAt_ > kEthDhcpWaitMs) {
                            ethDegraded_ = true;
                            writeEthDegradedStatus();
                        }
                    } else if (!platform::wifiStaConnected()) {
                        // A dropout is not a divorce: the radio reconnects itself within seconds.
                        if (staLostTime_ == 0) {
                            staLostTime_ = now;
                            std::printf("NetworkModule: WiFi STA dropped, reconnecting\n");
                            mm::formatTo(statusBuf_, sizeof(statusBuf_), "WiFi reconnecting…");
                            setStatus(statusBuf_, Severity::Warning);
                        } else if (now - staLostTime_ > kStaGraceMs) {
                            std::printf("NetworkModule: WiFi STA gone for %us, starting AP\n",
                                        (unsigned)(kStaGraceMs / 1000));
                            platform::mdnsStop();
                            platform::wifiStaStop();
                            noteRadioStopped();
                            staLostTime_ = 0;
                            startAP();
                        }
                    } else {
                        staLostTime_ = 0;   // reconnected in time, so back to normal
                        updateStatusIP();
                    }
                }
                break;

            case State::AP:
                if constexpr (platform::hasWiFi) {
                    // Promote as soon as something better appears.
                    if (platform::ethConnected()) {
                        onConnected("Ethernet");
                    } else if (ssid_[0] != 0 && platform::wifiStaConnected()) {
                        onConnected("WiFi STA");
                    } else if (ssid_[0] != 0 && now - stateChangeTime_ > kApRetryStaMs
                               && platform::wifiApClientCount() == 0) {
                        // A fallback, not a destination, so look periodically when nobody is on it.
                        std::printf("NetworkModule: AP — retrying WiFi STA (%s)\n", ssid_);
                        if (platform::wifiStaInit(ssid_, password_)) {
                            state_ = State::WaitingSta;
                            stateChangeTime_ = now;
                            syncTxPower();   // see setWifiCredentials's syncTxPower comment
                        } else {
                            stateChangeTime_ = now;   // init refused; wait out another interval
                        }
                    }
                }
                break;

            case State::Idle:
                // Every path failed, but the stack runs on, so a late interface still promotes.
                if (platform::ethConnected()) {
                    std::printf("NetworkModule: Ethernet up (recovered from Idle)\n");
                    onConnected("Ethernet");
                } else if constexpr (platform::hasWiFi) {
                    if (platform::wifiStaConnected()) {
                        std::printf("NetworkModule: WiFi STA up (recovered from Idle)\n");
                        onConnected("WiFi STA");
                    }
                }
                break;
        }

        syncMdns();
        syncTxPower();
        syncEthLive();          // apply an interface change live where possible
        syncAddressingLive();   // and an addressing change likewise

        // Writing the same storage is enough, since the UI polls for these.
        updateMetrics();

        // After our own state machine, since a child may call back into it.
        MoonModule::tick1s();
    }

    /// Release the children first, then shut down whichever interface is up.
    void release() override {
        // Children first, so the provisioning task stops before the network state goes.
        MoonModule::release();
        platform::mdnsShutdown();
        if constexpr (platform::hasWiFi) {
            if (state_ == State::AP) { platform::wifiApStop(); noteRadioStopped(); }
            if (state_ == State::ConnectedSta || state_ == State::WaitingSta) {
                platform::wifiStaStop();
                noteRadioStopped();
            }
        }
    }

private:
    Scheduler* scheduler_ = nullptr;
    SystemModule* systemModule_ = nullptr;

    enum class State : uint8_t {
        Idle,
        WaitingEth,
        WaitingSta,
        ConnectedEth,
        ConnectedSta,
        AP
    };

    State state_ = State::Idle;
    uint32_t stateChangeTime_ = 0;
    /// When the link was first seen down, the radio reconnecting itself meanwhile.
    uint32_t staLostTime_ = 0;
    /// How long WiFi gets to connect or recover, the question being the same in both cases.
    static constexpr uint32_t kStaGraceMs = 10000;
    /// How long a live Ethernet link may sit without an address before we give up on it.
    static constexpr uint32_t kEthDhcpWaitMs = 15000;
    /// How often the access point goes back and retries WiFi, long because each attempt bounces it.
    static constexpr uint32_t kApRetryStaMs = 60000;
    bool apShutdownPending_ = false;   ///< the access point is waiting to be torn down
    // Kept set so the warning outranks the fallback's connected line, until the cable is out.
    bool ethDegraded_ = false;
    // When the link last came up, so a re-plugged cable gets the same window as first boot.
    uint32_t ethLinkUpAt_ = 0;
    bool mdnsRunning_ = false;   ///< whether the local-name service is advertising
    // The name last advertised, so a live rename is noticed and re-registered.
    char lastMdnsName_[24] = {};

    char ssid_[33] = {};       ///< the network to join
    char password_[64] = {};   ///< its credential, stored obfuscated
    // Named so the comparisons below read as intent rather than as a literal.
    static constexpr uint8_t kAddressingDhcp = 0;
    static constexpr uint8_t kAddressingStatic = 1;
    uint8_t addressing_ = kAddressingDhcp;   ///< which addressing mode is selected
    bool mdnsEnabled_ = true;                ///< whether to advertise the local name
    // The storage behind the inherited status slot, which holds only a pointer into it.
    char statusBuf_[48] = {};

    // Octets rather than strings, always bound, with only their visibility conditional.
    uint8_t staticIp_[4]      = {0, 0, 0, 0};
    uint8_t staticGateway_[4] = {0, 0, 0, 0};
    uint8_t staticSubnet_[4]  = {255, 255, 255, 0};
    uint8_t staticDns_[4]     = {0, 0, 0, 0};

    char modeStr_[20] = {};   ///< the mode label, sized to the longest
    int8_t rssi_ = 0;         ///< the signal strength, stored directly rather than formatted
    int8_t txPower_ = 0;      ///< the radio's current power

    // The platform takes quarter-decibels, and the applied value detects a change.
    int16_t txPowerSetting_ = 0;
    int16_t appliedTxPowerSetting_ = -1;   ///< negative until the first apply

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
    // The same guard shape for addressing, so a re-apply follows only a real change.
    uint32_t appliedAddressingSig_ = 0;
    bool addressingSigApplied_ = false;

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
        // Waveshare ESP32-P4-ETH: same IP101 reference wiring as the P4-NANO (MDC/MDIO confirmed
        // from the schematic netlist, reset/clock confirmed by direct schematic inspection), kept
        // as its own entry rather than a silent alias so a future pin correction to one board
        // can't also move the other.
        {"ESP32-P4-ETH",      2,  1, 31, 52, 51, 50, true,  -1, -1, -1, -1, -1, false},
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

    /// Push the interface config to the platform, before bring-up reads it.
    void syncEthConfig() {
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
            appliedEthSig_ = ethSig();   ///< this config is now applied
            appliedEthType_ = ethType_;  ///< and this is what holds the pads
            ethSigApplied_ = true;
        }
    }

    /// Apply an interface change live where the hardware allows it, else at the next boot.
    void syncEthLive() {
        if constexpr (platform::hasEthernet) {
            if (ethSigApplied_ && ethSig() == appliedEthSig_) return;   // nothing changed
            // Only for the SPI device, since elsewhere a restart would strand the device.
            const bool hotReinit = (ethType_ == 3) && platform::hasEthW5500;
            if (hotReinit) {
                platform::ethStop();
                syncEthConfig();                       // pushes cfg + records the new sig
                if (platform::ethInit()) {
                    state_ = State::WaitingEth;
                    stateChangeTime_ = platform::millis();
                    std::printf("NetworkModule: W5500 re-init (live config change)\n");
                } else {
                    std::snprintf(statusBuf_, sizeof(statusBuf_),
                                  "W5500 re-init failed — check pins"); setStatus(statusBuf_, Severity::Error);
                }
            } else {
                // Record it for the next boot without disturbing the running interface.
                syncEthConfig();
                std::snprintf(statusBuf_, sizeof(statusBuf_),
                              "Ethernet config saved — restart to apply"); setStatus(statusBuf_);
            }
        }
    }

    /// Apply an addressing change live, on whichever interface is connected.
    void syncAddressingLive() {
        uint32_t sig = addressingSig();
        if (addressingSigApplied_ && sig == appliedAddressingSig_) return;   // nothing changed
        appliedAddressingSig_ = sig;
        addressingSigApplied_ = true;

        platform::NetIface iface;
        if (state_ == State::ConnectedEth) iface = platform::NetIface::Eth;
        else if constexpr (platform::hasWiFi) {
            if (state_ != State::ConnectedSta) return;   // not on a client interface: nothing to apply
            iface = platform::NetIface::Sta;
        } else return;

        if (addressing_ == kAddressingStatic) {
            applyStaticIfConfigured(iface);
        } else {
            platform::netSetDhcp(iface);   // Static → DHCP: re-lease live
        }
        updateStatusIP();   // reflect the new address (static IP, or the re-leased one once it lands)
    }

    /// A hash over the mode and the static octets, so an edit to any of them re-applies.
    uint32_t addressingSig() const {
        uint32_t h = 2166136261u;
        auto fold = [&](uint8_t b) { h = (h ^ b) * 16777619u; };
        fold(addressing_);
        for (int i = 0; i < 4; i++) { fold(staticIp_[i]); fold(staticGateway_[i]); fold(staticSubnet_[i]); fold(staticDns_[i]); }
        return h;
    }

    static constexpr const char* addressingOptions_[] = {"DHCP", "Static"};
    // The order must match the platform's own enum, since the control stores an index.
    static constexpr const char* ethTypeOptions_[] = {"None", "LAN8720", "IP101", "W5500", "YT8531"};

    /// Start our own access point, so a user can reach the device to configure it.
    void startAP() {
        // The same identity as every other name, so a device shows one everywhere.
        const char* apName = readDeviceName();
        if (platform::wifiApInit(apName, "4.3.2.1")) {
            state_ = State::AP;
            stateChangeTime_ = platform::millis();
            apShutdownPending_ = true;
            syncTxPower();  // see setWifiCredentials's syncTxPower comment
            std::snprintf(statusBuf_, sizeof(statusBuf_), "AP: %s @ 4.3.2.1", apName); setStatus(statusBuf_, Severity::Status);
            // The address is what a user needs, the name alone sending them looking.
            std::printf("NetworkModule: AP started: %s → join it and open http://4.3.2.1\n", apName);
        } else {
            state_ = State::Idle;
            std::snprintf(statusBuf_, sizeof(statusBuf_), "No network"); setStatus(statusBuf_, Severity::Error);
        }
        // The status needs no rebuild, but the radio readouts' visibility does.
        rebuildControls();
        if (scheduler_) scheduler_->prepareTree();
    }

    /// Adopt a connected interface, shutting down whatever it outranks.
    void onConnected(const char* via) {
        if (std::strcmp(via, "Ethernet") == 0) {
            state_ = State::ConnectedEth;
            ethDegraded_ = false;   // Ethernet itself got a lease — no longer degraded
        } else {
            state_ = State::ConnectedSta;
            // Associated, but the address comes from us, so pin it before the status reads it.
            if constexpr (platform::hasWiFi) applyStaticIfConfigured(platform::NetIface::Sta);
        }
        stateChangeTime_ = platform::millis();

        // Shut down whatever this outranks.
        if constexpr (platform::hasWiFi) {
            if (apShutdownPending_ || platform::wifiApConnected()) {
                std::printf("NetworkModule: Shutting down AP (higher priority connected)\n");
                platform::wifiApStop();
                noteRadioStopped();
                apShutdownPending_ = false;
            }
            if (state_ == State::ConnectedEth && platform::wifiStaConnected()) {
                std::printf("NetworkModule: Shutting down WiFi STA (Ethernet connected)\n");
                platform::wifiStaStop();
                noteRadioStopped();
            }
        }

        updateStatusIP();
        std::printf("NetworkModule: Connected via %s — %s\n", via, statusBuf_);

        syncMdns();

        // Again for the radio readouts' visibility, which depends on the state.
        rebuildControls();
        if (scheduler_) scheduler_->prepareTree();
    }

public:
    /// The current address as octets, all zero meaning not connected.
    void currentIp(uint8_t out[4]) const {
        out[0] = out[1] = out[2] = out[3] = 0;
        if (state_ == State::ConnectedEth) platform::ethGetIPv4(out);
        else if constexpr (platform::hasWiFi) {
            if (state_ == State::ConnectedSta) platform::wifiStaGetIPv4(out);
        }
    }

private:
    /// Pin the configured address onto this interface, or do nothing where a client runs.
    void applyStaticIfConfigured(platform::NetIface iface) {
        if (addressing_ != kAddressingStatic) return;   // leave the client running
        platform::netSetStaticIPv4(iface, staticIp_, staticGateway_, staticSubnet_, staticDns_);
    }

    /// The device name, which the system module owns and guarantees valid.
    const char* readDeviceName() const {
        return systemModule_ ? systemModule_->deviceName() : "";
    }
    /// Report a link that is up but unaddressed, which outranks a fallback's connected line.
    void writeEthDegradedStatus() {
        // A driver addressing the wire directly wants no address, so that is not a fault.
        if (platform::ethRawL2Claimed()) {
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "Ethernet: link up, no IP (L2 in use)");
            setStatus(statusBuf_, Severity::Status);
            return;
        }
        uint8_t ip[4] = {};
        platform::ethGetIPv4(ip);
        if (ip[0] || ip[1] || ip[2] || ip[3]) {
            char ipStr[16]; formatDottedQuad(ipStr, ip);
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "Ethernet detected (%s): no lease", ipStr);
        } else {
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "Ethernet detected: no address assigned");
        }
        setStatus(statusBuf_, Severity::Warning);
    }

    /// Report the current address, or keep the degraded warning where one stands.
    void updateStatusIP() {
        // The warning outranks this, so it is not buried the moment the cascade connects.
        if (ethDegraded_ && state_ != State::ConnectedEth) { writeEthDegradedStatus(); return; }
        uint8_t ip[4];
        currentIp(ip);   // same eth/wifi getter dispatch, in one place
        if (!ip[0] && !ip[1] && !ip[2] && !ip[3]) return;   // not connected, so keep what stands
        char ipStr[16];
        formatDottedQuad(ipStr, ip);
        if (state_ == State::ConnectedEth) {
            // The negotiated speed too, since a link that fell back looks identical without it.
            const uint16_t mbps = platform::ethLinkSpeedMbps();
            if (mbps > 0) mm::formatTo(statusBuf_, sizeof(statusBuf_), "Eth: %s (%u Mbit)",
                                        ipStr, static_cast<unsigned>(mbps));
            else          mm::formatTo(statusBuf_, sizeof(statusBuf_), "Eth: %s", ipStr);
        } else {
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "WiFi: %s", ipStr);
        }
        setStatus(statusBuf_, Severity::Status);
    }

    /// Push the power cap to the radio whenever it changes, idempotently.
    void syncTxPower() {
        if constexpr (!platform::hasWiFi) return;
        if (txPowerSetting_ == appliedTxPowerSetting_) return;
        // A genuine no-op, not an optimization: pushing one here boot-loops a device.
        if (txPowerSetting_ == 0 && appliedTxPowerSetting_ <= 0) {
            appliedTxPowerSetting_ = 0;   // mark synced so we don't re-check every tick
            return;
        }
        const bool radioUp = (state_ == State::ConnectedSta
                              || state_ == State::WaitingSta
                              || state_ == State::AP);
        if (!radioUp) return;
        // The platform has no reset call, so lifting a cap pushes the ceiling instead.
        const int8_t quarterDbm = (txPowerSetting_ == 0)
                                  ? static_cast<int8_t>(80)
                                  : static_cast<int8_t>(txPowerSetting_ * 4);
        if (platform::wifiSetTxPower(quarterDbm)) {
            appliedTxPowerSetting_ = txPowerSetting_;
        }
    }

    /// Forget the applied cap, since stopping the radio resets what it holds.
    void noteRadioStopped() { appliedTxPowerSetting_ = -1; }

    /// Start, restart or stop the local-name advertisement to match the state.
    void syncMdns() {
        bool shouldRun = mdnsEnabled_ && (state_ == State::ConnectedEth || state_ == State::ConnectedSta);
        const char* devName = readDeviceName();
        if (shouldRun && !mdnsRunning_) {
            // Marked only on success, so a failure retries next tick.
            if (platform::mdnsInit(devName)) {
                mdnsRunning_ = true;
                std::strncpy(lastMdnsName_, devName, sizeof(lastMdnsName_) - 1);
                lastMdnsName_[sizeof(lastMdnsName_) - 1] = 0;
            }
        } else if (shouldRun && mdnsRunning_ && std::strcmp(devName, lastMdnsName_) != 0) {
            // A live rename, so re-register and the local name follows at once.
            if (platform::mdnsInit(devName)) {
                std::strncpy(lastMdnsName_, devName, sizeof(lastMdnsName_) - 1);
                lastMdnsName_[sizeof(lastMdnsName_) - 1] = 0;
            }
        } else if (!shouldRun && mdnsRunning_) {
            platform::mdnsStop();
            mdnsRunning_ = false;
        }
    }

    /// The plain-language label for the current state, a switch so a new one must be handled.
    const char* modeLabel() const {
        switch (state_) {
            case State::Idle:         return "Idle";
            case State::WaitingEth:   return "Ethernet (waiting)";
            case State::WaitingSta:   return "WiFi STA (waiting)";
            case State::ConnectedEth: return "Ethernet";
            case State::ConnectedSta: return "WiFi STA";
            case State::AP:           return "WiFi AP";
        }
        return "Unknown";
    }

    /// Refresh the mode label and the radio readings.
    void updateMetrics() {
        mm::formatTo(modeStr_, sizeof(modeStr_), "%s", modeLabel());
        if constexpr (platform::hasWiFi) {
            // Refreshed even while hidden, so a return to a radio state shows no stale value.
            rssi_ = (state_ == State::ConnectedSta)
                    ? static_cast<int8_t>(platform::wifiStaRssi()) : 0;
            const bool radioOn = (state_ == State::ConnectedSta
                                  || state_ == State::WaitingSta
                                  || state_ == State::AP);
            txPower_ = radioOn ? static_cast<int8_t>(platform::wifiTxPower()) : 0;
        }
    }

};

} // namespace mm
