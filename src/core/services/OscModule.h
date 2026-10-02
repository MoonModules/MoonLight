#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/system/ControlModule.h"
#include "core/util/ControlSurface.h"
#include "core/module/MoonModule.h"
#include "core/services/OscPacket.h"
#include "core/util/Addressing.h"      // sendAddressed: feedback to hosts, a group, or everyone
#include "core/util/HostList.h"        // parseHostList: the feedback hosts
#include "core/util/HostResolver.h"    // a named host's address, looked up off the render thread
#include "core/module/Scheduler.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm {

/// Receives OSC over the network and writes it onto this device's controls.
///
/// A surface addresses the surface: the faders, encoders and switches the control module owns, which decides what each one drives.
/// @card OscModule.png
///
/// @moreinfo
///
/// ## Feedback
///
/// With it on, a control that changes anywhere is mirrored back to the surface, which keeps a client honest and moves a motorized fader.
/// `addressing` picks where it goes: `unicast` to each of `hosts`, or to whoever last wrote when the list is empty; `multicast` to `group`, which every board listening on it joins.
/// A client learns the state three ways: on its first write, on an address change, and on asking.
/// The last exists because a restart on the same address is invisible to the other two.
/// Most controllers send nothing on load, so every widget would otherwise show its own defaults.
///
/// ## What it refuses
///
/// The port is unauthenticated and writes controls, so it is off until turned on.
/// Feedback is off for the same reason, sending unasked-for traffic to whatever last wrote.
/// OSC control ingest: a fader move in another application becomes a control write here.
///
/// @moreinfo
///
/// It owns no surface of its own.
/// The control module already has the pads, encoders and faders, and everything here lands in the same control-set primitive the API and the UI use.
///
/// So OSC gains no privilege, every validator still runs, and there is no second copy of state.
/// Sending OSC, bundles and address wildcards are all out of scope.
class OscModule : public MoonModule, public ControlSurface {
public:
    /// A service, so the container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// Whether to receive at all, off by default since the port is unauthenticated.
    bool enabledOsc = false;
    /// The port we listen on.
    uint16_t port = osc::kDefaultPort;

    /// Whether to mirror changes back, which is what makes this a surface rather than a remote.
    bool feedback = false;
    /// How feedback travels, an Addressing value up to multicast + broadcast.
    uint8_t addressing = 0;
    /// Where the client listens, which is not where we do.
    uint16_t feedbackPort = 9001;

    /// Declare the receive settings and the feedback settings.
    void defineControls() override {
        MoonModule::defineControls();
        controls_.addControl("listen", enabledOsc);
        controls_.addControl("port", port, 1, 65535);
        controls_.addControl("feedback", feedback);
        controls_.addSelect("addressing", addressing, kAddressingNames, kModeCount);
        controls_.addText("hosts", hosts_, sizeof(hosts_));
        controls_.setHidden(controls_.count() - 1, addressing != 0);
        controls_.addText("group", group_, sizeof(group_));
        controls_.addControl("feedbackPort", feedbackPort, 1, 65535);
    }

    /// Reopen the socket on a receive change, and re-seed the client on a feedback change.
    void onControlChanged(const char* name) override {
        // The setting applies live: the socket reopens, joining a new group, rather than waiting for a reboot.
        if (std::strcmp(name, "port") == 0 || std::strcmp(name, "listen") == 0 || std::strcmp(name, "group") == 0)
            closeSocket();
        if (std::strcmp(name, "hosts") == 0) parseHosts(/*mayStart=*/true);
        if (std::strcmp(name, "group") == 0) parseGroup();
        if (std::strcmp(name, "addressing") == 0 || std::strcmp(name, "group") == 0 || std::strcmp(name, "hosts") == 0)
            reportPeer();
        // A receiver pointed here anew knows nothing, and would stay wrong until something moved.
        if (std::strcmp(name, "feedback") == 0 || std::strcmp(name, "addressing") == 0 || std::strcmp(name, "hosts") == 0
            || std::strcmp(name, "group") == 0 || std::strcmp(name, "feedbackPort") == 0) {
            if (feedback) resendAll_ = true;
        }
    }

    /// Parse the saved hosts and group, starting the name lookups here where allocating is allowed.
    void prepare() override {
        parseHosts(/*mayStart=*/true);
        parseGroup();
    }

    /// Detach from the surface list before closing, since it is walked from the render thread.
    void release() override {
        if (auto* c = ControlModule::active()) c->removeSurface(this);
        attached_ = false;
        closeSocket();
    }

    // Only the value verb is implemented, the others keeping their no-op defaults.

    /// Mirror one control's value back to the client, as the float an application expects.
    void sendValue(SurfaceControl kind, uint8_t index, uint8_t value) override {
        if (!feedback || !open_) return;
        const char* bank = kind == SurfaceControl::Fader   ? "fader"
                         : kind == SurfaceControl::Encoder ? "encoder"
                         : kind == SurfaceControl::Switch  ? "switch" : nullptr;
        if (!bank) return;                       // a pad has no address yet
        char addr[32];
        std::snprintf(addr, sizeof(addr), "/mm/%s/%u", bank, static_cast<unsigned>(index) + 1u);
        uint8_t pkt[64];
        // The inverse of what the read path does.
        const size_t len = osc::encodeFloat(pkt, sizeof(pkt), addr, static_cast<float>(value) / 255.0f);
        if (len == 0) return;
        if (!hostsParsed_) parseHosts(/*mayStart=*/false);
        const auto send = [&](const uint8_t ip[4]) { sock_.sendToAddr(ip, feedbackPort, pkt, len); };
        const Addressing mode = static_cast<Addressing>(addressing < kModeCount ? addressing : 0);
        if (mode == Addressing::Unicast && hostCount_ == 0) {
            // No hosts: answer whoever wrote last.
            if (peer_[0] || peer_[1] || peer_[2] || peer_[3]) send(peer_);
            return;
        }
        if (mode != Addressing::Unicast && !groupValid_) return;   // no group, nowhere to send; the status says so
        sendAddressed(mode, Traffic::Occasional, hostList_, hostCount_, groupIp_, send);
    }

    /// Refresh the status, which is time-dependent because a peer goes stale.
    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        // Only when the answer changes, since the string is identical in between.
        if (!enabledOsc) return;
        refreshNamedHosts();
        const bool fresh = peerFresh();
        if (fresh != peerWasFresh_) { peerWasFresh_ = fresh; reportPeer(); }
    }

    /// Drain whatever arrived, learning where to answer, then re-seed a new client once.
    void tick() MM_NONBLOCKING override {
        if constexpr (!platform::hasNetwork) return;
        if (!enabledOsc) { if (open_) closeSocket(); return; }
        if (!ensureSocket()) return;

        // Bounded: a desk moves a handful of controls per frame, so a cap keeps a flood out.
        uint8_t pkt[kMaxPacket];
        for (int i = 0; i < kMaxPerTick; i++) {
            uint8_t src[4] = {};
            const int n = sock_.recvFrom(pkt, sizeof(pkt), src);
            if (n <= 0) break;                       // -1 = nothing pending
            // A controller that wrote to us is reachable, and a new one knows no values yet.
            lastRecvMs_ = platform::millis();
            if (std::memcmp(peer_, src, 4) != 0) {
                std::memcpy(peer_, src, 4);
                resendAll_ = true;
                peerWasFresh_ = false;   // a new address: let the next tick1s say so
            }
            handle(pkt, static_cast<size_t>(n));
        }
        // After the drain, so a burst re-seeds once rather than per packet.
        if (resendAll_) {
            resendAll_ = false;
            if (auto* c = ControlModule::active()) c->resendTo(this);
        }
    }

private:
    static constexpr long   kSurfaceWidth = 8;    ///< how wide the surface is
    static constexpr size_t kMaxPacket   = 256;   ///< an address plus a few arguments
    static constexpr int    kMaxPerTick  = 16;    ///< the drain's own bound
    static constexpr uint32_t kOpenRetryMs = 2000;   ///< how long a failed open waits

    /// Route one datagram, an unknown address being ignored rather than reported.
    void handle(const uint8_t* pkt, size_t len) {
        osc::Message m;
        if (!osc::parse(pkt, len, m)) return;
        const char* a = m.address;

        if (std::strncmp(a, "/mm/fader/", 10) == 0) {
            writeSurface("fader", a + 10, m);
        } else if (std::strncmp(a, "/mm/encoder/", 12) == 0) {
            writeSurface("encoder", a + 12, m);
        } else if (std::strncmp(a, "/mm/switch/", 11) == 0) {
            // A boolean rather than a byte, since any nonzero value means on.
            writeSurface("switch", a + 11, m, /*asBool=*/true);
        } else if (std::strcmp(a, "/mm/hello") == 0) {
            // A client restarting on the same address is invisible to the checks above.
            resendAll_ = true;
        }
        received_++;
    }

    /// Write one of the surface's own controls, so it reacts exactly as it does to a click.
    void writeSurface(const char* prefix, const char* indexText, const osc::Message& m,
                      bool asBool = false) {
        // Unvalidated input, so a parse that cannot tell zero from junk will not do.
        char* end = nullptr;
        const long idx = std::strtol(indexText, &end, 10);
        if (end == indexText || *end != '\0') return;
        if (idx < 1 || idx > kSurfaceWidth) return;  // anything wider is not ours
        char control[16];
        mm::formatTo(control, sizeof(control), "%s%ld", prefix, idx);
        // The raw value, since scaling would round a small positive one down to off.
        if (asBool) setBool(control, osc::isTruthy(m));
        else        setValue(control, osc::toByte(m));
    }

    /// Write a boolean surface control, whose body is the literal rather than a number.
    void setBool(const char* control, bool on) {
        auto* sched = Scheduler::instance();
        if (!sched) return;
        char body[32];
        std::snprintf(body, sizeof(body), "{\"value\":%s}", on ? "true" : "false");
        sched->setControl(kSurfaceModule, control, body);
    }

    /// Write a numeric surface control through the shared primitive.
    void setValue(const char* control, uint8_t value) {
        auto* sched = Scheduler::instance();
        if (!sched) return;
        char body[32];
        mm::formatTo(body, sizeof(body), "{\"value\":%u}", static_cast<unsigned>(value));
        sched->setControl(kSurfaceModule, control, body);
    }

    /// Open and bind, deferred to the tick and throttled, so a busy port costs one socket.
    bool ensureSocket() {
        if (open_) return true;
        if (!platform::networkReady()) return false;
        const uint32_t now = platform::millis();
        if (lastFailMs_ != 0 && now - lastFailMs_ < kOpenRetryMs) return false;
        if (sock_.open() && sock_.bind(port)) {
            open_ = true;
            // Join the group a sender multicasts feedback to; a failed join leaves unicast working.
            if (groupValid_) sock_.joinMulticast(group_);
            if (!attached_) {
                if (auto* c = ControlModule::active()) { c->addSurface(this); attached_ = true; }
            }
            lastFailMs_ = 0;
            reportPeer();
            return true;
        }
        sock_.close();
        lastFailMs_ = now == 0 ? 1 : now;
        // A real failure rather than a note, since nothing will ever arrive.
        setStatusf(Severity::Error, "port %u busy", static_cast<unsigned>(port));
        return false;
    }

    /// Close the socket and report the resulting state.
    void closeSocket() {
        if (open_) sock_.close();
        open_ = false;
        lastFailMs_ = 0;
        setStatus(enabledOsc ? "opening" : "off");
    }

    /// The modes OSC offers, the first three of Addressing.
    static constexpr uint8_t kModeCount = 3;
    /// How many hosts feedback reaches by unicast: a few boards, not a wall.
    static constexpr uint8_t kMaxHosts = 8;

    /// Parse the host list, an invalid one sending to nobody rather than to a guess, with the parser's reason on the status.
    void parseHosts(bool mayStart) {
        hostsParsed_ = true;
        hostsError_ = parseHostList(hosts_, hostList_, kMaxHosts, hostCount_);
        if (hostsError_) { hostCount_ = 0; return; }
        if (!mayStart) return;
        for (uint8_t i = 0; i < hostCount_; i++)
            if (hostList_[i].isName()) { HostResolver::ensureStarted(); break; }
        refreshNamedHosts();
    }

    /// Parse the group once, rather than on every feedback message.
    void parseGroup() { groupValid_ = parseDottedQuad(group_, groupIp_); }

    /// Copy each named host's latest address in from the resolver, keeping the last one while it is busy or failing.
    void refreshNamedHosts() {
        for (uint8_t i = 0; i < hostCount_; i++) {
            if (!hostList_[i].isName()) continue;
            char name[kMaxHostName + 1];
            hostName(hosts_, hostList_[i], name);
            uint8_t ip[4];
            const HostResolver::State st = HostResolver::lookup(name, ip);
            if (st == HostResolver::State::Resolved || st == HostResolver::State::Stale) std::memcpy(hostList_[i].ip, ip, 4);
        }
    }

    char     hosts_[64] = {};         ///< where unicast feedback goes, empty meaning whoever wrote to us
    char     group_[16] = "239.255.77.78";   ///< the multicast group feedback goes to and this board joins, beside discovery's 239.255.77.77
    Host     hostList_[kMaxHosts] = {};
    uint8_t  hostCount_ = 0;
    bool     hostsParsed_ = false;    ///< the list is parsed on first use and after every edit
    const char* hostsError_ = nullptr;   ///< why the host list was refused, shown on the status
    uint8_t  groupIp_[4] = {};
    bool     groupValid_ = false;

    /// Report the port and who last reached us, a quiet peer being no peer at all, or the group multicast feedback lacks.
    void reportPeer() {
        if (feedback && addressing == 0 && hostsError_)
            setStatusf(Severity::Error, "hosts: %s", hostsError_);
        else if (feedback && addressing != 0 && !groupValid_)
            setStatusf(Severity::Warning, "multicast needs a group address");
        else if (peerFresh())
            setStatusf(Severity::Status, "%u from %u.%u.%u.%u", static_cast<unsigned>(port),
                       peer_[0], peer_[1], peer_[2], peer_[3]);
        else
            setStatusf(Severity::Status, "listening on %u", static_cast<unsigned>(port));
    }

    /// Whether a client is still talking to us.
    bool peerFresh() const {
        return lastRecvMs_ != 0 && platform::millis() - lastRecvMs_ < kPeerStaleMs;
    }
    /// Long enough to survive an idle controller, short enough to stop claiming one that left.
    static constexpr uint32_t kPeerStaleMs = 5000;

    uint8_t  peer_[4] = {};          ///< the last source address, learned in tick()
    uint32_t lastRecvMs_ = 0;        ///< when we last heard from it, so the status can go stale
    bool     peerWasFresh_ = false;  ///< what the status last said, so it is rewritten only on a change
    bool     attached_ = false;    ///< whether we are on the surface list
    bool     resendAll_ = false;   ///< a new peer appeared, so push every value once
    platform::UdpSocket sock_;     ///< the receive socket, which also sends feedback
    bool     open_ = false;        ///< whether it is bound
    uint32_t lastFailMs_ = 0;      ///< when an open last failed, which throttles the retry
    uint32_t received_ = 0;        ///< how many datagrams have been routed
    /// The status text, which the slot borrows rather than copies.
    char     statusStr_[32] = "off";

    /// Format into that buffer and report it.
    void setStatusf(Severity sev, const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(statusStr_, sizeof(statusStr_), fmt, ap);
        va_end(ap);
        setStatus(statusStr_, sev);
    }
};

} // namespace mm
