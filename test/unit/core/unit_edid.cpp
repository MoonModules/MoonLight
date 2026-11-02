/// The monitor's EDID reduced to what the HDMI output needs: its name, its preferred size, and which of the output's sizes it lists, from the base block's timings and the CEA extension's video codes.

#include "doctest.h"
#include "platform/edid.h"

#include <array>
#include <cstring>

using namespace mm::edid;

namespace {

// The output's sizes, in the order the ESP32 lists them.
constexpr Size kSizes[] = {{800, 600}, {1024, 768}, {1280, 720}, {1280, 800}, {1920, 1080}};
constexpr size_t kCount = sizeof(kSizes) / sizeof(kSizes[0]);

using Block = std::array<uint8_t, kBlockBytes>;

void seal(Block& b) {
    uint8_t sum = 0;
    for (size_t i = 0; i + 1 < b.size(); i++) sum = static_cast<uint8_t>(sum + b[i]);
    b[127] = static_cast<uint8_t>(-sum);
}

// A 1920x1200 monitor named DELL U2412M, as its base block describes it: 800x600 and 1024x768 established, 1280x800 as a standard timing, 1920x1200 preferred.
Block baseBlock() {
    Block b{};
    const uint8_t header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    std::memcpy(b.data(), header, 8);
    b[18] = 1; b[19] = 4;        // E-EDID 1.4
    b[35] = 0x01;                // established: 800x600 at 60
    b[36] = 0x08;                // established: 1024x768 at 60
    b[38] = 0x81; b[39] = 0x00;  // standard: (0x81 + 31) * 8 = 1280 wide, 16:10, so 800 high
    for (int i = 1; i < 8; i++) { b[38 + 2 * i] = 0x01; b[39 + 2 * i] = 0x01; }   // unused slots
    uint8_t* d = b.data() + 54;  // preferred: 1920x1200
    d[0] = 0x28; d[1] = 0x3c;    // a pixel clock, nonzero so it is a timing
    d[2] = 1920 & 0xff; d[4] = static_cast<uint8_t>((1920 >> 8) << 4);
    d[5] = 1200 & 0xff; d[7] = static_cast<uint8_t>((1200 >> 8) << 4);
    d = b.data() + 72;           // the name descriptor
    d[3] = 0xfc;
    std::memcpy(d + 5, "DELL U2412M\n ", 13);   // the 13 name bytes, padded with a newline and a space as the standard pads them
    b[126] = 1;                  // one extension follows
    seal(b);
    return b;
}

// A CEA-861 extension listing 720p (VIC 4) and 1080p at 60 (VIC 16), and nothing in its detailed timings.
Block ceaBlock() {
    Block b{};
    b[0] = 0x02; b[1] = 0x03;
    b[2] = 4 + 4;                // the detailed timings start after one data block of 4 bytes
    b[4] = static_cast<uint8_t>((2 << 5) | 3);   // video data block, three codes
    b[5] = 4; b[6] = 16; b[7] = 0x80 | 4;         // 4 again with its native flag set
    seal(b);
    return b;
}

}  // namespace

TEST_CASE("EDID: the base block gives the name, the preferred size and the sizes its timings list") {
    const Block base = baseBlock();
    Monitor m;
    REQUIRE(parse(base.data(), nullptr, kSizes, kCount, m));
    CHECK(std::strcmp(m.name, "DELL U2412M") == 0);
    CHECK(m.preferredW == 1920);
    CHECK(m.preferredH == 1200);
    CHECK(m.lists == 0b01011u);   // 800x600, 1024x768 and 1280x800; no 720p, no 1080p
}

TEST_CASE("EDID: the CEA extension's video codes add the sizes it names") {
    const Block base = baseBlock(), ext = ceaBlock();
    Monitor m;
    REQUIRE(parse(base.data(), ext.data(), kSizes, kCount, m));
    CHECK(m.lists == 0b11111u);
}

TEST_CASE("EDID: a block without the header or with a wrong checksum is no monitor's answer") {
    Block base = baseBlock();
    Monitor m;
    base[127] ^= 1;
    CHECK_FALSE(parse(base.data(), nullptr, kSizes, kCount, m));
    base = baseBlock();
    base[0] = 0xff;
    CHECK_FALSE(parse(base.data(), nullptr, kSizes, kCount, m));
    CHECK_FALSE(parse(nullptr, nullptr, kSizes, kCount, m));
}

TEST_CASE("EDID: an extension with a wrong checksum adds nothing, the base block still counts") {
    const Block base = baseBlock();
    Block ext = ceaBlock();
    ext[127] ^= 1;
    Monitor m;
    REQUIRE(parse(base.data(), ext.data(), kSizes, kCount, m));
    CHECK(m.lists == 0b01011u);
}

TEST_CASE("EDID: the CEA video code for 4K at 60 Hz marks 3840x2160") {
    const Size sizes[] = {{1920, 1080}, {3840, 2160}};
    const Block base = baseBlock();
    Block ext{};
    ext[0] = 0x02; ext[1] = 0x03;
    ext[2] = 4 + 2;                                   // one data block of 2 bytes
    ext[4] = static_cast<uint8_t>((2 << 5) | 1);      // video data block, one code
    ext[5] = 97;                                      // 3840x2160 at 60 Hz
    seal(ext);
    Monitor m;
    REQUIRE(parse(base.data(), ext.data(), sizes, 2, m));
    CHECK(m.lists == 0b10u);
}
