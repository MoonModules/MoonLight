/// @module MoonLiveEffect
/// @also HttpServerModule
///
/// A MoonLive effect driven entirely over the REST API, as the moonlive-api process does it: added under a layer, pointed at a script, and the script's own control set, each read back.

#include "doctest.h"
#include "MoonLiveScriptFixture.h"
#include "../core/http_loopback.h"
#include "core/module/Scheduler.h"
#include "core/util/ModuleFactory.h"
#include "core/system/HttpServerModule.h"
#include "light/layers/Effects.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/moonlive/MoonLiveEffect.h"
#include <cstring>
#include <string>

namespace {

constexpr uint16_t kTestPort = 18095;   // 18090 to 18094 belong to the core HTTP server tests
using namespace http_loopback;

std::string request(mm::HttpServerModule& http, const std::string& head, const std::string& body) {
    return requestOnce(http, kTestPort, head + " HTTP/1.1\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body);
}

bool ok(const std::string& reply) { return reply.starts_with("HTTP/1.1 200"); }

/// A layout, a layer under Effects, and the server, built through the factory as a device builds them.
struct Device {
    mm::Scheduler s;
    mm::HttpServerModule http;
    mm::MoonModule* layouts = nullptr;
    mm::MoonModule* effects = nullptr;
    mm::MoonModule* layer = nullptr;

    Device() {
        mm::ModuleFactory::registerType<mm::Layouts>("Layouts");
        mm::ModuleFactory::registerType<mm::GridLayout>("GridLayout");
        mm::ModuleFactory::registerType<mm::Effects>("Effects");
        mm::ModuleFactory::registerType<mm::Layer>("Layer");
        mm::ModuleFactory::registerType<mm::MoonLiveEffect>("MoonLiveEffect");
        layouts = mm::ModuleFactory::create("Layouts");
        effects = mm::ModuleFactory::create("Effects");
        s.addModule(layouts);
        s.addModule(effects);
        auto* grid = mm::ModuleFactory::create("GridLayout");
        layouts->addChild(grid);
        layer = mm::ModuleFactory::create("Layer");
        layer->setName("Layer");
        effects->addChild(layer);
        s.setup();
        http.port = kTestPort;
        http.setScheduler(&s);
        http.setup();
    }
    ~Device() { http.release(); s.release(); }

    /// The layer's child of this name, or null.
    mm::MoonModule* find(const char* name) {
        for (uint8_t i = 0; i < layer->childCount(); i++)
            if (std::strcmp(layer->child(i)->name(), name) == 0) return layer->child(i);
        return nullptr;
    }
};

const void* controlPtr(mm::MoonModule& m, const char* name) {
    auto& cs = m.controls();
    for (uint8_t i = 0; i < cs.count(); i++)
        if (std::strcmp(cs[i].name, name) == 0) return cs[i].ptr;
    FAIL("no control named ", name);
    return nullptr;
}

}  // namespace

// The three calls the moonlive-api process makes, each answered 200 and each visible on the module after it.
// @step moonlive-api/add, moonlive-api/point, moonlive-api/control
TEST_CASE("a MoonLive effect is added, pointed at a script and its own control set, all over the REST API") {
    Device d;
    const char* script = mmWriteScript(
        "class Hello {\n"
        "  byte hue = 10;\n"
        "  void defineControls() { addControl(\"hue\", hue, 0, 255); }\n"
        "  void tick() { int n = width * height; for (int i = 0; i < n; i = i + 1) { setRGB(i, hue, 0, 0); } }\n"
        "}\n");

    REQUIRE(ok(request(d.http, "POST /api/modules", R"({"type":"MoonLiveEffect","parent_id":"Layer"})")));
    mm::MoonModule* fx = d.find("MoonLive");
    REQUIRE(fx != nullptr);

    REQUIRE(ok(request(d.http, "POST /api/control",
                       std::string(R"({"module":"MoonLive","control":"script","value":")") + script + "\"}")));
    d.s.tick();   // the script compiles on the next frame, and its controls appear with it
    CHECK(std::string(static_cast<const char*>(controlPtr(*fx, "script"))) == script);

    REQUIRE(ok(request(d.http, "POST /api/control", R"({"module":"MoonLive","control":"hue","value":128})")));
    CHECK(*static_cast<const uint8_t*>(controlPtr(*fx, "hue")) == 128);
}
