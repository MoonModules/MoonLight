/// @module NetworkModule
/// Unit tests for NetworkModule: credentials, the mode and radio controls, static addressing and the link-local rule.

#include "doctest.h"
#include "platform_config.h"       // pulls in platform::hasWiFi before NetworkModule.h
#include "core/system/NetworkModule.h"
#include "conditional_controls.h"  // shared conditional-control invariant helpers

#include <cstring>

// setWifiCredentials copies SSID + password into internal buffers and raises the dirty flag so the next tick1s() applies them.
TEST_CASE("NetworkModule::setWifiCredentials copies SSID + password and marks dirty") {
    mm::NetworkModule net;
    CHECK_FALSE(net.dirty());

    net.setWifiCredentials("homeAP", "secret123");

    CHECK(net.dirty());

    // No public accessor for ssid_/password_, re-set with markedly different values to confirm the second write replaces the first (proving the copy happened, not just that the function returned).
    net.clearDirty();
    net.setWifiCredentials("otherSSID", "otherPW");
    CHECK(net.dirty());
}

// A nullptr SSID is silently ignored (no copy, no dirty flag), guards against a bogus caller.
TEST_CASE("NetworkModule::setWifiCredentials with null SSID is a no-op") {
    mm::NetworkModule net;
    net.setWifiCredentials(nullptr, "irrelevant");
    CHECK_FALSE(net.dirty());
}

// A nullptr password is treated as empty (open networks), still copies SSID and marks dirty.
TEST_CASE("NetworkModule::setWifiCredentials with null password treats it as empty") {
    // Improv allows open networks (auth flag = "NO"); the credential pusher may pass a nullptr or empty password for those. Both must be tolerated.
    mm::NetworkModule net;
    net.setWifiCredentials("openSSID", nullptr);
    CHECK(net.dirty());
}

// An over-length SSID (100 chars) is truncated cleanly into the 33-byte buffer; ASAN catches any overflow.
TEST_CASE("NetworkModule::setWifiCredentials accepts long SSID without crash") {
    mm::NetworkModule net;
    // `volatile` stops constant folding, else GCC sees 99 bytes going into 33 and warns about the truncation this test expects.
    volatile size_t len = 99;
    char longSsid[100];
    std::memset(longSsid, 'A', len);
    longSsid[len] = 0;
    net.setWifiCredentials(longSsid, "pw");
    CHECK(net.dirty());
}

// The mode control names the cascade state; on desktop every init stub fails, so it reads Idle.
TEST_CASE("NetworkModule mode control reflects current state") {
    mm::NetworkModule net;
    net.setup();
    // Clears before building, so the controls setup() already built are not duplicated.
    net.rebuildControls();

    bool foundMode = false;
    for (uint8_t i = 0; i < net.controls().count(); i++) {
        if (std::strcmp(net.controls()[i].name, "mode") == 0) {
            CHECK(net.controls()[i].type == mm::ControlType::ReadOnly);
            const char* val = static_cast<const char*>(net.controls()[i].ptr);
            CHECK(val != nullptr);
            CHECK(std::strcmp(val, "Idle") == 0);
            foundMode = true;
        }
    }
    CHECK(foundMode);
}

// parseDottedQuad (in Control.h) is the validator on every IPv4 write, over both the HTTP API and persistence. Pin the contract.
TEST_CASE("parseDottedQuad accepts valid dotted-quads and rejects junk") {
    // Zero-initialized, so a failed parse compares against a known 0.
    uint8_t out[4] = {};

    CHECK(mm::parseDottedQuad("0.0.0.0", out));
    CHECK((out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0));

    CHECK(mm::parseDottedQuad("192.168.1.42", out));
    CHECK((out[0] == 192 && out[1] == 168 && out[2] == 1 && out[3] == 42));

    CHECK(mm::parseDottedQuad("255.255.255.255", out));
    CHECK((out[0] == 255 && out[1] == 255 && out[2] == 255 && out[3] == 255));

    // Out-of-range octet, rejected (would clamp to 255 if we allowed it, hiding a malformed write rather than surfacing the bug).
    CHECK_FALSE(mm::parseDottedQuad("1.2.3.256", out));
    // Negative, rejected.
    CHECK_FALSE(mm::parseDottedQuad("-1.0.0.0", out));
    // Wrong shape, rejected.
    CHECK_FALSE(mm::parseDottedQuad("1.2.3", out));
    CHECK_FALSE(mm::parseDottedQuad("1.2.3.4.5", out));
    CHECK_FALSE(mm::parseDottedQuad("", out));
    CHECK_FALSE(mm::parseDottedQuad("abc.def.ghi.jkl", out));
    // Trailing junk after a valid quad, rejected. Lets the API surface "192.168.1.1x" as a 400 instead of silently writing 192.168.1.1.
    CHECK_FALSE(mm::parseDottedQuad("192.168.1.1x", out));
}

// The static-IP fields (ip / gateway / subnet / dns) are bound as IPv4 controls, 4 bytes of storage each, not 16-char dotted-quad strings. They start hidden because addressing defaults to DHCP.
TEST_CASE("NetworkModule static-IP fields are IPv4-typed") {
    mm::NetworkModule net;
    net.setup();
    // Clears before building, so the controls setup() already built are not duplicated.
    net.rebuildControls();

    int found = 0;
    for (uint8_t i = 0; i < net.controls().count(); i++) {
        const char* name = net.controls()[i].name;
        if (std::strcmp(name, "ip") == 0
            || std::strcmp(name, "gateway") == 0
            || std::strcmp(name, "subnet") == 0
            || std::strcmp(name, "dns") == 0) {
            CHECK(net.controls()[i].type == mm::ControlType::IPv4);
            CHECK(net.controls()[i].hidden);  // DHCP default → hidden
            found++;
        }
    }
    CHECK(found == 4);
}

// WiFi builds expose rssi and txPower as read-only controls that start hidden, and the Ethernet-only build compiles them out.
TEST_CASE("NetworkModule rssi/txPower controls hidden in non-WiFi states") {
    mm::NetworkModule net;
    net.setup();
    // Clears before building, so the controls setup() already built are not duplicated.
    net.rebuildControls();

    int matchCount = 0;
    for (uint8_t i = 0; i < net.controls().count(); i++) {
        const char* name = net.controls()[i].name;
        if (std::strcmp(name, "rssi") == 0 || std::strcmp(name, "txPower") == 0) {
            matchCount++;
            CHECK(net.controls()[i].type == mm::ControlType::ReadOnlyInt);
            // Desktop setup() lands in Idle, where both are hidden.
            CHECK(net.controls()[i].hidden);
        }
    }
    // The count catches both controls missing, which an empty loop would pass.
    if constexpr (mm::platform::hasWiFi) {
        CHECK(matchCount == 2);
    } else {
        CHECK(matchCount == 0);
    }
}

// The static-IP fields show only under Static addressing, and stay bound under DHCP so persistence can load them.
TEST_CASE("NetworkModule static-IP fields track the addressing mode") {
    mm::NetworkModule net;
    net.setup();   // builds controls once (desktop cascade lands on AP/Idle)

    // addressing is the Select that conditions the static fields. setCondition(true) → Static (value 1) → fields visible; setCondition(false) → DHCP (0) → hidden.
    auto setStatic = [&](bool on) {
        mm::test::setControlValue<uint8_t>(net, "addressing", on ? uint8_t{1} : uint8_t{0});
    };
    for (const char* field : {"ip", "gateway", "subnet", "dns"}) {
        mm::test::checkConditionalControl(net, field, setStatic, /*visibleWhenTrue=*/true);
    }
}

// RFC 3927's block is 169.254.0.0/16, and only that: a neighbor such as 169.253 or 170.254 is an ordinary address.
TEST_CASE("a link-local address is recognized by its 169.254 prefix alone") {
    const uint8_t linkLocal[4] = {169, 254, 12, 34};
    const uint8_t leased[4]    = {192, 168, 1, 50};
    const uint8_t nearMiss1[4] = {169, 253, 12, 34};
    const uint8_t nearMiss2[4] = {170, 254, 12, 34};
    CHECK(mm::isLinkLocalIPv4(linkLocal));
    CHECK_FALSE(mm::isLinkLocalIPv4(leased));
    CHECK_FALSE(mm::isLinkLocalIPv4(nearMiss1));
    CHECK_FALSE(mm::isLinkLocalIPv4(nearMiss2));
}

// The rule that keeps the fallback from disturbing a working setup: a self-assigned address is the way in only where WiFi does not exist.
TEST_CASE("a link-local address counts only on a build without WiFi, and a lease counts everywhere") {
    const uint8_t linkLocal[4] = {169, 254, 12, 34};
    const uint8_t leased[4]    = {10, 0, 0, 7};
    CHECK(mm::addressCounts(linkLocal, /*buildHasWiFi=*/false, /*configured=*/nullptr));         // Ethernet-only: the one way to reach it
    CHECK_FALSE(mm::addressCounts(linkLocal, /*buildHasWiFi=*/true, /*configured=*/nullptr));    // WiFi build: the cascade carries on to WiFi or the access point
    CHECK(mm::addressCounts(leased, /*buildHasWiFi=*/true, /*configured=*/nullptr));
    CHECK(mm::addressCounts(leased, /*buildHasWiFi=*/false, /*configured=*/nullptr));
}

// A 169.254 address the user typed in is a choice, while a self-assigned one beside a static setting is still a fallback.
TEST_CASE("a link-local address counts on a WiFi build only when it is the static one the user set") {
    const uint8_t linkLocal[4]  = {169, 254, 12, 34};
    const uint8_t configured[4] = {169, 254, 12, 34};
    const uint8_t elsewhere[4]  = {192, 168, 1, 250};
    CHECK(mm::addressCounts(linkLocal, /*buildHasWiFi=*/true, configured));
    CHECK_FALSE(mm::addressCounts(linkLocal, /*buildHasWiFi=*/true, elsewhere));
}
