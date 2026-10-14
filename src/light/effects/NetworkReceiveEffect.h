#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "light/effects/ChannelDataSink.h"   // staging, the held frame and its placement
#include "light/effects/EffectBase.h"

#include "light/util/ArtNetPacket.h"   // shared ArtNet wire formats (build + parse)
#include "light/util/DdpPacket.h"      // shared DDP wire format
#include "light/util/E131Packet.h"     // shared E1.31/sACN wire format
#include "platform/platform.h"    // platform::UdpSocket: the three receive sockets

#include <atomic>
#include <cstring>

namespace mm {


// Author: MoonLight original (E1.31 / Art-Net receive)
/// Effect that paints the layer from received Art-Net/E1.31/DDP pixels.
/// @card NetworkReceiveEffect.gif
///
/// External light data written into the layer buffer, so it composes with modifiers and blending; the receive side for NetworkSendDriver and industry senders.
///
/// Prior art: MoonLight's D_NetworkIn, WLED's realtime UDP input, and MoonLight v1's ArtNetInModule.
///
/// @moreinfo
///
/// ## Three protocols at once
///
/// It binds Art-Net 6454, E1.31 5568 and DDP 4048 with no protocol control, so whatever a sender speaks works; the status names it, and an ArtPoll gets an ArtPollReply.
///
/// ## Mapped by the receiver or by the sender
///
/// A receiver-mapped frame arrives row by row and the layer applies this board's wiring; a sender-mapped one arrives in wiring order, placed by the channel-data sink.
class NetworkReceiveEffect : public EffectBase {
public:
    /// Catalog tags: network input, of MoonLight and v1 lineage.
    const char* tags() const override { return "📡🌙"; }
    /// The sender decides the geometry, so this writes whatever the layer has.
    Dim dimensions() const override { return Dim::D3; }

    /// Mirrors the sender's own universe start, for ArtNet and E1.31.
    uint16_t universeStart = 0;
    /// Bytes a universe maps to, which also clamps the payload against bleeding into the next.
    uint16_t channelsPerUniverse = static_cast<uint16_t>(MAX_CHANNELS_PER_UNIVERSE);
    /// Who maps a frame onto the wiring, in the `mapped_by` options' order: row by row with this board applying its wiring, or in the order the lights are wired.
    enum MappedBy : uint8_t { kMappedByReceiver, kMappedBySender, kMappedByCount };
    /// The `mapped_by` option names.
    static constexpr const char* kMappers[kMappedByCount] = {"receiver", "sender"};
    /// Who maps the frame onto the wiring. @xref{mapped-by-the-receiver-or-by-the-sender}
    uint8_t mappedBy = kMappedByReceiver;
    /// Packets received in the last second, of any protocol.
    uint16_t packetsPerS = 0;
    /// Frames received in the last second: a DDP push, or the first universe of an Art-Net or E1.31 frame.
    uint16_t framesPerS = 0;

    /// Publish the universe start, the per-universe stride and who maps.
    void defineControls() override {
        controls_.addControl("universe_start", universeStart);
        controls_.addControl("channels_per_universe", channelsPerUniverse);
        controls_.addSelect("mapped_by", mappedBy, kMappers, kMappedByCount);
        controls_.addControl("packets_per_s", packetsPerS, 0, 65535);
        controls_.setReadOnly(controls_.count() - 1, true);
        controls_.setLive(controls_.count() - 1);
        controls_.addControl("frames_per_s", framesPerS, 0, 65535);
        controls_.setReadOnly(controls_.count() - 1, true);
        controls_.setLive(controls_.count() - 1);
    }

    /// Publish what arrived since the last call as a per-second rate, so loss shows against what the sender sent.
    void tick1s() MM_NONBLOCKING override {
        const uint32_t now = platform::millis();
        const uint32_t span = now - rateSinceMs_;
        rateSinceMs_ = now;
        const uint32_t packets = packets_.exchange(0, std::memory_order_relaxed);
        const uint32_t frames = frames_.exchange(0, std::memory_order_relaxed);
        if (span > 0) {
            packetsPerS = saturate16(static_cast<uint32_t>(uint64_t{packets} * 1000 / span));
            framesPerS = saturate16(static_cast<uint32_t>(uint64_t{frames} * 1000 / span));
        }
        EffectBase::tick1s();
    }

    /// Who maps sizes staging, by logical cells or by physical lights, so only it rebuilds.
    bool affectsPrepare(const char* name) const override { return std::strcmp(name, "mapped_by") == 0; }

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
        clearStatus();
    }

    /// Bind the three receive sockets independently, and size the staging buffer to the layer. @xref{three-protocols-at-once}
    void prepare() override {
        stopReceiveTask();   // the task writes into staging, which is resized below
        const bool artnetOk = artnetSocket_.open() && artnetSocket_.bind(ARTNET_PORT);
        const bool e131Ok = e131Socket_.open() && e131Socket_.bind(E131_PORT);
        const bool ddpOk = ddpSocket_.open() && ddpSocket_.bind(DDP_PORT);
        if (artnetOk && e131Ok && ddpOk) {
            if (status() == kBindFailMsg) clearStatus();
        } else {
            setStatus(kBindFailMsg, Severity::Error);
        }
        sink_.prepare(mappedBy == kMappedBySender);
        packets_.exchange(0, std::memory_order_relaxed);   // the rates restart with the stream, not from boot
        frames_.exchange(0, std::memory_order_relaxed);
        rateSinceMs_ = platform::millis();
        startReceiveTask();
    }

    /// Drain the sockets where no task does, then let the sink place a changed frame, the status naming the path it took.
    void tick() MM_NONBLOCKING override {
        if (!receiveTask_.impl) drainSockets<false>();   // a board without tasks, the Teensy, drains here as packets wait
        publishSender(sink_.tick());
    }

    /// A sender-mapped frame is in wiring order.
    bool deliversWiringOrder() const MM_NONBLOCKING override { return sink_.wiringOrder(); }

    /// The sink hands its held frame to the output.
    void copyWiringFrame(uint8_t* out, size_t bytes, bool force) override { sink_.copyWiringFrame(out, bytes, force); }

    /// Place one universe's payload, clamped to its stride and ignoring universes outside the buffer; `kWait` as for applyBytes.
    template <bool kWait = true>
    void applyDmx(uint16_t universe, const uint8_t* data, uint16_t len) {
        if (universe < universeStart || channelsPerUniverse == 0) return;
        if (len > channelsPerUniverse) len = channelsPerUniverse;
        applyBytes<kWait>(static_cast<size_t>(universe - universeStart) * channelsPerUniverse, data, len);
    }

    /// Write a payload into the sink at its byte offset; the task waits out one render copy, the tick drains only where no task runs.
    template <bool kWait = true>
    void applyBytes(size_t offset, const uint8_t* data, uint16_t len) { sink_.write<kWait>(offset, data, len); }

    /// Whether a receive task drains the sockets, rather than the render tick: the test seam for the fallback.
    bool receivesInTask() const { return receiveTask_.impl != nullptr; }

    /// Test seam: the staging bytes, so a test can pin the buffer's lifecycle.
    const uint8_t* stagingData() const { return sink_.data(); }
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
    static constexpr uint32_t kWaitMs = 20;   ///< how long the task waits for a packet before it checks for a stop
    static constexpr const char* kBindFailMsg = "UDP bind failed, port in use?";
    // A member rather than a stack buffer: setStatus holds the pointer, so the string must outlive it.
    char recvStatus_[56] = "";          ///< "receiving <protocol> from <ip>, <path>", sized for the longest form
    const char* lastProto_ = nullptr;   ///< the last protocol literal the status was built from
    uint8_t lastPath_ = 0;              ///< the last path the status was built from
    /// The status note for each of the sink's paths: none for receiver mapping, which maps once as every layer does.
    static constexpr const char* kPathNotes[3] = {"", ", one copy", ", mapped twice"};
    uint8_t     lastIp_[4] = {};        ///< the last sender IP the status was built from

    platform::UdpSocket artnetSocket_;   ///< ArtNet, port 6454
    platform::UdpSocket e131Socket_;     ///< E1.31, port 5568
    platform::UdpSocket ddpSocket_;      ///< DDP, port 4048
    uint8_t pkt_[1500] = {};             ///< one datagram of any protocol, DDP reaching 1450
    ChannelDataSink sink_{*this};        ///< where the frames land, held and placed
    std::atomic<uint32_t> packets_{0};   ///< packets since the last tick1s, counted by the receive path
    std::atomic<uint32_t> frames_{0};    ///< frames since the last tick1s, counted by the receive path
    uint32_t rateSinceMs_ = 0;           ///< when the counts last restarted, which the rates divide by
    platform::WorkerTask receiveTask_;        ///< drains the sockets as packets arrive, on core 1
    std::atomic<bool> receiveStop_{false};
    std::atomic<const char*> senderProto_{nullptr};   ///< the last sender's protocol, which the render tick publishes
    std::atomic<uint32_t> senderIp_{0};               ///< and its address, as the four octets in order

    static void receiveTrampoline(void* self) { static_cast<NetworkReceiveEffect*>(self)->runReceiveLoop(); }

    /// Wait for packets and drain them, until release or prepare stops the task.
    void runReceiveLoop() {
        platform::UdpSocket* sockets[3] = {&artnetSocket_, &e131Socket_, &ddpSocket_};
        while (!receiveStop_.load(std::memory_order_acquire)) {
            if (platform::UdpSocket::waitReadable(sockets, 3, kWaitMs) > 0) drainSockets<true>();
        }
    }

    // lwIP holds a frame's burst in a small mailbox, so a task on core 1 drains it as it arrives; a board without tasks drains in the tick.
    void startReceiveTask() {
        receiveStop_.store(false, std::memory_order_release);
        platform::spawnPinnedTask(receiveTask_, "mmReceive", &receiveTrampoline, this, 4096, 3, 1);
    }

    void stopReceiveTask() {
        if (!receiveTask_.impl) return;
        receiveStop_.store(true, std::memory_order_release);
        platform::stopPinnedTask(receiveTask_);
    }

    /// Drain all three sockets into staging, bounded per socket so a flood cannot hold the caller; `kWait` when the receive task drains.
    template <bool kWait>
    void drainSockets() {
        uint16_t universe = 0, dataLen = 0;
        uint32_t byteOffset = 0;
        const uint8_t* data = nullptr;
        uint8_t srcIp[4];
        for (int i = 0; i < kMaxPacketsPerDrain; i++) {
            const int n = artnetSocket_.recvFrom(pkt_, sizeof(pkt_), srcIp);
            if (n <= 0) break;
            if (parseArtDmxPacket(pkt_, static_cast<size_t>(n), universe, data, dataLen)) {
                countPacket(universe == universeStart);   // before the write, so a frame seen in the layer is already counted
                applyDmx<kWait>(universe, data, dataLen);
                recordSender("Art-Net", srcIp);
            } else if (isArtPoll(pkt_, static_cast<size_t>(n))) {
                replyToPoll(srcIp);   // make the device show up in controller node lists
            }
        }
        for (int i = 0; i < kMaxPacketsPerDrain; i++) {
            const int n = e131Socket_.recvFrom(pkt_, sizeof(pkt_), srcIp);
            if (n <= 0) break;
            if (parseE131Packet(pkt_, static_cast<size_t>(n), universe, data, dataLen)) {
                countPacket(universe == universeStart);
                applyDmx<kWait>(universe, data, dataLen);
                recordSender("E1.31", srcIp);
            }
        }
        for (int i = 0; i < kMaxPacketsPerDrain; i++) {
            const int n = ddpSocket_.recvFrom(pkt_, sizeof(pkt_), srcIp);
            if (n <= 0) break;
            bool push = false;
            if (parseDdpPacket(pkt_, static_cast<size_t>(n), byteOffset, data, dataLen, &push)) {
                countPacket(push);
                applyBytes<kWait>(byteOffset, data, dataLen);
                recordSender("DDP", srcIp);
            }
        }
    }

    // Read-modify-writes on both sides, since a module lives in PSRAM, as TryLock explains.
    /// Count one packet, and a frame when it starts or ends one.
    void countPacket(bool frame) {
        packets_.fetch_add(1, std::memory_order_relaxed);
        if (frame) frames_.fetch_add(1, std::memory_order_relaxed);
    }
    /// A per-second count as its 16-bit control, held at the top rather than wrapping.
    static uint16_t saturate16(uint32_t v) { return v > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(v); }

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
        mm::formatTo(recvStatus_, sizeof(recvStatus_), "receiving %s from %u.%u.%u.%u%s", proto,
                      static_cast<unsigned>(ip[0]), static_cast<unsigned>(ip[1]),
                      static_cast<unsigned>(ip[2]), static_cast<unsigned>(ip[3]), kPathNotes[path]);
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
