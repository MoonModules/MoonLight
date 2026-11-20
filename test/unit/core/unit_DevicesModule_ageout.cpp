/// @module DevicesModule
///
/// Pins the age-out: a device whose presence packets stop arriving is dropped once kStaleMs passes.
///
/// @technical
///
/// ## One window, because every row has been heard
///
/// A row exists only once its device is heard, so every non-self row gets the full kStaleMs; the self row stays, which shows the check discriminates.
///
/// ## How the test reaches it
///
/// Virtual time (platform::setTestNowMs) drives it: the test hears a presence packet, advances time and ticks, and listRowCount() says which rows survived.

#include "doctest.h"
#include "discovery_rows.h"
#include "core/system/DevicesModule.h"
#include "core/util/JsonSink.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstring>
#include <string>

using namespace mm;

namespace {

// RAII: restore the real clock on scope exit, even if a REQUIRE/CHECK throws mid-test, otherwise a failing assertion would leave virtual time frozen for later cases.
struct ClockGuard { ~ClockGuard() { platform::setTestNowMs(0); } };

// Hear a device at 192.168.1.20 (this device itself when `isSelf`) at t0, advance to t0+dt and tick once, returning whether its row survived.
bool heardRowPresentAfter(uint32_t t0, uint32_t dt, bool isSelf = false) {
    ClockGuard guard;   // real clock restored on return, even if a REQUIRE below fails
    // Discovery runs only once the device has an address, so give it one rather than depend on the host being online.
    struct AddressGuard { ~AddressGuard() { platform::setTestEthIPv4(nullptr); } } address;
    const uint8_t self[4] = {192, 168, 1, static_cast<uint8_t>(isSelf ? 20 : 2)};
    platform::setTestEthIPv4(self);
    platform::setTestNowMs(t0);
    DevicesModule dev;
    const uint8_t ip[4] = {192, 168, 1, 20};
    discovery_rows::hearPresence(dev, ip, "A", /*mm=*/false);
    REQUIRE_FALSE(discovery_rows::rowAt(dev, "192.168.1.20").empty());
    platform::setTestNowMs(t0 + dt);
    dev.tick1s();                                                    // runs ageOut() against the advanced clock
    return !discovery_rows::rowAt(dev, "192.168.1.20").empty();
}

}  // namespace

TEST_CASE("DevicesModule: a heard device survives just under 24h unheard") {
    CHECK(heardRowPresentAfter(1000, 23u * 60u * 60u * 1000u) == true);
}

TEST_CASE("DevicesModule: a heard device drops once past kStaleMs (24h) unheard") {
    CHECK(heardRowPresentAfter(1000, 25u * 60u * 60u * 1000u) == false);
}

TEST_CASE("DevicesModule: this device's own row never ages out") {
    CHECK(heardRowPresentAfter(1000, 25u * 60u * 60u * 1000u, /*isSelf=*/true) == true);
}

// A peer also answers as a plain WLED, so a later WLED sighting must not relabel a MoonLight row: this drives upsertDevice's downgrade-prevention through the public path.
TEST_CASE("DevicesModule: a MoonLight device is not downgraded by a WLED packet") {
    ClockGuard guard;
    platform::setTestNowMs(1);
    DevicesModule dev;
    const uint8_t ip[4] = {192, 168, 1, 30};
    discovery_rows::hearPresence(dev, ip, "MM-Bench", /*mm=*/true);
    REQUIRE(dev.listRowCount() == 1);

    // A later plain WLED presence packet (no MoonLight marker) from the SAME address.
    discovery_rows::hearPresence(dev, ip, "MM-Bench", /*mm=*/false);

    // Still MoonLight, upsertDevice only RAISES toward MoonLight, never downgrades.
    const std::string row = discovery_rows::rowAt(dev, "192.168.1.30");
    CHECK(row.find("\"type\":\"MoonLight\"") != std::string::npos);
    CHECK(row.find("\"type\":\"WLED\"") == std::string::npos);
}

// The bench case: boards heard on a guest network stayed listed at its addresses for a day after the desktop moved back to the main one.
TEST_CASE("DevicesModule: a new address of its own drops the rows learned on the old network") {
    ClockGuard guard;
    struct AddressGuard { ~AddressGuard() { platform::setTestEthIPv4(nullptr); } } address;
    platform::setTestNowMs(1000);
    const uint8_t guest[4] = {192, 168, 9, 151};
    platform::setTestEthIPv4(guest);
    DevicesModule dev;
    dev.tick1s();
    const uint8_t board[4] = {192, 168, 9, 137};
    discovery_rows::hearPresence(dev, board, "WLED0-0", /*mm=*/false);
    REQUIRE_FALSE(discovery_rows::rowAt(dev, "192.168.9.137").empty());

    const uint8_t home[4] = {192, 168, 1, 151};
    platform::setTestEthIPv4(home);
    dev.tick1s();
    CHECK(discovery_rows::rowAt(dev, "192.168.9.137").empty());
    CHECK_FALSE(discovery_rows::rowAt(dev, "192.168.1.151").empty());   // this device's own row moves with it

    const uint8_t same[4] = {192, 168, 1, 137};
    discovery_rows::hearPresence(dev, same, "WLED0-0", /*mm=*/false);
    dev.tick1s();
    CHECK_FALSE(discovery_rows::rowAt(dev, "192.168.1.137").empty());   // the new network's devices stay
}

// The bench case: a board renamed SS-0-1 still listed itself as MM-33FC, so its group showed the old name.
TEST_CASE("DevicesModule: this device's own row follows a rename on the next tick") {
    ClockGuard guard;
    struct AddressGuard { ~AddressGuard() { platform::setTestEthIPv4(nullptr); } } address;
    platform::setTestNowMs(1000);
    const uint8_t self[4] = {192, 168, 1, 235};
    platform::setTestEthIPv4(self);
    char name[24] = "MM-33FC";
    DevicesModule dev;
    dev.setSelfName(name);
    dev.tick1s();
    CHECK(discovery_rows::rowAt(dev, "192.168.1.235").find("\"name\":\"MM-33FC\"") != std::string::npos);
    std::strcpy(name, "SS-0-1");
    dev.tick1s();
    CHECK(discovery_rows::rowAt(dev, "192.168.1.235").find("\"name\":\"SS-0-1\"") != std::string::npos);
}
