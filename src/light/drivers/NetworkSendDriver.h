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

#include <atomic>


namespace mm {

/// Streams the buffer over UDP as Art-Net, E1.31 or DDP, its packets paced so receivers keep up, to Falcon and Advatek controllers, xLights and LedFx.
/// @card NetworkSendDriver.png
///
/// One driver feeds many receivers, each a run of the window at its own address, as an LED driver fans out over pins.
///
/// @attribution Art-Net, E1.31, DDP, troyhacks
///
/// @guide
///
/// ## Mixed fixtures
///
/// Mixed fixtures take a driver per fixture type, each with its own `start`, `count` and `fixture`, the richest motion fixture first.
///
/// | driver | window | fixtures |
/// |---|---|---|
/// | A | `start 0`, `count 2` | the two big pan/tilt/zoom heads |
/// | B | `start 2`, `count 4` | four smaller pan/tilt heads |
/// | C | `start 6`, `count 10` | ten RGBW pars |
///
/// @technical
///
/// **Wire formats.** Byte layouts live beside the receiver, so the two sides cannot drift. DDP's larger chunk is the fast path, since per-packet cost dominates wire time.
///
/// **Unicast.** A receiver parses every broadcast packet before dropping the universes it lacks, which starved an ESP32 near fifteen universes. A failed unicast send drops only that packet, so one dark tube stalls nothing.
///
/// ## Paced packets, sent from a task
///
/// A burst loses packets at an access point and at a wire-speed receiver, so `packet_gap` spaces them. The tick hands the frame to a task and skips frames meanwhile; a gap of 0 or a board without tasks sends the burst from the tick.
///
/// **Mixed fixtures.** `lightsPerHost` splits a window between receivers that share one fixture profile, so differing fixtures need a driver each. Motion slots come from the first driver with a motion profile, so the richest fixture leads.
///
/// **Motion.** Every light carries the motion bytes, so a mixed rig's buffer is as wide as its widest fixture. An effect's `setPan` reaches every light in its layer, so use separate Layers when heads should move independently.
class NetworkSendDriver : public DriverBase {
public:
    /// Default to the RGB profile: network fixtures are RGB by convention, unlike the strips.
    NetworkSendDriver() { setDefaultFixtureName("RGB"); }

    /// The protocol names, index-aligned with the constants the send switch uses.
    static constexpr const char* kProtocolOptions[] = {"ArtNet", "E1.31", "DDP"};
    /// The protocols `protocol` picks from, in its option order; E1.31 is the one with a multicast group per universe.
    enum Protocol : uint8_t { kProtoArtNet, kProtoE131, kProtoDdp, kProtocolCount };

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

    /// The sACN multicast group for a universe: the universe is in the address, which lets a snooping switch filter in hardware.
    static void e131MulticastAddr(uint16_t universe, uint8_t out[4]) {
        out[0] = 239; out[1] = 255;
        out[2] = static_cast<uint8_t>(universe >> 8);
        out[3] = static_cast<uint8_t>(universe & 0xFF);
    }


    /// How many receivers one driver can feed: a wall of tubes, not a subnet scan.
    static constexpr uint8_t kMaxDestinations = 32;
    /// Receiver addresses or names, in fan-out order; blank idles.
    /// @guide A name resolves by mDNS for `.local`, else by DNS; it sends once resolved, keeps its last address while silent, and the status names the ones waiting.
    char hosts[64] = {};
    /// Lights per receiver; blank splits evenly, one number to all, a list each.
    char lightsPerHost[64] = {};
    /// How packets travel: unicast, Art-Net broadcast or E1.31 multicast.
    /// @guide Art-Net 4 requires unicast; broadcast loads every host. E1.31 `multicast` needs [IGMP snooping](https://moonmodules.org/MoonLight/explanation/architecture/moonlight.html#multicast-and-igmp-snooping).
    uint8_t addressing = 0;
    /// The wire protocol: Art-Net, E1.31 or DDP; the port follows.
    uint8_t protocol = 0;
    /// The first universe, sent as is; DDP ignores it.
    uint16_t universeStart = 0;
    /// Channels each Art-Net or E1.31 universe carries, exactly; DDP ignores it.
    /// @guide A light stays in one universe when its channels divide this: **510** does for RGB, RGBWW and 6 channels, **512** fills every universe. RGBW takes 508, a 13-channel head 507.
    uint16_t channelsPerUniverse = static_cast<uint16_t>(MAX_CHANNELS_PER_UNIVERSE);
    /// Frame-rate limit, so a fast render tick does not flood the LAN.
    /// @guide It sets the load, fps times a frame's packets a second: lowering it eases a receiver.
    uint8_t fps = 50;
    /// Microseconds from the start of one packet to the next; 0 sends a burst.
    /// @guide The pause lets a receiver keep up, @xref{paced-packets-sent-from-a-task|sent from a task}. A frame fills at most 80% of its period, so a high fps or a large grid shrinks the gap.
    uint16_t packetGap = 500;

    /// It sends a slice of the shared buffer.
    bool hasWindowControls() const override { return true; }

    /// Bind the protocol, the destinations, the universe offset and the rate cap.
    void defineDriverControls() override {
        controls_.addSelect("protocol", protocol, kProtocolOptions, kProtocolCount);
        const uint8_t p = protocol < kProtocolCount ? protocol : 0;
        // Every write rebuilds the controls, so a protocol without the chosen mode falls back to unicast here.
        if (addressing >= kModeCount[p]) addressing = 0;
        controls_.addSelect("addressing", addressing, kModeNames[p], kModeCount[p]);
        controls_.setHidden(controls_.count() - 1, kModeCount[p] < 2);
        const bool unicast = mode() == Addressing::Unicast;
        controls_.addText("hosts", hosts, sizeof(hosts));
        controls_.setHostsField(controls_.count() - 1);
        controls_.setHidden(controls_.count() - 1, !unicast);
        controls_.addText("lightsPerHost", lightsPerHost, sizeof(lightsPerHost));
        controls_.setHidden(controls_.count() - 1, !unicast);
        controls_.addControl("universe_start", universeStart);
        controls_.addControl("channels_per_universe", channelsPerUniverse, 1, 512);
        controls_.addControl("fps", fps, 1, 120);
        controls_.addControl("packet_gap", packetGap, 0, 5000);
    }

    /// Which controls route through the prepare sweep, so the corrected buffer is re-sized.
    bool affectsPrepare(const char* name) const override {
        // Both are PARSED in prepare, so a change must re-run the sweep to re-derive the table.
        return std::strcmp(name, "hosts") == 0 || std::strcmp(name, "lightsPerHost") == 0
               || std::strcmp(name, "protocol") == 0 || std::strcmp(name, "addressing") == 0
               || std::strcmp(name, "packet_gap") == 0   // starts or stops the send task
               || isWindowControl(name) || isCorrectionControl(name);
    }

    /// The mode the packets travel by, read through the protocol's own list.
    Addressing mode() const {
        const uint8_t p = protocol < kProtocolCount ? protocol : 0;
        return kModes[p][addressing < kModeCount[p] ? addressing : 0];
    }

    /// Refresh the named hosts' addresses from the resolver, which never blocks, and the status: a warning, or what went out this second.
    void tick1s() MM_NONBLOCKING override {
        DriverBase::tick1s();
        const uint32_t frames = framesSent_.load(std::memory_order_relaxed);
        const uint32_t packets = packetsSent_.load(std::memory_order_relaxed);
        framesLastSecond_ = frames - framesAtSecond_;
        packetsLastSecond_ = packets - packetsAtSecond_;
        framesAtSecond_ = frames;
        packetsAtSecond_ = packets;
        refreshHosts();
    }


    /// Derive the stable component id from the MAC, which needs no UUID machinery.
    void setup() override {
        std::memcpy(cid_, "MoonLight\0", 10);
        platform::getMacAddress(cid_ + 10);
    }

    /// A task still running would read a destroyed object, so the destructor stops it too.
    ~NetworkSendDriver() override { stopSendTask(); }

    /// How many times the send task was started, so a test can tell a running stream from a restarted one.
    uint32_t taskStartsForTest() const { return taskStarts_; }

    /// Stop the send task and close the socket on release, then chain to the base, which clears the status.
    void release() override {
        stopSendTask();
        socket_.close();
        nDest_ = 0;   // re-derived by prepare() on the next enable
        DriverBase::release();
    }

    /// Take the shared source buffer and re-size the corrected buffer for it, off the hot path.
    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
        resizeCorrected();
    }

    /// Open the socket, resolve the destinations and their window slices, and size the buffer; all parsing happens here, so tick is a bare walk of two small arrays.
    void prepare() override {
        socket_.open();          // idempotent: no-op if already open
        resizeCorrected();       // which waits out a running task only when it reallocates

        // Published only once EVERYTHING validates: wrong output is worse than none at all.
        Host dest[kMaxDestinations] = {};
        uint8_t n = 0;
        if (mode() == Addressing::Unicast) {
            const char* err = parseHostList(hosts, dest, kMaxDestinations, n);
            if (err) { stopSendTask(); nDest_ = 0; setStatus(err, Severity::Error); return; }
            for (uint8_t d = 0; d < n; d++)
                if (dest[d].isName()) { HostResolver::ensureStarted(); break; }   // here, where allocating is allowed
            if (n == 0) {
                // A Warning, not an Error: an unset host is unfinished, not faulty.
                stopSendTask();
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
        if (err) { stopSendTask(); nDest_ = 0; setStatus(err, Severity::Error); return; }

        publishDestinations(dest, counts, n);
        prepareWarn_ = warn;
        refreshHosts();
        if (packetGap > 0) startSendTask();   // a no-op while it runs
        else stopSendTask();
    }

    /// Publish validated destinations as one unit. The send task reads them, so it stops only when they differ, and a rebuild elsewhere leaves a running stream alone.
    void publishDestinations(const Host* dest, const nrOfLightsType* counts, uint8_t n) {
        // By the text and mode they came from, since the resolver fills in a name's address later.
        if (n == nDest_ && mode() == preparedMode_ && std::strcmp(hosts, preparedHosts_) == 0
            && std::memcmp(destCounts_, counts, sizeof(nrOfLightsType) * n) == 0) return;
        stopSendTask();
        std::memcpy(dest_, dest, sizeof(Host) * n);
        std::memcpy(destCounts_, counts, sizeof(nrOfLightsType) * n);
        nDest_ = n;
        preparedMode_ = mode();
        std::snprintf(preparedHosts_, sizeof(preparedHosts_), "%s", hosts);
    }

    /// Re-size the corrected buffer when the fixture profile changes the output channel count.
    void onCorrectionChanged() override {
        resizeCorrected();
    }

    /// Correct the window into the staging buffer, then chunk it into packets and send.
    void tick() MM_NONBLOCKING override {
        // The render's frame period, smoothed over about eight frames, which the pacing spreads a frame across.
        const uint32_t nowUs = platform::micros();
        if (lastTickUs_) {
            const uint32_t sample = nowUs - lastTickUs_ < 1000000u ? nowUs - lastTickUs_ : 1000000u;   // a stall counts as one second at most
            const uint32_t period = framePeriodUs_.load(std::memory_order_relaxed);
            framePeriodUs_.store((period * 7 + sample) / 8, std::memory_order_relaxed);
        }
        lastTickUs_ = nowUs;
        if (!sourceBuffer_ || !sourceBuffer_->data()) return;

        // No destination means idle: it never falls back to broadcasting the whole LAN.
        if (nDest_ == 0) return;
        // The task still sending the last frame owns the corrected buffer, so this frame is skipped.
        if (sendTask_.impl && frameState_.load(std::memory_order_acquire) != kFrameIdle) return;

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

        const uint8_t bytesPerLight = (data == corrected_.data() && correction_.outChannels)
                                          ? correction_.outChannels
                                          : sourceBuffer_->channelsPerLight();
        // Paced from the task when it runs and the frame sits in the corrected buffer, which the task owns until it is done.
        if (sendTask_.impl && data == corrected_.data()) {
            frame_ = {data, totalBytes, bytesPerLight};
            frameState_.exchange(kFrameReady, std::memory_order_acq_rel);
            platform::notifyTask(sendTask_);
            return;
        }
        sendFrame<false>(data, totalBytes, bytesPerLight, 0);
    }

    /// Test-only accessor for the corrected buffer, pinning the no-allocation contract.
    const Buffer& correctedBuffer() const { return corrected_; }

    /// The slot spacing: the gap, shortened so a frame's slots fit in 80% of its period, leaving room for the next.
    uint32_t effectiveGap(size_t totalBytes) const {
        const size_t chunk = packetPayload();
        // An upper bound: one partial packet per destination on top of the whole ones.
        const uint32_t packets = static_cast<uint32_t>((totalBytes + chunk - 1) / chunk) + nDest_;
        const uint32_t budget = pacingPeriodUs(fps, framePeriodUs_.load(std::memory_order_relaxed)) / 5 * 4;
        const uint32_t fit = packets > 1 ? budget / (packets - 1) : budget;
        return packetGap < fit ? packetGap : fit;
    }

    /// The bytes one packet carries: a DDP packet's most, or a universe's channels, clamped to what a universe holds.
    size_t packetPayload() const {
        if (protocol == kProtoDdp) return DDP_MAX_PAYLOAD;
        return channelsPerUniverse >= 1 && channelsPerUniverse <= 512 ? channelsPerUniverse : MAX_CHANNELS_PER_UNIVERSE;
    }

    /// The period a frame is paced across: the render's own when it is slower than `fps` allows, else the `fps` one.
    static uint32_t pacingPeriodUs(uint8_t fps, uint32_t framePeriodUs) {
        const uint32_t capped = fps ? 1000000u / fps : 0;
        return framePeriodUs > capped ? framePeriodUs : capped;
    }

    /// Microseconds to wait before packet `i` of a frame begun at `startUs`, slots `spacingUs` apart: what remains until its slot, 0 when a slow send already used it up. Wraps with micros().
    static uint32_t slotWait(uint32_t startUs, uint32_t i, uint32_t spacingUs, uint32_t nowUs) {
        const int32_t left = static_cast<int32_t>(startUs + i * spacingUs - nowUs);
        return left > 0 ? static_cast<uint32_t>(left) : 0;
    }

    /// The universe sequence after `seq`: E1.31 runs 0 to 255, Art-Net 1 to 255 since its 0 means none.
    static uint8_t nextSequence(uint8_t seq, uint8_t protocol) {
        if (seq < 255) return static_cast<uint8_t>(seq + 1);
        return protocol == kProtoE131 ? 0 : 1;
    }

    /// Whether a task paces the packets, rather than the tick sending a burst: the test seam for the fallback.
    bool sendsInTask() const { return sendTask_.impl != nullptr; }

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
    /// Art-Net/E1.31 per-frame sequence counter, so each universe counts up once a frame, as nextSequence steps it.
    uint8_t sequence_ = 1;
    /// DDP's per-packet sequence counter, 1 to 15.
    uint8_t ddpSequence_ = 1;
    /// millis() of the last frame sent: the `fps` rate limiter's reference point.
    uint32_t lastSendTime_ = 0;
    /// micros() of the last tick, and the smoothed time between ticks, the render's frame period.
    uint32_t lastTickUs_ = 0;
    std::atomic<uint32_t> framePeriodUs_{0};   ///< written by the tick, read by the send task
    /// E1.31 component id, built once in setup() from the MAC so it is stable per device.
    uint8_t cid_[E131_CID_LENGTH] = {};
    /// Destination hosts, derived in prepare() (never in tick()); a name's address is refreshed each second.
    Host dest_[kMaxDestinations] = {};
    /// Each destination's slice of the window, index-aligned with `dest_`.
    nrOfLightsType destCounts_[kMaxDestinations] = {};
    /// How many entries of `dest_` / `destCounts_` are live.
    uint8_t nDest_ = 0;
    /// The status line the buffer holds: an unresolved-names warning, or what went out this second.
    char statusBuf_[64] = {};
    /// Frames and packets sent since start, by the tick or the task, which tick1s turns into rates.
    std::atomic<uint32_t> framesSent_{0}, packetsSent_{0};
    /// The last frame's packet count and slot spacing, for the status.
    std::atomic<uint32_t> packetsPerFrame_{0}, packetSpacing_{0};
    uint32_t framesAtSecond_ = 0, packetsAtSecond_ = 0;       ///< the counts at the last tick1s
    uint32_t framesLastSecond_ = 0, packetsLastSecond_ = 0;   ///< sent in the second before it
    /// The light-count warning prepare found, restored once the names resolve.
    const char* prepareWarn_ = nullptr;

    static constexpr uint8_t kFrameIdle = 0;      ///< the tick may correct the next frame
    static constexpr uint8_t kFrameReady = 1;     ///< a corrected frame waits for the task
    static constexpr uint8_t kFrameSending = 2;   ///< the task is sending it
    static constexpr uint8_t kSendRetries = 3;          ///< tries after a failed send, while the transmit queue drains
    static constexpr uint32_t kRetryBackoffUs = 500;    ///< the wait before each of them
    /// The task that paces a frame's packets, running while `packetGap` is above 0.
    platform::WorkerTask sendTask_;
    /// Who owns the corrected buffer: the tick while idle, the task while ready or sending; every write is a read-modify-write, since a module lives in PSRAM, as TryLock explains.
    std::atomic<uint8_t> frameState_{kFrameIdle};
    /// Set to end the send task, read before each paced packet.
    std::atomic<bool> sendStop_{false};
    /// One packet being built, 1450 B covering all three protocols, a member so the send task's stack stays small; the tick and the task never send at once.
    uint8_t packet_[DDP_HEADER_SIZE + DDP_MAX_PAYLOAD] = {};
    /// The frame the tick hands over: where its bytes are, how many, and their stride.
    struct Frame { const uint8_t* data; size_t totalBytes; uint8_t bytesPerLight; } frame_ = {nullptr, 0, 0};

    static void sendTrampoline(void* self) { static_cast<NetworkSendDriver*>(self)->runSendLoop(); }

    /// Wait for a frame from the tick, then send it paced, until release or prepare stops the task.
    void runSendLoop() {
        while (!sendStop_.load(std::memory_order_acquire)) {
            uint8_t ready = kFrameReady;
            if (!frameState_.compare_exchange_strong(ready, kFrameSending, std::memory_order_acq_rel)) {
                platform::waitNotify(sendTask_, 100);
                continue;
            }
            sendFrame<true>(frame_.data, frame_.totalBytes, frame_.bytesPerLight, effectiveGap(frame_.totalBytes));
            frameState_.exchange(kFrameIdle, std::memory_order_acq_rel);
        }
    }

    /// Start the paced send task on core 1, beside the receive task; a board that cannot start it sends from the tick, unpaced.
    void startSendTask() {
        if (sendTask_.impl) return;
        taskStarts_++;
        sendStop_.store(false, std::memory_order_release);
        frameState_.exchange(kFrameIdle, std::memory_order_acq_rel);
        // Priority 7, above the encode (5) and capture (6) tasks on core 1: it wakes every gap, the shortest period there (rate-monotonic priority).
        platform::spawnPinnedTask(sendTask_, "mmSend", &sendTrampoline, this, 4096, 7, 1);
    }

    void stopSendTask() {
        if (!sendTask_.impl) return;
        sendStop_.store(true, std::memory_order_release);
        platform::stopPinnedTask(sendTask_);
        frameState_.exchange(kFrameIdle, std::memory_order_acq_rel);
    }

    /// Chunk a frame into packets and send them, each in its slot `spacingUs` apart from the task (`kPaced`) and back to back from the tick, whose instance holds no wait.
    template <bool kPaced>
    void sendFrame(const uint8_t* data, size_t totalBytes, uint8_t bytesPerLight, uint32_t spacingUs) {
        const size_t chunk = packetPayload();
        uint8_t* packet = packet_;
        const uint16_t port = protocolPort(protocol);

        const Addressing sendMode = mode();
        uint32_t packets = 0;   // sent so far this frame: the first goes out at once, and the status reports the count
        const uint32_t frameStart = kPaced ? platform::micros() : 0;   // slot 0, which the others count from
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
                    case kProtoDdp:
                        // Byte-addressed, and relative to this destination's own strip.
                        packetLen = buildDdpPacket(packet, static_cast<uint32_t>(sent),
                                                   /*push=*/sent + n >= runBytes,
                                                   src, static_cast<uint16_t>(n), ddpSequence_);
                        ddpSequence_ = ddpSequence_ == DDP_SEQUENCE_MAX ? 1 : ddpSequence_ + 1;
                        break;
                    default:
                        packetLen = buildArtDmxPacket(packet, universe, sequence_,
                                                      src, static_cast<uint16_t>(n));
                        break;
                }
                // Every packet but the first waits for its slot, then a failed send is retried after a backoff when paced, else dropped: one dark tube stalls no other.
                if constexpr (kPaced) {
                    if (spacingUs && packets > 0) {
                        const uint32_t wait = slotWait(frameStart, packets, spacingUs, platform::micros());
                        if (wait) platform::sleepUs(sendTask_, wait);
                    }
                    if (sendStop_.load(std::memory_order_acquire)) return;
                }
                uint8_t grp[4];
                e131MulticastAddr(universe, grp);
                sendAddressed(sendMode, Traffic::FrameRate, &dest_[d], 1, grp, [&](const uint8_t ip[4]) {
                    bool ok = socket_.sendToAddr(ip, port, packet, packetLen);
                    if constexpr (kPaced) {
                        for (uint8_t r = 0; !ok && r < kSendRetries; r++) {
                            platform::sleepUs(sendTask_, kRetryBackoffUs);
                            ok = socket_.sendToAddr(ip, port, packet, packetLen);
                        }
                    }
                });
                packets++;
                sent += n;
                universe++;
            }
            offset += runBytes;
        }

        sequence_ = nextSequence(sequence_, protocol);
        packetsPerFrame_.store(packets, std::memory_order_relaxed);
        packetSpacing_.store(spacingUs, std::memory_order_relaxed);
        framesSent_.fetch_add(1, std::memory_order_relaxed);
        packetsSent_.fetch_add(packets, std::memory_order_relaxed);
    }

    /// The UDP port for a protocol index: each wire format has its own registered port.
    static uint16_t protocolPort(uint8_t p) {
        return p == kProtoE131 ? E131_PORT : p == kProtoDdp ? DDP_PORT : ARTNET_PORT;
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
            // Not while the task sends: an address it reads mid-copy would be half old, half new.
            const bool sending = sendTask_.impl && frameState_.load(std::memory_order_acquire) != kFrameIdle;
            if (!sending && (st == HostResolver::State::Resolved || st == HostResolver::State::Stale)) std::memcpy(dest_[d].ip, ip, 4);
            if (st == HostResolver::State::Waiting || st == HostResolver::State::Full) waiting++;
        }
        if (waiting) {
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "%u host%s not resolved yet", waiting, waiting == 1 ? "" : "s");
            setStatus(statusBuf_, Severity::Warning);
        } else if (mode() == Addressing::Multicast && NetworkPath::multicastDropped) {
            // A light stream never falls back to broadcast, which would flood every device on the LAN.
            setStatus("multicast blocked here: use unicast", Severity::Warning);
        } else if (prepareWarn_) {
            setStatus(prepareWarn_, Severity::Warning);
        } else {
            // What reached the wire this second, so a rate the receiver sees can be told from one the driver sends.
            const uint32_t spacing = packetSpacing_.load(std::memory_order_relaxed);
            const uint32_t packets = packetsPerFrame_.load(std::memory_order_relaxed);
            // Frames and packets a second first, as Network Receive reports what arrived, so the two cards compare at a glance.
            if (sendTask_.impl && spacing)
                mm::formatTo(statusBuf_, sizeof(statusBuf_), "%u fps, %u packets/s, %u each, %u us apart",
                             static_cast<unsigned>(framesLastSecond_), static_cast<unsigned>(packetsLastSecond_),
                             static_cast<unsigned>(packets), static_cast<unsigned>(spacing));
            else
                mm::formatTo(statusBuf_, sizeof(statusBuf_), "%u fps, %u packets/s, %u each, a burst",
                             static_cast<unsigned>(framesLastSecond_), static_cast<unsigned>(packetsLastSecond_),
                             static_cast<unsigned>(packets));
            setStatus(statusBuf_, Severity::Status);
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
        // The send task reads the buffer while it paces a frame, so a reallocation waits it out and restarts it.
        const bool wasSending = sendTask_.impl != nullptr;
        stopSendTask();
        corrected_.allocate(n, ch);
        if (wasSending) startSendTask();
    }
    uint32_t taskStarts_ = 0;   ///< send-task starts, for taskStartsForTest
    char preparedHosts_[sizeof(hosts)] = {};      ///< the host text the running destinations came from
    Addressing preparedMode_ = Addressing::Unicast;   ///< and the addressing
};

} // namespace mm
