/// @module HttpServerModule
///
/// The WLED API a MoonLight device answers, over loopback: the info object every route carries, and the segment's capability code.

#include "doctest.h"
#include "core/module/Scheduler.h"
#include "core/system/HttpServerModule.h"
#include "core/util/JsonUtil.h"
#include "http_loopback.h"

#include <string>

namespace {

using namespace http_loopback;
using mm::json::JsonDoc;
using mm::json::JsonNode;

constexpr uint16_t kTestPort = 18097;   // 18090 to 18096 belong to the other server tests

// A bare server on the test port: the WLED shim answers without a layer or a Drivers.
struct WledDevice {
    mm::Scheduler s;
    mm::HttpServerModule http;
    WledDevice() {
        http.port = kTestPort;
        http.setScheduler(&s);
        http.setup();
    }
    ~WledDevice() { http.release(); }
    // One request on its own connection, read until the server closes it, so headers and body both arrive.
    std::string get(const char* path) {
        mm::platform::TcpConnection client;
        if (!connectAndAccept(http, client, kTestPort)) return {};
        const std::string request = std::string("GET ") + path + " HTTP/1.1\r\nHost: test\r\n\r\n";
        if (!client.write(reinterpret_cast<const uint8_t*>(request.data()), request.size())) return {};
        std::string reply = readUntilClosed(http, client);
        client.close();
        return reply;
    }
    // The JSON body of `path`'s reply.
    bool json(const char* path, JsonDoc& doc) {
        const std::string reply = get(path);
        const size_t at = reply.find("\r\n\r\n");
        return at != std::string::npos && mm::json::parse(reply.c_str() + at + 4, doc);
    }
};

// The info keys python-wled and the WLED-Android app read, in a fixed order, so two objects compare as strings.
std::string infoKeys(JsonDoc& doc, const JsonNode* info) {
    std::string keys;
    for (const char* key : {"ver", "name", "mac", "ip", "leds", "wifi", "fs", "freeheap", "uptime", "ws", "fxcount", "palcount"})
        if (mm::json::member(doc, info, key)) keys += std::string(key) + ",";
    return keys;
}

}  // namespace

TEST_CASE("WLED API: /json, /json/si and /json/info carry the same info keys, since python-wled merges each poll of /json/si into the first /json") {
    WledDevice d;
    JsonDoc full, poll, bare;
    REQUIRE(d.json("/json", full));
    REQUIRE(d.json("/json/si", poll));
    REQUIRE(d.json("/json/info", bare));
    const std::string expected = infoKeys(full, mm::json::member(full, full.rootNode(), "info"));
    CHECK(expected == "ver,name,mac,ip,leds,wifi,fs,freeheap,uptime,ws,fxcount,palcount,");
    CHECK(infoKeys(poll, mm::json::member(poll, poll.rootNode(), "info")) == expected);
    CHECK(infoKeys(bare, bare.rootNode()) == expected);
}

TEST_CASE("WLED API: the segment carries its own capability code, which Home Assistant reads to create a light for it") {
    WledDevice d;
    JsonDoc doc;
    REQUIRE(d.json("/json/state", doc));
    const JsonNode* seg = mm::json::element(doc, mm::json::member(doc, doc.rootNode(), "seg"), 0);
    CHECK(mm::json::readInt(mm::json::member(doc, seg, "lc"), -1) == 1);   // RGB_COLOR
}
