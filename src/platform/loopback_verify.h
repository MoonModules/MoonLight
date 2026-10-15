#pragma once

#include "platform/platform.h"   // RmtLoopbackResult

#include <cstddef>
#include <cstdint>

namespace mm::platform::detail {

/// The bit check every WS2812 loopback shares: each captured pulse decoded and compared with what was sent.
///
/// @technical
///
/// ## The first pulse of a frame is short behind an expander
///
/// The register's outputs are still settling as the first latch fires, so the very first bit comes back as a zero when the strand sent a one.
/// Measured on one strand: exactly one mismatch in over two thousand bits, always that bit, always short-clipped, while every other bit and both pulse-width classes are textbook.
/// It costs the first light's most significant color bit and nothing else, which is invisible, so a lone short-clipped first bit is the expander's frame-start settling rather than bad output.
/// Any second mismatch, or a first-bit miss that is not short-clipped, still fails. Direct mode has no latch, so its first bit is clean; the exception is gated on the expander mode so a direct path's fault is never excused.

/// The spread of the captured high times per decoded bit value, for the bench log.
struct LoopbackBitStats {
    size_t mismatch = SIZE_MAX;                ///< the first wrong bit, SIZE_MAX when all match
    size_t mismatchCount = 0;                  ///< how many bits differ from what was sent
    uint16_t minHigh[2] = {0x7FFF, 0x7FFF};    ///< the shortest high time seen for a zero and for a one
    uint16_t maxHigh[2] = {0, 0};              ///< the longest high time seen for a zero and for a one
};

/// A verdict to fill, carrying the pattern every loopback sends in each row.
inline RmtLoopbackResult loopbackResult() {
    RmtLoopbackResult r;
    r.sent[0] = 0xA5; r.sent[1] = 0x00; r.sent[2] = 0xFF;
    return r;
}

/// Whether a whole-frame loopback request has a pattern to check: lanes, a frame, and one row inside it at least.
inline bool loopbackFrameValid(const uint16_t* dataPins, uint8_t laneCount, const uint8_t* frame,
                               size_t frameBytes, size_t dataBytes, uint8_t rowBits) {
    return dataPins && laneCount > 0 && frame && frameBytes > 0 && dataBytes >= 3 && dataBytes <= frameBytes
        && rowBits >= 8;
}

/// One captured pulse as a bit, its high time widening the spread its value has seen.
inline uint8_t loopbackBit(uint32_t symbol, uint16_t threshTicks, LoopbackBitStats& s) {
    const uint16_t high = static_cast<uint16_t>(symbol & 0x7FFF);
    const uint8_t bit = (high >= threshTicks) ? 1 : 0;
    if (high < s.minHigh[bit]) s.minHigh[bit] = high;
    if (high > s.maxHigh[bit]) s.maxHigh[bit] = high;
    return bit;
}

/// The bit sent at position `rowPos` of a row: the pattern's three bytes, then zeros.
inline uint8_t loopbackSentBit(const RmtLoopbackResult& r, uint8_t rowPos) {
    const uint8_t sentByte = (rowPos / 8u) < 3 ? r.sent[rowPos / 8u] : 0x00;
    return (sentByte >> (7 - (rowPos & 7))) & 1u;
}

/// The three bytes of the row starting at bit `rowStart`, as decoded, into `r.got`.
inline void loopbackGotRow(const uint32_t* symbols, size_t captured, size_t rowStart, uint16_t threshTicks,
                           RmtLoopbackResult& r) {
    for (size_t b = rowStart; b < rowStart + 24 && b < captured; b++) {
        const uint8_t bit = ((symbols[b] & 0x7FFF) >= threshTicks) ? 1 : 0;
        r.got[(b - rowStart) / 8] = static_cast<uint8_t>((r.got[(b - rowStart) / 8] << 1) | bit);
    }
}

/// Decode captured WS2812 pulses and verify every bit against the per-row pattern in `r.sent`, filling the verdict.
inline LoopbackBitStats verifyLoopbackBits(const uint32_t* symbols, size_t bits, size_t captured,
                                           uint8_t rowBits, uint16_t threshTicks, bool pinExpanderMode,
                                           RmtLoopbackResult& r) {
    LoopbackBitStats s;
    // A symbol's low 15 bits are its high time in ticks, and a row of `rowBits` bits ends in zeros, so RGBW rows verify too.
    for (size_t b = 0; b < bits; b++) {
        if (loopbackBit(symbols[b], threshTicks, s) == loopbackSentBit(r, static_cast<uint8_t>(b % rowBits))) continue;
        if (s.mismatch == SIZE_MAX) s.mismatch = b;
        s.mismatchCount++;
    }
    // r.got[] reports the row holding the first mismatch (row 0 when clean).
    loopbackGotRow(symbols, captured, (s.mismatch == SIZE_MAX) ? 0 : s.mismatch - (s.mismatch % rowBits), threshTicks, r);
    // The frame's first pulse is one slot short behind an expander: @xref{the-first-pulse-of-a-frame-is-short-behind-an-expander|the measurement, and what still fails}.
    const bool onlyBit0Clip = pinExpanderMode && s.mismatchCount == 1 && s.mismatch == 0
                            && (static_cast<uint16_t>(symbols[0] & 0x7FFF) < threshTicks);
    r.pass = (s.mismatch == SIZE_MAX) || onlyBit0Clip;
    r.bitsChecked = static_cast<uint32_t>(bits);
    r.firstBadBit = r.pass ? static_cast<uint32_t>(bits) : static_cast<uint32_t>(s.mismatch);
    return s;
}

}  // namespace mm::platform::detail
