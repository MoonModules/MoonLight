/// @module WiFiModule
/// The known networks: remembered in priority order, edited from the card, and saved and restored with their passwords.

#include "doctest.h"
#include "core/system/WiFiModule.h"
#include "core/util/JsonSink.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
/// The list as the device saves it (`saving`) or as the collapsed card shows it, `{"known":[...]}`.
std::string known(mm::WiFiModule& w, bool saving) {
    for (uint8_t i = 0; i < w.controls().count(); i++) {
        const auto& c = w.controls()[i];
        if (std::strcmp(c.name, "known") != 0) continue;
        mm::JsonSink sink;
        sink.append("{\"known\":");
        mm::writeControlValue(sink, c, saving);
        sink.append("}");
        return std::string(sink.data(), sink.size());
    }
    return "";
}
std::string saved(mm::WiFiModule& w) { return known(w, /*saving=*/true); }
}  // namespace

TEST_CASE("known networks are added, edited, reordered and forgotten from the card") {
    mm::WiFiModule w;
    w.rebuildControls();
    uint32_t a = 0, b = 0;
    REQUIRE(w.addListRow(a));
    REQUIRE(w.addListRow(b));
    CHECK(w.setListRowField(a, "ssid", "{\"value\":\"workshop\"}"));
    CHECK(w.setListRowField(a, "password", "{\"value\":\"pw1\"}"));
    CHECK(w.setListRowField(b, "ssid", "{\"value\":\"venue\"}"));
    CHECK(std::strcmp(w.ssidAt(0), "workshop") == 0);

    // Moving a row up prefers it.
    CHECK(w.moveListRow(b, 0));
    CHECK(std::strcmp(w.ssidAt(0), "venue") == 0);
    CHECK(std::strcmp(w.ssidAt(1), "workshop") == 0);

    // Forget is the row delete.
    CHECK(w.deleteListRow(b));
    REQUIRE(w.knownCount() == 1);
    CHECK(std::strcmp(w.ssidAt(0), "workshop") == 0);
    CHECK_FALSE(w.setListRowField(b, "ssid", "{\"value\":\"x\"}"));   // a forgotten row takes no edit
}

// A network with no password is marked open, the warning a phone gives: the device joins any network of that name. The collapsed row never carries the password; the saved file does.
TEST_CASE("an open known network is marked open, and only the saved file carries passwords") {
    mm::WiFiModule w;
    w.rebuildControls();
    w.remember("cafe", "");
    w.remember("home", "secret");
    const std::string shown = known(w, /*saving=*/false);
    CHECK(shown.find("\"ssid\":\"cafe\",\"security\":\"open\"") != std::string::npos);
    CHECK(shown.find("secret") == std::string::npos);
    const std::string file = saved(w);
    CHECK(file.find("\"ssid\":\"home\",\"password\":\"secret\"") != std::string::npos);
}

// Each network carries its own IP settings, as a phone keeps them: DHCP by default, the static fields only under Static, and an address that does not parse refused.
TEST_CASE("each known network has its own IP settings") {
    mm::WiFiModule w;
    w.rebuildControls();
    w.remember("venue", "pw");
    constexpr uint32_t kFirstRowId = 1;   // a fresh list numbers its first network 1
    CHECK(w.staticIpAt(0) == nullptr);
    mm::JsonSink dhcp;
    w.writeListRowDetail(dhcp, 0);
    CHECK(std::string(dhcp.data(), dhcp.size()).find("\"gateway\"") == std::string::npos);

    REQUIRE(w.setListRowField(kFirstRowId, "ipSettings", "{\"value\":1}"));
    REQUIRE(w.setListRowField(kFirstRowId, "ip", "{\"value\":\"10.0.0.7\"}"));
    CHECK_FALSE(w.setListRowField(kFirstRowId, "gateway", "{\"value\":\"10.0.0\"}"));
    REQUIRE(w.staticIpAt(0) != nullptr);
    CHECK(w.staticIpAt(0)[3] == 7);
    mm::JsonSink detail;
    w.writeListRowDetail(detail, 0);
    CHECK(std::string(detail.data(), detail.size()).find("\"gateway\"") != std::string::npos);
    CHECK(known(w, false).find("static 10.0.0.7") != std::string::npos);

    // The settings survive a save and a restore.
    mm::WiFiModule r;
    r.rebuildControls();
    REQUIRE(r.restoreList(saved(w).c_str(), "known"));
    REQUIRE(r.staticIpAt(0) != nullptr);
    CHECK(r.staticIpAt(0)[0] == 10);
}

// What is saved comes back, passwords and order included, and ids are never reissued after a restore.
TEST_CASE("known networks survive a save and a restore") {
    mm::WiFiModule w;
    w.rebuildControls();
    w.remember("second", "pw2");
    w.remember("first", "pw1");
    const std::string json = saved(w);

    mm::WiFiModule r;
    r.rebuildControls();
    REQUIRE(r.restoreList(json.c_str(), "known"));
    REQUIRE(r.knownCount() == 2);
    CHECK(std::strcmp(r.ssidAt(0), "first") == 0);
    CHECK(std::strcmp(r.passwordAt(0), "pw1") == 0);
    CHECK(std::strcmp(r.ssidAt(1), "second") == 0);
    uint32_t fresh = 0;
    REQUIRE(r.addListRow(fresh));
    CHECK(json.find("\"id\":" + std::to_string(fresh) + ",") == std::string::npos);
}

// The list is bounded, since every row is RAM a board without PSRAM pays for.
TEST_CASE("the known list stops at its cap, and a new network replaces the lowest-priority one") {
    mm::WiFiModule w;
    w.rebuildControls();
    uint32_t id = 0;
    int added = 0;
    while (w.addListRow(id) && added < 100) added++;
    CHECK(added == 8);
    // A network given now (Improv, a join from the scan) is wanted first, so the lowest-priority one makes room, and the card says which.
    REQUIRE(w.setListRowField(id, "ssid", "{\"value\":\"last\"}"));
    CHECK(w.remember("one more", "pw"));
    CHECK(w.knownCount() == 8);
    CHECK(std::strcmp(w.ssidAt(0), "one more") == 0);
    for (uint8_t k = 0; k < w.knownCount(); k++) CHECK(std::strcmp(w.ssidAt(k), "last") != 0);
    REQUIRE(w.status() != nullptr);
    CHECK(std::string(w.status()).find("last was forgotten") != std::string::npos);
}

// A row whose Static setting cannot work joins by DHCP, and its row says why.
TEST_CASE("a known network with an unusable Static setting says so on its row and joins by DHCP") {
    mm::WiFiModule w;
    w.rebuildControls();
    uint32_t id = 0;
    REQUIRE(w.addListRow(id));
    REQUIRE(w.setListRowField(id, "ssid", "{\"value\":\"home\"}"));
    REQUIRE(w.setListRowField(id, "ipSettings", "{\"value\":1}"));
    REQUIRE(w.setListRowField(id, "ip", "{\"value\":\"192.168.1.9\"}"));
    REQUIRE(w.setListRowField(id, "gateway", "{\"value\":\"192.168.2.1\"}"));
    CHECK(w.staticIpAt(0) == nullptr);
    mm::JsonSink sink;
    w.writeListRow(sink, 0);
    CHECK(std::string(sink.data(), sink.size()).find("\"ipSettings\":\"static IP not used: the gateway is outside the subnet\"") != std::string::npos);
}

namespace {
// Two known networks, the first joined on a lease of 192.168.8.158/24 via 192.168.8.1 with no DNS server named; the seams reset when it ends.
struct OnLease {
    static constexpr uint8_t kIp[4] = {192, 168, 8, 158};
    static constexpr uint8_t kGw[4] = {192, 168, 8, 1};
    static constexpr uint8_t kMask[4] = {255, 255, 255, 0};
    mm::WiFiModule w;
    uint32_t joined = 0, other = 0;
    OnLease() {
        mm::platform::setTestWifiStaIPv4(kIp);
        mm::platform::setTestNetLease(kGw, kMask, nullptr);
        w.rebuildControls();
        REQUIRE(w.addListRow(joined));
        REQUIRE(w.setListRowField(joined, "ssid", "{\"value\":\"home\"}"));
        REQUIRE(w.addListRow(other));
        REQUIRE(w.setListRowField(other, "ssid", "{\"value\":\"away\"}"));
        w.showRadio(-50, 20, true, true, joined);
    }
    ~OnLease() {
        mm::platform::setTestWifiStaIPv4(nullptr);
        mm::platform::setTestNetLease(nullptr, nullptr, nullptr);
    }
};
}  // namespace

// Switching the joined network to Static fills its fields from the lease it runs with, so the device keeps its address and the page stays.
TEST_CASE("the joined network switched to Static starts from its lease") {
    OnLease l;
    auto& w = l.w;
    const uint32_t joined = l.joined, other = l.other;
    const uint8_t* ip = OnLease::kIp;
    const uint8_t* gw = OnLease::kGw;
    const uint8_t* mask = OnLease::kMask;
    REQUIRE(w.setListRowField(joined, "ipSettings", "{\"value\":1}"));
    const mm::IpSettings& s = w.ipAt(0);
    CHECK(s.usable());
    CHECK(std::memcmp(s.ip, ip, 4) == 0);
    CHECK(std::memcmp(s.gateway, gw, 4) == 0);
    CHECK(std::memcmp(s.subnet, mask, 4) == 0);
    CHECK(std::memcmp(s.dns, gw, 4) == 0);   // the gateway stands in for a missing DNS server
    REQUIRE(w.setListRowField(other, "ipSettings", "{\"value\":1}"));
    CHECK_FALSE(w.ipAt(1).usable());          // not the joined network: its lease is not this one's to copy
}

// A row field carries its default as a control does, so the page offers the same reset: DHCP, and for the joined network its lease.
TEST_CASE("a known network's fields carry their defaults, the joined one's address fields its lease") {
    OnLease l;
    auto& w = l.w;
    const uint32_t joined = l.joined, other = l.other;
    REQUIRE(w.setListRowField(joined, "ipSettings", "{\"value\":1}"));
    REQUIRE(w.setListRowField(joined, "ip", "{\"value\":\"192.168.8.200\"}"));
    REQUIRE(w.setListRowField(other, "ipSettings", "{\"value\":1}"));
    const auto detail = [&](uint8_t row) { mm::JsonSink sink; w.writeListRowDetail(sink, row); return std::string(sink.data(), sink.size()); };
    const std::string j = detail(0), o = detail(1);
    CHECK(j.find("\"name\":\"ipSettings\",\"type\":\"select\",\"value\":1,\"default\":0") != std::string::npos);
    CHECK(j.find("\"name\":\"ip\",\"type\":\"ipv4\",\"value\":\"192.168.8.200\",\"default\":\"192.168.8.158\"") != std::string::npos);
    CHECK(j.find("\"name\":\"dns\",\"type\":\"ipv4\",\"value\":\"192.168.8.1\",\"default\":\"192.168.8.1\"") != std::string::npos);
    CHECK(o.find("\"name\":\"ip\",\"type\":\"ipv4\",\"value\":\"0.0.0.0\",\"default\":\"0.0.0.0\"") != std::string::npos);
    CHECK(o.find("\"name\":\"subnet\",\"type\":\"ipv4\",\"value\":\"255.255.255.0\",\"default\":\"255.255.255.0\"") != std::string::npos);
}

// The list is the priority, and a recent scan moves only the networks it missed to the end, so no time goes to what is not there.
TEST_CASE("the join order puts the networks a recent scan saw first, each part in list order") {
    struct Reset { ~Reset() { mm::platform::setTestNowMs(0); mm::platform::setTestWifiScan(nullptr, 0); } } reset;
    mm::platform::setTestNowMs(1000);
    mm::WiFiModule w;
    w.rebuildControls();
    for (const char* name : {"Alpha", "Bravo", "Charlie"}) {
        uint32_t id = 0;
        REQUIRE(w.addListRow(id));
        REQUIRE(w.setListRowField(id, "ssid", (std::string("{\"value\":\"") + name + "\"}").c_str()));
    }
    uint8_t order[mm::WiFiModule::kMaxKnown];
    REQUIRE(w.joinOrder(order, 1000) == 3);
    CHECK((order[0] == 0 && order[1] == 1 && order[2] == 2));   // no scan yet: the list order
    const mm::platform::WifiNetwork seen[] = {{"Charlie", -50, true}, {"Alpha", -70, true}};
    mm::platform::setTestWifiScan(seen, 2);
    w.onControlChanged("scan");
    w.tick1s();
    REQUIRE(w.joinOrder(order, 2000) == 3);
    CHECK((order[0] == 0 && order[1] == 2 && order[2] == 1));   // Alpha, Charlie seen in list order, then Bravo
    REQUIRE(w.joinOrder(order, 1000 + mm::WiFiModule::kScanFreshMs + 1) == 3);
    CHECK((order[0] == 0 && order[1] == 1 && order[2] == 2));   // a stale scan orders nothing
}

TEST_CASE("a full known list never forgets the network carrying the device") {
    mm::WiFiModule w;
    w.rebuildControls();
    uint32_t id = 0, before = 0;
    while (w.knownCount() < 8) { before = id; REQUIRE(w.addListRow(id)); }
    REQUIRE(w.setListRowField(before, "ssid", "{\"value\":\"spare\"}"));
    REQUIRE(w.setListRowField(id, "ssid", "{\"value\":\"carrier\"}"));
    w.showRadio(-50, 20, true, true, id);   // the lowest-priority row carries the device
    CHECK(w.remember("one more", "pw"));
    bool carrierKept = false, spareKept = false;
    for (uint8_t k = 0; k < w.knownCount(); k++) {
        carrierKept |= std::strcmp(w.ssidAt(k), "carrier") == 0;
        spareKept |= std::strcmp(w.ssidAt(k), "spare") == 0;
    }
    CHECK(carrierKept);
    CHECK_FALSE(spareKept);
    CHECK(std::string(w.status()).find("spare was forgotten") != std::string::npos);
}

namespace {
/// A list control's source by name, the way the API reaches it.
mm::ListSource* listOf(mm::WiFiModule& w, const char* name) {
    for (uint8_t i = 0; i < w.controls().count(); i++)
        if (std::strcmp(w.controls()[i].name, name) == 0) return static_cast<mm::ListSource*>(w.controls()[i].ptr);
    return nullptr;
}
std::string rowText(mm::ListSource& l, uint8_t row) {
    mm::JsonSink sink;
    l.writeListRow(sink, row);
    return std::string(sink.data(), sink.size());
}
mm::platform::WifiNetwork net(const char* ssid, int8_t rssi, bool secured) {
    mm::platform::WifiNetwork n{};
    std::snprintf(n.ssid, sizeof(n.ssid), "%s", ssid);
    n.rssi = rssi;
    n.secured = secured;
    return n;
}
}  // namespace

// The scan lists what a WiFi picker lists: named networks once each at their strongest, signal as bars, a lock on secured ones, and the known ones marked.
TEST_CASE("a scan lists the networks in range as a WiFi picker shows them") {
    const mm::platform::WifiNetwork found[] = {
        net("home", -50, true), net("", -55, true), net("cafe", -70, false), net("home", -80, true)};
    mm::platform::setTestWifiScan(found, 4);
    mm::WiFiModule w;
    w.rebuildControls();
    w.remember("home", "pw");
    w.onControlChanged("scan");
    w.tick1s();   // starts the scan
    w.tick1s();   // collects it
    mm::ListSource* avail = listOf(w, "available");
    REQUIRE(avail != nullptr);
    REQUIRE(avail->listRowCount() == 2);   // the hidden one and the weaker duplicate are left out
    const std::string home = rowText(*avail, 0);
    CHECK(home.find("\"ssid\":\"home\"") != std::string::npos);
    CHECK(home.find("▂▄▆█") != std::string::npos);
    CHECK(home.find("🔒") != std::string::npos);
    CHECK(home.find("known") != std::string::npos);
    const std::string cafe = rowText(*avail, 1);
    CHECK(cafe.find("🔒") == std::string::npos);
    CHECK_FALSE(avail->persistsList());   // a scan is never saved
    CHECK(avail->listRowsFixed());        // nothing adds, deletes or moves its rows
    mm::platform::setTestWifiScan(nullptr, 0);
}

// A scan whose results never arrive here, because the radio stopped or Improv took them, ends after its time rather than reading "scanning" until a restart.
TEST_CASE("a scan that never finishes ends after its time") {
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } guard;
    mm::platform::setTestNowMs(1000);
    mm::platform::setTestWifiScan(nullptr, -1);   // the radio never reports this scan done
    mm::WiFiModule w;
    w.rebuildControls();
    w.onControlChanged("scan");
    w.tick1s();
    mm::platform::setTestNowMs(5000);
    w.tick1s();
    REQUIRE(w.status() != nullptr);
    CHECK(std::string(w.status()).find("scanning") != std::string::npos);
    mm::platform::setTestNowMs(20000);
    w.tick1s();
    CHECK(std::string(w.status()).find("did not finish") != std::string::npos);
    mm::platform::setTestWifiScan(nullptr, 0);
}

// A radio still starting or joining refuses a scan, and pressing scan then must not be lost or blamed on a radio that is off.
TEST_CASE("a scan the radio cannot start yet waits for it and starts once it can") {
    struct Reset { ~Reset() { mm::platform::setTestWifiScanRefused(false); mm::platform::setTestWifiScan(nullptr, 0); } } reset;
    mm::platform::setTestWifiScanRefused(true);
    mm::WiFiModule w;
    w.rebuildControls();
    w.onControlChanged("scan");
    w.tick1s();
    REQUIRE(w.status() != nullptr);
    CHECK(std::string(w.status()) == "the scan waits for the radio");
    mm::platform::setTestWifiScanRefused(false);
    w.tick1s();   // the desktop's scan finishes in the tick it starts
    const char* scanned = nullptr;
    for (uint8_t i = 0; i < w.controls().count(); i++)
        if (std::strcmp(w.controls()[i].name, "scanned") == 0) scanned = static_cast<const char*>(w.controls()[i].ptr);
    REQUIRE(scanned != nullptr);
    CHECK(std::string(scanned) == "just now");
}

// A radio that stays off, as while Ethernet carries the device, never takes the request, so the card ends it rather than waiting forever.
TEST_CASE("a scan the radio refuses for a scan's whole time ends with a reason") {
    struct Reset { ~Reset() { mm::platform::setTestWifiScanRefused(false); mm::platform::setTestNowMs(0); } } reset;
    mm::platform::setTestNowMs(1000);
    mm::platform::setTestWifiScanRefused(true);
    mm::WiFiModule w;
    w.rebuildControls();
    w.onControlChanged("scan");
    mm::platform::setTestNowMs(15000);
    w.tick1s();
    REQUIRE(w.status() != nullptr);
    CHECK(std::string(w.status()) == "the scan waits for the radio");
    mm::platform::setTestNowMs(17000);
    w.tick1s();
    CHECK(std::string(w.status()) == "the radio cannot scan now: press scan to try again");
    mm::platform::setTestWifiScanRefused(false);
    mm::platform::setTestNowMs(18000);
    w.tick1s();
    CHECK(std::string(w.status()) == "the radio cannot scan now: press scan to try again");   // the ended request does not start on its own
}

// The list shows a known network's password as the API shows a Password control's: obfuscated, never as typed, while the saved file keeps it as it is.
TEST_CASE("a known network's password reaches the page obfuscated") {
    mm::WiFiModule w;
    w.rebuildControls();
    uint32_t a = 0;
    REQUIRE(w.addListRow(a));
    REQUIRE(w.setListRowField(a, "ssid", "{\"value\":\"workshop\"}"));
    REQUIRE(w.setListRowField(a, "password", "{\"value\":\"pw1\"}"));
    mm::JsonSink detail;
    w.writeListRowDetail(detail, 0);
    const std::string shown(detail.data(), detail.size());
    CHECK(shown.find("pw1") == std::string::npos);
    CHECK(shown.find("\"type\":\"password\",\"value\":\"Ki1r\"") != std::string::npos);   // "pw1" XOR 0x5A, base64
    CHECK(saved(w).find("pw1") != std::string::npos);
}
