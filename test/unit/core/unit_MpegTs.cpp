/// @module MpegTs
/// @also HlsDriver

/// The MPEG-TS packaging the ESP32-P4 does for itself, where desktop hands the whole job to ffmpeg. These tests are why the muxer is pure logic in a header: the packet structure a player has to accept is pinned here, on the host, rather than only by watching a TV.

#include "doctest.h"
#include "core/util/MpegTs.h"

#include <cstring>   // std::memcmp: declared here, not borrowed from MpegTs.h
#include <vector>

namespace {

// A frame's worth of Annex-B bytes. Content is irrelevant to muxing (the muxer never parses the NALs), so a recognizable filler proves payload placement.
std::vector<uint8_t> fakeAccessUnit(size_t len) {
    std::vector<uint8_t> au(len);
    for (size_t i = 0; i < len; i++) au[i] = static_cast<uint8_t>(i & 0xFF);
    return au;
}

// Every packet's PID, as a player's demuxer would read it.
uint16_t pidOf(const uint8_t* pkt) {
    return static_cast<uint16_t>(((pkt[1] & 0x1F) << 8) | pkt[2]);
}

}  // namespace

// The invariant a demuxer relies on above all others: the stream is a whole number of 188-byte packets, each starting with the sync byte. Everything else is read relative to that.
TEST_CASE("Every muxed packet is 188 bytes and starts with the sync byte") {
    std::vector<uint8_t> out(64 * 1024);
    mm::ts::Continuity wcc;
    mm::ts::Writer w(out.data(), out.size(), wcc);
    w.writeTables();
    const auto au = fakeAccessUnit(1000);
    w.writeAccessUnit(au.data(), au.size(), 0, true);

    REQUIRE(w.size() % mm::ts::kPacketSize == 0);
    REQUIRE(w.size() > 0);
    CHECK_FALSE(w.overflowed());
    for (size_t off = 0; off < w.size(); off += mm::ts::kPacketSize)
        CHECK(out[off] == 0x47);
}

// A player joining mid-stream can only decode a segment that tells it what the segment contains, so each one opens with the program tables before any video.
TEST_CASE("A segment opens with the program tables, then video") {
    std::vector<uint8_t> out(64 * 1024);
    mm::ts::Continuity wcc;
    mm::ts::Writer w(out.data(), out.size(), wcc);
    w.writeTables();
    const auto au = fakeAccessUnit(500);
    w.writeAccessUnit(au.data(), au.size(), 0, true);

    CHECK(pidOf(out.data()) == mm::ts::kPidPat);
    CHECK(pidOf(&out[mm::ts::kPacketSize]) == mm::ts::kPidPmt);
    CHECK(pidOf(&out[2 * mm::ts::kPacketSize]) == mm::ts::kPidVideo);
}

// A player follows the PAT to the PMT to the elementary stream, so the packets merely existing is not enough: a PAT at PID 0 instead of the PMT left VLC with no video track while ffmpeg, which probes regardless, still played. This walks the chain.
TEST_CASE("The PAT points at the PMT, and the PMT at the video stream") {
    std::vector<uint8_t> out(64 * 1024);
    mm::ts::Continuity wcc;
    mm::ts::Writer w(out.data(), out.size(), wcc);
    w.writeTables();
    const auto au = fakeAccessUnit(500);
    w.writeAccessUnit(au.data(), au.size(), 0, true);

    // Read a PSI section out of its packet: skip the header, any adaptation field, the pointer.
    auto section = [](const uint8_t* pkt) {
        size_t o = 4;
        if (pkt[3] & 0x20) o += 1 + pkt[4];
        o += 1 + pkt[o];
        return pkt + o;
    };

    const uint8_t* pat = nullptr;
    const uint8_t* pmt = nullptr;
    for (size_t off = 0; off < w.size(); off += mm::ts::kPacketSize) {
        const uint8_t* pkt = &out[off];
        if (pidOf(pkt) == mm::ts::kPidPat && !pat) pat = section(pkt);
        if (pidOf(pkt) == mm::ts::kPidPmt && !pmt) pmt = section(pkt);
    }
    REQUIRE(pat != nullptr);
    REQUIRE(pmt != nullptr);

    // The PAT's first program entry names the PID its map lives on (13 bits; top 3 reserved).
    const uint16_t mapPid = static_cast<uint16_t>(((pat[10] & 0x1F) << 8) | pat[11]);
    CHECK(mapPid == mm::ts::kPidPmt);

    // The PMT names the elementary stream: H.264 (type 0x1B) on the video PID.
    const uint16_t progInfoLen = static_cast<uint16_t>(((pmt[10] & 0x0F) << 8) | pmt[11]);
    const uint8_t* es = pmt + 12 + progInfoLen;
    CHECK(es[0] == 0x1B);
    CHECK(static_cast<uint16_t>(((es[1] & 0x1F) << 8) | es[2]) == mm::ts::kPidVideo);
}

// The continuity counter is how a player detects a dropped packet: it must advance by one per packet on each PID independently, wrapping at 4 bits.
TEST_CASE("Continuity counters advance per PID") {
    std::vector<uint8_t> out(256 * 1024);
    mm::ts::Continuity wcc;
    mm::ts::Writer w(out.data(), out.size(), wcc);
    w.writeTables();
    // Enough frames that the video counter must wrap past 15 at least once.
    const auto au = fakeAccessUnit(400);
    for (int i = 0; i < 25; i++) w.writeAccessUnit(au.data(), au.size(), i * 3000u, i == 0);

    int expected = -1;
    for (size_t off = 0; off < w.size(); off += mm::ts::kPacketSize) {
        const uint8_t* pkt = &out[off];
        if (pidOf(pkt) != mm::ts::kPidVideo) continue;
        const int cc = pkt[3] & 0x0F;
        if (expected >= 0) CHECK(cc == expected);
        expected = (cc + 1) & 0x0F;
    }
    CHECK(expected > 0);   // the loop actually saw video packets
}

// Only the first packet of a frame may claim a payload start; a decoder uses the flag to find frame boundaries, so a spurious one mid-frame corrupts the split.
TEST_CASE("Only a frame's first packet flags the payload start") {
    std::vector<uint8_t> out(64 * 1024);
    mm::ts::Continuity wcc;
    mm::ts::Writer w(out.data(), out.size(), wcc);
    const auto au = fakeAccessUnit(2000);        // spans several packets
    w.writeAccessUnit(au.data(), au.size(), 0, false);

    int starts = 0, videoPackets = 0;
    for (size_t off = 0; off < w.size(); off += mm::ts::kPacketSize) {
        if (pidOf(&out[off]) != mm::ts::kPidVideo) continue;
        videoPackets++;
        if (out[off + 1] & 0x40) starts++;
    }
    CHECK(videoPackets > 1);
    CHECK(starts == 1);
}

// A player needs a clock reference where it can start decoding, and that is the keyframe. A non-key frame carries none.
TEST_CASE("A keyframe carries the clock reference, a delta frame does not") {
    auto hasPcr = [](const std::vector<uint8_t>& buf, size_t len) {
        for (size_t off = 0; off < len; off += mm::ts::kPacketSize) {
            const uint8_t* pkt = &buf[off];
            if (pidOf(pkt) != mm::ts::kPidVideo) continue;
            if (!(pkt[3] & 0x20)) continue;              // no adaptation field
            if (pkt[4] > 0 && (pkt[5] & 0x10)) return true;
        }
        return false;
    };
    const auto au = fakeAccessUnit(600);

    std::vector<uint8_t> key(64 * 1024);
    mm::ts::Continuity kwcc;
    mm::ts::Writer kw(key.data(), key.size(), kwcc);
    kw.writeAccessUnit(au.data(), au.size(), 0, true);
    CHECK(hasPcr(key, kw.size()));

    std::vector<uint8_t> delta(64 * 1024);
    mm::ts::Continuity dwcc;
    mm::ts::Writer dw(delta.data(), delta.size(), dwcc);
    dw.writeAccessUnit(au.data(), au.size(), 3000, false);
    CHECK_FALSE(hasPcr(delta, dw.size()));
}

// Muxing many frames must not disturb its surroundings: the muxer builds each PES header in a fixed local buffer and once overran it by a byte, invisible in the output. A guard around the output gives the sanitizer build a case that fails loudly.
TEST_CASE("Muxing many frames stays inside its buffers") {
    const uint8_t guard = 0x5A;
    std::vector<uint8_t> arena(512 * 1024, guard);
    const size_t usable = arena.size() - 128;

    mm::ts::Continuity wcc;
    mm::ts::Writer w(arena.data(), usable, wcc);
    w.writeTables();
    const auto au = fakeAccessUnit(1200);
    for (int i = 0; i < 40; i++) w.writeAccessUnit(au.data(), au.size(), i * 3000u, i % 30 == 0);

    CHECK_FALSE(w.overflowed());
    size_t clobbered = 0;
    for (size_t i = w.size(); i < arena.size(); i++) if (arena[i] != guard) clobbered++;
    CHECK(clobbered == 0);
}

// A frame larger than the buffer must be reported, never written past the end: on the P4 the destination is a fixed PSRAM slot, so a silent overrun would corrupt whatever follows it.
TEST_CASE("A frame too large for the buffer reports overflow instead of overrunning") {
    std::vector<uint8_t> out(3 * mm::ts::kPacketSize);
    const uint8_t guard = 0xAB;
    std::vector<uint8_t> arena(out.size() + 64, guard);

    mm::ts::Continuity wcc;
    mm::ts::Writer w(arena.data(), out.size(), wcc);
    const auto au = fakeAccessUnit(10000);       // far more than 3 packets can hold
    w.writeAccessUnit(au.data(), au.size(), 0, true);

    CHECK(w.overflowed());
    CHECK(w.size() <= out.size());
    size_t past = 0;
    for (size_t i = out.size(); i < arena.size(); i++) if (arena[i] != guard) past++;
    CHECK(past == 0);
}

// The whole frame reaches the stream: what a decoder reassembles from the payloads must be the bytes we handed in, in order, with nothing dropped at a packet seam.
TEST_CASE("The frame's bytes survive the split across packets") {
    std::vector<uint8_t> out(64 * 1024);
    mm::ts::Continuity wcc;
    mm::ts::Writer w(out.data(), out.size(), wcc);
    const auto au = fakeAccessUnit(1500);
    w.writeAccessUnit(au.data(), au.size(), 0, false);

    // Reassemble payloads the way a demuxer does: skip the header and any adaptation field.
    std::vector<uint8_t> got;
    for (size_t off = 0; off < w.size(); off += mm::ts::kPacketSize) {
        const uint8_t* pkt = &out[off];
        if (pidOf(pkt) != mm::ts::kPidVideo) continue;
        size_t o = 4;
        if (pkt[3] & 0x20) o += 1 + pkt[4];
        got.insert(got.end(), pkt + o, pkt + mm::ts::kPacketSize);
    }

    // The reassembled run holds the PES header, then the frame; find the frame inside it.
    REQUIRE(got.size() >= au.size());
    bool found = false;
    for (size_t start = 0; start + au.size() <= got.size() && !found; start++)
        found = std::memcmp(got.data() + start, au.data(), au.size()) == 0;
    CHECK(found);
}

// A Writer is built per FRAME, so the continuity counters cannot live inside it: MPEG-TS requires one unbroken sequence per PID across all frames and segments; inside the Writer each frame restarted at zero and ffmpeg reported "Packet corrupt".
TEST_CASE("Continuity survives across the separate writers a stream is muxed with") {
    std::vector<uint8_t> out(256 * 1024);
    mm::ts::Continuity cc;
    const auto au = fakeAccessUnit(600);

    // Mux many frames the way the encoder does: a fresh Writer each time, one shared counter.
    size_t used = 0;
    for (int frame = 0; frame < 20; frame++) {
        mm::ts::Writer w(out.data() + used, out.size() - used, cc);
        if (frame == 0) w.writeTables();
        w.writeAccessUnit(au.data(), au.size(), static_cast<uint32_t>(frame) * 3000u, frame == 0);
        REQUIRE_FALSE(w.overflowed());
        used += w.size();
    }

    int expected = -1, checked = 0;
    for (size_t off = 0; off < used; off += mm::ts::kPacketSize) {
        const uint8_t* pkt = &out[off];
        if (pidOf(pkt) != mm::ts::kPidVideo) continue;
        const int seen = pkt[3] & 0x0F;
        if (expected >= 0) { CHECK(seen == expected); checked++; }
        expected = (seen + 1) & 0x0F;
    }
    CHECK(checked > 40);   // enough packets that a per-writer reset could not hide
}

// A frame the segment cannot hold is DISCARDED with its counters: the writer advances them per packet, so keeping the advance for unsent packets leaves a gap a player reads as lost packets. This pins that a caller can snapshot and restore them.
TEST_CASE("Discarding an overflowed frame can restore the continuity counters") {
    std::vector<uint8_t> out(64 * 1024);
    mm::ts::Continuity cc;
    const auto small = fakeAccessUnit(400);

    size_t used = 0;
    for (int i = 0; i < 3; i++) {
        mm::ts::Writer w(out.data() + used, out.size() - used, cc);
        w.writeAccessUnit(small.data(), small.size(), static_cast<uint32_t>(i) * 3000u, i == 0);
        REQUIRE_FALSE(w.overflowed());
        used += w.size();
    }
    const mm::ts::Continuity before = cc;

    // A frame far too large for what is left: the writer overflows and the caller drops it.
    std::vector<uint8_t> tiny(2 * mm::ts::kPacketSize);
    mm::ts::Writer w(tiny.data(), tiny.size(), cc);
    const auto huge = fakeAccessUnit(20000);
    w.writeAccessUnit(huge.data(), huge.size(), 9000, false);
    REQUIRE(w.overflowed());
    CHECK(cc.video != before.video);   // it DID advance while emitting
    cc = before;                        // ...and the caller can put it back

    // The next real frame therefore continues the sequence without a gap.
    mm::ts::Writer w2(out.data() + used, out.size() - used, cc);
    w2.writeAccessUnit(small.data(), small.size(), 12000, false);
    REQUIRE_FALSE(w2.overflowed());

    int expected = -1, checked = 0;
    for (size_t off = 0; off < used + w2.size(); off += mm::ts::kPacketSize) {
        const uint8_t* pkt = &out[off];
        if (pidOf(pkt) != mm::ts::kPidVideo) continue;
        const int seen = pkt[3] & 0x0F;
        if (expected >= 0) { CHECK(seen == expected); checked++; }
        expected = (seen + 1) & 0x0F;
    }
    CHECK(checked > 5);
}
