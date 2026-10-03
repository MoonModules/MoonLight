#pragma once

#include "light/drivers/DriverBase.h"

#include "light/util/ArtNetPacket.h"   // shared ArtNet wire formats (build + parse)
#include "light/util/DdpPacket.h"      // shared DDP wire format
#include "light/util/E131Packet.h"     // shared E1.31/sACN wire format
#include "core/util/Addressing.h"      // sendAddressed: unicast, multicast or broadcast, one rule for every sender
#include "core/util/HostList.h"        // parseHostList: the host-list parser (core primitive)
#include "core/util/HostResolver.h"    // a named host's address, looked up off the render thread
#include "core/util/format.h"          // formatTo: the unresolved-names status
#include "light/drivers/PinList.h"  // assignCounts: the same window-split idiom as ledsPerPin
#include "platform/platform.h"


namespace mm {

/// Output driver: streams the buffer over UDP, one driver carrying three industry protocols selected by a control. Byte layouts live beside the receiver, so the two sides cannot drift.
///
/// One driver feeds many receivers, each taking a contiguous run of the window under its own address. A wall of tubes is one driver, the twin of one LED driver fanning out to several lanes, and unicast is the default.
///
/// Prior art: MoonLight's D_NetworkOut, and the Art-Net 4, E1.31 and DDP specifications.
///
/// @moreinfo
///
/// ## Why unicast is the default
///
/// Art-Net 4 requires unicast; broadcast survives here as legacy compatibility. A universe number lives in the payload rather than a header, so a receiver parses every packet before discarding the universes it lacks. Broadcast makes each host pay for all of them. The practical ceiling is near fifteen universes, measured on the bench where a large grid starved an ESP32's network stack.
///
/// Unicast duplicates nothing when each node owns a different slice, which is the normal case. The sender emits as many packets as a broadcast stream would, each reaching only its owner.
///
/// ## Liveness
///
/// UDP is fire and forget, so a dead receiver is invisible. The send loop tolerates one rather than pretending to detect it. A failed send drops that packet, so one dark tube stalls nothing.
///
/// @card NetworkSendDriver.png
class NetworkSendDriver : public DriverBase {
public:
    /// Default to the RGB profile: network fixtures are RGB by convention, unlike the strips.
    NetworkSendDriver() { setDefaultFixtureName("RGB"); }

    /// The protocol names, index-aligned with the constants the send switch uses.
    static constexpr const char* kProtocolOptions[] = {"ArtNet", "E1.31", "DDP"};
    /// How many protocols the selector offers.
    static constexpr uint8_t kProtocolCount = 3;
    /// The E1.31 protocol index, the one with a multicast group per universe.
    static constexpr uint8_t kProtoE131 = 1;

    /// The modes each protocol allows, by protocol index: Art-Net's legacy broadcast, E1.31's native multicast, DDP unicast only, its row padded to the table's width.
    static constexpr Addressing kModes[kProtocolCount][2] = {
        {Addressing::Unicast, Addressing::Broadcast},
        {Addressing::Unicast, Addressing::Multicast},
        {Addressing::Unicast, Addressing::Unicast},
    };
    /// How many modes each protocol offers.
    static constexpr uint8_t kModeCount[kProtocolCount] = {2, 2, 1};
    /// The mode names each protocol's `addressing` select shows.
    static constexpr const char* kModeNames[kProtocolCount][2] = {
        {"unicast", "broadcast"}, {"unicast", "multicast"}, {"unicast", "unicast"},
    };

    // The universe is IN the address, which lets a snooping switch filter in hardware.
    /// The sACN multicast group for a universe.
    static void e131MulticastAddr(uint16_t universe, uint8_t out[4]) {
        out[0] = 239; out[1] = 255;
        out[2] = static_cast<uint8_t>(universe >> 8);
        out[3] = static_cast<uint8_t>(universe & 0xFF);
    }


    /// How many receivers one driver can feed: a wall of tubes, not a subnet scan.
    static constexpr uint8_t kMaxDestinations = 32;
    /// Receiver addresses or names, comma-separated; the list order is the fan-out order.
    char hosts[64] = {};
    /// Lights per host; blank splits the window evenly, as an LED driver's list does.
    char lightsPerHost[64] = {};
    /// How the packets travel, an index into the protocol's own modes.
    uint8_t addressing = 0;
    // Per-packet cost dominates wire time, so the larger DDP chunk is the fast path.
    /// The wire protocol, which selects both the packet layout and the chunking.
    uint8_t protocol = 0;
    // Emitted verbatim, with no hidden one-based adjust, so both ends must agree.
    /// The first universe the slice maps onto.
    uint16_t universeStart = 0;
    /// Send-rate ceiling (Hz); tick() rate-limits to this so a fast render tick doesn't flood the LAN.
    uint8_t fps = 50;

    /// Bind the protocol, the destinations, the universe offset, the window and the rate cap.
    void defineDriverControls() override {
        controls_.addSelect("protocol", protocol, kProtocolOptions, kProtocolCount);
        const uint8_t p = protocol < kProtocolCount ? protocol : 0;
        // Every write rebuilds the controls, so a protocol without the chosen mode falls back to unicast here.
        if (addressing >= kModeCount[p]) addressing = 0;
        controls_.addSelect("addressing", addressing, kModeNames[p], kModeCount[p]);
        controls_.setHidden(controls_.count() - 1, kModeCount[p] < 2);
        const bool unicast = mode() == Addressing::Unicast;
        controls_.addText("hosts", hosts, sizeof(hosts));
        controls_.setHidden(controls_.count() - 1, !unicast);
        controls_.addText("lightsPerHost", lightsPerHost, sizeof(lightsPerHost));
        controls_.setHidden(controls_.count() - 1, !unicast);
        controls_.addControl("universe_start", universeStart);
        addWindowControls();   // start / count: the slice of the shared buffer this sink sends
        controls_.addControl("fps", fps, 1, 120);
    }

    /// Which controls route through the prepare sweep, so the corrected buffer is re-sized.
    bool affectsPrepare(const char* name) const override {
        // Both are PARSED in prepare, so a change must re-run the sweep to re-derive the table.
        return std::strcmp(name, "hosts") == 0 || std::strcmp(name, "lightsPerHost") == 0
               || std::strcmp(name, "protocol") == 0 || std::strcmp(name, "addressing") == 0
               || isWindowControl(name) || isCorrectionControl(name);
    }

    /// The mode the packets travel by, read through the protocol's own list.
    Addressing mode() const {
        const uint8_t p = protocol < kProtocolCount ? protocol : 0;
        return kModes[p][addressing < kModeCount[p] ? addressing : 0];
    }

    /// Refresh the named hosts' addresses from the resolver, which never blocks, and the status they drive.
    void tick1s() MM_NONBLOCKING override {
        DriverBase::tick1s();
        refreshHosts();
    }


    /// Derive the stable component id from the MAC, which needs no UUID machinery.
    void setup() override {
        std::memcpy(cid_, "MoonLight\0", 10);
        platform::getMacAddress(cid_ + 10);
    }

    /// Close the socket on release, then chain to the base to clear any status this driver set.
    void release() override {
        socket_.close();
        nDest_ = 0;   // re-derived by prepare() on the next enable
        DriverBase::release();
    }

    /// Take the shared source buffer and re-size the corrected buffer for it, off the hot path.
    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
        resizeCorrected();
    }

    // All the parsing happens HERE, so tick is a bare walk of two small arrays.
    /// Open the socket, resolve the destinations and their window slices, and size the buffer.
    void prepare() override {
        socket_.open();          // idempotent: no-op if already open
        resizeCorrected();

        // Published only once EVERYTHING validates: wrong output is worse than none at all.
        Host dest[kMaxDestinations] = {};
        uint8_t n = 0;
        if (mode() == Addressing::Unicast) {
            const char* err = parseHostList(hosts, dest, kMaxDestinations, n);
            if (err) { nDest_ = 0; setStatus(err, Severity::Error); return; }
            for (uint8_t d = 0; d < n; d++)
                if (dest[d].isName()) { HostResolver::ensureStarted(); break; }   // here, where allocating is allowed
            if (n == 0) {
                // A Warning, not an Error: an unset host is unfinished, not faulty.
                nDest_ = 0;
                setStatus("set a host", Severity::Warning);
                return;
            }
        } else {
            n = 1;   // one stream of universes, to the group or to everyone, owning the whole window
        }

        // The identical rule, and helper, an LED driver uses to split its window across pins.
        nrOfLightsType winStart = 0, winLen = 0;
        if (sourceBuffer_) windowSlice(sourceBuffer_->count(), winStart, winLen);
        nrOfLightsType counts[kMaxDestinations] = {};
        const char* warn = nullptr;
        const char* err = assignCounts(mode() == Addressing::Unicast ? lightsPerHost : "", n, winLen, counts, 0, &warn);
        if (err) { nDest_ = 0; setStatus(err, Severity::Error); return; }

        // Everything validated: publish as one unit.
        std::memcpy(dest_, dest, sizeof(Host) * n);
        std::memcpy(destCounts_, counts, sizeof(nrOfLightsType) * n);
        nDest_ = n;
        prepareWarn_ = warn;
        refreshHosts();
    }

    /// Re-size the corrected buffer when the fixture profile changes the output channel count.
    void onCorrectionChanged() override {
        resizeCorrected();
    }

    /// Correct the window into the staging buffer, then chunk it into packets and send.
    void tick() MM_NONBLOCKING override {
        if (!sourceBuffer_ || !sourceBuffer_->data()) return;

        // No destination means idle: it never falls back to broadcasting the whole LAN.
        if (nDest_ == 0) return;

        // FPS limiting
        if (fps == 0) return;
        uint32_t now = platform::millis();
        uint32_t interval = 1000 / fps;
        if (now - lastSendTime_ < interval) return;
        lastSendTime_ = now;

        // A pure reader: an unwired or mismatched correction falls back to passthrough.
        const uint8_t* data;
        size_t totalBytes;
        // This sink's window slice only, so no frame is packed for lights it does not own.
        nrOfLightsType winStart, nLights;
        windowSlice(sourceBuffer_->count(), winStart, nLights);
        // Defensive even so: a stale buffer must miss the apply rather than corrupt memory.
        const uint8_t outCh = correction_.outChannels;
        if (outCh != 0 && corrected_.data()
            && corrected_.count() >= nLights
            && corrected_.channelsPerLight() >= outCh) {
            const uint8_t* src = sourceBuffer_->data();
            const uint8_t srcCh = sourceBuffer_->channelsPerLight();
            uint8_t* dst = corrected_.data();
            for (nrOfLightsType i = 0; i < nLights; i++) {
                // The source stride lets a wide light hand its motion channels through.
                correction_.apply(src + (winStart + i) * srcCh, dst + i * outCh, srcCh);
            }
            data = dst;
            totalBytes = static_cast<size_t>(nLights) * outCh;
        } else {
            // The same window as the corrected path, so a sliced sink sends only its lights.
            const uint8_t srcCh = sourceBuffer_->channelsPerLight();
            data = sourceBuffer_->data() + static_cast<size_t>(winStart) * srcCh;
            totalBytes = static_cast<size_t>(nLights) * srcCh;
        }

        // Rounded DOWN to whole fixtures: one straddling two universes reads a neighbor's channels.
        size_t chunk = (protocol == 2) ? DDP_MAX_PAYLOAD : MAX_CHANNELS_PER_UNIVERSE;
        uint8_t packet[DDP_HEADER_SIZE + DDP_MAX_PAYLOAD];  // 1450 B covers all three
        const uint16_t port = protocolPort(protocol);
        const uint8_t bytesPerLight = (data == corrected_.data() && correction_.outChannels)
                                          ? correction_.outChannels
                                          : sourceBuffer_->channelsPerLight();
        if (protocol != 2 && bytesPerLight > 1) {
            const size_t whole = (chunk / bytesPerLight) * bytesPerLight;
            // A fixture wider than a universe keeps the full one, so the failure is visible.
            if (whole > 0) chunk = whole;
        }

        const Addressing sendMode = mode();
        size_t offset = 0;   // byte cursor into `data`, walking destination by destination
        for (uint8_t d = 0; d < nDest_ && offset < totalBytes; d++) {
            // This destination's run: its light count × the wire stride, clipped to what's left.
            size_t runBytes = static_cast<size_t>(destCounts_[d]) * bytesPerLight;
            if (offset + runBytes > totalBytes) runBytes = totalBytes - offset;
            if (runBytes == 0) continue;   // a zero-count destination is configured but idle

            uint16_t universe = universeStart;   // restart per destination
            size_t sent = 0;
            while (sent < runBytes) {
                const size_t n = std::min(runBytes - sent, chunk);
                const uint8_t* src = data + offset + sent;
                size_t packetLen;
                switch (protocol) {
                    case kProtoE131:
                        packetLen = buildE131Packet(packet, universe, sequence_, cid_,
                                                    src, static_cast<uint16_t>(n));
                        break;
                    case 2:
                        // Byte-addressed, and relative to this destination's own strip.
                        packetLen = buildDdpPacket(packet, static_cast<uint32_t>(sent),
                                                   /*push=*/sent + n >= runBytes,
                                                   src, static_cast<uint16_t>(n));
                        break;
                    default:
                        packetLen = buildArtDmxPacket(packet, universe, sequence_,
                                                      src, static_cast<uint16_t>(n));
                        break;
                }
                // A failed send drops that packet and continues: one dark tube stalls no other.
                uint8_t grp[4];
                e131MulticastAddr(universe, grp);
                sendAddressed(sendMode, Traffic::FrameRate, &dest_[d], 1, grp,
                              [&](const uint8_t ip[4]) { socket_.sendToAddr(ip, port, packet, packetLen); });
                sent += n;
                universe++;
            }
            offset += runBytes;
        }

        sequence_++;
    }

    // The packet builds and their inverse parses are shared with the receiver, defined once.

    /// Test-only accessor for the corrected buffer, pinning the no-allocation contract.
    const Buffer& correctedBuffer() const { return corrected_; }

    /// How many receivers the parse derived, for a test pinning the fan-out arithmetic.
    uint8_t destinationCount() const { return nDest_; }
    /// The address of destination `i`, 0.0.0.0 for a name not resolved yet.
    const uint8_t* destinationAt(uint8_t i) const { return dest_[i].ip; }
    /// How many lights of the window destination `i` owns.
    nrOfLightsType lightsAt(uint8_t i) const { return destCounts_[i]; }

private:
    /// The send socket, reused for every destination and closed on release.
    platform::UdpSocket socket_;
    /// The shared frame this driver reads its window from; borrowed, not owned.
    Buffer* sourceBuffer_ = nullptr;
    /// Owned: source bytes after brightness/order/white. Sized off the hot path (resizeCorrected).
    Buffer corrected_;
    /// Art-Net/E1.31 per-frame sequence counter; wraps at 255, which both protocols expect.
    uint8_t sequence_ = 0;
    /// millis() of the last frame sent: the `fps` rate limiter's reference point.
    uint32_t lastSendTime_ = 0;
    /// E1.31 component id, built once in setup() from the MAC so it is stable per device.
    uint8_t cid_[E131_CID_LENGTH] = {};
    /// Destination hosts, derived in prepare() (never in tick()); a name's address is refreshed each second.
    Host dest_[kMaxDestinations] = {};
    /// Each destination's slice of the window, index-aligned with `dest_`.
    nrOfLightsType destCounts_[kMaxDestinations] = {};
    /// How many entries of `dest_` / `destCounts_` are live.
    uint8_t nDest_ = 0;
    /// The unresolved-names warning, which the status points at.
    char statusBuf_[32] = {};
    /// The light-count warning prepare found, restored once the names resolve.
    const char* prepareWarn_ = nullptr;

    /// The UDP port for a protocol index: each wire format has its own registered port.
    static uint16_t protocolPort(uint8_t p) {
        return p == kProtoE131 ? E131_PORT : p == 2 ? DDP_PORT : ARTNET_PORT;
    }

    /// Copy each named host's latest address in from the resolver, keeping the last one while it is busy or failing, then report.
    void refreshHosts() {
        if (nDest_ == 0) return;   // idle or refused: prepare's own status stands
        uint8_t waiting = 0;
        for (uint8_t d = 0; d < nDest_; d++) {
            if (!dest_[d].isName()) continue;
            char name[kMaxHostName + 1];
            hostName(hosts, dest_[d], name);
            uint8_t ip[4];
            const HostResolver::State st = HostResolver::lookup(name, ip);
            if (st == HostResolver::State::Busy) return;   // the lookup task held the cache this instant: keep the status as it is
            if (st == HostResolver::State::Resolved || st == HostResolver::State::Stale) std::memcpy(dest_[d].ip, ip, 4);
            if (st == HostResolver::State::Waiting || st == HostResolver::State::Full) waiting++;
        }
        if (waiting) {
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "%u host%s not resolved yet", waiting, waiting == 1 ? "" : "s");
            setStatus(statusBuf_, Severity::Warning);
        } else if (mode() == Addressing::Multicast && NetworkPath::multicastDropped) {
            // A light stream never falls back to broadcast, which would flood every device on the LAN.
            setStatus("multicast blocked here: use unicast", Severity::Warning);
        } else {
            setStatus(prepareWarn_, prepareWarn_ ? Severity::Warning : Severity::Status);
        }
    }

    /// Size the corrected buffer, off the hot path, which is the no-allocation contract.
    void resizeCorrected() {
        if (!sourceBuffer_) return;
        // Sized for the window slice, using the same resolver the send loop does.
        nrOfLightsType winStart, n;
        windowSlice(sourceBuffer_->count(), winStart, n);
        const uint8_t ch = correction_.outChannels;
        if (n == 0 || ch == 0) return;
        if (corrected_.count() >= n && corrected_.channelsPerLight() >= ch) return;
        corrected_.allocate(n, ch);
    }
};

} // namespace mm
