/// @module HttpServerModule

#include "doctest.h"
#include "core/system/HttpServerModule.h"
#include "core/module/Scheduler.h"
#include "core/module/MoonModule.h"
#include "fake_light_output.h"   // the fake Drivers is the light output too

namespace {

// Stands in for the Drivers the WLED shim drives, by name, and is the light output a test claims when it reaches the palettes.
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

/// A light output beside the HTTP server, the tree the WLED state route reaches; `seated` claims the output, as a running driver does.
struct WledRig {
    mm::Scheduler s;
    FakeDrivers* drivers = new FakeDrivers();
    mm::HttpServerModule http;
    explicit WledRig(bool seated = true) {
        drivers->setName("Drivers");
        drivers->defineControls();   // on, brightness and palette, which applyWledState finds
        s.addModule(drivers);
        http.setScheduler(&s);
        if (seated) drivers->outputSeat_.claim();
    }
    ~WledRig() { s.deleteTree(drivers); }
};

} // namespace

// The WLED shim's `{on,bri}` apply drives the real `on` + `brightness` controls independently: turning off must NOT clobber the brightness value. Home Assistant and the WLED app both post through this path.
TEST_CASE("apply-core: applyWledState sets on + bri independently (no brightness clobber)") {
    WledRig rig(false);
    FakeDrivers* drivers = rig.drivers;  mm::HttpServerModule& http = rig.http;

    // Set a real brightness first.
    http.applyWledState("{\"bri\":200}");
    CHECK(drivers->brightness == 200);
    CHECK(drivers->on == true);

    // Turn OFF → on flips false, brightness value is PRESERVED (the fudge would have zeroed it).
    http.applyWledState("{\"on\":false}");
    CHECK(drivers->on == false);
    CHECK(drivers->brightness == 200);   // preserved: this is the subtraction's payoff

    // Turn back ON → on true, brightness still the level the user had.
    http.applyWledState("{\"on\":true}");
    CHECK(drivers->on == true);
    CHECK(drivers->brightness == 200);

    // Combined {on,bri} applies both.
    http.applyWledState("{\"on\":true,\"bri\":50}");
    CHECK(drivers->on == true);
    CHECK(drivers->brightness == 50);
}

// The WLED app and Home Assistant pick a palette by index or by color, and both reach the palette through the light output.
TEST_CASE("apply-core: applyWledState picks a palette by index and by color through the light output") {
    WledRig rig(false);
    FakeDrivers* drivers = rig.drivers;  mm::HttpServerModule& http = rig.http;

    // No light output running: the palette stays put, by index and by color alike.
    http.applyWledState("{\"seg\":[{\"pal\":3,\"col\":[[0,0,255]]}]}");
    CHECK(drivers->palette == 0);

    drivers->outputSeat_.claim();
    http.applyWledState("{\"seg\":[{\"pal\":3}]}");
    CHECK(drivers->palette == 3);
    http.applyWledState("{\"seg\":[{\"pal\":40}]}");
    CHECK(drivers->palette == 11);   // clamped to the last palette the output offers
    http.applyWledState("{\"seg\":[{\"col\":[[0,0,255]]}]}");
    CHECK(drivers->palette == 8);    // pure blue is 240 degrees, the fake's palette 8
}

// The body is read as JSON (the parser of howels's PR #273), so a key inside a segment stays the segment's, a value of the wrong kind or a malformed body leaves the device as it was, and segment 0, the device's one segment, sets the device.
TEST_CASE("apply-core: applyWledState reads the body as JSON: a segment's key stays the segment's, and a malformed body or a value of the wrong kind leaves the device as it was") {
    WledRig rig;
    FakeDrivers* drivers = rig.drivers;  mm::HttpServerModule& http = rig.http;
    http.applyWledState("{\"on\":true,\"bri\":200}");

    http.applyWledState("{\"seg\":[{\"id\":1,\"on\":false,\"bri\":1,\"pal\":3}]}");   // a segment past the device's one
    http.applyWledState("{\"bri\":\"~-10\",\"on\":{}}");   // WLED's relative step and an object are no values here
    http.applyWledState("{\"bri\":-5}");
    http.applyWledState("{\"bri\":");
    http.applyWledState("not json");
    http.applyWledState("");
    http.applyWledState(nullptr);
    CHECK(drivers->on == true);
    CHECK(drivers->brightness == 200);
    CHECK(drivers->palette == 0);

    http.applyWledState("{\"bri\":999}");
    CHECK(drivers->brightness == 255);   // held to the range's top
    http.applyWledState("{\"seg\":{\"pal\":2}}");   // one object is the main segment
    CHECK(drivers->palette == 2);
    http.applyWledState("{\"seg\":[{\"pal\":4}]}");   // the first element, naming no id, is segment 0
    CHECK(drivers->palette == 4);
    http.applyWledState("{\"seg\":[{\"id\":0,\"on\":false}]}");   // Home Assistant's segment light switching off
    CHECK(drivers->on == false);
    CHECK(drivers->brightness == 255);   // master power keeps the level
}

// WLED picks a segment by its id in the object form as in the array form, and this device has segment 0 alone.
TEST_CASE("apply-core: applyWledState leaves the device as it was for a segment object that names a segment past its one") {
    WledRig rig;
    FakeDrivers* drivers = rig.drivers;  mm::HttpServerModule& http = rig.http;
    http.applyWledState("{\"on\":true,\"bri\":200}");

    http.applyWledState("{\"seg\":{\"id\":1,\"on\":false,\"bri\":1,\"pal\":3}}");
    CHECK(drivers->on == true);
    CHECK(drivers->brightness == 200);
    CHECK(drivers->palette == 0);

    http.applyWledState("{\"seg\":{\"id\":0,\"pal\":3}}");   // the object that names segment 0 sets the device
    CHECK(drivers->palette == 3);
}

// Home Assistant sends a single-segment device its segment at 255 and then the level in the body's bri, as two requests; reading the segment's bri as the device's would flash full brightness between them.
TEST_CASE("apply-core: applyWledState takes the level from the body's brightness alone, a segment's being the segment's own") {
    WledRig rig;
    FakeDrivers* drivers = rig.drivers;  mm::HttpServerModule& http = rig.http;

    http.applyWledState("{\"on\":true,\"bri\":40,\"seg\":[{\"id\":0,\"on\":true,\"bri\":255}]}");
    CHECK(drivers->brightness == 40);
    http.applyWledState("{\"seg\":[{\"id\":0,\"bri\":255}]}");   // Home Assistant's first request
    CHECK(drivers->brightness == 40);                                    // the level holds, no flash to full
}

TEST_CASE("apply-core: applyWledState toggles power on \"t\" and reads a color as hex as well as an array") {
    WledRig rig;
    FakeDrivers* drivers = rig.drivers;  mm::HttpServerModule& http = rig.http;

    http.applyWledState("{\"on\":\"t\"}");
    CHECK(drivers->on == false);
    http.applyWledState("{\"seg\":[{\"id\":0,\"on\":\"t\"}]}");
    CHECK(drivers->on == true);
    http.applyWledState("{\"on\":\"true\"}");   // a string other than "t" is no toggle, and no flag either
    CHECK(drivers->on == true);

    http.applyWledState("{\"seg\":[{\"col\":[\"00FF00\"]}]}");
    CHECK(drivers->palette == 4);    // pure green, the fake's palette 4
    http.applyWledState("{\"seg\":[{\"col\":[\"00FF0000\"]}]}");   // WWRRGGBB: the white byte is dropped
    CHECK(drivers->palette == 0);
    http.applyWledState("{\"seg\":[{\"col\":[\"zzzzzz\"]}]}");   // not hex: unchanged
    CHECK(drivers->palette == 0);
}
