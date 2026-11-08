/// @module MoonLightAudioSyncPacket
/// @also WledAudioSyncPacket

/// Pins the MoonLight audio-sync format: every field round-trips, a golden vector fixes the bytes, and a receiver skips what a later version adds.

#include "doctest.h"
#include "core/util/MoonLightAudioSyncPacket.h"

#include <cstdint>
#include <cstring>

using namespace mm;

namespace {
// A frame with distinct values in every field the format carries, the harmony on.
AudioFrame fullFrame() {
    AudioFrame f;
    f.level = 200; f.levelSmoothed = 150; f.gate = 40; f.gateOpen = true;
    f.onset = 90; f.onsetLow = 80; f.onsetMid = 70; f.onsetHigh = 60;
    f.peakHz = 4321; f.peakMag = 77;
    for (uint8_t i = 0; i < 16; i++) { f.bands[i] = static_cast<uint8_t>(i * 16); f.bandsSmoothed[i] = static_cast<uint8_t>(255 - i); }
    f.thirdsCount = 31;   // a 48 kHz frame, every third measured
    for (uint8_t i = 0; i < 31; i++) f.thirds[i] = static_cast<uint8_t>(i * 9);
    f.thirdsHits[0] = 0x81; f.thirdsHits[3] = 0x05;
    for (uint8_t i = 0; i < 12; i++) f.chroma[i] = static_cast<uint8_t>(i * 20);
    f.key = 22; f.tuning = -12; f.harmonyChange = true; f.bassNote = 40;
    f.notesCount = kAudioNoteKeys;
    for (uint8_t i = 0; i < kAudioNoteKeys; i++) f.notes[i] = static_cast<uint8_t>(i * 3);
    f.notesHits[0] = 0x10; f.notesHits[9] = 0x40;
    f.notesKept[0] = 0x30; f.notesKept[5] = 0x02;
    f.notesKeptHits[0] = 0x10; f.notesKeptHits[5] = 0x02;
    f.seq = 0xBEEF;
    return f;
}
}  // namespace

TEST_CASE("a frame with the harmony round-trips every field it carries, in 218 bytes") {
    const AudioFrame f = fullFrame();
    uint8_t pkt[kMlSyncMaxPacket];
    const size_t n = buildMoonLightAudioSync(pkt, f);
    CHECK(n == 218);
    CHECK(n == kMlSyncMaxPacket);
    AudioFrame out;
    REQUIRE(parseMoonLightAudioSync(pkt, n, out));
    CHECK(out.seq == f.seq);
    CHECK(out.level == f.level);
    CHECK(out.levelSmoothed == f.levelSmoothed);
    CHECK(out.gate == f.gate);
    CHECK(out.gateOpen == f.gateOpen);
    CHECK(out.onset == f.onset);
    CHECK(out.onsetLow == f.onsetLow);
    CHECK(out.onsetMid == f.onsetMid);
    CHECK(out.onsetHigh == f.onsetHigh);
    CHECK(out.peakHz == f.peakHz);
    CHECK(out.peakMag == f.peakMag);
    CHECK(std::memcmp(out.bands, f.bands, 16) == 0);
    CHECK(std::memcmp(out.bandsSmoothed, f.bandsSmoothed, 16) == 0);
    CHECK(out.thirdsCount == f.thirdsCount);
    CHECK(std::memcmp(out.thirds, f.thirds, f.thirdsCount) == 0);
    CHECK(std::memcmp(out.thirdsHits, f.thirdsHits, sizeof(f.thirdsHits)) == 0);
    CHECK(std::memcmp(out.chroma, f.chroma, 12) == 0);
    CHECK(out.key == f.key);
    CHECK(out.tuning == f.tuning);
    CHECK(out.harmonyChange == f.harmonyChange);
    CHECK(out.bassNote == f.bassNote);
    CHECK(out.notesCount == f.notesCount);
    CHECK(std::memcmp(out.notes, f.notes, kAudioNoteKeys) == 0);
    CHECK(std::memcmp(out.notesHits, f.notesHits, sizeof(f.notesHits)) == 0);
    CHECK(std::memcmp(out.notesKept, f.notesKept, sizeof(f.notesKept)) == 0);
    CHECK(std::memcmp(out.notesKeptHits, f.notesKeptHits, sizeof(f.notesKeptHits)) == 0);
}

// Every receiver shows the notes the sender's core kept, so a notes section without them is malformed, as a section running short is.
TEST_CASE("a notes section without the kept notes and their strikes is refused") {
    const AudioFrame f = fullFrame();
    uint8_t pkt[kMlSyncMaxPacket];
    const size_t n = buildMoonLightAudioSync(pkt, f);
    const size_t kept = sizeof(f.notesKept) + sizeof(f.notesKeptHits);
    pkt[n - (1 + kAudioNoteKeys + sizeof(f.notesHits) + kept) - 1] = static_cast<uint8_t>(1 + kAudioNoteKeys + sizeof(f.notesHits));   // the length an older sender wrote
    AudioFrame out;
    CHECK_FALSE(parseMoonLightAudioSync(pkt, n - kept, out));
}

// With `harmony` off a board sends no keys and no harmony, and a receiver reads them as absent, which is what the effects fall back on.
TEST_CASE("a frame without the harmony leaves its sections out, 90 bytes, and reads back with none") {
    AudioFrame f = fullFrame();
    f.notesCount = 0;
    uint8_t pkt[kMlSyncMaxPacket];
    const size_t n = buildMoonLightAudioSync(pkt, f);
    CHECK(n == 90);
    AudioFrame out;
    REQUIRE(parseMoonLightAudioSync(pkt, n, out));
    CHECK(out.thirdsCount == 31);
    CHECK(out.notesCount == 0);
    CHECK(out.key == 0);
    CHECK(out.chroma[5] == 0);
    CHECK_FALSE(out.harmonyChange);
}

TEST_CASE("golden vector: the header, the core and the smoothed bands, the exact bytes on the wire") {
    AudioFrame f;
    f.level = 100; f.levelSmoothed = 50; f.gate = 40; f.gateOpen = true;
    f.onset = 7; f.onsetLow = 7;
    f.peakHz = 1000; f.peakMag = 25;
    for (uint8_t i = 0; i < 16; i++) f.bands[i] = i;
    f.seq = 0x0102;
    uint8_t pkt[kMlSyncMaxPacket];
    REQUIRE(buildMoonLightAudioSync(pkt, f) == 53);
    const uint8_t golden[53] = {
        'M', 'L', 1, 0,             // magic, version, flags
        0x02, 0x01,                 // seq, little-endian
        0x03, 0x00,                 // section mask: the core and the smoothed bands
        27,                         // the core's length
        100, 50, 40, 1,             // level, levelSmoothed, gate, gateOpen
        7, 7, 0, 0,                 // onset, onsetLow, onsetMid, onsetHigh
        0xE8, 0x03, 25,             // peakHz 1000, peakMag
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,   // bands
        16,                                                     // the smoothed bands' length
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,          // bandsSmoothed
    };
    CHECK(std::memcmp(pkt, golden, sizeof(golden)) == 0);
}

// The extension rule: a later version adds a section bit or lengthens a section, and this receiver still reads what it knows.
TEST_CASE("a receiver skips a section it does not know and reads the known prefix of a longer one") {
    AudioFrame f;
    f.level = 77; f.bands[3] = 33;
    uint8_t pkt[kMlSyncMaxPacket + 16];
    size_t n = buildMoonLightAudioSync(pkt, f);
    REQUIRE(n == 53);
    // The smoothed bands, the last section, grow by two bytes a later version appended.
    pkt[8 + 1 + kMlSyncCoreSize] = kMlSyncSmoothedSize + 2;
    pkt[n++] = 0xAA; pkt[n++] = 0xBB;
    // And a section bit 9 this version does not know, three bytes long.
    pkt[7] = 0x02;
    pkt[n++] = 3; pkt[n++] = 1; pkt[n++] = 2; pkt[n++] = 3;
    AudioFrame out;
    REQUIRE(parseMoonLightAudioSync(pkt, n, out));
    CHECK(out.level == 77);
    CHECK(out.bands[3] == 33);
}

// A sender that does not smooth, WLED adopting the format among them, leaves the smoothed bands out rather than sending zeros a receiver would show.
TEST_CASE("a packet of the core alone is 36 bytes, parses, and says the smoothed bands are absent") {
    AudioFrame f;
    f.level = 120; f.bands[0] = 200;
    uint8_t pkt[kMlSyncMaxPacket];
    size_t n = buildMoonLightAudioSync(pkt, f);
    // Drop the smoothed section as such a sender would: the core runs to the end.
    pkt[6] = 1u << kMlSectionCore;
    n = kMlSyncHeaderSize + 1 + kMlSyncCoreSize;
    CHECK(n == 36);
    AudioFrame out;
    const uint16_t sections = parseMoonLightAudioSync(pkt, n, out);
    REQUIRE(sections != 0);
    CHECK((sections & (1u << kMlSectionSmoothed)) == 0);
    CHECK(out.level == 120);
    CHECK(out.bands[0] == 200);
}

TEST_CASE("parse rejects a short packet, another magic or version, a missing core, a section past the end and a count past its array") {
    const AudioFrame f = fullFrame();
    uint8_t pkt[kMlSyncMaxPacket];
    const size_t n = buildMoonLightAudioSync(pkt, f);
    AudioFrame out;
    REQUIRE(parseMoonLightAudioSync(pkt, n, out));
    CHECK_FALSE(parseMoonLightAudioSync(nullptr, n, out));
    CHECK_FALSE(parseMoonLightAudioSync(pkt, kMlSyncHeaderSize - 1, out));
    CHECK_FALSE(parseMoonLightAudioSync(pkt, n - 1, out));   // the notes section runs past the datagram

    uint8_t bad[kMlSyncMaxPacket];
    std::memcpy(bad, pkt, n); bad[0] = '0';
    CHECK_FALSE(parseMoonLightAudioSync(bad, n, out));       // WLED's "00002" is not ours
    std::memcpy(bad, pkt, n); bad[2] = kMlSyncVersion + 1;
    CHECK_FALSE(parseMoonLightAudioSync(bad, n, out));
    std::memcpy(bad, pkt, n); bad[6] &= static_cast<uint8_t>(~1u);
    CHECK_FALSE(parseMoonLightAudioSync(bad, n, out));       // no core section
    std::memcpy(bad, pkt, n); bad[8 + 1 + kMlSyncCoreSize + 1 + kMlSyncSmoothedSize + 1] = 32;
    CHECK_FALSE(parseMoonLightAudioSync(bad, n, out));       // 32 thirds, one past the array
}
