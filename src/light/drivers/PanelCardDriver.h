#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "light/drivers/DriverBase.h"

#include "light/drivers/ColorLight5A75Packet.h"   // the first wire format (byte layout lives there)
#include "platform/platform.h"

namespace mm {

/// Output driver: streams the buffer to LED panel cards over raw Ethernet frames, below IP. These cards take a sender-card feed, so they need an L2 seam rather than a socket. The wire format lives in ColorLight5A75Packet.h, and this driver owns the window, the correction and the chunking. The board renders and sends, making the device a complete panel controller.
///
/// Prior art: FPP (Falcon Player), which drives these cards from a Raspberry Pi, and the ColorLight 5A-75 documented byte layout. The wiring, the vendors and the host setup are on the panel cards page.
///
/// @moreinfo
///
/// ## Why a gigabit link
///
/// The cards require a 1000 Mbps link, for wire time rather than bandwidth. They latch on the sync frame with no buffering, so a whole frame must arrive inside the inter-frame window. At 100 Mbit the same bytes overrun the frame budget and break the timing the sync depends on.
///
/// A slow link is silent: frames go out, the link is up, and the panels tear or never latch. The driver reads the negotiated speed and says so, then sends anyway, since a small panel may be fine.
///
/// ## No geometry controls
///
/// The panel arrangement belongs to the Layout. It states the count, size, wiring order and snaking, and maps every light to an (x, y). This driver reads the finished picture and cuts it into card rows. So a wall is described in exactly one place.
///
/// @card PanelCardDriver.png
class PanelCardDriver : public DriverBase {
public:
    /// Default to the RGB preset, as the network sinks do, rather than the strips' GRB.
    PanelCardDriver() { setDefaultPresetName("RGB"); }

    /// The card's own gain, held at full so it does not compound our own brightness.
    static constexpr uint8_t kCardGain = 0xFF;

    /// Wire formats; the control exists because the category is panel cards, not one vendor.
    static constexpr const char* kFormatOptions[] = {"ColorLight 5A-75"};
    /// How many wire formats the selector offers.
    static constexpr uint8_t kFormatCount = 1;

    /// Wire format (index into kFormatOptions).
    uint8_t format = 0;

    // A control rather than a probe because this driver is send-only, having no receive seam.
    /// Card firmware generation, which decides whether the sync frame goes out once or twice.
    static constexpr const char* kFirmwareOptions[] = {"v12 and older", "v13 and newer"};
    /// How many card firmware generations the selector offers.
    static constexpr uint8_t kFirmwareCount = 2;

    /// Card firmware generation (index into kFirmwareOptions): 0 is v12-and-older, 1 is v13+.
    uint8_t firmware = 0;
    // Persisted by LABEL, not index, so a NIC keeps its identity across a re-enumeration.
    /// Host NIC to send from, row 0 being capture-only. Not built on ESP32.
    uint8_t interfaceSel_ = 0;
    /// The label behind the selected row, so a reordered list restores the same NIC.
    char chosenIf_[64] = {};
    /// The row the last rebuild settled on, which tells a user's pick from a re-enumeration.
    uint8_t lastResolvedSel_ = 0;
    /// Send-rate ceiling (Hz); tick() rate-limits so a fast render tick doesn't saturate the link.
    uint8_t fps = 40;

    /// Bind the format, the host interface and the shared window, after the correction block.
    void defineDriverControls() override {
        controls_.addSelect("format", format, kFormatOptions, kFormatCount);
        controls_.addSelect("firmware", firmware, kFirmwareOptions, kFirmwareCount);
        // Re-enumerated on every rebuild, so a hot-plugged NIC appears.
        if constexpr (platform::hasNamedNetInterfaces) {
            const char* const* ifOptions = nullptr;
            const size_t n = platform::rawInterfaces(&ifOptions);
            // The index moving means the USER picked; the index standing still means the list did.
            const bool userPicked = (interfaceSel_ != lastResolvedSel_);
            if (!userPicked && chosenIf_[0] && ifOptions) {
                // Compare the stable head: a label carries a live link speed that can change.
                const char* mySep = std::strstr(chosenIf_, ", ");
                const size_t mine = mySep ? static_cast<size_t>(mySep - chosenIf_)
                                          : std::strlen(chosenIf_);
                // No match means no NIC, explicitly: the old index now points at a stranger.
                interfaceSel_ = 0;
                for (size_t i = 0; i < n && i < 255; i++) {
                    if (!ifOptions[i]) continue;
                    const char* sep = std::strstr(ifOptions[i], ", ");
                    const size_t head = sep ? static_cast<size_t>(sep - ifOptions[i])
                                            : std::strlen(ifOptions[i]);
                    if (head == mine && std::strncmp(ifOptions[i], chosenIf_, head) == 0) {
                        interfaceSel_ = static_cast<uint8_t>(i);
                        break;
                    }
                }
            }
            // Written here too, so a rebuild that never reaches prepare still leaves the pair agreeing.
            lastResolvedSel_ = interfaceSel_;
            if (ifOptions && interfaceSel_ < n && ifOptions[interfaceSel_])
                std::snprintf(chosenIf_, sizeof(chosenIf_), "%s", ifOptions[interfaceSel_]);
            controls_.addSelect("interface", interfaceSel_, ifOptions,
                                static_cast<uint8_t>(n < 255 ? n : 255));
            controls_.setPersistLabel(controls_.count() - 1);
        }
        addWindowControls();   // start / count: which slice of the shared buffer this sink sends
        controls_.addControl("fps", fps, 1, 120);
    }

    /// Which controls re-run the prepare sweep: the geometry, the window and the interface.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "interface") == 0
               || isWindowControl(name) || isCorrectionControl(name);
    }

    /// Drop back to capture mode so a disabled driver holds no raw socket, then chain to the base.
    void release() override {
        if (claimed_) { platform::ethClaimRawL2(false); claimed_ = false; }
        platform::ethBindRawInterface(nullptr);
        DriverBase::release();
    }

    /// Take the shared source buffer and size the corrected_ buffer for it.
    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
        resizeCorrected();
    }

    /// Bind the raw interface, size the corrected buffer, and publish the link status.
    void prepare() override {
        resizeCorrected();

        // Claimed before any frame goes out, so it cannot race the DHCP cascade's own timeout.
        if (!claimed_) { platform::ethClaimRawL2(true); claimed_ = true; }

        // Re-synced every prepare INCLUDING the blank case, so clearing returns to capture-only.
        const char* ifName = nullptr;
        if constexpr (platform::hasNamedNetInterfaces) {
            // Remember the adapter behind this row, so a reordered list can find it again.
            const char* const* ifOptions = nullptr;
            const size_t n = platform::rawInterfaces(&ifOptions);
            // A PAIR: writing one without the other makes a later rebuild misread what happened.
            lastResolvedSel_ = interfaceSel_;
            if (ifOptions && interfaceSel_ < n && ifOptions[interfaceSel_])
                std::snprintf(chosenIf_, sizeof(chosenIf_), "%s", ifOptions[interfaceSel_]);
            ifName = platform::rawInterfaceName(interfaceSel_);
        }
        if (!platform::ethBindRawInterface(ifName)) {
            // Name the string we failed to match: blaming root for a typo sends the reader to sudo.
            if (ifName) {
                std::snprintf(statusBuf_, sizeof(statusBuf_),
                              "cannot open '%s' - no adapter matches, or needs root", ifName);
                setStatus(statusBuf_, Severity::Warning);
            } else {
                setStatus("cannot open interface (needs root?)", Severity::Warning);
            }
            return;
        }

        writeLinkStatus();
    }

    /// Refresh the link status once a second, so a cable plugged in later is picked up.
    void tick1s() MM_NONBLOCKING override {
        writeLinkStatus();
        MoonModule::tick1s();
    }

    /// A preset toggle changes correction_.outChannels without a structural rebuild.
    void onCorrectionChanged() override { resizeCorrected(); }

    /// Correct the window, emit it row by row, and latch it with one sync frame.
    void tick() MM_NONBLOCKING override {
        if (!sourceBuffer_ || !sourceBuffer_->data()) return;
        if (fps == 0) return;

        const uint32_t interval = 1000 / fps;
        const uint32_t now = platform::millis();
        if (now - lastSendTime_ < interval) return;
        lastSendTime_ = now;

        // The wall comes from the Layout, so this driver needs no panel geometry of its own.
        if (!layer()) return;                       // no dimensions to send against
        const lengthType wallW = layer()->physicalWidth();
        const lengthType wallH = layer()->physicalHeight();
        if (wallW == 0 || wallH == 0) return;

        // A stale or unwired correction degrades to raw bytes, never overruns the allocation.
        nrOfLightsType winStart, nLights;
        windowSlice(sourceBuffer_->count(), winStart, nLights);
        if (nLights == 0) return;

        const uint8_t* data;
        uint8_t stride;
        const uint8_t outCh = correction_.outChannels;
        if (outCh != 0 && corrected_.data()
            && corrected_.count() >= nLights
            && corrected_.channelsPerLight() >= outCh) {
            const uint8_t* src = sourceBuffer_->data();
            const uint8_t srcCh = sourceBuffer_->channelsPerLight();
            uint8_t* dst = corrected_.data();
            for (nrOfLightsType i = 0; i < nLights; i++) {
                // srcCh carries a wide light's motion channels through, as NetworkSendDriver does.
                correction_.apply(src + (winStart + i) * srcCh, dst + i * outCh, srcCh);
            }
            data = dst;
            stride = outCh;
        } else {
            const uint8_t srcCh = sourceBuffer_->channelsPerLight();
            data = sourceBuffer_->data() + static_cast<size_t>(winStart) * srcCh;
            stride = srcCh;
        }

        // Bail before the brightness pair, so a misconfigured window is silent.
        if (nLights < static_cast<nrOfLightsType>(wallW)) return;

        // Sending two to a card that acts on the first is not harmless, hence a choice.
        const int frameCopies = (firmware == 1) ? 2 : 1;

        // Brightness first, the order the cards expect; advisory, and older firmware ignores it.
        {
            const size_t len = buildColorLightBrightnessPacket(packet_, kCardGain);
            // Counted like the rows, so the totals describe the same set of frames.
            for (int i = 0; i < frameCopies; i++) {
                if (platform::ethSendRaw(packet_, len)) framesSent_++;
                else framesDroppedTotal_++;
            }
        }

        // One card row per wall row: the card numbers rows across its outputs, top to bottom.
        bool anyRowSent = false;
        for (lengthType row = 0; row < wallH; row++) {
            if (static_cast<size_t>(row) * wallW >= nLights) break;   // buffer ran out before the wall
            for (lengthType off = 0; off < wallW; off += COLORLIGHT_MAX_PIXELS_PER_PACKET) {
                lengthType n = static_cast<lengthType>(wallW - off);
                if (n > COLORLIGHT_MAX_PIXELS_PER_PACKET) n = COLORLIGHT_MAX_PIXELS_PER_PACKET;

                const size_t first = static_cast<size_t>(row) * wallW + off;
                if (first >= nLights) break;                       // buffer ran out before the wall
                if (first + n > nLights) n = static_cast<lengthType>(nLights - first);
                if (n == 0) break;

                const size_t len = packRow(static_cast<uint16_t>(row), static_cast<uint16_t>(off),
                                           static_cast<uint16_t>(n), data + first * stride, stride);
                // Set on a frame that reached the WIRE: latching after a mid-frame drop blanks the wall.
                if (platform::ethSendRaw(packet_, len)) { framesSent_++; anyRowSent = true; }
                else framesDroppedTotal_++;
            }
        }

        // The sync latches everything above, so it goes last and only if something was sent.
        if (anyRowSent) {
            const size_t len = buildColorLightSyncPacket(packet_, kCardGain);
            // This is the one that matters: a second sync is a second LATCH, not a repeat.
            for (int i = 0; i < frameCopies; i++) {
                if (platform::ethSendRaw(packet_, len)) framesSent_++;
                else framesDroppedTotal_++;
            }
        }
        // The cards need the burst inside one inter-frame window rather than trickled.
        platform::ethFlushRaw();
    }

    /// Report what the wire is doing: no link, a link too slow, or the packet rate reaching it.
    void writeLinkStatus() MM_NONBLOCKING {
        // A failed restart outranks everything: it is the one state a cable cannot resolve.
        if (restartFailed_) {
            setStatus("ethernet restart failed - restart the device", Severity::Error);
            framesReported_ = framesSent_;
            return;
        }

        // Checked BEFORE the link: a wedge is defined by sends failing, whatever the link claims.
        static constexpr uint32_t kWedgedStreak = 500;
        const uint32_t failStreak = platform::ethSendFailStreak();
        if (failStreak >= kWedgedStreak) {
            if (!restartTried_) {
                restartTried_ = true;
                // A failed restart leaves the driver stopped, which must not read as a loose cable.
                if (platform::ethRestartTx()) {
                    setStatus("transmit wedged - restarting ethernet", Severity::Warning);
                } else {
                    // Latched, or the next tick reports a loose cable for a board needing a power cycle.
                    restartFailed_ = true;
                    setStatus("ethernet restart failed - restart the device", Severity::Error);
                }
                framesReported_ = framesSent_;
                return;
            }
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "transmit wedged (%u refused, link %u Mbit)",
                          static_cast<unsigned>(failStreak),
                          static_cast<unsigned>(platform::ethLinkSpeedMbps()));
            setStatus(statusBuf_, Severity::Error);
            framesReported_ = framesSent_;
            return;
        }

        if (!platform::ethLinkUp()) {
            // On a host, link-down also means a name matching no adapter, so name the field.
            if constexpr (platform::hasNamedNetInterfaces) {
                const char* boundName = platform::rawInterfaceName(interfaceSel_);
                if (!boundName) {
                    setStatus("no ethernet link - pick an 'interface' adapter", Severity::Warning);
                } else {
                    mm::formatTo(statusBuf_, sizeof(statusBuf_),
                                  "no ethernet link - cable, or no adapter matches '%s'", boundName);
                    setStatus(statusBuf_, Severity::Warning);
                }
            } else {
                setStatus("no ethernet link", Severity::Warning);
            }
            framesReported_ = framesSent_;
            return;
        }
        const uint16_t mbps = platform::ethLinkSpeedMbps();
        // A rising total says the sender is outrunning the wire, which explains a stuttering wall.
        const uint32_t dropped = framesDroppedTotal_;
        // The one number separating "not sending" from "sending and the card ignores it".
        const uint32_t sent = framesSent_ - framesReported_;
        framesReported_ = framesSent_;
        if (mbps && mbps < 1000) {
            mm::formatTo(statusBuf_, sizeof(statusBuf_),
                          "%u Mbit (needs 1 Gbit) - %u packets/s",
                          static_cast<unsigned>(mbps), static_cast<unsigned>(sent));
            setStatus(statusBuf_, Severity::Warning);
            return;
        }
        // Re-armed only on evidence of FLOW, or a rebuilding wedge bounces the interface in a loop.
        if (sent > 0 && failStreak == 0) { restartTried_ = false; restartFailed_ = false; }

        if (dropped) {
            // Split by cause: a flapping link and a full TX ring need different fixes.
            uint32_t linkDown = 0, ringFull = 0;
            platform::ethSendFailCounts(linkDown, ringFull);
            mm::formatTo(statusBuf_, sizeof(statusBuf_),
                          "%u Mbit - %u pkt/s, %u lost (%u link, %u ring)",
                          static_cast<unsigned>(mbps), static_cast<unsigned>(sent),
                          static_cast<unsigned>(dropped),
                          static_cast<unsigned>(linkDown), static_cast<unsigned>(ringFull));
        } else {
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "%u Mbit - %u packets/s",
                          static_cast<unsigned>(mbps), static_cast<unsigned>(sent));
        }
        setStatus(statusBuf_, Severity::Status);
    }

    /// Test-only accessor for the corrected buffer, pinning the no-allocation contract.
    const Buffer& correctedBuffer() const { return corrected_; }

private:
    // The source stride need not match the wire's 3 bytes, so a white channel has nowhere to go.
    /// Build one row packet into the reused frame buffer.
    size_t packRow(uint16_t row, uint16_t pixelOffset, uint16_t pixelCount,
                   const uint8_t* src, uint8_t stride) MM_NONBLOCKING {
        if (stride == COLORLIGHT_BYTES_PER_PIXEL) {
            // Dense already: one memcpy inside the builder.
            return buildColorLightRowPacket(packet_, row, pixelOffset, pixelCount, src);
        }
        // Strided source: write the header with no data, then pack RGB out of each light.
        buildColorLightRowPacket(packet_, row, pixelOffset, pixelCount, nullptr);
        uint8_t* dst = packet_ + COLORLIGHT_ROW_PREFIX;
        for (uint16_t i = 0; i < pixelCount; i++) {
            dst[i * 3 + 0] = src[i * stride + 0];
            dst[i * 3 + 1] = src[i * stride + 1];
            dst[i * 3 + 2] = src[i * stride + 2];
        }
        return COLORLIGHT_ROW_PREFIX + static_cast<size_t>(pixelCount) * COLORLIGHT_BYTES_PER_PIXEL;
    }

    /// Size the corrected buffer, off the hot path, which is the no-allocation contract.
    void resizeCorrected() {
        if (!sourceBuffer_) return;
        nrOfLightsType winStart, n;
        windowSlice(sourceBuffer_->count(), winStart, n);
        const uint8_t ch = correction_.outChannels;
        if (n == 0 || ch == 0) return;
        if (corrected_.count() >= n && corrected_.channelsPerLight() >= ch) return;
        corrected_.allocate(n, ch);
    }

    /// The shared frame this driver reads its window from; borrowed, not owned.
    Buffer* sourceBuffer_ = nullptr;
    /// Owned: source bytes after brightness/order/white. Sized off the hot path.
    Buffer corrected_;
    /// One reused frame buffer, sized for the largest packet the format builds.
    uint8_t packet_[COLORLIGHT_MAX_FRAME] = {};
    /// millis() of the last frame sent: the `fps` limiter's reference.
    uint32_t lastSendTime_ = 0;
    /// Frames the MAC accepted since boot; the difference per second is the rate shown.
    uint32_t framesSent_ = 0;
    /// framesSent_ at the last status write: subtracting gives the rate without a timer.
    uint32_t framesReported_ = 0;
    /// Frames the MAC refused since boot, cumulative, since a rate hides a slow trickle.
    uint32_t framesDroppedTotal_ = 0;
    /// Backing store for the status line, sized for the longest one, the split-drop report.
    char statusBuf_[96] = {};
    /// Whether a restart was already attempted for the current wedge; one attempt per wedge.
    bool restartTried_ = false;
    /// Set when a recovery attempt itself failed, latched so the softer warning cannot hide it.
    bool restartFailed_ = false;
    /// Whether this driver holds the raw-L2 claim, keeping prepare and release balanced.
    bool claimed_ = false;
};

}  // namespace mm
