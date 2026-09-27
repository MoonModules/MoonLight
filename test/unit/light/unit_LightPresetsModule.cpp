/// @module LightPresetsModule

/// Pins the light-preset library: the curated built-ins seed as locked (read-only) rows, a user can add / edit / delete / reorder custom presets via the editable-list hooks, a preset resolves into a driver's flat Correction (the cold-path bridge the render loop never touches), and the whole set, with each preset's role wiring, round-trips through persistence.

#include "doctest.h"
#include "light/drivers/LightPresetsModule.h"
#include "core/util/JsonSink.h"

#include <cstdint>
#include <cstring>
#include <string>

using mm::LightPresetsModule;
using mm::Correction;

namespace {
// The count of seeded read-only built-ins (see LightPresetsModule::seedBuiltins). Referenced by name so adding a built-in updates one constant, not a scatter of magic numbers across the cases.
constexpr int kBuiltinCount = 14;   // RGB,GRB,BGR,RGBW,GRBW,WRGB,GRB6,RGBWYP,RGBCCT,IRGB + 4 moving heads

// Serialize the presets List value (the persisted form) to a string.
std::string presetsJson(LightPresetsModule& m) {
    m.defineControls();
    // The "presets" List is the last control added after MoonModule::defineControls().
    for (uint8_t i = 0; i < m.controls().count(); i++) {
        if (std::strcmp(m.controls()[i].name, "presets") == 0) {
            mm::JsonSink sink;
            mm::writeControlValue(sink, m.controls()[i]);
            return std::string(sink.data());
        }
    }
    return "";
}
}  // namespace

TEST_CASE("LightPresets seeds the curated built-ins as locked rows") {
    LightPresetsModule m;
    m.setup();
    CHECK(m.listRowCount() == kBuiltinCount);   // the curated built-ins (RGB, GRB, … + moving heads)
    // Each built-in resolves into a Correction; GRB derives G at 0, R at 1, B at 2. Row order is seed order, so id of row 1 (GRB) is discoverable via the default path.
    const uint32_t firstId = m.defaultId();
    CHECK(firstId != 0);
    Correction c;
    CHECK(m.deriveCorrection(firstId, 255, c));   // the first built-in (RGB) resolves
    CHECK(c.offRed == 0); CHECK(c.offGreen == 1); CHECK(c.offBlue == 2);   // RGB order
}

// Option-array hoist (the 1 Hz-push efficiency fix): the 14 channel-role option strings are emitted ONCE per list in optionSets["channelRole"], and each ch<N> select references it via optionsRef, NOT re-inlined per channel per row. A 32-channel fixture × 13 rows would otherwise repeat that array 400+ times in every state push. Pins that the row detail carries optionsRef, never inline options.
TEST_CASE("LightPresets serializes the channel-role options ONCE, rows reference by optionsRef") {
    LightPresetsModule m;
    m.setup();
    m.defineControls();
    // The optionSets + detail ride the metadata block (writeControlMetadata), not the value.
    std::string json;
    for (uint8_t i = 0; i < m.controls().count(); i++) {
        if (std::strcmp(m.controls()[i].name, "presets") == 0) {
            mm::JsonSink sink;
            mm::writeControlMetadata(sink, m.controls()[i]);
            json = std::string(sink.data());
            break;
        }
    }
    // The shared set is present once, with the role names.
    CHECK(json.find("\"optionSets\":{\"channelRole\":[") != std::string::npos);
    CHECK(json.find("\"Pan\"") != std::string::npos);      // a role name appears (in the shared set)
    // Rows reference the set; they do NOT inline a per-channel options array.
    CHECK(json.find("\"optionsRef\":\"channelRole\"") != std::string::npos);
    CHECK(json.find("\"type\":\"select\",\"value\":") != std::string::npos);   // channel selects present
    // The role-name array must appear ONCE (the shared set), not once per channel: count "Rotate" (a distinctive role), it should be exactly one occurrence across the whole serialization.
    size_t count = 0, pos = 0;
    while ((pos = json.find("\"Rotate\"", pos)) != std::string::npos) { count++; pos += 8; }
    CHECK(count == 1);   // one shared set, not 13 rows × N channels of inlined options
}

TEST_CASE("LightPresets add / edit / resolve a custom preset") {
    LightPresetsModule m;
    m.setup();
    uint32_t id = 0;
    REQUIRE(m.addListRow(id));             // append a custom preset (defaults R,G,B)
    CHECK(id != 0);
    CHECK(m.listRowCount() == kBuiltinCount + 1);

    // Edit it into a GRBW wiring: 4 channels, G,R,B,W.
    CHECK(m.setListRowField(id, "channels", "{\"value\":4}"));
    CHECK(m.setListRowField(id, "ch0", "{\"value\":2}"));   // 2 = G in kChannelRoleOptions
    CHECK(m.setListRowField(id, "ch1", "{\"value\":1}"));   // 1 = R
    CHECK(m.setListRowField(id, "ch2", "{\"value\":3}"));   // 3 = B
    CHECK(m.setListRowField(id, "ch3", "{\"value\":4}"));   // 4 = W

    Correction c;
    REQUIRE(m.deriveCorrection(id, 255, c));
    CHECK(c.outChannels == 4);
    CHECK(c.offGreen == 0);   // G at channel 0
    CHECK(c.offRed == 1);     // R at channel 1
    CHECK(c.offBlue == 2);    // B at channel 2
    CHECK(c.offWhite == 3);   // W at channel 3
}

// setListRowField parses the "ch<N>" channel index with strtol, not atoi, a malformed suffix must be REJECTED, not silently coerced to channel 0 (atoi("ch3x")→0 would misroute the write). Pins that a trailing-garbage or out-of-range channel name returns false and leaves the roles unchanged.
TEST_CASE("LightPresets: a malformed ch<N> field name is rejected, not coerced to 0") {
    LightPresetsModule m;
    m.setup();
    uint32_t id = 0;
    REQUIRE(m.addListRow(id));                                  // 3 channels, R,G,B by default
    CHECK(m.setListRowField(id, "ch0", "{\"value\":6}"));       // valid: ch0 → Yellow(6), succeeds
    CHECK_FALSE(m.setListRowField(id, "ch3x", "{\"value\":1}")); // trailing garbage → rejected
    CHECK_FALSE(m.setListRowField(id, "ch9",  "{\"value\":1}")); // out of range (only 3 channels) → rejected
    Correction c;
    REQUIRE(m.deriveCorrection(id, 255, c));
    CHECK(c.outChannels == 3);   // unchanged by the rejected writes
    // ch0 took the valid Yellow write; ch1/ch2 still G/B, the rejected "ch3x"/"ch9" wrote nothing.
    CHECK(c.offGreen == 1);
    CHECK(c.offBlue == 2);
}

// Regression (live bug): growing a preset's channel count must PRESERVE the roles already set on the existing channels, only the new channels get defaults. The earlier rebuildPool copied the NEW (larger) count of bytes from the old (smaller) slice, reading past it / skipping the copy, so the existing picks were lost on every channel increase.
TEST_CASE("LightPresets: growing channels preserves existing role picks") {
    LightPresetsModule m;
    m.setup();
    uint32_t id = 0;
    m.addListRow(id);                                  // 3 channels, R,G,B
    m.setListRowField(id, "ch0", "{\"value\":5}");     // 5 = Pan (a distinctive non-default pick)
    m.setListRowField(id, "ch1", "{\"value\":6}");     // 6 = Tilt

    // Grow 3 → 6. ch0/ch1/ch2 keep their picks; ch3..ch5 get defaults.
    CHECK(m.setListRowField(id, "channels", "{\"value\":6}"));
    Correction c;
    REQUIRE(m.deriveCorrection(id, 255, c));
    CHECK(c.outChannels == 6);
    // Pan(5)/Tilt(6) are non-color roles → not color offsets, but ch2 was Blue and MUST survive.
    CHECK(c.offBlue == 2);     // ch2 = B preserved across the grow (the bug lost this)
    // A second grow then a shrink keeps the head stable.
    CHECK(m.setListRowField(id, "channels", "{\"value\":4}"));   // shrink 6 → 4
    REQUIRE(m.deriveCorrection(id, 255, c));
    CHECK(c.outChannels == 4);
    CHECK(c.offBlue == 2);     // ch2 still B after shrink
}

TEST_CASE("LightPresets: a built-in is protected; a missing id does not resolve") {
    LightPresetsModule m;
    m.setup();
    const uint32_t builtinId = m.defaultId();
    CHECK_FALSE(m.deleteListRow(builtinId));                       // locked → protected
    CHECK_FALSE(m.setListRowField(builtinId, "name", "{\"value\":\"x\"}"));  // read-only
    Correction c;
    CHECK_FALSE(m.deriveCorrection(99999, 255, c));               // no such id → false (caller falls back)
}

TEST_CASE("LightPresets: delete + reorder keep ids stable") {
    LightPresetsModule m;
    m.setup();
    uint32_t a = 0, b = 0;
    m.addListRow(a); m.addListRow(b);      // two custom presets after the built-ins
    CHECK(m.listRowCount() == kBuiltinCount + 2);
    // Move b to the front. It clamps to the first CUSTOM slot (the built-ins are a fixed locked block at the top), and its id is unchanged so it still resolves.
    CHECK(m.moveListRow(b, 0));
    Correction c;
    CHECK(m.deriveCorrection(b, 255, c));  // b still resolves after the reorder
    // Delete a; b survives with its id.
    CHECK(m.deleteListRow(a));
    CHECK(m.listRowCount() == kBuiltinCount + 1);
    CHECK(m.deriveCorrection(b, 255, c));  // b's id still resolves after a's deletion
    CHECK_FALSE(m.deriveCorrection(a, 255, c));   // a is gone
}

TEST_CASE("LightPresets: a custom preset round-trips through persistence with its roles") {
    // Build a module with a custom 4-channel GRBW preset, serialize its presets List, then restore into a fresh module and confirm the wiring (not just the name) survived.
    LightPresetsModule src;
    src.setup();
    uint32_t id = 0;
    src.addListRow(id);
    src.setListRowField(id, "name", "{\"value\":\"MovingHead\"}");
    src.setListRowField(id, "channels", "{\"value\":4}");
    // Set ALL four channels to an unambiguous GRBW wiring (G,R,B,W), each role appears once.
    src.setListRowField(id, "ch0", "{\"value\":2}");   // G
    src.setListRowField(id, "ch1", "{\"value\":1}");   // R
    src.setListRowField(id, "ch2", "{\"value\":3}");   // B
    src.setListRowField(id, "ch3", "{\"value\":4}");   // W
    const std::string rows = presetsJson(src);
    REQUIRE(!rows.empty());

    LightPresetsModule dst;
    // restoreList takes the FULL object {key:[...]}; wrap the array under "presets".
    const std::string wrapped = "{\"presets\":" + rows + "}";
    CHECK(dst.restoreList(wrapped.c_str(), "presets"));
    CHECK(dst.listRowCount() == kBuiltinCount + 1);   // built-ins + the custom one, all restored

    // The custom preset resolves to the SAME wiring after the round-trip.
    Correction c;
    REQUIRE(dst.deriveCorrection(id, 255, c));   // same id, resolved from restored data
    CHECK(c.outChannels == 4);
    CHECK(c.offGreen == 0);   // G at channel 0 survived
    CHECK(c.offWhite == 3);   // W at channel 3 survived
}

// Regression: a preset name containing a JSON metacharacter (a double-quote or a backslash) must round-trip through persistence. writeListRow escapes the name via writeJsonString; a raw %s would emit malformed JSON that fails to parse on the next boot, SILENTLY WIPING every custom preset, a legal keystroke ("MH \"BeeEyes\"") wiping the whole library. The persisted form must be valid JSON for any name the editor accepts.
TEST_CASE("LightPresets: a preset name with a quote or backslash survives persistence") {
    LightPresetsModule src;
    src.setup();
    uint32_t id = 0;
    src.addListRow(id);
    // A name carrying both a double-quote and a backslash, exactly what breaks a raw %s emit. The field-edit path decodes JSON escapes, so this lands the literal 5 chars  A"B\C  in the name.
    src.setListRowField(id, "name", "{\"value\":\"A\\\"B\\\\C\"}");
    src.setListRowField(id, "channels", "{\"value\":3}");
    const std::string rows = presetsJson(src);
    REQUIRE(!rows.empty());

    LightPresetsModule dst;
    const std::string wrapped = "{\"presets\":" + rows + "}";
    CHECK(dst.restoreList(wrapped.c_str(), "presets"));   // MUST parse — the whole point
    CHECK(dst.listRowCount() == kBuiltinCount + 1);       // the custom preset survived, not wiped
    // And the driver can still resolve it (the row is intact, not a half-parsed remnant).
    Correction c;
    CHECK(dst.deriveCorrection(id, 255, c));
}

// Regression: a persisted role byte out of the valid ChannelRole range (a hand-edited / corrupt file) must clamp to a safe default on restore, matching the validation setListRowField applies on the live edit path. An unclamped cast would store e.g. 250, which the UI's role Select mis-renders and the derived Correction silently drops, the "corrupted-but-looks-fine" outcome the robustness contract forbids. (The load path is ApplyPolicy::Clamp: a stale/bad value snaps to a valid one, never survives.)
TEST_CASE("LightPresets: an out-of-range persisted role clamps to a default on restore") {
    // A hand-authored persisted array with role bytes past kChannelRoleCount (250) and a negative (-1). R (role 1) sits at channel 2, a non-default offset, so a passing offRed==2 proves the value came from the restored roles, not Correction's built-in default (offRed=1).
    const std::string wrapped =
        "{\"presets\":[{\"id\":900,\"name\":\"corrupt\",\"channels\":3,\"roles\":[250,-1,1]}]}";
    LightPresetsModule dst;
    CHECK(dst.restoreList(wrapped.c_str(), "presets"));
    CHECK(dst.listRowCount() == 1);   // this hand-authored file carries only the one corrupt preset

    // The valid role (1 = R) at channel 2 survives; the two bad ones (250, -1) become 0 (None), not 250/255. If they hadn't clamped, no channel would carry R and offRed would fall back to default.
    Correction c;
    REQUIRE(dst.deriveCorrection(900, 255, c));
    CHECK(c.outChannels == 3);
    CHECK(c.offRed == 2);      // R restored at channel 2 — proves the in-range role survived the clamp
}

// Driver reference (Inc 2): a driver's `preset` Select is populated from the library and picking a preset changes the driver's resolved Correction. This would have caught the "(none)" bug where the library seat was claimed too late (in prepare, phase 4) for the driver's defineControls (phase 1) to see it, the seat is now claimed at construction, so active() is available immediately.
#include "light/drivers/NetworkSendDriver.h"

TEST_CASE("A driver's preset Select is populated from the library, and picking one resolves") {
    LightPresetsModule lib;            // construction claims the seat + defineControls seeds built-ins
    lib.defineControls();
    REQUIRE(mm::LightPresetsModule::active() == &lib);   // seat held immediately, not only after prepare

    mm::NetworkSendDriver drv;         // defaults to referencing "RGB"
    drv.defineControls();              // builds the preset Select from the library

    // The preset Select exists and its options are the library's names, NOT the "(none)" fallback.
    const mm::ControlDescriptor* preset = nullptr;
    for (uint8_t i = 0; i < drv.controls().count(); i++)
        if (std::strcmp(drv.controls()[i].name, "lightPreset") == 0) preset = &drv.controls()[i];
    REQUIRE(preset != nullptr);
    REQUIRE(preset->max >= kBuiltinCount);   // option count: the built-ins at least (max carries it for a Select)
    const auto* opts = reinterpret_cast<const char* const*>(preset->aux);
    CHECK(std::strcmp(opts[0], "RGB") == 0);
    CHECK(std::strcmp(opts[1], "GRB") == 0);

    // Default reference "RGB" resolves to RGB order.
    drv.rebuildCorrection(255);
    CHECK(drv.correctionForTest().offRed == 0);   // RGB: R at 0
    CHECK(drv.correctionForTest().offGreen == 1);

    // Pick "GRB" (index 1) via the Select the EXACT way Scheduler::setControl does: write the Select value, THEN rebuildControls() (buildPresetOptions re-runs), THEN onControlChanged(). The rebuild runs BEFORE onControlChanged, so buildPresetOptions must honour the fresh Select value, else it re-syncs the index back to the old id and the pick is silently reverted (the live bug).
    *static_cast<uint8_t*>(preset->ptr) = 1;       // select index 1 = GRB
    drv.rebuildControls();                          // production order: rebuild FIRST
    drv.onControlChanged("lightPreset");                 // then the change reaction
    CHECK(drv.correctionForTest().offGreen == 0);  // GRB: G at 0 — the pick took, not reverted
    CHECK(drv.correctionForTest().offRed == 1);    // R at 1
}

// CONSISTENCY (the product owner's requirement): editing a preset's wiring must immediately reach EVERY driver that references it, no reboot. The device wires this via prepareTree() (fired by the list mutation), which re-runs each driver's rebuildCorrection() → re-resolves its preset from the library. This test edits a referenced preset and re-resolves, asserting the driver's Correction now reflects the edit. Two drivers on the SAME preset both update, one shared definition, consistent.
TEST_CASE("Editing a preset flows to every driver referencing it (consistency)") {
    LightPresetsModule lib;
    lib.defineControls();

    // Two drivers both reference the same custom preset.
    uint32_t id = 0;
    lib.addListRow(id);
    lib.setListRowField(id, "name", "{\"value\":\"Shared\"}");
    lib.setListRowField(id, "channels", "{\"value\":3}");
    lib.setListRowField(id, "ch0", "{\"value\":1}");   // R
    lib.setListRowField(id, "ch1", "{\"value\":2}");   // G
    lib.setListRowField(id, "ch2", "{\"value\":3}");   // B  → RGB order

    mm::NetworkSendDriver a, b;
    for (auto* d : {&a, &b}) {
        d->defineControls();
        // Point each driver at "Shared" by name (the persisted reference), then resolve. (setDefaultPresetName is protected; drive it via the Select instead.)
        for (uint8_t i = 0; i < d->controls().count(); i++)
            if (std::strcmp(d->controls()[i].name, "lightPreset") == 0)
                *static_cast<uint8_t*>(d->controls()[i].ptr) = lib.indexOfId(id);
        d->rebuildControls();
        d->onControlChanged("lightPreset");
        d->rebuildCorrection(255);
    }
    // Both see RGB order now.
    CHECK(a.correctionForTest().offRed == 0);
    CHECK(b.correctionForTest().offRed == 0);

    // EDIT the shared preset: swap to GRB order (G,R,B). This is the mutation the UI does.
    lib.setListRowField(id, "ch0", "{\"value\":2}");   // G
    lib.setListRowField(id, "ch1", "{\"value\":1}");   // R

    // Re-resolve each driver, as prepareTree() does after the mutation. Both must now see GRB.
    a.rebuildCorrection(255);
    b.rebuildCorrection(255);
    CHECK(a.correctionForTest().offGreen == 0);   // driver A picked up the edit
    CHECK(a.correctionForTest().offRed == 1);
    CHECK(b.correctionForTest().offGreen == 0);   // driver B too — one definition, both consistent
    CHECK(b.correctionForTest().offRed == 1);
}

// A newly-ADDED preset must become selectable on a driver, the driver's `preset` Select option set is built from the library, so adding a preset has to refresh it. On the device, afterListMutation rebuilds every module's controls after a list mutation for exactly this reason; here we simulate that by rebuilding the driver's controls after the add and checking the Select grew.
TEST_CASE("A newly-added preset becomes selectable on a driver") {
    LightPresetsModule lib;
    lib.defineControls();
    const uint8_t before = lib.presetCount();      // the seeded built-ins

    mm::NetworkSendDriver drv;
    drv.defineControls();
    auto presetOptCount = [&]() -> int {
        for (uint8_t i = 0; i < drv.controls().count(); i++)
            if (std::strcmp(drv.controls()[i].name, "lightPreset") == 0)
                return drv.controls()[i].max;      // Select option count rides `max`
        return -1;
    };
    CHECK(presetOptCount() == before);             // driver Select has the built-ins

    // Add a custom preset, then rebuild the driver's controls (afterListMutation does this device-side).
    uint32_t id = 0;
    REQUIRE(lib.addListRow(id));
    drv.rebuildControls();
    CHECK(presetOptCount() == before + 1);         // the new preset is now an option (was out-of-range before)

    // And it's actually selectable + resolves: pick the last index, rebuild, no out-of-range.
    for (uint8_t i = 0; i < drv.controls().count(); i++)
        if (std::strcmp(drv.controls()[i].name, "lightPreset") == 0)
            *static_cast<uint8_t*>(drv.controls()[i].ptr) = lib.indexOfId(id);
    drv.rebuildControls();
    drv.onControlChanged("lightPreset");
    mm::Correction c;
    CHECK(lib.deriveCorrection(id, 255, c));        // the driver now references the new preset, resolves fine
}

// whiteMode visibility: a driver's whiteMode control is hidden unless the REFERENCED preset carries a white channel, an RGB/GRB strip has nothing to synthesise. Regression: after Inc 2 moved preset selection to a library reference, whiteMode was shown for every preset (the old inline-preset hasWhite check was gone).
TEST_CASE("whiteMode is hidden for a no-white preset, shown for an RGBW one") {
    LightPresetsModule lib;
    lib.defineControls();                              // seeds RGB(0) GRB(1) BGR(2) RGBW(3) GRBW(4)

    mm::NetworkSendDriver drv;
    auto whiteModeHidden = [&]() -> int {
        drv.rebuildControls();
        for (uint8_t i = 0; i < drv.controls().count(); i++)
            if (std::strcmp(drv.controls()[i].name, "whiteMode") == 0)
                return drv.controls()[i].hidden ? 1 : 0;
        return -1;
    };
    auto pickPreset = [&](uint8_t idx) {
        for (uint8_t i = 0; i < drv.controls().count(); i++)
            if (std::strcmp(drv.controls()[i].name, "lightPreset") == 0)
                *static_cast<uint8_t*>(drv.controls()[i].ptr) = idx;
        drv.rebuildControls();          // buildPresetOptions maps idx→id
        drv.onControlChanged("lightPreset");
    };
    drv.defineControls();

    pickPreset(1);                       // GRB — no white
    CHECK(whiteModeHidden() == 1);       // hidden

    pickPreset(3);                       // RGBW — has white
    CHECK(whiteModeHidden() == 0);       // shown

    pickPreset(0);                       // RGB — no white again
    CHECK(whiteModeHidden() == 1);       // hidden
}

// Find a built-in preset id by its name (the seeded rows have stable names).
namespace {
uint32_t builtinId(LightPresetsModule& m, const char* name) {
    for (uint8_t i = 0; i < m.presetCount(); i++)
        if (std::strcmp(m.nameAt(i), name) == 0) return m.idAt(i);
    return 0;
}
}  // namespace

// The migrated color-order built-ins resolve to the right channel offsets. WRGB (ws2814) puts white at channel 0, so R/G/B shift up one, a distinctive layout that catches a bad migration.
TEST_CASE("Built-in WRGB resolves to W,R,G,B offsets") {
    LightPresetsModule m;
    m.setup();
    Correction c;
    REQUIRE(m.deriveCorrection(builtinId(m, "WRGB"), 255, c));
    CHECK(c.outChannels == 4);
    CHECK(c.offWhite == 0);   // W first (ws2814)
    CHECK(c.offRed == 1);
    CHECK(c.offGreen == 2);
    CHECK(c.offBlue == 3);
}

// RGBCCT carries a cold white (W) AND a warm white (WW). The new WarmWhite role must count as "has white" so a driver referencing it still shows whiteMode, the second white is white too.
TEST_CASE("Built-in RGBCCT has white (WarmWhite counts) and resolves 5 channels") {
    LightPresetsModule m;
    m.setup();
    const uint32_t id = builtinId(m, "RGBCCT");
    REQUIRE(id != 0);
    CHECK(m.presetHasSynthChannel(id));   // W at ch3 (WarmWhite at ch4 / Yellow / UV would also satisfy)
    Correction c;
    REQUIRE(m.deriveCorrection(id, 255, c));
    CHECK(c.outChannels == 5);
    CHECK(c.offRed == 0); CHECK(c.offGreen == 1); CHECK(c.offBlue == 2);
    CHECK(c.offWhite == 3);               // cold white derives as the White offset
}

// A moving-head built-in migrates as a wide fixture: the RGB block sits at its real offset within the DMX map (BeeEyes: R@10,G@11,B@12 of 15), the fixture is the right width, and it resolves without crashing at that odd width (Robust-to-any-input). The Pan/Tilt/Zoom/Gobo channels carry their roles in the preset but aren't color offsets, so they're inert until effect writers land.
TEST_CASE("Built-in moving head (BeeEyes-15) resolves at full width with the RGB block placed") {
    LightPresetsModule m;
    m.setup();
    const uint32_t id = builtinId(m, "MH BeeEyes 15");
    REQUIRE(id != 0);
    Correction c;
    REQUIRE(m.deriveCorrection(id, 255, c));   // resolves at 15 channels, no crash
    CHECK(c.outChannels == 15);
    CHECK(c.offRed == 10);                 // RGB block within the wide DMX map
    CHECK(c.offGreen == 11);
    CHECK(c.offBlue == 12);
}

// APPEND-ONLY regression: inserting WarmWhite/Yellow/UV after White must NOT renumber the existing color roles, or every persisted RGBW preset's bytes would resolve to the wrong colors. A straight RGBW built-in still deriving R@0,G@1,B@2,W@3 proves the low indices are unchanged.
TEST_CASE("Color roles keep their indices after the vocabulary grew (append-only)") {
    LightPresetsModule m;
    m.setup();
    Correction c;
    REQUIRE(m.deriveCorrection(builtinId(m, "RGBW"), 255, c));
    CHECK(c.offRed == 0); CHECK(c.offGreen == 1); CHECK(c.offBlue == 2); CHECK(c.offWhite == 3);
}

// MIGRATION SECURITY (the product owner's ask): a driver whose referenced preset no longer exists, a custom preset deleted, or a persisted reference to a preset a firmware no longer ships, must fall back to the default (first) built-in, resolving to a valid RGB output rather than blanking or crashing. rebuildCorrection re-points a dangling id to defaultId(); this pins that path so the fallback can't silently regress (Robust-to-any-input). Two routes are checked: a deleted custom, and a straight-up bogus id.
TEST_CASE("A driver referencing a missing preset falls back to the default built-in") {
    LightPresetsModule lib;
    lib.setup();                                   // seeds the built-ins; first is RGB (R@0,G@1,B@2)

    // Route 1: reference a custom preset, then delete it out from under the driver.
    uint32_t custom = 0;
    lib.addListRow(custom);
    lib.setListRowField(custom, "channels", "{\"value\":4}");
    lib.setListRowField(custom, "ch0", "{\"value\":4}");   // W first — distinct from the RGB default

    mm::NetworkSendDriver drv;
    drv.defineControls();
    for (uint8_t i = 0; i < drv.controls().count(); i++)
        if (std::strcmp(drv.controls()[i].name, "lightPreset") == 0)
            *static_cast<uint8_t*>(drv.controls()[i].ptr) = lib.indexOfId(custom);
    drv.rebuildControls();
    drv.onControlChanged("lightPreset");
    drv.rebuildCorrection(255);
    REQUIRE(drv.correctionForTest().outChannels == 4);     // referencing the custom

    // Delete the referenced preset, the driver's stored id now dangles.
    REQUIRE(lib.deleteListRow(custom));
    drv.rebuildCorrection(255);                            // re-resolve, as the device does after a mutation
    // Fell back to the default (RGB): valid 3-channel output, R@0/G@1/B@2, not blanked, not stale W-first.
    CHECK(drv.correctionForTest().outChannels == 3);
    CHECK(drv.correctionForTest().offRed == 0);
    CHECK(drv.correctionForTest().offGreen == 1);
    CHECK(drv.correctionForTest().offBlue == 2);

    // Route 2: a bogus id that was never a preset (a persisted ref to a dropped firmware fixture) still resolves to the default, never crashing.
    Correction c;
    CHECK_FALSE(lib.deriveCorrection(0xDEADBEEF, 255, c)); // the id itself doesn't resolve…
    CHECK(lib.deriveCorrection(lib.defaultId(), 255, c));  // …and the default always does
    CHECK(c.outChannels == 3);
}

// whiteLevel reaches the White and WarmWhite dies only, so its gate is narrower than whiteMode's. A preset carrying amber but no white would otherwise show a slider that changes nothing.
TEST_CASE("whiteLevel is shown for a white preset and hidden for a no-white one") {
    LightPresetsModule lib;
    lib.defineControls(); // seeds RGB(0) GRB(1) BGR(2) RGBW(3) GRBW(4)

    mm::NetworkSendDriver drv;
    auto whiteLevelHidden = [&]() -> int {
        drv.rebuildControls();
        for (uint8_t i = 0; i < drv.controls().count(); i++)
            if (std::strcmp(drv.controls()[i].name, "whiteLevel") == 0)
                return drv.controls()[i].hidden ? 1 : 0;
        return -1;
    };
    auto pickPreset = [&](uint8_t idx) {
        for (uint8_t i = 0; i < drv.controls().count(); i++)
            if (std::strcmp(drv.controls()[i].name, "lightPreset") == 0)
                *static_cast<uint8_t*>(drv.controls()[i].ptr) = idx;
        drv.rebuildControls();
        drv.onControlChanged("lightPreset");
    };
    drv.defineControls();

    pickPreset(1); // GRB - no white die
    CHECK(whiteLevelHidden() == 1);

    pickPreset(4); // GRBW - carries White
    CHECK(whiteLevelHidden() == 0);
}
