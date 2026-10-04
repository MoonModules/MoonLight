#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"
#include "core/system/SystemModule.h"
#include "core/system/FilesystemModule.h"
#include "core/system/EthernetModule.h"
#include "core/system/WiFiModule.h"
#include "core/system/AccessPointModule.h"
#include "core/util/CaptivePortal.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>

namespace mm {

/// Whether an IPv4 address is link-local (169.254.0.0/16), the one a device gives itself when no DHCP server answers.
inline bool isLinkLocalIPv4(const uint8_t ip[4]) { return ip[0] == 169 && ip[1] == 254; }

/// Whether an interface's address lets the cascade use it, `configured` being the user's static address or null: @xref{a-link-local-address-is-a-last-resort}.
inline bool addressCounts(const uint8_t ip[4], bool buildHasWiFi, const uint8_t* configured) {
    if (!isLinkLocalIPv4(ip) || !buildHasWiFi) return true;
    return configured && std::memcmp(ip, configured, 4) == 0;
}

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
/// ## A link-local address is a last resort
///
/// With no DHCP server the client gives itself a 169.254.x.y address (RFC 3927), so a laptop on the same cable still reaches the device by name.
/// It counts as connected only on a build without WiFi, where it is the one way in.
/// Where WiFi exists it never displaces the cascade: a cable on a network without DHCP would otherwise switch off a working WiFi path or the access point.
/// A 169.254 address the user set as static is a choice, not a fallback, so it counts everywhere, while a self-assigned one beside a static setting still does not.
///
/// The device name belongs to the system module, and is the one identity behind every name.
/// It registers before the light pipeline, which then sees the real remaining heap.
class NetworkModule : public MoonModule {
public:
    /// Adopt the scheduler, which the tree rebuild after a mode change goes through.
    void setScheduler(Scheduler* s) { scheduler_ = s; }
    /// Adopt the system module, which owns the device name this one reads.
    void setSystemModule(SystemModule* s) { systemModule_ = s; }
    /// Adopt the wired interface's settings, a child this module's cascade reads.
    void setEthernet(EthernetModule* e) { ethernet_ = e; }
    /// Adopt the station's settings, the known networks and the radio's, a child this module's cascade reads.
    void setWiFi(WiFiModule* w) { wifi_ = w; }
    /// Adopt the access point's settings, a child this module's cascade opens and closes.
    void setAccessPoint(AccessPointModule* a) { ap_ = a; }

    /// Persist and apply the power cap, before credentials arriving with it are used.
    void setTxPowerSetting(uint8_t dBm) {
        if (!wifi_) return;
        wifi_->setTxPowerSetting(dBm);
        syncTxPower();   // now if the radio is up, else on the next start
    }

    /// Remember a network at the top of the known list and drive a clean transition into joining it.
    void setWifiCredentials(const char* ssid, const char* password) {
        if (!ssid || !wifi_ || !wifi_->remember(ssid, password)) return;
        // Ethernet outranks WiFi, so while the cable carries the device the network is only remembered, as a join from the card is.
        if (state_ == State::WaitingEth || state_ == State::ConnectedEth) return;
        if constexpr (platform::hasWiFi) {
            // Tear down first: the platform would otherwise skip registering its handler. A running access point stays, beside the station.
            if (state_ == State::WaitingSta || state_ == State::ConnectedSta) {
                platform::wifiStaStop();
                noteRadioStopped();
            }
            if (startSta()) {
                state_ = State::WaitingSta;
                stateChangeTime_ = platform::millis();
                // Before the radio's first burst, which is the window the cap protects.
                syncTxPower();
                std::snprintf(statusBuf_, sizeof(statusBuf_), "WiFi STA: %s", wifi_->ssidAt(staIndex()));
                setStatus(statusBuf_, Severity::Status);
                // Re-evaluate visibility, or a now-stale signal reading would stay rendered.
                rebuildControls();
            } else {
                // Recover through the access point, so credentials can be re-entered.
                fallBack();
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
        if (ethernet_) ethernet_->syncConfig();
        // The system module's own buffer, so a rename shows in the links at once.
        if (wifi_) wifi_->setDeviceName(readDeviceName());
        // Ethernet first, without blocking.
        if (platform::ethInit()) {
            state_ = State::WaitingEth;
            std::printf("NetworkModule: Ethernet init started\n");
        } else if constexpr (platform::hasWiFi) {
            // No Ethernet, so fall back through WiFi to the access point.
            if (startSta()) {
                state_ = State::WaitingSta;
                syncTxPower();  // see setWifiCredentials's syncTxPower comment
            } else {
                fallBack();
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

    /// Declare the mode, the credentials and the station's IP settings.
    void defineControls() override {
        // Chained first, so a child's controls land before this module's own.
        MoonModule::defineControls();

        setStatus(statusBuf_);

        // So a rebuild mid-transition shows current numbers.
        updateMetrics();

        // Always present, since every variant has a mode.
        controls_.addReadOnly("mode", modeStr_, sizeof(modeStr_));

        // Expert-only: discovery works without it.
        controls_.addControl("mDNS", mdnsEnabled_);
        controls_.setAdvanced(controls_.count() - 1);
    }

    /// Advance the cascade, then apply anything a control changed live.
    void tick1s() MM_NONBLOCKING override {
        uint32_t now = platform::millis();

        // Held only while the leaseless cable is still in, so unplugging clears it.
        if (!platform::ethLinkUp() && (ethDegraded_ || ethLinkUpAt_ != 0)) {
            ethLinkUpAt_ = 0;
            if (ethDegraded_) {
                ethDegraded_ = false;
                updateStatusIP();   // reverts to the WiFi/AP IP line (or leaves prior status if none)
            }
        }

        // Before the cascade judges the link, or a new static address reads as a lost one.
        syncStaIpLive();
        startRequestedJoin(now);
        if (ethernet_ && ethernet_->syncIpLive(state_ == State::ConnectedEth)) updateStatusIP();
        // After a join the card asked for, which restarts the clock, or that join reads as timed out at once.
        const uint32_t elapsed = now - stateChangeTime_;

        switch (state_) {
            case State::WaitingEth:
                // A static address needs no lease, so pin it as soon as the link is up.
                if (ethernet_ && platform::ethLinkUp() && !ethUp()) ethernet_->applyStatic();
                if (ethUp()) {
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
                        if (startSta()) {
                            state_ = State::WaitingSta;
                            stateChangeTime_ = now;
                            syncTxPower();  // see setWifiCredentials's syncTxPower comment
                        } else {
                            fallBack();
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
                    if (!staUp()) applyStaStatic();
                    if (staUp()) {
                        // A join asked for from the card succeeded: a scanned network is now known, at the top.
                        if (wifi_ && wifi_->joinRequest()) {
                            staId_ = wifi_->joinSucceeded();
                            appliedStaIpSig_ = wifi_->ipSigAt(staIndex());
                        }
                        onConnected("WiFi STA");
                        // The phone that asked from the access point follows the links to the new address.
                        if (apHoldFrom_ && wifi_) {
                            uint8_t ip[4];
                            platform::wifiStaGetIPv4(ip);
                            wifi_->showHandoff(ip);
                        }
                    } else if (elapsed > kStaGraceMs && wifi_ && wifi_->joinRequest()) {
                        // It said why on the card; the known networks take over again.
                        wifi_->joinFailed(platform::wifiStaLastFailure());
                        platform::wifiStaStop();
                        noteRadioStopped();
                        if (startSta()) { stateChangeTime_ = now; syncTxPower(); }
                        else fallBack();
                    } else if (elapsed > kStaGraceMs) {
                        // It did not connect in time, so try the next known network, then fall back.
                        platform::wifiStaStop();
                        noteRadioStopped();
                        if (startSta(static_cast<uint8_t>(wifi_ ? wifi_->indexOfId(staId_) + 1 : 0))) {
                            stateChangeTime_ = now;
                            syncTxPower();   // see setWifiCredentials's syncTxPower comment
                        } else {
                            fallBack();
                        }
                    }
                }
                break;

            case State::ConnectedEth:
                if (!ethUp()) {
                    if constexpr (platform::hasWiFi) {
                        std::printf("NetworkModule: Ethernet dropped, cascading\n");
                        platform::mdnsStop();
                        if (startSta()) {
                            state_ = State::WaitingSta;
                            stateChangeTime_ = now;
                            syncTxPower();  // see setWifiCredentials's syncTxPower comment
                        } else {
                            fallBack();
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
                    if (ethernet_ && platform::ethLinkUp() && !ethUp()) ethernet_->applyStatic();
                    if (ethUp()) {
                        std::printf("NetworkModule: Ethernet up, switching from WiFi STA\n");
                        platform::mdnsStop();
                        onConnected("Ethernet");
                        break;
                    }
                    if (platform::ethLinkUp()) {
                        // Already being retried, so only a persistently addressless link is flagged.
                        if (ethLinkUpAt_ == 0) ethLinkUpAt_ = now;   // link just (re)appeared: start the clock
                        if (!ethDegraded_ && now - ethLinkUpAt_ > kEthDhcpWaitMs) {
                            ethDegraded_ = true;
                            writeEthDegradedStatus();
                        }
                    }
                    // Checked whatever the cable does, since the station is what carries the device here.
                    if (!staUp()) {
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
                            fallBack();
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
                    if (ethUp()) {
                        onConnected("Ethernet");
                    } else if (hasKnown() && staUp()) {
                        onConnected("WiFi STA");
                    } else {
                        retryKnown(now);
                    }
                }
                break;

            case State::Idle:
                // Every path failed, but the stack runs on, so a late interface still promotes.
                if (ethUp()) {
                    std::printf("NetworkModule: Ethernet up (recovered from Idle)\n");
                    onConnected("Ethernet");
                } else if constexpr (platform::hasWiFi) {
                    if (staUp()) {
                        std::printf("NetworkModule: WiFi STA up (recovered from Idle)\n");
                        onConnected("WiFi STA");
                    } else {
                        retryKnown(now);
                    }
                }
                break;
        }

        syncAccessPoint(now);
        syncMdns();
        syncTxPower();
        // An interface change applies live where the hardware allows, the cascade then waiting on it again.
        if (ethernet_ && ethernet_->syncLive()) {
            state_ = State::WaitingEth;
            stateChangeTime_ = platform::millis();
        }

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
            closeAp();
            if (state_ == State::ConnectedSta || state_ == State::WaitingSta) {
                platform::wifiStaStop();
                noteRadioStopped();
            }
        }
    }

private:
    Scheduler* scheduler_ = nullptr;
    SystemModule* systemModule_ = nullptr;
    EthernetModule* ethernet_ = nullptr;   ///< the wired interface's settings, absent on a build without Ethernet
    WiFiModule* wifi_ = nullptr;           ///< the station's settings, absent on a build without WiFi
    AccessPointModule* ap_ = nullptr;      ///< the access point's settings, absent on a build without WiFi

    /// The known network being tried or joined, by row id so adding, moving or forgetting a row keeps it, or 0 for a network picked from the scan.
    uint32_t staId_ = 0;
    /// Where that network now is in the known list; past the end (no row) for none, which the list's lookups treat as no network.
    uint8_t staIndex() const MM_NONBLOCKING {
        const int i = wifi_ ? wifi_->indexOfId(staId_) : -1;
        return i < 0 ? 0xFF : static_cast<uint8_t>(i);
    }
    /// Which join request has been started, 0 for none; a request with another number replaces it.
    uint32_t startedSeq_ = 0;

    /// Carry out a join the card asked for, connect-first: drop what the station is doing and join that network now.
    void startRequestedJoin(uint32_t now) {
        if constexpr (platform::hasWiFi) {
            if (!wifi_) return;
            const WiFiModule::JoinRequest* req = wifi_->joinRequest();
            if (!req) { startedSeq_ = 0; return; }
            if (startedSeq_ == req->seq) return;   // already joining it; the WaitingSta case finishes it
            if (state_ == State::ConnectedEth || state_ == State::WaitingEth) {
                wifi_->joinRefused("Ethernet is in use: WiFi joins when it is unplugged");
                return;
            }
            // Asked for from the access point: it stays up beside the station, so the phone sees how the join went.
            if (apUp_) apHoldFrom_ = now ? now : 1;
            if (state_ == State::WaitingSta || state_ == State::ConnectedSta) {
                platform::mdnsStop();
                platform::wifiStaStop();
                noteRadioStopped();
            }
            startedSeq_ = req->seq;
            staId_ = req->knownId;
            appliedStaIpSig_ = wifi_->ipSigAt(staIndex());
            std::printf("NetworkModule: WiFi STA joining %s, asked for from the card\n", req->ssid);
            if (platform::wifiStaInit(req->ssid, req->password)) {
                state_ = State::WaitingSta;
                stateChangeTime_ = now;
                syncTxPower();
            } else {
                wifi_->joinFailed(platform::WifiFailure::Other);
                fallBack();
            }
        }
    }

    /// Whether any network is known to join.
    bool hasKnown() const MM_NONBLOCKING { return wifi_ && wifi_->knownCount() > 0; }
    /// Start joining the known networks from `from` on, in priority order, false once none is left or the station cannot start.
    bool startSta(uint8_t from = 0) {
        if (!wifi_) return false;
        for (uint8_t i = from; i < wifi_->knownCount(); i++) {
            if (!wifi_->ssidAt(i)[0]) continue;   // a row not named yet
            staId_ = wifi_->idAt(i);
            appliedStaIpSig_ = wifi_->ipSigAt(i);   // what joining applies, so only a later edit re-applies
            std::printf("NetworkModule: WiFi STA trying %s (%u of %u)\n", wifi_->ssidAt(i),
                        static_cast<unsigned>(i + 1), static_cast<unsigned>(wifi_->knownCount()));
            return platform::wifiStaInit(wifi_->ssidAt(i), wifi_->passwordAt(i));
        }
        return false;
    }

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
    /// How often the fallback retries the known networks, long because each attempt moves the radio to a router's channel, knocking the access point's phones off.
    static constexpr uint32_t kApRetryStaMs = 60000;
    bool apUp_ = false;          ///< whether the access point runs
    bool idleForNever_ = false;  ///< the fallback found nothing because the access point never opens
    uint32_t apSig_ = 0;         ///< how it appears as it was opened, so a change re-opens it live
    uint32_t apHoldFrom_ = 0;    ///< when a join asked for from the access point started, 0 when none holds it open
    /// How long the access point stays after a join asked for from it, so the phone follows to the new address.
    static constexpr uint32_t kHandoffMs = 120000;
    // Kept set so the warning outranks the fallback's connected line, until the cable is out.
    bool ethDegraded_ = false;
    // When the link last came up, so a re-plugged cable gets the same window as first boot.
    uint32_t ethLinkUpAt_ = 0;
    bool mdnsRunning_ = false;   ///< whether the local-name service is advertising
    // The name last advertised, so a live rename is noticed and re-registered.
    char lastMdnsName_[24] = {};

    bool mdnsEnabled_ = true;                ///< whether to advertise the local name
    // The storage behind the inherited status slot, which holds only a pointer into it.
    char statusBuf_[80] = {};   // 62 needed by the longest line, the link-local one: a 15-character address, a 23-character device name and .local

    char modeStr_[20] = {};   ///< the mode label, sized to the longest

    // The platform takes quarter-decibels, and the applied value detects a change.
    int16_t appliedTxPowerSetting_ = -1;   ///< negative until the first apply

    // The joined network's IP settings as last applied, so an edit to them re-applies live.
    uint32_t appliedStaIpSig_ = 0;

    /// Apply an edit to the joined network's IP settings live while WiFi carries the device.
    void syncStaIpLive() {
        if constexpr (platform::hasWiFi) {
            if (state_ != State::ConnectedSta || !wifi_) return;   // applied on the next connect
            if (staId_ && wifi_->indexOfId(staId_) < 0) {
                // Forgotten while joined: leave it, as a phone does, and the known networks take over.
                std::printf("NetworkModule: joined network forgotten, leaving it\n");
                platform::mdnsStop();
                platform::wifiStaStop();
                noteRadioStopped();
                staId_ = 0;
                if (startSta()) { state_ = State::WaitingSta; stateChangeTime_ = platform::millis(); syncTxPower(); }
                else fallBack();
                return;
            }
            const uint32_t sig = wifi_->ipSigAt(staIndex());
            if (sig == appliedStaIpSig_) return;   // nothing changed
            appliedStaIpSig_ = sig;
            if (wifi_->staticIpAt(staIndex())) applyStaStatic();
            else platform::netSetDhcp(platform::NetIface::Sta);   // Static → DHCP: re-lease live
            updateStatusIP();   // reflect the new address (static IP, or the re-leased one once it lands)
        }
    }

    /// Nothing joined: open the access point, so a user can reach the device to configure it, unless it never opens.
    void fallBack() {
        const bool never = ap_ && ap_->opens(othersConfigured()) == AccessPointModule::Opens::Never;
        idleForNever_ = never;
        if (!never && openAp()) {
            state_ = State::AP;
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "AP: %s @ %s", readDeviceName(), captive::kAddressText);
            setStatus(statusBuf_, Severity::Status);
        } else {
            // Idle retries the known networks, so a device whose access point never opens still rejoins.
            state_ = State::Idle;
            mm::formatTo(statusBuf_, sizeof(statusBuf_), never ? "No network: retrying, the access point never opens" : "No network");
            setStatus(statusBuf_, never ? Severity::Warning : Severity::Error);
        }
        stateChangeTime_ = platform::millis();
        // The status needs no rebuild, but the radio readouts' visibility does.
        rebuildControls();
        if (scheduler_) scheduler_->prepareTree();
    }

    /// Whether anything but the access point could reach the device, which is what lets it stay closed.
    bool othersConfigured() const MM_NONBLOCKING {
        return hasKnown() || (ethernet_ && ethernet_->configured());
    }

    /// Open the access point beside whatever else runs, true once it is up.
    bool openAp() {
        if (apUp_) return true;
        // The same identity as every other name, so a device shows one everywhere.
        const char* name = readDeviceName();
        const platform::WifiApConfig cfg = ap_ ? ap_->config(name)
                                               : platform::WifiApConfig{name, captive::kAddressText, "", 1, false};
        if (!platform::wifiApInit(cfg)) return false;
        apUp_ = true;
        apSig_ = ap_ ? ap_->sig(name) : 0;
        syncTxPower();  // see setWifiCredentials's syncTxPower comment
        // The address is what a user needs, the name alone sending them looking.
        std::printf("NetworkModule: AP started: %s → join it and open http://%s\n", name, captive::kAddressText);
        // So a phone joining the access point finds the networks in range already listed.
        if (wifi_) wifi_->onAccessPointStarted();
        if (ap_) ap_->started();
        restartMdns();
        return true;
    }

    /// Close the access point, leaving whatever else runs.
    void closeAp() {
        if (!apUp_) return;
        platform::wifiApStop();
        noteRadioStopped();
        apUp_ = false;
        if (ap_) ap_->stopped();
        endHandoff();
        restartMdns();
    }

    /// Re-advertise on the next tick: an access point opening or closing changes the radio's interfaces, and the bench showed the advertisement going silent when the access point closed under it.
    void restartMdns() {
        if (!mdnsRunning_) return;
        platform::mdnsStop();
        mdnsRunning_ = false;   // syncMdns starts it again
    }

    /// The access point no longer holds open for a join asked for from it.
    void endHandoff() {
        apHoldFrom_ = 0;
        if (wifi_) wifi_->endHandoff();
    }

    /// Open or close the access point by its `opens` rule, and re-open it when how it appears changed.
    void syncAccessPoint(uint32_t now) {
        if constexpr (!platform::hasWiFi) return;
        if (!ap_) return;
        const bool others = othersConfigured();
        ap_->advise(others);
        const auto opens = ap_->opens(others);
        const bool connected = (state_ == State::ConnectedEth || state_ == State::ConnectedSta);
        if (!apUp_) {
            if (opens == AccessPointModule::Opens::Always) openAp();
            // Idle only because it never opened: lifting that opens it now, as every setting applies live.
            else if (idleForNever_ && opens == AccessPointModule::Opens::OnFailure) fallBack();
            return;
        }
        const uint32_t clients = platform::wifiApClientCount();
        ap_->showClients(clients);
        // The whole time, whoever is on it: the join's channel move knocks every phone off, and one may come back to read the result.
        if (apHoldFrom_ && now - apHoldFrom_ > kHandoffMs) endHandoff();
        const bool wanted = opens == AccessPointModule::Opens::Always
                         || (opens == AccessPointModule::Opens::OnFailure && (!connected || apHoldFrom_));
        if (!wanted) {
            std::printf("NetworkModule: Shutting down AP (%s)\n", connected ? "higher priority connected" : "it never opens");
            closeAp();
            if (state_ == State::AP) fallBack();   // never chosen while it was the fallback
        } else if (ap_->sig(readDeviceName()) != apSig_) {
            // A new password, channel, name or name visibility applies now, which drops the phones on it to rejoin.
            closeAp();
            openAp();
        }
    }

    /// Retry the known networks now and then from the fallback, while nobody is on the access point.
    void retryKnown(uint32_t now) {
        if (!hasKnown() || now - stateChangeTime_ <= kApRetryStaMs) return;
        if (apUp_ && platform::wifiApClientCount() > 0) return;
        std::printf("NetworkModule: retrying WiFi STA\n");
        stateChangeTime_ = now;   // init refused; wait out another interval
        if (startSta()) {
            state_ = State::WaitingSta;
            syncTxPower();   // see setWifiCredentials's syncTxPower comment
        }
    }

    /// Adopt a connected interface, shutting down whatever it outranks.
    void onConnected(const char* via) {
        if (std::strcmp(via, "Ethernet") == 0) {
            state_ = State::ConnectedEth;
            ethDegraded_ = false;   // Ethernet itself has a usable address, so it is no longer degraded
        } else {
            state_ = State::ConnectedSta;
            // Associated, but the address comes from us, so pin it before the status reads it.
            applyStaStatic();
        }
        stateChangeTime_ = platform::millis();

        // Shut down whatever this outranks; the access point follows its own rule.
        if constexpr (platform::hasWiFi) {
            if (!ap_ && apUp_) closeAp();   // no settings to keep it open by
            if (state_ == State::ConnectedEth && platform::wifiStaConnected()) {
                std::printf("NetworkModule: Shutting down WiFi STA (Ethernet connected)\n");
                platform::wifiStaStop();
                noteRadioStopped();
            }
        }

        updateStatusIP();
        std::printf("NetworkModule: Connected via %s: %s\n", via, statusBuf_);

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
    /// Ethernet has an address the cascade may use: @xref{a-link-local-address-is-a-last-resort}.
    bool ethUp() const MM_NONBLOCKING {
        if (!platform::ethConnected()) return false;
        uint8_t ip[4];
        platform::ethGetIPv4(ip);
        return addressCounts(ip, platform::hasWiFi, ethernet_ ? ethernet_->configuredIp() : nullptr);
    }

    /// The static address of the network being joined, or null where it runs DHCP.
    const uint8_t* configuredIp() const MM_NONBLOCKING {
        return wifi_ ? wifi_->staticIpAt(staIndex()) : nullptr;
    }

    /// The station has an address the cascade may use, by the same rule as Ethernet.
    bool staUp() const MM_NONBLOCKING {
        if constexpr (!platform::hasWiFi) return false;
        if (!platform::wifiStaConnected()) return false;
        uint8_t ip[4];
        platform::wifiStaGetIPv4(ip);
        return addressCounts(ip, platform::hasWiFi, configuredIp());
    }

    /// Pin the static address of the network being joined, or do nothing where it runs DHCP.
    void applyStaStatic() {
        if constexpr (!platform::hasWiFi) return;
        if (wifi_) wifi_->applyStatic(staIndex());
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
        if (state_ == State::ConnectedEth && isLinkLocalIPv4(ip)) {
            // Working, but the address says why it is unusual and the name says how to reach it: @xref{a-link-local-address-is-a-last-resort}.
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "Eth: %s (no DHCP), %s.local", ipStr, readDeviceName());
            setStatus(statusBuf_, Severity::Warning);
            return;
        }
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
        const int16_t txPowerSetting = wifi_ ? wifi_->txPowerSetting() : 0;
        if (txPowerSetting == appliedTxPowerSetting_) return;
        // A genuine no-op, not an optimization: pushing one here boot-loops a device.
        if (txPowerSetting == 0 && appliedTxPowerSetting_ <= 0) {
            appliedTxPowerSetting_ = 0;   // mark synced so we don't re-check every tick
            return;
        }
        const bool radioUp = (state_ == State::ConnectedSta
                              || state_ == State::WaitingSta
                              || apUp_);
        if (!radioUp) return;
        // The platform has no reset call, so lifting a cap pushes the ceiling instead.
        const int8_t quarterDbm = (txPowerSetting == 0)
                                  ? static_cast<int8_t>(80)
                                  : static_cast<int8_t>(txPowerSetting * 4);
        if (platform::wifiSetTxPower(quarterDbm)) {
            appliedTxPowerSetting_ = txPowerSetting;
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
            if (!wifi_) return;
            // Refreshed even while hidden, so a return to a radio state shows no stale value.
            const bool connected = (state_ == State::ConnectedSta);
            const bool radioOn = connected || state_ == State::WaitingSta || apUp_;
            uint8_t ip[4] = {};
            if (connected) platform::wifiStaGetIPv4(ip);
            wifi_->showRadio(connected ? static_cast<int8_t>(platform::wifiStaRssi()) : 0,
                             radioOn ? static_cast<int8_t>(platform::wifiTxPower()) : 0, radioOn, connected,
                             staId_, ip);
        }
    }

};

} // namespace mm
