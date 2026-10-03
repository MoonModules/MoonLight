/// @module Control
/// @also FilesystemModule

/// Regression test for the persistence-overlay bug that silently zeroed a control's non-zero default when a saved JSON file omitted that control's key.
///
/// The bug: applyControlValue() called mm::json::parseInt(json, key), which returns 0 for an ABSENT key, indistinguishable from a real 0, and then wrote that 0 into the control (Clamp policy). So loading an older/partial NetworkModule.json (one written before a control existed, or that simply didn't include it) clobbered the control's chip default. On the ESP32-P4 this zeroed eth `ethType` (default 2 = IP101) to 0 (= none), so ethInit() dispatched to "no Ethernet" and the board got a link but never a DHCP lease. The fix: applyControlValue() skips absent keys (mm::json::hasKey guard), leaving the control at its current value.
///
/// These tests pin the contract at the unit level so the regression can't recur: an absent key must NEVER mutate a control; a present key still applies.

#include "doctest.h"
#include "core/module/Control.h"
#include "core/util/JsonUtil.h"
#include "core/util/JsonSink.h"   // PaletteOptionsFn's JsonSink parameter (the palette-crash regression)

#include <cstdint>
#include <cstring>
#include <string>   // std::string — the overlong-label regression builds its JSON

// hasKey distinguishes an absent key from one whose value is 0, the capability the fix relies on. parseInt alone can't (returns 0 for both).
TEST_CASE("json::hasKey detects presence independent of value") {
    const char* json = "{\"a\":0,\"b\":5}";
    CHECK(mm::json::hasKey(json, "a"));        // present, value 0
    CHECK(mm::json::hasKey(json, "b"));        // present, value 5
    CHECK_FALSE(mm::json::hasKey(json, "c"));  // absent
    CHECK_FALSE(mm::json::hasKey(json, ""));
    CHECK_FALSE(mm::json::hasKey(nullptr, "a"));
}

// The core regression: a control bound with a non-zero value, overlaid with a JSON that does NOT contain its key, must keep its value, not snap to 0.
TEST_CASE("applyControlValue leaves a control untouched when its key is absent") {
    mm::ControlList controls;

    // Mirror the eth controls that triggered the bug: a Select (ethType, default 2) and an Int16 (a pin, default 31), plus a Uint8 and a Bool for coverage.
    uint8_t  ethType = 2;                 // IP101 — the value that got zeroed on P4
    int16_t  mdcGpio = 31;
    uint8_t  small   = 7;
    bool      flag   = true;
    static const char* const opts[] = {"None", "LAN8720", "IP101", "W5500"};
    controls.addSelect("ethType", ethType, opts, 4);
    controls.addControl("ethMdcGpio", mdcGpio, -1, 48);
    controls.addControl("small", small, 0, 100);
    controls.addControl("flag", flag);

    // A persisted file that contains an UNRELATED key only, none of our controls.
    const char* partialJson = "{\"ssid\":\"home\"}";

    // Clamp policy is what the persistence overlay uses. Every absent key must be a no-op (Ok, value preserved), this is exactly what failed before the fix.
    for (uint8_t i = 0; i < controls.count(); i++) {
        auto r = mm::applyControlValue(controls[i], partialJson, controls[i].name,
                                       mm::ApplyPolicy::Clamp);
        CHECK(r == mm::ApplyResult::Ok);
    }
    CHECK(ethType == 2);    // NOT zeroed — the bug would have made this 0
    CHECK(mdcGpio == 31);
    CHECK(small == 7);
    CHECK(flag == true);
}

// A present key still applies (the fix must not break the normal load path).
TEST_CASE("applyControlValue still applies a present key") {
    mm::ControlList controls;
    uint8_t ethType = 2;
    int16_t mdcGpio = 31;
    static const char* const opts[] = {"None", "LAN8720", "IP101", "W5500"};
    controls.addSelect("ethType", ethType, opts, 4);
    controls.addControl("ethMdcGpio", mdcGpio, -1, 48);

    // Saved file carries new values for both.
    const char* json = "{\"ethType\":3,\"ethMdcGpio\":23}";
    CHECK(mm::applyControlValue(controls[0], json, "ethType", mm::ApplyPolicy::Clamp)
          == mm::ApplyResult::Ok);
    CHECK(mm::applyControlValue(controls[1], json, "ethMdcGpio", mm::ApplyPolicy::Clamp)
          == mm::ApplyResult::Ok);
    CHECK(ethType == 3);    // applied
    CHECK(mdcGpio == 23);   // applied
}

// A present key whose value IS 0 must apply the 0 (don't confuse "present 0" with "absent"). Guards against an over-eager fix that skipped on value rather than key.
TEST_CASE("applyControlValue applies an explicit zero when the key is present") {
    mm::ControlList controls;
    uint8_t ethType = 2;
    static const char* const opts[] = {"None", "LAN8720", "IP101", "W5500"};
    controls.addSelect("ethType", ethType, opts, 4);

    const char* json = "{\"ethType\":0}";   // explicitly set to None
    CHECK(mm::applyControlValue(controls[0], json, "ethType", mm::ApplyPolicy::Clamp)
          == mm::ApplyResult::Ok);
    CHECK(ethType == 0);    // explicit 0 IS applied
}

// A per-control validator (ControlDescriptor::validate) runs on EVERY write path, the backend home for input rules that used to live in a bespoke per-transport RPC (e.g. deviceModel's printable-ASCII check, formerly the SET_DEVICE_MODEL Improv RPC). A reject returns Malformed and leaves the stored value untouched (no partial write); any transport (HTTP, APPLY_OP over serial, persistence) gets the check for free.
static bool acceptPrintableAscii(const char* v) {
    if (!v) return false;
    size_t n = std::strlen(v);
    if (n == 0 || n >= 32) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = static_cast<unsigned char>(v[i]);
        if (b < 0x20 || b > 0x7E) return false;
    }
    return true;
}

TEST_CASE("a per-control validator accepts a valid value and rejects bad input") {
    mm::ControlList controls;
    char deviceModel[32] = "initial";
    controls.addText("deviceModel", deviceModel, sizeof(deviceModel), acceptPrintableAscii);

    // Valid printable-ASCII → applied.
    CHECK(mm::applyControlValue(controls[0], "{\"deviceModel\":\"LOLIN D32\"}",
                                "deviceModel", mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(std::strcmp(deviceModel, "LOLIN D32") == 0);

    // A raw non-printable byte embedded in the value (0x01), parseString copies bytes verbatim (it only un-escapes \" and \\), so a wire-untrusted control byte reaches the validator, which rejects it → Malformed, prior value preserved (no partial write).
    const char bad[] = {'{','"','d','e','v','i','c','e','M','o','d','e','l','"',':','"',
                        'b','a','d', 0x01, 'x','"','}', 0};
    CHECK(mm::applyControlValue(controls[0], bad,
                                "deviceModel", mm::ApplyPolicy::Clamp) == mm::ApplyResult::Malformed);
    CHECK(std::strcmp(deviceModel, "LOLIN D32") == 0);   // unchanged

    // Empty string → Malformed (the validator rejects 0-length), prior value preserved.
    CHECK(mm::applyControlValue(controls[0], "{\"deviceModel\":\"\"}",
                                "deviceModel", mm::ApplyPolicy::Clamp) == mm::ApplyResult::Malformed);
    CHECK(std::strcmp(deviceModel, "LOLIN D32") == 0);
}

// Length boundary of the deviceModel validator (accepts 1..31). Uses a buffer wider than the validator's limit so the 32-char value reaches the validator intact (parseString truncates to bufSize-1, so the buffer must exceed 32 for the validator's own length check, not parse truncation, to be what rejects it). The scratch buffer in applyControlValue is sized to bufSize, so a long value isn't truncated before validation.
TEST_CASE("the validator enforces its length limit on the long end") {
    mm::ControlList controls;
    char label[64] = "init";   // wider than the validator's 31-char limit
    controls.addText("label", label, sizeof(label), acceptPrintableAscii);

    const char s31[] = "{\"label\":\"1234567890123456789012345678901\"}";   // 31 chars
    CHECK(mm::applyControlValue(controls[0], s31, "label", mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(std::strlen(label) == 31);

    const char s32[] = "{\"label\":\"12345678901234567890123456789012\"}";  // 32 chars → rejected
    CHECK(mm::applyControlValue(controls[0], s32, "label", mm::ApplyPolicy::Clamp) == mm::ApplyResult::Malformed);
    CHECK(std::strlen(label) == 31);   // prior 31-char value preserved, not overwritten/truncated
}

TEST_CASE("a Text control with no validator accepts anything that fits") {
    mm::ControlList controls;
    char label[16] = {};
    controls.addText("label", label, sizeof(label));   // no validator

    CHECK(mm::applyControlValue(controls[0], "{\"label\":\"hi\"}",
                                "label", mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(std::strcmp(label, "hi") == 0);
}

// A Palette control's aux holds a PaletteOptionsFn, a function rather than an options array, so a name resolves through the function's name request and never by walking aux as an array, which would read code bytes and crash on ESP32.
static void paletteOptions(mm::JsonSink& sink) {
    static const char* const names[] = {"Rainbow", "Ocean", "fire.mlp"};
    if (sink.nameIndex() >= 0) {
        if (sink.nameIndex() < 3) sink.append(names[sink.nameIndex()]);
        return;
    }
    sink.append("{\"name\":\"Rainbow\"},{\"name\":\"Ocean\"},{\"name\":\"fire.mlp\"}");
}
TEST_CASE("applyControlValue: a palette takes a name as well as an index, and an unknown name changes nothing") {
    mm::ControlList controls;
    uint8_t palette = 1;
    controls.addPalette("palette", palette, paletteOptions, 3);

    // A scripted palette's index moves as files come and go, so its name is what a client can rely on.
    CHECK(mm::applyControlValue(controls[0], "{\"palette\":\"fire.mlp\"}", "palette",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(palette == 2);

    // A name no palette carries leaves the choice alone, and Strict says so.
    CHECK(mm::applyControlValue(controls[0], "{\"palette\":\"Nowhere\"}", "palette",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(palette == 2);
    CHECK(mm::applyControlValue(controls[0], "{\"palette\":\"Nowhere\"}", "palette",
                                mm::ApplyPolicy::Strict) == mm::ApplyResult::OutOfRange);

    // A numeric value still applies straight through.
    CHECK(mm::applyControlValue(controls[0], "{\"palette\":1}", "palette",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(palette == 1);
}

// The complement: a Select's aux IS the options array, so a string LABEL value matches an option by name (the board-portable catalog path, a peripheral label is stable while its filtered index is not). This keeps the label-match feature working where it is safe.
TEST_CASE("applyControlValue: a Select accepts an option label as a string value") {
    mm::ControlList controls;
    uint8_t sel = 0;
    static const char* const opts[] = {"i80", "MoonI80", "Parlio"};
    controls.addSelect("peripheral", sel, opts, 3);

    CHECK(mm::applyControlValue(controls[0], "{\"peripheral\":\"Parlio\"}", "peripheral",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(sel == 2);   // matched the "Parlio" label → index 2

    // A numeric index still works alongside the label path.
    CHECK(mm::applyControlValue(controls[0], "{\"peripheral\":1}", "peripheral",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(sel == 1);
}

// Saved by name, a scripted palette survives a script added before it, which shifts every index after it.
TEST_CASE("writeControlValue: a palette saved by name round-trips, and an unflagged one stays an index") {
    mm::ControlList controls;
    uint8_t palette = 2;
    controls.addPalette("palette", palette, paletteOptions, 3);
    mm::JsonSink plain;
    mm::writeControlValue(plain, controls[0]);
    CHECK(std::string(plain.data(), plain.size()) == "2");

    controls.setPersistLabel(0);
    mm::JsonSink named;
    mm::writeControlValue(named, controls[0]);
    CHECK(std::string(named.data(), named.size()) == "\"fire.mlp\"");

    palette = 0;
    const std::string saved = std::string("{\"palette\":") + std::string(named.data(), named.size()) + "}";
    CHECK(mm::applyControlValue(controls[0], saved.c_str(), "palette", mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(palette == 2);
}

// An empty option list (max == 0) has no valid index, applying a value must not manufacture index 0. Strict rejects; Lenient (Clamp) leaves the bound value untouched. Guards a board-filtered Select that filtered down to zero options (e.g. a peripheral list on a chip that supports none).
TEST_CASE("applyControlValue: an empty Select rejects/no-ops instead of accepting index 0") {
    mm::ControlList controls;
    uint8_t sel = 7;                       // a sentinel that a spurious index-0 write would clobber
    static const char* const noOpts[] = {nullptr};
    controls.addSelect("peripheral", sel, noOpts, 0);   // zero options

    CHECK(mm::applyControlValue(controls[0], "{\"peripheral\":0}", "peripheral",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(sel == 7);   // untouched — no index 0 manufactured
    CHECK(mm::applyControlValue(controls[0], "{\"peripheral\":\"i80\"}", "peripheral",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(sel == 7);   // a label on an empty list is also a no-op
    CHECK(mm::applyControlValue(controls[0], "{\"peripheral\":0}", "peripheral",
                                mm::ApplyPolicy::Strict) == mm::ApplyResult::OutOfRange);
}

// The Palette twin of the empty-Select guard: an empty palette (addPalette(..., 0)) has no valid index, so a value must not write index 0, and critically must not let `hi = c.max - 1` underflow to -1 and clamp the stored value up to 255. Lenient leaves the sentinel untouched; Strict returns OutOfRange.
TEST_CASE("applyControlValue: an empty Palette rejects/no-ops and never underflows to 255") {
    mm::ControlList controls;
    uint8_t pal = 7;                       // sentinel: a spurious index-0 OR a 255 underflow would clobber it
    controls.addPalette("palette", pal, paletteOptions, 0);   // zero options

    CHECK(mm::applyControlValue(controls[0], "{\"palette\":0}", "palette",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(pal == 7);   // untouched — neither index 0 nor a 255 underflow
    CHECK(mm::applyControlValue(controls[0], "{\"palette\":3}", "palette",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(pal == 7);   // a representative palette value is also a no-op on an empty list
    CHECK(pal != 255); // explicit: the c.max-1 underflow the guard prevents
    CHECK(mm::applyControlValue(controls[0], "{\"palette\":0}", "palette",
                                mm::ApplyPolicy::Strict) == mm::ApplyResult::OutOfRange);
}

// A label longer than any real option (here, longer than the parse buffer) must NOT match a real option by prefix, it is "no such option", so Lenient keeps the default and Strict rejects. Guards against a truncated value spuriously equalling a shorter option that shares its leading characters.
TEST_CASE("applyControlValue: an overlong Select label does not prefix-match a real option") {
    mm::ControlList controls;
    uint8_t sel = 0;
    static const char* const opts[] = {"i80", "MoonI80", "Parlio"};
    controls.addSelect("peripheral", sel, opts, 3);

    // 80 'M' chars, far past any option and past the parse buffer; must not match "MoonI80" by prefix.
    std::string longVal(80, 'M');
    std::string json = "{\"peripheral\":\"" + longVal + "\"}";
    CHECK(mm::applyControlValue(controls[0], json.c_str(), "peripheral",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(sel == 0);   // default kept, no spurious match
    CHECK(mm::applyControlValue(controls[0], json.c_str(), "peripheral",
                                mm::ApplyPolicy::Strict) == mm::ApplyResult::OutOfRange);
}

// The exact boundary of the overlong guard. The Select label parses into a 64-byte buffer and a value that FILLS it (length >= 63, i.e. buffer_size - 1) is treated as overlong, it may have been truncated to the cap, so it cannot legitimately equal any option and the match is skipped. A value one shorter (62) is NOT overlong and matches normally. This pins the threshold so a future buffer-size change can't silently shift where a legitimate long label starts being rejected. Real option labels sit far below this (the longest peripheral/mode label is ~35 chars), so the boundary only ever fences off junk, but the test makes that contract explicit rather than incidental.
TEST_CASE("applyControlValue: the Select overlong-label boundary is exactly the parse buffer") {
    mm::ControlList controls;
    uint8_t sel = 0;
    // Two options at the boundary lengths: one 62 chars (just under the cap), one 63 (at the cap).
    static const std::string at62(62, 'a');
    static const std::string at63(63, 'b');
    static const char* const opts[] = {"i80", at62.c_str(), at63.c_str()};
    controls.addSelect("peripheral", sel, opts, 3);

    // 62 chars: not overlong → matches option index 1.
    const std::string j62 = "{\"peripheral\":\"" + at62 + "\"}";
    CHECK(mm::applyControlValue(controls[0], j62.c_str(), "peripheral",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(sel == 1);

    // 63 chars: fills the buffer → treated as overlong, the match is skipped even though an option of that exact text EXISTS. Lenient keeps the current value; Strict rejects.
    sel = 0;
    const std::string j63 = "{\"peripheral\":\"" + at63 + "\"}";
    CHECK(mm::applyControlValue(controls[0], j63.c_str(), "peripheral",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(sel == 0);   // NOT matched to index 2 — the cap fences it off
    CHECK(mm::applyControlValue(controls[0], j63.c_str(), "peripheral",
                                mm::ApplyPolicy::Strict) == mm::ApplyResult::OutOfRange);
}

// A Select over ENUMERATED options (a NIC list, an audio device list) persists by LABEL when flagged: the index shifts when the machine's device list reorders, but the name is what the user chose. Both directions of robustness: a label round-trips, and an old index-persisted value still applies (the apply path always accepted both).
TEST_CASE("a persist-label Select saves the option string and loads by it") {
    static constexpr const char* kNics[] = {"none (capture only)", "Ethernet 5GbE", "WiFi"};
    uint8_t sel = 1;
    mm::ControlList controls;
    controls.addSelect("interface", sel, kNics, 3);
    controls.setPersistLabel(controls.count() - 1);

    // Save: the LABEL, not the index.
    mm::JsonSink sink;
    sink.append("{\"interface\":");
    mm::writeControlValue(sink, controls[0]);
    sink.append("}");
    const std::string saved(sink.data(), sink.size());
    CHECK(saved.find("\"Ethernet 5GbE\"") != std::string::npos);

    // Load onto a REORDERED list (the machine enumerated differently): the name still wins.
    static constexpr const char* kReordered[] = {"none (capture only)", "WiFi", "Ethernet 5GbE"};
    uint8_t sel2 = 0;
    mm::ControlList reloaded;
    reloaded.addSelect("interface", sel2, kReordered, 3);
    reloaded.setPersistLabel(reloaded.count() - 1);
    CHECK(mm::applyControlValue(reloaded[0], saved.c_str(), "interface",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(sel2 == 2);   // found by label at its NEW position

    // Backward compatibility: an old config that stored the index still applies.
    uint8_t sel3 = 0;
    mm::ControlList old;
    old.addSelect("interface", sel3, kNics, 3);
    old.setPersistLabel(old.count() - 1);
    CHECK(mm::applyControlValue(old[0], "{\"interface\":2}", "interface",
                                mm::ApplyPolicy::Clamp) == mm::ApplyResult::Ok);
    CHECK(sel3 == 2);
}
