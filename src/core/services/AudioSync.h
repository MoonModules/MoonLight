#pragma once

#include <cstdint>
#include <cstring>
#include <memory>   // unique_ptr: the receive buffer, held only while receiving
#include <new>      // std::nothrow

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

/// The frame order of each sender, by its address, as RTP numbers per source: a frame its sender already passed is late or repeated, and the senders on one group never judge each other's numbers.
struct SenderOrder {
    static constexpr uint8_t kSenders = 4;          ///< the senders tracked at once; a fifth takes the one heard longest ago
    static constexpr uint16_t kWindow = 64;         ///< how far behind its sender's last frame a frame is late, not a restarted sender
    /// A frame against its sender's last: new, late, or the last one again.
    enum class Order : uint8_t { New, Late, Repeat };
    /// Where frame `seq` from `ip` at `nowMs` stands for its sender, a sender silent for `forgetMs` counting afresh, as a restarted one may come back.
    Order judge(const uint8_t (&ip)[4], uint16_t seq, uint32_t nowMs, uint32_t forgetMs) {
        Sender& s = senderOf(ip);
        const uint16_t behind = static_cast<uint16_t>(s.seq - seq);
        const bool known = s.heard && nowMs - s.ms < forgetMs;
        s.ms = nowMs;
        s.heard = true;
        if (known && behind == 0) return Order::Repeat;
        if (known && behind < kWindow) return Order::Late;
        s.seq = seq;
        return Order::New;
    }
    /// Forget every sender.
    void clear() { for (Sender& s : senders_) s = Sender{}; }

private:
    struct Sender { uint8_t ip[4] = {}; uint16_t seq = 0; uint32_t ms = 0; bool heard = false; };
    Sender senders_[kSenders] = {};
    /// The entry of `ip`, or the free or longest-silent one, given to it.
    Sender& senderOf(const uint8_t (&ip)[4]) {
        Sender* pick = &senders_[0];
        for (Sender& s : senders_) {
            if (s.heard && std::memcmp(s.ip, ip, 4) == 0) return s;
            if (!s.heard || (pick->heard && s.ms < pick->ms)) pick = &s;
        }
        *pick = Sender{};
        std::memcpy(pick->ip, ip, 4);
        return *pick;
    }
};

/// Which sender a receiver follows, as sACN (E1.31) selects a source: the highest priority, the first heard on a tie, held until it falls silent, so two senders on one group never make the lights alternate between their frames.
struct SourceSelection {
    static constexpr uint8_t kDefaultPriority = 100;   ///< sACN's default, every sender's until a priority travels in the packet
    /// Whether a packet from `ip` with `priority` at `nowMs` is the followed sender's: a sender with a higher priority, or any once the followed one is silent for `forgetMs`, takes over.
    bool follows(const uint8_t (&ip)[4], uint8_t priority, uint32_t nowMs, uint32_t forgetMs) {
        const bool same = following_ && std::memcmp(ip_, ip, 4) == 0;
        const bool lapsed = !following_ || nowMs - ms_ >= forgetMs;
        if (!same && !lapsed && priority <= priority_) { ignoredMs_ = nowMs; ignored_ = true; return false; }
        std::memcpy(ip_, ip, 4);
        priority_ = priority;
        ms_ = nowMs;
        following_ = true;
        return true;
    }
    /// Whether another sender was passed over within `forgetMs` of `nowMs`, for the status line.
    bool othersIgnored(uint32_t nowMs, uint32_t forgetMs) const { return ignored_ && nowMs - ignoredMs_ < forgetMs; }
    /// Follow no one, the next sender heard taken.
    void clear() { *this = SourceSelection{}; }

private:
    uint8_t ip_[4] = {};
    uint8_t priority_ = 0;
    uint32_t ms_ = 0, ignoredMs_ = 0;
    bool following_ = false, ignored_ = false;
};

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
        order_.clear();
        selection_.clear();
        // The receive buffer is taken here, where allocating is allowed, and only for receiving.
        if (role == kReceive) rx_.reset(new (std::nothrow) uint8_t[kMlSyncMaxDatagram]);
        // Only when there IS a socket to wait for.
        if (role == kSend)         owner_.setStatus("send: waiting for network");
        else if (role == kReceive) owner_.setStatus("receive: waiting for network");
        else if (isOwnStatus(owner_.status())) owner_.setStatus("");
    }

    /// Close the socket and free the receive buffer.
    void close() {
        sock_.close();
        open_ = false;
        rx_.reset();
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
        open_ = (role == kSend || rx_) && sock_.open() && (role == kSend || sock_.bind(port));
        if (!open_) {
            sock_.close();
            owner_.setStatus(role == kSend ? "send: socket failed" : !rx_ ? "receive: no memory for packets" : "receive: bind failed", MoonModule::Severity::Error);
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
        uint8_t* pkt = rx_.get();            // the whole datagram the format allows, so a later version's sections arrive whole
        if (!pkt) return false;
        uint8_t srcIp[4] = {};
        bool fresh = false;
        for (int i = 0; i < kMaxRecvPerTick; i++) {
            const int n = sock_.recvFrom(pkt, kMlSyncMaxDatagram, srcIp);
            if (n <= 0) break;                     // -1 = nothing pending
            uint16_t sections = 0;
            const bool ml = parseMoonLightAudioSync(pkt, static_cast<size_t>(n), frame_, &sections);
            if (!ml && (mlHeardMs_ != 0 && platform::millis() - mlHeardMs_ < kFallbackMs)) continue;   // the same frame again, from a board sending both
            if (!ml && !parseWledAudioSync(pkt, static_cast<size_t>(n), frame_)) continue;   // a v1 / foreign packet
            if (!selection_.follows(srcIp, SourceSelection::kDefaultPriority, platform::millis(), kFallbackMs)) continue;   // another sender while the followed one plays
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
        mm::formatTo(status_, sizeof(status_), "receiving %s from %s%s", rxWled_ ? "WLED" : "MoonLight", peer,
                     selection_.othersIgnored(platform::millis(), kFallbackMs) ? ", ignoring another sender" : "");
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
    static constexpr uint8_t kMaxHosts = 8;           // how many boards a unicast send reaches

    MoonModule& owner_;
    platform::UdpSocket sock_;       // one socket, either format, bound only in send or receive
    std::unique_ptr<uint8_t[]> rx_;  // the receive buffer, a whole datagram, held only while receiving
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
    SenderOrder order_;              // each MoonLight sender's last frame number, so a late or repeated one is dropped
    SourceSelection selection_;      // the one sender followed while it plays
    bool     rxWled_ = false;        // whether the last frame taken came in WLED's format, for the status line
    bool     open_ = false;          // the socket is open for the current role
    uint8_t  peer_[4] = {};          // the last packet's sender, for the status line
    char     status_[sizeof("receiving MoonLight from , ignoring another sender") + HostResolver::kPeerChars] = {};   // "receiving <format> from <board> (<ip>)", and whether another sender is passed over

    /// Take the packet just parsed into `frame_` from `srcIp`, its hits kept; false for a late or repeated MoonLight frame, whose hits the next frame carries.
    bool take(bool ml, const uint8_t (&srcIp)[4]) {
        lastRecvMs_ = platform::millis();
        if (ml) mlHeardMs_ = lastRecvMs_;
        std::memcpy(peer_, srcIp, sizeof(peer_));   // whose audio this is
        const SenderOrder::Order o = ml ? order_.judge(srcIp, frame_.seq, lastRecvMs_, kFallbackMs) : SenderOrder::Order::New;   // judged by its own sender's numbers
        if (o != SenderOrder::Order::Repeat) rxHits_.add(frame_);   // a late frame's hits went in no other packet, so the next frame carries them
        return o == SenderOrder::Order::New;
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
        static constexpr const char* kOwn[] = {"waiting for network", "socket failed", "bind failed", "no memory for packets", "from ", "listening", "sending", "set hosts"};
        if (!s) return false;
        for (const char* k : kOwn) if (std::strstr(s, k)) return true;
        return false;
    }
};

} // namespace mm
