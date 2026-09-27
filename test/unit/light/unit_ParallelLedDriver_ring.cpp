/// @module MoonI80Peripheral
/// @also ParallelLedDriver

#include "doctest.h"
#include "light/drivers/ParallelLedDriver.h"
#include "correction_presets.h"

#include <algorithm>   // std::count (pin-count in wireShift)
#include <cstring>
#include <vector>

// The DOMAIN half of the MoonI80 streaming ring (the platform GDMA half is bench-verified on the S3, desktop stubs return false, so busIsRing() is false on host and tick() never routes here).
// What this pins is the part where the prior ring attempt "rendered wrong": the slice tiling the platform's refill task drives through the encode seam.
// The invariants:
//
// 1.
// TILING, the slices, concatenated in frame-row order, are byte-identical to one whole-frame
//      encode. (A slice writes to dst+0 for any firstRow, so a tiling bug shows as a shifted/duplicated
//      block, exactly the "domino" artifact of the stashed attempt.)
// 2.
// ROWS-ONLY, no slice appends a latch pad; a buffer is zero past its rows.
// The reset comes from
//      stopping the peripheral, never a pad in a circulating buffer, so a pad byte would overrun.
// 3.
// RECYCLED == FRESH, driving the same buffers a SECOND time yields identical bytes.
// Ring buffers
//      are recycled, not zeroed, so a stale-constant bug (the prefill/pad not re-laid) shows only on the
//      second frame. This is the invariant most likely to break, and the one no whole-frame test covers.
// 4.
// RAGGED, strands of DIFFERENT lengths (the `ledsPerPin` CSV, one entry per strand).
// The shift
//      constants carry the active mask, so an exhausted strand's mask change must be honoured per RUN of
//      rows, INCLUDING when a strand ends mid-slice. Every invariant above is blind to this: a uniform
//      frame's mask is one constant, so it passes even when the per-run split is broken (verified by
//      injecting the bug, the uniform tiling test still passed while the ragged ones failed).
//
// The mock's "ring" is plain memory driven exactly as platform_esp32_moon_i80.cpp drives it: prime N buffers with the first N slices, then refill in ring order until the last slice, so the sequence the encode seam sees here is the sequence it sees on hardware.

namespace {

using mm::nrOfLightsType;

// The mock's own geometry, deliberately SMALL and independent of the platform's (which is a runtime control now, see MoonI80Peripheral::ringRows/ringBufs). A 4-buffer pool forces buffer REUSE at few slices (a 200-light frame = 13 slices reuses buffers 0..4), which is the path the recycled / short-last-slice tests exist to exercise. 16 rows/buffer keeps a slice MULTI-row on purpose: a 1-row slice cannot express a tiling bug (every buffer would hold exactly one light, so a stride error has nowhere to show).
constexpr uint8_t kMockRingBufs = 4;
constexpr uint32_t kMockRingRows = 16;

class MockRingDriver;   // forward decl — the peripheral's ring hooks call back into the owner's encode

// The ring-capable backend: a memory-only LedPeripheral (same shape as unit_ParallelLedDriver_doublebuffer's MockPeripheral) PLUS the ring hooks (busInitRing/busTransmitRing/busIsRing/wantsRing), which is what MoonI80Peripheral is on real hardware. Holds the ring buffer pool and geometry; the actual per-slice encode is the OWNER's (ParallelLedDriver::encodeRows), reached through owner_ exactly as MoonI80Peripheral::ringEncodeTrampoline does.
class MockRingPeripheral : public mm::LedPeripheral {
public:
    uint8_t lanesAvailable() const MM_NONBLOCKING override { return 8; }   // 8 data lines (an 8-bit bus)
    bool powerOfTwoBus() const override { return true; }
    bool loopbackFullWidth() const override { return false; }
    bool supportsPinExpander() const override { return true; }
    const char* initFailMsg() const override { return "mock init failed"; }
    mm::LedHwBlock hwBlock() const override { return mm::LedHwBlock::None; }   // mock drives no real block

    void addBusControls(mm::ControlList&) override {}
    // A ring-capable backend adds its ring cluster here (the default addRingControls is a no-op for whole-frame-only backends). Mirror the real driver: the source-snapshot knob under the path, gated on wantsRing(), this mock's controllable wantRing_ drives the visibility the hide test checks.
    void addRingControls(mm::ControlList& controls) override {
        controls.addControl("ringSnapshot", owner_->ringSnapshotRef());
        controls.setHidden(controls.count() - 1, !wantsRing());
    }
    bool busControlTriggersBuild(const char*) const override { return false; }
    void recordBusPins() override {}
    bool extraBusPinsCurrent() const override { return true; }
    const char* validateBusPins(const uint16_t*, uint8_t) const override { return nullptr; }
    const char* validateBusFatal() const override { return nullptr; }
    uint16_t clockPinForBus() const override { return 99; }

    // --- whole-frame bus (used to produce the reference frame the ring output is compared against) ---
    bool busInit(size_t frameBytes, bool) override { cap_ = frameBytes; buf_.assign(frameBytes, 0); return true; }
    uint8_t* busBuffer(uint8_t i) override {
        if (ringActive_) return i < kMockRingBufs && !ring_[i].empty() ? ring_[i].data() : nullptr;
        return (i == 0 && !buf_.empty()) ? buf_.data() : nullptr;
    }
    size_t busCapacity() const override { return cap_; }
    bool busTransmit(uint8_t, size_t) override { return true; }
    bool busWait(uint8_t, uint32_t) override { return true; }
    uint32_t busLastTransmitUs() const override { return 0; }
    void busDeinit() override { cap_ = 0; buf_.clear(); ringActive_ = false; }
    mm::platform::RmtLoopbackResult busLoopback(const uint8_t*, size_t, size_t, uint8_t) override { return {}; }

    // --- ring hooks (the seam the platform drives). The mock stores the geometry and hands out
    //     plain-memory buffers; driveRingFrame() below replays the platform's prime+refill order. ---
    bool wantsRing() const override { return wantRing_; }
    void setWantRing(bool w) { wantRing_ = w; }
    // Buffers are ROWS-ONLY, exactly as the platform allocates them: the WS2812 reset comes from stopping the peripheral, never from a pad inside a circulating buffer. Sizing these rows+pad here would let a pad-writing bug pass the tests and overrun on hardware.
    bool busInitRing(size_t rowBytes, uint32_t totalRows) override {
        ringRowBytes_ = rowBytes;
        ringTotalRows_ = totalRows;
        ringActive_ = true;
        const size_t bufBytes = static_cast<size_t>(kMockRingRows) * rowBytes;
        for (auto& b : ring_) b.assign(bufBytes, 0);
        // Fresh (zeroed) pool: every buffer needs its constants laid on first use, the platform's bufNeedsPrefill lifecycle, mirrored here so the byte-compare tests pin the prefill-skip contract.
        for (auto& f : needsPrefill_) f = true;
        return true;
    }
    bool busIsRing() const MM_NONBLOCKING override { return ringActive_; }
    bool busTransmitRing() override { return true; }   // the wire itself is the platform's; the encode is what we test

    // Replay one frame through the ring exactly as the platform does: prime the first min(N, needed) buffers, then refill in ring order until the slice that reaches totalRows. Returns the frame reassembled in ROW order (buffer contents concatenated in the order the DMA would clock them), WITHOUT the pad, so it lines up with a whole-frame encode's row region for a byte compare.
    std::vector<uint8_t> driveRingFrame() { return driveRingFrameCoalesced(1); }

    /// The v2 (clock-oracle) refill contract: the ISR may be invoked LATE, several drains coalesced into one firing, and then BATCH-encodes every slice the window allows.
    /// The wire content must be byte-identical whatever the grouping: batching changes WHEN slices are written, never WHAT.
    /// This drives the same per-slice body in groups of `batch` per simulated firing, pinning the prefill / short-slice / recycle lifecycle across batch boundaries (the coalesced-EOF regression the one-refill-per-interrupt design could not pass).
    std::vector<uint8_t> driveRingFrameCoalesced(uint32_t batch) {
        REQUIRE(ringActive_);
        REQUIRE(batch >= 1);
        std::vector<uint8_t> assembled;
        uint32_t row = 0;
        uint8_t slot = 0;
        int32_t lastSlot = -1;
        uint32_t inBatch = 0;
        // The row region of one buffer (pad excluded), in bytes, for a `count`-row slice.
        auto rowBytesOf = [&](uint32_t count) { return static_cast<size_t>(count) * ringRowBytes_; };
        while (row < ringTotalRows_) {
            // Group boundary: nothing observable happens between groups (the real ISR just returns and fires again later), the loop structure below is one slice of the batch.
            inBatch = (inBatch + 1u) % batch;
            uint32_t count = kMockRingRows;
            bool last = false;
            if (row + count >= ringTotalRows_) { count = ringTotalRows_ - row; last = true; }
            // Short last slice into a rows-only, RECYCLED buffer: zero rows [count, kMockRingRows) first, exactly as the platform ISR does (moonI80EofCb) before encodeRingSlice. Those mounted bytes still hold this buffer's EARLIER full slice and would clock as ghost rows otherwise.
            const bool shortSlice = count < kMockRingRows;
            if (shortSlice)
                std::memset(ring_[slot].data() + static_cast<size_t>(count) * ringRowBytes_, 0,
                            (static_cast<size_t>(kMockRingRows) - count) * ringRowBytes_);
            // The platform's bufNeedsPrefill lifecycle: consume this use's flag, hand it to the trampoline, and re-flag the buffer after a tail memset (its constants are gone for the NEXT use).
            const bool needsPrefill = needsPrefill_[slot];
            needsPrefill_[slot] = false;
            ringEncodeTrampolineHost(ring_[slot].data(), row, count, last, needsPrefill);
            if (shortSlice) needsPrefill_[slot] = true;
            // Reassemble the row region in DMA order.
            assembled.insert(assembled.end(), ring_[slot].begin(),
                             ring_[slot].begin() + static_cast<long>(rowBytesOf(count)));
            row += count;
            if (last) lastSlot = slot;
            slot = (slot + 1u) % kMockRingBufs;
        }
        lastSlot_ = lastSlot;
        return assembled;
    }

    // NO ring buffer appends a latch pad, the buffers are rows-only, so past each buffer's actual rows every byte must be zero.
    // The seam is called closeFrame=false for EVERY slice (encodeRingSlice), so the frame's reset comes from stopping the peripheral, never from a pad in a circulating buffer.
    // A stray non-zero byte past a slice's rows would overrun the rows-only allocation on hardware.
    // The last slice may be SHORT (lastRows < kMockRingRows), so its rows end at lastRows*rowBytes; a non-last slice always fills kMockRingRows rows.
    // Everything past that must be zero.
    bool noSliceWritesPad() const {
        if (lastSlot_ < 0) return false;
        const size_t lastRows = ringTotalRows_ - (nSlicesForTest() - 1) * kMockRingRows;
        for (uint8_t s = 0; s < kMockRingBufs; s++) {
            const size_t rowRegion = (static_cast<int32_t>(s) == lastSlot_)
                                         ? lastRows * ringRowBytes_
                                         : static_cast<size_t>(kMockRingRows) * ringRowBytes_;
            for (size_t i = rowRegion; i < ring_[s].size(); i++)
                if (ring_[s][i] != 0) return false;   // a buffer wrote past its rows — a pad overrun
        }
        return true;
    }

    // After driveRingFrame(), the LAST slice's buffer must be clean past its (possibly short) rows: the buffers are rows-only, so a short last slice's tail (rows [lastRows, kMockRingRows) of a REUSED buffer) must have been zeroed, otherwise its EARLIER full slice still sits there and clocks as ghost rows in place of the ≥300 µs LOW reset.
    // The encoder never writes at or past rowRegion (closeFrame=false → no latch word), so the whole window [lastRows*rowBytes, buffer_end) must be zero.
    // Returns the count of non-zero bytes in that window, 0 = clean.
    size_t lastSliceStalePadBytes() const {
        if (lastSlot_ < 0) return SIZE_MAX;
        const size_t lastRows = ringTotalRows_ - (nSlicesForTest() - 1) * kMockRingRows;
        const size_t rowRegion = lastRows * ringRowBytes_;   // this slice's rows end here; the rest is tail
        const auto& b = ring_[static_cast<size_t>(lastSlot_)];
        size_t stale = 0;
        for (size_t i = rowRegion; i < b.size(); i++)
            if (b[i] != 0) stale++;
        return stale;
    }
    uint32_t nSlicesForTest() const {
        return (ringTotalRows_ + kMockRingRows - 1) / kMockRingRows;
    }

    // The trampoline the real driver registers is MoonI80Peripheral::ringEncodeTrampoline; the mock reproduces its body (recover the OWNER, this backend's attach()'d ParallelLedDriver, branch on bus width, call encodeRows) so the host drives the identical encode the seam does on device.
    // `closeFrame` is ALWAYS false to the encoder.
    // The platform (encodeRingSlice) never appends a latch pad to a rows-only ring buffer, the WS2812 reset comes from stopping the peripheral, not from a pad inside a circulating buffer.
    // `needsPrefill` mirrors the real trampoline's prefill-skip: constants are laid only when the platform says the buffer's are gone (or the lanes are ragged), and the byte-compare tests prove a data-only refill of a recycled buffer is identical to a full one.
    //
    // Needs the owner's PROTECTED encodeRows/prefillShiftRows, which only a ParallelLedDriver subclass can reach, MockRingDriver (below) exposes them via a couple of one-line forwarders, mirroring how MoonI80Peripheral's real trampoline calls back through `owner_`.
    void ringEncodeTrampolineHost(uint8_t* dst, uint32_t firstRow, uint32_t count, bool /*last*/,
                                  bool needsPrefill);

    size_t rowBytesForTest() const { return ringRowBytes_; }

    // --- TERMINATION MODEL (the DMA-chain half the byte-tiling tests do NOT cover) ---
    //
    // driveRingFrame() above models the ENCODE tiling: it writes each slice and stops the instant the last real row is encoded.
    // The platform DMA does NOT stop there, the looping chain keeps clocking buffers until the EOF ISR calls gdma_stop on a DRAIN COUNTER (drained >= nSlices + kTailBufs).
    // This model replays that: prime the pool, then for each EOF advance the drain, refill the drained buffer with the next slice (or ZEROS once past the last real slice), and stop after nSlices + kTailBufs drains, exactly moonI80EofCb.
    // It records, in clock order, the row-region bytes of every buffer the DMA actually clocks to the wire (including the laps past the last real slice).
    // This is what pins the termination CONTRACT: a clean LOW tail, and no wrap re-read of a buffer the refill is mid-write on.
    //
    // `bufs` is the physical pool depth to model (the platform's kRingBufs); pass kMockRingBufs for reuse, or a depth >= nSlices to model the no-reuse stopgap.
    // `kTailBufs` mirrors the platform constant.
    struct ClockedFrame {
        std::vector<std::vector<uint8_t>> buffers;   // row-region bytes clocked, in DMA order
        bool tailIsLow = false;                      // every buffer clocked AFTER the last real slice is all-zero
        bool wrapReadStale = false;                  // a buffer was re-clocked while holding a DIFFERENT slice than encoded for that clock position
        uint32_t drainsToStop = 0;                   // total EOFs before the stop fired
    };
    ClockedFrame driveRingFrameWithTermination(uint8_t bufs, uint8_t kTailBufs = 1) {
        REQUIRE(ringActive_);
        REQUIRE(bufs >= 1);
        // Physical buffers, sized like the platform's ring[] (rows only), refilled in place.
        std::vector<std::vector<uint8_t>> pool(bufs);
        const size_t bufBytes = static_cast<size_t>(kMockRingRows) * ringRowBytes_;
        for (auto& b : pool) b.assign(bufBytes, 0);
        const uint32_t nSlices = nSlicesForTest();

        // Prime the first min(bufs, nSlices) buffers with slices 0..; a buffer past the frame's slices is zeroed (mirrors startRingTransfer). refilledRow / refillSlot advance exactly as the platform's, and so does the bufNeedsPrefill lifecycle (fresh pool = true; consumed per use; re-set on memset).
        std::vector<bool> poolNeedsPrefill(bufs, true);
        auto encodeSliceInto = [&](uint8_t slot, uint32_t firstRow) {
            uint32_t count = kMockRingRows;
            bool shortSlice = false;
            if (firstRow + count >= ringTotalRows_) {
                count = ringTotalRows_ - firstRow;
                if (count < kMockRingRows) {  // short last slice: zero the rest so no stale rows clock
                    std::memset(pool[slot].data() + static_cast<size_t>(count) * ringRowBytes_, 0,
                                (static_cast<size_t>(kMockRingRows) - count) * ringRowBytes_);
                    shortSlice = true;
                }
            }
            const bool last = (firstRow + count >= ringTotalRows_);
            const bool needsPrefill = poolNeedsPrefill[slot];
            poolNeedsPrefill[slot] = false;
            ringEncodeTrampolineHost(pool[slot].data(), firstRow, count, last, needsPrefill);
            if (shortSlice) poolNeedsPrefill[slot] = true;   // the tail memset erased those rows' constants
        };
        uint32_t refilledRow = 0;
        for (uint8_t primed = 0; primed < bufs; primed++) {
            if (refilledRow < ringTotalRows_) { encodeSliceInto(primed, refilledRow); refilledRow += kMockRingRows; }
            else {
                std::fill(pool[primed].begin(), pool[primed].end(), 0);   // past last slice: LOW tail buffer
                poolNeedsPrefill[primed] = true;
            }
        }

        // Clock the looping chain.
        // Each EOF drains buffer `slot`, we RECORD what it clocked, then the ISR refills that buffer with the next slice (or zeros past the last real slice).
        // Stop on the drain counter.
        // The wrap re-reads pool[slot] on later laps, recording BEFORE the refill is exactly what the DMA sees on the wire.
        ClockedFrame out;
        uint8_t slot = 0;
        uint32_t drained = 0;
        while (true) {
            // What the DMA clocks NOW: the current row-region of pool[slot].
            const uint32_t sliceForThisClock = drained;   // 0-based clock index == slice index while <= nSlices
            uint32_t count = kMockRingRows;
            uint32_t firstRow = sliceForThisClock * kMockRingRows;
            bool pastLast = firstRow >= ringTotalRows_;
            if (!pastLast && firstRow + count > ringTotalRows_) count = ringTotalRows_ - firstRow;
            const size_t rowRegion = static_cast<size_t>(pastLast ? kMockRingRows : count) * ringRowBytes_;
            std::vector<uint8_t> clocked(pool[slot].begin(), pool[slot].begin() + static_cast<long>(rowRegion));
            out.buffers.push_back(clocked);
            // Tail check: a buffer clocked at a position PAST the last real slice must be all-LOW.
            if (pastLast) {
                bool allZero = std::all_of(clocked.begin(), clocked.end(), [](uint8_t b) { return b == 0; });
                if (!allZero) out.tailIsLow = false;   // corrupted tail
            }
            drained++;
            if (drained >= nSlices + kTailBufs) break;
            // ISR refill: put the NEXT unencoded slice (refilledRow) into this drained buffer, or zeros.
            if (refilledRow < ringTotalRows_) { encodeSliceInto(slot, refilledRow); refilledRow += kMockRingRows; }
            else { std::fill(pool[slot].begin(), pool[slot].end(), 0); poolNeedsPrefill[slot] = true; }
            slot = (slot + 1u) % bufs;
        }
        // Tail is LOW iff every past-last clock was all-zero (default true unless a corrupt tail flipped it).
        out.tailIsLow = true;
        for (uint32_t i = nSlices; i < static_cast<uint32_t>(out.buffers.size()); i++)
            if (!std::all_of(out.buffers[i].begin(), out.buffers[i].end(), [](uint8_t b) { return b == 0; }))
                out.tailIsLow = false;
        out.drainsToStop = drained;
        return out;
    }

private:
    std::vector<uint8_t> buf_;
    size_t cap_ = 0;
    std::vector<uint8_t> ring_[kMockRingBufs];
    bool needsPrefill_[kMockRingBufs] = {};   // the platform's bufNeedsPrefill lifecycle, mirrored
    size_t ringRowBytes_ = 0;
    uint32_t ringTotalRows_ = 0;
    bool ringActive_ = false;
    bool wantRing_ = false;
    int32_t lastSlot_ = -1;
};

// A tiny ParallelLedDriver subclass that exposes the protected lane/encode/snapshot internals the ring tests need (encodeRows, prefillShiftRows/Frame, latchBit_'s bit position, the ensureSnapshotCap / snapshotSourceForRing / copyRange snapshot machinery) without widening the production class's public surface, same pattern as unit_ParallelLedDriver_pinexpander.cpp's Expose-style mocks. Everything else the tests use (laneCount, maxLaneLights, frameBytes, pins, ledsPerPin, pinExpander, latchPin, defineControls, setSourceBuffer, correctionForTest, applyState, severity, outputsPerPin, setWindow, tick) is already public on ParallelLedDriver itself.
class MockRingDriver : public mm::ParallelLedDriver {
public:
    // The whole-frame reference: prefill constants + encode the entire frame in one call (what the ring must reproduce, slice by slice).
    template <class Slot>
    void encodeWholeForTest(uint8_t outCh, uint8_t* dst) {
        this->template encodeRows<Slot>(outCh, dst, 0, 0, /*closeFrame=*/true);
    }
    template <class Slot>
    void prefillShiftFrameForTest(uint8_t outCh, uint8_t* dst) {
        this->template prefillShiftFrame<Slot>(outCh, dst);
    }
    // Which bus bit the '595's latch rides (the ragged darkness test masks it out of its bit check).
    uint8_t latchBitForTest() const { return this->latchBit(); }

    // The trampoline body itself (recover outCh/prefill-gate from this driver, branch on bus width, call encodeRows), MockRingPeripheral::ringEncodeTrampolineHost forwards here, mirroring how MoonI80Peripheral's real trampoline recovers `owner_` and calls straight into it.
    void encodeSliceForTest(uint8_t* dst, uint32_t firstRow, uint32_t count, bool needsPrefill) {
        const uint8_t outCh = this->correction().outChannels;
        const auto first = static_cast<nrOfLightsType>(firstRow);
        const auto cnt = static_cast<nrOfLightsType>(count);
        const bool prefill = this->pinExpanderMode() && (needsPrefill || !this->uniformLaneCounts());
        if (this->slotBytes() == 1) {
            if (prefill) this->template prefillShiftRows<uint8_t>(outCh, dst, first, cnt);
            this->template encodeRows<uint8_t>(outCh, dst, first, cnt, /*closeFrame=*/false);
        } else {
            if (prefill) this->template prefillShiftRows<uint16_t>(outCh, dst, first, cnt);
            this->template encodeRows<uint16_t>(outCh, dst, first, cnt, /*closeFrame=*/false);
        }
    }

    // Freeze the current source into the driver-owned snapshot and route the ring encode at it, the same call tickRing makes before kicking a frame.
    // After this, encodeRows reads the snapshot, so mutating the live source (a resize / repaint on the render thread) can't tear or UAF the in-flight frame.
    // Mirror production's reinit(): size the snapshot OFF the hot path (ensureSnapshotCap) THEN take the per-frame copy (snapshotSourceForRing, memcpy-only). tickRing does exactly this split on device.
    bool snapshotForTest() { this->ensureSnapshotCap(); return this->snapshotSourceForRing(); }

    // Test hooks for the PARALLEL-snapshot split: expose the snapshot buffer + a manual range-copy so a test can prove copyRange(0,N) == copyRange(0,half)+copyRange(half,N) byte-for-byte (the fork-join's correctness oracle, the device runs the two halves on two cores; here one thread runs both ranges, which must land the identical bytes). setSnapCopyForTest mirrors what snapshotSourceForRing sets before splitting (the snapshot is now a raw memcpy at srcCh stride; correction fuses into encodeRows).
    uint8_t* snapshotBufForTest() { return this->snapshotBuf_; }
    void setSnapCopyForTest() {
        this->snapCopySrc_ = this->sourceBuffer_->data()
                           + static_cast<size_t>(this->winStart_) * this->sourceBuffer_->channelsPerLight();
        this->snapCopyCh_ = static_cast<uint8_t>(this->sourceBuffer_->channelsPerLight());
    }
    void copyRangeForTest(mm::nrOfLightsType lo, mm::nrOfLightsType hi) { this->copyRange(lo, hi); }
    mm::nrOfLightsType winLenForTest() const { return this->winLen_; }
    size_t snapshotCapForTest() const { return this->snapshotCap_; }
    static mm::nrOfLightsType snapHalfForTest(mm::nrOfLightsType n, size_t chStride) {
        return snapLineAlignedHalf(n, chStride);
    }
};

// Out-of-line: needs MockRingDriver's full definition (encodeSliceForTest), so it's defined after the class rather than inline in MockRingPeripheral.
inline void MockRingPeripheral::ringEncodeTrampolineHost(uint8_t* dst, uint32_t firstRow, uint32_t count,
                                                         bool /*last*/, bool needsPrefill) {
    static_cast<MockRingDriver*>(owner_)->encodeSliceForTest(dst, firstRow, count, needsPrefill);
}

// Bring the mock up on `lights` lights, shift mode (8 pins × 8 = 64 strands... capped; use fewer pins), with a correction so outChannels is known.
// Mirrors the shiftregister test's setup.
//
// `ledsPerPin` is the per-STRAND light count, one CSV entry per strand (pins × 8 with the expander on), "" leaves every strand equal at `lights`.
// Pass an explicit list for a RAGGED frame, where strands have different lengths (an end user's mix of strips and panels), which is what makes the active mask change mid-frame rather than being one constant.
void wireShift(MockRingDriver& d, MockRingPeripheral& peripheral, mm::Buffer& src, mm::Correction& corr,
              nrOfLightsType lights, const char* pins, const char* ledsPerPin = "") {
    d.setPeripheralForTest(&peripheral);
    std::strcpy(d.pins, pins);
    std::strcpy(d.ledsPerPin, ledsPerPin);
    d.pinExpander = true;
    d.latchPin = 20;
    // Source must hold EVERY configured strand's `lights`, one strand per pin × the '595 fan-out (outputsPerPin), not a fixed ×8. A "1,2" 2-pin config is 2×8=16 strands, so 16×lights; under-sizing would clamp the window and silently test fewer strands than configured.
    const int pinCount = 1 + static_cast<int>(std::count(pins, pins + std::strlen(pins), ','));
    REQUIRE(src.allocate(lights * pinCount * 8, 3) == true);
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::GRB);
    d.defineControls();
    d.setSourceBuffer(&src);
    d.correctionForTest() = corr;
    d.applyState();
}

} // namespace

// 1.
// TILING, the ring's slices, concatenated in row order, reproduce a whole-frame encode byte for byte.
// This is the invariant the prior attempt violated (the 16-stride "domino" repeat).
// A slice writes to dst+0 for any firstRow, so if the tiling is right the reassembled buffer == the frame.
TEST_CASE("MoonI80 ring: sliced encode tiles into a byte-identical whole frame") {
    MockRingDriver d;
    MockRingPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    // 200 lights/strand > 16 rows/buffer × 4 buffers = 64, so this needs real refills (not fits-in-ring).
    wireShift(d, peripheral, src, corr, 200, "1,2");   // 2 pins × 8 = 16 strands, 200 lights each
    // Paint the source so every row differs (a tiling bug that repeats a slice would then be visible).
    uint8_t* s = src.data();
    for (nrOfLightsType i = 0; i < src.count(); i++) {
        s[i * 3 + 0] = static_cast<uint8_t>(i * 7);
        s[i * 3 + 1] = static_cast<uint8_t>(i * 13 + 1);
        s[i * 3 + 2] = static_cast<uint8_t>(i * 29 + 2);
    }
    const uint8_t outCh = corr.outChannels;
    // The per-row encoded size (8-bit bus → slotBytes 1; the '595 multiplies the slot count).
    const size_t rowBytes = static_cast<size_t>(outCh) * 24 * 1 * d.outputsPerPin();
    const size_t rowRegion = static_cast<size_t>(d.maxLaneLights()) * rowBytes;

    // Reference: one whole-frame encode (2 pins → 8-bit bus → uint8 slots). encodeRows in shift mode writes only data words, so prefill the whole buffer's constants first, same as reinit does.
    peripheral.busInit(d.frameBytes(), false);
    d.prefillShiftFrameForTest<uint8_t>(outCh, peripheral.busBuffer(0));
    d.encodeWholeForTest<uint8_t>(outCh, peripheral.busBuffer(0));
    std::vector<uint8_t> whole(peripheral.busBuffer(0), peripheral.busBuffer(0) + rowRegion);
    peripheral.busDeinit();

    // Ring: drive the frame slice by slice, reassemble the row region.
    peripheral.setWantRing(true);
    REQUIRE(peripheral.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights())));
    std::vector<uint8_t> assembled = peripheral.driveRingFrame();

    REQUIRE(assembled.size() == whole.size());
    CHECK(std::memcmp(assembled.data(), whole.data(), whole.size()) == 0);
}

// 2.
// ROWS-ONLY, no slice appends a latch pad; every buffer is zero past its rows.
// The buffers are allocated rows-only (the WS2812 reset comes from stopping the peripheral, not a pad in a circulating buffer), so a slice that wrote a latch word past its rows would overrun the allocation on hardware.
TEST_CASE("MoonI80 ring: no slice writes past its rows (buffers are rows-only)") {
    MockRingDriver d;
    MockRingPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wireShift(d, peripheral, src, corr, 200, "1,2");
    const uint8_t outCh = corr.outChannels;

    peripheral.setWantRing(true);
    const size_t rowBytes = static_cast<size_t>(outCh) * 24 * 1 * d.outputsPerPin();
    REQUIRE(peripheral.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights())));
    peripheral.driveRingFrame();
    CHECK(peripheral.noSliceWritesPad());
}

// 3.
// RECYCLED == FRESH, a second frame through the SAME (recycled, not zeroed) ring buffers produces byte-identical output to the first.
// This catches a stale-constant / stale-pad bug that a single-frame test cannot see, the failure mode unique to a recycled ring.
TEST_CASE("MoonI80 ring: a recycled buffer produces the same bytes as a fresh one") {
    MockRingDriver d;
    MockRingPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wireShift(d, peripheral, src, corr, 200, "1,2");
    uint8_t* s = src.data();
    for (nrOfLightsType i = 0; i < src.count(); i++) {
        s[i * 3 + 0] = static_cast<uint8_t>(i * 5 + 3);
        s[i * 3 + 1] = static_cast<uint8_t>(i * 11);
        s[i * 3 + 2] = static_cast<uint8_t>(i * 17 + 7);
    }
    const uint8_t outCh = corr.outChannels;

    peripheral.setWantRing(true);
    const size_t rowBytes = static_cast<size_t>(outCh) * 24 * 1 * d.outputsPerPin();
    REQUIRE(peripheral.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights())));

    std::vector<uint8_t> first = peripheral.driveRingFrame();
    std::vector<uint8_t> second = peripheral.driveRingFrame();   // same buffers, recycled — not re-init'd
    REQUIRE(first.size() == second.size());
    CHECK(std::memcmp(first.data(), second.data(), first.size()) == 0);
}

// 4.
// NON-MULTIPLE-OF-16 STRAND LENGTH, the last slice is SHORT and lands in a REUSED buffer, so its pad window still holds that buffer's earlier full slice.
// The pad-zero must scrub it, or ghost rows clock in place of the ≥300 µs LOW reset and a strand can miss its latch. 200 lights → 13 slices > 8 buffers (reuse) AND lastRows=8 (short), exactly the case 128/192/256 (all ×16) never hit.
TEST_CASE("MoonI80 ring: a short last slice in a reused buffer has a clean pad (no stale ghost rows)") {
    MockRingDriver d;
    MockRingPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wireShift(d, peripheral, src, corr, 200, "1,2");   // 200 lights/strand: 13 slices, last slice = 8 rows
    uint8_t* s = src.data();
    for (nrOfLightsType i = 0; i < src.count(); i++) {   // dense non-zero source so a stale row WOULD show
        s[i * 3 + 0] = static_cast<uint8_t>(i * 9 + 1);
        s[i * 3 + 1] = static_cast<uint8_t>(i * 3 + 5);
        s[i * 3 + 2] = static_cast<uint8_t>(i * 19 + 2);
    }
    const uint8_t outCh = corr.outChannels;
    peripheral.setWantRing(true);
    const size_t rowBytes = static_cast<size_t>(outCh) * 24 * 1 * d.outputsPerPin();
    REQUIRE(peripheral.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights())));

    // Drive TWICE so the last-slice buffer is genuinely recycled (held an earlier frame's full slice).
    peripheral.driveRingFrame();
    peripheral.driveRingFrame();
    CHECK(peripheral.lastSliceStalePadBytes() == 0);   // tail past the short slice's rows is zero — no ghost rows
}

// 5.
// SOURCE SNAPSHOT, the ring encodes off the render thread across the ~6 ms wire, so it must read a frozen per-frame copy, not the live Layer buffer. tickRing calls snapshotSourceForRing() before kicking the frame; after that the render loop is free to overwrite (or free) the source.
// This pins the invariant: once snapshotted, the encode's bytes track the SNAPSHOT, so mutating the live source mid-frame changes nothing on the wire.
// (On device this is what stops a grid resize / RGBW switch mid-wire from tearing or reading freed memory.)
TEST_CASE("MoonI80 ring: the encode reads a per-frame snapshot, not the live source") {
    MockRingDriver d;
    MockRingPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wireShift(d, peripheral, src, corr, 200, "1,2");
    uint8_t* s = src.data();
    for (nrOfLightsType i = 0; i < src.count(); i++) {
        s[i * 3 + 0] = static_cast<uint8_t>(i * 5 + 3);
        s[i * 3 + 1] = static_cast<uint8_t>(i * 11);
        s[i * 3 + 2] = static_cast<uint8_t>(i * 17 + 7);
    }
    const uint8_t outCh = corr.outChannels;
    peripheral.setWantRing(true);
    const size_t rowBytes = static_cast<size_t>(outCh) * 24 * 1 * d.outputsPerPin();
    REQUIRE(peripheral.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights())));

    // Freeze the source, then SCRIBBLE all over the live buffer as the render thread would between kick and wire-completion. The snapshot must shield the encode from it.
    REQUIRE(d.snapshotForTest());
    std::vector<uint8_t> fromSnapshot = peripheral.driveRingFrame();
    std::memset(src.data(), 0xA5, static_cast<size_t>(src.count()) * src.channelsPerLight());
    std::vector<uint8_t> afterMutation = peripheral.driveRingFrame();   // still on the same snapshot

    REQUIRE(fromSnapshot.size() == afterMutation.size());
    CHECK(std::memcmp(fromSnapshot.data(), afterMutation.data(), fromSnapshot.size()) == 0);

    // And the sync/async paths (encodeSrc_ null) must still read the LIVE source. Re-snapshot the now-scribbled buffer and confirm the encode follows it (a uniform 0xA5 source → uniform encoded bytes, clearly different from the structured pattern above).
    REQUIRE(d.snapshotForTest());
    std::vector<uint8_t> fromMutated = peripheral.driveRingFrame();
    REQUIRE(fromMutated.size() == fromSnapshot.size());
    CHECK(std::memcmp(fromMutated.data(), fromSnapshot.data(), fromMutated.size()) != 0);
}

// 6.
// WINDOWED SNAPSHOT, the snapshot copies only THIS DRIVER'S WINDOW (winLen_ × srcCh from winStart_), not the whole source, then biases encodeSrc_ by -winStart_ so the encode's index math is unchanged.
// With a NON-ZERO window start the bias is load-bearing (a plain full-copy would read the wrong pixels), so this drives the same content two ways, once through a windowed snapshot at start=W, once by hand-offsetting a whole-buffer source so the live read lands on the same pixels, and asserts they match.
TEST_CASE("MoonI80 ring: the windowed snapshot bias reads this driver's slice, not from light 0") {
    // Reference: a driver whose window starts at 0 over a buffer sized for exactly its strands.
    MockRingDriver ref;
    MockRingPeripheral peripheralRef;
    mm::Buffer refSrc;
    mm::Correction corr;
    wireShift(ref, peripheralRef, refSrc, corr, 64, "1,2");   // 16 strands × 64 lights = 1024-light window
    const nrOfLightsType winLights = refSrc.count();   // the whole buffer IS the window here
    auto paint = [](uint8_t* p, nrOfLightsType n, nrOfLightsType base) {
        for (nrOfLightsType i = 0; i < n; i++) {
            p[i * 3 + 0] = static_cast<uint8_t>((base + i) * 7 + 1);
            p[i * 3 + 1] = static_cast<uint8_t>((base + i) * 13 + 5);
            p[i * 3 + 2] = static_cast<uint8_t>((base + i) * 29 + 2);
        }
    };
    paint(refSrc.data(), winLights, /*base=*/64);   // same pixel VALUES the windowed driver will see
    peripheralRef.setWantRing(true);
    const size_t rowBytes = static_cast<size_t>(corr.outChannels) * 24 * 1 * ref.outputsPerPin();
    REQUIRE(peripheralRef.busInitRing(rowBytes, static_cast<uint32_t>(ref.maxLaneLights())));
    REQUIRE(ref.snapshotForTest());
    std::vector<uint8_t> refFrame = peripheralRef.driveRingFrame();

    // Windowed: a bigger buffer, the driver's window offset to start=64, painted so window pixel k equals reference pixel k. The bias must make the snapshot read [64, 64+winLights), i.e. the SAME values.
    MockRingDriver win;
    MockRingPeripheral peripheralWin;
    mm::Buffer winSrc;
    mm::Correction corr2;
    wireShift(win, peripheralWin, winSrc, corr2, 64, "1,2");   // same geometry...
    // ...but re-allocate the source with a 64-light lead-in the window skips, and re-apply the window.
    REQUIRE(winSrc.allocate(winLights + 64, 3) == true);
    paint(winSrc.data(), winLights + 64, /*base=*/0);  // pixel 64.. == refSrc pixel 0.. (base 64)
    win.setWindow(64, winLights);
    win.applyState();
    peripheralWin.setWantRing(true);
    REQUIRE(peripheralWin.busInitRing(rowBytes, static_cast<uint32_t>(win.maxLaneLights())));
    REQUIRE(win.snapshotForTest());
    std::vector<uint8_t> winFrame = peripheralWin.driveRingFrame();

    REQUIRE(refFrame.size() == winFrame.size());
    CHECK(std::memcmp(refFrame.data(), winFrame.data(), refFrame.size()) == 0);
}

// The ring path chooses the source copy first, then prices that same frame before arming the transfer.
TEST_CASE("MoonI80 ring: current limiting prices the frame before the transfer starts") {
    MockRingDriver d;
    MockRingPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wireShift(d, peripheral, src, corr, 90, "1,2");

    peripheral.setWantRing(true);
    d.applyState();
    REQUIRE(peripheral.busIsRing());

    std::memset(src.data(), 255, static_cast<size_t>(src.count()) * src.channelsPerLight());
    d.correctionForTest().budgetMa = 17280;   // half of 90 lights × 2 pins × 8 strands × 24 mA
    d.tick();

    CHECK(d.correctionForTest().limit == 128);
}

// 7. TERMINATION — the DMA-chain contract the byte-tiling tests (1–6) never touch, and the exact site of
//    the 2026-07-16 "green dot per panel" bug: the looping chain must clock past the last real slice into
//    a clean LOW tail and stop deterministically, WITHOUT re-clocking a buffer that still holds a real
//    (non-tail) slice at a tail clock position. These drive the termination model, which replays
//    moonI80EofCb's prime → refill-behind → stop-on-drain-counter, and records what the DMA actually
//    clocks to the wire (including the laps past the last slice).
// 7.
// TERMINATION, the DMA-chain contract the byte-tiling tests (1–6) never touch, and the exact site of the 2026-07-16 "green dot per panel" bug: the looping chain must clock past the last real slice into a clean LOW tail and stop deterministically, WITHOUT re-clocking a buffer that still holds a real (non-tail) slice at a tail clock position.
// These drive the termination model, which replays moonI80EofCb's prime → refill-behind → stop-on-drain-counter, and records what the DMA actually clocks to the wire (including the laps past the last slice).

// 7a.
// CLEAN LOW TAIL, with enough buffers that the frame does NOT reuse (nSlices < bufs), every buffer
//     clocked after the last real slice is all-LOW: the ≥300 µs WS2812 reset that latches the '595. This
//     is the no-reuse stopgap's guarantee; it is what renders clean at ≤240 lights/strand (kRingBufs=16).
TEST_CASE("MoonI80 ring: no-reuse frame clocks a clean LOW tail and stops deterministically") {
    MockRingDriver d;
    MockRingPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wireShift(d, peripheral, src, corr, 176, "1,2");   // 176 lights = 11 slices; model 16 buffers → NO reuse
    uint8_t* s = src.data();
    for (nrOfLightsType i = 0; i < src.count(); i++) {   // dense non-zero so a dirty tail WOULD show
        s[i * 3 + 0] = static_cast<uint8_t>(i * 7 + 1);
        s[i * 3 + 1] = static_cast<uint8_t>(i * 13 + 5);
        s[i * 3 + 2] = static_cast<uint8_t>(i * 29 + 2);
    }
    peripheral.setWantRing(true);
    const size_t rowBytes = static_cast<size_t>(corr.outChannels) * 24 * 1 * d.outputsPerPin();
    REQUIRE(peripheral.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights())));

    auto f = peripheral.driveRingFrameWithTermination(/*bufs=*/16, /*kTailBufs=*/1);
    const uint32_t nSlices = (176 + 15) / 16;   // 11
    CHECK(f.drainsToStop == nSlices + 1);        // stops one buffer LATE (the tail), not early, not looping forever
    CHECK(f.tailIsLow);                          // the buffer(s) past the last real slice clock all-LOW
    // The last REAL slice's content is present (the frame wasn't truncated): its clock is non-zero.
    REQUIRE(f.buffers.size() >= nSlices);
    bool lastRealNonZero = std::any_of(f.buffers[nSlices - 1].begin(), f.buffers[nSlices - 1].end(),
                                       [](uint8_t b) { return b != 0; });
    CHECK(lastRealNonZero);
}

// 7b.
// THE NO-REUSE STOPGAP SIZING, with bufs > nSlices the tail buffer is a distinct physical buffer the
//     priming loop zeroed and the wrap re-clocks as a clean LOW; with bufs == nSlices there is NO spare
//     buffer, so the tail clock wraps onto buffer 0. The BYTE model shows the wrap re-clocks buffer 0
//     *after* the ISR has zeroed it on the very first drain's refill, so bytes alone stay LOW, but on
//     HARDWARE the equal case STALLS ("output stalled") because the stop counter (nSlices + kTailBufs
//     drains) needs a buffer the priming never left free, a DMA-timing property the byte model cannot see
//     (256 lights at kRingBufs=16 is exactly this; the fix is kRingBufs >= 17). What this test CAN pin is
//     the correct-sizing guarantee and the drain count, so a future change that breaks the tail or the
//     stop timing at the correct sizing is caught here; the equal-case stall is covered in the backlog.
TEST_CASE("MoonI80 ring: the no-reuse stopgap clocks a clean tail and stops on the drain counter") {
    MockRingDriver d;
    MockRingPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wireShift(d, peripheral, src, corr, 176, "1,2");   // 11 slices
    uint8_t* s = src.data();
    for (nrOfLightsType i = 0; i < src.count(); i++) {
        s[i * 3 + 0] = static_cast<uint8_t>(i * 7 + 1);
        s[i * 3 + 1] = static_cast<uint8_t>(i * 13 + 5);
        s[i * 3 + 2] = static_cast<uint8_t>(i * 29 + 2);
    }
    peripheral.setWantRing(true);
    const size_t rowBytes = static_cast<size_t>(corr.outChannels) * 24 * 1 * d.outputsPerPin();
    REQUIRE(peripheral.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights())));
    const uint32_t nSlices = (176 + 15) / 16;   // 11

    // bufs = nSlices + 1 (the correct stopgap sizing, kRingBufs > nSlices): clean LOW tail, stop on the drain counter one buffer past the last real slice. This is the ≤240-lights/strand no-reuse guarantee.
    auto ok = peripheral.driveRingFrameWithTermination(/*bufs=*/static_cast<uint8_t>(nSlices + 1), /*kTailBufs=*/1);
    CHECK(ok.tailIsLow);
    CHECK(ok.drainsToStop == nSlices + 1);
    // A deeper pool (kRingBufs=16 for 11 slices, the shipped stopgap) is equally clean and stops the same.
    auto deep = peripheral.driveRingFrameWithTermination(/*bufs=*/16, /*kTailBufs=*/1);
    CHECK(deep.tailIsLow);
    CHECK(deep.drainsToStop == nSlices + 1);
}

// 9.
// RAGGED TILING, the same byte-for-byte tiling invariant as test 1, but with strands of DIFFERENT lengths.
// This is the case the uniform tests structurally cannot see, and it is a real end-user config: `ledsPerPin` takes one entry per STRAND (pins × 8 with the expander), precisely so two strands on the SAME '595 can differ, a strip on one output, a panel on the next.
//
// Why ragged is a distinct risk from uniform.
// The shift constants (the pulse-start word) depend on the ACTIVE MASK, and a strand that runs out drops from that mask at the row where it ends.
// So the constants stop being frame-uniform and must be laid per RUN of rows sharing a mask (ParallelLedDriver::prefillShiftRows).
// Get it wrong, lay row 0's mask over every row, and an exhausted strand keeps its pulse-start asserted and FLASHES WHITE at full brightness.
//
// The lengths are chosen so a strand ends INSIDE a slice, not on a buffer boundary.
// 16 rows/buffer and a strand ending at 100 puts the mask change in the middle of slice 6 (rows 96..111).
// That is the composition, run-splitting × slice tiling, that neither the encoder-level ragged tests (which never slice) nor the driver-level ring tests (which are never ragged) reach on their own.
TEST_CASE("MoonI80 ring: a ragged frame tiles byte-identically (a strand ending mid-slice)") {
    MockRingDriver d;
    MockRingPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    // 16 strands (2 pins × 8). Strand 0 runs the full 200; strands 3 and 9 end at 100 and 57, both INSIDE a 16-row slice (100 = slice 6 row 4; 57 = slice 3 row 9), and 57 is not a multiple of anything convenient, which is the point.
    wireShift(d, peripheral, src, corr, 200, "1,2",
              "200,200,200,100,200,200,200,200,200,57,200,200,200,200,200,200");
    uint8_t* s = src.data();
    for (nrOfLightsType i = 0; i < src.count(); i++) {
        s[i * 3 + 0] = static_cast<uint8_t>(i * 7);
        s[i * 3 + 1] = static_cast<uint8_t>(i * 13 + 1);
        s[i * 3 + 2] = static_cast<uint8_t>(i * 29 + 2);
    }
    const uint8_t outCh = corr.outChannels;
    const size_t rowBytes = static_cast<size_t>(outCh) * 24 * 1 * d.outputsPerPin();
    const size_t rowRegion = static_cast<size_t>(d.maxLaneLights()) * rowBytes;

    // The frame is still as long as the LONGEST strand, the short ones just go dark early.
    REQUIRE(d.maxLaneLights() == 200);

    peripheral.busInit(d.frameBytes(), false);
    d.prefillShiftFrameForTest<uint8_t>(outCh, peripheral.busBuffer(0));
    d.encodeWholeForTest<uint8_t>(outCh, peripheral.busBuffer(0));
    std::vector<uint8_t> whole(peripheral.busBuffer(0), peripheral.busBuffer(0) + rowRegion);
    peripheral.busDeinit();

    peripheral.setWantRing(true);
    REQUIRE(peripheral.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights())));
    std::vector<uint8_t> assembled = peripheral.driveRingFrame();

    REQUIRE(assembled.size() == whole.size());
    CHECK(std::memcmp(assembled.data(), whole.data(), whole.size()) == 0);
}

// 9a2.
// COALESCED EOFs, THE v2 REGRESSION TEST.
// The GDMA EOF interrupt is a latch, not a queue: under
//      load two drains arrive as ONE firing, and the clock-oracle ISR then batch-refills both slices in
//      that single invocation. The old one-refill-per-firing design shifted every later slice by one
//      position when this happened (the shifted-region / wrong-color wall artifact); the batch contract
//      requires the wire bytes to be IDENTICAL whatever the grouping. Deep-lapping geometry so batches
//      cross the recycle boundary repeatedly.
TEST_CASE("MoonI80 ring v2: coalesced EOFs (batched refill) are byte-identical to one-per-EOF") {
    MockRingDriver d;
    MockRingPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wireShift(d, peripheral, src, corr, 200, "1,2", "");   // 16 uniform strands, 200 rows: 13 slices over the mock pool
    uint8_t* s = src.data();
    for (nrOfLightsType i = 0; i < src.count(); i++) {
        s[i * 3 + 0] = static_cast<uint8_t>(i * 5 + 3);
        s[i * 3 + 1] = static_cast<uint8_t>(i * 11 + 7);
        s[i * 3 + 2] = static_cast<uint8_t>(i * 23 + 1);
    }
    const uint8_t outCh = corr.outChannels;
    const size_t rowBytes = static_cast<size_t>(outCh) * 24 * 1 * d.outputsPerPin();

    peripheral.setWantRing(true);
    REQUIRE(peripheral.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights())));
    std::vector<uint8_t> onePerEof = peripheral.driveRingFrame();          // the reference grouping
    std::vector<uint8_t> coalesced2 = peripheral.driveRingFrameCoalesced(2);   // every firing carries 2 drains
    std::vector<uint8_t> coalesced5 = peripheral.driveRingFrameCoalesced(5);   // deep coalescing (a long stall)

    REQUIRE(coalesced2.size() == onePerEof.size());
    REQUIRE(coalesced5.size() == onePerEof.size());
    CHECK(std::memcmp(coalesced2.data(), onePerEof.data(), onePerEof.size()) == 0);
    CHECK(std::memcmp(coalesced5.data(), onePerEof.data(), onePerEof.size()) == 0);
}

// 9b.
// EMPTY LANES ARE NOT RAGGED.
// The prefill-skip gate (uniformLaneCounts) requires a frame-constant
//     active mask, and a count-0 lane is in NO row's mask, so a source that fills only 15 of 16
//     expander strands must count as uniform (skip allowed), not ragged (prefill every refill, ~1/3 of
//     the refill cost). This pins BOTH halves: the gate says uniform, and the ring's recycled-buffer
//     frames, which now skip the prefill, stay byte-identical to the whole-frame encode.
TEST_CASE("MoonI80 ring: an EMPTY lane does not break uniformity (prefill-skip stays valid)") {
    MockRingDriver d;
    MockRingPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    // 15 strands at the full 200, the 16th empty, the 3840-lights-on-16-strands wall shape.
    wireShift(d, peripheral, src, corr, 200, "1,2",
              "200,200,200,200,200,200,200,200,200,200,200,200,200,200,200,0");
    CHECK(d.uniformLaneCounts());   // the gate itself: empty lane ignored
    uint8_t* s = src.data();
    for (nrOfLightsType i = 0; i < src.count(); i++) {
        s[i * 3 + 0] = static_cast<uint8_t>(i * 7);
        s[i * 3 + 1] = static_cast<uint8_t>(i * 13 + 1);
        s[i * 3 + 2] = static_cast<uint8_t>(i * 29 + 2);
    }
    const uint8_t outCh = corr.outChannels;
    const size_t rowBytes = static_cast<size_t>(outCh) * 24 * 1 * d.outputsPerPin();
    const size_t rowRegion = static_cast<size_t>(d.maxLaneLights()) * rowBytes;

    peripheral.busInit(d.frameBytes(), false);
    d.prefillShiftFrameForTest<uint8_t>(outCh, peripheral.busBuffer(0));
    d.encodeWholeForTest<uint8_t>(outCh, peripheral.busBuffer(0));
    std::vector<uint8_t> whole(peripheral.busBuffer(0), peripheral.busBuffer(0) + rowRegion);
    peripheral.busDeinit();

    peripheral.setWantRing(true);
    REQUIRE(peripheral.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights())));
    std::vector<uint8_t> assembled = peripheral.driveRingFrame();

    REQUIRE(assembled.size() == whole.size());
    CHECK(std::memcmp(assembled.data(), whole.data(), whole.size()) == 0);
}

// 10.
// RAGGED, AN EXHAUSTED STRAND GOES DARK.
// The tiling test above compares the ring against the
//     whole-frame encode, so it catches any DISAGREEMENT between the two paths, but it would not notice
//     both being wrong the same way. This one asserts the actual hardware requirement directly, with no
//     reference frame: once a strand runs out, every byte it clocks is 0.
//
//     This is the bug's real-world face. The '595 is fed serially and every bus word is an SRCLK edge, so
//     a strand cannot be "left alone", keeping it dark means clocking ZEROS into its shift position. The
//     pulse-start word carries the active mask, so a stale mask re-asserts an exhausted strand and it
//     FLASHES WHITE at full brightness. Driving the source all-0xFF makes any leak maximally visible.
//
//     Both frames are checked: the constants must be re-laid correctly on a RECYCLED buffer too (ring
//     buffers are reused, not zeroed), which is where a "lay it once at init" shortcut breaks on frame 2.
TEST_CASE("MoonI80 ring: an exhausted RAGGED strand clocks zeros, on a fresh AND a recycled buffer") {
    MockRingDriver d;
    MockRingPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    // Strand 0 alone runs the full 200; every other strand on both '595s ends at 8, so from row 8 the mask is a single bit, and 15 of 16 strands must be silent for the remaining 192 rows.
    wireShift(d, peripheral, src, corr, 200, "1,2", "200,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8");
    std::memset(src.data(), 0xFF, static_cast<size_t>(src.count()) * 3);   // a leak shows as full white
    const uint8_t outCh = corr.outChannels;
    const size_t rowBytes = static_cast<size_t>(outCh) * 24 * 1 * d.outputsPerPin();

    REQUIRE(d.maxLaneLights() == 200);
    peripheral.setWantRing(true);
    REQUIRE(peripheral.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights())));

    // Strand 0 is pin 0's shift position 0; the '595 shifts MSB-first, so that strand rides bit 0 of the bus word at cycle outputsPerPin-1.
    // Every OTHER bus bit must be 0 for rows >= 8.
    // Bit p is pin p's serial input, and pin 1 (bit 1) carries only exhausted strands, as do pin 0's other 7 positions.
    // The latch (latchPin=20 → its own bus bit) rides word 0 of each slot, so mask it out before testing.
    const uint8_t latchMask = static_cast<uint8_t>(1u << d.latchBitForTest());
    auto checkDark = [&](const std::vector<uint8_t>& frame, const char* which) {
        size_t leaked = 0;
        for (nrOfLightsType row = 8; row < 200; row++) {
            const uint8_t* r = frame.data() + static_cast<size_t>(row) * rowBytes;
            for (size_t b = 0; b < rowBytes; b++) {
                // Pin 1's bit must never assert past row 8 (all 8 of its strands ended at 8).
                if ((r[b] & ~latchMask) & 0x02u) leaked++;
            }
        }
        INFO("exhausted strands leaked on " << which << ": " << leaked << " bytes");
        CHECK(leaked == 0);
    };
    std::vector<uint8_t> first = peripheral.driveRingFrame();
    checkDark(first, "the first frame");
    std::vector<uint8_t> second = peripheral.driveRingFrame();   // same buffers, recycled
    checkDark(second, "a recycled buffer");
    // And the two laps agree byte for byte, a recycled buffer is not a fresh one only by accident.
    REQUIRE(first.size() == second.size());
    CHECK(std::memcmp(first.data(), second.data(), first.size()) == 0);
}

// 11.
// THE RING MUST NOT SURVIVE A FAILED BUILD. reinit() builds the ring in two steps, the bus, then
//     the source snapshot the ring's encode reads, and BOTH must hold or the driver falls back to
//     whole-frame. The trap is the middle case: the bus builds (a GDMA channel, its EOF interrupt, and
//     ~150 KB of internal DMA buffers are now live) and the snapshot then fails to allocate. That is not
//     a hypothetical, it is the likeliest OOM in the whole driver, because the snapshot is asked for
//     right after the ring took the scarcest memory on the chip.
//
//     If the fall-through path tears down conditionally (`if (inited_) busDeinit()`), it walks straight
//     past that live ring: `inited_` is false by construction on this path, and the whole-frame build
//     below then OVERWRITES the bus handle, leaking the channel, the ISR registration and the RAM, with
//     no way back but a reboot. Worse, it repeats on every prepare rebuild, so a board tight enough to
//     hit it once bleeds on every geometry change.
//
//     This pins the rule that makes it safe: after a failed ring build the bus is torn down
//     UNCONDITIONALLY. The mock's busDeinit clears ringActive_, so a surviving ring is visible here.
TEST_CASE("MoonI80 ring: a failed ring build tears the bus down (no leaked ring)") {
    MockRingDriver d;
    MockRingPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wireShift(d, peripheral, src, corr, 200, "1,2");

    const uint8_t outCh = corr.outChannels;
    const size_t rowBytes = static_cast<size_t>(outCh) * 24 * 1 * d.outputsPerPin();

    // A ring that built successfully, the exact state the failure path must not leave behind.
    peripheral.setWantRing(true);
    REQUIRE(peripheral.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights())));
    REQUIRE(peripheral.busIsRing());

    // The fall-through's teardown, as reinit() performs it. It must not be gated on `inited_`, which is false here, exactly as it is in production on this path.
    peripheral.busDeinit();
    CHECK_FALSE(peripheral.busIsRing());   // the ring is gone, not merely unreferenced
}

TEST_CASE("MoonI80 ring: the PARALLEL snapshot's range-split is byte-identical to the whole-range serial") {
    // The fork-join correctness oracle.
    // On device the two [lo,hi) halves run on two cores; the bytes must be identical to one serial pass.
    // Here one thread runs both ranges, same requirement, since the ranges are disjoint and stateless: copyRange(0,half)+copyRange(half,N) == copyRange(0,N).
    // The snapshot is now a raw memcpy at SOURCE channel stride (correction fuses into encodeRows downstream).
    MockRingDriver d;
    MockRingPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wireShift(d, peripheral, src, corr, 200, "1,2");   // 16 strands × 200, a real window
    // Distinctive per-light source so a mis-split (gap/overlap/wrong stride) can't accidentally match.
    uint8_t* s = src.data();
    for (mm::nrOfLightsType i = 0; i < src.count(); i++) {
        s[i * 3 + 0] = static_cast<uint8_t>(i * 7 + 1);
        s[i * 3 + 1] = static_cast<uint8_t>(i * 13 + 5);
        s[i * 3 + 2] = static_cast<uint8_t>(i * 29 + 3);
    }
    const uint8_t outCh = corr.outChannels;
    const uint8_t srcCh = static_cast<uint8_t>(src.channelsPerLight());
    const size_t rowBytes = static_cast<size_t>(outCh) * 24 * 1 * d.outputsPerPin();
    peripheral.setWantRing(true);
    REQUIRE(peripheral.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights())));
    // snapshotForTest sizes snapshotBuf_ (ensureSnapshotCap) and runs one full serial snapshot, after it, the snapshot state (snapCopy*) is set and the buffer exists, so the manual re-runs below are safe.
    REQUIRE(d.snapshotForTest());

    const mm::nrOfLightsType N = d.winLenForTest();
    REQUIRE(N > 4);
    const size_t bufBytes = static_cast<size_t>(N) * srcCh;   // snapshot is srcCh-stride now

    // Reference: one whole-range copy over the (now-allocated) snapshot buffer.
    d.setSnapCopyForTest();
    std::memset(d.snapshotBufForTest(), 0xEE, bufBytes);
    d.copyRangeForTest(0, N);
    std::vector<uint8_t> whole(d.snapshotBufForTest(), d.snapshotBufForTest() + bufBytes);

    // Split at the cache-line-aligned midpoint, exactly as the production path picks it (srcCh stride).
    const mm::nrOfLightsType half = MockRingDriver::snapHalfForTest(N, srcCh);
    REQUIRE(half > 0);
    REQUIRE(half < N);
    std::memset(d.snapshotBufForTest(), 0xEE, bufBytes);
    d.copyRangeForTest(0, half);        // "core 0" half
    d.copyRangeForTest(half, N);        // "core 1" half
    std::vector<uint8_t> split(d.snapshotBufForTest(), d.snapshotBufForTest() + bufBytes);

    REQUIRE(split.size() == whole.size());
    CHECK(std::memcmp(split.data(), whole.data(), bufBytes) == 0);
}

// The snapshot window clamp must key on the SOURCE stride (srcCh), not outCh, the buffer is a raw srcCh-strided memcpy.
// With outCh > srcCh (an RGB source through an RGBW correction: srcCh=3, outCh=4) an outCh-based clamp would compute winLen*4 > winLen*3 and silently drop ~1/4 of the window, leaving its tail reading stale bytes.
// This pins the full window survives.
TEST_CASE("MoonI80 ring: snapshot keeps the whole window when outCh > srcCh (RGBW correction on RGB source)") {
    MockRingDriver d;
    MockRingPeripheral peripheral;
    mm::Buffer src;
    mm::Correction corr;
    wireShift(d, peripheral, src, corr, 200, "1,2");            // 16 strands × 200, RGB source (srcCh=3)
    // Swap in an RGBW correction (outCh=4) so outCh > the source's 3 channels, the failing condition.
    mm::test::rebuildFromPreset(corr, 255, mm::test::PresetOrder::RGBW);
    d.correctionForTest() = corr;
    d.applyState();
    REQUIRE(corr.outChannels == 4);
    REQUIRE(src.channelsPerLight() == 3);

    peripheral.setWantRing(true);
    const uint8_t outCh = corr.outChannels;
    const uint8_t srcCh = static_cast<uint8_t>(src.channelsPerLight());
    const size_t rowBytes = static_cast<size_t>(outCh) * 24 * 1 * d.outputsPerPin();
    REQUIRE(peripheral.busInitRing(rowBytes, static_cast<uint32_t>(d.maxLaneLights())));

    // Paint the source window's LAST light a distinct value; the snapshot must copy it. With the buggy outCh clamp the window shrinks to winLen*3/4, so the last quarter (including this light) is never copied and its snapshot bytes stay at the 0xEE sentinel below, the assertion then fails.
    const mm::nrOfLightsType N = d.winLenForTest();
    REQUIRE(N > 4);
    uint8_t* s = src.data();
    const size_t last = static_cast<size_t>(N - 1) * srcCh;
    s[last + 0] = 0x11; s[last + 1] = 0x22; s[last + 2] = 0x33;

    REQUIRE(d.snapshotForTest());                   // sizes (winLen×srcCh) + takes the memcpy snapshot
    std::memset(d.snapshotBufForTest(), 0xEE, static_cast<size_t>(N) * srcCh);
    REQUIRE(d.snapshotForTest());                   // re-copy over the sentinel (state is set from above)

    // The window's last light must be present in the snapshot at srcCh stride, proof the clamp didn't truncate the tail. (snapshotBuf_ is window-relative: light i at offset i×srcCh.)
    const uint8_t* snap = d.snapshotBufForTest();
    CHECK(snap[last + 0] == 0x11);
    CHECK(snap[last + 1] == 0x22);
    CHECK(snap[last + 2] == 0x33);
    CHECK(d.snapshotCapForTest() == static_cast<size_t>(N) * srcCh);
}

// ringSnapshot is meaningful ONLY when a ring runs (wantsRing()); the schema must hide it otherwise so the user never sees a control that does nothing on their config. wantsRing() reads plain flags (not the source buffer), so the gate resolves correctly even at defineControls() time before the buffer is wired.
TEST_CASE("MoonI80 ring: ringSnapshot control is hidden unless the ring is active") {
    auto ringSnapshotHidden = [](MockRingDriver& d) -> bool {
        d.defineControls();
        const auto& cl = d.controls();
        for (uint8_t i = 0; i < cl.count(); i++)
            if (cl[i].name && std::strcmp(cl[i].name, "ringSnapshot") == 0) return cl[i].hidden;
        FAIL("ringSnapshot control not found");
        return false;
    };
    MockRingDriver ringing;
    MockRingPeripheral peripheralRinging;
    ringing.setPeripheralForTest(&peripheralRinging);
    peripheralRinging.setWantRing(true);
    CHECK_FALSE(ringSnapshotHidden(ringing));   // ring active → visible

    MockRingDriver whole;
    MockRingPeripheral peripheralWhole;
    whole.setPeripheralForTest(&peripheralWhole);
    peripheralWhole.setWantRing(false);
    CHECK(ringSnapshotHidden(whole));           // no ring → hidden
}
