/**
    @title     MoonLight Unit Tests — Nodes
    @file      test_nodes.cpp
    @repo      https://github.com/MoonModules/MoonLight
    @Copyright © 2026 Github MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007

    Native unit tests for pure functions from Nodes.h / Nodes.cpp.
    All tested functions are included directly from source headers — no copies.
    Run with: pio test -e native
**/

#include "doctest.h"

#include <ArduinoJson.h>
#include <cstring>
#include <string>

#include "MoonBase/utilities/PureFunctions.h"       // buildNameAndTags, dimension constants, triangle8, control functions — no copy!
#include "MoonBase/utilities/Char.h"                // Char<N> — no copy!

// ============================================================
// Tests — buildNameAndTags (from PureFunctions.h)
// ============================================================

TEST_CASE("buildNameAndTags: no dimension, no tags") {
  auto result = buildNameAndTags("TestNode", _NoD, "");
  CHECK(result == "TestNode");
}

TEST_CASE("buildNameAndTags: 0D adds lightbulb emoji") {
  auto result = buildNameAndTags("Driver", _0D, "");
  CHECK(result.find("Driver ") == 0);
  CHECK(result.size() > 7);  // has emoji appended
}

TEST_CASE("buildNameAndTags: 1D adds ruler emoji") {
  auto result = buildNameAndTags("Effect", _1D, "");
  CHECK(result.find("Effect ") == 0);
}

TEST_CASE("buildNameAndTags: with tags") {
  auto result = buildNameAndTags("Glow", _1D, "\xE2\x9A\x99\xEF\xB8\x8F");  // ⚙️
  CHECK(result.find("Glow") == 0);
  CHECK(result.find("\xE2\x9A\x99") != std::string::npos);  // contains gear emoji
}

TEST_CASE("buildNameAndTags: 2D and 3D dimensions") {
  auto r2d = buildNameAndTags("Grid", _2D, "");
  auto r3d = buildNameAndTags("Cube", _3D, "");
  CHECK(r2d.size() > 4);
  CHECK(r3d.size() > 4);
  CHECK(r2d != r3d);
}

// ============================================================
// Tests — findOrCreateControl (from ControlFunctions.h)
// ============================================================

TEST_CASE("findOrCreateControl: creates new control when empty") {
  JsonDocument doc;
  JsonArray controls = doc.to<JsonArray>();
  bool newControl = false;

  JsonObject ctrl = findOrCreateControl(controls, "speed", newControl);

  CHECK(newControl == true);
  CHECK(ctrl["name"] == "speed");
  CHECK(controls.size() == 1);
}

TEST_CASE("findOrCreateControl: finds existing control") {
  JsonDocument doc;
  JsonArray controls = doc.to<JsonArray>();
  JsonObject existing = controls.add<JsonObject>();
  existing["name"] = "brightness";
  existing["value"] = 200;

  bool newControl = false;
  JsonObject ctrl = findOrCreateControl(controls, "brightness", newControl);

  CHECK(newControl == false);
  CHECK(ctrl["value"] == 200);
  CHECK(controls.size() == 1);
}

TEST_CASE("findOrCreateControl: creates second control when first doesn't match") {
  JsonDocument doc;
  JsonArray controls = doc.to<JsonArray>();
  JsonObject existing = controls.add<JsonObject>();
  existing["name"] = "color";

  bool newControl = false;
  JsonObject ctrl = findOrCreateControl(controls, "speed", newControl);

  CHECK(newControl == true);
  CHECK(ctrl["name"] == "speed");
  CHECK(controls.size() == 2);
}

TEST_CASE("findOrCreateControl: finds among multiple controls") {
  JsonDocument doc;
  JsonArray controls = doc.to<JsonArray>();

  JsonObject c1 = controls.add<JsonObject>();
  c1["name"] = "alpha";
  JsonObject c2 = controls.add<JsonObject>();
  c2["name"] = "beta";
  c2["value"] = 42;
  JsonObject c3 = controls.add<JsonObject>();
  c3["name"] = "gamma";

  bool newControl = false;
  JsonObject ctrl = findOrCreateControl(controls, "beta", newControl);

  CHECK(newControl == false);
  CHECK(ctrl["value"] == 42);
  CHECK(controls.size() == 3);
}

// ============================================================
// Tests — updateControl (from ControlFunctions.h)
// ============================================================

TEST_CASE("updateControl: writes uint8_t slider value") {
  uint8_t speed = 0;

  JsonDocument doc;
  JsonObject control = doc.to<JsonObject>();
  control["name"] = "speed";
  control["type"] = "slider";
  control["size"] = 8;
  control["value"] = 200;
  control["p"] = (uintptr_t)&speed;

  updateControl(control);
  CHECK(speed == 200);
}

TEST_CASE("updateControl: writes uint16_t number value") {
  uint16_t count = 0;

  JsonDocument doc;
  JsonObject control = doc.to<JsonObject>();
  control["name"] = "count";
  control["type"] = "number";
  control["size"] = 16;
  control["value"] = 1024;
  control["p"] = (uintptr_t)&count;

  updateControl(control);
  CHECK(count == 1024);
}

TEST_CASE("updateControl: writes float value") {
  float gain = 0.0f;

  JsonDocument doc;
  JsonObject control = doc.to<JsonObject>();
  control["name"] = "gain";
  control["type"] = "number";
  control["size"] = 34;
  control["value"] = 3.14f;
  control["p"] = (uintptr_t)&gain;

  updateControl(control);
  CHECK(gain == doctest::Approx(3.14f));
}

TEST_CASE("updateControl: writes bool checkbox value") {
  bool enabled = false;

  JsonDocument doc;
  JsonObject control = doc.to<JsonObject>();
  control["name"] = "enabled";
  control["type"] = "checkbox";
  control["size"] = (int)sizeof(bool);
  control["value"] = true;
  control["p"] = (uintptr_t)&enabled;

  updateControl(control);
  CHECK(enabled == true);
}

TEST_CASE("updateControl: writes text value with truncation") {
  char name[8] = "";

  JsonDocument doc;
  JsonObject control = doc.to<JsonObject>();
  control["name"] = "label";
  control["type"] = "text";
  control["size"] = 8;
  control["value"] = "Hello World Long String";
  control["p"] = (uintptr_t)name;

  updateControl(control);
  CHECK(std::string(name) == "Hello W");
}

TEST_CASE("updateControl: text respects max field") {
  char buf[32] = "";

  JsonDocument doc;
  JsonObject control = doc.to<JsonObject>();
  control["name"] = "label";
  control["type"] = "text";
  control["size"] = 32;
  control["max"] = 5;
  control["value"] = "Hello World";
  control["p"] = (uintptr_t)buf;

  updateControl(control);
  CHECK(std::string(buf) == "Hello");
}

TEST_CASE("updateControl: writes int value (size 33)") {
  int temperature = 0;

  JsonDocument doc;
  JsonObject control = doc.to<JsonObject>();
  control["name"] = "temp";
  control["type"] = "number";
  control["size"] = 33;
  control["value"] = -42;
  control["p"] = (uintptr_t)&temperature;

  updateControl(control);
  CHECK(temperature == -42);
}

TEST_CASE("updateControl: writes int8_t value (size 108)") {
  int8_t offset = 0;

  JsonDocument doc;
  JsonObject control = doc.to<JsonObject>();
  control["name"] = "offset";
  control["type"] = "slider";
  control["size"] = 108;
  control["value"] = -10;
  control["p"] = (uintptr_t)&offset;

  updateControl(control);
  CHECK(offset == -10);
}

TEST_CASE("updateControl: skips when pointer is null/zero") {
  JsonDocument doc;
  JsonObject control = doc.to<JsonObject>();
  control["name"] = "test";
  control["type"] = "slider";
  control["size"] = 8;
  control["value"] = 100;
  control["p"] = 0;

  updateControl(control);
}

TEST_CASE("updateControl: skips when name is missing") {
  uint8_t val = 0;

  JsonDocument doc;
  JsonObject control = doc.to<JsonObject>();
  control["type"] = "slider";
  control["size"] = 8;
  control["value"] = 100;
  control["p"] = (uintptr_t)&val;

  updateControl(control);
  CHECK(val == 0);
}

TEST_CASE("updateControl: writes uint32_t value (size 32)") {
  uint32_t bigVal = 0;

  JsonDocument doc;
  JsonObject control = doc.to<JsonObject>();
  control["name"] = "big";
  control["type"] = "number";
  control["size"] = 32;
  control["value"] = 100000;
  control["p"] = (uintptr_t)&bigVal;

  updateControl(control);
  CHECK(bigVal == 100000);
}
