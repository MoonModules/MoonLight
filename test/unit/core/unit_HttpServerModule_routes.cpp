/// @module HttpServerModule
///
/// The module routes read a name the way the client encodes it, and every upload route asks for its length, driven over loopback.

#include "doctest.h"
#include "core/util/JsonUtil.h"   // the type list parses as one document
#include "core/module/Scheduler.h"
#include "core/util/ModuleFactory.h"
#include "core/system/HttpServerModule.h"
#include "core/system/DevicesModule.h"   // the boards a hosts text names
#include "core/system/WledPacket.h"      // a board announcing itself
#include "platform/platform.h"
#include "http_loopback.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace {

constexpr uint16_t kTestPort = 18094;   // 18090 to 18093 belong to the connections, SD card, subscription and upload tests
using namespace http_loopback;

struct Dial : public mm::MoonModule {};
struct Rack : public mm::MoonModule {
    const char* acceptsChildRoles() const override { return "generic"; }
};

// Sends one raw request and returns the reply once its status line has arrived.
std::string request(mm::HttpServerModule& http, const std::string& req) {
    mm::platform::TcpConnection client;
    if (!connectAndAccept(http, client, kTestPort)) return {};
    if (!client.write(reinterpret_cast<const uint8_t*>(req.data()), req.size())) return {};
    std::string reply = pumpUntil(http, client, 12);
    client.close();
    return reply;
}

std::string withBody(const char* head, const std::string& body) {
    return std::string(head) + " HTTP/1.1\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

/// A scheduler holding one `Rack`, served on the test port.
struct RackServer {
    mm::Scheduler s;
    Rack* rack = new Rack();
    mm::HttpServerModule http;
    RackServer() {
        rack->setName("Rack");
        s.addModule(rack);
        http.port = kTestPort;
        http.setScheduler(&s);
        http.setup();
    }
    ~RackServer() {
        http.release();
        s.deleteTree(rack);
    }
};

/// What a request whose body comes after its headers met: the slowest tick while the body was held back, and the reply once it came.
struct LateBody { uint32_t slowestTickMs = 0; std::string reply; };

/// Send `method path` with its headers alone, tick while the body is held back, then send `body` and read the reply.
LateBody sendBodyLate(mm::HttpServerModule& http, const std::string& request, const std::string& body) {
    LateBody out;
    const std::string head = request + " HTTP/1.1\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    mm::platform::TcpConnection client;
    if (!connectAndAccept(http, client, kTestPort)) return out;
    if (!client.write(reinterpret_cast<const uint8_t*>(head.data()), head.size())) return out;
    for (int i = 0; i < 20; i++) {   // the headers are read, and the body has not come
        const uint32_t start = mm::platform::millis();
        http.tick20ms();
        out.slowestTickMs = std::max(out.slowestTickMs, mm::platform::millis() - start);
        mm::platform::delayMs(1);
    }
    if (!client.write(reinterpret_cast<const uint8_t*>(body.data()), body.size())) return out;
    out.reply = pumpUntil(http, client, 12);
    client.close();
    return out;
}

/// A module with one control a route can set.
struct Level : public mm::MoonModule {
    uint8_t level = 0;
    void defineControls() override { controls_.addControl("level", level, 0, 255); }
};

// A module with a hosts text, declared as a service declares one.
struct Sender : public mm::MoonModule {
    char hosts[64] = "192.168.1.125, MM-Eye, 10.0.0.9";
    void defineControls() override {
        controls_.addText("hosts", hosts, sizeof(hosts));
        controls_.setHostsField(controls_.count() - 1);
    }
};

// A board announcing itself under `name` from 192.168.1.`last`.
void announce(mm::DevicesModule& devices, const char* name, uint8_t last) {
    const uint8_t ip[4] = {192, 168, 1, last};
    uint8_t pkt[mm::WledPacket::kSize];
    mm::WledPacket::build(pkt, ip, name, /*boardType=*/34, /*lightsOn=*/true);
    mm::WledPacket::stampMmMarker(pkt);
    devices.injectPacketForTest(pkt, sizeof(pkt), ip);
}

mm::MoonModule* childNamed(mm::MoonModule* parent, const char* name) {
    for (uint8_t i = 0; i < parent->childCount(); i++)
        if (parent->child(i) && std::strcmp(parent->child(i)->name(), name) == 0) return parent->child(i);
    return nullptr;
}

} // namespace

TEST_CASE("a module whose name holds a space is moved, replaced and deleted through its percent-encoded path, as it is read") {
    static bool registered = false;
    if (!registered) { mm::ModuleFactory::registerType<Dial>("RouteDial"); registered = true; }
    mm::Scheduler s;
    auto* rack = new Rack();
    rack->setName("Rack");
    s.addModule(rack);
    auto* a = new Dial(); a->setName("Dial A"); rack->addChild(a);
    auto* b = new Dial(); b->setName("Dial B"); rack->addChild(b);
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setScheduler(&s);
    http.setup();

    CHECK(request(http, withBody("POST /api/modules/Dial%20A/move", "{\"to\":1}")).rfind("HTTP/1.1 200", 0) == 0);
    CHECK(rack->child(1) == childNamed(rack, "Dial A"));
    CHECK(request(http, withBody("POST /api/modules/Dial%20B/replace", "{\"type\":\"RouteDial\",\"name\":\"Dial C\"}")).rfind("HTTP/1.1 200", 0) == 0);
    CHECK(childNamed(rack, "Dial C") != nullptr);
    CHECK(request(http, "DELETE /api/modules/Dial%20A HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 200", 0) == 0);
    CHECK(childNamed(rack, "Dial A") == nullptr);

    http.release();
    s.deleteTree(rack);
}

TEST_CASE("a firmware upload without a Content-Length is refused with 411, as every upload route refuses it") {
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setup();
    CHECK(request(http, "POST /api/firmware/upload HTTP/1.1\r\n\r\nxx").rfind("HTTP/1.1 411", 0) == 0);
    http.release();
}

// A name past what a module holds is refused on every route that names one, rather than cut short into a name that could match another module.
TEST_CASE("a module route whose name does not fit is refused with 400 on move, replace, delete and read alike") {
    RackServer server;
    mm::HttpServerModule& http = server.http;
    const std::string longName(40, 'x');
    CHECK(request(http, withBody(("POST /api/modules/" + longName + "/move").c_str(), "{\"to\":0}")).rfind("HTTP/1.1 400", 0) == 0);
    CHECK(request(http, withBody(("POST /api/modules/" + longName + "/replace").c_str(), "{\"type\":\"RouteDial\"}")).rfind("HTTP/1.1 400", 0) == 0);
    CHECK(request(http, "DELETE /api/modules/" + longName + " HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 400", 0) == 0);
    const std::string pastAName(mm::MoonModule::kNameLen + 4, 'y');   // fits the route, not a module's name
    CHECK(request(http, "GET /api/modules/" + pastAName + " HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 400", 0) == 0);
    CHECK(request(http, "DELETE /api/modules/" + pastAName + " HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 400", 0) == 0);
    CHECK(request(http, "GET /api/modules/Rack HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 200", 0) == 0);
}

// `?live=1` applies a document that marks nothing to save; it is read as a whole query parameter, so `xlive=1` and `live=10` are plain requests.
TEST_CASE("PATCH /api/state?live=1 applies without marking the tree to save, and only that exact parameter does") {
    static bool registered = false;
    if (!registered) { mm::ModuleFactory::registerType<Dial>("LiveDial"); registered = true; }
    RackServer server;
    mm::HttpServerModule& http = server.http;
    Rack* rack = server.rack;

    const std::string doc = R"({"Rack":{"A":{"type":"LiveDial"}}})";
    CHECK(request(http, withBody("PATCH /api/state?live=1", doc)).rfind("HTTP/1.1 200", 0) == 0);
    CHECK(childNamed(rack, "A") != nullptr);
    CHECK_FALSE(rack->dirty());
    for (const char* path : {"PATCH /api/state?xlive=1", "PATCH /api/state?live=10"}) {
        rack->clearDirty();
        const std::string another = std::string(R"({"Rack":{"B)") + (path[17] == 'x' ? "x" : "y") + R"(":{"type":"LiveDial"}}})";
        CHECK(request(http, withBody(path, another)).rfind("HTTP/1.1 200", 0) == 0);
        CHECK(rack->dirty());   // an ordinary request: saved
    }
}

// An ESP32 pushing a look sends its headers, then holds the small body back for the delayed ACK, and a lost segment comes a resend later. Every push once answered 400, then the eye froze waiting on the render loop.
TEST_CASE("a state document whose body comes after its headers applies, and the frame keeps running meanwhile") {
    static bool registered = false;
    if (!registered) { mm::ModuleFactory::registerType<Dial>("WaitDial"); registered = true; }
    RackServer server;
    const LateBody late = sendBodyLate(server.http, "PATCH /api/state?live=1", R"({"Rack":{"Waited":{"type":"WaitDial"}}})");
    CHECK(late.slowestTickMs < 20);
    CHECK(late.reply.rfind("HTTP/1.1 200", 0) == 0);
    CHECK(childNamed(server.rack, "Waited") != nullptr);
}

// The late-body race was fixed for state documents first, while every other route with a body still answered 400 on hardware: one rule now holds for each.
TEST_CASE("any request whose body comes after its headers is answered whole, and the frame keeps running meanwhile") {
    RackServer server;
    auto* knob = new Level();
    knob->setName("Knob");
    server.rack->addChild(knob);
    knob->rebuildControls();
    const LateBody late = sendBodyLate(server.http, "POST /api/control", R"({"module":"Knob","control":"level","value":7})");
    CHECK(late.slowestTickMs < 20);
    CHECK(late.reply.rfind("HTTP/1.1 200", 0) == 0);
    CHECK(knob->level == 7);
}

// A byte every few seconds once kept a slot forever, the stall guard seeing progress each time: a body now owes a minimum in every window.
TEST_CASE("a body that trickles a byte a second is answered 400 once its window ends, its slot free again") {
    struct Clock { ~Clock() { mm::platform::setTestNowMs(0); } } clock;
    mm::platform::setTestNowMs(1000);
    RackServer server;
    const std::string body(100, ' ');
    const std::string head = "POST /api/control HTTP/1.1\r\nContent-Length: 100\r\n\r\n";
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(server.http, client, kTestPort));
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(head.data()), head.size()));
    std::string reply;
    for (int second = 1; second <= 12 && reply.empty(); second++) {
        REQUIRE(client.write(reinterpret_cast<const uint8_t*>(body.data()), 1));
        mm::platform::setTestNowMs(1000 + second * 1000);
        for (int i = 0; i < 3; i++) { mm::platform::delayMs(1); server.http.tick20ms(); }
        char buf[256];
        const int n = client.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
        if (n > 0) reply.assign(buf, static_cast<size_t>(n));
    }
    CHECK(reply.rfind("HTTP/1.1 400", 0) == 0);
    client.close();
}

namespace {
// Sends one raw request and reads until the server closes, the whole answer.
std::string requestWhole(mm::HttpServerModule& http, const std::string& req) {
    mm::platform::TcpConnection client;
    if (!connectAndAccept(http, client, kTestPort)) return {};
    if (!client.write(reinterpret_cast<const uint8_t*>(req.data()), req.size())) return {};
    std::string out = readUntilClosed(http, client);
    client.close();
    return out;
}
}  // namespace

// Every page load built every registered module to read its defaults, 0.6 s and a burst of allocations on a classic ESP32, which once ran it out of memory.
TEST_CASE("the type list arrives whole with its tag, and a browser holding that tag is answered 304 with no body") {
    if (mm::ModuleFactory::typeCount() == 0) mm::ModuleFactory::registerType<Dial>("RouteDial");
    RackServer server;
    const std::string full = requestWhole(server.http, "GET /api/types HTTP/1.1\r\n\r\n");
    REQUIRE(full.rfind("HTTP/1.1 200", 0) == 0);
    const size_t at = full.find("\r\n\r\n");
    REQUIRE(at != std::string::npos);
    mm::json::JsonDoc doc;
    CHECK(mm::json::parse(full.c_str() + at + 4, doc));
    for (uint8_t i = 0; i < mm::ModuleFactory::typeCount(); i++)
        if (const char* name = mm::ModuleFactory::typeName(i))
            CHECK(full.find(std::string("\"name\":\"") + name + "\"") != std::string::npos);
    const size_t tagAt = full.find("ETag: ");
    REQUIRE(tagAt != std::string::npos);
    const std::string tag = full.substr(tagAt + 6, full.find("\r\n", tagAt) - tagAt - 6);

    const std::string again = requestWhole(server.http, "GET /api/types HTTP/1.1\r\nIf-None-Match: " + tag + "\r\n\r\n");
    CHECK(again.rfind("HTTP/1.1 304", 0) == 0);
    CHECK(again.find("\r\n\r\n") + 4 == again.size());   // headers only
}

// An address says nothing about which board it is: the state names each board the device list knows, and leaves a desk or a computer alone.
TEST_CASE("a hosts text carries the boards the device list knows among its entries, each address with its name and each name with its address") {
    RackServer server;
    mm::DevicesModule devices;
    devices.setup();
    devices.applyState();
    announce(devices, "MM-S31", 125);
    announce(devices, "MM-Eye", 30);
    auto* sender = new Sender();
    sender->setName("Sender");
    server.rack->addChild(sender);
    sender->rebuildControls();
    mm::platform::TcpConnection client;
    REQUIRE(connectAndAccept(server.http, client, kTestPort));
    const char* req = "GET /api/state HTTP/1.1\r\nHost: test\r\n\r\n";
    REQUIRE(client.write(reinterpret_cast<const uint8_t*>(req), std::strlen(req)));
    const std::string reply = readUntilClosed(server.http, client);
    CHECK(reply.find(R"("known":{"192.168.1.125":"MM-S31","MM-Eye":"192.168.1.30"})") != std::string::npos);   // 10.0.0.9 is no board heard
    client.close();
    devices.release();
}
