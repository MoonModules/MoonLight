#pragma once
/// RTP packetisation of an H.264 stream, RFC 3550 for the header and RFC 6184 for the payload.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "core/util/H264Bitstream.h"

namespace mm::rtp {

/// The 90 kHz clock an RTP video timestamp counts in, the same one the encoder stamps frames with.
static constexpr uint32_t kClockHz = 90000;

/// Bytes of RTP header before the payload: version, marker, type, sequence, timestamp, SSRC.
static constexpr size_t kHeaderBytes = 12;

/// The payload type H.264 is assigned in the SDP this server answers with.
static constexpr uint8_t kPayloadType = 96;

/// What one datagram carries, chosen to clear a 1500-byte Ethernet MTU with IP and UDP headroom.
static constexpr size_t kMaxPacketBytes = 1400;

/// The bytes of Annex B start code at `p`: core reads the same bitstream the packetiser writes.
using mm::h264::startCodeLen;

/// The next NAL as an offset and a length past its start code, false once the buffer holds none.
inline bool nextNal(const uint8_t* buf, size_t len, size_t* at, size_t* nalLen) {
    size_t i = *at;
    while (i < len && startCodeLen(buf + i, len - i) == 0) i++;
    const size_t sc = (i < len) ? startCodeLen(buf + i, len - i) : 0;
    if (sc == 0) return false;
    const size_t start = i + sc;
    size_t end = start;
    while (end < len && startCodeLen(buf + end, len - end) == 0) end++;
    if (end <= start) return false;
    *at = end;
    *nalLen = end - start;
    // The caller reads from `buf + *at - *nalLen`, which is where this NAL's payload begins.
    return true;
}

/// Packetises one access unit into caller-owned datagram buffers, reporting each as it is written.
/// @technical
/// ## One NAL is one packet while it fits
/// A NAL under the MTU rides alone, its own header byte becoming the packet's first payload byte.
/// An oversized NAL splits into FU-A fragments carrying a two-byte indicator and header, the first setting S and the last E, every one repeating the original's type and reference bits.
/// A decoder reassembles from those alone, so a lost fragment costs one frame rather than the stream.
/// ## The marker bit ends an access unit
/// Set on the last packet of a frame and clear on every other, so a decoder knows a frame is whole without waiting for the next timestamp to change.
/// Every packet of one frame carries the SAME timestamp, a timestamp naming the moment a frame is displayed rather than sent.
class Packetiser {
public:
    /// `ssrc` identifies this stream for the session's life, and the sequence continues across frames.
    Packetiser(uint32_t ssrc, uint16_t firstSeq) : ssrc_(ssrc), seq_(firstSeq) {}

    /// Where each datagram is written: the callee sends it before the next call overwrites nothing, since the buffer is the caller's.
    using Sink = bool (*)(void* ctx, const uint8_t* packet, size_t len);

    /// Packetise one access unit, whose NALs are Annex B framed. Returns the packets written, or 0 where the sink refused: a refused packet abandons the frame rather than sending it torn.
    size_t writeAccessUnit(const uint8_t* annexB, size_t len, uint32_t pts90,
                           uint8_t* scratch, size_t scratchLen, Sink sink, void* ctx) {
        // +3, never +2: an FU-A fragment spends two bytes on its header, so room for exactly those carries no payload and the loop never advances.
        if (!annexB || !scratch || scratchLen < kHeaderBytes + 3 || !sink) return 0;
        // The LAST NAL carries the marker, so the walk finds the end before emitting anything.
        size_t at = 0, nalLen = 0, lastEnd = 0;
        while (nextNal(annexB, len, &at, &nalLen)) lastEnd = at;

        size_t packets = 0;
        at = 0;
        while (nextNal(annexB, len, &at, &nalLen)) {
            const uint8_t* nal = annexB + at - nalLen;
            const bool last = (at == lastEnd);
            const size_t n = writeNal(nal, nalLen, pts90, last, scratch, scratchLen, sink, ctx);
            if (n == 0) return 0;
            packets += n;
        }
        return packets;
    }

    /// The sequence number the next packet carries, which a session reports in its RTP-Info.
    uint16_t nextSequence() const { return seq_; }

private:
    /// One NAL, whole or fragmented: the packets written, or 0 where the sink refused.
    size_t writeNal(const uint8_t* nal, size_t len, uint32_t pts90, bool lastOfFrame,
                    uint8_t* scratch, size_t scratchLen, Sink sink, void* ctx) {
        const size_t room = (scratchLen < kMaxPacketBytes ? scratchLen : kMaxPacketBytes) - kHeaderBytes;
        if (len == 0) return 0;

        if (len <= room) {
            writeHeader(scratch, pts90, lastOfFrame);
            std::memcpy(scratch + kHeaderBytes, nal, len);
            return sink(ctx, scratch, kHeaderBytes + len) ? 1 : 0;
        }

        // FU-A: the original header's type moves into the fragment header, its top three bits (the forbidden-zero and reference bits) staying with the indicator.
        const uint8_t nri  = static_cast<uint8_t>(nal[0] & 0xE0);
        const uint8_t type = static_cast<uint8_t>(nal[0] & 0x1F);
        size_t off = 1;                       // the original header byte is replaced, never sent
        size_t packets = 0;
        while (off < len) {
            const size_t take = (len - off < room - 2) ? len - off : room - 2;
            const bool first = (off == 1);
            const bool final = (off + take >= len);
            writeHeader(scratch, pts90, lastOfFrame && final);
            scratch[kHeaderBytes]     = static_cast<uint8_t>(nri | 28);   // 28 = FU-A
            scratch[kHeaderBytes + 1] = static_cast<uint8_t>((first ? 0x80 : 0) |
                                                             (final ? 0x40 : 0) | type);
            std::memcpy(scratch + kHeaderBytes + 2, nal + off, take);
            if (!sink(ctx, scratch, kHeaderBytes + 2 + take)) return 0;
            packets++;
            off += take;
        }
        return packets;
    }

    /// The twelve fixed bytes: version 2, no padding or extension, one CSRC-free source.
    void writeHeader(uint8_t* p, uint32_t pts90, bool marker) {
        p[0] = 0x80;
        p[1] = static_cast<uint8_t>((marker ? 0x80 : 0) | kPayloadType);
        p[2] = static_cast<uint8_t>(seq_ >> 8);
        p[3] = static_cast<uint8_t>(seq_ & 0xFF);
        seq_++;
        p[4] = static_cast<uint8_t>(pts90 >> 24);
        p[5] = static_cast<uint8_t>(pts90 >> 16);
        p[6] = static_cast<uint8_t>(pts90 >> 8);
        p[7] = static_cast<uint8_t>(pts90);
        p[8]  = static_cast<uint8_t>(ssrc_ >> 24);
        p[9]  = static_cast<uint8_t>(ssrc_ >> 16);
        p[10] = static_cast<uint8_t>(ssrc_ >> 8);
        p[11] = static_cast<uint8_t>(ssrc_);
    }

    uint32_t ssrc_;
    uint16_t seq_;
};

}  // namespace mm::rtp
