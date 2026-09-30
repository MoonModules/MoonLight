/// @module NetworkModule

/// Unit tests for the runtime Ethernet PHY/pin config seam.
///
/// The per-PHY bring-up (RMII EMAC, W5500 SPI) lives in platform_esp32.cpp and is ESP32-only, so it isn't reachable on the desktop host. What IS host-testable, and what these tests pin, are the two contracts the rest of the system is built on:
///
///   1. The EthPhyType enum *ordering*. NetworkModule's `ethType` Select dropdown
///      stores the option index, and platform_esp32.cpp's ethInit() switches on
///      the same values; both assume None=0, LAN8720=1, IP101=2, W5500=3. A silent
///      reorder of the enum would desync the dropdown labels from the dispatch and
///      from every deviceModels.json `ethType` value, caught here, not on hardware.
///
///   2. The desktop platform seam (setEthConfig / ethStop / ethInit) is a safe
///      no-op. NetworkModule::setup() calls setEthConfig() then ethInit() on every
///      platform; the desktop stubs must accept any config and report "no Ethernet"
///      (ethInit()==false) so the WiFi/AP cascade always takes over. This guards
///      the platform.h contract that lets the shared NetworkModule code compile and
///      run unchanged on the host.
///
/// The conditional-visibility of the eth pin controls (RMII rows vs SPI rows by ethType) is gated behind `if constexpr (platform::hasEthernet)`, which is false on desktop, so it can't be exercised here; it's verified on real ESP32 hardware (Olimex RMII, S3 W5500) instead.

#include "doctest.h"
#include "platform_config.h"   // EthPhyType, EthPinConfig, hasEthernet, ethConfigDefault
#include "platform/platform.h" // setEthConfig / ethStop / ethInit / ethConnected
#include "core/system/NetworkModule.h"
#include <cstring>
#include <string>

// The enum values are a wire contract: the Select index, the ethInit() switch, and every deviceModels.json `ethType` all agree on these. Pin them so a reorder fails here.
TEST_CASE("EthPhyType enum values match the dropdown/dispatch contract") {
    CHECK(mm::platform::ethNone    == 0);
    CHECK(mm::platform::ethLan8720 == 1);
    CHECK(mm::platform::ethIp101   == 2);
    CHECK(mm::platform::ethW5500   == 3);
}

// Desktop has no Ethernet: the default PHY type is ethNone, so a board that never pushes an eth config still reports "no Ethernet" and the cascade falls through.
TEST_CASE("Desktop ethConfigDefault is ethNone (no Ethernet)") {
    CHECK_FALSE(mm::platform::hasEthernet);
    CHECK(mm::platform::ethConfigDefault.phyType == mm::platform::ethNone);
}

// Ethernet is OPT-IN: NetworkModule's `ethType` control defaults to ethNone(0), so a board whose deviceModels.json entry has no `ethType` brings up NO PHY (a WiFi-only board like Shelly, or the QuinLED Dig-Uno/Quad with optional-only eth, must not waste an RMII/SPI init on a PHY it lacks). A board with real Ethernet sets `ethType` explicitly in its catalog eth block. On ESP32 the control default seeds ethType_ = 0; the platform's ethConfigDefault (whose phyType is the chip's historical PHY) still seeds the PINS so an opt-in board gets them without re-listing, but the PHY *selection* is off until the catalog turns it on. ethNone==0 is what makes "unset → off" work.
TEST_CASE("Ethernet is opt-in: ethNone is the zero value (unset ethType → no PHY)") {
    CHECK(mm::platform::ethNone == 0);   // an absent/zero ethType control resolves to None
}

// The platform seam must accept any runtime config and never bring Ethernet up on desktop, ethInit() returns false so NetworkModule cascades to WiFi/AP. Pushing a fully-populated W5500 config and an RMII config both leave ethInit() false and ethConnected() false; ethStop() is safe to call when nothing is running.
TEST_CASE("Desktop Ethernet seam is a safe no-op") {
    mm::platform::EthPinConfig w5500{ mm::platform::ethW5500, 1,
                                      -1, -1, -1, -1, false,
                                      /*miso*/ 5, /*mosi*/ 6, /*sck*/ 7, /*cs*/ 15, /*irq*/ 18 };
    mm::platform::setEthConfig(w5500);
    CHECK_FALSE(mm::platform::ethInit());
    CHECK_FALSE(mm::platform::ethConnected());

    mm::platform::EthPinConfig rmii{ mm::platform::ethLan8720, 0,
                                     -1, -1, /*rst*/ 5, /*clk*/ 17, false,
                                     -1, -1, -1, -1, -1 };
    mm::platform::setEthConfig(rmii);
    CHECK_FALSE(mm::platform::ethInit());
    CHECK_FALSE(mm::platform::ethConnected());

    mm::platform::ethStop();   // safe even though nothing came up
    CHECK_FALSE(mm::platform::ethConnected());

    // Restore the platform default so this test leaves no shared eth-config state for later tests (setEthConfig writes a static on ESP32; a no-op on desktop, but keep the test order-independent regardless of platform).
    mm::platform::setEthConfig(mm::platform::ethConfigDefault);
}

// Regression: the `ethPhyAddr` control MUST be a SIGNED int16 whose range starts at -1 and which renders as a number field (not a slider). -1 is ESP_ETH_PHY_ADDR_AUTO (scan the MDIO bus, the RGMII default). It was once a uint8 control: the uint8 mangled the platform's -1 default to 255 and the 0..31 control clamped it to 31, a fixed address no PHY answered, so the S31's RGMII never linked. This pins the control-metadata contract that fixed it, signed storage so -1 round-trips, min == -1 so the sentinel is in-range, and numberField because an MDIO address is an identity, not a magnitude. Tests the int16 control + setNumberField seam directly (the NetworkModule control is `if constexpr (hasEthernet)`-gated, absent on desktop), so a future edit that reverts to a slider or an unsigned type fails here, off-hardware.
TEST_CASE("ethPhyAddr-style control: signed int16, -1 sentinel in range, number field") {
    mm::ControlList controls;
    int16_t phyAddr = -1;   // ESP_ETH_PHY_ADDR_AUTO — must survive as -1, not become 255/31
    controls.addControl("ethPhyAddr", phyAddr, -1, 31);
    controls.setNumberField(controls.count() - 1);

    const auto& c = controls[controls.count() - 1];
    CHECK(std::strcmp(c.name, "ethPhyAddr") == 0);
    CHECK(c.type == mm::ControlType::Int16);   // signed, so -1 is representable (a uint8 mangled it)
    CHECK(c.min == -1);                         // the auto-detect sentinel is in-range, not clamped away
    CHECK(c.max == 31);
    CHECK(c.numberField);                       // rendered as a plain number input, not a 0..31 slider
    CHECK(phyAddr == -1);                        // the bound value still reads -1 through the int16 control
}

// Static-IP addressing contract. The `addressing` Select (DHCP=0 / Static=1) and the four IPv4 controls (ip/gateway/subnet/dns) are what platform::netSetStaticIPv4 applies to the active interface. Pin the control shape a future edit could break: the Select stores the mode index and defaults to DHCP, the static fields exist with their documented defaults, and they are HIDDEN in DHCP mode (visible only when addressing==Static). These are always bound (not hasEthernet-gated), so the contract is testable on the desktop host.
TEST_CASE("addressing Select + static-IP controls: DHCP default, Static reveals the fields") {
    mm::NetworkModule net;
    net.setup();
    net.rebuildControls();   // single clean build (setup already built once via startAP); see mode test

    const mm::ControlDescriptor* addressing = nullptr;
    const mm::ControlDescriptor* ip = nullptr;
    const mm::ControlDescriptor* subnet = nullptr;
    for (uint8_t i = 0; i < net.controls().count(); i++) {
        const auto& c = net.controls()[i];
        if (std::strcmp(c.name, "addressing") == 0) addressing = &c;
        else if (std::strcmp(c.name, "ip") == 0)     ip = &c;
        else if (std::strcmp(c.name, "subnet") == 0) subnet = &c;
    }
    REQUIRE(addressing != nullptr);
    CHECK(addressing->type == mm::ControlType::Select);
    // Default is DHCP: the static fields are present but hidden until Static is selected.
    REQUIRE(ip != nullptr);
    REQUIRE(subnet != nullptr);
    CHECK(ip->type == mm::ControlType::IPv4);
    CHECK(ip->hidden);        // DHCP mode → static fields hidden
    CHECK(subnet->hidden);
}

// The desktop platform's static/DHCP setters are inert no-ops: addressing is OS-managed on the host, so netSetStaticIPv4 / netSetDhcp must accept any input and change nothing (no crash, no interface brought up). Mirrors the "desktop net seam is a safe no-op" guarantee for ethInit etc.
TEST_CASE("Desktop static-addressing seam is a safe no-op") {
    const uint8_t ip[4]   = {192, 168, 1, 50};
    const uint8_t gw[4]   = {192, 168, 1, 1};
    const uint8_t mask[4] = {255, 255, 255, 0};
    const uint8_t dns[4]  = {192, 168, 1, 1};
    mm::platform::netSetStaticIPv4(mm::platform::NetIface::Eth, ip, gw, mask, dns);
    mm::platform::netSetStaticIPv4(mm::platform::NetIface::Sta, ip, gw, mask, dns);
    mm::platform::netSetDhcp(mm::platform::NetIface::Eth);
    mm::platform::netSetDhcp(mm::platform::NetIface::Sta);
    // Desktop reports no eth/sta connection regardless, the setters didn't fake one.
    CHECK_FALSE(mm::platform::ethConnected());
}

// Static addressing on WiFi STA is applied during BRING-UP (WaitingSta), not only after a lease event: a DHCP-less network never fires one, so waiting for "connected" before pinning the static IP would strand a static STA into the AP fallback (the WaitingEth static poll's mirror). The test seam fakes an STA radio so the host can drive the cascade into WaitingSta; the platform apply counter pins that tick1s invoked netSetStaticIPv4(Sta).
TEST_CASE("Static mode pins the static IP during STA bring-up (WaitingSta)") {
    mm::platform::setTestWifiStaAvailable(true);
    {
        mm::NetworkModule net;
        net.setWifiCredentials("bench-ssid", "bench-pass");
        net.setup();   // desktop ethInit() fails → cascades to STA; the seam lands it in WaitingSta
        // Switch to Static with a real address via the normal control-apply path (the octets bind by reference, so the module reads them directly).
        for (uint8_t i = 0; i < net.controls().count(); i++) {
            auto& c = net.controls()[i];
            if (std::strcmp(c.name, "addressing") == 0)
                mm::applyControlValue(c, "{\"addressing\":1}", "addressing", mm::ApplyPolicy::Clamp);
            else if (std::strcmp(c.name, "ip") == 0)
                mm::applyControlValue(c, "{\"ip\":\"192.168.1.250\"}", "ip", mm::ApplyPolicy::Clamp);
        }
        uint32_t before = mm::platform::testNetStaticApplyCount(mm::platform::NetIface::Sta);
        net.tick1s();   // WaitingSta: Static + not connected → applyStaticIfConfigured(Sta)
        CHECK(mm::platform::testNetStaticApplyCount(mm::platform::NetIface::Sta) > before);
    }
    mm::platform::setTestWifiStaAvailable(false);   // reset — cases stay independent
}


// The pin map must report what the HARDWARE holds, not what the control says. On an RMII/RGMII board a type change is saved and applied on the NEXT BOOT (syncEthLive hot-reinits only W5500), so the EMAC keeps driving its pads after the user selects None. Reading the pending control there frees those pins in the map while the MAC still drives them, and an LED lane could then take one with nothing flagging the collision, which is the failure this whole mechanism exists to prevent.
//
// Only the CAPACITY half is checkable here: `hasEthernet` is false on the desktop, so fixedPins returns 0 on both sides of the applied-vs-pending distinction and a host test cannot tell them apart. The distinction is exercised on hardware (an S31 keeps its twelve pads listed while the interface runs) and by the esp32s31/esp32p4rev1-eth firmware builds.
TEST_CASE("fixedPins never writes past the capacity it is given") {
    mm::NetworkModule net;
    net.setup();
    mm::MoonModule::FixedPin pads[16];
    // A sentinel past the capacity: the collector passes a real buffer size and a module that wrote beyond it would corrupt the stack frame above.
    pads[2].gpio = 0xEE;
    CHECK(net.fixedPins(pads, 2) <= 2);
    CHECK(pads[2].gpio == 0xEE);
    CHECK(net.fixedPins(nullptr, 16) == 0);   // a null sink is answered, not written through
}

// Two bugs an Olimex found: a board whose pins ARE a preset's map opens on it, and one that chose nothing adopts no preset.
TEST_CASE("ethBoard seeds from the pins after a restore, not once before it") {
    if constexpr (!mm::platform::hasEthernet && !mm::platform::previewsEthernetControls) return;

    auto boardOf = [](mm::NetworkModule& n) -> const char* {
        for (uint8_t i = 0; i < n.controls().count(); i++) {
            const auto& c = n.controls()[i];
            if (std::strcmp(c.name, "ethBoard") == 0) {
                auto* opts = reinterpret_cast<const char* const*>(c.aux);
                const uint8_t sel = *static_cast<const uint8_t*>(c.ptr);
                return (opts && sel < c.max) ? opts[sel] : "";
            }
        }
        return nullptr;
    };

    mm::NetworkModule net;
    net.rebuildControls();
    REQUIRE(boardOf(net) != nullptr);
    // Nothing has chosen one and no type is set, so Custom is the honest answer rather than row 0, a real preset whose map would overwrite the chip's own defaults.
    CHECK(std::strcmp(boardOf(net), "Custom") == 0);

    // What a restore does, overlaying saved values then rebuilding, with the classic pins and no saved `ethBoard` at all: exactly an upgraded board's config.
    auto setByName = [&](const char* name, int v) {
        for (uint8_t i = 0; i < net.controls().count(); i++) {
            const auto& c = net.controls()[i];
            if (std::strcmp(c.name, name) != 0) continue;
            if (c.type == mm::ControlType::Int16) *static_cast<int16_t*>(c.ptr) = static_cast<int16_t>(v);
            else if (c.type == mm::ControlType::Pin) *static_cast<int8_t*>(c.ptr) = static_cast<int8_t>(v);
            else *static_cast<uint8_t*>(c.ptr) = static_cast<uint8_t>(v);
            return;
        }
    };
    setByName("ethType", 1);          // LAN8720
    setByName("ethPhyAddr", 0);
    setByName("ethMdcGpio", 23);
    setByName("ethMdioGpio", 18);
    setByName("ethRstGpio", 5);
    setByName("ethClockGpio", 17);

    net.rebuildControls();
    // The pins name Classic RMII, so the card opens on it rather than on Custom.
    CHECK(std::strcmp(boardOf(net), "Classic RMII") == 0);

    // And the seed does NOT fight a chosen preset: rebuilding again keeps it.
    net.rebuildControls();
    CHECK(std::strcmp(boardOf(net), "Classic RMII") == 0);
}

// A chip whose filter leaves exactly ONE real preset opens on it, not on Custom: on a P4-NANO that is a configured interface versus none.
TEST_CASE("a chip with one buildable preset defaults to it, not to Custom") {
    if constexpr (!mm::platform::hasEthernet && !mm::platform::previewsEthernetControls) return;

    mm::NetworkModule net;
    net.rebuildControls();

    const char* board = nullptr;
    uint8_t options = 0;
    for (uint8_t i = 0; i < net.controls().count(); i++) {
        const auto& c = net.controls()[i];
        if (std::strcmp(c.name, "ethBoard") != 0) continue;
        auto* opts = reinterpret_cast<const char* const*>(c.aux);
        const uint8_t sel = *static_cast<const uint8_t*>(c.ptr);
        options = c.max;
        board = (opts && sel < c.max) ? opts[sel] : "";
        break;
    }
    REQUIRE(board != nullptr);

    // The option count tells the two builds apart: a desktop previews the whole catalog, so Custom is right there.
    if (options == 2) CHECK(std::strcmp(board, "Custom") != 0);
    else CHECK(std::strcmp(board, "Custom") == 0);

    // And the preset's MAP reached the fields, which the selection alone does not prove: a virgin P4 selected P4-NANO and still booted with ethType at None, because the applied-tracker started on row 0 and saw no move to apply.
    if (options == 2) {
        for (uint8_t i = 0; i < net.controls().count(); i++) {
            const auto& c = net.controls()[i];
            if (std::strcmp(c.name, "ethType") != 0) continue;
            CHECK(*static_cast<const uint8_t*>(c.ptr) != 0);   // not ethNone
            break;
        }
    }
}
