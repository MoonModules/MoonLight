/// @module JsonFetch
/// @also Scheduler, platform

/// Pins the documents a script holds by handle: its own text parsed once, a feed kept fresh in the background, and a push answered by a reply. The network is a stand-in, so every case runs without one.

#include "doctest.h"
#include "core/util/JsonFetch.h"
#include "core/module/MoonModule.h"
#include "core/module/Scheduler.h"
#include "core/util/ModuleFactory.h"

#include <cstring>
#include <string>

using namespace mm;

namespace {

/// What the stand-in network saw and what it answers.
struct Net {
    int calls = 0;
    std::string method, url, body;
    std::string answer = R"({"current":{"temperature_2m":12.5}})";
    int status = 200;
};
Net net;

int standIn(const char* method, const char* url, const char* reqBody, char* body, size_t bodyLen) {
    net.calls++;
    net.method = method;
    net.url = url;
    net.body = reqBody ? reqBody : "";
    if (!net.status) return 0;
    if (body && bodyLen) {
        if (net.answer.size() + 1 > bodyLen) return 0;
        std::strcpy(body, net.answer.c_str());
    }
    return net.status;
}

/// Installs the stand-in and a clock the test moves, and frees whatever the test's owner held.
struct Fixture {
    int owner = 0;   ///< any address names an owner, as a script's arena does
    JsonFetch& f = JsonFetch::instance();
    Fixture() {
        net = Net{};
        platform::setTestHttpFetch(&standIn);
        platform::setTestNowMs(100000);
    }
    ~Fixture() {
        f.settle(2000);
        f.release(&owner);
        platform::setTestHttpFetch(nullptr);
        platform::setTestNowMs(0);
    }
};

struct JfKnob : public MoonModule {
    uint8_t value = 1;
    void defineControls() override { controls_.addControl("value", value, 0, 100); }
};
struct JfBox : public MoonModule {
    const char* acceptsChildRoles() const override { return "generic,effect"; }
};

}  // namespace

TEST_CASE("a script's own text parses once, and asking again answers the same document") {
    Fixture fx;
    const char* text = R"({"a":{"b":3}})";
    const int h = fx.f.literal(&fx.owner, text);
    REQUIRE(h > 0);
    CHECK(fx.f.literal(&fx.owner, text) == h);
    CHECK(json::readInt(json::at(*fx.f.doc(&fx.owner, h), "/a/b")) == 3);
    CHECK(fx.f.changed(&fx.owner, h));        // the first ask sees it arrive
    CHECK_FALSE(fx.f.changed(&fx.owner, h));
    CHECK(fx.f.literal(&fx.owner, "{not json") == 0);
}

TEST_CASE("a handle names nothing for another owner") {
    Fixture fx;
    int other = 0;
    const int h = fx.f.literal(&fx.owner, R"({"x":1})");
    REQUIRE(h > 0);
    CHECK(fx.f.doc(&other, h) == nullptr);
    CHECK(fx.f.doc(&fx.owner, 0) == nullptr);
    CHECK(fx.f.doc(&fx.owner, 999) == nullptr);
}

TEST_CASE("a feed answers at once and fills when its answer arrives") {
    Fixture fx;
    const int h = fx.f.fetch(&fx.owner, "https://api.example/weather", 600000);
    REQUIRE(h > 0);
    REQUIRE(fx.f.settle(2000));
    CHECK(net.method == "GET");
    CHECK(net.url == "https://api.example/weather");
    REQUIRE(fx.f.doc(&fx.owner, h) != nullptr);
    CHECK(json::readFixed(json::at(*fx.f.doc(&fx.owner, h), "/current/temperature_2m")) == 12 * 65536 + 32768);
    CHECK(fx.f.status(&fx.owner, h) == 200);
    CHECK(fx.f.changed(&fx.owner, h));
    // Asking every tick neither adds a feed nor fetches again inside the period.
    CHECK(fx.f.fetch(&fx.owner, "https://api.example/weather", 600000) == h);
    fx.f.settle(2000);
    CHECK(net.calls == 1);
}

TEST_CASE("a feed refreshes once its period ran out, and a failed refresh keeps the last document") {
    Fixture fx;
    const int h = fx.f.fetch(&fx.owner, "https://api.example/news", 60000);
    REQUIRE(fx.f.settle(2000));
    CHECK(fx.f.changed(&fx.owner, h));
    net.status = 0;   // the network goes away
    platform::setTestNowMs(100000 + 60000);
    fx.f.doc(&fx.owner, h);   // a read past the period asks again
    REQUIRE(fx.f.settle(2000));
    CHECK(net.calls == 2);
    CHECK(fx.f.status(&fx.owner, h) == 0);
    REQUIRE(fx.f.doc(&fx.owner, h) != nullptr);   // stale, and still there
    CHECK_FALSE(fx.f.changed(&fx.owner, h));
}

// A feed whose server answered an error in JSON took the error as its new document and stopped retrying.
TEST_CASE("a feed answered with an error status keeps its last document and retries soon") {
    Fixture fx;
    const int h = fx.f.fetch(&fx.owner, "https://api.example/news", 60000);
    REQUIRE(fx.f.settle(2000));
    REQUIRE(fx.f.changed(&fx.owner, h));
    net.status = 503;
    net.answer = R"({"error":"busy"})";
    platform::setTestNowMs(100000 + 60000);
    fx.f.doc(&fx.owner, h);
    REQUIRE(fx.f.settle(2000));
    CHECK(fx.f.status(&fx.owner, h) == 503);
    const json::JsonDoc* doc = fx.f.doc(&fx.owner, h);
    REQUIRE(doc != nullptr);
    CHECK_FALSE(fx.f.changed(&fx.owner, h));   // still the good document
    net.status = 200;
    platform::setTestNowMs(100000 + 60000 + 5000);
    fx.f.doc(&fx.owner, h);
    fx.f.settle(2000);
    CHECK(net.calls == 3);   // asked again 5 s later, not a period later
}

// A classic ESP32 showed its weather placeholder for ten minutes after a reboot: the first fetch went before WiFi was up, and a failure waited out the whole period.
TEST_CASE("a failed fetch is asked again within seconds, backing off, and a success returns to the period") {
    Fixture fx;
    net.status = 0;   // no network yet
    const int h = fx.f.fetch(&fx.owner, "https://api.example/weather", 600000);
    REQUIRE(fx.f.settle(2000));
    CHECK(net.calls == 1);
    CHECK(fx.f.doc(&fx.owner, h) == nullptr);
    const auto at = [&](uint32_t ms) { platform::setTestNowMs(100000 + ms); fx.f.doc(&fx.owner, h); fx.f.settle(2000); };
    at(5000);
    CHECK(net.calls == 2);   // 5 s after the first failure
    at(10000);
    CHECK(net.calls == 2);   // the second failure waits 10 s
    at(15000);
    CHECK(net.calls == 3);
    net.status = 200;        // the network is up
    at(35000);
    CHECK(net.calls == 4);   // 20 s after the third failure
    REQUIRE(fx.f.doc(&fx.owner, h) != nullptr);
    at(35000 + 300000);
    CHECK(net.calls == 4);   // back to the period after a success
    at(35000 + 600000);
    CHECK(net.calls == 5);
}

TEST_CASE("an answer too big for the device is refused, not cut") {
    Fixture fx;
    net.answer = "[" + std::string(JsonFetch::kAnswerCap, '1') + "]";
    const int h = fx.f.fetch(&fx.owner, "https://api.example/scores", 60000);
    REQUIRE(fx.f.settle(2000));
    CHECK(fx.f.doc(&fx.owner, h) == nullptr);
    CHECK(fx.f.status(&fx.owner, h) == 0);
}

TEST_CASE("a push to another MoonLight patches its state live, and to a URL posts the document") {
    Fixture fx;
    const int d = fx.f.literal(&fx.owner, R"({"Effects":{"Layer":{"opacity":0}}})");
    const int r = fx.f.push(&fx.owner, "stadbeest-eye-left.local", d);
    REQUIRE(r > 0);
    REQUIRE(fx.f.settle(2000));
    CHECK(net.method == "PATCH");
    CHECK(net.url == "http://stadbeest-eye-left.local/api/state?live=1");
    CHECK(net.body == R"({"Effects":{"Layer":{"opacity":0}}})");
    CHECK(fx.f.changed(&fx.owner, r));   // the reply arrived
    CHECK(fx.f.push(&fx.owner, "https://hooks.example/in", d) > 0);
    REQUIRE(fx.f.settle(2000));
    CHECK(net.method == "POST");
    CHECK(net.url == "https://hooks.example/in");
}

TEST_CASE("a push to this device builds the tree at the frame boundary and answers as the API does") {
    Fixture fx;
    ModuleFactory::registerType<JfKnob>("JfKnob");
    Scheduler s;
    auto* effects = new JfBox();
    effects->setName("Effects");
    s.addModule(effects);
    s.setup();
    const int d = fx.f.literal(&fx.owner, R"({"Effects":{"Knob":{"type":"JfKnob","value":33}}})");
    const int r = fx.f.push(&fx.owner, nullptr, d);
    REQUIRE(r > 0);
    CHECK(fx.f.push(&fx.owner, "", d) == r);   // the first is on its way, so this one waits
    CHECK(s.firstByName("Knob") == nullptr);
    s.preparePending();
    REQUIRE(s.firstByName("Knob") != nullptr);
    CHECK(static_cast<JfKnob*>(s.firstByName("Knob"))->value == 33);
    CHECK_FALSE(effects->dirty());   // live: nothing to save
    CHECK(fx.f.changed(&fx.owner, r));
    CHECK(fx.f.status(&fx.owner, r) == 200);
    CHECK(json::readBool(json::at(*fx.f.doc(&fx.owner, r), "/ok")));
    // The one that waited went when the first was answered, and applies at the next boundary.
    static_cast<JfKnob*>(s.firstByName("Knob"))->value = 1;
    s.preparePending();
    CHECK(static_cast<JfKnob*>(s.firstByName("Knob"))->value == 33);
    s.release();
}

TEST_CASE("pushes to a busy target coalesce: the newest waits, the ones it replaced never go") {
    Fixture fx;
    const int a = fx.f.literal(&fx.owner, R"({"v":1})");
    const int b = fx.f.literal(&fx.owner, R"({"v":2})");
    const int c = fx.f.literal(&fx.owner, R"({"v":3})");
    fx.f.push(&fx.owner, "host-a", a);
    fx.f.push(&fx.owner, "host-a", b);
    fx.f.push(&fx.owner, "host-a", c);
    REQUIRE(fx.f.settle(2000));
    CHECK(net.calls == 2);
    CHECK(net.body == R"({"v":3})");
}

TEST_CASE("releasing an owner frees its documents and drops an answer still on its way") {
    Fixture fx;
    const int h = fx.f.fetch(&fx.owner, "https://api.example/slow", 60000);
    REQUIRE(h > 0);
    fx.f.release(&fx.owner);
    REQUIRE(fx.f.settle(2000));
    CHECK(fx.f.doc(&fx.owner, h) == nullptr);
    // The freed place serves a new document.
    CHECK(fx.f.literal(&fx.owner, R"({"y":2})") > 0);
}

TEST_CASE("the worker starts with the first request and stops once nothing uses it") {
    Fixture fx;
    CHECK(fx.f.fetch(&fx.owner, "https://api.example/a", 60000) > 0);
    CHECK(fx.f.workerRunning());
    REQUIRE(fx.f.settle(2000));
    JsonFetch::poll();
    CHECK(fx.f.workerRunning());          // a document is held: it stays
    fx.f.release(&fx.owner);
    CHECK_FALSE(fx.f.workerRunning());    // nothing held, nothing on its way: gone
    CHECK(fx.f.fetch(&fx.owner, "https://api.example/a", 60000) > 0);
    CHECK(fx.f.workerRunning());          // and back for the next request
}

TEST_CASE("a post keeps the status and never reads the answer, so its size cannot refuse it") {
    Fixture fx;
    net.answer = "[" + std::string(JsonFetch::kAnswerCap * 2, '1') + "]";
    const int r = fx.f.post(&fx.owner, "https://cloud.example/api/report", R"({"e":1})");
    REQUIRE(r > 0);
    REQUIRE(fx.f.settle(2000));
    CHECK(fx.f.outcome(&fx.owner, r) == 200);
    CHECK(net.method == "POST");
    CHECK(net.body == R"({"e":1})");
}

// A json member keeps its handle across a recompile; the document behind it was freed, and the handle then named the next document in that place.
TEST_CASE("a handle kept past its document's release names nothing, even once the place is taken again") {
    Fixture fx;
    const int old = fx.f.literal(&fx.owner, R"({"a":1})");
    REQUIRE(old > 0);
    fx.f.release(&fx.owner);
    const int fresh = fx.f.literal(&fx.owner, R"({"b":2})");
    REQUIRE(fresh > 0);
    CHECK(fresh != old);
    CHECK(fx.f.doc(&fx.owner, old) == nullptr);
    CHECK(fx.f.doc(&fx.owner, fresh) != nullptr);
}

TEST_CASE("a push that could not go says so: status 0, and a change to read") {
    Fixture fx;
    // No scheduler to apply it: a push to this device reaches nothing.
    const int d = fx.f.literal(&fx.owner, R"({"Effects":{}})");
    const int r = fx.f.push(&fx.owner, nullptr, d);
    REQUIRE(r > 0);
    CHECK(fx.f.changed(&fx.owner, r));
    CHECK(fx.f.status(&fx.owner, r) == 0);
}

TEST_CASE("a push still queued when the device stops is answered, so nothing waits for it forever") {
    Fixture fx;
    Scheduler s;
    auto* effects = new JfBox();
    effects->setName("Effects");
    s.addModule(effects);
    s.setup();
    const int d = fx.f.literal(&fx.owner, R"({"Effects":{}})");
    const int r = fx.f.push(&fx.owner, nullptr, d);
    REQUIRE(fx.f.outcome(&fx.owner, r) == -1);   // on its way
    s.release();                              // before the frame boundary applied it
    CHECK(fx.f.outcome(&fx.owner, r) == 400);
    CHECK(fx.f.settle(100));                  // nothing counted as on its way any more
}
