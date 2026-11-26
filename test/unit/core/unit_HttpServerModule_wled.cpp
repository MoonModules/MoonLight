/// @module HttpServerModule
///
/// The WLED API a MoonLight device answers, over loopback: the info object every route carries, and a segment that is a layer with its first effect's sliders, read and written, and the routes that list and set them.

#include "doctest.h"
#include "core/module/Scheduler.h"
#include "core/system/HttpServerModule.h"
#include "core/util/JsonSink.h"
#include "core/util/JsonUtil.h"
#include "core/util/ModuleFactory.h"
#include "light/effects/EffectBase.h"   // a registered effect type is placed in a real layer by the module contract survey
#include "fake_light_output.h"   // the fake Drivers is the light output too
#include "http_loopback.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

using namespace http_loopback;
using mm::json::JsonDoc;
using mm::json::JsonNode;

constexpr uint16_t kTestPort = 18097;   // 18090 to 18096 belong to the other server tests

// The Drivers the WLED shim drives by name, and the light output a test claims to reach the palettes.
struct FakeDrivers : public mm::MoonModule, public mm::FakeLightOutput {
    bool on = true;
    uint8_t brightness = 20;
    uint8_t palette = 0;
    void defineControls() override {
        controls_.addControl("on", on);
        controls_.addControl("brightness", brightness, 0, 255);
        controls_.addControl("palette", palette, 0, 255);
    }
};

// The container of layers, which the shim finds by its type name.
struct FakeEffects : public mm::MoonModule {
    const char* acceptsChildRoles() const override { return "layer"; }
};

struct FakeLayer : public mm::MoonModule {
    uint8_t opacity = 255;
    mm::ModuleRole role() const MM_NONBLOCKING override { return mm::ModuleRole::Layer; }
    const char* acceptsChildRoles() const override { return "effect"; }
    void defineControls() override {
        controls_.addControl("opacity", opacity, 0, 255);
        MoonModule::defineControls();   // the effects below
    }
};

// An effect with five sliders and two options, so every WLED slot has a control.
struct SolidFx : public mm::EffectBase {
    uint8_t speed = 60, scale = 100, twist = 128, spin = 10, depth = 255;
    bool mirror = false, invert = true;
    void defineControls() override {
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("scale", scale, 0, 255);
        controls_.addControl("twist", twist, 0, 255);
        controls_.addControl("spin", spin, 0, 255);
        controls_.addControl("depth", depth, 0, 255);
        controls_.addControl("mirror", mirror);
        controls_.addControl("invert", invert);
    }
};

// An effect with one slider, so the slots after it have no control.
struct WaveFx : public mm::EffectBase {
    uint8_t bpm = 30;
    void defineControls() override { controls_.addControl("bpm", bpm, 10, 200); }
};

void registerTypes() {
    static bool done = false;
    if (done) return;
    mm::ModuleFactory::registerType<SolidFx>("WledSolidEffect");
    mm::ModuleFactory::registerType<WaveFx>("WledWaveEffect");
    done = true;
}

std::string bodyOf(const std::string& reply) {
    const size_t at = reply.find("\r\n\r\n");
    return at == std::string::npos ? std::string() : reply.substr(at + 4);
}

// Two layers, `A` enabled at opacity 200 and `B` disabled, each holding one SolidFx, and a Drivers beside them, served on the test port.
struct WledDevice {
    mm::Scheduler s;
    mm::MoonModule* effects = new FakeEffects();
    FakeDrivers* drivers = new FakeDrivers();
    mm::HttpServerModule http;
    explicit WledDevice(bool withEffect = true) {
        registerTypes();
        effects->setTypeName("Effects");
        effects->setName("Effects");
        for (int i = 0; i < 2; i++) {
            auto* layer = new FakeLayer();
            layer->setName(i == 0 ? "Layer A" : "Layer B");
            layer->setEnabled(i == 0);
            effects->addChild(layer);
            if (!withEffect && i == 1) continue;
            mm::MoonModule* fx = mm::ModuleFactory::create("WledSolidEffect");
            fx->setName(i == 0 ? "Fx A" : "Fx B");
            layer->addChild(fx);
        }
        drivers->setName("Drivers");
        s.addModule(effects);
        s.addModule(drivers);
        s.setup();   // defineControls on every module, so each control exists
        layer(0).opacity = 200;
        http.port = kTestPort;
        http.setScheduler(&s);
        http.setup();
    }
    ~WledDevice() {
        http.release();
        s.deleteTree(effects);
        s.deleteTree(drivers);
    }
    // Both answer a reference after a REQUIRE, so a missing module fails the test rather than dereferencing null.
    FakeLayer& layer(int i) {
        auto* l = static_cast<FakeLayer*>(effects->child(static_cast<uint8_t>(i)));
        REQUIRE(l);
        return *l;
    }
    mm::MoonModule& effectOf(int i) {
        mm::MoonModule* e = layer(i).child(0);
        REQUIRE(e);
        return *e;
    }
    void apply(const char* body) { http.applyWledState(body); }
    std::string state() {
        mm::JsonSink sink;
        http.writeWledStateForTest(sink);
        return sink.data();
    }
    // One request on its own connection, read until the server closes it, so headers and body both arrive.
    std::string exchange(const std::string& request) {
        mm::platform::TcpConnection client;
        if (!connectAndAccept(http, client, kTestPort)) return {};
        if (!client.write(reinterpret_cast<const uint8_t*>(request.data()), request.size())) return {};
        std::string reply = readUntilClosed(http, client);
        client.close();
        return reply;
    }
    std::string get(const char* path) { return exchange(std::string("GET ") + path + " HTTP/1.1\r\nHost: test\r\n\r\n"); }
    std::string post(const char* path, const std::string& body) {
        return exchange(std::string("POST ") + path + " HTTP/1.1\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
    }
    // The JSON body of `path`'s reply.
    bool json(const char* path, JsonDoc& doc) {
        return mm::json::parse(bodyOf(get(path)).c_str(), doc);
    }
};

// The info keys python-wled and the WLED-Android app read, in a fixed order, so two objects compare as strings.
std::string infoKeys(JsonDoc& doc, const JsonNode* info) {
    std::string keys;
    for (const char* key : {"ver", "name", "mac", "ip", "leds", "wifi", "fs", "freeheap", "uptime", "ws", "fxcount", "palcount"})
        if (mm::json::member(doc, info, key)) keys += std::string(key) + ",";
    return keys;
}

// The segment `i` of a state document, read by a real parser so a test pins the value and not the spelling.
struct Segment {
    JsonDoc doc;
    const JsonNode* seg = nullptr;
    Segment(const std::string& state, int i) {
        REQUIRE(mm::json::parse(state.c_str(), doc));
        seg = mm::json::element(doc, mm::json::member(doc, doc.rootNode(), "seg"), i);
    }
    long number(const char* key) const { return mm::json::readInt(mm::json::member(doc, seg, key), -999); }
    bool flag(const char* key) const { return mm::json::readBool(mm::json::member(doc, seg, key)); }
    std::string text(const char* key) const {
        char out[64];
        mm::json::readString(mm::json::member(doc, seg, key), out, sizeof(out));
        return out;
    }
};

// Where `name` sits in the effect list `/json` carries, which is the fx a segment names it by; -1 when it is not listed.
int fxOf(WledDevice& d, const char* name) {
    JsonDoc doc;
    REQUIRE(d.json("/json", doc));
    const JsonNode* effects = mm::json::member(doc, doc.rootNode(), "effects");
    for (int i = 0; const JsonNode* n = mm::json::element(doc, effects, i); i++) {
        char text[48];
        mm::json::readString(n, text, sizeof(text));
        if (std::strcmp(text, name) == 0) return i;
    }
    return -1;
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

TEST_CASE("WLED segments: each layer is a segment, carrying its enabled flag, opacity, name and effect index") {
    WledDevice d;
    Segment a(d.state(), 0), b(d.state(), 1);
    REQUIRE(a.seg != nullptr);
    REQUIRE(b.seg != nullptr);
    CHECK(a.number("id") == 0);
    CHECK(a.flag("on"));
    CHECK(a.number("bri") == 200);
    CHECK(a.text("n") == "Layer A");
    CHECK(a.number("fx") == fxOf(d, "WledSolid"));
    CHECK(b.number("id") == 1);
    CHECK_FALSE(b.flag("on"));
    CHECK(b.number("bri") == 255);
    CHECK(Segment(d.state(), 2).seg == nullptr);   // two layers, two segments
}

TEST_CASE("WLED segments: sx, ix, c1, c2, c3 and o1, o2 are the effect's first sliders and options in declaration order") {
    WledDevice d;
    Segment a(d.state(), 0);
    CHECK(a.number("sx") == 60);    // speed
    CHECK(a.number("ix") == 100);   // scale
    CHECK(a.number("c1") == 128);   // twist
    CHECK(a.number("c2") == 10);    // spin
    CHECK(a.number("c3") == 31);    // depth 255, which WLED bounds at 31
    CHECK_FALSE(a.flag("o1"));      // mirror
    CHECK(a.flag("o2"));            // invert
    CHECK_FALSE(a.flag("o3"));      // no third option
}

TEST_CASE("WLED segments: a segment carries its own capability code, which Home Assistant reads to create a light for it") {
    WledDevice d;
    for (int i = 0; i < 2; i++) CHECK(Segment(d.state(), i).number("lc") == 1);   // RGB_COLOR
}

TEST_CASE("WLED segments: a device with no layer reports one stand-in segment, sized to its lights") {
    mm::Scheduler s;
    mm::HttpServerModule http;
    http.setScheduler(&s);
    mm::JsonSink sink;
    http.writeWledStateForTest(sink);
    const Segment seg(sink.data(), 0);
    CHECK(seg.number("id") == 0);
    CHECK(seg.number("lc") == 1);
    CHECK(seg.number("start") == 0);
    CHECK(seg.number("len") == seg.number("stop"));
    CHECK(mm::json::arraySize(seg.doc, mm::json::member(seg.doc, seg.doc.rootNode(), "seg")) == 1);
}

TEST_CASE("WLED API: /json counts every layer as a segment and every effect type, and /json/info agrees") {
    WledDevice d;
    JsonDoc doc;
    REQUIRE(d.json("/json", doc));
    const JsonNode* info = mm::json::member(doc, doc.rootNode(), "info");
    CHECK(mm::json::readInt(mm::json::member(doc, mm::json::member(doc, info, "leds"), "maxseg")) == 2);
    CHECK(mm::json::readInt(mm::json::member(doc, info, "fxcount")) ==
          mm::json::arraySize(doc, mm::json::member(doc, doc.rootNode(), "effects")));
    CHECK(mm::json::arraySize(doc, mm::json::member(doc, mm::json::member(doc, doc.rootNode(), "state"), "seg")) == 2);
}

TEST_CASE("WLED segments: a write sets the layer and its effect, the slider clamped to the control's range") {
    WledDevice t;
    t.apply("{\"seg\":[{\"id\":1,\"on\":true,\"bri\":128,\"sx\":200,\"ix\":7,\"c1\":0,\"c2\":99,\"c3\":16,\"o1\":true,\"o2\":false}]}");
    CHECK(t.layer(1).enabled());
    CHECK(t.layer(1).opacity == 128);
    auto* fx = static_cast<SolidFx*>(&t.effectOf(1));
    CHECK(fx->speed == 200);
    CHECK(fx->scale == 7);
    CHECK(fx->twist == 0);
    CHECK(fx->spin == 99);
    CHECK(fx->depth == 131);   // 16 of WLED's 31, scaled to a byte
    CHECK(fx->mirror);
    CHECK_FALSE(fx->invert);
    t.apply("{\"seg\":[{\"id\":1,\"sx\":0}]}");
    CHECK(fx->speed == 1);     // speed's lowest value is 1
    t.apply("{\"seg\":[{\"id\":1,\"c3\":31}]}");
    CHECK(fx->depth == 255);
    CHECK(Segment(t.state(), 1).number("c3") == 31);
    // The other layer's effect is untouched.
    CHECK(static_cast<SolidFx*>(&t.effectOf(0))->speed == 60);
}

TEST_CASE("WLED segments: an element without an id takes its place in the array, and an object without one is the main segment") {
    WledDevice t;
    t.apply("{\"seg\":[{\"bri\":10},{\"bri\":20}]}");
    CHECK(t.layer(0).opacity == 10);
    CHECK(t.layer(1).opacity == 20);
    t.apply("{\"seg\":{\"bri\":30}}");
    CHECK(t.layer(0).opacity == 30);
    CHECK(t.layer(1).opacity == 20);
}

TEST_CASE("WLED segments: a segment past the last layer, a malformed body, a value of the wrong kind and a negative number change nothing") {
    WledDevice t;
    t.drivers->outputSeat_.claim();
    t.drivers->palette = 2;
    t.apply("{\"seg\":[{\"id\":9,\"on\":false,\"bri\":1,\"fx\":0,\"pal\":4}]}");
    t.apply("{\"seg\":[{\"id\":0,\"bri\":\"~-10\",\"sx\":\"fast\",\"on\":{}}]}");
    t.apply("{\"seg\":[{\"id\":-1,\"bri\":5,\"fx\":-3,\"pal\":-2,\"sx\":-5}]}");   // a negative id is the first segment by position, a negative value is no value
    t.apply("{\"seg\":[{\"id\":0,\"bri\":");
    t.apply("not json");
    t.apply("");
    t.apply(nullptr);
    CHECK(t.layer(0).enabled());
    CHECK(t.layer(0).opacity == 5);   // only the negative id's own bri applied, to the first segment by position
    CHECK(static_cast<SolidFx*>(&t.effectOf(0))->speed == 60);
    CHECK(t.drivers->palette == 2);
}

TEST_CASE("WLED segments: a value above its range is held to the range's top, and a slider to its control's own bounds") {
    WledDevice t;
    t.apply("{\"seg\":[{\"id\":0,\"bri\":999,\"sx\":0}]}");
    CHECK(t.layer(0).opacity == 255);
    CHECK(static_cast<SolidFx*>(&t.effectOf(0))->speed == 1);
}

TEST_CASE("WLED segments: a pal in the request wins over a col, which otherwise picks the nearest palette") {
    WledDevice t;
    t.drivers->outputSeat_.claim();
    t.apply("{\"seg\":[{\"id\":0,\"col\":[[0,0,255]]}]}");
    CHECK(t.drivers->palette == 8);   // pure blue alone picks the fake's palette 8
    t.apply("{\"seg\":[{\"id\":0,\"pal\":5,\"col\":[[0,0,255]]}]}");
    CHECK(t.drivers->palette == 5);
}

TEST_CASE("WLED segments: a device with no layer still applies a palette written to its stand-in segment") {
    registerTypes();
    mm::Scheduler s;
    auto* drivers = new FakeDrivers();
    drivers->setName("Drivers");
    s.addModule(drivers);
    s.setup();
    drivers->outputSeat_.claim();
    mm::HttpServerModule http;
    http.setScheduler(&s);
    http.applyWledState("{\"seg\":[{\"id\":0,\"pal\":5}]}");
    CHECK(drivers->palette == 5);
    s.deleteTree(drivers);
}

TEST_CASE("WLED segments: \"t\" toggles the device's power and a segment's visibility") {
    WledDevice t;
    t.apply("{\"on\":\"t\"}");
    CHECK_FALSE(t.drivers->on);
    t.apply("{\"on\":\"t\"}");
    CHECK(t.drivers->on);
    t.apply("{\"seg\":[{\"id\":0,\"on\":\"t\"}]}");
    CHECK_FALSE(t.layer(0).enabled());
    t.apply("{\"seg\":[{\"id\":0,\"on\":\"t\"}]}");
    CHECK(t.layer(0).enabled());
}

TEST_CASE("WLED segments: fx replaces the layer's effect in place, and the sliders in the same request reach the new one") {
    WledDevice t;
    const int wave = fxOf(t, "WledWave");
    REQUIRE(wave >= 0);
    char body[96];
    std::snprintf(body, sizeof(body), "{\"seg\":[{\"id\":0,\"fx\":%d,\"sx\":90}]}", wave);
    t.apply(body);
    REQUIRE(t.layer(0).childCount() == 1);
    CHECK(std::strcmp(t.effectOf(0).typeName(), "WledWaveEffect") == 0);
    CHECK(static_cast<WaveFx*>(&t.effectOf(0))->bpm == 90);
    CHECK(Segment(t.state(), 0).number("fx") == wave);
    // The same effect again changes no module.
    mm::MoonModule* before = &t.effectOf(0);
    t.apply(body);
    CHECK(&t.effectOf(0) == before);
    // The slots the new effect has no control for read 0, and writes to them are dropped.
    Segment a(t.state(), 0);
    CHECK(a.number("sx") == 90);
    CHECK(a.number("ix") == 0);
    t.apply("{\"seg\":[{\"id\":0,\"ix\":50,\"o1\":true}]}");
    CHECK(Segment(t.state(), 0).number("ix") == 0);
    // The other layer keeps its own effect.
    CHECK(std::strcmp(t.effectOf(1).typeName(), "WledSolidEffect") == 0);
    // An index past the last effect is ignored.
    t.apply("{\"seg\":[{\"id\":0,\"fx\":255}]}");
    CHECK(std::strcmp(t.effectOf(0).typeName(), "WledWaveEffect") == 0);
}

TEST_CASE("WLED segments: fx on a layer with no effect adds one") {
    WledDevice t(/*withEffect=*/false);
    REQUIRE(t.layer(1).childCount() == 0);
    Segment empty(t.state(), 1);
    CHECK(empty.number("sx") == 0);   // nothing to read
    char body[64];
    std::snprintf(body, sizeof(body), "{\"seg\":[{\"id\":1,\"fx\":%d}]}", fxOf(t, "WledSolid"));
    t.apply(body);
    REQUIRE(t.layer(1).childCount() == 1);
    CHECK(std::strcmp(t.effectOf(1).typeName(), "WledSolidEffect") == 0);
}

TEST_CASE("WLED segments: pal and a color reach the one palette, whichever segment names them, as hex or as an array") {
    WledDevice t;
    t.drivers->outputSeat_.claim();
    t.apply("{\"seg\":[{\"id\":1,\"pal\":3}]}");
    CHECK(t.drivers->palette == 3);
    t.apply("{\"seg\":[{\"id\":0,\"col\":[[0,0,255]]}]}");
    CHECK(t.drivers->palette == 8);    // pure blue is 240 degrees, the fake's palette 8
    t.apply("{\"seg\":[{\"id\":0,\"col\":[\"FF0000\"]}]}");
    CHECK(t.drivers->palette == 0);
    t.apply("{\"seg\":[{\"id\":0,\"col\":[\"00FF0000\"]}]}");   // WWRRGGBB: the white byte is dropped
    CHECK(t.drivers->palette == 0);
    t.apply("{\"seg\":[{\"id\":0,\"col\":[\"00FF00\"]}]}");
    CHECK(t.drivers->palette == 4);
    t.apply("{\"seg\":[{\"id\":0,\"col\":[\"zzzzzz\"]}]}");   // not hex: unchanged
    CHECK(t.drivers->palette == 4);
    // Every segment reports the one palette.
    CHECK(Segment(t.state(), 0).number("pal") == 4);
    CHECK(Segment(t.state(), 1).number("pal") == 4);
}

TEST_CASE("WLED API: /json/effects, /json/palettes and /json/fxdata list the effect types in fx order, with each effect's sliders named") {
    WledDevice t;
    for (const char* path : {"/json/effects", "/json/eff"}) {
        const std::string reply = t.get(path);
        CHECK(reply.find("200 OK") != std::string::npos);
        CHECK(bodyOf(reply).find("\"WledSolid\"") != std::string::npos);
    }
    CHECK(bodyOf(t.get("/json/palettes")) == "[]");   // no light output claimed yet
    t.drivers->outputSeat_.claim();
    CHECK(bodyOf(t.get("/json/pal")) == "[\"P0\"]");

    JsonDoc names, data;
    REQUIRE(mm::json::parse(bodyOf(t.get("/json/effects")).c_str(), names));
    REQUIRE(mm::json::parse(bodyOf(t.get("/json/fxdata")).c_str(), data));
    CHECK(mm::json::arraySize(data, data.rootNode()) == mm::json::arraySize(names, names.rootNode()));   // one entry per effect
    auto entry = [&](const char* effect) {
        char text[192];
        mm::json::readString(mm::json::element(data, data.rootNode(), fxOf(t, effect)), text, sizeof(text));
        return std::string(text);
    };
    CHECK(entry("WledSolid") == "speed,scale,twist,spin,depth,mirror,invert;!;!;12");   // flags 12: an effect drawing in two dimensions or more
    CHECK(entry("WledWave") == "bpm;!;!;12");
    CHECK(bodyOf(t.get("/json/fxda")) == bodyOf(t.get("/json/fxdata")));
}

TEST_CASE("WLED API: POST /json applies the body and answers success, or the whole device for \"v\":true") {
    WledDevice t;
    const std::string quiet = t.post("/json", "{\"bri\":77,\"seg\":[{\"id\":1,\"bri\":5}]}");
    CHECK(bodyOf(quiet) == "{\"success\":true}");
    CHECK(t.drivers->brightness == 77);
    CHECK(t.layer(1).opacity == 5);
    const std::string verbose = bodyOf(t.post("/json", "{\"v\":true,\"bri\":78}"));
    CHECK(t.drivers->brightness == 78);
    CHECK(verbose.find("\"state\":") != std::string::npos);
    CHECK(verbose.find("\"effects\":") != std::string::npos);
    // POST /json/state keeps answering the state.
    const std::string echoed = bodyOf(t.post("/json/state", "{\"bri\":79}"));
    CHECK(t.drivers->brightness == 79);
    CHECK(echoed.find("\"bri\":79") != std::string::npos);
}

TEST_CASE("WLED API: GET /win applies its keys, in the path or the query, to the main segment or the one SS names") {
    WledDevice t;
    t.drivers->outputSeat_.claim();
    const int wave = fxOf(t, "WledWave");
    REQUIRE(wave >= 0);

    const std::string reply = t.get("/win&A=64&T=0&SX=200&IX=9&FP=5&M1=1&M2=0");
    CHECK(reply.find("200 OK") != std::string::npos);
    CHECK(t.drivers->brightness == 64);
    CHECK_FALSE(t.drivers->on);
    CHECK(t.drivers->palette == 5);
    auto* fx = static_cast<SolidFx*>(&t.effectOf(0));
    CHECK(fx->speed == 200);
    CHECK(fx->scale == 9);
    CHECK(fx->mirror);        // M1 is the first option
    CHECK_FALSE(fx->invert);  // M2 the second
    CHECK(bodyOf(reply).find("\"bri\":64") != std::string::npos);   // the answer is the state

    char path[96];
    std::snprintf(path, sizeof(path), "/win?T=2&SS=1&SB=33&FX=%d&X1=40", wave);
    t.get(path);
    CHECK(t.drivers->on);                                           // toggled back on
    CHECK(t.layer(1).opacity == 33);
    CHECK(std::strcmp(t.effectOf(1).typeName(), "WledWaveEffect") == 0);
    CHECK(static_cast<SolidFx*>(&t.effectOf(0))->speed == 200);      // the first segment is untouched

    // A color by channel keeps the other channels of the current color; one in hex sets all three.
    t.get("/win&CL=0000FF");
    CHECK(t.drivers->palette == 8);
    t.get("/win&R=255&G=0&B=0");
    CHECK(t.drivers->palette == 0);
    // A relative step is not read, and a stray key changes nothing.
    t.get("/win&A=~10&ZZ=1&SX=abc");
    CHECK(t.drivers->brightness == 64);
    CHECK(static_cast<SolidFx*>(&t.effectOf(0))->speed == 200);
    // A hex color followed by another key is read to its end, and only hex digits reach the body.
    t.get("/win&CL=FF0000&A=70");
    CHECK(t.drivers->palette == 0);
    CHECK(t.drivers->brightness == 70);
    t.get("/win&CL=00FF00\",\"bri\":1&A=71");
    CHECK(t.drivers->brightness == 71);
}
