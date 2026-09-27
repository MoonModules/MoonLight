/// @module WledAudioSyncPacket

/// Pins the WLED audio-sync wire format, the 44-byte v2 packet MoonLight broadcasts on UDP 11988 and that WLED / MoonLight (D_WLEDAudio.h) receive. A wire format breaks silently, so build → parse is round-tripped AND a golden byte vector fixes the exact offsets: the packet is a fixed compatibility contract (netmindz/WLED-sync), not ours to drift. (Same rigor as the Improv frame golden vector.)

#include "doctest.h"
#include "light/util/WLEDAudioSyncPacket.h"

#include <cstdint>
#include <cstring>
#include <limits>

using namespace mm;

// Build a frame with distinct, checkable values in every field.
static AudioFrame sampleFrame() {
    AudioFrame f;
    f.level = 200;
    f.levelSmoothed = 150;
    f.peakHz = 440;
    f.peakMag = 77;
    for (int i = 0; i < 16; i++) f.bands[i] = static_cast<uint8_t>(i * 16);  // 0,16,...,240
    return f;
}

TEST_CASE("build produces a 44-byte v2 packet with the exact WLED layout") {
    AudioFrame f = sampleFrame();
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    size_t n = buildWledAudioSync(pkt, f, /*peak=*/true);

    CHECK(n == 44);
    CHECK(WLED_SYNC_PACKET_SIZE == 44);
    CHECK(WLED_SYNC_PORT == 11988);

    // header "00002" (+ NUL) at offset 0
    CHECK(std::memcmp(pkt, "00002", 6) == 0);
    // gap1 (offset 6-7) is zero
    CHECK(pkt[6] == 0); CHECK(pkt[7] == 0);
    // sampleRaw = level (float LE) at offset 8
    CHECK(wledGetFloatLE(pkt + 8) == doctest::Approx(200.0f));
    // sampleSmth = levelSmoothed at offset 12
    CHECK(wledGetFloatLE(pkt + 12) == doctest::Approx(150.0f));
    // samplePeak at 16; byte 17 is WLED's `reserved2` ("not used yet") and MUST be zero, not a counter of ours: WLED transmits 0 there and may claim the byte in a later version.
    CHECK(pkt[16] == 1);
    CHECK(pkt[17] == 0);
    // fftResult[16] = bands at offset 18
    for (int i = 0; i < 16; i++) CHECK(pkt[18 + i] == static_cast<uint8_t>(i * 16));
    // gap2 (offset 34-35) is zero
    CHECK(pkt[34] == 0); CHECK(pkt[35] == 0);
    // FFT_Magnitude = peakMag at 36, FFT_MajorPeak = peakHz at 40 peakMag rides the wire in WLED's units: x16, the divisor its effects apply.
    CHECK(wledGetFloatLE(pkt + 36) == doctest::Approx(77.0f * kWledMagScale));
    CHECK(wledGetFloatLE(pkt + 40) == doctest::Approx(440.0f));
}

// WLED clamps every band to 254 on send (constrain(fftResult[i], 0, 254)), so 255 never appears on the wire. A receiver written against WLED may treat 255 as a value real data cannot carry. The magnitude crosses in WLED's units and comes back in ours, so a MoonLight pair round-trips exactly while a WLED peer reads the value its own effects expect.
TEST_CASE("FFT_Magnitude carries WLED units and round-trips back to ours") {
    AudioFrame f{};
    f.peakMag = 144;                     // WLED's "full brightness" threshold after its /16
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, f, /*peak=*/false);
    CHECK(wledGetFloatLE(pkt + 36) == doctest::Approx(2304.0f));   // 144 * 16

    AudioFrame back{};
    REQUIRE(parseWledAudioSync(pkt, WLED_SYNC_PACKET_SIZE, back));
    CHECK(back.peakMag == 144);
}

// A real WLED source reaches ~9500, far past our 0..255. Clamping keeps a received frame from driving effects harder than a locally analyzed one ever could.
TEST_CASE("a WLED magnitude above our range clamps instead of wrapping") {
    AudioFrame f{};
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, f, /*peak=*/false);
    wledPutFloatLE(pkt + 36, 9491.0f);          // measured from a real WLED device
    AudioFrame out{};
    REQUIRE(parseWledAudioSync(pkt, WLED_SYNC_PACKET_SIZE, out));
    CHECK(out.peakMag == 255);
}

TEST_CASE("bands are clamped to 254, the way WLED sends them") {
    AudioFrame f{};
    for (int i = 0; i < 16; i++) f.bands[i] = 255;
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, f, /*peak=*/false);
    for (int i = 0; i < 16; i++) CHECK(pkt[18 + i] == 254);
}

TEST_CASE("build -> parse round-trips every AudioFrame field") {
    AudioFrame f = sampleFrame();
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, f, false);

    AudioFrame out;
    REQUIRE(parseWledAudioSync(pkt, WLED_SYNC_PACKET_SIZE, out));
    CHECK(out.level == f.level);
    CHECK(out.levelSmoothed == f.levelSmoothed);
    CHECK(out.peakHz == f.peakHz);
    CHECK(out.peakMag == f.peakMag);
    for (int i = 0; i < 16; i++) CHECK(out.bands[i] == f.bands[i]);
}

TEST_CASE("parse rejects wrong length, wrong header, v1, and null") {
    AudioFrame f = sampleFrame();
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, f, false);
    AudioFrame out;

    // exact-length valid packet parses
    CHECK(parseWledAudioSync(pkt, 44, out));
    // one byte short / one byte long, rejected (WLED sends exactly 44)
    CHECK_FALSE(parseWledAudioSync(pkt, 43, out));
    CHECK_FALSE(parseWledAudioSync(pkt, 45, out));
    // wrong header ("00001" = the legacy v1 packet), rejected, not crashed
    uint8_t v1[44]; std::memcpy(v1, pkt, 44); std::memcpy(v1, "00001", 6);
    CHECK_FALSE(parseWledAudioSync(v1, 44, out));
    // an 83-byte v1-sized buffer, rejected on length
    uint8_t big[83] = {}; std::memcpy(big, "00001", 6);
    CHECK_FALSE(parseWledAudioSync(big, sizeof(big), out));
    // null
    CHECK_FALSE(parseWledAudioSync(nullptr, 44, out));
}

TEST_CASE("parse clamps NaN / out-of-range floats instead of undefined casts") {
    // A foreign packet passes the header check but carries hostile float payloads. wledFloatToU16 must bound them to [0, 65535] rather than let static_cast<uint16_t> of a NaN / huge / negative float produce an undefined result.
    AudioFrame f = sampleFrame();
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, f, false);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    wledPutFloatLE(pkt + 8,  nan);          // level         → NaN
    wledPutFloatLE(pkt + 12, -5.0f);        // levelSmoothed → negative
    wledPutFloatLE(pkt + 36, 1e9f);         // peakMag       → far over 65535
    wledPutFloatLE(pkt + 40, 440.6f);       // peakHz        → normal, truncates to 440

    AudioFrame out;
    REQUIRE(parseWledAudioSync(pkt, WLED_SYNC_PACKET_SIZE, out));
    CHECK(out.level == 0);            // NaN → 0
    CHECK(out.levelSmoothed == 0);    // negative → 0
    CHECK(out.peakMag == 255);        // huge magnitude → clamped to OUR 0..255 range
    CHECK(out.peakHz == 440);         // in range → truncated

    // The helper directly, at the boundaries.
    CHECK(wledFloatToU16(nan) == 0);
    CHECK(wledFloatToU16(-1.0f) == 0);
    CHECK(wledFloatToU16(0.0f) == 0);
    CHECK(wledFloatToU16(65535.0f) == 65535);
    CHECK(wledFloatToU16(70000.0f) == 65535);
    CHECK(wledFloatToU16(123.9f) == 123);
}

TEST_CASE("golden vector — the exact bytes on the wire (the compatibility contract)") {
    // A fixed frame → the exact 44 bytes any WLED receiver must accept. If this changes, interop with WLED/MoonLight breaks, the test is the alarm.
    AudioFrame f;
    f.level = 100; f.levelSmoothed = 50; f.peakHz = 1000; f.peakMag = 25;
    for (int i = 0; i < 16; i++) f.bands[i] = static_cast<uint8_t>(i);   // 0..15
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, f, /*peak=*/false);

    // Little-endian IEEE-754 for 100.0, 50.0, 25.0, 1000.0
    const uint8_t golden[44] = {
        '0','0','0','0','2','\0',   // 0-5  header
        0, 0,                       // 6-7  gap1
        0x00,0x00,0xC8,0x42,        // 8-11  sampleRaw   = 100.0f
        0x00,0x00,0x48,0x42,        // 12-15 sampleSmth  = 50.0f
        0,                          // 16    samplePeak
        0,                          // 17    reserved2: WLED sends zero here, so we do too
        0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,  // 18-33 fftResult
        0, 0,                       // 34-35 gap2
        0x00,0x00,0xC8,0x43,        // 36-39 FFT_Magnitude = 400.0f (peakMag 25 x16, WLED units)
        0x00,0x00,0x7A,0x44,        // 40-43 FFT_MajorPeak = 1000.0f
    };
    CHECK(std::memcmp(pkt, golden, 44) == 0);
}
