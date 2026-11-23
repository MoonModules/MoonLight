/// @module MoonModulesAudioSyncPacket
/// @also WledAudioSyncPacket

/// Pins the MoonModules audio-sync format: every field round-trips, a golden vector fixes the bytes, and a receiver reads what an older or a later version sends.

#include "doctest.h"
#include "core/util/MoonModulesAudioSyncPacket.h"

#include <cstdint>
#include <cstring>

using namespace mm;

namespace {
// A frame with distinct values in every field the format carries, the harmony and a beat on.
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
    f.beat.bpm100 = 12400; f.beat.count = 77; f.beat.phase = 100; f.beat.inBar = 2; f.beat.source = kBeatStageLinq;
    f.beat.confidence = 255; f.beat.inPhrase = 6; f.beat.phraseBeats = 32; f.beat.flags = AudioBeat::kBeatDownbeatKnown;
    return f;
}
}  // namespace

TEST_CASE("a frame with the harmony and a beat round-trips every field it carries, in 239 bytes") {
    const AudioFrame f = fullFrame();
    uint8_t pkt[kMmSyncMaxPacket];
    const size_t n = buildMoonModulesAudioSync(pkt, f);
    CHECK(n == 239);
    CHECK(n == kMmSyncMaxPacket);
    AudioFrame out;
    REQUIRE(parseMoonModulesAudioSync(pkt, n, out));
    CHECK(out.seq == f.seq);
    CHECK(out.beat.bpm100 == f.beat.bpm100);   // the beat, field by field: a memcmp would read the struct's padding
    CHECK(out.beat.count == f.beat.count);
    CHECK(out.beat.phase == f.beat.phase);
    CHECK(out.beat.inBar == f.beat.inBar);
    CHECK(out.beat.source == f.beat.source);
    CHECK(out.beat.confidence == f.beat.confidence);
    CHECK(out.beat.inPhrase == f.beat.inPhrase);
    CHECK(out.beat.phraseBeats == f.beat.phraseBeats);
    CHECK(out.beat.flags == f.beat.flags);
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

// The rule that lets a section grow: a receiver reads the fields a section holds, so an older sender's shorter section still counts.
TEST_CASE("an older sender's notes section without the kept notes gives its notes, the kept ones read as none") {
    AudioFrame f = fullFrame();
    f.beat = AudioBeat{};   // no beat, so the notes are the last section
    uint8_t pkt[kMmSyncMaxPacket];
    const size_t n = buildMoonModulesAudioSync(pkt, f);
    const size_t kept = sizeof(f.notesKept) + sizeof(f.notesKeptHits);
    const size_t older = 1 + kAudioNoteKeys + sizeof(f.notesHits);   // the notes section an older sender wrote
    const size_t lengthAt = n - (older + kept) - 2;
    pkt[lengthAt] = static_cast<uint8_t>(older); pkt[lengthAt + 1] = 0;
    AudioFrame out;
    REQUIRE(parseMoonModulesAudioSync(pkt, n - kept, out));
    CHECK(out.notesCount == kAudioNoteKeys);
    CHECK(out.notesHits[9] == 0x40);
    CHECK(out.notesKept[0] == 0);
    CHECK(out.notesKeptHits[5] == 0);
}

// While the notes do not run a board sends no keys and no harmony, and a receiver reads them as absent, which is what the effects fall back on.
TEST_CASE("a frame without the harmony leaves its sections out, 93 bytes, and reads back with none") {
    AudioFrame f = fullFrame();
    f.notesCount = 0;
    f.beat = AudioBeat{};
    uint8_t pkt[kMmSyncMaxPacket];
    const size_t n = buildMoonModulesAudioSync(pkt, f);
    CHECK(n == 93);
    AudioFrame out;
    REQUIRE(parseMoonModulesAudioSync(pkt, n, out));
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
    uint8_t pkt[kMmSyncMaxPacket];
    REQUIRE(buildMoonModulesAudioSync(pkt, f) == 54);
    const uint8_t golden[54] = {
        'M', 'M', 1,                // magic, version
        0x02, 0x01,                 // seq, little-endian
        0, 27, 0,                   // the core: type 0, 27 bytes
        100, 50, 40, 1,             // level, levelSmoothed, gate, gateOpen
        7, 7, 0, 0,                 // onset, onsetLow, onsetMid, onsetHigh
        0xE8, 0x03, 25,             // peakHz 1000, peakMag
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,   // bands
        1, 16, 0,                                               // the smoothed bands: type 1, 16 bytes
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,          // bandsSmoothed
    };
    CHECK(std::memcmp(pkt, golden, sizeof(golden)) == 0);
}

// The extension rules: a later version adds a type or lengthens a section, sections come in any order, and this receiver still reads what it knows.
TEST_CASE("a receiver skips a type it does not know, reads the start of a longer section, and takes sections in any order") {
    uint8_t pkt[64] = {'M', 'M', 1, 0x05, 0x00};
    size_t n = kMmSyncHeaderSize;
    const uint8_t unknown[] = {9, 3, 0, 1, 2, 3};   // type 9, three bytes this version does not know
    std::memcpy(pkt + n, unknown, sizeof(unknown)); n += sizeof(unknown);
    pkt[n++] = kMmSectionSmoothed; pkt[n++] = kMmSyncSmoothedSize + 2; pkt[n++] = 0;   // two bytes a later version appended
    for (uint8_t i = 0; i < 16; i++) pkt[n++] = static_cast<uint8_t>(100 + i);
    pkt[n++] = 0xAA; pkt[n++] = 0xBB;
    pkt[n++] = kMmSectionCore; pkt[n++] = 1; pkt[n++] = 0;   // a core of its first field alone, after the smoothed bands
    pkt[n++] = 77;
    AudioFrame out;
    uint16_t sections = 0;
    REQUIRE(parseMoonModulesAudioSync(pkt, n, out, &sections));
    CHECK(out.seq == 5);
    CHECK(out.level == 77);
    CHECK(out.bands[3] == 0);   // a field the short core does not hold reads as none
    CHECK(out.bandsSmoothed[15] == 115);
    CHECK(sections == ((1u << kMmSectionCore) | (1u << kMmSectionSmoothed)));
}

// A later version's section is skipped by its length, and no section is required.
TEST_CASE("a packet without the core parses, an absent section meaning not measured") {
    const uint8_t laterOnly[] = {'M', 'M', 1, 0x07, 0x00, 9, 2, 0, 0x00, 0x32};   // a type this version does not know
    AudioFrame out;
    uint16_t sections = 0xFFFF;
    REQUIRE(parseMoonModulesAudioSync(laterOnly, sizeof(laterOnly), out, &sections));
    CHECK(sections == 0);
    CHECK(out.seq == 7);
    CHECK(out.level == 0);
    const uint8_t headerOnly[] = {'M', 'M', 1, 0x08, 0x00};
    CHECK(parseMoonModulesAudioSync(headerOnly, sizeof(headerOnly), out));
}

// A sender that does not smooth, WLED adopting the format among them, leaves the smoothed bands out rather than sending zeros a receiver would show.
TEST_CASE("a packet of the core alone is 35 bytes, parses, and says the smoothed bands are absent") {
    AudioFrame f;
    f.level = 120; f.bands[0] = 200;
    uint8_t pkt[kMmSyncMaxPacket];
    buildMoonModulesAudioSync(pkt, f);
    const size_t n = kMmSyncHeaderSize + kMmSyncSectionHeader + kMmSyncCoreSize;   // the core comes first: what such a sender sends
    CHECK(n == 35);
    AudioFrame out;
    uint16_t sections = 0;
    REQUIRE(parseMoonModulesAudioSync(pkt, n, out, &sections));
    CHECK(sections == (1u << kMmSectionCore));
    CHECK(out.level == 120);
    CHECK(out.bands[0] == 200);
}

// A missing field ends its section: the fields after it are missing too, so none is read from the wrong place.
TEST_CASE("a thirds section cut inside its levels reads no levels and no hits") {
    const uint8_t pkt[] = {'M', 'M', 1, 0, 0, kMmSectionThirds, 6, 0, 31, 1, 2, 3, 4, 5};   // 31 levels announced, five there
    AudioFrame out;
    REQUIRE(parseMoonModulesAudioSync(pkt, sizeof(pkt), out));
    CHECK(out.thirdsCount == 0);
    CHECK(out.thirdsHits[0] == 0);
}

TEST_CASE("parse rejects a short packet, another magic or version, a section past the end and a count past its array") {
    const AudioFrame f = fullFrame();
    uint8_t pkt[kMmSyncMaxPacket];
    const size_t n = buildMoonModulesAudioSync(pkt, f);
    AudioFrame out;
    REQUIRE(parseMoonModulesAudioSync(pkt, n, out));
    CHECK_FALSE(parseMoonModulesAudioSync(nullptr, n, out));
    CHECK_FALSE(parseMoonModulesAudioSync(pkt, kMmSyncHeaderSize - 1, out));
    CHECK_FALSE(parseMoonModulesAudioSync(pkt, n - 1, out));                        // the last section runs past the datagram
    CHECK_FALSE(parseMoonModulesAudioSync(pkt, kMmSyncHeaderSize + 2, out));        // a section's header cut off

    uint8_t bad[kMmSyncMaxPacket];
    std::memcpy(bad, pkt, n); bad[0] = '0';
    CHECK_FALSE(parseMoonModulesAudioSync(bad, n, out));       // WLED's "00002" is not ours
    std::memcpy(bad, pkt, n); bad[2] = kMmSyncVersion + 1;
    CHECK_FALSE(parseMoonModulesAudioSync(bad, n, out));
    const size_t thirdsCount = kMmSyncHeaderSize + kMmSyncSectionHeader + kMmSyncCoreSize + kMmSyncSectionHeader + kMmSyncSmoothedSize + kMmSyncSectionHeader;
    std::memcpy(bad, pkt, n); bad[thirdsCount] = 32;
    CHECK_FALSE(parseMoonModulesAudioSync(bad, n, out));       // 32 thirds, one past the array
}

// A Pro DJ Link bridge sends the beat alone, with no audio measured: the beat section on its own is a packet.
TEST_CASE("the beat section carries a deck's tempo, count, phase, bar, phrase and fill, byte for byte") {
    AudioFrame f;
    f.seq = 0x0102;
    f.beat.bpm100 = 12800; f.beat.count = 0x0304; f.beat.phase = 64; f.beat.inBar = 1;
    f.beat.source = kBeatProLink; f.beat.confidence = 255; f.beat.inPhrase = 29; f.beat.phraseBeats = 32;
    f.beat.flags = AudioBeat::kBeatDownbeatKnown | AudioBeat::kBeatFill;
    uint8_t pkt[kMmSyncMaxPacket];
    const size_t n = buildMoonModulesAudioSync(pkt, f);
    const uint8_t beat[] = {kMmSectionBeat, 11, 0, 0x00, 0x32, 0x04, 0x03, 64, 1, kBeatProLink, 255, 29, 32, 3};
    REQUIRE(n >= sizeof(beat));
    CHECK(std::memcmp(pkt + n - sizeof(beat), beat, sizeof(beat)) == 0);   // the last section, 128.00 BPM little-endian

    const uint8_t beatOnly[] = {'M', 'M', 1, 0x02, 0x01, kMmSectionBeat, 11, 0, 0x00, 0x32, 0x04, 0x03, 64, 1, kBeatProLink, 255, 29, 32, 3};
    AudioFrame out;
    uint16_t sections = 0;
    REQUIRE(parseMoonModulesAudioSync(beatOnly, sizeof(beatOnly), out, &sections));
    CHECK(sections == (1u << kMmSectionBeat));
    CHECK(out.level == 0);   // no core: no audio measured
    CHECK(out.beat.bpm100 == 12800);
    CHECK(out.beat.count == 0x0304);
    CHECK(out.beat.phase == 64);
    CHECK(out.beat.inBar == 1);
    CHECK(out.beat.source == kBeatProLink);
    CHECK(out.beat.confidence == 255);
    CHECK(out.beat.inPhrase == 29);
    CHECK(out.beat.phraseBeats == 32);
    CHECK(out.beat.flags == (AudioBeat::kBeatDownbeatKnown | AudioBeat::kBeatFill));
}

TEST_CASE("a frame with no beat sends no beat section, and an 8-byte beat section gives the beat with no phrase") {
    AudioFrame f;
    f.level = 10;
    uint8_t pkt[kMmSyncMaxPacket];
    const size_t n = buildMoonModulesAudioSync(pkt, f);
    AudioFrame out;
    uint16_t sections = 0;
    REQUIRE(parseMoonModulesAudioSync(pkt, n, out, &sections));
    CHECK((sections & (1u << kMmSectionBeat)) == 0);   // bpm100 0: no beat to land on
    CHECK(out.beat.bpm100 == 0);

    const uint8_t older[] = {'M', 'M', 1, 0, 0, kMmSectionBeat, 8, 0, 0x10, 0x2E, 7, 0, 128, 3, kBeatTracker, 180};   // 118.24 BPM, the proposal's 8 bytes
    REQUIRE(parseMoonModulesAudioSync(older, sizeof(older), out));
    CHECK(out.beat.bpm100 == 11792);
    CHECK(out.beat.inBar == 3);
    CHECK(out.beat.confidence == 180);
    CHECK(out.beat.phraseBeats == 0);   // fields the section does not hold read as none
    CHECK(out.beat.flags == 0);
}
