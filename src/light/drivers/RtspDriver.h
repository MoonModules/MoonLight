#pragma once
#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "core/module/Control.h"
#include "core/util/ScratchBuffer.h"
#include "light/drivers/DriverBase.h"
#include "light/util/RtpH264.h"
#include "light/util/RtspSession.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <utility>   // move: the accepted connection becomes the viewer

namespace mm {

/// Serves the layer as an H.264 stream a player pulls over RTSP, with a fraction of the delay HLS carries.
/// @card RtspDriver.png
///
/// HLS ships whole segments and a player buffers several before it starts, so a viewer sees seconds ago. RTSP sends each frame as the encoder produces it, which makes this the remote view to reach for.
///
/// Point VLC or ffplay at the `url` the card shows. It runs on the ESP32-P4 and the desktop, and shares one encoder with @ref HlsDriver, so only one of the two runs at a time.
///
/// @attribution Delphium, RTSP, RTP
///
/// @guide
///
/// ## Playing it
///
/// Open the `url` in VLC or ffplay ([Watching the output](https://moonmodules.org/MoonLight/reference/watching-the-output.html#playing-an-rtsp-stream)). The second of RTSP and HLS to start reports that the encoder is in use.
///
/// ## One viewer at a time
///
/// A new connection takes the session over, and the displaced viewer sees its connection close. A network that blocks the UDP port the viewer names leaves the stream silent ([Watching the output](https://moonmodules.org/MoonLight/reference/watching-the-output.html#when-an-rtsp-stream-stays-silent)).
///
/// ## The delay that remains
///
/// H.264 emits a frame once it has the whole frame, and a decoder holds one more, so tens of milliseconds stay. RTSP removes HLS's segment buffering, the seconds.
///
/// @technical
///
/// **One encoder.** HLS muxes the encoder's output into segments where this packetizes the same NALs into RTP. A second `encoderStart` would silently reconfigure the first driver's stream, so the second driver reports the encoder in use.
///
/// **The newest viewer wins.** A player that vanishes without TEARDOWN leaves a silent socket, since TCP reports a peer's absence only to a write it stops acknowledging. Waiting on that would strand the stream, so a new request outranks it.
///
/// **Transport.** A player asking for interleaved TCP is refused by code, since this server speaks UDP only.
class RtspDriver : public DriverBase {
public:
    /// A destroyed driver releases the encoder, since nothing else can: a claim outliving its owner would refuse every later driver.
    ~RtspDriver() override { platform::encoderRelease(this); }

    /// The catalog tag this driver carries.
    static constexpr const char* kTags = "🖥️";

    /// Bind the frame buffer to this module, so its memory is accounted for.
    RtspDriver() : rgb_(*this) {}

    /// The catalog tags shown on this driver's card.
    const char* tags() const override { return kTags; }

    /// Point the driver at the shared source buffer.
    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; }

    /// Bind the frame rate, the scale, and the address a player is pointed at.
    void defineDriverControls() override {
        controls_.addControl("targetFps", targetFps, 1, 60);
        controls_.addControl("scale", scale, 0, kMaxScale);
        /// The address to play in VLC or ffplay, with the device's own IP.
        controls_.addReadOnly("url", urlBuf_, sizeof(urlBuf_));
    }

    /// Which controls need a fresh encode, the encoder fixing its rate at spawn.
    bool affectsPrepare(const char* name) const override {
        return std::strcmp(name, "targetFps") == 0 || std::strcmp(name, "scale") == 0 ||
               isCorrectionControl(name);
    }

    /// Derive the scaled geometry, start the encoder, and listen for a client.
    void prepare() override {
        release();
        if constexpr (!platform::hasRtsp) {
            setStatus("this board has no hardware H.264 encoder", Severity::Warning);
            return;
        }
        if (!layer_) return;

        srcWidth_  = layer_->physicalWidth()  > 0 ? layer_->physicalWidth()  : 1;
        srcHeight_ = layer_->physicalHeight() > 0 ? layer_->physicalHeight() : 1;
        scale_     = scale ? scale : autoScale();
        uint32_t scaledW = static_cast<uint32_t>(srcWidth_)  * scale_;
        uint32_t scaledH = static_cast<uint32_t>(srcHeight_) * scale_;
        // Chroma is sampled in 2x2 blocks, so the hardware encoder takes even dimensions only.
        if ((scaledW & 1u) || (scaledH & 1u)) {
            scale_  = static_cast<uint8_t>(scale_ * 2);
            scaledW *= 2;
            scaledH *= 2;
        }
        width_  = static_cast<lengthType>(scaledW);
        height_ = static_cast<lengthType>(scaledH);

        const size_t pixels = static_cast<size_t>(width_) * height_;
        if (!rgb_.resize(pixels * 3)) {
            setStatus("out of memory for the video frame", Severity::Error);
            return;
        }
        if (!sizeLightScratch()) return;   // its status names the shortage

        platform::EncoderConfig cfg{};
        cfg.width       = static_cast<uint16_t>(width_);
        cfg.height      = static_cast<uint16_t>(height_);
        cfg.fps         = targetFps;
        cfg.bitrateKbit = bitrateFor(pixels, targetFps);
        cfg.encoderName = nullptr;
        cfg.outDir      = nullptr;      // RTSP takes the frames rather than a directory of them
        // Claimed before it is configured, since the other video driver's stream would otherwise continue at this one's geometry.
        if (!platform::encoderClaim(this)) {
            setStatus("the video encoder is in use by another driver", Severity::Error);
            return;
        }
        if (!platform::encoderStart(cfg)) {
            platform::encoderRelease(this);
            setStatus("the encoder refused to start", Severity::Error);
            return;
        }
        open_ = true;

        // release() first: the encoder is already running here, and a driver left so holds an encode task nothing reads.
        if (!control_.open(mm::rtsp::kPort)) {
            release();
            setStatus("port 554 is already in use", Severity::Error);
            return;
        }
        if (!rtpOut_.open()) {
            release();
            setStatus("no socket for the video stream", Severity::Error);
            return;
        }
        // The host is EMPTY for the reader to fill from the address it reached the device by.
        std::snprintf(urlBuf_, sizeof(urlBuf_), "rtsp://:%u/",
                      static_cast<unsigned>(mm::rtsp::kPort));
        sendEpochMs_ = platform::millis();
        nextSendMs_  = sendEpochMs_;
        frameIndex_  = 0;
        std::snprintf(statusBuf_, sizeof(statusBuf_), "ready at %ux%u, waiting for a viewer",
                      static_cast<unsigned>(width_), static_cast<unsigned>(height_));
        setStatus(statusBuf_, Severity::Status);
    }

    /// Stop the encoder, drop the session and stop listening.
    void release() override {
        if (open_) {
            platform::rtspReleaseFrame();     // a frame taken but never shipped, so the buffer is free
            platform::encoderRelease(this);   // stops it, and only where this driver holds the claim
            open_ = false;
        }
        client_.close();
        control_.close();
        playing_ = false;
        packetsSent_ = 0;
        DriverBase::release();
    }

    /// Answer the RTSP control channel, then encode a frame on a fixed schedule while a client plays.
    void tick() MM_NONBLOCKING override {
        if constexpr (!platform::hasRtsp) return;
        if (!open_ || targetFps == 0 || !sourceBuffer_ || !sourceBuffer_->data()) return;

        serveControl();
        if (!playing_) return;

        // A FIXED schedule, so the rate is exact at any fps and a late tick shifts nothing.
        const uint32_t now = platform::millis();
        const uint32_t periodMs = 1000u / targetFps;
        if (static_cast<int32_t>(now - nextSendMs_) < 0) return;
        frameIndex_++;
        nextSendMs_ = sendEpochMs_ + static_cast<uint32_t>(
            (static_cast<uint64_t>(frameIndex_) * 1000u) / targetFps);
        if (static_cast<int32_t>(now - nextSendMs_) > static_cast<int32_t>(periodMs * 4)) {
            sendEpochMs_ = now;
            frameIndex_  = 1;
            nextSendMs_  = now + periodMs;
        }

        feedEncoder();
        shipEncodedFrame();
    }

private:
    /// Read one request from the client and answer it, the newest arrival being the viewer.
    void serveControl() MM_NONBLOCKING {
        platform::TcpConnection fresh = control_.accept();
        if (fresh.valid()) {
            client_.close();                  // the previous viewer, whether alive or long gone
            client_  = std::move(fresh);
            session_ = mm::rtsp::Session(platform::millis());
            playing_ = false;                 // the new arrival negotiates from the start
            reqLen_  = 0;                     // and its bytes never mix with the last viewer's
        }
        if (!client_.valid()) return;
        // TCP is a STREAM: bytes accumulate until a blank line marks a whole request, and the surplus waits for the next tick.
        const size_t room = sizeof(req_) - reqLen_ - 1;
        if (room == 0) { dropClient(); return; }     // no request is this long: the peer is confused
        const int n = client_.read(reinterpret_cast<uint8_t*>(req_) + reqLen_, room);
        if (n == 0) { dropClient(); return; }        // the viewer closed
        if (n < 0) return;                           // nothing pending this tick
        reqLen_ += static_cast<size_t>(n);
        req_[reqLen_] = '\0';

        // RFC 2326 ends a request's headers with a blank line; this server takes no bodied requests.
        const char* end = std::strstr(req_, "\r\n\r\n");
        if (!end) return;                            // still arriving
        const size_t used = static_cast<size_t>(end - req_) + 4;

        mm::rtsp::Request parsed;
        const bool ok = mm::rtsp::parseRequest(req_, used, &parsed);
        // Consume it either way: a request this server cannot parse must not be re-parsed forever.
        std::memmove(req_, req_ + used, reqLen_ - used);
        reqLen_ -= used;
        req_[reqLen_] = '\0';
        if (!ok) return;
        // 0.0.0.0 in the origin line: a client reads the address it connected to, which is what makes one SDP correct on every interface.
        char sdp[512];
        mm::rtsp::buildSdp(sdp, sizeof(sdp), "0.0.0.0", static_cast<uint16_t>(width_),
                           static_cast<uint16_t>(height_), targetFps, mm::rtp::kPayloadType);

        char out[1024];
        const size_t len = session_.respond(parsed, sdp, urlBuf_, out, sizeof(out));
        if (len) client_.write(reinterpret_cast<const uint8_t*>(out), len);

        const bool nowPlaying = session_.state() == mm::rtsp::State::Playing;
        if (nowPlaying && !playing_) {
            rtp_ = mm::rtp::Packetiser(platform::millis(), 0);
            // Where the packets go, read off the control socket: a client names a port it can receive on, and an address it cannot.
            client_.peerIPv4(peerIp_);
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "streaming %ux%u at %u fps to a viewer",
                          static_cast<unsigned>(width_), static_cast<unsigned>(height_),
                          static_cast<unsigned>(targetFps));
            setStatus(statusBuf_, Severity::Status);
        }
        playing_ = nowPlaying;
        if (parsed.verb == mm::rtsp::Request::Verb::Teardown) dropClient();
    }

    /// Release the viewer and go back to waiting for one.
    void dropClient() MM_NONBLOCKING {
        client_.close();
        playing_ = false;
        reqLen_  = 0;
        mm::formatTo(statusBuf_, sizeof(statusBuf_), "ready at %ux%u, waiting for a viewer",
                      static_cast<unsigned>(width_), static_cast<unsigned>(height_));
        setStatus(statusBuf_, Severity::Status);
    }

    /// Pack the corrected frame and hand it to the encoder, which never blocks the caller.
    void feedEncoder() MM_NONBLOCKING {
        const size_t lights = static_cast<size_t>(srcWidth_) * srcHeight_;
        const size_t have   = sourceBuffer_->count();
        const size_t n      = lights < have ? lights : have;
        const size_t frameBytes = static_cast<size_t>(width_) * height_ * 3;
        if (n == 0 || rgb_.count() < frameBytes) return;
        // A buffer shorter than the layout would leave the rest of the frame holding the previous one.
        if (n < lights) std::memset(rgb_.data(), 0, frameBytes);

        const uint8_t* src   = sourceBuffer_->data();
        const uint8_t  srcCh = sourceBuffer_->channelsPerLight();
        const uint8_t  outCh = correction_.outChannels;
        if (srcCh < 3) return;   // a non-color buffer (DMX roles) has no frame to send

        uint8_t* dst = rgb_.data();
        const bool wide = outCh > 3 && lightScratch_.count() >= outCh;
        const size_t rowBytes = static_cast<size_t>(width_) * 3;
        for (nrOfLightsType i = 0; i < n; i++) {
            const uint8_t* s = src + static_cast<size_t>(i) * srcCh;
            uint8_t rgb[3];
            if (outCh == 3) {
                correction_.apply(s, rgb, srcCh);
            } else if (wide) {
                correctToRgb(s, srcCh, &lightScratch_[0], rgb);
            } else {
                rgb[0] = s[0]; rgb[1] = s[1]; rgb[2] = s[2];
            }
            // One light becomes a scale x scale block, which replication keeps pixel-exact.
            const size_t lx = (i % srcWidth_) * scale_, ly = (i / srcWidth_) * scale_;
            for (uint8_t by = 0; by < scale_; by++) {
                uint8_t* row = dst + (ly + by) * rowBytes + lx * 3;
                for (uint8_t bx = 0; bx < scale_; bx++) {
                    row[bx * 3 + 0] = rgb[0];
                    row[bx * 3 + 1] = rgb[1];
                    row[bx * 3 + 2] = rgb[2];
                }
            }
        }
        platform::encoderWrite(dst, frameBytes);
    }

    /// Packetise whatever the encoder produced and send it to the viewer's RTP port.
    void shipEncodedFrame() MM_NONBLOCKING {
        platform::EncodedFrame frame{};
        if (!platform::rtspTakeFrame(&frame)) return;
        SendCtx ctx{&rtpOut_, peerIp_, session_.rtpPort(), &packetsSent_};
        rtp_.writeAccessUnit(frame.nal, frame.len, frame.pts90,
                             packet_, sizeof(packet_), sendPacket, &ctx);
        platform::rtspReleaseFrame();   // the bytes are on the wire: the encoder may reuse them
    }

    /// What one datagram needs to reach the viewer, handed to the packetiser's sink.
    struct SendCtx { platform::UdpSocket* sock; const uint8_t* ip; uint16_t port; uint32_t* sent; };

    /// The packetiser's sink: one datagram to the viewer's RTP port, dropping rather than waiting.
    static bool sendPacket(void* ctx, const uint8_t* packet, size_t len) {
        auto* c = static_cast<SendCtx*>(ctx);
        if (!c->sock->sendToAddr(c->ip, c->port, packet, len)) return false;
        (*c->sent)++;
        return true;
    }

    /// The bitrate a frame of this size and rate needs, at about 0.1 bits per pixel per frame.
    static uint16_t bitrateFor(size_t pixels, uint8_t fps) {
        const uint32_t kbit = static_cast<uint32_t>(pixels * fps / 10000u);
        return static_cast<uint16_t>(kbit < 500 ? 500 : (kbit > 8000 ? 8000 : kbit));
    }

    /// The smallest scale clearing the encoder's minimum frame, so a small wall still encodes.
    uint8_t autoScale() const {
        uint8_t s = 1;
        while (s < kMaxScale && (srcWidth_ * s < 128 || srcHeight_ * s < 96)) s = static_cast<uint8_t>(s + 1);
        return s;
    }

    /// The largest block one light is drawn as, which bounds the encoded frame.
    static constexpr uint8_t kMaxScale = 16;

public:
    /// Frame-rate ceiling, which is also the keyframe interval.
    /// @guide The rate is a ceiling: a heavy effect that renders at 8 fps streams at 8 fps. Each module's tick time in the UI says which effect spends it.
    uint8_t targetFps = 30;
    /// Video pixels per light, 0 for auto; each light a solid block.
    uint8_t scale = 0;

private:
    Buffer*  sourceBuffer_ = nullptr;
    ScratchBuffer<uint8_t> rgb_;

    platform::TcpServer     control_;
    platform::TcpConnection client_;
    platform::UdpSocket     rtpOut_;
    mm::rtsp::Session       session_{0};
    mm::rtp::Packetiser     rtp_{0, 0};

    lengthType srcWidth_ = 0, srcHeight_ = 0;
    lengthType width_ = 0, height_ = 0;
    uint8_t    scale_ = 1;
    bool       open_ = false;
    bool       playing_ = false;
    char       req_[1024] = {};     ///< the request being assembled, which TCP may split or pipeline
    size_t     reqLen_ = 0;         ///< bytes of it held so far
    uint32_t   packetsSent_ = 0;
    uint8_t    peerIp_[4] = {};     ///< where RTP goes, read off the control socket at PLAY
    uint32_t   sendEpochMs_ = 0, nextSendMs_ = 0, frameIndex_ = 0;
    uint8_t    packet_[mm::rtp::kMaxPacketBytes] = {};
    char       urlBuf_[48] = {};
    char       statusBuf_[96] = {};
};

}  // namespace mm
