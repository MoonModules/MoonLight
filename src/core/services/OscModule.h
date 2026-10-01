#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/system/ControlModule.h"
#include "core/util/ControlSurface.h"
#include "core/module/MoonModule.h"
#include "core/services/OscPacket.h"
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
/// With it on, a control that changes anywhere is mirrored back to the surface.
/// That is what keeps a client honest, and what moves a motorised fader.
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
    /// Where the client listens, which is not where we do.
    uint16_t feedbackPort = 9001;

    /// Declare the receive settings and the feedback settings.
    void defineControls() override {
        MoonModule::defineControls();
        controls_.addControl("listen", enabledOsc);
        controls_.addControl("port", port, 1, 65535);
        controls_.addControl("feedback", feedback);
        controls_.addText("feedbackTo", feedbackTo_, sizeof(feedbackTo_));
        controls_.addControl("feedbackPort", feedbackPort, 1, 65535);
    }

    /// Reopen the socket on a receive change, and re-seed the client on a feedback change.
    void onControlChanged(const char* name) override {
        // The setting applies live, so the socket reopens rather than waiting for a reboot.
        if (std::strcmp(name, "port") == 0 || std::strcmp(name, "listen") == 0) closeSocket();
        // A receiver pointed here anew knows nothing, and would stay wrong until something moved.
        if (std::strcmp(name, "feedback") == 0 || std::strcmp(name, "feedbackTo") == 0
            || std::strcmp(name, "feedbackPort") == 0) {
            if (feedback) resendAll_ = true;
        }
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
        uint8_t dest[4];
        if (!feedbackDest(dest)) return;
        sock_.sendToAddr(dest, feedbackPort, pkt, len);
    }

    /// Refresh the status, which is time-dependent because a peer goes stale.
    void tick1s() MM_NONBLOCKING override {
        MoonModule::tick1s();
        // Only when the answer changes, since the string is identical in between.
        if (!enabledOsc) return;
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
                // Persisted, so a rig survives a reboot; only an empty field is filled.
                if (!feedbackTo_[0]) {
                    mm::formatTo(feedbackTo_, sizeof(feedbackTo_), "%u.%u.%u.%u",
                                  src[0], src[1], src[2], src[3]);
                    markDirty();
                    FilesystemModule::noteDirty();
                }
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

    /// Where feedback goes: the configured address, else the last peer, else nowhere.
    bool feedbackDest(uint8_t out[4]) const {
        unsigned a, b, c, d;
        int used = 0;
        // Trailing junk is rejected, since sending to a mistyped host is worse than falling back.
        if (feedbackTo_[0]
            && std::sscanf(feedbackTo_, "%u.%u.%u.%u%n", &a, &b, &c, &d, &used) == 4
            && feedbackTo_[used] == '\0'
            && a < 256 && b < 256 && c < 256 && d < 256) {
            out[0] = static_cast<uint8_t>(a); out[1] = static_cast<uint8_t>(b);
            out[2] = static_cast<uint8_t>(c); out[3] = static_cast<uint8_t>(d);
            return true;
        }
        if (peer_[0] || peer_[1] || peer_[2] || peer_[3]) { std::memcpy(out, peer_, 4); return true; }
        return false;
    }

    char     feedbackTo_[16] = {};   ///< an override, empty meaning answer whoever wrote to us

    /// Report the port and who last reached us, a quiet peer being no peer at all.
    void reportPeer() {
        if (peerFresh())
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
