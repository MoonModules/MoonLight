/// @module HueDriver

/// Pins HueDriver's host-testable core: the changed-only diff, the RGB→HSV color body it PUTs, and the parse that keeps only color-capable, reachable lights. Live bridge I/O (httpRequest, pairing) needs a real bridge, that's the bench; here the seams run with no socket.

#include "doctest.h"
#include "light/drivers/HueDriver.h"

#include <cstdlib>
#include <cstring>
#include <string>

// @step smart-home/hue-driver
TEST_CASE("HueDriver: a colored pixel becomes an on/bri/hue/sat state body") {
    mm::HueDriver hue;
    char body[80] = {};
    CHECK(hue.wouldPushForTest(0, 255, 0, 0, body, sizeof(body)));   // pure red
    CHECK(std::strstr(body, "\"on\":true") != nullptr);
    CHECK(std::strstr(body, "\"bri\":254") != nullptr);             // max value
    CHECK(std::strstr(body, "\"hue\":") != nullptr);
    CHECK(std::strstr(body, "\"sat\":254") != nullptr);            // fully saturated
}

TEST_CASE("HueDriver: a black pixel becomes on:false") {
    mm::HueDriver hue;
    char body[80] = {};
    CHECK(hue.wouldPushForTest(1, 0, 0, 0, body, sizeof(body)));
    CHECK(std::strstr(body, "\"on\":false") != nullptr);
    CHECK(std::strstr(body, "\"hue\"") == nullptr);   // off → no color fields
}

TEST_CASE("HueDriver: RGB→HSV maps the primaries to the right Hue wheel positions") {
    uint16_t h; uint8_t s, v;
    mm::HueDriver::rgbToHsvForTest(255, 0, 0, h, s, v);   // red → hue ~0
    CHECK(v == 254); CHECK(s == 254);
    CHECK((h < 2000 || h > 63000));                       // near the 0/65535 wrap
    mm::HueDriver::rgbToHsvForTest(0, 255, 0, h, s, v);   // green → ~1/3 of the wheel
    CHECK(h > 65535 / 3 - 3000); CHECK(h < 65535 / 3 + 3000);
    mm::HueDriver::rgbToHsvForTest(0, 0, 255, h, s, v);   // blue → ~2/3
    CHECK(h > 65535 * 2 / 3 - 3000); CHECK(h < 65535 * 2 / 3 + 3000);
    mm::HueDriver::rgbToHsvForTest(128, 128, 128, h, s, v); // grey → sat 0
    CHECK(s == 0);
}

TEST_CASE("HueDriver: unchanged color is not resent, a changed one is") {
    mm::HueDriver hue;
    char body[80] = {};
    CHECK(hue.wouldPushForTest(2, 10, 20, 30, body, sizeof(body)));        // first → yes
    CHECK_FALSE(hue.wouldPushForTest(2, 10, 20, 30, body, sizeof(body)));  // same → skip
    CHECK(hue.wouldPushForTest(2, 10, 20, 31, body, sizeof(body)));        // changed → yes
}

TEST_CASE("HueDriver: parseLights keeps only color-capable, reachable lights") {
    mm::HueDriver hue;
    // id 5 color + reachable (keep); id 7 dimmable-only white (drop); id 10 on/off plug (drop); id 8 color but UNREACHABLE (drop). The shapes the real bridge returns.
    const char* json =
        "{\"5\":{\"state\":{\"on\":false,\"bri\":77,\"hue\":8595,\"sat\":121,\"reachable\":true},\"name\":\"Bureau lamp\"},"
        "\"7\":{\"state\":{\"on\":true,\"bri\":40,\"reachable\":true},\"name\":\"Gang lamp\"},"
        "\"10\":{\"state\":{\"on\":true,\"reachable\":true},\"name\":\"Bureau\"},"
        "\"8\":{\"state\":{\"on\":false,\"bri\":1,\"hue\":0,\"sat\":0,\"reachable\":false},\"name\":\"Nachtkastje\"}}";
    hue.parseLightsForTest(json);
    REQUIRE(hue.lightCountForTest() == 1);     // only id 5 qualifies
    CHECK(hue.hueIdForTest(0) == 5);
    CHECK(hue.colorCountForTest() == 1);
}

// Room + light selection filters which color lights the driver actually drives. Both dropdowns default to "All" (index 0): then every color light is driven (unchanged behavior). Selecting a room narrows the driven set to that room's color lights; selecting a light drives just that one.
TEST_CASE("HueDriver: room/light selection filters the driven set") {
    mm::HueDriver hue;
    // Four color+reachable lights, ids 1..4.
    const char* lights =
        "{\"1\":{\"state\":{\"hue\":1,\"reachable\":true},\"name\":\"Lamp A\"},"
        "\"2\":{\"state\":{\"hue\":2,\"reachable\":true},\"name\":\"Lamp B\"},"
        "\"3\":{\"state\":{\"hue\":3,\"reachable\":true},\"name\":\"Lamp C\"},"
        "\"4\":{\"state\":{\"hue\":4,\"reachable\":true},\"name\":\"Lamp D\"}}";
    hue.parseLightsForTest(lights);
    REQUIRE(hue.lightCountForTest() == 4);

    // Two rooms: "Living" = lights 1,2 ; "Office" = lights 3,4. (Plus a Zone that must be ignored.)
    const char* groups =
        "{\"1\":{\"name\":\"Living\",\"lights\":[\"1\",\"2\"],\"type\":\"Room\"},"
        "\"2\":{\"name\":\"Office\",\"lights\":[\"3\",\"4\"],\"type\":\"Room\"},"
        "\"3\":{\"name\":\"Whole house\",\"lights\":[\"1\",\"2\",\"3\",\"4\"],\"type\":\"Zone\"}}";
    hue.parseGroupsForTest(groups);
    CHECK(hue.roomCountForTest() == 2);          // the Zone is not a Room

    // Default (room=All, light=All): all four driven.
    CHECK(hue.drivenCountForTest() == 4);

    // Select room "Living" (index 1): only its two lights (ids 1,2) are driven.
    hue.setRoomForTest(1);
    REQUIRE(hue.drivenCountForTest() == 2);
    CHECK(hue.drivenIdForTest(0) == 1);
    CHECK(hue.drivenIdForTest(1) == 2);

    // Within "Living", select the 2nd light (index 2 → light id 2): just that one.
    hue.setLightForTest(2);
    REQUIRE(hue.drivenCountForTest() == 1);
    CHECK(hue.drivenIdForTest(0) == 2);

    // Back to room=All resets the breadth.
    hue.setRoomForTest(0);
    hue.setLightForTest(0);
    CHECK(hue.drivenCountForTest() == 4);
}

// The single status line (folding what were the separate hueStatus / colorLights controls) shows the light count as driven-of-total: "N-M lights" while filtered, the plain "M lights" when not.
TEST_CASE("HueDriver: status reports the driven-of-total light count") {
    mm::HueDriver hue;
    hue.appKey[0] = 'k'; hue.appKey[1] = '\0';   // any non-empty key → "paired" not "unpaired"; size-independent
    const char* lights =
        "{\"1\":{\"state\":{\"hue\":1,\"reachable\":true},\"name\":\"Lamp A\"},"
        "\"2\":{\"state\":{\"hue\":2,\"reachable\":true},\"name\":\"Lamp B\"},"
        "\"3\":{\"state\":{\"hue\":3,\"reachable\":true},\"name\":\"Lamp C\"},"
        "\"4\":{\"state\":{\"hue\":4,\"reachable\":true},\"name\":\"Lamp D\"}}";
    hue.parseLightsForTest(lights);
    const char* groups =
        "{\"1\":{\"name\":\"Living\",\"lights\":[\"1\",\"2\"],\"type\":\"Room\"}}";
    hue.parseGroupsForTest(groups);

    // Unfiltered (room=All): the plain count.
    hue.refreshStatusForTest();
    CHECK(std::string(hue.status()) == "paired, 4 lights");

    // Filtered to "Living" (2 of 4): the driven-of-total form.
    hue.setRoomForTest(1);
    hue.refreshStatusForTest();
    CHECK(std::string(hue.status()) == "paired, 2-4 lights");
}

// fetchLights sizes its read buffer by growing while the body looks truncated. The signal is "does the body end in '}'": a too-small buffer cuts the JSON mid-content. (Regression: an earlier check tested strlen==cap-1, which never fires because httpRequest strips headers first, so a >2 KB bridge response was parsed truncated and lights silently disappeared.)
TEST_CASE("HueDriver: bodyLooksComplete is the truncation signal for the grow-and-retry fetch") {
    // Complete: a whole /lights object, with and without trailing whitespace.
    CHECK(mm::HueDriver::bodyLooksCompleteForTest("{\"5\":{\"state\":{\"hue\":1}}}"));
    CHECK(mm::HueDriver::bodyLooksCompleteForTest("{\"5\":{}}\r\n  "));
    // Truncated: cut mid-object (the symptom of a too-small buffer) → grow.
    CHECK_FALSE(mm::HueDriver::bodyLooksCompleteForTest("{\"5\":{\"state\":{\"hue\":85"));
    CHECK_FALSE(mm::HueDriver::bodyLooksCompleteForTest(""));
    CHECK_FALSE(mm::HueDriver::bodyLooksCompleteForTest("   \n"));
}
