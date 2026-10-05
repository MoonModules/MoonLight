/// @module NetworkModule
/// @also WiFiModule
/// Unit tests for NetworkModule: credentials, the mode and radio controls, static addressing and the link-local rule.

#include "doctest.h"
#include "platform_config.h"       // pulls in platform::hasWiFi before NetworkModule.h
#include "core/system/NetworkModule.h"
#include "core/system/WiFiModule.h"
#include "core/util/JsonSink.h"
#include "conditional_controls.h"  // shared conditional-control invariant helpers

#include <cstdio>
#include <cstring>
#include <string>

namespace {
// A network module with its station child, as main wires them.
struct WiFiNetwork {
    mm::NetworkModule net;
    mm::WiFiModule wifi;
    WiFiNetwork() {
        net.addChild(&wifi);
        net.setWiFi(&wifi);
    }
    ~WiFiNetwork() { net.removeChild(&wifi); }   // the child is a member, not the tree's to free
};
}  // namespace

// Provisioning remembers its network at the top of the known list and saves it, so the next boot joins it.
TEST_CASE("NetworkModule::setWifiCredentials remembers the network at the top and saves it") {
    WiFiNetwork w;
    CHECK_FALSE(w.wifi.dirty());
    w.net.setWifiCredentials("homeAP", "secret123");
    CHECK(w.wifi.dirty());
    REQUIRE(w.wifi.knownCount() == 1);
    CHECK(std::strcmp(w.wifi.ssidAt(0), "homeAP") == 0);
    CHECK(std::strcmp(w.wifi.passwordAt(0), "secret123") == 0);

    // A second network goes on top; the same name again updates its row rather than adding one.
    w.net.setWifiCredentials("venue", "pw2");
    w.net.setWifiCredentials("homeAP", "changed");
    REQUIRE(w.wifi.knownCount() == 2);
    CHECK(std::strcmp(w.wifi.ssidAt(0), "homeAP") == 0);
    CHECK(std::strcmp(w.wifi.passwordAt(0), "changed") == 0);
    CHECK(std::strcmp(w.wifi.ssidAt(1), "venue") == 0);
}

// A nullptr SSID is silently ignored (no row, no save), guards against a bogus caller.
TEST_CASE("NetworkModule::setWifiCredentials with null SSID is a no-op") {
    WiFiNetwork w;
    w.net.setWifiCredentials(nullptr, "irrelevant");
    CHECK_FALSE(w.wifi.dirty());
    CHECK(w.wifi.knownCount() == 0);
}

// A nullptr password is treated as empty (open networks), still remembering the network.
TEST_CASE("NetworkModule::setWifiCredentials with null password treats it as empty") {
    // Improv allows open networks (auth flag = "NO"); the credential pusher may pass a nullptr or empty password for those. Both must be tolerated.
    WiFiNetwork w;
    w.net.setWifiCredentials("openSSID", nullptr);
    REQUIRE(w.wifi.knownCount() == 1);
    CHECK(w.wifi.passwordAt(0)[0] == 0);
}

// An over-length SSID (100 chars) is truncated cleanly into the 33-byte row; ASAN catches any overflow.
TEST_CASE("NetworkModule::setWifiCredentials accepts long SSID without crash") {
    WiFiNetwork w;
    // `volatile` stops constant folding, else GCC sees 99 bytes going into 33 and warns about the truncation this test expects.
    volatile size_t len = 99;
    char longSsid[100];
    std::memset(longSsid, 'A', len);
    longSsid[len] = 0;
    w.net.setWifiCredentials(longSsid, "pw");
    REQUIRE(w.wifi.knownCount() == 1);
    CHECK(std::strlen(w.wifi.ssidAt(0)) == 32);
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
    // A refused value leaves the address as it was, so a half-typed "10.0.0" never becomes 10.0.0.x.
    CHECK((out[0] == 255 && out[1] == 255 && out[2] == 255 && out[3] == 255));
    CHECK_FALSE(mm::parseDottedQuad("10.0.0", out));
    CHECK((out[0] == 255 && out[1] == 255 && out[2] == 255 && out[3] == 255));
}

namespace {
// Frozen time, so the cascade's timeouts fire only when the test moves the clock.
struct FrozenClock {
    FrozenClock() { mm::platform::setTestNowMs(1000); }
    ~FrozenClock() { mm::platform::setTestNowMs(0); }
};
}  // namespace

// A known network that does not join in time gives way to the next in priority order, and only after the last does the device open its access point.
TEST_CASE("the station tries each known network in priority order before the access point") {
    FrozenClock clock;
    mm::platform::setTestWifiStaAvailable(true);
    {
        WiFiNetwork w;
        w.wifi.remember("third", "pw3");
        w.wifi.remember("second", "pw2");
        w.wifi.remember("first", "pw1");
        w.net.setup();   // no Ethernet on the desktop, so it starts on WiFi
        CHECK(std::strcmp(mm::platform::testLastStaSsid(), "first") == 0);
        mm::platform::setTestNowMs(1000 + 11000);   // past the per-network grace
        w.net.tick1s();
        CHECK(std::strcmp(mm::platform::testLastStaSsid(), "second") == 0);
        mm::platform::setTestNowMs(1000 + 22000);
        w.net.tick1s();
        CHECK(std::strcmp(mm::platform::testLastStaSsid(), "third") == 0);
    }
    mm::platform::setTestWifiStaAvailable(false);   // reset, so cases stay independent
}

namespace {
mm::ListSource* availableOf(mm::WiFiModule& w) {
    for (uint8_t i = 0; i < w.controls().count(); i++)
        if (std::strcmp(w.controls()[i].name, "available") == 0) return static_cast<mm::ListSource*>(w.controls()[i].ptr);
    return nullptr;
}
// A device that sees one secured network, "venue", and knows "home".
struct ScannedNetwork : WiFiNetwork {
    ScannedNetwork() {
        mm::platform::WifiNetwork venue{};
        std::snprintf(venue.ssid, sizeof(venue.ssid), "venue");
        venue.rssi = -60;
        venue.secured = true;
        mm::platform::setTestWifiScan(&venue, 1);
        mm::platform::setTestWifiStaAvailable(true);
        wifi.rebuildControls();   // the scheduler builds them in the running system
        wifi.remember("home", "pw");
        net.setup();
        wifi.onControlChanged("scan");
        wifi.tick1s();
        wifi.tick1s();
    }
    ~ScannedNetwork() {
        mm::platform::setTestWifiScan(nullptr, 0);
        mm::platform::setTestWifiStaAvailable(false);
        mm::platform::setTestWifiStaIPv4(nullptr);
        mm::platform::setTestWifiFailure(mm::platform::WifiFailure::None);
    }
};
}  // namespace

// Connect-first, as a phone joins: tap a scanned network, type its password, and the device joins at once, saving the network only once it joined.
TEST_CASE("connecting to a scanned network joins it now and remembers it once joined") {
    FrozenClock clock;
    ScannedNetwork s;
    mm::ListSource* avail = availableOf(s.wifi);
    REQUIRE(avail != nullptr);
    REQUIRE(avail->setListRowField(1, "password", "{\"value\":\"secret\"}"));
    REQUIRE(avail->setListRowField(1, "connect", ""));
    CHECK(s.wifi.knownCount() == 1);   // not saved yet
    s.net.tick1s();
    CHECK(std::strcmp(mm::platform::testLastStaSsid(), "venue") == 0);
    const uint8_t ip[4] = {192, 168, 4, 20};
    mm::platform::setTestWifiStaIPv4(ip);
    s.net.tick1s();
    REQUIRE(s.wifi.knownCount() == 2);
    CHECK(std::strcmp(s.wifi.ssidAt(0), "venue") == 0);   // joined, so it is known, at the top
    CHECK(std::strcmp(s.wifi.passwordAt(0), "secret") == 0);
}

// A wrong password is said in words and nothing is saved; the known networks take over again.
TEST_CASE("a scanned network that does not join says why and is not remembered") {
    FrozenClock clock;
    ScannedNetwork s;
    mm::ListSource* avail = availableOf(s.wifi);
    REQUIRE(avail != nullptr);
    REQUIRE(avail->setListRowField(1, "password", "{\"value\":\"wrong\"}"));
    REQUIRE(avail->setListRowField(1, "connect", ""));
    s.net.tick1s();
    mm::platform::setTestWifiFailure(mm::platform::WifiFailure::WrongPassword);
    mm::platform::setTestNowMs(1000 + 11000);   // past the grace
    s.net.tick1s();
    REQUIRE(s.wifi.status() != nullptr);
    CHECK(std::string(s.wifi.status()) == "incorrect password");
    CHECK(s.wifi.knownCount() == 1);
    CHECK(std::strcmp(mm::platform::testLastStaSsid(), "home") == 0);
}

// A join asked for long after the cascade last moved is tried for its full time, not read as timed out in the tick that starts it.
TEST_CASE("a join asked for after a long wait is tried, not failed at once") {
    FrozenClock clock;
    ScannedNetwork s;
    mm::platform::setTestNowMs(1000 + 30000);   // the cascade has sat in one state for 30 seconds
    mm::ListSource* avail = availableOf(s.wifi);
    REQUIRE(avail != nullptr);
    REQUIRE(avail->setListRowField(1, "password", "{\"value\":\"secret\"}"));
    REQUIRE(avail->setListRowField(1, "connect", ""));
    s.net.tick1s();
    CHECK(std::strcmp(mm::platform::testLastStaSsid(), "venue") == 0);
    CHECK(s.wifi.joinRequest() != nullptr);   // still joining
}

// A join that failed keeps what was typed, so Connect again retries with it, as the field still shows it.
TEST_CASE("a failed join keeps the typed password for a retry") {
    FrozenClock clock;
    ScannedNetwork s;
    mm::ListSource* avail = availableOf(s.wifi);
    REQUIRE(avail != nullptr);
    REQUIRE(avail->setListRowField(1, "password", "{\"value\":\"typo\"}"));
    REQUIRE(avail->setListRowField(1, "connect", ""));
    s.net.tick1s();
    mm::platform::setTestWifiFailure(mm::platform::WifiFailure::WrongPassword);
    mm::platform::setTestNowMs(1000 + 11000);
    s.net.tick1s();
    mm::JsonSink sink;
    avail->writeListRowDetail(sink, 0);
    CHECK(std::string(sink.data(), sink.size()).find("\"LiMqNQ==\"") != std::string::npos);   // "typo", obfuscated as every password on the page
}

// A known network the card asked for that fails counts as tried, so the round goes on to the others and does not try it again at once.
TEST_CASE("a known network asked for from the card that fails is not tried again in the same round") {
    FrozenClock clock;
    mm::platform::setTestWifiStaAvailable(true);
    {
        WiFiNetwork w;
        w.wifi.rebuildControls();
        w.wifi.remember("second", "pw2");
        w.wifi.remember("home", "pw1");
        w.net.setup();
        REQUIRE(std::strcmp(mm::platform::testLastStaSsid(), "home") == 0);
        REQUIRE(w.wifi.setListRowField(w.wifi.idAt(1), "connect", ""));
        w.net.tick1s();
        REQUIRE(std::strcmp(mm::platform::testLastStaSsid(), "second") == 0);
        mm::platform::setTestNowMs(1000 + 11000);
        w.net.tick1s();
        CHECK(std::strcmp(mm::platform::testLastStaSsid(), "home") == 0);
        mm::platform::setTestNowMs(1000 + 22000);
        w.net.tick1s();
        CHECK(std::strcmp(mm::platform::testLastStaSsid(), "second") != 0);
    }
    mm::platform::setTestWifiStaAvailable(false);
    mm::platform::setTestWifiFailure(mm::platform::WifiFailure::None);
}

namespace {
const char* modeOf(mm::NetworkModule& net) {
    for (uint8_t i = 0; i < net.controls().count(); i++)
        if (std::strcmp(net.controls()[i].name, "mode") == 0) return static_cast<const char*>(net.controls()[i].ptr);
    return "";
}
}  // namespace

// Ethernet outranks WiFi, so a join asked for from the card while the cable carries the device is refused, and the card says why rather than dropping the wired link.
TEST_CASE("a join asked for while Ethernet carries the device is refused with the reason") {
    FrozenClock clock;
    const uint8_t leased[4] = {192, 168, 1, 20};
    mm::platform::setTestEthIPv4(leased);
    {
        WiFiNetwork w;
        mm::platform::WifiNetwork venue{};
        std::snprintf(venue.ssid, sizeof(venue.ssid), "venue");
        mm::platform::setTestWifiScan(&venue, 1);
        mm::platform::setTestWifiStaAvailable(true);
        w.wifi.rebuildControls();
        w.net.setup();
        w.net.tick1s();
        REQUIRE(std::string(modeOf(w.net)) == "Ethernet");
        w.wifi.onControlChanged("scan");
        w.wifi.tick1s();
        w.wifi.tick1s();
        mm::ListSource* avail = availableOf(w.wifi);
        REQUIRE(avail != nullptr);
        REQUIRE(avail->setListRowField(1, "connect", ""));
        w.net.tick1s();
        CHECK(std::string(modeOf(w.net)) == "Ethernet");
        REQUIRE(w.wifi.status() != nullptr);
        CHECK(std::string(w.wifi.status()).find("Ethernet is in use") != std::string::npos);
        CHECK(w.wifi.joinRequest() == nullptr);
    }
    mm::platform::setTestEthIPv4(nullptr);
    mm::platform::setTestWifiScan(nullptr, 0);
    mm::platform::setTestWifiStaAvailable(false);
}

// Ethernet is preferred, as every operating system prefers the wired route: a cable that gets an address takes over from WiFi.
TEST_CASE("Ethernet takes over from WiFi once it has an address") {
    FrozenClock clock;
    mm::platform::setTestWifiStaAvailable(true);
    {
        WiFiNetwork w;
        w.wifi.remember("home", "pw");
        w.net.setup();
        const uint8_t staIp[4] = {192, 168, 1, 30};
        mm::platform::setTestWifiStaIPv4(staIp);
        w.net.tick1s();
        REQUIRE(std::string(modeOf(w.net)) == "WiFi STA");
        const uint8_t leased[4] = {192, 168, 1, 20};
        mm::platform::setTestEthIPv4(leased);
        w.net.tick1s();
        CHECK(std::string(modeOf(w.net)) == "Ethernet");
    }
    mm::platform::setTestEthIPv4(nullptr);
    mm::platform::setTestWifiStaIPv4(nullptr);
    mm::platform::setTestWifiStaAvailable(false);
}

// An edit to the IP settings of the network the device is on applies at once, the way every setting applies live.
TEST_CASE("editing the joined network's IP settings applies them live") {
    FrozenClock clock;
    mm::platform::setTestWifiStaAvailable(true);
    {
        WiFiNetwork w;
        w.wifi.remember("home", "pw");
        w.net.setup();
        const uint8_t staIp[4] = {192, 168, 1, 30};
        mm::platform::setTestWifiStaIPv4(staIp);
        w.net.tick1s();
        REQUIRE(std::string(modeOf(w.net)) == "WiFi STA");
        const uint32_t before = mm::platform::testNetStaticApplyCount(mm::platform::NetIface::Sta);
        constexpr uint32_t kFirstRowId = 1;
        REQUIRE(w.wifi.setListRowField(kFirstRowId, "ipSettings", "{\"value\":1}"));
        REQUIRE(w.wifi.setListRowField(kFirstRowId, "ip", "{\"value\":\"192.168.1.240\"}"));
        w.net.tick1s();
        CHECK(mm::platform::testNetStaticApplyCount(mm::platform::NetIface::Sta) > before);
    }
    mm::platform::setTestWifiStaIPv4(nullptr);
    mm::platform::setTestWifiStaAvailable(false);
}

// Back to DHCP the address is gone until the router leases one, which takes seconds; that gap is a lease the device asked for, not a dropout.
TEST_CASE("a switch back to DHCP waits for the lease rather than falling back to the access point") {
    FrozenClock clock;
    mm::platform::setTestWifiStaAvailable(true);
    const uint8_t staIp[4] = {192, 168, 1, 30};
    const auto run = [&](uint32_t gapMs, bool stillOnStation) {
        WiFiNetwork w;
        w.wifi.remember("home", "pw");
        w.net.setup();
        mm::platform::setTestWifiStaIPv4(staIp);
        w.net.tick1s();
        REQUIRE(std::string(modeOf(w.net)) == "WiFi STA");
        constexpr uint32_t kFirstRowId = 1;
        REQUIRE(w.wifi.setListRowField(kFirstRowId, "ipSettings", "{\"value\":1}"));
        REQUIRE(w.wifi.setListRowField(kFirstRowId, "ip", "{\"value\":\"192.168.1.240\"}"));
        w.net.tick1s();
        REQUIRE(w.wifi.setListRowField(kFirstRowId, "ipSettings", "{\"value\":0}"));
        mm::platform::setTestWifiStaIPv4(nullptr);   // the lease has not landed yet
        mm::platform::setTestClockStep(1);   // a running device's clock moves within the tick, which a frozen one hides
        w.net.tick1s();
        w.net.tick1s();
        mm::platform::setTestClockStep(0);
        CHECK(std::string(modeOf(w.net)) == "WiFi STA");   // a moment after the switch, still waiting for the lease
        mm::platform::setTestNowMs(1000 + gapMs);
        w.net.tick1s();
        CHECK((std::string(modeOf(w.net)) == "WiFi STA") == stillOnStation);
        mm::platform::setTestNowMs(1000);
    };
    run(12000, true);    // past the dropout grace, inside the lease's window
    run(16000, false);   // a lease that never comes still ends in the fallback (Idle here, with no access point wired)
    mm::platform::setTestWifiStaIPv4(nullptr);
    mm::platform::setTestWifiStaAvailable(false);
}

// A known network the scan saw is tried before one higher in the list that it did not, and the unseen one still gets its turn.
TEST_CASE("the cascade tries the networks in range first, then the rest") {
    FrozenClock clock;
    mm::platform::setTestWifiStaAvailable(true);   // every join starts, and none gets an address
    {
        WiFiNetwork w;
        w.wifi.remember("Away", "pw");
        w.wifi.remember("Home", "pw");   // the top of the list, but out of range
        const mm::platform::WifiNetwork seen[] = {{"Away", -60, true}};
        mm::platform::setTestWifiScan(seen, 1);
        w.wifi.onControlChanged("scan");
        w.wifi.tick1s();
        w.net.setup();
        CHECK(std::string(mm::platform::testLastStaSsid()) == "Away");
        mm::platform::setTestNowMs(1000 + 11000);   // past the grace for that network
        w.net.tick1s();
        CHECK(std::string(mm::platform::testLastStaSsid()) == "Home");
    }
    mm::platform::setTestWifiScan(nullptr, 0);
    mm::platform::setTestWifiStaAvailable(false);
}

// A scan finishing between two attempts reorders what is left, and the untried network is still reached.
TEST_CASE("a scan between two attempts still leaves every known network its try") {
    FrozenClock clock;
    mm::platform::setTestWifiStaAvailable(true);
    {
        WiFiNetwork w;
        w.wifi.remember("Away", "pw");
        w.wifi.remember("Home", "pw");
        w.net.setup();   // no scan yet: the list order, Home first
        REQUIRE(std::string(mm::platform::testLastStaSsid()) == "Home");
        const mm::platform::WifiNetwork seen[] = {{"Away", -60, true}};   // now Away comes first, Home after it
        mm::platform::setTestWifiScan(seen, 1);
        w.wifi.onControlChanged("scan");
        w.wifi.tick1s();
        mm::platform::setTestNowMs(1000 + 11000);
        w.net.tick1s();
        CHECK(std::string(mm::platform::testLastStaSsid()) == "Away");
    }
    mm::platform::setTestWifiScan(nullptr, 0);
    mm::platform::setTestWifiStaAvailable(false);
}

// The joined network is a row, not a position: adding a network above it keeps the check mark, and its IP settings, on the network the device is on.
TEST_CASE("a network added above the joined one leaves the joined one marked") {
    FrozenClock clock;
    mm::platform::setTestWifiStaAvailable(true);
    {
        WiFiNetwork w;
        w.wifi.remember("home", "pw");
        w.net.setup();
        const uint8_t staIp[4] = {192, 168, 1, 30};
        mm::platform::setTestWifiStaIPv4(staIp);
        w.net.tick1s();
        REQUIRE(std::string(modeOf(w.net)) == "WiFi STA");
        uint32_t id = 0;
        REQUIRE(w.wifi.addListRow(id));
        REQUIRE(w.wifi.setListRowField(id, "ssid", "{\"value\":\"other\"}"));
        REQUIRE(w.wifi.moveListRow(id, 0));
        w.net.tick1s();
        auto rowText = [&](uint8_t i) { mm::JsonSink sink; w.wifi.writeListRow(sink, i); return std::string(sink.data(), sink.size()); };
        CHECK(rowText(0).find("\"joined\"") == std::string::npos);   // "other", above it
        CHECK(rowText(1).find("\"joined\"") != std::string::npos);   // "home", still the one it is on
    }
    mm::platform::setTestWifiStaIPv4(nullptr);
    mm::platform::setTestWifiStaAvailable(false);
}

// Forget on the network the device is on leaves it, as a phone does, and the next known network takes over.
TEST_CASE("forgetting the network the device is on leaves it for the next known one") {
    FrozenClock clock;
    mm::platform::setTestWifiStaAvailable(true);
    {
        WiFiNetwork w;
        w.wifi.remember("second", "pw2");
        w.wifi.remember("home", "pw");
        w.net.setup();
        const uint8_t staIp[4] = {192, 168, 1, 30};
        mm::platform::setTestWifiStaIPv4(staIp);
        w.net.tick1s();
        REQUIRE(std::string(modeOf(w.net)) == "WiFi STA");
        REQUIRE(w.wifi.deleteListRow(w.wifi.idAt(0)));
        mm::platform::setTestWifiStaIPv4(nullptr);
        w.net.tick1s();
        CHECK(std::string(modeOf(w.net)) == "WiFi STA (waiting)");
        CHECK(std::strcmp(mm::platform::testLastStaSsid(), "second") == 0);
    }
    mm::platform::setTestWifiStaIPv4(nullptr);
    mm::platform::setTestWifiStaAvailable(false);
}

// Credentials pushed over Improv while Ethernet carries the device are remembered, and WiFi joins when the cable goes, as a join from the card does.
TEST_CASE("credentials arriving while Ethernet carries the device are only remembered") {
    FrozenClock clock;
    const uint8_t leased[4] = {192, 168, 1, 20};
    mm::platform::setTestEthIPv4(leased);
    mm::platform::setTestWifiStaAvailable(true);
    {
        WiFiNetwork w;
        w.net.setup();
        w.net.tick1s();
        REQUIRE(std::string(modeOf(w.net)) == "Ethernet");
        w.net.setWifiCredentials("pushed-over-improv", "pw");
        CHECK(w.wifi.knownCount() == 1);
        CHECK(std::strcmp(mm::platform::testLastStaSsid(), "pushed-over-improv") != 0);
        w.net.tick1s();
        CHECK(std::string(modeOf(w.net)) == "Ethernet");
    }
    mm::platform::setTestEthIPv4(nullptr);
    mm::platform::setTestWifiStaAvailable(false);
}

// Connect pressed on another network while a join is in flight replaces that join, rather than letting the first one finish and claim the second's name.
TEST_CASE("a second Connect during a join replaces the join") {
    FrozenClock clock;
    ScannedNetwork s;
    mm::ListSource* avail = availableOf(s.wifi);
    REQUIRE(avail != nullptr);
    REQUIRE(avail->setListRowField(1, "password", "{\"value\":\"secret\"}"));
    REQUIRE(avail->setListRowField(1, "connect", ""));
    s.net.tick1s();
    REQUIRE(std::strcmp(mm::platform::testLastStaSsid(), "venue") == 0);
    REQUIRE(s.wifi.setListRowField(s.wifi.idAt(0), "connect", ""));   // the known network, "home"
    s.net.tick1s();
    CHECK(std::strcmp(mm::platform::testLastStaSsid(), "home") == 0);
}

// The station's card shows rssi and txPower as read-only controls that start hidden, until the radio is up.
TEST_CASE("WiFi rssi/txPower controls hidden until the radio is up") {
    mm::WiFiModule wifi;
    wifi.rebuildControls();
    auto hidden = [&](const char* name) {
        for (uint8_t i = 0; i < wifi.controls().count(); i++)
            if (std::strcmp(wifi.controls()[i].name, name) == 0) return wifi.controls()[i].hidden;
        FAIL("no such control");
        return false;
    };
    CHECK(hidden("rssi"));
    CHECK(hidden("txPower"));
    wifi.showRadio(-60, 19, /*radioOn=*/true, /*connected=*/true);
    CHECK_FALSE(hidden("rssi"));
    CHECK_FALSE(hidden("txPower"));
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
