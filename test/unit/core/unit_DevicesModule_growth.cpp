/// @module DevicesModule
///
/// Pins the list's growth: no fixed size, and when memory refuses to grow it, a newly heard device takes the place of the one worth least.
///
/// @technical
///
/// ## How memory is refused
///
/// The list grows through `ScratchBuffer::ensure`, which checks `platform::fitsWithReserve` first. `platform::setTestMaxAllocBlock` caps the largest block the platform reports, so a growth past the rows already held is refused while those rows stay.
///
/// ## What is worth keeping
///
/// This device most, then MoonLight boards, then everything else, and among equals the one heard longest ago goes first. Virtual time through `platform::setTestNowMs` orders the sightings.

#include "doctest.h"
#include "discovery_rows.h"
#include "core/system/DevicesModule.h"
#include "core/system/WledPacket.h"
#include "core/util/JsonSink.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace mm;

namespace {

// Restores the real clock, the uncapped heap and the host's own address, even when an assertion throws.
struct Guard {
    ~Guard() {
        platform::setTestNowMs(0);
        platform::setTestMaxAllocBlock(0);
        platform::setTestEthIPv4(nullptr);
    }
};

// A presence packet from `ip`, marked a MoonLight peer when `mm`, heard at virtual time `atMs`.
void hear(DevicesModule& dev, const uint8_t ip[4], const char* name, bool mm, uint32_t atMs) {
    platform::setTestNowMs(atMs);
    discovery_rows::hearPresence(dev, ip, name, mm);
}

// Whether a row holds this address.
bool listed(const DevicesModule& dev, const char* ip) { return !discovery_rows::rowAt(dev, ip).empty(); }

// Four rows, then growth refused: two other devices heard at 1 s and 2 s, two MoonLight boards at 3 s and 4 s. RFC 5737's 192.0.2.0/24.
void fillFourThenRefuse(DevicesModule& dev) {
    const uint8_t w1[4] = {192, 0, 2, 11}, w2[4] = {192, 0, 2, 12}, m1[4] = {192, 0, 2, 21}, m2[4] = {192, 0, 2, 22};
    hear(dev, w1, "wled-1", false, 1000);
    hear(dev, w2, "wled-2", false, 2000);
    hear(dev, m1, "mm-1", true, 3000);
    hear(dev, m2, "mm-2", true, 4000);
    REQUIRE(dev.listRowCount() == 4);
    platform::setTestMaxAllocBlock(1);   // every growth past the four rows held is refused
}

}  // namespace

TEST_CASE("DevicesModule: a thousand devices are all listed, the list having no fixed size") {
    Guard guard;
    DevicesModule dev;
    char name[16];
    // RFC 2544's benchmarking range 198.18.0.0/15, wide enough for a thousand addresses.
    for (uint16_t n = 0; n < 1000; n++) {
        const uint8_t ip[4] = {198, 18, static_cast<uint8_t>(n / 250), static_cast<uint8_t>(1 + n % 250)};
        std::snprintf(name, sizeof(name), "dev-%04u", static_cast<unsigned>(n));
        hear(dev, ip, name, n % 2 == 0, 1000u + n);
    }
    CHECK(dev.listRowCount() == 1000);
    CHECK(listed(dev, "198.18.0.1"));
    CHECK(listed(dev, "198.18.3.250"));
    CHECK(std::strstr(dev.status(), "1000 devices") != nullptr);
    CHECK(std::strstr(dev.status(), "memory full") == nullptr);
}

TEST_CASE("DevicesModule: with memory full, a new MoonLight board replaces the other device heard longest ago") {
    Guard guard;
    DevicesModule dev;
    fillFourThenRefuse(dev);
    const uint8_t m3[4] = {192, 0, 2, 23};
    hear(dev, m3, "mm-3", true, 5000);
    CHECK(dev.listRowCount() == 4);
    CHECK(listed(dev, "192.0.2.23"));
    CHECK_FALSE(listed(dev, "192.0.2.11"));   // the oldest other device made room
    CHECK(listed(dev, "192.0.2.12"));
    CHECK(listed(dev, "192.0.2.21"));         // a MoonLight board, kept though heard earlier
    CHECK(std::strstr(dev.status(), "memory full") != nullptr);
}

TEST_CASE("DevicesModule: with memory full, an other device never replaces a MoonLight board") {
    Guard guard;
    DevicesModule dev;
    fillFourThenRefuse(dev);
    const uint8_t w3[4] = {192, 0, 2, 13}, w4[4] = {192, 0, 2, 14}, w5[4] = {192, 0, 2, 15};
    hear(dev, w3, "wled-3", false, 5000);     // replaces wled-1, an equal heard longest ago
    hear(dev, w4, "wled-4", false, 6000);     // replaces wled-2
    CHECK(listed(dev, "192.0.2.13"));
    CHECK(listed(dev, "192.0.2.14"));
    CHECK_FALSE(listed(dev, "192.0.2.11"));
    CHECK_FALSE(listed(dev, "192.0.2.12"));
    hear(dev, w5, "wled-5", false, 7000);     // replaces wled-3, never a MoonLight board
    CHECK_FALSE(listed(dev, "192.0.2.13"));
    CHECK(listed(dev, "192.0.2.15"));
    CHECK(listed(dev, "192.0.2.21"));
    CHECK(listed(dev, "192.0.2.22"));
    CHECK(dev.listRowCount() == 4);
}

TEST_CASE("DevicesModule: with memory full, this device's row is never replaced") {
    Guard guard;
    const uint8_t self[4] = {192, 0, 2, 1};
    platform::setTestEthIPv4(self);
    DevicesModule dev;
    hear(dev, self, "MM-Self", true, 1000);   // this device, heard first and so the oldest row
    const uint8_t m1[4] = {192, 0, 2, 21}, m2[4] = {192, 0, 2, 22}, m3[4] = {192, 0, 2, 23}, m4[4] = {192, 0, 2, 24};
    hear(dev, m1, "mm-1", true, 2000);
    hear(dev, m2, "mm-2", true, 3000);
    hear(dev, m3, "mm-3", true, 4000);
    REQUIRE(dev.listRowCount() == 4);
    platform::setTestMaxAllocBlock(1);
    hear(dev, m4, "mm-4", true, 5000);
    CHECK(listed(dev, "192.0.2.1"));          // kept though heard longest ago
    CHECK_FALSE(listed(dev, "192.0.2.21"));   // the oldest MoonLight board made room
    CHECK(listed(dev, "192.0.2.24"));
}

TEST_CASE("DevicesModule: once memory returns, the list grows again and the status clears") {
    Guard guard;
    DevicesModule dev;
    fillFourThenRefuse(dev);
    const uint8_t m3[4] = {192, 0, 2, 23}, m4[4] = {192, 0, 2, 24};
    hear(dev, m3, "mm-3", true, 5000);
    REQUIRE(std::strstr(dev.status(), "memory full") != nullptr);
    platform::setTestMaxAllocBlock(0);        // memory back
    hear(dev, m4, "mm-4", true, 6000);
    CHECK(dev.listRowCount() == 5);
    CHECK(std::strstr(dev.status(), "memory full") == nullptr);
}

TEST_CASE("DevicesModule: disabling frees the rows, and they fill again when heard") {
    Guard guard;
    DevicesModule dev;
    const uint8_t w1[4] = {192, 0, 2, 11};
    hear(dev, w1, "wled-1", false, 1000);
    REQUIRE(dev.listRowCount() == 1);
    REQUIRE(dev.dynamicBytes() > 0);
    dev.release();
    CHECK(dev.listRowCount() == 0);
    CHECK(dev.dynamicBytes() == 0);
    hear(dev, w1, "wled-1", false, 2000);
    CHECK(dev.listRowCount() == 1);
}
