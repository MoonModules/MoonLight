#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/util/HostResolver.h"   // formatPeer: the sender by its board's name
#include "light/effects/ChannelDataSink.h"   // staging, the held frame and its placement
#include "light/effects/EffectBase.h"

#include "light/util/ArtNetPacket.h"   // shared ArtNet wire formats (build + parse)
#include "light/util/DdpPacket.h"      // shared DDP wire format
#include "light/util/E131Packet.h"     // shared E1.31/sACN wire format
#include "platform/platform.h"    // platform::UdpSocket: the three receive sockets

#include <atomic>
#include <cstring>

namespace mm {

/// Receives lights over UDP into the layer, from Resolume, Madrix, xLights and LedFx.
/// @card NetworkReceiveEffect.gif
///
/// It listens for Art-Net, E1.31 and DDP at once, with no protocol control, so whatever a sender speaks works and the status names it. The received lights land in the layer buffer, so they compose with modifiers and blending.
///
/// @attribution Art-Net, E1.31, DDP, troyhacks
///
/// @technical
///
/// Sender mapping is one copy alone on its layer, else two mappings, as the status shows. It binds Art-Net 6454, E1.31 5568 and DDP 4048, and an ArtPoll gets an ArtPollReply so controllers list the device.
class NetworkReceiveEffect : public EffectBase {
public:
    /// Catalog tags: network input, a MoonModules origin.
    const char* tags() const override { return "🖼️🌙"; }
    /// The sender decides the geometry, so this writes whatever the layer has.
    Dim dimensions() const override { return Dim::D3; }

    /// The first incoming universe to map, mirroring the sender.
    uint16_t universeStart = 0;
    /// Bytes each universe maps to, 510 or 512.
    uint16_t channelsPerUniverse = static_cast<uint16_t>(MAX_CHANNELS_PER_UNIVERSE);
    /// Who maps a frame onto the wiring, in the `mapped_by` options' order: row by row with this board applying its wiring, or in the order the lights are wired.
    enum MappedBy : uint8_t { kMappedByReceiver, kMappedBySender, kMappedByCount };
    /// The `mapped_by` option names.
    static constexpr const char* kMappers[kMappedByCount] = {"receiver", "sender"};
    /// Whether the sender or the receiver maps the frame onto the wiring.
    /// @guide **receiver**: frames arrive row by row and this board applies its wiring. **sender**: a @ref NetworkSendDriver sends wiring order, keeping byte order, brightness 255 and the linear curve.
    uint8_t mappedBy = kMappedByReceiver;

    /// Publish the universe start, the per-universe stride and who maps.
    void defineControls() override {
        controls_.addControl("universe_start", universeStart);
        controls_.addControl("channels_per_universe", channelsPerUniverse, 1, 512);
        controls_.addSelect("mapped_by", mappedBy, kMappers, kMappedByCount);
    }

    /// Turn what arrived since the last call into per-second rates for the status, so loss shows against what the sender sent.
    void tick1s() MM_NONBLOCKING override {
        const uint32_t now = platform::millis();
        const uint32_t span = now - rateSinceMs_;
        rateSinceMs_ = now;
        const uint32_t packets = packets_.exchange(0, std::memory_order_relaxed);
        const uint32_t frames = frames_.exchange(0, std::memory_order_relaxed);
        const uint32_t lost = lost_.exchange(0, std::memory_order_relaxed);
        if (span > 0) {
            packetsPerS_ = perSecond(packets, span);
            framesPerS_ = perSecond(frames, span);
            lostPerS_ = perSecond(lost, span);
        }
        lastProto_ = nullptr;   // the status is built again on the next tick, with these rates and a board heard since by name
        EffectBase::tick1s();
    }

    /// Frames received in the last second, which the status shows.
    uint32_t framesPerS() const { return framesPerS_; }
    /// Packets received in the last second.
    uint32_t packetsPerS() const { return packetsPerS_; }
    /// Packets missing from the last second's sequences or offsets.
    uint32_t lostPerS() const { return lostPerS_; }

    /// Who maps sizes staging, by logical cells or by physical lights, so only it rebuilds.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "mapped_by") == 0 || std::strcmp(name, "channels_per_universe") == 0;   // the latter sizes the per-universe sequences
    }

    /// A task still running would read a destroyed object, so the destructor stops it too.
    ~NetworkReceiveEffect() override { stopReceiveTask(); }

    /// Stop the receive task, close the three sockets, then chain so the staging buffer is freed too.
    void release() override {
        stopReceiveTask();
        // Chaining is required, or the registered staging buffer leaks on disable.
        artnetSocket_.close();
        e131Socket_.close();
        ddpSocket_.close();
        MoonModule::release();
    }

    /// Bind the three receive sockets independently, and size the staging buffer to the layer.
    void prepare() override {
        stopReceiveTask();   // the task writes into staging, which is resized below
        // Closed first: a rebuild prepares again without a release, and a socket bound once refuses a second bind.
        artnetSocket_.close();
        e131Socket_.close();
        ddpSocket_.close();
        const bool artnetOk = artnetSocket_.open() && artnetSocket_.bind(ARTNET_PORT);
        const bool e131Ok = e131Socket_.open() && e131Socket_.bind(E131_PORT);
        const bool ddpOk = ddpSocket_.open() && ddpSocket_.bind(DDP_PORT);
        if (artnetOk && e131Ok && ddpOk) {
            if (status() == kBindFailMsg) clearStatus();
        } else {
            setStatus(kBindFailMsg, Severity::Error);
        }
        sink_.prepare(mappedBy == kMappedBySender);
        pkt_.resize(kDatagramBytes);
        // One last sequence per universe the staging holds, each starting unknown.
        lastSeq_.resize(channelsPerUniverse ? sink_.bytes() / channelsPerUniverse + 1 : 0);
        for (size_t u = 0; u < lastSeq_.count(); u++) lastSeq_[u] = 0;
        ddpEnd_ = ddpFrameBytes_ = ddpPacketBytes_ = 0;
        ddpSynced_ = false;
        ddpPushes_ = false;
        lastUniverse_ = 0;
        frameMax_ = 0;
        frameShown_ = true;
        packets_.exchange(0, std::memory_order_relaxed);   // the rates restart with the stream, not from boot
        frames_.exchange(0, std::memory_order_relaxed);
        lost_.exchange(0, std::memory_order_relaxed);
        rateSinceMs_ = platform::millis();
        startReceiveTask();
    }

    /// Drain the sockets where no task does, then let the sink place a changed frame, the status naming the path it took.
    void tick() MM_NONBLOCKING override {
        if (!receiveTask_.impl) drainSockets<false>(kAllSockets);   // a board without tasks, the Teensy, drains here as packets wait
        publishSender(sink_.tick());
    }

    /// A sender-mapped frame is in wiring order.
    bool deliversWiringOrder() const MM_NONBLOCKING override { return sink_.wiringOrder(); }

    /// The sink hands its held frame to the output.
    void copyWiringFrame(uint8_t* out, size_t bytes, bool force) override { sink_.copyWiringFrame(out, bytes, force); }

    /// Place one universe's payload, clamped to its stride and ignoring universes outside the buffer; `kWait` as for applyBytes. A frame ends at its highest universe, or at its first when the last went missing.
    template <bool kWait = true>
    void applyDmx(uint16_t universe, const uint8_t* data, uint16_t len) {
        if (universe < universeStart || channelsPerUniverse == 0) return;
        if (len > channelsPerUniverse) len = channelsPerUniverse;
        bool frameDone = false;
        if (universe == universeStart) {
            frameDone = !frameShown_;   // the last frame's highest universe went missing: show what came
            frameShown_ = false;
            if (frameMax_) lastUniverse_ = frameMax_;   // the frame that ended sets where the next ends, so a stream that shrinks is learned at once
            frameMax_ = universe;
        }
        if (universe > frameMax_) frameMax_ = universe;
        if (universe >= lastUniverse_) { frameDone = true; frameShown_ = true; }
        applyBytes<kWait>(static_cast<size_t>(universe - universeStart) * channelsPerUniverse, data, len, frameDone);
    }

    /// Write a payload into the sink at its byte offset; the task waits out one render copy, the tick drains only where no task runs.
    template <bool kWait = true>
    void applyBytes(size_t offset, const uint8_t* data, uint16_t len, bool frameDone = true) { sink_.write<kWait>(offset, data, len, frameDone); }

    /// Whether a receive task drains the sockets, rather than the render tick: the test seam for the fallback.
    bool receivesInTask() const { return receiveTask_.impl != nullptr; }

    /// Test seam: the staging bytes, so a test can pin the buffer's lifecycle.
    const uint8_t* stagingData() const { return sink_.data(); }
    /// Test seam: one staged byte, synchronized with the receive task's writes.
    uint8_t stagedAt(size_t i) { return sink_.byteAt(i); }
    /// Test seam: how many staging bytes are allocated.
    size_t stagingBytes() const { return sink_.bytes(); }

    /// Note the last sender, per packet from the receive path; a test calls it to stand in for one.
    void recordSender(const char* proto, const uint8_t ip[4]) {
        senderIp_.store(static_cast<uint32_t>(ip[0]) << 24 | static_cast<uint32_t>(ip[1]) << 16 |
                        static_cast<uint32_t>(ip[2]) << 8 | ip[3], std::memory_order_relaxed);
        senderProto_.store(proto, std::memory_order_release);
    }

private:
    static constexpr int kMaxPacketsPerDrain = 128;   ///< packets one drain takes per socket, so a flood cannot hold the caller
    static constexpr int kAllSockets = 0b111;   ///< Art-Net, E1.31 and DDP, in waitReadable's bit order
    static constexpr uint32_t kCoalesceUs = 1000;   ///< the wait after a drain, so one wake drains several packets, as Linux's NAPI coalesces interrupts
    static constexpr uint32_t kWaitMs = 20;   ///< how long the task waits for a packet before it checks for a stop
    static constexpr const char* kBindFailMsg = "UDP bind failed, port in use?";
    char recvStatus_[128] = "";         ///< "<n> fps, <n> packets/s, <n> lost/s, <protocol> from <board> (<ip>), <path>", a member since setStatus holds the pointer
    const char* lastProto_ = nullptr;   ///< the last protocol literal the status was built from
    uint8_t lastPath_ = 0;              ///< the last path the status was built from
    /// The status note for each of the sink's paths: none for receiver mapping, which maps once as every layer does.
    static constexpr const char* kPathNotes[3] = {"", ", one copy", ", mapped twice"};
    uint8_t     lastIp_[4] = {};        ///< the last sender IP the status was built from

    platform::UdpSocket artnetSocket_;   ///< ArtNet, port 6454
    platform::UdpSocket e131Socket_;     ///< E1.31, port 5568
    platform::UdpSocket ddpSocket_;      ///< DDP, port 4048
    static constexpr size_t kDatagramBytes = 1500;   ///< one datagram of any protocol, DDP reaching 1450
    ScratchBuffer<uint8_t> pkt_{*this};  ///< the datagram being read, taken in prepare
    ChannelDataSink sink_{*this};        ///< where the frames land, held and placed
    std::atomic<uint32_t> packets_{0};   ///< packets since the last tick1s, counted by the receive path
    std::atomic<uint32_t> frames_{0};    ///< frames since the last tick1s, counted by the receive path
    std::atomic<uint32_t> lost_{0};      ///< packets missing from the sequence since the last tick1s
    uint32_t rateSinceMs_ = 0;           ///< when the counts last restarted, which the rates divide by
    uint32_t packetsPerS_ = 0, framesPerS_ = 0, lostPerS_ = 0;   ///< the last second's rates
    /// Each universe's last sequence, the protocol's tag in the high byte so a switch between Art-Net and E1.31 starts over; 0 is none yet.
    ScratchBuffer<uint16_t> lastSeq_{*this};
    uint32_t ddpEnd_ = 0;                ///< where the last DDP packet ended
    uint32_t ddpFrameBytes_ = 0;         ///< the furthest end a DDP frame reached, its size
    uint16_t ddpPacketBytes_ = 0;        ///< the largest DDP payload seen, what a missing stretch is counted in
    bool ddpSynced_ = false;             ///< a frame's start or end has been seen, so offsets are counted from a frame's start
    bool ddpPushes_ = false;             ///< the DDP sender marks its frames' last packets, so a frame shows at its push
    uint16_t lastUniverse_ = 0;          ///< the highest universe the last frame reached, where an Art-Net or E1.31 frame ends
    uint16_t frameMax_ = 0;              ///< the highest universe the current frame has reached
    bool frameShown_ = true;             ///< the current Art-Net or E1.31 frame reached its highest universe
    static constexpr uint16_t kTagArtNet = 0x100, kTagE131 = 0x200;
    platform::WorkerTask receiveTask_;        ///< drains the sockets as packets arrive, on core 1
    std::atomic<bool> receiveStop_{false};
    std::atomic<const char*> senderProto_{nullptr};   ///< the last sender's protocol, which the render tick publishes
    std::atomic<uint32_t> senderIp_{0};               ///< and its address, as the four octets in order

    static void receiveTrampoline(void* self) { static_cast<NetworkReceiveEffect*>(self)->runReceiveLoop(); }

    /// Wait for packets and drain them, until release or prepare stops the task.
    void runReceiveLoop() {
        platform::UdpSocket* sockets[3] = {&artnetSocket_, &e131Socket_, &ddpSocket_};
        while (!receiveStop_.load(std::memory_order_acquire)) {
            const int ready = platform::UdpSocket::waitReadable(sockets, 3, kWaitMs);
            if (ready > 0) {
                drainSockets<true>(ready);
                platform::sleepUs(receiveTask_, kCoalesceUs);   // the next packets gather meanwhile, so one wake drains several
            }
        }
    }

    /// Start the receive task, since lwIP holds a frame's burst in a small mailbox that a task on core 1 drains as it arrives. A board without tasks drains in the tick.
    void startReceiveTask() {
        receiveStop_.store(false, std::memory_order_release);
        // Priority 7, the send task's: a packet arrives every few hundred microseconds, the shortest period on core 1, and the task blocks between them (rate-monotonic priority).
        platform::spawnPinnedTask(receiveTask_, "mmReceive", &receiveTrampoline, this, 4096, 7, 1);
    }

    void stopReceiveTask() {
        if (!receiveTask_.impl) return;
        receiveStop_.store(true, std::memory_order_release);
        platform::stopPinnedTask(receiveTask_);
    }

    /// Drain the `ready` sockets into staging, bit i for the i-th of Art-Net, E1.31 and DDP, bounded per socket so a flood cannot hold the caller; `kWait` when the receive task drains.
    template <bool kWait>
    void drainSockets(int ready) {
        if (!pkt_) return;
        if (ready & 1) drain(artnetSocket_, [&](const uint8_t* p, size_t n, const uint8_t* ip) { onArtNet<kWait>(p, n, ip); });
        if (ready & 2) drain(e131Socket_, [&](const uint8_t* p, size_t n, const uint8_t* ip) { onE131<kWait>(p, n, ip); });
        if (ready & 4) drain(ddpSocket_, [&](const uint8_t* p, size_t n, const uint8_t* ip) { onDdp<kWait>(p, n, ip); });
    }

    /// Read one socket until it is empty or the bound is reached, handing each datagram to `on`.
    template <typename On>
    void drain(platform::UdpSocket& socket, On on) {
        uint8_t srcIp[4];
        for (int i = 0; i < kMaxPacketsPerDrain; i++) {
            const int n = socket.recvFrom(pkt_.data(), kDatagramBytes, srcIp);
            if (n <= 0) break;
            on(pkt_.data(), static_cast<size_t>(n), srcIp);
        }
    }

    /// One Art-Net datagram: a universe to place, or a poll to answer.
    template <bool kWait>
    void onArtNet(const uint8_t* pkt, size_t n, const uint8_t* srcIp) {
        uint16_t universe = 0, dataLen = 0;
        const uint8_t* data = nullptr;
        uint8_t seq = 0;
        if (parseArtDmxPacket(pkt, n, universe, data, dataLen, &seq)) {
            countPacket(universe == universeStart);   // before the write, so a frame seen in the layer is already counted
            if (seq) countGap(universe, kTagArtNet, seq, 1);   // Art-Net's 0 is a sender without a sequence
            applyDmx<kWait>(universe, data, dataLen);
            recordSender("Art-Net", srcIp);
        } else if (isArtPoll(pkt, n)) {
            replyToPoll(srcIp);   // make the device show up in controller node lists
        }
    }

    /// One E1.31 datagram: a universe to place.
    template <bool kWait>
    void onE131(const uint8_t* pkt, size_t n, const uint8_t* srcIp) {
        uint16_t universe = 0, dataLen = 0;
        const uint8_t* data = nullptr;
        uint8_t seq = 0;
        if (!parseE131Packet(pkt, n, universe, data, dataLen, &seq)) return;
        countPacket(universe == universeStart);
        countGap(universe, kTagE131, seq, 0);
        applyDmx<kWait>(universe, data, dataLen);
        recordSender("E1.31", srcIp);
    }

    /// One DDP datagram: bytes to place at their offset, a frame shown at its push once the sender pushes.
    template <bool kWait>
    void onDdp(const uint8_t* pkt, size_t n, const uint8_t* srcIp) {
        uint32_t byteOffset = 0;
        uint16_t dataLen = 0;
        const uint8_t* data = nullptr;
        bool push = false;
        if (!parseDdpPacket(pkt, n, byteOffset, data, dataLen, &push)) return;
        countPacket(push);
        countDdpGap(byteOffset, dataLen, push);
        if (push) ddpPushes_ = true;
        applyBytes<kWait>(byteOffset, data, dataLen, push || !ddpPushes_);   // a sender without pushes shows every packet
        recordSender("DDP", srcIp);
    }

    /// Count one packet, and a frame when it starts or ends one, with read-modify-writes on both sides since a module lives in PSRAM, as TryLock explains.
    void countPacket(bool frame) {
        packets_.fetch_add(1, std::memory_order_relaxed);
        if (frame) frames_.fetch_add(1, std::memory_order_relaxed);
    }
    /// Count the packets missing before `seq` in this universe's sequence, which runs from `first` to 255.
    void countGap(uint16_t universe, uint16_t tag, uint8_t seq, uint8_t first) {
        if (universe < universeStart || static_cast<size_t>(universe - universeStart) >= lastSeq_.count()) return;
        uint16_t& last = lastSeq_[universe - universeStart];
        if ((last & 0xFF00) == tag) {
            const uint32_t step = ahead(static_cast<uint8_t>(last), seq, first, 255);
            if (step == 0) return;   // a repeat or a late packet: nothing lost, and the sequence stays where it was
            countLost(step - 1);
        }
        last = static_cast<uint16_t>(tag | seq);
    }
    /// Count the DDP packets missing before this one by byte offset, since DDP's 4-bit sequence wraps after 15: a jump past the last end, or a frame restarting short of its usual end.
    void countDdpGap(uint32_t offset, uint16_t len, bool push) {
        if (len > ddpPacketBytes_) ddpPacketBytes_ = len;
        if (offset == 0 && !ddpSynced_) { ddpSynced_ = true; ddpEnd_ = 0; }   // a frame's start counts from here, a push lost or not
        if (ddpSynced_) {
            const uint32_t missing = offset >= ddpEnd_ ? offset - ddpEnd_   // a stretch skipped inside the frame
                : (ddpFrameBytes_ > ddpEnd_ ? ddpFrameBytes_ - ddpEnd_ : 0) + offset;   // the last frame's tail, and this one's head
            countLost((missing + ddpPacketBytes_ - 1) / ddpPacketBytes_);
        }
        ddpEnd_ = offset + len;
        if (ddpEnd_ > ddpFrameBytes_) ddpFrameBytes_ = ddpEnd_;
        if (push) ddpSynced_ = true;   // a frame starting again at 0 reads as the next, so a sender pushing every packet loses none
    }
    /// Add the packets a gap missed, touching the shared counter only when there are some.
    void countLost(uint32_t n) { if (n) lost_.fetch_add(n, std::memory_order_relaxed); }
    /// How far `seq` lies ahead of `prev` on a counter that runs from `first` to `last` and wraps; 0 for a repeat or a packet at most kLateWindow - 1 behind, which E1.31-2018 §6.7.2 reads as out of order rather than a wrap.
    static uint32_t ahead(uint8_t prev, uint8_t seq, uint8_t first, uint8_t last) {
        const uint32_t cycle = last - first + 1u;
        const uint32_t step = (seq + cycle - prev) % cycle;
        return step > cycle - kLateWindow ? 0 : step;
    }
    static constexpr uint32_t kLateWindow = 20;   ///< E1.31's window: a packet less than 20 behind the last is late, not a wrap
    /// A count over `spanMs` as a per-second rate.
    static uint32_t perSecond(uint32_t count, uint32_t spanMs) { return static_cast<uint32_t>(uint64_t{count} * 1000 / spanMs); }

    /// Show the last sender in the status, from the render side, which owns the status.
    void publishSender(ChannelDataSink::Path path) {
        const char* proto = senderProto_.load(std::memory_order_acquire);
        if (!proto) return;
        const uint32_t v = senderIp_.load(std::memory_order_relaxed);
        const uint8_t ip[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
                               static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
        noteReceiving(proto, ip, path);
    }

    /// Update the receiving diagnostic, short-circuiting the common case since this is the hot path.
    void noteReceiving(const char* proto, const uint8_t ip[4], uint8_t path) {
        const char* s = status();
        if (s != nullptr && s != recvStatus_) return;   // a bind error (or foreign status) wins
        // Already showing this source and path, so skip both the format and the setStatus.
        if (s == recvStatus_ && proto == lastProto_ && path == lastPath_ && std::memcmp(ip, lastIp_, 4) == 0) return;
        lastProto_ = proto;
        lastPath_ = path;
        std::memcpy(lastIp_, ip, 4);
        char peer[HostResolver::kPeerChars];
        HostResolver::formatPeer(peer, sizeof(peer), ip);
        mm::formatTo(recvStatus_, sizeof(recvStatus_), "%u fps, %u packets/s, %u lost/s, %s from %s%s",
                     static_cast<unsigned>(framesPerS_), static_cast<unsigned>(packetsPerS_), static_cast<unsigned>(lostPerS_),
                     proto, peer, kPathNotes[path]);
        setStatus(recvStatus_, Severity::Status);
    }

    /// Answer an ArtPoll with our address and name, so controllers list the device.
    void replyToPoll(const uint8_t pollerIp[4]) {
        uint8_t myIp[4];
        platform::localIPv4(myIp);
        // The desktop has no netif, so parse hostIp()'s string as the last resort.
        if (!myIp[0] && !myIp[1] && !myIp[2] && !myIp[3]) {
            if (!parseDottedQuad(platform::hostIp(), myIp)) return;  // no usable IP, so stay silent
        }
        uint8_t mac[6];
        platform::getMacAddress(mac);
        uint8_t reply[ARTNET_POLL_REPLY_SIZE];
        buildArtPollReply(reply, myIp, mac, "MoonLight", "MoonLight NetworkReceive",
                          universeStart);
        artnetSocket_.sendToAddr(pollerIp, ARTNET_PORT, reply, sizeof(reply));
    }
};

} // namespace mm
