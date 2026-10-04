/// @module AccessPointModule
/// The device's own network: when it opens by its `opens` rule, how it appears, the captive portal a joining phone sees, and the handoff to the home network.

#include "doctest.h"
#include "platform_config.h"   // pulls in platform::hasWiFi before NetworkModule.h
#include "core/system/NetworkModule.h"
#include "core/system/AccessPointModule.h"
#include "core/util/CaptivePortal.h"
#include "core/util/JsonSink.h"

#include <cstring>
#include <string>
#include <vector>

namespace {
/// A DNS query for `name` of `type`, as a phone's resolver sends it.
std::vector<uint8_t> dnsQuery(const char* name, uint16_t type) {
    std::vector<uint8_t> q = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
    const char* p = name;
    while (*p) {
        const char* dot = std::strchr(p, '.');
        const size_t n = dot ? static_cast<size_t>(dot - p) : std::strlen(p);
        q.push_back(static_cast<uint8_t>(n));
        q.insert(q.end(), p, p + n);
        p += n + (dot ? 1 : 0);
    }
    q.push_back(0);
    q.push_back(static_cast<uint8_t>(type >> 8)); q.push_back(static_cast<uint8_t>(type));
    q.push_back(0); q.push_back(1);   // class IN
    return q;
}
}  // namespace

// The A question every captive check asks comes back answered with the access point's own address, so the phone opens the UI.
TEST_CASE("the DNS responder answers an A query with the access point's address") {
    auto q = dnsQuery("captive.apple.com", 1);
    const size_t qLen = q.size();
    q.resize(mm::captive::kMaxMessage);
    const size_t n = mm::captive::dnsReply(q.data(), qLen, q.size(), mm::captive::kAddress);
    REQUIRE(n == qLen + 16);
    CHECK(q[0] == 0x12); CHECK(q[1] == 0x34);   // the query's id
    CHECK(q[2] == 0x85);                         // a response, authoritative, recursion desired echoed
    CHECK(q[3] == 0x00);                         // no error
    CHECK(q[7] == 1);                            // one answer
    CHECK(std::memcmp(&q[n - 4], mm::captive::kAddress, 4) == 0);
}

// Any other type, such as the AAAA a phone asks alongside, gets an empty answer, so it falls back to the A record.
TEST_CASE("the DNS responder answers another type with no records") {
    auto q = dnsQuery("connectivitycheck.gstatic.com", 28);
    const size_t qLen = q.size();
    q.resize(mm::captive::kMaxMessage);
    CHECK(mm::captive::dnsReply(q.data(), qLen, q.size(), mm::captive::kAddress) == qLen);
    CHECK(q[7] == 0);
}

// A packet that is not one whole question is dropped rather than answered, whatever its bytes.
TEST_CASE("the DNS responder drops a malformed or oversized packet") {
    uint8_t shortPacket[8] = {};
    CHECK(mm::captive::dnsReply(shortPacket, sizeof(shortPacket), sizeof(shortPacket), mm::captive::kAddress) == 0);

    auto truncated = dnsQuery("captive.apple.com", 1);
    truncated.resize(truncated.size() - 3);   // the name runs, but its type and class are cut off
    CHECK(mm::captive::dnsReply(truncated.data(), truncated.size(), 512, mm::captive::kAddress) == 0);

    auto pointer = dnsQuery("a.b", 1);
    pointer[12] = 0xC0;   // a compression pointer, which a question never carries
    CHECK(mm::captive::dnsReply(pointer.data(), pointer.size(), 512, mm::captive::kAddress) == 0);

    std::vector<uint8_t> runaway = {0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 40; i++) { runaway.push_back(63); runaway.insert(runaway.end(), 63, 'x'); }
    runaway.push_back(0); runaway.insert(runaway.end(), {0, 1, 0, 1});
    CHECK(mm::captive::dnsReply(runaway.data(), runaway.size(), runaway.size() + 16, mm::captive::kAddress) == 0);

    auto response = dnsQuery("a.b", 1);
    response[2] |= 0x80;   // already a response
    CHECK(mm::captive::dnsReply(response.data(), response.size(), 512, mm::captive::kAddress) == 0);

    auto noRoom = dnsQuery("a.b", 1);   // a reply that does not fit the buffer
    CHECK(mm::captive::dnsReply(noRoom.data(), noRoom.size(), noRoom.size(), mm::captive::kAddress) == 0);
}

// A request through the access point for any other name is sent to the UI; its own address, and every request through the home network, is served.
TEST_CASE("a request through the access point for another host is redirected") {
    const uint8_t home[4] = {192, 168, 1, 50};
    CHECK(mm::captive::redirects(mm::captive::kAddress, "captive.apple.com\r\n", "/"));
    CHECK(mm::captive::redirects(mm::captive::kAddress, "moonlight.local", "/"));
    CHECK_FALSE(mm::captive::redirects(mm::captive::kAddress, "4.3.2.1\r\n", "/"));
    CHECK_FALSE(mm::captive::redirects(mm::captive::kAddress, "4.3.2.1:80", "/"));
    CHECK(mm::captive::redirects(mm::captive::kAddress, "4.3.2.10", "/"));
    CHECK_FALSE(mm::captive::redirects(mm::captive::kAddress, nullptr, "/"));
    CHECK_FALSE(mm::captive::redirects(home, "captive.apple.com", "/"));
    // The API through the access point answers as itself: a client asking for JSON is not sent a page.
    CHECK_FALSE(mm::captive::redirects(mm::captive::kAddress, "moonlight.local", "/api/state"));
    CHECK(mm::captive::redirects(mm::captive::kAddress, "moonlight.local", "/app.js"));
}

namespace {
template <typename T>
T* controlPtr(mm::MoonModule& m, const char* name) {
    for (uint8_t i = 0; i < m.controls().count(); i++)
        if (std::strcmp(m.controls()[i].name, name) == 0) return static_cast<T*>(m.controls()[i].ptr);
    return nullptr;
}
bool isHidden(mm::MoonModule& m, const char* name) {
    for (uint8_t i = 0; i < m.controls().count(); i++)
        if (std::strcmp(m.controls()[i].name, name) == 0) return m.controls()[i].hidden;
    return true;
}
std::string modeOf(mm::NetworkModule& net) {
    const char* mode = controlPtr<const char>(net, "mode");
    return mode ? mode : "";
}

// A network module with its station and access point children, as main wires them, on a radio the test drives.
struct ApNetwork {
    mm::NetworkModule net;
    mm::WiFiModule wifi;
    mm::AccessPointModule ap;
    ApNetwork() {
        mm::platform::setTestNowMs(1000);
        mm::platform::setTestWifiStaAvailable(true);
        mm::platform::setTestWifiApAvailable(true);
        net.addChild(&wifi);
        net.addChild(&ap);
        net.setWiFi(&wifi);
        net.setAccessPoint(&ap);
        net.rebuildControls();
        wifi.rebuildControls();
        ap.rebuildControls();
    }
    ~ApNetwork() {
        net.removeChild(&ap);
        net.removeChild(&wifi);
        mm::platform::setTestWifiStaAvailable(false);
        mm::platform::setTestWifiApAvailable(false);
        mm::platform::setTestWifiStaIPv4(nullptr);
        mm::platform::setTestWifiScan(nullptr, 0);
        mm::platform::setTestNowMs(0);
    }
    void opens(mm::AccessPointModule::Opens o) { *controlPtr<uint8_t>(ap, "opens") = static_cast<uint8_t>(o); }
    void at(uint32_t ms) { mm::platform::setTestNowMs(1000 + ms); net.tick1s(); }
    void joined() {
        static const uint8_t ip[4] = {192, 168, 1, 50};
        mm::platform::setTestWifiStaIPv4(ip);
    }
};
}  // namespace

// On failure, the default: a device with nothing to join opens its access point, and closes it once a known network joins.
TEST_CASE("the access point opens on failure and closes once a network joins") {
    ApNetwork n;
    n.net.setup();
    CHECK(modeOf(n.net) == "WiFi AP");
    CHECK(mm::platform::wifiApConnected());
    CHECK_FALSE(isHidden(n.ap, "clients"));
    n.wifi.remember("home", "pw");
    n.at(61000);   // the fallback retries the known networks
    CHECK(std::strcmp(mm::platform::testLastStaSsid(), "home") == 0);
    n.joined();
    n.at(62000);
    CHECK(modeOf(n.net) == "WiFi STA");
    CHECK_FALSE(mm::platform::wifiApConnected());
    CHECK(isHidden(n.ap, "clients"));
}

// Always keeps the access point up beside a joined network.
TEST_CASE("the access point set to always stays up while connected") {
    ApNetwork n;
    n.opens(mm::AccessPointModule::Opens::Always);
    n.wifi.remember("home", "pw");
    n.joined();
    n.net.setup();
    n.at(1000);
    CHECK(modeOf(n.net) == "WiFi STA");
    CHECK(mm::platform::wifiApConnected());
}

// Never keeps the access point closed when nothing joins, and the device goes on retrying its known networks.
TEST_CASE("the access point set to never stays closed when a network is known") {
    ApNetwork n;
    n.opens(mm::AccessPointModule::Opens::Never);
    n.wifi.remember("home", "pw");
    n.net.setup();
    n.at(11000);   // the only known network does not join in time
    CHECK(modeOf(n.net) == "Idle");
    CHECK_FALSE(mm::platform::wifiApConnected());
    n.at(72000);
    CHECK(modeOf(n.net) == "WiFi STA (waiting)");
}

// Never is refused while nothing else would reach the device: it opens on failure, and the card says why.
TEST_CASE("never with nothing configured still opens the access point and says so") {
    ApNetwork n;
    n.opens(mm::AccessPointModule::Opens::Never);
    n.net.setup();
    n.at(1000);
    CHECK(modeOf(n.net) == "WiFi AP");
    REQUIRE(n.ap.status() != nullptr);
    CHECK(std::string(n.ap.status()).find("opens on failure") != std::string::npos);
}

// The password, channel and hidden name reach the radio, and a change re-opens it live.
TEST_CASE("the access point opens with its settings and re-opens on a change") {
    ApNetwork n;
    std::strcpy(controlPtr<char>(n.ap, "password"), "first-pass");
    *controlPtr<uint8_t>(n.ap, "channel") = 6;
    *controlPtr<bool>(n.ap, "hidden") = true;
    n.net.setup();
    CHECK(std::string(mm::platform::testLastApConfig().password) == "first-pass");
    CHECK(mm::platform::testLastApConfig().channel == 6);
    CHECK(mm::platform::testLastApConfig().hidden);
    n.at(1000);
    std::strcpy(controlPtr<char>(n.ap, "password"), "longenough");
    n.at(2000);
    CHECK(std::string(mm::platform::testLastApConfig().password) == "longenough");
    CHECK(mm::platform::wifiApConnected());
}

// WPA2 takes 8 to 63 characters, so a shorter password is refused and the stored one stays, never an access point open by surprise.
TEST_CASE("a password of 1 to 7 characters is refused, empty and 8 or more are taken") {
    mm::AccessPointModule ap;
    ap.rebuildControls();
    const mm::ControlDescriptor* pw = nullptr;
    for (uint8_t i = 0; i < ap.controls().count(); i++)
        if (std::strcmp(ap.controls()[i].name, "password") == 0) pw = &ap.controls()[i];
    REQUIRE(pw != nullptr);
    CHECK(mm::applyControlValue(*pw, "{\"password\":\"12345678\"}", "password", mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(mm::applyControlValue(*pw, "{\"password\":\"1234\"}", "password", mm::ApplyPolicy::Clamp) != mm::ApplyResult::Ok);
    CHECK(std::string(static_cast<const char*>(pw->ptr)) == "12345678");   // the refused one left it untouched
    CHECK(mm::applyControlValue(*pw, "{\"password\":\"\"}", "password", mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(std::string(static_cast<const char*>(pw->ptr)).empty());
}

// Open is the default, so a device that knows a network says nothing about its open access point.
TEST_CASE("an open access point gives no warning, even once a network is known") {
    ApNetwork n;
    n.net.setup();
    n.wifi.remember("home", "pw");
    n.at(1000);
    CHECK((n.ap.status() == nullptr || std::string(n.ap.status()).empty()));
}

// A join asked for from a phone on the access point keeps the access point up, with links to the new address, until the phone leaves or two minutes pass.
TEST_CASE("a join from the access point holds it open with links to the new address") {
    ApNetwork n;
    mm::platform::WifiNetwork home{};
    std::snprintf(home.ssid, sizeof(home.ssid), "home");
    home.rssi = -50;
    mm::platform::setTestWifiScan(&home, 1);
    n.net.setup();
    mm::platform::setTestWifiApClients(1);
    n.wifi.onControlChanged("scan");
    n.wifi.tick1s();
    n.wifi.tick1s();
    auto* avail = controlPtr<mm::ListSource>(n.wifi, "available");
    REQUIRE(avail != nullptr);
    REQUIRE(avail->setListRowField(1, "connect", ""));
    n.at(1000);
    CHECK(std::strcmp(mm::platform::testLastStaSsid(), "home") == 0);
    n.joined();
    n.at(2000);
    CHECK(modeOf(n.net) == "WiFi STA");
    CHECK(mm::platform::wifiApConnected());   // held for the phone
    const char* address = controlPtr<const char>(n.wifi, "address");
    REQUIRE(address != nullptr);
    CHECK(std::string(address) == "http://192.168.1.50/");
    n.at(60000);
    CHECK(mm::platform::wifiApConnected());   // the phone is still on it
    n.at(123000);
    CHECK_FALSE(mm::platform::wifiApConnected());   // two minutes passed
    CHECK(controlPtr<const char>(n.wifi, "address") == nullptr);
}

// The hold lasts its two minutes even with nobody on it, since the join's channel move knocks every phone off and one may come back.
TEST_CASE("the handoff holds the access point two minutes even when the phone leaves") {
    ApNetwork n;
    mm::platform::WifiNetwork home{};
    std::snprintf(home.ssid, sizeof(home.ssid), "home");
    mm::platform::setTestWifiScan(&home, 1);
    n.net.setup();
    n.wifi.onControlChanged("scan");
    n.wifi.tick1s();
    n.wifi.tick1s();
    auto* avail = controlPtr<mm::ListSource>(n.wifi, "available");
    REQUIRE(avail != nullptr);
    REQUIRE(avail->setListRowField(1, "connect", ""));
    n.at(1000);
    n.joined();
    n.at(2000);
    n.at(60000);
    CHECK(mm::platform::wifiApConnected());   // nobody on it, still held
    n.at(123000);
    CHECK_FALSE(mm::platform::wifiApConnected());
}

// The row a phone joins from names where the device will be afterward, since the phone often lands back on its own network mid-join.
TEST_CASE("a network in range offers the device's .local link before the join") {
    ApNetwork n;
    mm::platform::WifiNetwork home{};
    std::snprintf(home.ssid, sizeof(home.ssid), "home");
    mm::platform::setTestWifiScan(&home, 1);
    n.net.setup();
    n.wifi.setDeviceName("MM-bench");   // the system module's name, which setup adopts in the running system
    n.wifi.onControlChanged("scan");
    n.wifi.tick1s();
    n.wifi.tick1s();
    auto* avail = controlPtr<mm::ListSource>(n.wifi, "available");
    REQUIRE(avail != nullptr);
    mm::JsonSink sink;
    avail->writeListRowDetail(sink, 0);
    CHECK(std::string(sink.data(), sink.size()).find("http://MM-bench.local/") != std::string::npos);
}

// The fallback retries the known networks now and then, but not while a phone is on the access point, since each try moves the radio and knocks it off.
TEST_CASE("the fallback waits to retry a known network while a phone is on the access point") {
    ApNetwork n;
    n.net.setup();
    REQUIRE(modeOf(n.net) == "WiFi AP");
    mm::platform::setTestWifiApClients(1);
    n.wifi.remember("retry-me", "pw");
    n.at(61000);
    CHECK(modeOf(n.net) == "WiFi AP");   // held back for the phone
    mm::platform::setTestWifiApClients(0);
    n.at(62000);
    CHECK(modeOf(n.net) == "WiFi STA (waiting)");
    CHECK(std::strcmp(mm::platform::testLastStaSsid(), "retry-me") == 0);
}

// Credentials pushed over Improv while the access point runs join beside it, and the access point closes once they joined, with no hold since no phone asked.
TEST_CASE("credentials arriving while the access point runs join beside it, then it closes") {
    ApNetwork n;
    n.net.setup();
    REQUIRE(mm::platform::wifiApConnected());
    n.net.setWifiCredentials("pushed", "pw");
    CHECK(std::strcmp(mm::platform::testLastStaSsid(), "pushed") == 0);
    CHECK(mm::platform::wifiApConnected());   // still up while the station joins
    n.joined();
    n.at(1000);
    CHECK(modeOf(n.net) == "WiFi STA");
    CHECK_FALSE(mm::platform::wifiApConnected());
}

// Lifting "never" while the device has nothing to join opens the access point at once, as every setting applies live.
TEST_CASE("switching never to on failure opens the access point without a restart") {
    ApNetwork n;
    n.opens(mm::AccessPointModule::Opens::Never);
    n.wifi.remember("not-here", "pw");
    n.net.setup();
    n.at(11000);
    REQUIRE(modeOf(n.net) == "Idle");
    n.opens(mm::AccessPointModule::Opens::OnFailure);
    n.at(12000);
    CHECK(modeOf(n.net) == "WiFi AP");
    CHECK(mm::platform::wifiApConnected());
}

// A phone on the access point during a retry gets the same hold when it asks for a join, since it is on the access point.
TEST_CASE("a join asked for from the access point during a retry still holds it open") {
    ApNetwork n;
    n.net.setup();
    REQUIRE(modeOf(n.net) == "WiFi AP");
    n.wifi.remember("home", "pw");
    n.at(61000);   // the fallback retries the known networks beside the access point
    REQUIRE(modeOf(n.net) == "WiFi STA (waiting)");
    REQUIRE(mm::platform::wifiApConnected());
    REQUIRE(n.wifi.setListRowField(n.wifi.idAt(0), "connect", ""));
    n.at(62000);
    n.joined();
    n.at(63000);
    CHECK(modeOf(n.net) == "WiFi STA");
    CHECK(mm::platform::wifiApConnected());   // held for the phone
}

// The access point is named after the device, so a rename reopens it under the new name, as every setting applies live.
TEST_CASE("renaming the device reopens the access point under the new name") {
    ApNetwork n;
    mm::SystemModule sys;
    sys.rebuildControls();
    n.net.setSystemModule(&sys);
    n.net.setup();
    REQUIRE(mm::platform::wifiApConnected());
    std::strcpy(controlPtr<char>(sys, "deviceName"), "MM-renamed");
    n.at(1000);
    CHECK(std::string(mm::platform::testLastApConfig().name) == "MM-renamed");
}
