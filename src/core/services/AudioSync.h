#pragma once

#include <cstdint>
#include <cstring>

#include "platform/platform.h"
#include "core/module/MoonModule.h"     // the owner whose status line the sync reports on
#include "core/util/format.h"           // formatTo: nonblocking formatting into a fixed buffer
#include "core/util/Addressing.h"       // sendAddressed: the multicast rule every sender shares
#include "core/util/HostList.h"         // the boards a unicast send goes to
#include "core/util/HostResolver.h"     // a named board's address, looked up off the render thread, and formatPeer: the sender by its board's name
#include "core/util/AudioFrame.h"
#include "core/util/MoonLightAudioSyncPacket.h"   // the MoonLight audio-sync format
#include "core/util/WLEDAudioSyncPacket.h"        // WLED's audio-sync format

namespace mm {

/// AudioService's audio sync: one socket that sends the frame a packet at a time or receives a peer's, its state on the owner's status line: @xref{audio-sync-one-packet-a-frame}.
///
/// @technical
///
/// ## Audio sync, one packet a frame
///
/// A sending board sends its frame in `syncFormat`: MoonLight's whole frame, WLED's packet, or both for an installation that mixes the two. Each format multicasts to its own group, so a WLED device never wakes for a packet it cannot read. The hits of the frames between two packets ride in the next, so the throttle drops none.
///
/// A receiving board joins both groups and tells the formats by their header. A board sending both sends each frame twice, so MoonLight's packet outranks WLED's while it arrives. A frame with the sender's smoothed bands keeps them, so every board shows the same frame, and one without, a WLED frame among them, gets the bands' ballistic here; a late or repeated MoonLight frame number is dropped.
class AudioSync {
public:
    /// What the socket does: nothing, send the local analysis, or receive a peer's.
    enum Role : uint8_t { kNone, kSend, kReceive };

    static constexpr uint32_t kSendIntervalMs = 25;   ///< ~40/s, WLED-friendly, well under a flood
    static constexpr uint32_t kFallbackMs = 1000;     ///< no packet this long and the peer is gone
    static constexpr uint32_t kOpenRetryMs = 1000;    ///< pause between socket bring-up retries after a failure

    /// The sync reporting on `owner`'s status line.
    explicit AudioSync(MoonModule& owner) : owner_(owner) {}

    /// Close the socket and forget the peer, for `role` to open on a later tick; the status says what it waits for.
    void reset(Role role) {
        if constexpr (!platform::hasNetwork) return;
        close();                           // ensureOpen() re-opens per role when the net is up
        lastOpenFailMs_ = 0;               // a mode change retries bring-up immediately (no stale backoff)
        lastRecvMs_ = mlHeardMs_ = 0;
        std::memset(peer_, 0, sizeof(peer_));
        hits_ = {};
        rxHits_ = {};
        rxSeqValid_ = false;
        // Only when there IS a socket to wait for.
        if (role == kSend)         owner_.setStatus("send: waiting for network");
        else if (role == kReceive) owner_.setStatus("receive: waiting for network");
        else if (isOwnStatus(owner_.status())) owner_.setStatus("");
    }

    /// Close the socket.
    void close() {
        sock_.close();
        open_ = false;
    }

    /// Open the socket for `role` on `port` once the network stack is up, backing off after a failure; answers whether it is open.
    bool ensureOpen(Role role, uint16_t port, bool unicast) {
        if constexpr (!platform::hasNetwork) return false;
        if (role == kNone) return false;
        if (open_) return true;
        if (!platform::networkReady()) return false;   // interface not up yet, try again next tick
        const uint32_t now = platform::millis();
        if (lastOpenFailMs_ != 0 && now - lastOpenFailMs_ < kOpenRetryMs) return false;
        // Send → each format's group, or the hosts; receive → bind, then join both groups. A failed join degrades rather than refuses, since unicast still arrives.
        open_ = sock_.open() && (role == kSend || sock_.bind(port));
        if (!open_) {
            sock_.close();
            owner_.setStatus(role == kSend ? "send: socket failed" : "receive: bind failed", MoonModule::Severity::Error);
        } else if (role == kSend) {
            reportSending(unicast);
        } else {
            sock_.setMulticastGroups(kGroups, kGroupCount);
            reportListening();
        }
        lastOpenFailMs_ = open_ ? 0 : (now == 0 ? 1 : now);   // stamp a failure, or clear the timer on success
        return open_;
    }

    /// Take a new frame's hits, which the next packet carries.
    void addHits(const AudioFrame& f) { hits_.add(f); }

    /// Send `frame` in MoonLight's format, WLED's or both, with the hits since the last packet, throttled to ~40/s: @xref{audio-sync-one-packet-a-frame}.
    void send(const AudioFrame& frame, bool moonLight, bool wled, bool unicast, uint16_t port) {
        if constexpr (!platform::hasNetwork) return;
        const uint32_t now = platform::millis();
        if (now - lastSendMs_ < kSendIntervalMs) return;
        lastSendMs_ = now;
        frame_ = frame;
        hits_.applyTo(frame_);
        hits_ = {};
        uint8_t pkt[kMlSyncMaxPacket];
        if (moonLight) sendPacket(pkt, buildMoonLightAudioSync(pkt, frame_), kGroups[0].ip, unicast, port);
        if (wled) sendPacket(pkt, buildWledAudioSync(pkt, frame_), kGroups[1].ip, unicast, port);
        sendCount_++;
    }

    /// Drain the socket, bounded and non-blocking, either format, into `frame` with the hits of every packet; answers whether a new frame came, `smoothHere` when it carried no smoothed bands, whose ballistic `frame`'s own bands carry on.
    bool receive(AudioFrame& frame, bool& smoothHere) {
        if constexpr (!platform::hasNetwork) return false;
        if (lastRecvMs_ != 0 && platform::millis() - lastRecvMs_ >= kFallbackMs) rxSeqValid_ = false;   // a silent sender may come back restarted
        uint8_t pkt[kMlSyncMaxDatagram];     // what the format allows, so a later version's sections arrive whole
        uint8_t srcIp[4] = {};
        bool fresh = false;
        for (int i = 0; i < kMaxRecvPerTick; i++) {
            const int n = sock_.recvFrom(pkt, sizeof(pkt), srcIp);
            if (n <= 0) break;                     // -1 = nothing pending
            const uint16_t sections = parseMoonLightAudioSync(pkt, static_cast<size_t>(n), frame_);
            const bool ml = sections != 0;
            if (!ml && (mlHeardMs_ != 0 && platform::millis() - mlHeardMs_ < kFallbackMs)) continue;   // the same frame again, from a board sending both
            if (!ml && !parseWledAudioSync(pkt, static_cast<size_t>(n), frame_)) continue;   // a v1 / foreign packet
            if (!take(ml, srcIp)) continue;
            smoothHere = !(sections & (1u << kMlSectionSmoothed));   // WLED's packet, or a sender that does not smooth
            if (smoothHere) std::memcpy(frame_.bandsSmoothed, frame.bandsSmoothed, sizeof(frame_.bandsSmoothed));   // the ballistic run here lives across packets
            frame = frame_;
            rxWled_ = !ml;
            fresh = true;
        }
        if (!fresh) return false;
        rxHits_.applyTo(frame);
        rxHits_ = {};
        return true;
    }

    /// Parse `hosts`, starting the name lookups here where allocating is allowed.
    void parseHosts(const char* hosts) {
        hostsError_ = parseHostList(hosts, hosts_, kMaxHosts, hostCount_);
        if (hostsError_) { hostCount_ = 0; return; }
        startResolverForNames(hosts_, hostCount_);
        refreshNamedHosts(hosts, hosts_, hostCount_);
    }

    /// Take the names in `hosts` that have resolved since.
    void refreshHosts(const char* hosts) { refreshNamedHosts(hosts, hosts_, hostCount_); }

    /// The live state on the owner's status line, once a second: sending, receiving from whom, or listening.
    void report(Role role) {
        if constexpr (!platform::hasNetwork) return;
        if (role == kNone || !open_) return;
        if (role == kSend) { owner_.setStatus("sending"); return; }
        if (lastRecvMs_ == 0 || platform::millis() - lastRecvMs_ >= kFallbackMs) { reportListening(); return; }
        // Named, because "receiving" alone cannot tell a rig taking the right source from one locked.
        char peer[HostResolver::kPeerChars];
        HostResolver::formatPeer(peer, sizeof(peer), peer_);
        mm::formatTo(status_, sizeof(status_), "receiving %s from %s", rxWled_ ? "WLED" : "MoonLight", peer);
        owner_.setStatus(status_);
    }

    /// Whether the socket is open.
    bool isOpen() const { return open_; }
    /// How many sends have gone out.
    uint32_t sendCount() const { return sendCount_; }
    /// How many times the receive socket joined a group.
    uint32_t joinCount() const { return sock_.joinCountForTest(); }

private:
    /// Each format's multicast group: MoonLight's own, and WLED's, which every WLED device joins.
    static constexpr platform::UdpSocket::MulticastGroup kGroups[] = {{{239, 0, 0, 2}}, {{239, 0, 0, 1}}};
    static constexpr uint8_t kGroupCount = sizeof(kGroups) / sizeof(kGroups[0]);
    static constexpr int kMaxRecvPerTick = 8;         // bounded non-blocking drain (sync is low-rate)
    static constexpr uint16_t kSeqWindow = 64;        // how far behind the last MoonLight frame number is late, not a restarted sender
    static constexpr uint8_t kMaxHosts = 8;           // how many boards a unicast send reaches

    MoonModule& owner_;
    platform::UdpSocket sock_;       // one socket, either format, bound only in send or receive
    AudioFrame frame_;               // the frame a packet is parsed into or built from, a member since it and the datagram together would pass the stack's budget
    AudioHits hits_;                 // the hits of the frames since the last packet, which a throttled send would drop
    AudioHits rxHits_;               // the hits of the packets since the last frame taken, late ones among them
    Host     hosts_[kMaxHosts] = {};
    uint8_t  hostCount_ = 0;
    const char* hostsError_ = nullptr;   // why the host list was refused, shown on the status
    uint32_t lastSendMs_ = 0;        // the send throttle
    uint32_t sendCount_ = 0;
    uint32_t lastOpenFailMs_ = 0;    // the last failed open, 0 for none: the bring-up backoff
    uint32_t lastRecvMs_ = 0;        // the last packet taken
    uint32_t mlHeardMs_ = 0;         // the last MoonLight packet, which outranks WLED's while it arrives
    uint16_t rxSeq_ = 0;             // the last MoonLight frame number taken, so a late or repeated one is dropped
    bool     rxSeqValid_ = false;    // whether rxSeq_ holds one, false until the first and after a silence
    bool     rxWled_ = false;        // whether the last frame taken came in WLED's format, for the status line
    bool     open_ = false;          // the socket is open for the current role
    uint8_t  peer_[4] = {};          // the last packet's sender, for the status line
    char     status_[sizeof("receiving MoonLight from ") + HostResolver::kPeerChars] = {};   // "receiving <format> from <board> (<ip>)"

    /// Take the packet just parsed into `frame_` from `srcIp`, its hits kept; false for a late or repeated MoonLight frame, whose hits the next frame carries.
    bool take(bool ml, const uint8_t (&srcIp)[4]) {
        lastRecvMs_ = platform::millis();
        if (ml) mlHeardMs_ = lastRecvMs_;
        std::memcpy(peer_, srcIp, sizeof(peer_));   // whose audio this is
        const uint16_t behind = static_cast<uint16_t>(rxSeq_ - frame_.seq);
        const bool late = ml && rxSeqValid_ && behind < kSeqWindow;   // repeated, or late
        if (!late || behind != 0) rxHits_.add(frame_);   // a late frame's hits went in no other packet, so the next frame carries them
        if (late) return false;
        if (ml) { rxSeq_ = frame_.seq; rxSeqValid_ = true; }
        return true;
    }

    /// One packet to its format's `group`, or to the hosts. A steady 40 frames a second is a stream, so no broadcast copy on any network.
    void sendPacket(const uint8_t* pkt, size_t n, const uint8_t group[4], bool unicast, uint16_t port) {
        sendAddressed(unicast ? Addressing::Unicast : Addressing::Multicast, Traffic::FrameRate,
                      hosts_, hostCount_, group, [&](const uint8_t ip[4]) { sock_.sendToAddr(ip, port, pkt, n); });
    }

    /// Where a send goes, or why a unicast send reaches nobody.
    void reportSending(bool unicast) {
        if (unicast && hostsError_) owner_.setStatus(hostsError_, MoonModule::Severity::Error);
        else if (unicast && hostCount_ == 0) owner_.setStatus("set hosts to send to", MoonModule::Severity::Warning);
        else owner_.setStatus("sending");
    }

    /// An open receive socket, saying so when only unicast can reach it.
    void reportListening() {
        const uint8_t joined = sock_.multicastJoined();
        if (joined == kGroupCount) owner_.setStatus("listening");
        else owner_.setStatus(joined ? "listening, one format's multicast group refused" : "listening, unicast only: multicast unavailable", MoonModule::Severity::Warning);
    }

    /// Whether status line `s` holds one of the sync's own messages, which a mode without a socket clears.
    static bool isOwnStatus(const char* s) {
        static constexpr const char* kOwn[] = {"waiting for network", "socket failed", "bind failed", "from ", "listening", "sending", "set hosts"};
        if (!s) return false;
        for (const char* k : kOwn) if (std::strstr(s, k)) return true;
        return false;
    }
};

} // namespace mm
