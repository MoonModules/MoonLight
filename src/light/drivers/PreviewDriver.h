#pragma once

#include "core/util/format.h"   // formatTo: nonblocking formatting into a fixed buffer
#include "light/drivers/DriverBase.h"

#include "light/util/light_types.h"  // lengthType, nrOfLightsType
#include "core/util/BinaryBroadcaster.h"
#include "platform/platform.h"

#include <limits>  // numeric_limits for the memory-derived point cap

namespace mm {

/// Streams a true-shape 3D preview to the web UI over its own binary WebSocket: @xref{its-own-channel-and-why}.
///
/// The preview is a POINT LIST, not a dense grid. Only the real lights are sent, at their real (x, y, z) positions, on MoonLight's PhysicalLayer model. Positions go out once at mapping time and channels per frame. This driver owns both wire formats, and the HTTP server is a domain-neutral broadcaster that writes the bytes.
///
/// Resolution is client-driven, over the wire format in @xref{the-wire-format}: the browser reads the drops counter each frame carries and posts the standing request it wants. No standing request means no work at all.
///
/// @moreinfo
///
/// ## The wire format
///
/// --8<-- [start:wire-format]
/// ```text
/// 0x03 coordinate table, sent only in answer to a client's request:
///      [0x03][count:u32][bx][by][bz][stride:u16][epoch:u8][(x,y,z):u8x3 x count]
/// 0x02 per-frame channels:
///      [0x02][count:u32][stride:u16][epoch:u8][drops:u8][(r,g,b) x count]
/// 0x04 per-frame aim, only for a rig whose fixtures carry pan and tilt:
///      [0x04][count:u32][stride:u16][epoch:u8][reserved:u8][(pan,tilt):u8x2 x count]
/// Client requests: [0x51][stride][fps] standing, [0x52] one-shot table. The table's own stride is memory-derived, so the request carries none.
/// ```
/// --8<-- [end:wire-format]
///
/// ## Its own channel, and why
///
/// Preview frames are lossy and large; control-plane state is small and latency-sensitive. Sharing one WebSocket queues the small messages behind the big ones, which reads as a flickering connection indicator, so each gets its own.
///
/// @card PreviewDriver.png
class PreviewDriver : public DriverBase, public BinaryBroadcaster::ClientMessageSink {
public:
    /// Not user-editable: deleting it from the UI would silently kill the 3D preview.
    bool userEditable() const override { return false; }
    /// Running in safe mode, which leaves the lights to the screen.
    bool heldInSafeMode() const override { return false; }

    /// The frame rate the preview aims for, in Hz, independent of the render rate.
    uint8_t targetFps = 24;

    /// Set the sink each message is pushed to, and register as its inbound-message sink.
    void setBroadcaster(BinaryBroadcaster* b) {
        broadcaster_ = b;
        if (b) b->setClientMessageSink(this);
    }

    // Arrives on the transport thread; single-byte fields, so the encode reader tolerates the race.
    /// Handle a client request: a standing frame request, or a one-shot table request.
    void onClientMessage(int slot, const uint8_t* payload, int len) override {
        if (slot < 0 || slot >= kMaxRequestSlots || len < 2) return;
        if (payload[0] == 0x51) {
            const uint8_t stride = payload[1];
            if (stride < 1 || stride > 64) return;
            reqStride_[slot] = stride;
            reqFps_[slot] = (len >= 3 && payload[2] >= 1 && payload[2] <= 25) ? payload[2] : 0;
        } else if (payload[0] == 0x52) {
            tableRequested_ = true;
        }
    }
    /// Drop a departed client's standing request, so its slot stops being served.
    void onClientGone(int slot) override {
        if (slot < 0 || slot >= kMaxRequestSlots) return;
        reqStride_[slot] = 0;   // a dead client's request dies with its slot
        reqFps_[slot] = 0;
    }



    /// Does this kept light survive a frame at `fs`? The device's own rule, on its own coordinates.
    bool keptAtStride(nrOfLightsType k, nrOfLightsType fs) const {
        const size_t at = static_cast<size_t>(k) * 3;
        return keptPos_[at] % fs == 0 && keptPos_[at + 1] % fs == 0 && keptPos_[at + 2] % fs == 0;
    }

    /// The stride a color frame ships at: the link's pacing, never finer than the table's.
    nrOfLightsType frameStride() const {
        // Coarsening needs the kept-light cache, or the count and the gather filter differently and the browser rejects every frame.
        const bool canCoarsen = denseGrid() || (keptPos_ && keptCount_ == coordCount_);
        if (!canCoarsen) return previewStride_;
        return downscale_ > previewStride_ ? downscale_ : previewStride_;
    }

    /// How many lights a frame at `frameStride()` carries, counted the way the table is.
    nrOfLightsType frameCount() const {
        const nrOfLightsType fs = frameStride();
        if (fs == previewStride_) return coordCount_;
        if (!layer_) return coordCount_;
        const lengthType ax = layer_->physicalWidth()  > 0 ? layer_->physicalWidth()  : 1;
        const lengthType ay = layer_->physicalHeight() > 0 ? layer_->physicalHeight() : 1;
        const lengthType az = layer_->physicalDepth()  > 0 ? layer_->physicalDepth()  : 1;
        if (denseGrid()) {
            const nrOfLightsType cx = (ax + fs - 1) / fs, cy = (ay + fs - 1) / fs,
                                 cz = (az + fs - 1) / fs;
            return static_cast<nrOfLightsType>(static_cast<uint32_t>(cx) * cy * cz);
        }
        // From the CACHE, never a placeLights walk: this runs per frame on a nonblocking tick.
        if (!keptPos_ || keptCount_ != coordCount_) return coordCount_;
        nrOfLightsType out = 0;
        for (nrOfLightsType k = 0; k < keptCount_; k++) if (keptAtStride(k, fs)) out++;
        return out;
    }

    /// Test-only: the currently served downsample factor, 1 being full resolution.
    nrOfLightsType downscaleForTest() const { return downscale_; }


    /// Preview shows the raw logical buffer, no correction.
    bool hasCorrectionControls() const override { return false; }

    /// Bind the target frame rate, which is the ceiling the browser trades resolution toward.
    void defineDriverControls() override {
        controls_.addControl("targetFps", targetFps, 1, 25);
    }

    /// Point the driver at the same sparse buffer the other drivers read, with no copy.
    void setSourceBuffer(Buffer* buf) override {
        sourceBuffer_ = buf;
    }

    // Cancels any in-flight send FIRST: a resize reallocs the buffer a half-sent frame reads.
    /// Rebuild the coordinate table for the new geometry and start a fresh epoch.
    void prepare() override {
        // Cancel BEFORE the rebuild: an in-flight send holds a pointer about to dangle.
        if (broadcaster_) broadcaster_->cancelBufferedSend();
        else freePreviewBuffers();            // no broadcaster wired: nothing streams, release the buffers
        // A NEW epoch, so every client's table cache misses and each asks for a fresh one.
        epoch_++;
        buildCoordTable();
        // Pre-sized for the FINEST stride, so a later stride change on the tick allocates nothing.
        if (layer_ && layer_->layouts()) {
            const nrOfLightsType finest = layer_->layouts()->totalLightCount();
            const nrOfLightsType capPts = maxPreviewPoints();
            const size_t maxPts = finest < capPts ? finest : capPts;
            ensureStaging(maxPts * 3u);
            if (!denseGrid() && keptIdxCap_ < maxPts) {
                auto* grown = static_cast<nrOfLightsType*>(platform::alloc(maxPts * sizeof(nrOfLightsType)));
                auto* steps = static_cast<uint8_t*>(platform::alloc(maxPts * 3));
                if (grown && steps) {
                    if (keptIdx_) platform::free(keptIdx_);
                    if (keptPos_) platform::free(keptPos_);
                    keptIdx_ = grown;
                    keptPos_ = steps;
                    keptIdxCap_ = static_cast<nrOfLightsType>(maxPts);
                    publishHeapBytes();
                } else {
                    if (grown) platform::free(grown);
                    if (steps) platform::free(steps);
                }
            }
        }
        refreshStatus();   // surface an index-cache alloc miss in the tab
    }

    /// Free the preview buffers, then release the base.
    void release() override {
        freePreviewBuffers();
        DriverBase::release();
    }

    /// No control changes the transport structure, so nothing here re-runs prepare.
    bool affectsPrepare(const char* /*name*/) const override { return false; }

    // REPORTED AS BLOCKING deliberately: the socket write and the resize are both real.
    /// Serve a requested table, then stream one frame if the previous one finished draining.
    void tick() MM_NONBLOCKING override {
        // The PULL model: no standing request means no gather, no send, nothing at all.
        if (!broadcaster_) return;
        nrOfLightsType wantStride = 0;
        uint8_t wantFps = 255;
        for (int i = 0; i < kMaxRequestSlots; i++) {
            const uint8_t rs = reqStride_[i];
            if (!rs) continue;
            if (rs > wantStride) wantStride = rs;                       // coarsest wins
            const uint8_t rf = reqFps_[i] ? reqFps_[i] : targetFps;
            if (rf < wantFps) wantFps = rf;                             // slowest wins
        }
        if (wantStride == 0) return;                                    // nobody asked: no work
        if (wantFps > targetFps) wantFps = targetFps;                   // the control is the ceiling
        if (wantFps == 0) return;

        uint32_t now = platform::millis();
        if (now - lastSendTime_ < 1000u / wantFps) return;              // rate: the served request

        // TRY-acquire, never block: a skipped preview frame is invisible, a blocked encode is not.
        SendLease lease{broadcaster_};
        if (!lease) return;

        lastSendTime_ = now;   // only after we own the sender: a skipped slot must retry next tick

        // Gated on an idle slot only because the staging buffer must not be rewritten mid-drain.
        const bool idle = broadcaster_->bufferedSendIdle();
        if ((wantStride != downscale_ || coordCount_ == 0) && idle) {
            downscale_ = wantStride;
            buildCoordTable();
        }
        if (coordCount_ == 0) return;   // nothing previewable (empty layout / staging alloc miss)

        // A requested table outranks the next frame: the asker can render nothing until it lands.
        if (tableRequested_) {
            if (idle) {
                buildCoordTable();
                if (sendCoordTable()) tableRequested_ = false;
            }
            return;
        }

        // Drop-new, and the drop is REPORTED, so the client adapts on a real congestion signal.
        if (idle) {
            // ALTERNATE: one send is in flight at a time, so back-to-back calls lose every aim frame.
            if (aimTurn_ && sendAim()) {
                aimTurn_ = false;
            } else {
                if (!sendFrame() && dropsSinceLast_ < 255) dropsSinceLast_++;
                aimTurn_ = true;
            }
        } else if (dropsSinceLast_ < 255) {
            dropsSinceLast_++;
        }
    }

    // Sampling positions rather than indices, so a downsampled preview shows no moiré.
    /// Build the cached coordinate table from the layout's real lights.
    void buildCoordTable() {
        coordCount_ = 0;
        if (!layer_ || !layer_->layouts()) return;
        Layouts* layouts = layer_->layouts();
        nrOfLightsType n = layouts->totalLightCount();
        if (n == 0) return;

        // EXTENT, not size: an 8-wide grid spans 0 to 7, and the browser centers on this.
        auto extent = [](lengthType size) -> lengthType { return size > 0 ? size - 1 : 0; };
        const lengthType ex = extent(layer_->physicalWidth());
        const lengthType ey = extent(layer_->physicalHeight());
        const lengthType ez = extent(layer_->physicalDepth());
        // Every axis scales by the same factor, so the aspect ratio survives a >255 extent.
        lengthType maxEdge = ex;
        if (ey > maxEdge) maxEdge = ey;
        if (ez > maxEdge) maxEdge = ez;
        if (maxEdge < 1) maxEdge = 1;
        posScale_ = (maxEdge > 255) ? maxEdge : 0;   // 0 = no scaling (1:1)
        bx_ = scaleAxis(ex);
        by_ = scaleAxis(ey);
        bz_ = scaleAxis(ez);

        // Grown only when the layout has more LIGHTS than the cap, never for its box size alone.
        const lengthType ax = layer_->physicalWidth()  > 0 ? layer_->physicalWidth()  : 1;
        const lengthType ay = layer_->physicalHeight() > 0 ? layer_->physicalHeight() : 1;
        const lengthType az = layer_->physicalDepth()  > 0 ? layer_->physicalDepth()  : 1;
        nrOfLightsType s = 1;
        const nrOfLightsType cap = maxPreviewPoints();   // memory-derived this rebuild
        if (n > cap) {
            auto latticeCount = [&](nrOfLightsType step) {
                nrOfLightsType cx = (ax + step - 1) / step, cy = (ay + step - 1) / step,
                               cz = (az + step - 1) / step;
                return static_cast<uint32_t>(cx) * cy * cz;
            };
            while (latticeCount(s) > cap) s++;
        }
        // MEMORY bounds the table, never the link: geometry is sent once, and `downscale_` paces the colors instead.
        previewStride_ = s;

        // A dense grid is closed-form; a mapped layout is counted by one placeLights pass.
        if (denseGrid()) {
            const nrOfLightsType cx = (ax + s - 1) / s, cy = (ay + s - 1) / s, cz = (az + s - 1) / s;
            coordCount_ = static_cast<nrOfLightsType>(static_cast<uint32_t>(cx) * cy * cz);
        } else {
            struct CountCtx { nrOfLightsType s, out; };
            CountCtx cc{s, 0};
            // A gap is a real preview position, drawn dark, so it counts like any other light.
            layouts->placeLights(CoordSink{[](void* c, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
                auto* p = static_cast<CountCtx*>(c);
                if (x % p->s == 0 && y % p->s == 0 && z % p->s == 0) p->out++;
            }, nullptr, &cc});
            coordCount_ = cc.out;
            // Sized to EXACTLY this count before the emit fills it, so the cache cannot truncate.
            if (keptIdxCap_ < coordCount_) {
                auto* grown = static_cast<nrOfLightsType*>(platform::alloc(coordCount_ * sizeof(nrOfLightsType)));
                auto* steps = static_cast<uint8_t*>(platform::alloc(static_cast<size_t>(coordCount_) * 3));
                if (grown && steps) {
                    if (keptIdx_) platform::free(keptIdx_);
                    if (keptPos_) platform::free(keptPos_);
                    keptIdx_ = grown;
                    keptPos_ = steps;
                    keptIdxCap_ = coordCount_;
                    keptIdxAllocFailed_ = false;
                    publishHeapBytes();   // the index cache grew: refresh the memory readout
                } else {
                    if (grown) platform::free(grown);
                    if (steps) platform::free(steps);
                    keptIdxAllocFailed_ = true;   // degraded: the gather walks placeLights per frame
                }
            }
        }
        if (coordCount_ == 0) return;

        // Built COMPLETE into the staging buffer, which stays stable for a drain's lifetime.
        if (!ensureStaging(static_cast<size_t>(coordCount_) * 3)) {
            coordCount_ = 0;   // alloc miss: nothing previewable until memory frees; retried next adopt
            return;
        }
        // BOTH paths visit the kept lights in the SAME order the color pass uses.
        struct PosCtx {
            PreviewDriver* self; uint8_t* out; size_t at; nrOfLightsType s;
            void emit(lengthType x, lengthType y, lengthType z) {
                out[at++] = self->scaleAxis(x);
                out[at++] = self->scaleAxis(y);
                out[at++] = self->scaleAxis(z);
            }
        };
        PosCtx pc{this, staging_, 0, s};
        keptCount_ = 0;   // rebuilt below for the sparse path; dense gathers closed-form, no index map
        if (denseGrid()) {
            for (lengthType z = 0; z < az; z += s)
                for (lengthType y = 0; y < ay; y += s)
                    for (lengthType x = 0; x < ax; x += s) pc.emit(x, y, z);
        } else {
            // CACHE the kept indices here: re-walking placeLights per frame measured ~8 ms at 12K.
            layouts->placeLights(CoordSink{[](void* c, nrOfLightsType idx, lengthType x, lengthType y, lengthType z) {
                auto* p = static_cast<PosCtx*>(c);
                if (x % p->s != 0 || y % p->s != 0 || z % p->s != 0) return;
                PreviewDriver* self = p->self;
                if (self->keptIdx_ && self->keptCount_ < self->keptIdxCap_) {
                    // SCALED, as the coord table emits them: past a 255 extent it carries v*255/extent, and the browser runs its modulo over those bytes.
                    if (self->keptPos_) {
                        const size_t at = static_cast<size_t>(self->keptCount_) * 3;
                        self->keptPos_[at + 0] = self->scaleAxis(x);
                        self->keptPos_[at + 1] = self->scaleAxis(y);
                        self->keptPos_[at + 2] = self->scaleAxis(z);
                    }
                    self->keptIdx_[self->keptCount_++] = idx;
                }
                p->emit(x, y, z);
            }, nullptr, &pc});
        }
        stagingUsed_ = pc.at;   // the built table's byte length, what sendCoordTable ships
    }

    /// Answer a table request, returning whether the send was accepted or the slot was busy.
    bool sendCoordTable() {
        if (!broadcaster_ || coordCount_ == 0 || !staging_) return false;
        uint8_t h[11];
        h[0] = 0x03;
        h[1] = static_cast<uint8_t>(coordCount_ & 0xFF);
        h[2] = static_cast<uint8_t>((coordCount_ >> 8) & 0xFF);
        h[3] = static_cast<uint8_t>((coordCount_ >> 16) & 0xFF);
        h[4] = static_cast<uint8_t>((coordCount_ >> 24) & 0xFF);
        h[5] = bx_; h[6] = by_; h[7] = bz_;
        h[8] = static_cast<uint8_t>(previewStride_ & 0xFF);
        h[9] = static_cast<uint8_t>(previewStride_ >> 8);
        h[10] = epoch_;
        return broadcaster_->sendBufferedFrame(h, sizeof(h), staging_, stagingUsed_);
    }

    // Costs nothing on a rig without motion: the first line is a flag test, and an LED wall stops.
    /// Stream one aim message, so the preview can draw where each moving head points.
    bool sendAim() {
        Layer* l = layer();
        if (!l) return false;
        const FixtureChannels& fc = l->fixtureChannels();
        if (!fc.movable()) return false;                 // the common case: one branch, then out
        if (!broadcaster_ || !sourceBuffer_ || !sourceBuffer_->data() || coordCount_ == 0) return false;

        const uint8_t* src = sourceBuffer_->data();
        const uint8_t cpl = sourceBuffer_->channelsPerLight();
        const nrOfLightsType n = sourceBuffer_->count();
        if (cpl == 0 || n == 0) return false;
        // Sized for RGB at the table build and aim needs less, so bail rather than overrun.
        uint8_t header[9];
        header[0] = 0x04;
        header[1] = static_cast<uint8_t>(coordCount_ & 0xFF);
        header[2] = static_cast<uint8_t>((coordCount_ >> 8) & 0xFF);
        header[3] = static_cast<uint8_t>((coordCount_ >> 16) & 0xFF);
        header[4] = static_cast<uint8_t>((coordCount_ >> 24) & 0xFF);
        header[5] = static_cast<uint8_t>(previewStride_ & 0xFF);
        header[6] = static_cast<uint8_t>(previewStride_ >> 8);
        header[7] = epoch_;
        header[8] = 0;   // reserved: keeps the header the same width as 0x02's

        // In the COORD TABLE's order, or a mapped layout pairs each beam with a different fixture.
        const size_t bodyBytes = static_cast<size_t>(coordCount_) * 2;
        if (!staging_ || stagingCap_ < bodyBytes) return false;
        const nrOfLightsType s = previewStride_;
        struct AimCtx {
            uint8_t* out; size_t at; const uint8_t* src; nrOfLightsType n; uint8_t cpl;
            uint8_t panOff, tiltOff;
            void emit(nrOfLightsType idx) {
                const uint8_t* px = (idx < n) ? src + static_cast<size_t>(idx) * cpl : nullptr;
                out[at++] = (px && panOff  != FixtureChannels::kAbsent && panOff  < cpl)
                                ? px[panOff]  : 128;
                out[at++] = (px && tiltOff != FixtureChannels::kAbsent && tiltOff < cpl)
                                ? px[tiltOff] : 128;
            }
        };
        AimCtx aim{staging_, 0, src, n, cpl, fc.pan, fc.tilt};
        if (denseGrid()) {
            const lengthType W = layer_->physicalWidth(), H = layer_->physicalHeight();
            const lengthType az = layer_->physicalDepth() > 0 ? layer_->physicalDepth() : 1;
            const lengthType ay = H > 0 ? H : 1, ax = W > 0 ? W : 1;
            for (lengthType z = 0; z < az; z += s)
                for (lengthType y = 0; y < ay; y += s)
                    for (lengthType x = 0; x < ax; x += s)
                        aim.emit(static_cast<nrOfLightsType>(static_cast<size_t>(z) * H * W
                                                             + static_cast<size_t>(y) * W + x));
        } else if (keptIdx_ && keptCount_ == coordCount_) {
            for (nrOfLightsType k = 0; k < keptCount_; k++) aim.emit(keptIdx_[k]);
        } else {
            struct Skip { AimCtx* aim; nrOfLightsType s; } sk{&aim, s};
            layer_->layouts()->placeLights(CoordSink{[](void* c, nrOfLightsType idx, lengthType x, lengthType y, lengthType z) {
                auto* p = static_cast<Skip*>(c);
                if (x % p->s != 0 || y % p->s != 0 || z % p->s != 0) return;
                p->aim->emit(idx);
            }, nullptr, &sk});
        }
        return broadcaster_->sendBufferedFrame(header, sizeof(header), staging_, aim.at);
    }

    /// Stream one color frame, gathered into staging in the coord table's order.
    bool sendFrame() {
        if (!broadcaster_ || !sourceBuffer_ || !sourceBuffer_->data() || coordCount_ == 0) return false;
        const uint8_t* src = sourceBuffer_->data();
        const uint8_t cpl = sourceBuffer_->channelsPerLight();
        const nrOfLightsType n = sourceBuffer_->count();
        // The LINK's stride, coarser than the table's when the link is slow: the browser upsamples the colors back onto the full table it already holds.
        const nrOfLightsType s = frameStride();
        const nrOfLightsType sendCount = frameCount();

        // The epoch and stride pair is the client's table-cache key; drops is its congestion signal.
        uint8_t header[9];
        header[0] = 0x02;
        header[1] = static_cast<uint8_t>(sendCount & 0xFF);
        header[2] = static_cast<uint8_t>((sendCount >> 8) & 0xFF);
        header[3] = static_cast<uint8_t>((sendCount >> 16) & 0xFF);
        header[4] = static_cast<uint8_t>((sendCount >> 24) & 0xFF);
        header[5] = static_cast<uint8_t>(s & 0xFF);
        header[6] = static_cast<uint8_t>(s >> 8);
        header[7] = epoch_;
        header[8] = dropsSinceLast_;

        if (s == 1 && cpl == 3 && sendCount <= n) {
            // Full resolution: the producer buffer IS the payload, drained with no copy at all.
            const bool ok = broadcaster_->sendBufferedFrame(header, sizeof(header),
                                                            src, static_cast<size_t>(sendCount) * 3);
            if (ok) dropsSinceLast_ = 0;
            return ok;
        }

        // Gathered into staging, in the coord table's exact subset and order, or the browser drops it.
        const size_t bodyBytes = static_cast<size_t>(sendCount) * 3;
        if (!staging_ || stagingCap_ < bodyBytes) return false;   // alloc miss: skip, lossy channel
        struct ColCtx {
            uint8_t* out; size_t at; const uint8_t* src; nrOfLightsType n; uint8_t cpl;
            void emit(nrOfLightsType idx) {
                const uint8_t* px = (idx < n) ? src + static_cast<size_t>(idx) * cpl : nullptr;
                out[at++] = px ? px[0] : 0;
                out[at++] = (px && cpl >= 2) ? px[1] : 0;
                out[at++] = (px && cpl >= 3) ? px[2] : 0;
            }
        };
        ColCtx col{staging_, 0, src, n, cpl};
        if (denseGrid()) {
            const lengthType W = layer_->physicalWidth(), H = layer_->physicalHeight();
            const lengthType az = layer_->physicalDepth() > 0 ? layer_->physicalDepth() : 1;
            const lengthType ay = H > 0 ? H : 1, ax = W > 0 ? W : 1;
            for (lengthType z = 0; z < az; z += s)
                for (lengthType y = 0; y < ay; y += s)
                    for (lengthType x = 0; x < ax; x += s)
                        col.emit(static_cast<nrOfLightsType>(static_cast<size_t>(z) * H * W
                                                             + static_cast<size_t>(y) * W + x));
        } else if (keptIdx_ && keptCount_ == coordCount_) {
            // The cached index map, filtered by the frame's stride on the same bytes the browser holds.
            for (nrOfLightsType k = 0; k < keptCount_; k++)
                if (!keptPos_ || s == previewStride_ || keptAtStride(k, s)) col.emit(keptIdx_[k]);   // the stride test, on the bytes the browser has
        } else {
            // The alloc-miss fallback: the full lattice walk, at the same stride as the table's.
            struct Skip { ColCtx* col; nrOfLightsType s; } sk{&col, s};
            layer_->layouts()->placeLights(CoordSink{[](void* c, nrOfLightsType idx, lengthType x, lengthType y, lengthType z) {
                auto* p = static_cast<Skip*>(c);
                if (x % p->s != 0 || y % p->s != 0 || z % p->s != 0) return;
                p->col->emit(idx);
            }, nullptr, &sk});
        }
        const bool ok = broadcaster_->sendBufferedFrame(header, sizeof(header), staging_, col.at);
        if (ok) dropsSinceLast_ = 0;
        return ok;
    }

private:
    /// Free the preview buffers, cancelling any in-flight send so no drain outlives its buffer.
    void freePreviewBuffers() {
        if (broadcaster_) broadcaster_->cancelBufferedSend();
        if (keptIdx_) { platform::free(keptIdx_); keptIdx_ = nullptr; keptIdxCap_ = 0; keptCount_ = 0; }
        if (keptPos_) { platform::free(keptPos_); keptPos_ = nullptr; }
        if (staging_) { platform::free(staging_); staging_ = nullptr; stagingCap_ = 0; }
        publishHeapBytes();
    }

    /// Grow the one stable staging buffer the resumable drain reads across transport ticks.
    bool ensureStaging(size_t bytes) {
        if (stagingCap_ >= bytes) return staging_ != nullptr;
        auto* grown = static_cast<uint8_t*>(platform::alloc(bytes));
        if (!grown) return false;
        if (staging_) platform::free(staging_);
        staging_ = grown;
        stagingCap_ = bytes;
        publishHeapBytes();
        return true;
    }


    /// Publish who is watching and at what stride, or warn when the index cache could not fit.
    void refreshStatus() {
        if (keptIdxAllocFailed_) {
            setStatus("preview degraded — index cache alloc failed, gathering per frame (slower)",
                      Severity::Warning);
        } else if (lastClients_ > 0) {
            // Who is watching, and at what resolution they asked to be served.
            mm::formatTo(statusBuf_, sizeof(statusBuf_), "%d watching · 1/%u",
                          lastClients_, static_cast<unsigned>(downscale_));
            setStatus(statusBuf_, Severity::Status);
        } else {
            clearStatus();
        }
    }

    /// Refresh the watcher count in the status when it changes, and only then.
    void tick1s() MM_NONBLOCKING override {
        const int c = broadcaster_ ? broadcaster_->subscriberCount() : 0;
        if (c != lastClients_ || downscale_ != lastShownStride_) {
            lastClients_ = c;
            lastShownStride_ = downscale_;
            refreshStatus();
        }
        MoonModule::tick1s();
    }
    int lastClients_ = 0;
    nrOfLightsType lastShownStride_ = 0;
    char statusBuf_[40]{};
    bool keptIdxAllocFailed_ = false;     // index cache couldn't allocate → gather walks per frame
    nrOfLightsType* keptIdx_ = nullptr;   // sparse layouts: kept lights' buffer indices, coord-table order
    nrOfLightsType keptIdxCap_ = 0, keptCount_ = 0;
    // Each kept light's (x,y,z): a coarser frame applies the device's own modulo test rather than re-walking placeLights (~8 ms at 12K lights, per frame).
    uint8_t* keptPos_ = nullptr;

protected:
    /// This driver's heap: the base scratch plus the kept-index cache, for the memory readout.
    size_t driverHeapBytes() const override {
        return DriverBase::driverHeapBytes()
             + static_cast<size_t>(keptIdxCap_) * sizeof(nrOfLightsType)
             + static_cast<size_t>(keptIdxCap_) * 3   // keptPos_, three bytes per kept light
             + stagingCap_;
    }

private:

    // No LUT means Drivers passed the dense box buffer, so the closed-form path is valid.
    /// Whether the source is a dense grid in natural order, which needs no placeLights walk.
    bool denseGrid() const { return layer_ && !layer_->lut().hasLUT(); }

    nrOfLightsType maxPreviewPoints() const {
        // NO display cap: memory is the only bound, and everything else self-degrades where it binds.
        constexpr size_t kReserve = 32u * 1024u;     // leave this much contiguous headroom
        constexpr size_t kBytesPerPoint = 3u;        // RGB on the wire / position bytes in the table
        constexpr nrOfLightsType kFloor = 1024;      // always previewable (hard-downsampled) on any board
        constexpr uint32_t kTypeMax = static_cast<uint32_t>(std::numeric_limits<nrOfLightsType>::max());
        const size_t block = platform::maxAllocBlock();
        // maxAllocBlock() returns 0 = "unlimited / not reported" (desktop, test default).
        if (block == 0) return static_cast<nrOfLightsType>(kTypeMax);
        const size_t usable = block > kReserve ? block - kReserve : 0;
        uint32_t memPts = static_cast<uint32_t>(usable / kBytesPerPoint);
        if (memPts < kFloor) memPts = kFloor;
        if (memPts > kTypeMax) memPts = kTypeMax;
        return static_cast<nrOfLightsType>(memPts);
    }

    // Scaled by the largest box edge, so a >255 axis is not flattened onto the 255 plane.
    uint8_t scaleAxis(lengthType v) const {
        if (v < 0) return 0;
        int32_t s = posScale_ ? (static_cast<int32_t>(v) * 255 / posScale_) : v;
        return s > 255 ? 255 : static_cast<uint8_t>(s);
    }

    Buffer* sourceBuffer_ = nullptr;
    BinaryBroadcaster* broadcaster_ = nullptr;
    bool aimTurn_ = false;          // alternates color/aim so each gets its own send slot
    uint8_t* staging_ = nullptr;           // stable body for table + gathered frames (see ensureStaging)
    size_t stagingCap_ = 0;
    nrOfLightsType coordCount_ = 0;        // lights the lattice keeps = the streamed 0x03/0x02 count
    nrOfLightsType previewStride_ = 1;     // wire field: the lattice/downscale factor (1 = full res)
    uint8_t bx_ = 0, by_ = 0, bz_ = 0;
    int32_t posScale_ = 0;            // 0 = positions 1:1; else largest box edge (>255) to scale by
    uint32_t lastSendTime_ = 0;
    // Written on the transport thread and read on the encode one: single bytes, benign to race.
    static constexpr int kMaxRequestSlots = 8;
    volatile uint8_t reqStride_[kMaxRequestSlots] = {};   // 0 = no standing request in this slot
    volatile uint8_t reqFps_[kMaxRequestSlots] = {};      // 0 = use the targetFps control
    volatile bool tableRequested_ = false;                // a [0x52] is owed the table
    uint8_t epoch_ = 0;             // bumped per geometry rebuild; the table-cache key's half
    uint8_t dropsSinceLast_ = 0;    // frames discarded at the source since the last delivered one
    size_t stagingUsed_ = 0;        // byte length of the last-built table in staging_

    // The coarsest standing client request; 1 is full resolution and the value with no request.
    nrOfLightsType downscale_ = 1;
};

} // namespace mm
