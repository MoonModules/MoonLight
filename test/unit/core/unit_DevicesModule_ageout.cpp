/// @module DevicesModule
///
/// Pins the age-out: a device whose presence packets stop arriving is dropped once its window passes.
///
/// @moreinfo
///
/// ## Two windows, because a restored row has not been heard
///
/// A cached or restored row is on a short probation, where a live-confirmed one gets the full kStaleMs.
/// Each presence packet stamps lastSeenMs, so the drop is an unheard-too-long check rather than a timer.
/// A still-fresh device and the self row stay, which is what says the check discriminates.
///
/// ## How the test reaches it
///
/// Virtual time through platform::setTestNowMs drives it deterministically, with no network and no wall clock.
/// The age-out runs in tick1s(), so the test restores a cached list through the public persistence entry, advances time, and ticks.
/// listRowCount() then says which rows survived.
/// The UDP listener that tick1s() drains is inert here since nothing sends to the host bind, and the self row is registered against the host's own address.

#include "doctest.h"
#include "core/system/DevicesModule.h"
#include "core/system/WledPacket.h"
#include "core/util/JsonSink.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstring>

using namespace mm;

namespace {

// RAII: restore the real clock on scope exit, even if a REQUIRE/CHECK throws mid-test, otherwise a failing assertion would leave virtual time frozen for later cases.
struct ClockGuard { ~ClockGuard() { platform::setTestNowMs(0); } };

// True if the cached device at `ip` is still in the list (a serialized-row scan, the only public window into the rows). The self entry the live tick adds is ignored, we assert specifically on the two restored, non-self devices' survival.
bool present(const DevicesModule& dev, const char* ip) {
    for (uint8_t i = 0; i < dev.listRowCount(); i++) {
        mm::JsonSink sink;
        dev.writeListRow(sink, i);
        if (std::strstr(sink.data(), ip)) return true;
    }
    return false;
}

// Restore two cached devices, optionally re-confirm A with a live packet, advance to t0+dt and tick once, returning whether A survived.
bool aPresentAfter(uint32_t t0, uint32_t dt, bool reconfirmA) {
    ClockGuard guard;   // real clock restored on return, even if a REQUIRE below fails
    platform::setTestNowMs(t0);
    DevicesModule dev;
    // Mirrors the persistence-overlay shape: the whole module JSON object, the array under the control's key, restoreList navigates `member(root, "devices")`.
    const char* cached =
        "{\"devices\":["
        "{\"name\":\"A\",\"ip\":\"192.168.1.20\",\"type\":\"WLED\"},"
        "{\"name\":\"B\",\"ip\":\"192.168.1.21\",\"type\":\"WLED\"}]}";
    REQUIRE(dev.restoreList(cached, "devices"));
    REQUIRE(dev.listRowCount() == 2);
    if (reconfirmA) {
        // A live WLED presence packet from A's IP, clears `cached`, gives it the 24 h window.
        const uint8_t ip[4] = {192, 168, 1, 20};
        uint8_t pkt[WledPacket::kSize];
        WledPacket::build(pkt, ip, "A", /*boardType=*/34, /*lightsOn=*/true);
        dev.injectPacketForTest(pkt, sizeof(pkt), ip);
    }
    platform::setTestNowMs(t0 + dt);
    dev.tick1s();                           // runs ageOut() against the advanced clock
    return present(dev, "192.168.1.20");    // ClockGuard restores the real clock on return
}

}  // namespace

// A restored-but-never-re-heard device drops past kCachedGraceMs, since its clock resets each restore and it would otherwise outlive every reboot.
TEST_CASE("DevicesModule: a cached device survives just under the probation window") {
    CHECK(aPresentAfter(1000, 50u * 1000u, /*reconfirmA=*/false) == true);   // 50s < 60s probation
}

TEST_CASE("DevicesModule: a cached device drops once past the probation window") {
    CHECK(aPresentAfter(1000, 70u * 1000u, /*reconfirmA=*/false) == false);  // 70s > 60s probation
}

// A live-confirmed device (a presence packet cleared its `cached` flag) gets the full 24 h.
TEST_CASE("DevicesModule: a live-confirmed device survives well past probation (to 24h)") {
    CHECK(aPresentAfter(1000, 23u * 60u * 60u * 1000u, /*reconfirmA=*/true) == true);   // 23h < 24h
}

TEST_CASE("DevicesModule: a live-confirmed device drops once past kStaleMs (24h)") {
    CHECK(aPresentAfter(1000, 25u * 60u * 60u * 1000u, /*reconfirmA=*/true) == false);  // 25h > 24h
}

// A peer also answers as a plain WLED, so a later WLED sighting must not relabel a restored row: this drives upsertDevice's downgrade-prevention through the public path.
TEST_CASE("DevicesModule: a restored MoonLight device is not downgraded by a WLED packet") {
    ClockGuard guard;
    platform::setTestNowMs(1);
    DevicesModule dev;
    const char* cached =
        "{\"devices\":[{\"name\":\"MM-Bench\",\"ip\":\"192.168.1.30\",\"type\":\"MoonLight\"}]}";
    REQUIRE(dev.restoreList(cached, "devices"));
    REQUIRE(dev.listRowCount() == 1);

    // A later plain WLED presence packet (no MoonLight marker) from the SAME address.
    const uint8_t ip[4] = {192, 168, 1, 30};
    uint8_t pkt[WledPacket::kSize];
    WledPacket::build(pkt, ip, "MM-Bench", /*boardType=*/34, /*lightsOn=*/true);  // unmarked = WLED
    dev.injectPacketForTest(pkt, sizeof(pkt), ip);

    // Still MoonLight, upsertDevice only RAISES toward MoonLight, never downgrades.
    mm::JsonSink sink;
    dev.writeListRow(sink, 0);
    CHECK(std::strstr(sink.data(), "\"type\":\"MoonLight\"") != nullptr);
    CHECK(std::strstr(sink.data(), "\"type\":\"WLED\"") == nullptr);
}

TEST_CASE("DevicesModule: restore tolerates an empty / malformed cache") {
    // Robustness: a malformed array yields an empty list, never a crash.
    ClockGuard guard;   // restores the real clock on return, even if a CHECK fails
    platform::setTestNowMs(1);
    DevicesModule dev;
    CHECK(dev.restoreList("not json", "devices") == false);
    CHECK(dev.listRowCount() == 0);
}
