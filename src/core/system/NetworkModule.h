#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"
#include "core/module/StateDocument.h"   // isFlatConfig: a 6.0 config is a flat file
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

/// All device connectivity, cascading from Ethernet through WiFi to an access point: @xref{the-cascade}.
///
/// Network holds the cascade, its mode and mDNS; each interface's settings are on its own card below it: Ethernet, WiFi and the access point.
/// A desktop builds the same cards over stubs, so they can be shown and tested, and reports no network of its own.
/// @card NetworkModule.png
///
/// @moreinfo
///
/// ## The cascade
///
/// Ethernet is preferred, then WiFi, then the access point; a higher-priority link tears the lower ones down to reclaim memory, each is tried unconditionally, a late one promoted live.
///
/// ## A link-local address is a last resort
///
/// With no DHCP server the client gives itself a 169.254.x.y address (RFC 3927), so a laptop on the same cable still reaches the device by name. It counts as connected only on a build without WiFi, where it is the one way in.
/// Where WiFi exists it never displaces the cascade: a cable on a network without DHCP would otherwise switch off a working WiFi path or the access point.
/// A 169.254 address the user set as static is a choice, not a fallback, so it counts everywhere, while a self-assigned one beside a static setting still does not.
///
/// The device name belongs to the system module, and is the one identity behind every name. It registers before the light pipeline, which then sees the real remaining heap.
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
            if (state_ == State::WaitingSta || state_ == State::ConnectedSta) stopSta();
            // On failure the access point opens, so credentials can be re-entered.
            if (beginSta(platform::millis())) {
                std::snprintf(statusBuf_, sizeof(statusBuf_), "WiFi STA: %s", wifi_->ssidAt(staIndex()));
                setStatus(statusBuf_, Severity::Status);
                // Re-evaluate visibility, or a now-stale signal reading would stay rendered.
                rebuildControls();
            }
        }
    }

    /// Keep the cascade ticking whatever the toggle says, or the device drops off the network.
    bool respectsEnabled() const MM_NONBLOCKING override { return false; }

    /// Bring-up runs once and is not re-entrant, so a restored config applies at the next boot.
    bool appliesConfigLive() const override { return false; }

    /// Set the hostname, push the interface config, then start the cascade.
    void setup() override {
        // Before anything reads the interface settings.
        adoptLegacySettings();
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
            beginSta(platform::millis());
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
        syncStaIpLive(now);
        startRequestedJoin(now);
        if (ethernet_ && ethernet_->syncIpLive(state_ == State::ConnectedEth)) {
            if (ethernet_->configuredIp()) updateStatusIP();
            else beginRelease(now, "Ethernet");   // back to DHCP: the address is gone until the lease lands
        }
        // After a join the card asked for, which restarts the clock, or that join reads as timed out at once.
        const uint32_t elapsed = now - stateChangeTime_;

        switch (state_) {
            case State::WaitingEth: tickWaitingEth(now, elapsed); break;
            case State::WaitingSta: tickWaitingSta(now, elapsed); break;
            case State::ConnectedEth: tickConnectedEth(now); break;
            case State::ConnectedSta: tickConnectedSta(now); break;
            case State::AP: tickAP(now); break;
            case State::Idle: tickIdle(now); break;
        }

        syncAccessPoint(now);
        syncMdns();
        syncTxPower();
        // An interface change applies live where the hardware allows, the cascade then waiting on it again, unless WiFi carries the device, which it leaves alone.
        if (ethernet_ && ethernet_->syncLive() && state_ != State::ConnectedSta && state_ != State::WaitingSta) {
            state_ = State::WaitingEth;
            stateChangeTime_ = now;
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
        mdnsRunning_ = false;   // a prepare after this advertises afresh
        if constexpr (platform::hasWiFi) {
            closeAp();
            if (state_ == State::ConnectedSta || state_ == State::WaitingSta) stopSta();
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
    /// The first known network's password, which the access point carries: stable while the station tries one network after another.
    const char* firstNetworkPassword() const MM_NONBLOCKING { return wifi_ ? wifi_->passwordAt(0) : ""; }
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
                restartMdns();
                stopSta();
            }
            startedSeq_ = req->seq;
            staId_ = req->knownId;
            appliedStaIp_.mark(wifi_->ipAt(staIndex()));
            std::printf("NetworkModule: WiFi STA joining %s, asked for from the card\n", req->ssid);
            if (joinNetwork(req->ssid, req->password)) {
                enterWaitingSta(now);
            } else {
                wifi_->joinFailed(platform::WifiFailure::Other);
                fallBack(now);
            }
        }
    }

    /// Waiting on Ethernet: connected once it has an address, else on to WiFi, or on polling where there is no WiFi.
    void tickWaitingEth(uint32_t now, uint32_t elapsed) {
        // A static address needs no lease, so pin it as soon as the link is up.
        if (ethernet_ && platform::ethLinkUp() && !ethUp()) ethernet_->applyStatic();
        if (ethUp()) onConnected(State::ConnectedEth, now);
        // No link at all, or a link with no address, the second being remembered.
        else if ((elapsed > 3000 && !platform::ethLinkUp()) || elapsed > kDhcpWaitMs) giveUpOnEth(now);
    }

    /// Ethernet found no link, or no address, in its window: say which, then cascade on to WiFi, or keep polling where there is none.
    void giveUpOnEth(uint32_t now) {
        ethDegraded_ = platform::ethLinkUp();
        if (ethDegraded_) {
            writeEthDegradedStatus();
        } else {
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "Ethernet not detected: no cable/link");
            setStatus(statusBuf_, Severity::Warning);
        }
        if constexpr (platform::hasWiFi) {
            std::printf("NetworkModule: Ethernet %s, cascading\n", ethDegraded_ ? "no IP (DHCP timeout)" : "no link (no cable)");
            beginSta(now);
        } else {
            // No fallback here, so keep polling for a cable.
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "No network (Ethernet only)"); setStatus(statusBuf_, Severity::Error);
            stateChangeTime_ = now;
        }
    }

    /// Waiting on the station: connected once it has an address, else the next known network or the fallback.
    void tickWaitingSta(uint32_t now, uint32_t elapsed) {
        if constexpr (!platform::hasWiFi) return;
        // Pinned during bring-up, since a network without a server fires no event.
        if (!staUp()) applyStaStatic();
        if (staUp()) { onStaJoined(now); return; }
        if (elapsed <= kStaGraceMs) return;
        // A join asked for from the card says why there, and the known networks take over from the top; otherwise the next one is tried, then the fallback.
        const bool asked = wifi_ && wifi_->joinRequest();
        if (asked) {
            wifi_->joinFailed(platform::wifiStaLastFailure());
            // A new round, with the network that just failed already tried, as a phone does not retry it at once.
            triedCount_ = 0;
            if (staId_) tried_[triedCount_++] = staId_;
        }
        stopSta();
        beginSta(now, true);
    }

    /// The station has an address: adopt it, and send a phone that asked from the access point on to the new address.
    void onStaJoined(uint32_t now) {
        // A join asked for from the card succeeded: a scanned network is now known, at the top.
        if (wifi_ && wifi_->joinRequest()) {
            staId_ = wifi_->joinSucceeded();
            appliedStaIp_.mark(wifi_->ipAt(staIndex()));
        }
        onConnected(State::ConnectedSta, now);
        if (!apHoldFrom_ || !wifi_) return;
        uint8_t ip[4];
        platform::wifiStaGetIPv4(ip);
        wifi_->showHandoff(ip);
    }

    /// Ethernet carries the device: on to WiFi when it drops.
    void tickConnectedEth(uint32_t now) {
        if (!ethUp() && releasing_ && now - lostTime_ <= kDhcpWaitMs) return;   // the lease the device asked for
        releasing_ = false;
        if (!ethUp()) {
            if constexpr (platform::hasWiFi) {
                std::printf("NetworkModule: Ethernet dropped, cascading\n");
                restartMdns();
                beginSta(now);
            } else {
                // Drop back to polling for the cable.
                std::printf("NetworkModule: Ethernet dropped\n");
                restartMdns();
                mm::formatTo(statusBuf_, sizeof(statusBuf_), "No network (Ethernet only)"); setStatus(statusBuf_, Severity::Error);
                state_ = State::WaitingEth;
                stateChangeTime_ = now;
            }
        }
        updateStatusIP();
    }

    /// WiFi carries the device: Ethernet takes over once it works, and a dropout gets its grace before the fallback.
    void tickConnectedSta(uint32_t now) {
        if constexpr (!platform::hasWiFi) return;
        if (ethernetTakesOver(now)) return;
        // Checked whatever the cable does, since the station is what carries the device here.
        if (staUp()) {
            lostTime_ = 0;   // reconnected in time, so back to normal
            releasing_ = false;
            updateStatusIP();
        } else {
            watchStaDropout(now);
        }
    }

    /// Ethernet outranks WiFi, but only once it works: true when it took over, and a link that stays without an address is flagged.
    bool ethernetTakesOver(uint32_t now) {
        if (ethernet_ && platform::ethLinkUp() && !ethUp()) ethernet_->applyStatic();
        if (ethUp()) {
            std::printf("NetworkModule: Ethernet up, switching from WiFi STA\n");
            restartMdns();
            onConnected(State::ConnectedEth, now);
            return true;
        }
        if (!platform::ethLinkUp()) return false;
        // Already being retried, so only a persistently addressless link is flagged.
        if (ethLinkUpAt_ == 0) ethLinkUpAt_ = now;   // link just (re)appeared: start the clock
        if (!ethDegraded_ && now - ethLinkUpAt_ > kDhcpWaitMs) {
            ethDegraded_ = true;
            writeEthDegradedStatus();
        }
        return false;
    }

    /// A dropout is not a divorce: the radio reconnects itself within seconds, so the fallback opens only once the grace runs out.
    void watchStaDropout(uint32_t now) {
        if (lostTime_ == 0) {
            lostTime_ = now;
            std::printf("NetworkModule: WiFi STA dropped, reconnecting\n");
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "WiFi reconnecting…");
            setStatus(statusBuf_, Severity::Warning);
            return;
        }
        const uint32_t grace = releasing_ ? kDhcpWaitMs : kStaGraceMs;
        if (now - lostTime_ <= grace) return;
        std::printf("NetworkModule: WiFi STA gone for %us, starting AP\n", static_cast<unsigned>(grace / 1000));
        restartMdns();
        stopSta();
        lostTime_ = 0;
        releasing_ = false;
        fallBack(now);
    }

    /// The fallback: promote to whatever connects, and retry the known networks now and then.
    void tickAP(uint32_t now) {
        if constexpr (platform::hasWiFi) {
            // Promote as soon as something better appears.
            if (ethUp()) {
                onConnected(State::ConnectedEth, now);
            } else if (hasKnown() && staUp()) {
                onConnected(State::ConnectedSta, now);
            } else {
                retryKnown(now);
            }
        }
    }

    /// Every path failed: a late interface still promotes, and the known networks are retried.
    void tickIdle(uint32_t now) {
        // Every path failed, but the stack runs on, so a late interface still promotes.
        if (ethUp()) {
            std::printf("NetworkModule: Ethernet up (recovered from Idle)\n");
            onConnected(State::ConnectedEth, now);
        } else if constexpr (platform::hasWiFi) {
            if (staUp()) {
                std::printf("NetworkModule: WiFi STA up (recovered from Idle)\n");
                onConnected(State::ConnectedSta, now);
            } else {
                retryKnown(now);
            }
        }
    }

    /// Whether any network is known to join.
    bool hasKnown() const MM_NONBLOCKING { return wifi_ && wifi_->knownCount() > 0; }
    /// Join the first network in the join order not tried this round, a new round unless `next`, since a scan between attempts reorders what is left.
    bool startSta(uint32_t now, bool next = false) {
        if (!wifi_) return false;
        if (!next) triedCount_ = 0;
        uint8_t order[WiFiModule::kMaxKnown];
        const uint8_t n = wifi_->joinOrder(order, now);
        for (uint8_t k = 0; k < n; k++) {
            const uint8_t i = order[k];
            if (!wifi_->ssidAt(i)[0] || triedThisRound(wifi_->idAt(i))) continue;   // unnamed, or already tried
            if (triedCount_ < WiFiModule::kMaxKnown) tried_[triedCount_++] = wifi_->idAt(i);
            staId_ = wifi_->idAt(i);
            appliedStaIp_.mark(wifi_->ipAt(i));   // what joining applies, so only a later edit re-applies
            std::printf("NetworkModule: WiFi STA trying %s (%u of %u)\n", wifi_->ssidAt(i),
                        static_cast<unsigned>(i + 1), static_cast<unsigned>(wifi_->knownCount()));
            return joinNetwork(wifi_->ssidAt(i), wifi_->passwordAt(i));
        }
        return false;
    }

    // A pinned address stays on the station until DHCP is restored, so a DHCP network joined after a Static one came up on the Static one's address.
    /// Start joining `ssid` with the addressing of the network `staId_` names, Static or DHCP, so no earlier network's setting carries over.
    bool joinNetwork(const char* ssid, const char* password) {
        if (!platform::wifiStaInit(ssid, password)) return false;
        wifi_->ipAt(staIndex()).applyLive(platform::NetIface::Sta);
        return true;
    }

    /// The station has started joining: wait for it, its power cap set before the radio's first burst, the window the cap protects.
    void enterWaitingSta(uint32_t now) {
        state_ = State::WaitingSta;
        stateChangeTime_ = now;
        syncTxPower();
    }

    /// Join the known networks in their join order, continuing the round when `next`, or open the fallback when none starts; true when one is joining.
    bool beginSta(uint32_t now, bool next = false) {
        if (startSta(now, next)) { enterWaitingSta(now); return true; }
        fallBack(now);
        return false;
    }

    /// Stop the station, noting the radio stopped with it.
    void stopSta() {
        platform::wifiStaStop();
        noteRadioStopped();
    }

    enum class State : uint8_t {
        Idle,
        WaitingEth,
        WaitingSta,
        ConnectedEth,
        ConnectedSta,
        AP
    };

    /// The known networks tried in this round, by row id.
    uint32_t tried_[WiFiModule::kMaxKnown] = {};
    uint8_t triedCount_ = 0;
    bool triedThisRound(uint32_t id) const {
        for (uint8_t k = 0; k < triedCount_; k++) if (tried_[k] == id) return true;
        return false;
    }

    State state_ = State::Idle;
    uint32_t stateChangeTime_ = 0;
    /// When the carrying interface lost its address: a WiFi dropout the radio recovers from itself, or a re-lease.
    uint32_t lostTime_ = 0;
    /// How long WiFi gets to connect or recover, the question being the same in both cases.
    static constexpr uint32_t kStaGraceMs = 10000;
    /// How long a lease may take: a live Ethernet link without an address, or an interface re-leasing after a switch to DHCP.
    static constexpr uint32_t kDhcpWaitMs = 15000;
    /// The interface carrying the device is taking a lease the cascade itself asked for, so the gap is that wait rather than a dropout.
    bool releasing_ = false;

    /// The carrying interface went back to DHCP: its address is gone until the router leases one, which the connected tick waits for.
    void beginRelease(uint32_t now, const char* interface) {
        releasing_ = true;
        lostTime_ = now;   // the tick's own time, so the clock never runs from a later reading
        mm::formatTo(statusBuf_, sizeof(statusBuf_), "%s taking an address from the router…", interface);
        setStatus(statusBuf_, Severity::Status);
    }
    /// How often the fallback retries the known networks, long because each attempt moves the radio to a router's channel, knocking the access point's phones off.
    static constexpr uint32_t kApRetryStaMs = 60000;
    bool apUp_ = false;          ///< whether the access point runs
    bool idleAfterSetup_ = false;  ///< the fallback found nothing because the access point was for the first setup only
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
    IpApplied appliedStaIp_;

    /// Apply an edit to the joined network's IP settings live while WiFi carries the device.
    void syncStaIpLive(uint32_t now) {
        if constexpr (platform::hasWiFi) {
            if (state_ != State::ConnectedSta || !wifi_) return;   // applied on the next connect
            if (staId_ && wifi_->indexOfId(staId_) < 0) {
                // Forgotten while joined: leave it, as a phone does, and the known networks take over.
                std::printf("NetworkModule: joined network forgotten, leaving it\n");
                restartMdns();
                stopSta();
                staId_ = 0;
                beginSta(now);
                return;
            }
            if (!appliedStaIp_.changedTo(wifi_->ipAt(staIndex()))) return;   // nothing changed
            const IpSettings& ip = wifi_->ipAt(staIndex());
            ip.applyLive(platform::NetIface::Sta);
            // Back to DHCP drops the address until the router leases one, which the dropout clock then waits for.
            if (!ip.usable()) { beginRelease(now, "WiFi"); return; }
            updateStatusIP();   // reflect the new static address
        }
    }

    /// Nothing joined: open the access point, so a user can reach the device to configure it, unless it was for the first setup only.
    void fallBack(uint32_t now) {
        const bool closed = ap_ && ap_->opens(othersConfigured()) == AccessPointModule::Opens::FirstSetup;
        idleAfterSetup_ = closed;
        if (!closed && openAp()) {
            state_ = State::AP;
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "AP: %s @ %s", ap_ ? ap_->name() : readDeviceName(), captive::kAddressText);
            setStatus(statusBuf_, Severity::Status);
        } else {
            // Idle retries the known networks, so a device whose access point stays closed still rejoins.
            state_ = State::Idle;
            mm::formatTo(statusBuf_, sizeof(statusBuf_), closed ? "No network: retrying, the access point was for the first setup only" : "No network");
            setStatus(statusBuf_, closed ? Severity::Warning : Severity::Error);
        }
        stateChangeTime_ = now;
        // The status needs no rebuild, but the radio readouts' visibility does.
        rebuildControls();
        if (scheduler_) scheduler_->requestPrepareTree();   // at the frame boundary, as every other rebuild
    }

    /// Move the Ethernet and WiFi settings a 6.0 config keeps at Network's top level onto their cards, once; temporary, see MIGRATING.
    void adoptLegacySettings() {
        char path[FilesystemModule::MAX_PATH];
        if (!FilesystemModule::pathFor(this, path, sizeof(path))) return;
        char* json = FilesystemModule::readWholeFile(path);
        if (!json) return;
        // Only a flat file is a 6.0 config; a state document holds the same names inside its children.
        const bool flat = isFlatConfig(json);
        const bool eth = flat && ethernet_ && ethernet_->adoptLegacy(json);
        const bool sta = flat && wifi_ && wifi_->adoptLegacy(json);
        platform::free(json);
        if (!eth && !sta) return;
        std::printf("NetworkModule: moved 6.0's network settings onto their cards\n");
        markDirty();
        FilesystemModule::noteDirty();
    }

    /// Whether anything but the access point could reach the device, which is what lets it stay closed.
    bool othersConfigured() const MM_NONBLOCKING {
        return hasKnown() || (ethernet_ && ethernet_->configured());
    }

    /// Open the access point beside whatever else runs, true once it is up.
    bool openAp() {
        if (apUp_) return true;
        const platform::WifiApConfig cfg = ap_ ? ap_->config(firstNetworkPassword())
                                               : platform::WifiApConfig{readDeviceName(), captive::kAddressText, ""};
        if (!platform::wifiApInit(cfg)) return false;
        apUp_ = true;
        apSig_ = ap_ ? ap_->sig(firstNetworkPassword()) : 0;
        syncTxPower();  // see setWifiCredentials's syncTxPower comment
        // The address is what a user needs, the name alone sending them looking.
        std::printf("NetworkModule: AP started: %s → join it and open http://%s\n", cfg.name, captive::kAddressText);
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

    // The one way to stop it: a stop that left mdnsRunning_ set kept syncMdns from ever advertising again, so a device lost its .local name after any network change.
    /// Stop advertising now and on the next connected tick again: every change of interface, an access point opening or closing, a join or a dropped link, goes through here.
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
        const auto opens = ap_->opens(others);
        const bool connected = (state_ == State::ConnectedEth || state_ == State::ConnectedSta);
        if (!apUp_) {
            // Not while a station join is in progress: one radio, so opening moves the channel under the join, and the first join after boot failed that way on the bench.
            if (opens == AccessPointModule::Opens::Always && state_ != State::WaitingSta) openAp();
            // Idle only because it stayed closed: lifting that opens it now, as every setting applies live.
            else if (idleAfterSetup_ && opens == AccessPointModule::Opens::OnFailure) fallBack(now);
            return;
        }
        const uint32_t clients = platform::wifiApClientCount();
        ap_->showClients(clients);
        // The whole time, whoever is on it: the join's channel move knocks every phone off, and one may come back to read the result.
        if (apHoldFrom_ && now - apHoldFrom_ > kHandoffMs) endHandoff();
        const bool wanted = opens == AccessPointModule::Opens::Always
                         || (opens == AccessPointModule::Opens::OnFailure && (!connected || apHoldFrom_));
        if (!wanted) {
            std::printf("NetworkModule: Shutting down AP (%s)\n", connected ? "higher priority connected" : "it was for the first setup only");
            closeAp();
            if (state_ == State::AP) fallBack(now);   // never chosen while it was the fallback
        } else if (ap_->sig(firstNetworkPassword()) != apSig_) {
            // A new password or name applies now, which drops the phones on it to rejoin.
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
        if (startSta(now)) enterWaitingSta(now);
    }

    /// Adopt a connected interface, ConnectedEth or ConnectedSta, shutting down whatever it outranks.
    void onConnected(State to, uint32_t now) {
        idleAfterSetup_ = false;   // connected, so idle for want of an access point is over
        state_ = to;
        // A fresh connection, so a dropout or re-lease clock from before it never cuts a later grace short.
        lostTime_ = 0;
        releasing_ = false;
        if (to == State::ConnectedEth) {
            ethDegraded_ = false;   // Ethernet itself has a usable address, so it is no longer degraded
        } else {
            // Associated, but the address comes from us, so pin it before the status reads it.
            applyStaStatic();
        }
        stateChangeTime_ = now;

        // Shut down whatever this outranks; the access point follows its own rule.
        if constexpr (platform::hasWiFi) {
            if (!ap_ && apUp_) closeAp();   // no settings to keep it open by
            if (state_ == State::ConnectedEth && platform::wifiStaConnected()) {
                std::printf("NetworkModule: Shutting down WiFi STA (Ethernet connected)\n");
                stopSta();
            }
        }

        updateStatusIP();
        std::printf("NetworkModule: Connected via %s: %s\n", to == State::ConnectedEth ? "Ethernet" : "WiFi STA", statusBuf_);

        syncMdns();

        // Again for the radio readouts' visibility, which depends on the state.
        rebuildControls();
        if (scheduler_) scheduler_->requestPrepareTree();
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

    /// Pin the static address of the network being joined again during bring-up; joinNetwork already set its addressing, DHCP included.
    void applyStaStatic() {
        if constexpr (!platform::hasWiFi) return;
        if (wifi_) wifi_->ipAt(staIndex()).applyStatic(platform::NetIface::Sta);
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
            restartMdns();
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
