/**
    @title     MoonBase Unit Tests — Module
    @file      test_module.cpp
    @repo      https://github.com/MoonModules/MoonLight
    @Copyright © 2026 GitHub MoonLight Commit Authors
    @license   GNU GENERAL PUBLIC LICENSE Version 3, 29 June 2007

    Native unit tests for pure ArduinoJson logic used by the Module system.
    Functions are copied here to avoid ESP32 header dependencies.
    Run with: pio test -e native
**/

#include "doctest.h"

#include <ArduinoJson.h>

// ============================================================
// Copied from Module.cpp — keep in sync with the original
// ============================================================

/// Populates a controls object with default values from a definition array.
/// Non-"rows" controls get their "default" value; "rows" controls are skipped.
void setDefaults(JsonObject controls, JsonArray definition) {
  for (JsonObject control : definition) {
    if (control["type"] != "rows") {
      controls[control["name"]] = control["default"];
    } else {
      // rows are not given defaults (they start empty)
    }
  }
}

// ============================================================
// Tests
// ============================================================

TEST_CASE("setDefaults: text control gets default value") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();
  JsonObject ctrl = definition.add<JsonObject>();
  ctrl["name"] = "brightness";
  ctrl["type"] = "number";
  ctrl["default"] = 128;

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls["brightness"] == 128);
}

TEST_CASE("setDefaults: multiple controls") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();

  JsonObject c1 = definition.add<JsonObject>();
  c1["name"] = "name";
  c1["type"] = "text";
  c1["default"] = "MoonLight";

  JsonObject c2 = definition.add<JsonObject>();
  c2["name"] = "speed";
  c2["type"] = "number";
  c2["default"] = 50;

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls["name"] == "MoonLight");
  CHECK(controls["speed"] == 50);
}

TEST_CASE("setDefaults: rows type is skipped") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();

  JsonObject c1 = definition.add<JsonObject>();
  c1["name"] = "items";
  c1["type"] = "rows";
  c1["default"] = "should_not_appear";

  JsonObject c2 = definition.add<JsonObject>();
  c2["name"] = "color";
  c2["type"] = "color";
  c2["default"] = "#FF0000";

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls["items"].isNull());
  CHECK(controls["color"] == "#FF0000");
}

TEST_CASE("setDefaults: control with no default sets null") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();
  JsonObject ctrl = definition.add<JsonObject>();
  ctrl["name"] = "optional";
  ctrl["type"] = "text";
  // no "default" key

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls["optional"].isNull());
}

TEST_CASE("setDefaults: empty definition produces empty controls") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls.size() == 0);
}

TEST_CASE("setDefaults: boolean default") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();
  JsonObject ctrl = definition.add<JsonObject>();
  ctrl["name"] = "enabled";
  ctrl["type"] = "checkbox";
  ctrl["default"] = true;

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls["enabled"] == true);
}

TEST_CASE("setDefaults: overwrites existing value in controls") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();
  JsonObject ctrl = definition.add<JsonObject>();
  ctrl["name"] = "volume";
  ctrl["type"] = "number";
  ctrl["default"] = 75;

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  controls["volume"] = 42;  // pre-existing value
  setDefaults(controls, definition);

  CHECK(controls["volume"] == 75);  // overwritten by default
}

TEST_CASE("setDefaults: string default value") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();
  JsonObject ctrl = definition.add<JsonObject>();
  ctrl["name"] = "hostname";
  ctrl["type"] = "text";
  ctrl["default"] = "moon-device";

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls["hostname"] == "moon-device");
}

TEST_CASE("setDefaults: float default value") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();
  JsonObject ctrl = definition.add<JsonObject>();
  ctrl["name"] = "gain";
  ctrl["type"] = "number";
  ctrl["default"] = 1.5f;

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls["gain"].as<float>() == doctest::Approx(1.5f));
}

TEST_CASE("setDefaults: mixed types with rows skipped") {
  JsonDocument defDoc;
  JsonArray definition = defDoc.to<JsonArray>();

  JsonObject c1 = definition.add<JsonObject>();
  c1["name"] = "speed";
  c1["type"] = "number";
  c1["default"] = 200;

  JsonObject c2 = definition.add<JsonObject>();
  c2["name"] = "label";
  c2["type"] = "text";
  c2["default"] = "hello";

  JsonObject c3 = definition.add<JsonObject>();
  c3["name"] = "nodes";
  c3["type"] = "rows";
  c3["default"] = "ignored";

  JsonObject c4 = definition.add<JsonObject>();
  c4["name"] = "active";
  c4["type"] = "checkbox";
  c4["default"] = false;

  JsonDocument outDoc;
  JsonObject controls = outDoc.to<JsonObject>();
  setDefaults(controls, definition);

  CHECK(controls["speed"] == 200);
  CHECK(controls["label"] == "hello");
  CHECK(controls["nodes"].isNull());
  CHECK(controls["active"] == false);
  CHECK(controls.size() == 3);  // rows not included
}
