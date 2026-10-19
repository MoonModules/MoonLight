/// @module HttpServerModule
///
/// The module routes read a name the way the client encodes it, and every upload route asks for its length, driven over loopback.

#include "doctest.h"
#include "core/module/Scheduler.h"
#include "core/util/ModuleFactory.h"
#include "core/system/HttpServerModule.h"
#include "platform/platform.h"
#include "http_loopback.h"

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
TEST_CASE("a module route whose name does not fit is refused with 400 on move, replace and delete alike") {
    mm::Scheduler s;
    auto* rack = new Rack();
    rack->setName("Rack");
    s.addModule(rack);
    mm::HttpServerModule http;
    http.port = kTestPort;
    http.setScheduler(&s);
    http.setup();
    const std::string longName(40, 'x');
    CHECK(request(http, withBody(("POST /api/modules/" + longName + "/move").c_str(), "{\"to\":0}")).rfind("HTTP/1.1 400", 0) == 0);
    CHECK(request(http, withBody(("POST /api/modules/" + longName + "/replace").c_str(), "{\"type\":\"RouteDial\"}")).rfind("HTTP/1.1 400", 0) == 0);
    CHECK(request(http, "DELETE /api/modules/" + longName + " HTTP/1.1\r\n\r\n").rfind("HTTP/1.1 400", 0) == 0);
    http.release();
    s.deleteTree(rack);
}
