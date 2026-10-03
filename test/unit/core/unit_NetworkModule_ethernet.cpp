/// @module EthernetModule
/// @also NetworkModule
/// Unit tests for the Ethernet config seam and the cascade around it; the per-PHY bring-up is ESP32-only and verified on hardware.

#include "doctest.h"
#include "platform_config.h"   // EthPhyType, EthPinConfig, hasEthernet, ethConfigDefault
#include "platform/platform.h" // setEthConfig / ethStop / ethInit / ethConnected
#include "core/system/NetworkModule.h"
#include "core/system/EthernetModule.h"
#include "core/system/WiFiModule.h"
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

// Ethernet is opt-in: an unset ethType resolves to ethNone, so a board without a catalog eth block brings up no PHY.
TEST_CASE("Ethernet is opt-in: ethNone is the zero value (unset ethType → no PHY)") {
    CHECK(mm::platform::ethNone == 0);   // an absent/zero ethType control resolves to None
}

// ethInit() returns false on desktop for any config, so NetworkModule cascades to WiFi/AP, and ethStop() is safe when nothing runs.
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

    // Restore the platform default so later tests inherit no shared eth-config state.
    mm::platform::setEthConfig(mm::platform::ethConfigDefault);
}

// Regression: ethPhyAddr is a signed int16 with min -1 (auto-detect), rendered as a number field; as a uint8 it clamped to 31 and the S31 RGMII never linked.
TEST_CASE("ethPhyAddr-style control: signed int16, -1 sentinel in range, number field") {
    mm::ControlList controls;
    int16_t phyAddr = -1;   // ESP_ETH_PHY_ADDR_AUTO, which must survive as -1, not become 255/31
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

// Desktop addressing is OS-managed, so netSetStaticIPv4 and netSetDhcp accept any input and change nothing.
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

// A known network set to Static pins its address during bring-up, since a DHCP-less network never fires a lease event.
TEST_CASE("Static mode pins the static IP during STA bring-up (WaitingSta)") {
    mm::platform::setTestWifiStaAvailable(true);
    {
        mm::NetworkModule net;
        mm::WiFiModule wifi;
        net.addChild(&wifi);
        net.setWiFi(&wifi);
        net.setWifiCredentials("bench-ssid", "bench-pass");
        // The network's own IP settings: Static, with a real address, as the card's row edit sets them.
        constexpr uint32_t kFirstRowId = 1;   // a fresh list numbers its first network 1
        REQUIRE(wifi.setListRowField(kFirstRowId, "ipSettings", "{\"value\":1}"));
        REQUIRE(wifi.setListRowField(kFirstRowId, "ip", "{\"value\":\"192.168.1.250\"}"));
        net.setup();   // desktop ethInit() fails → cascades to STA; the seam lands it in WaitingSta
        uint32_t before = mm::platform::testNetStaticApplyCount(mm::platform::NetIface::Sta);
        net.tick1s();   // WaitingSta: Static + not connected → applyStaStatic()
        CHECK(mm::platform::testNetStaticApplyCount(mm::platform::NetIface::Sta) > before);
        net.removeChild(&wifi);   // the child is a local, not the tree's to free
    }
    mm::platform::setTestWifiStaAvailable(false);   // reset, so cases stay independent
}

namespace {
// Frozen time, so the cascade's timeouts cannot fire on a slow machine; restored when the test ends.
struct FrozenClock {
    FrozenClock() { mm::platform::setTestNowMs(1000); }
    ~FrozenClock() { mm::platform::setTestNowMs(0); }
};

// Switch the wired interface to Static with this address through the normal control-apply path.
void setStatic(mm::EthernetModule& eth, const char* ipJson) {
    for (uint8_t i = 0; i < eth.controls().count(); i++) {
        auto& c = eth.controls()[i];
        if (std::strcmp(c.name, "ipSettings") == 0)
            mm::applyControlValue(c, "{\"ipSettings\":1}", "ipSettings", mm::ApplyPolicy::Clamp);
        else if (std::strcmp(c.name, "ip") == 0)
            mm::applyControlValue(c, ipJson, "ip", mm::ApplyPolicy::Clamp);
    }
}

// A network module with its wired interface, as main wires them.
struct WiredNetwork {
    mm::NetworkModule net;
    mm::EthernetModule eth;
    WiredNetwork() {
        net.addChild(&eth);
        net.setEthernet(&eth);
    }
    ~WiredNetwork() { net.removeChild(&eth); }   // the child is a member, not the tree's to free
};

const char* networkMode(mm::NetworkModule& net) {
    for (uint8_t i = 0; i < net.controls().count(); i++)
        if (std::strcmp(net.controls()[i].name, "mode") == 0) return static_cast<const char*>(net.controls()[i].ptr);
    return "";
}
}  // namespace

// The wired interface has its own IP settings, named as routers and phones name them, DHCP by default with the static fields hidden.
TEST_CASE("Ethernet's IP settings: DHCP by default, Static reveals the fields") {
    if constexpr (!mm::platform::hasEthernet && !mm::platform::previewsEthernetControls) return;
    mm::EthernetModule eth;
    eth.rebuildControls();
    const mm::ControlDescriptor* ipSettings = nullptr;
    const mm::ControlDescriptor* ip = nullptr;
    for (uint8_t i = 0; i < eth.controls().count(); i++) {
        const auto& c = eth.controls()[i];
        if (std::strcmp(c.name, "ipSettings") == 0) ipSettings = &c;
        else if (std::strcmp(c.name, "ip") == 0) ip = &c;
    }
    REQUIRE(ipSettings != nullptr);
    REQUIRE(ip != nullptr);
    CHECK(std::strcmp(reinterpret_cast<const char* const*>(ipSettings->aux)[0], "DHCP") == 0);
    CHECK(ip->hidden);
    CHECK(eth.configuredIp() == nullptr);
    setStatic(eth, "{\"ip\":\"192.168.1.250\"}");
    eth.rebuildControls();
    CHECK(eth.isStatic());
    CHECK(eth.configuredIp()[3] == 250);
}

// A cable on a network without DHCP gives itself 169.254.x.y. The desktop declares WiFi, so it plays the WiFi build: there that address must not end the cascade, and a lease that lands later must.
TEST_CASE("a self-assigned Ethernet address keeps the cascade waiting on a WiFi build, and a lease ends it") {
    const uint8_t linkLocal[4] = {169, 254, 7, 9};
    const uint8_t leased[4]    = {192, 168, 1, 20};
    FrozenClock clock;
    mm::platform::setTestEthIPv4(linkLocal);
    {
        mm::NetworkModule net;
        net.setup();
        net.rebuildControls();   // the scheduler builds them in the running system
        net.tick1s();
        CHECK(std::string(networkMode(net)) == "Ethernet (waiting)");
        mm::platform::setTestEthIPv4(leased);
        net.tick1s();
        CHECK(std::string(networkMode(net)) == "Ethernet");
    }
    mm::platform::setTestEthIPv4(nullptr);   // reset, so cases stay independent
}

// Static mode must still pin the user's address over a self-assigned one, or a leaseless cable keeps 169.254 until reboot.
TEST_CASE("Static mode pins its address over a self-assigned Ethernet one") {
    const uint8_t linkLocal[4] = {169, 254, 7, 9};
    FrozenClock clock;
    mm::platform::setTestEthIPv4(linkLocal);
    {
        WiredNetwork w;
        auto& net = w.net;
        net.setup();
        net.rebuildControls();   // the scheduler builds them in the running system
        setStatic(w.eth, "{\"ip\":\"192.168.1.250\"}");
        uint32_t before = mm::platform::testNetStaticApplyCount(mm::platform::NetIface::Eth);
        net.tick1s();   // WaitingEth: Static, link up, and the address on the wire is not the configured one
        CHECK(mm::platform::testNetStaticApplyCount(mm::platform::NetIface::Eth) > before);
        CHECK(std::string(networkMode(net)) == "Ethernet");   // connected on the user's address
    }
    mm::platform::setTestEthIPv4(nullptr);
}

// Editing a static link-local address applies before the cascade judges the link, or the old address reads as lost and the device drops to its access point.
TEST_CASE("changing a static link-local address keeps Ethernet connected") {
    const uint8_t linkLocal[4] = {169, 254, 7, 9};
    FrozenClock clock;
    mm::platform::setTestEthIPv4(linkLocal);
    {
        WiredNetwork w;   // no WiFi credentials, so a drop would land on the access point
        auto& net = w.net;
        net.setup();
        net.rebuildControls();
        setStatic(w.eth, "{\"ip\":\"169.254.7.9\"}");
        net.tick1s();
        REQUIRE(std::string(networkMode(net)) == "Ethernet");
        setStatic(w.eth, "{\"ip\":\"169.254.7.10\"}");
        uint32_t before = mm::platform::testNetStaticApplyCount(mm::platform::NetIface::Eth);
        net.tick1s();
        CHECK(mm::platform::testNetStaticApplyCount(mm::platform::NetIface::Eth) > before);
        CHECK(std::string(networkMode(net)) == "Ethernet");
    }
    mm::platform::setTestEthIPv4(nullptr);
}

// fixedPins stays inside its capacity; the applied-versus-pending half needs hardware, since hasEthernet is false here.
TEST_CASE("fixedPins never writes past the capacity it is given") {
    mm::EthernetModule eth;
    eth.syncConfig();
    mm::MoonModule::FixedPin pads[16];
    // A sentinel past the capacity: the collector passes a real buffer size and a module that wrote beyond it would corrupt the stack frame above.
    pads[2].gpio = 0xEE;
    CHECK(eth.fixedPins(pads, 2) <= 2);
    CHECK(pads[2].gpio == 0xEE);
    CHECK(eth.fixedPins(nullptr, 16) == 0);   // a null sink is answered, not written through
}

// Two bugs an Olimex found: a board whose pins ARE a preset's map opens on it, and one that chose nothing adopts no preset.
TEST_CASE("ethBoard seeds from the pins after a restore, not once before it") {
    if constexpr (!mm::platform::hasEthernet && !mm::platform::previewsEthernetControls) return;

    auto boardOf = [](mm::EthernetModule& n) -> const char* {
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

    mm::EthernetModule net;
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

    mm::EthernetModule net;
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

    // The preset's map must reach the fields, which the selection alone does not prove.
    if (options == 2) {
        for (uint8_t i = 0; i < net.controls().count(); i++) {
            const auto& c = net.controls()[i];
            if (std::strcmp(c.name, "ethType") != 0) continue;
            CHECK(*static_cast<const uint8_t*>(c.ptr) != 0);   // not ethNone
            break;
        }
    }
}
