#pragma once

#include "light/effects/EffectBase.h"

#include "light/util/ArtNetPacket.h"   // shared ArtNet wire formats (build + parse)
#include "light/util/DdpPacket.h"      // shared DDP wire format
#include "light/util/E131Packet.h"     // shared E1.31/sACN wire format
#include "light/layers/Layer.h"   // Layer::bufferGen: is the held frame still ours?
#include "platform/platform.h"    // platform::UdpSocket: the three receive sockets

namespace mm {

// Author: MoonLight original (E1.31 / Art-Net receive)
/// Effect that paints the layer from received Art-Net/E1.31/DDP pixels.
/// @card NetworkReceiveEffect.gif
///
/// External light data is another module writing into the layer buffer.
/// So it composes with modifiers and blending like any generated effect.
/// This is the end-to-end pair with NetworkSendDriver, and the receive side for industry senders.
///
/// Prior art: MoonLight's D_NetworkIn, WLED's realtime UDP input, and MoonLight v1's ArtNetInModule.
///
/// @moreinfo
///
/// ## Three protocols at once
///
/// The effect binds ArtNet 6454, E1.31 5568 and DDP 4048, validating each packet against its format.
/// There is deliberately no protocol control: whatever a sender speaks works.
/// The status field then shows which protocol is arriving and from where.
/// Controllers find the device by broadcasting ArtPoll, which this answers with ArtPollReply.
///
/// ## Hold-last-frame, through a staging buffer
///
/// Packets drain into an owned staging buffer, which is copied to the layer each tick.
/// Without that the lights would strobe black between frames.
/// The drain is non-blocking and bounded per tick, so a packet flood cannot wedge the render loop.
/// Sequence fields and DDP's push flag are ignored, so the last write into staging wins.
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

    /// Publish the universe start and the per-universe stride.
    void defineControls() override {
        controls_.addControl("universe_start", universeStart);
        controls_.addControl("channels_per_universe", channelsPerUniverse);
    }

    /// Close the three sockets, then chain so the staging buffer is freed too.
    void release() override {
        // Chaining is required, or the registered staging buffer leaks on disable.
        artnetSocket_.close();
        e131Socket_.close();
        ddpSocket_.close();
        MoonModule::release();
        clearStatus();
    }

    /// Bind the three receive sockets independently, and size the staging buffer to the layer.
    void prepare() override {
        const bool artnetOk = artnetSocket_.open() && artnetSocket_.bind(ARTNET_PORT);
        const bool e131Ok = e131Socket_.open() && e131Socket_.bind(E131_PORT);
        const bool ddpOk = ddpSocket_.open() && ddpSocket_.bind(DDP_PORT);
        if (artnetOk && e131Ok && ddpOk) {
            if (status() == kBindFailMsg) clearStatus();
        } else {
            setStatus(kBindFailMsg, Severity::Error);
        }
        // One byte per channel byte, zero-filled so a fresh grid starts dark.
        staging_.resize(static_cast<size_t>(nrOfLights()) * channelsPerLight());
        dirty_ = true;   // a resize (or a re-enable) must repaint the layer from staging once
    }

    /// Drain all three sockets, then copy staging to the layer when anything changed.
    void tick() MM_NONBLOCKING override {
        if (!staging_) return;
        // Bounded per socket, so a flood waits in the socket buffers rather than wedging the tick.
        uint16_t universe = 0, dataLen = 0;
        uint32_t byteOffset = 0;
        const uint8_t* data = nullptr;
        uint8_t srcIp[4];
        for (int i = 0; i < kMaxPacketsPerTick; i++) {
            const int n = artnetSocket_.recvFrom(pkt_, sizeof(pkt_), srcIp);
            if (n <= 0) break;
            if (parseArtDmxPacket(pkt_, static_cast<size_t>(n), universe, data, dataLen)) {
                applyDmx(universe, data, dataLen);
                noteReceiving("Art-Net", srcIp);
            } else if (isArtPoll(pkt_, static_cast<size_t>(n))) {
                replyToPoll(srcIp);   // make the device show up in controller node lists
            }
        }
        for (int i = 0; i < kMaxPacketsPerTick; i++) {
            const int n = e131Socket_.recvFrom(pkt_, sizeof(pkt_), srcIp);
            if (n <= 0) break;
            if (parseE131Packet(pkt_, static_cast<size_t>(n), universe, data, dataLen)) {
                applyDmx(universe, data, dataLen);
                noteReceiving("E1.31", srcIp);
            }
        }
        for (int i = 0; i < kMaxPacketsPerTick; i++) {
            const int n = ddpSocket_.recvFrom(pkt_, sizeof(pkt_), srcIp);
            if (n <= 0) break;
            if (parseDdpPacket(pkt_, static_cast<size_t>(n), byteOffset, data, dataLen)) {
                applyBytes(byteOffset, data, dataLen);
                noteReceiving("DDP", srcIp);
            }
        }
        // An unchanged buffer needs no copy, so the layer's write generation decides.
        uint8_t* buf = buffer();
        if (!buf) return;
        const auto* layer = static_cast<const Layer*>(parent());
        const uint32_t gen = layer ? layer->bufferGen() : 0;
        if (!dirty_ && layer && gen == lastGen_) return;
        dirty_ = false;
        const size_t bufBytes = static_cast<size_t>(nrOfLights()) * channelsPerLight();
        std::memcpy(buf, staging_.data(), staging_.bytes() < bufBytes ? staging_.bytes() : bufBytes);
        // The Layer bumps the generation after this tick, so record what our own copy produces.
        lastGen_ = gen + 1;
    }

    /// Place one universe's payload, clamped to its stride and ignoring universes outside the buffer.
    void applyDmx(uint16_t universe, const uint8_t* data, uint16_t len) {
        if (universe < universeStart || channelsPerUniverse == 0) return;
        if (len > channelsPerUniverse) len = channelsPerUniverse;
        applyBytes(static_cast<size_t>(universe - universeStart) * channelsPerUniverse,
                   data, len);
    }

    /// The one clamped write into staging, bound-checked before any addition against an overflow.
    void applyBytes(size_t offset, const uint8_t* data, uint16_t len) {
        if (!staging_ || offset >= staging_.bytes()) return;
        size_t n = len;
        if (offset + n > staging_.bytes()) n = staging_.bytes() - offset;
        std::memcpy(staging_.data() + offset, data, n);
        dirty_ = true;   // the one write into staging, which is what tick() copies out
    }

    /// Test seam: the staging bytes, so a test can pin the buffer's lifecycle.
    const uint8_t* stagingData() const { return staging_.data(); }
    /// Test seam: how many staging bytes are allocated.
    size_t stagingBytes() const { return staging_.bytes(); }

private:
    static constexpr int kMaxPacketsPerTick = 128;
    static constexpr const char* kBindFailMsg = "UDP bind failed, port in use?";
    // A member rather than a stack buffer: setStatus holds the pointer, so the string must outlive it.
    char recvStatus_[40] = "";          ///< "receiving <protocol> from <ip>", sized for the longest form
    const char* lastProto_ = nullptr;   ///< the last protocol literal the status was built from
    uint8_t     lastIp_[4] = {};        ///< the last sender IP the status was built from

    platform::UdpSocket artnetSocket_;   ///< ArtNet, port 6454
    platform::UdpSocket e131Socket_;     ///< E1.31, port 5568
    platform::UdpSocket ddpSocket_;      ///< DDP, port 4048
    uint8_t pkt_[1500] = {};             ///< one datagram of any protocol, DDP reaching 1450
    ScratchBuffer<uint8_t> staging_{*this};   ///< the held frame, sized to the layer
    /// Staging changed since the last copy, starting true so a fresh buffer paints once.
    bool dirty_ = true;
    /// The layer's write generation as of our last copy, which is what makes skipping it safe.
    uint32_t lastGen_ = 0;

    /// Update the receiving diagnostic, short-circuiting the common case since this is the hot path.
    void noteReceiving(const char* proto, const uint8_t ip[4]) {
        const char* s = status();
        if (s != nullptr && s != recvStatus_) return;   // a bind error (or foreign status) wins
        // Already showing this source, so skip both the format and the setStatus.
        if (s == recvStatus_ && proto == lastProto_ && std::memcmp(ip, lastIp_, 4) == 0) return;
        lastProto_ = proto;
        std::memcpy(lastIp_, ip, 4);
        std::snprintf(recvStatus_, sizeof(recvStatus_), "receiving %s from %u.%u.%u.%u", proto,
                      static_cast<unsigned>(ip[0]), static_cast<unsigned>(ip[1]),
                      static_cast<unsigned>(ip[2]), static_cast<unsigned>(ip[3]));
        setStatus(recvStatus_, Severity::Status);
    }

    /// Answer an ArtPoll with our address and name, so controllers list the device.
    void replyToPoll(const uint8_t pollerIp[4]) {
        uint8_t myIp[4];
        platform::ethGetIPv4(myIp);
        if (!myIp[0] && !myIp[1] && !myIp[2] && !myIp[3]) platform::wifiStaGetIPv4(myIp);
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
