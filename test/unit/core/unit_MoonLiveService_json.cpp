/// @module MoonLiveService
/// @also JsonFetch, Scheduler

/// Pins a service script's `json` values end to end. A feed from the internet drives the surface, a document the script writes builds this device's look, and copied text goes to a second device. The network is a stand-in.

#include "doctest.h"
#include "moonlive_script_file.h"
#include "core/services/MoonLiveService.h"
#include "core/module/Scheduler.h"
#include "core/module/MoonModule.h"
#include "core/system/FilesystemModule.h"
#include "core/util/JsonFetch.h"
#include "core/util/ModuleFactory.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

using namespace mm;

namespace {

struct JmControl : public MoonModule {
    uint8_t switch1 = 0;
    void defineControls() override { controls_.addControl("switch1", switch1, 0, 1); }
};
struct JmKnob : public MoonModule {
    uint8_t value = 1;
    void defineControls() override { controls_.addControl("value", value, 0, 100); }
};
struct JmBox : public MoonModule {
    const char* acceptsChildRoles() const override { return "generic,effect"; }
};

/// What the stand-in network saw last, and what it answers a GET with.
struct Net {
    std::string method, url, body;
    std::string answer;
};
Net net;

int standIn(const char* method, const char* url, const char* reqBody, char* body, size_t bodyLen) {
    net.method = method;
    net.url = url;
    net.body = reqBody ? reqBody : "";
    const std::string reply = std::strcmp(method, "GET") == 0 ? net.answer : R"({"ok":true,"changes":1})";
    if (body && bodyLen) {
        if (reply.size() + 1 > bodyLen) return 0;
        std::strcpy(body, reply.c_str());
    }
    return 200;
}

struct Rig {
    Scheduler scheduler;
    FilesystemModule* fs = new FilesystemModule();
    JmControl* control = new JmControl();
    JmBox* effects = new JmBox();
    MoonLiveService* svc = new MoonLiveService();
    char root_[64];
    Rig() {
        static bool registered = false;
        if (!registered) { ModuleFactory::registerType<JmKnob>("JmKnob"); registered = true; }
        static unsigned seq = 0;
        std::snprintf(root_, sizeof(root_), "/tmp/mm_mls_json_%u", ++seq);
        std::filesystem::remove_all(root_);
        platform::fsSetRoot(root_);
        net = Net{};
        platform::setTestHttpFetch(&standIn);
        fs->setName("Filesystem");
        control->setName("Control");
        effects->setName("Effects");
        svc->setName("Script");
        scheduler.addModule(fs);
        scheduler.addModule(control);
        scheduler.addModule(effects);
        scheduler.addModule(svc);
        scheduler.setup();
    }
    ~Rig() {
        JsonFetch::instance().settle(2000);
        scheduler.release();
        platform::setTestHttpFetch(nullptr);
    }
    void run(const char* name, const char* script) {
        writeScript(name, script);
        svc->setScript(name);
        svc->prepare();
    }
    /// One tick of the service, then whatever it sent has its answer and the frame boundary has passed.
    void tick() {
        svc->tick20ms();
        JsonFetch::instance().settle(2000);
        scheduler.preparePending();
    }
    std::string status() const { return svc->status() ? svc->status() : ""; }
};

}  // namespace

TEST_CASE("a script reads a number from a fetched document and drives the surface with it") {
    Rig rig;
    net.answer = R"({"current":{"on":1,"temperature_2m":21.5}})";
    rig.run("t_weather.mls",
            "class Weather {\n"
            "  json w;\n"
            "  void tick20ms() {\n"
            "    w = getJson(\"https://api.example/weather\", 600);\n"
            "    if (jsonChanged(w)) {\n"
            "      fixed temp = jsonFixed(w, \"/current/temperature_2m\");\n"
            "      if (temp > 20.0) { setControl(\"switch1\", jsonInt(w, \"/current/on\")); }\n"
            "    }\n"
            "  }\n"
            "}\n");
    INFO(rig.status());
    rig.tick();   // asks
    rig.tick();   // reads the answer
    CHECK(net.url == "https://api.example/weather");
    CHECK(rig.control->switch1 == 1);
}

TEST_CASE("a script builds a module on its own device with one push, which marks nothing to save") {
    Rig rig;
    rig.run("t_show.mls",
            "class Show {\n"
            "  bool built = false;\n"
            "  void tick20ms() {\n"
            "    if (!built) {\n"
            "      pushJson(R\"({\"Effects\":{\"Knob\":{\"type\":\"JmKnob\",\"value\":42}}})\");\n"
            "      built = true;\n"
            "    }\n"
            "  }\n"
            "}\n");
    INFO(rig.status());
    rig.effects->clearDirty();
    rig.tick();
    MoonModule* knob = rig.scheduler.firstByName("Knob");
    REQUIRE(knob != nullptr);
    CHECK(static_cast<JmKnob*>(knob)->value == 42);
    CHECK_FALSE(rig.effects->dirty());
}

TEST_CASE("a script copies text between documents and sends the result to another device") {
    Rig rig;
    net.answer = R"({"results":[{"title":"Rocket \"Lab\" launches"}],"n":4})";
    rig.run("t_relay.mls",
            "class Relay {\n"
            "  json feed;\n"
            "  json show;\n"
            "  void tick20ms() {\n"
            "    feed = getJson(\"https://api.example/news\", 600);\n"
            "    if (jsonChanged(feed)) {\n"
            "      show = R\"({\"Effects\":{\"Text\":{\"text\":\"\"}}})\";\n"
            "      jsonCopy(show, \"/Effects/Text/text\", feed, \"/results/0/title\");\n"
            "      jsonSetInt(show, \"/Effects/Text/speed\", jsonInt(feed, \"/n\") + 1);\n"
            "      pushJsonTo(\"eye.local\", show);\n"
            "    }\n"
            "  }\n"
            "}\n");
    INFO(rig.status());
    rig.tick();
    rig.tick();
    CHECK(net.method == "PATCH");
    CHECK(net.url == "http://eye.local/api/state?live=1");
    CHECK(net.body == R"({"Effects":{"Text":{"text":"Rocket \"Lab\" launches","speed":5}}})");
}

TEST_CASE("a json value takes no arithmetic and a number is no document, named at compile time") {
    Rig rig;
    rig.run("t_bad1.mls", "class B {\n  json d;\n  void tick20ms() { int x = d + 1; }\n}\n");
    CHECK(rig.status().find("a json value is a document") != std::string::npos);
    rig.run("t_bad2.mls", "class B {\n  void tick20ms() { int n = 5; pushJson(n); }\n}\n");
    CHECK(rig.status().find("this is a json document") != std::string::npos);
    rig.run("t_bad3.mls", "class B {\n  json d = 3;\n  void tick20ms() { }\n}\n");
    CHECK(rig.status().find("a json member starts empty") != std::string::npos);
    rig.run("t_bad4.mls", "class B {\n  json d;\n  void defineControls() { addControl(\"d\", d, 0, 1); }\n  void tick20ms() { }\n}\n");
    CHECK(rig.status().find("a control binds") != std::string::npos);
    rig.run("t_bad5.mls", "class B {\n  void tick20ms() { json d = R\"(unterminated; }\n}\n");
    CHECK(rig.status().find("unterminated raw string") != std::string::npos);
}

TEST_CASE("a raw string keeps quotes and spans lines, and a long one costs only its own script") {
    Rig rig;
    std::string big = "class Big {\n  void tick20ms() {\n    json d = R\"({\n";
    for (int i = 0; i < 100; i++) big += "      \"key" + std::to_string(i) + "\": \"a value with \\\"quotes\\\"\",\n";
    big += "      \"end\": 1\n    })\";\n  }\n}\n";
    rig.run("t_big.mls", big.c_str());
    INFO(rig.status());
    CHECK(rig.status().find(" B") != std::string::npos);  // compiled: the status names its size
    CHECK(rig.status().find('@') == std::string::npos);   // and no error position
    rig.tick();
}

TEST_CASE("an edited script starts with no documents, and a removed one leaves none behind") {
    Rig rig;
    net.answer = R"({"v":1})";
    const char* script = "class F {\n  json w;\n  void tick20ms() { w = getJson(\"https://api.example/a\", 600); }\n}\n";
    rig.run("t_life.mls", script);
    rig.tick();
    // A new program frees the old one's documents, so the table does not fill edit by edit.
    for (int i = 0; i < JsonFetch::kMaxDocs + 2; i++) {
        rig.svc->onFileChanged(moonlive::kScriptDir);
        writeScript("t_life.mls", script);
        rig.svc->prepare();
        rig.tick();
    }
    CHECK(net.url == "https://api.example/a");
    JsonFetch& f = JsonFetch::instance();
    int dummy = 0;
    CHECK(f.literal(&dummy, R"({"room":true})") > 0);   // the table still has room
    f.release(&dummy);
}

TEST_CASE("a shipped script the device lacks is listed for an open page, and leaves the list when it lands") {
    Rig rig;
    moonlive::missingScripts()[0] = '\0';
    rig.svc->setScript("stadbeest-autopilot.mls");
    rig.svc->prepare();
    CHECK(rig.status().find("script missing") != std::string::npos);
    CHECK(std::string(moonlive::missingScripts()) == "stadbeest-autopilot.mls");
    // A name the firmware does not ship is the user's mistake, which no page can fix.
    rig.svc->setScript("t_nowhere.mls");
    rig.svc->prepare();
    CHECK(rig.status().find("script not found") != std::string::npos);
    CHECK(std::string(moonlive::missingScripts()) == "stadbeest-autopilot.mls");
    // The page writes the file, as its download does, and the next compile finds it.
    writeScript("stadbeest-autopilot.mls", "class A {\n  void tick20ms() { }\n}\n");
    rig.svc->setScript("stadbeest-autopilot.mls");
    rig.svc->onFileChanged(moonlive::kScriptDir);
    rig.svc->prepare();
    CHECK(std::string(moonlive::missingScripts()).empty());
}

TEST_CASE("the missing list holds each name once and drops any one of them") {
    moonlive::missingScripts()[0] = '\0';
    moonlive::noteScriptMissing("a.mle");
    moonlive::noteScriptMissing("b.mlp");
    moonlive::noteScriptMissing("a.mle");
    moonlive::noteScriptMissing("c.mls");
    CHECK(std::string(moonlive::missingScripts()) == "a.mle|b.mlp|c.mls");
    moonlive::noteScriptPresent("b.mlp");
    CHECK(std::string(moonlive::missingScripts()) == "a.mle|c.mls");
    moonlive::noteScriptPresent("c.mls");
    CHECK(std::string(moonlive::missingScripts()) == "a.mle");
    moonlive::noteScriptPresent("a.mle");
    CHECK(std::string(moonlive::missingScripts()).empty());
    // Past the room a name waits rather than cutting the list.
    for (int i = 0; i < 40; i++) moonlive::noteScriptMissing(("long-script-name-" + std::to_string(i) + ".mle").c_str());
    CHECK(std::strlen(moonlive::missingScripts()) < moonlive::kMissingText);
    moonlive::missingScripts()[0] = '\0';
}

// Only a compile dropped a name, so one whose module went before its download landed was fetched again every minute.
TEST_CASE("a missing script leaves the list when its file lands, whether or not anything compiles it") {
    moonlive::missingScripts()[0] = '\0';
    moonlive::noteScriptMissing("voronoi.mle");
    moonlive::noteScriptMissing("fire.mlp");
    moonlive::noteScriptLanded("/.moonlive/voronoi.mle");
    CHECK(std::string(moonlive::missingScripts()) == "fire.mlp");
    moonlive::noteScriptLanded("fire.mlp");
    CHECK(std::string(moonlive::missingScripts()).empty());
}
