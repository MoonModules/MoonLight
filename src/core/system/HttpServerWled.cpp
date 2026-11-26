/// HttpServerModule's WLED shim: the routes and the WebSocket frame that make a MoonLight device appear in the WLED apps and Home Assistant's WLED integration.
///
/// @technical
///
/// ## Why the WLED shim reports a sentinel version
///
/// The `ver` field is a sentinel, not the MoonLight version.
///
/// Home Assistant parses WLED tags as CalVer, so a real `2.1.0-dev` compares lower than `16.0.1` and HA offers an update whose `.bin` would brick the device.
///
/// Under CalVer `99.0.0` outranks any WLED tag, so HA's update check stays silent.
///
/// The real version belongs on the MQTT update topic, where the question is whether MoonLight shipped a release.
///
/// ## Why one info object serves every route
///
/// python-wled reads `/json` once, then polls `/json/si`, and merges each reply into the first, so `/json/si` needs every field `/json` carries: a reply without `fs` fails the update and HA marks the device unavailable.
///
/// The WLED-Android app requires `leds`, `wifi`, `name` and `mac` in the same object, and real WLED always sends `wifi`.
///
/// One writer therefore emits the full `info` for `/json`, `/json/info`, `/json/si` and the WebSocket push, and an Ethernet device reports `wifi` with a zero signal.

#include "core/system/HttpServerModule.h"

#include "core/module/Control.h"         // the controls a WLED slider stands for
#include "core/module/Scheduler.h"
#include "core/util/HttpHead.h"          // queryValue: the /win query's keys
#include "core/util/JsonUtil.h"
#include "core/util/ModuleFactory.h"     // the effect types `fx` indexes
#include "core/util/parse.h"             // parseIntStr: the /win query's numbers
#include "core/util/JsonSink.h"
#include "core/util/format.h"            // formatTo: nonblocking formatting into a fixed buffer
#include "core/system/ControlModule.h"   // look presets on /presets.json (HA WLED integration)
#include "core/system/SystemModule.h"      // deviceName() for the WLED /json/info shim
#include "core/util/LightOutput.h"   // the device shape and the palettes as colors, for the WLED shim
#include "platform/platform_clock.h"   // millis
#include "platform/platform_heap.h"   // freeHeap
#include "platform/platform_system.h"   // getMacAddress
#include "platform/platform_net.h"   // TcpConnection, localIPv4, wifiStaRssi

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdint>

namespace mm {

namespace {

using json::JsonDoc;
using json::JsonNode;
using json::JsonType;

// WLED's sliders `sx`, `ix`, `c1`, `c2` and `c3`, and its options `o1`, `o2` and `o3`, in that order.
constexpr uint8_t kSliderCount = 5;
constexpr uint8_t kToggleCount = 3;
// `c3`'s place among the sliders, the one WLED bounds at 31 where a control byte runs to 255.
constexpr uint8_t kCustom3 = 4;
constexpr int kCustom3Max = 31;
// What `switchField` answers for `"t"`, WLED's toggle: flip the current state.
constexpr int kToggle = 2;

// The segments are the layers of `Effects`, in child order; with no `Effects` there are none.
MoonModule* layerAt(const Scheduler* scheduler, int index) {
    const MoonModule* effects = scheduler ? scheduler->topOfType("Effects") : nullptr;
    int seen = 0;
    for (uint8_t i = 0; effects && i < effects->childCount(); i++) {
        MoonModule* child = effects->child(i);
        if (!child || child->role() != ModuleRole::Layer) continue;
        if (seen++ == index) return child;
    }
    return nullptr;
}

int layerCount(const Scheduler* scheduler) {
    int count = 0;
    while (layerAt(scheduler, count)) count++;
    return count;
}

// One segment per layer, or the one stand-in segment on a device without a layer, which keeps HA's light entity available.
int segmentCount(const Scheduler* scheduler) {
    const int layers = layerCount(scheduler);
    return layers ? layers : 1;
}

// The effect a segment's fx, sliders and options stand for: the first effect in the layer.
MoonModule* leadEffect(MoonModule* layer) {
    for (uint8_t i = 0; layer && i < layer->childCount(); i++) {
        MoonModule* child = layer->child(i);
        if (child && child->role() == ModuleRole::Effect) return child;
    }
    return nullptr;
}

bool isEffectType(uint8_t type) {
    return ModuleFactory::typeRole(type) == ModuleRole::Effect;
}

// `fx` counts the registered effect types in registration order, which the effect list names in the same order.
int effectIndexOf(const char* typeName) {
    int seen = 0;
    for (uint8_t i = 0; i < ModuleFactory::typeCount(); i++) {
        if (!isEffectType(i)) continue;
        if (std::strcmp(ModuleFactory::typeName(i), typeName) == 0) return seen;
        seen++;
    }
    return 0;
}

const char* effectTypeAt(int fx) {
    int seen = 0;
    for (uint8_t i = 0; i < ModuleFactory::typeCount(); i++)
        if (isEffectType(i) && seen++ == fx) return ModuleFactory::typeName(i);
    return nullptr;
}

int effectTypeCount() {
    int count = 0;
    for (uint8_t i = 0; i < ModuleFactory::typeCount(); i++)
        if (isEffectType(i)) count++;
    return count;
}

// Every registered effect type's name as JSON strings, comma-separated, in `fx` order.
void writeEffectNames(JsonSink& sink) {
    bool first = true;
    for (uint8_t i = 0; i < ModuleFactory::typeCount(); i++) {
        if (!isEffectType(i)) continue;
        // displayNameFor answers from one shared buffer, written as it is read.
        if (!first) sink.append(",");
        sink.writeJsonString(ModuleFactory::displayNameFor(ModuleFactory::typeName(i), ModuleRole::Effect));
        first = false;
    }
}

// The n-th control of `module` that WLED has a widget for: sliders are the Uint8 controls and options the Bool ones, in declaration order, as WLED's own effects number theirs.
const ControlDescriptor* wledControl(MoonModule& module, ControlType type, uint8_t n) {
    auto& controls = module.controls();
    for (uint8_t i = 0; i < controls.count(); i++) {
        const ControlDescriptor& c = controls[i];
        if (c.type != type || c.hidden || c.readonly) continue;
        if (n-- == 0) return &c;
    }
    return nullptr;
}

// A slider's top value, and its scale to and from the control byte: every slider is a byte but `c3`.
int sliderMax(uint8_t slot) {
    return slot == kCustom3 ? kCustom3Max : 255;
}

int sliderFromControl(uint8_t slot, int control) {
    return slot == kCustom3 ? (control * kCustom3Max + 127) / 255 : control;
}

int sliderToControl(uint8_t slot, int slider) {
    return slot == kCustom3 ? slider * 255 / kCustom3Max : slider;
}

int sliderValue(MoonModule* effect, uint8_t slot) {
    const ControlDescriptor* c = effect ? wledControl(*effect, ControlType::Uint8, slot) : nullptr;
    return c ? sliderFromControl(slot, *static_cast<const uint8_t*>(c->ptr)) : 0;
}

const char* toggleValue(MoonModule* effect, uint8_t slot) {
    const ControlDescriptor* c = effect ? wledControl(*effect, ControlType::Bool, slot) : nullptr;
    return c && *static_cast<const bool*>(c->ptr) ? "true" : "false";
}

// The applied look as a WLED preset slot (1-based, as /presets.json), or -1; without it HA's preset dropdown reads unknown even while a preset is active.
int currentPresetSlot(const ControlModule* control) {
    const char* look = control ? control->currentLook() : nullptr;
    if (!look || !look[0]) return -1;
    for (uint8_t i = 0; i < control->presetCount(); i++) {
        if (!control->isLookOnly(i)) continue;
        const char* name = control->presetName(i);
        if (name && std::strcmp(name, look) == 0) return i + 1;
    }
    return -1;
}

// pmt is the presets-modified time HA uses to re-fetch /presets.json, stamped by ControlModule on every preset change; python-wled treats 0 as "no presets support", so the no-ControlModule fallback is 1.
unsigned presetsStamp(const ControlModule* control) {
    const unsigned stamp = control ? static_cast<unsigned>(control->presetsRevision()) : 0;
    return stamp ? stamp : 1;
}

// One layer as a segment: its enabled flag and opacity, its name, the palette's color, and its first effect's index, sliders and options. With no layer the one stand-in segment carries the Drivers' power.
void writeSegment(JsonSink& sink, Scheduler* scheduler, MoonModule* layer, int index, uint8_t pal, const RGB& color) {
    MoonModule* effect = leadEffect(layer);
    const unsigned lights = lightSummary().lightCount;
    const bool on = layer ? layer->enabled() : driversOn(scheduler);
    // bri 255 on the stand-in, so HA's segment.bri x state.bri / 255 shows the master value.
    sink.appendf("{\"id\":%d,\"start\":0,\"stop\":%u,\"len\":%u,\"on\":%s,\"bri\":%u,\"lc\":1,\"n\":",
                 index, lights, lights, on ? "true" : "false", layer ? static_cast<unsigned>(layer->readUint8("opacity", 255)) : 255u);
    sink.writeJsonString(layer ? layer->name() : "");
    sink.appendf(",\"col\":[[%u,%u,%u]],\"fx\":%d,\"sx\":%d,\"ix\":%d,\"pal\":%u,\"c1\":%d,\"c2\":%d,\"c3\":%d,"
                 "\"o1\":%s,\"o2\":%s,\"o3\":%s,\"sel\":%s}",
                 color.r, color.g, color.b, effect ? effectIndexOf(effect->typeName()) : 0,
                 sliderValue(effect, 0), sliderValue(effect, 1), pal,
                 sliderValue(effect, 2), sliderValue(effect, 3), sliderValue(effect, 4),
                 toggleValue(effect, 0), toggleValue(effect, 1), toggleValue(effect, 2), index == 0 ? "true" : "false");
}

// The integer member `key` of `obj` held to [lo, hi], false when it is absent, not an integer or below `lo`, so a negative number is no value and a string such as "~10", WLED's relative step, is not read.
bool intField(const JsonDoc& doc, const JsonNode* obj, const char* key, int lo, int hi, int& out) {
    const JsonNode* n = json::member(doc, obj, key);
    if (!n || n->type != JsonType::Int || n->intValue < lo) return false;
    out = n->intValue > hi ? hi : static_cast<int>(n->intValue);
    return true;
}

// A flag member as 0 or 1, or kToggle for the string "t"; false when it is absent or of another kind.
bool switchField(const JsonDoc& doc, const JsonNode* obj, const char* key, bool allowToggle, int& out) {
    const JsonNode* n = json::member(doc, obj, key);
    if (!n) return false;
    if (allowToggle && n->type == JsonType::String && n->str && n->str[0] == 't') { out = kToggle; return true; }
    if (n->type != JsonType::Bool && n->type != JsonType::Int) return false;
    out = n->intValue != 0 ? 1 : 0;
    return true;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// A color as WLED writes it in hex, `RRGGBB` or `WWRRGGBB`, whose white byte this RGB output has no use for.
bool hexColor(const char* text, uint8_t rgb[3]) {
    const size_t n = text ? std::strlen(text) : 0;
    if (n != 6 && n != 8) return false;
    for (int i = 0; i < 3; i++) {
        const int hi = hexDigit(text[n - 6 + 2 * i]), lo = hexDigit(text[n - 5 + 2 * i]);
        if (hi < 0 || lo < 0) return false;
        rgb[i] = static_cast<uint8_t>(hi * 16 + lo);
    }
    return true;
}

// The primary color of `col`: an [r,g,b] array or a hex string, as the WLED JSON API gives both.
bool colorField(const JsonDoc& doc, const JsonNode* obj, uint8_t rgb[3]) {
    const JsonNode* primary = json::element(doc, json::member(doc, obj, "col"), 0);
    if (!primary) return false;
    if (primary->type == JsonType::String) return hexColor(primary->str, rgb);
    for (int i = 0; i < 3; i++) {
        const long channel = json::readInt(json::element(doc, primary, i), -1);
        if (channel < 0) return false;
        rgb[i] = static_cast<uint8_t>(channel > 255 ? 255 : channel);
    }
    return true;
}

// A control set to a number or a flag, through the apply path every client shares.
void setNumber(HttpServerModule& http, const char* module, const char* control, int value) {
    char valueJson[24];
    std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%d}", value);
    http.applySetControl(module, control, valueJson);
}

void setFlag(HttpServerModule& http, const char* module, const char* control, bool value) {
    http.applySetControl(module, control, value ? "{\"value\":true}" : "{\"value\":false}");
}

// The palette and its color belong to Drivers, so any segment's pal and col reach the one palette. A pal wins over a col, which is mapped to the nearest palette; HA's brightness slider sends `bri`, so the color's value channel is ignored.
void applyPalette(HttpServerModule& http, const JsonDoc& doc, const JsonNode* seg) {
    const LightOutput* out = LightOutput::active();
    if (!out || out->paletteCount() == 0) return;
    int pal;
    uint8_t rgb[3];
    // Clamped to the full count, built-ins plus the scripted tail, the list served as `palettes[]`, so a scripted index the device offered is not snapped back.
    if (intField(doc, seg, "pal", 0, out->paletteCount() - 1, pal)) setNumber(http, "Drivers", "palette", pal);
    else if (colorField(doc, seg, rgb)) setNumber(http, "Drivers", "palette", out->nearestPalette(RGB{rgb[0], rgb[1], rgb[2]}));
}

// A segment's on and bri are the layer's enabled flag and opacity.
void applyLayer(HttpServerModule& http, const JsonDoc& doc, const JsonNode* seg, MoonModule& layer) {
    int v;
    if (switchField(doc, seg, "on", true, v)) setFlag(http, layer.name(), "enabled", v == kToggle ? !layer.enabled() : v != 0);
    if (intField(doc, seg, "bri", 0, 255, v)) setNumber(http, layer.name(), "opacity", v);
}

// A new effect replaces the layer's first one in its place, or joins an empty layer; answers the effect the sliders then address.
MoonModule* applyEffect(HttpServerModule& http, const JsonDoc& doc, const JsonNode* seg, MoonModule& layer) {
    int fx;
    const char* type = intField(doc, seg, "fx", 0, 255, fx) ? effectTypeAt(fx) : nullptr;
    MoonModule* effect = leadEffect(&layer);
    if (!type) return effect;
    if (!effect) {
        http.applyAddModule(type, nullptr, layer.name());
    } else if (std::strcmp(type, effect->typeName()) != 0) {
        char effectName[MoonModule::kNameLen];   // copied: the replace deletes the module this name sits in
        std::snprintf(effectName, sizeof(effectName), "%s", effect->name());
        http.applyReplaceModule(effectName, type, nullptr);
    }
    return leadEffect(&layer);
}

// The sliders and options a segment names land on the effect's byte and Bool controls, in declaration order. A control is looked up per write: a control's reaction may rebuild the list and move its descriptors.
void applyControls(HttpServerModule& http, const JsonDoc& doc, const JsonNode* seg, MoonModule& effect) {
    static constexpr const char* kSliderKey[kSliderCount] = {"sx", "ix", "c1", "c2", "c3"};
    static constexpr const char* kToggleKey[kToggleCount] = {"o1", "o2", "o3"};
    int v;
    for (uint8_t slot = 0; slot < kSliderCount; slot++) {
        if (!intField(doc, seg, kSliderKey[slot], 0, sliderMax(slot), v)) continue;
        if (const ControlDescriptor* c = wledControl(effect, ControlType::Uint8, slot))
            setNumber(http, effect.name(), c->name, std::clamp(sliderToControl(slot, v), static_cast<int>(c->min), static_cast<int>(c->max)));
    }
    for (uint8_t slot = 0; slot < kToggleCount; slot++) {
        if (!switchField(doc, seg, kToggleKey[slot], false, v)) continue;
        if (const ControlDescriptor* c = wledControl(effect, ControlType::Bool, slot)) setFlag(http, effect.name(), c->name, v != 0);
    }
}

// Apply what a client asks of segment `id`: the device's palette, then the layer and its first effect. A segment past the last layer is ignored whole; a device with no layer reports segment 0 as a stand-in, whose palette still applies.
void applySegment(HttpServerModule& http, Scheduler* scheduler, const JsonDoc& doc, const JsonNode* seg, int id) {
    MoonModule* layer = layerAt(scheduler, id);
    if (!layer && id != 0) return;
    applyPalette(http, doc, seg);
    if (!layer) return;
    applyLayer(http, doc, seg, *layer);
    if (MoonModule* effect = applyEffect(http, doc, seg, *layer)) applyControls(http, doc, seg, *effect);
}

// The layer a segment object names by `id`, or `fallback` when it names none.
int segmentId(const JsonDoc& doc, const JsonNode* element, int fallback) {
    int id;
    return intField(doc, element, "id", 0, 255, id) ? id : fallback;
}

// A body names its segments as an array, or as one object that is the main segment. An element without an id takes its place in the array.
void applySegments(HttpServerModule& http, Scheduler* scheduler, const JsonDoc& doc, const JsonNode* seg) {
    const bool array = seg && seg->type == JsonType::Array;
    const int count = array ? json::arraySize(doc, seg) : (seg ? 1 : 0);
    for (int i = 0; i < count; i++) {
        const JsonNode* element = array ? json::element(doc, seg, i) : seg;
        if (!element || element->type != JsonType::Object) continue;
        applySegment(http, scheduler, doc, element, segmentId(doc, element, array ? i : 0));
    }
}

// The bytes one effect's slider and option labels share in `/json/fxdata`.
constexpr size_t kLabelBytes = 192;

// The palette names as JSON strings, comma-separated, in `pal` order.
void writePaletteNames(JsonSink& sink) {
    if (const LightOutput* out = LightOutput::active()) out->writePaletteNames(sink);
}

// The label of a probe's n-th slider or option, or an empty one.
const char* controlLabel(MoonModule& probe, ControlType type, uint8_t slot) {
    const ControlDescriptor* c = wledControl(probe, type, slot);
    return c ? c->name : "";
}

void appendLabel(char (&labels)[kLabelBytes], size_t& used, const char* name, bool separator) {
    const int n = std::snprintf(labels + used, sizeof(labels) - used, "%s%s", separator ? "," : "", name);
    if (n > 0 && used + static_cast<size_t>(n) < sizeof(labels)) used += static_cast<size_t>(n);
}

// The comma-separated slider labels, then the option labels, of an effect a probe stands for.
void effectLabels(MoonModule& probe, char (&labels)[kLabelBytes]) {
    size_t used = 0;
    uint8_t sliders = 0, options = 0;
    while (sliders < kSliderCount && wledControl(probe, ControlType::Uint8, sliders)) sliders++;
    while (options < kToggleCount && wledControl(probe, ControlType::Bool, options)) options++;
    // Options sit after the fifth slider's place, so a short slider list is padded with empty labels, which the UI hides.
    const uint8_t slots = options ? kSliderCount : sliders;
    for (uint8_t slot = 0; slot < slots; slot++) appendLabel(labels, used, controlLabel(probe, ControlType::Uint8, slot), slot > 0);
    for (uint8_t slot = 0; slot < options; slot++) appendLabel(labels, used, controlLabel(probe, ControlType::Bool, slot), true);
}

// One string per effect, `sliders;colors;palette;flags`, which WLED's own UI reads to show an effect's controls and label them. Sliders are the Uint8 controls, then the options after a fifth slot, as the UI numbers them.
void writeEffectData(JsonSink& sink) {
    bool first = true;
    for (uint8_t i = 0; i < ModuleFactory::typeCount(); i++) {
        if (!isEffectType(i)) continue;
        char labels[kLabelBytes] = {};
        // Built once to read its controls and deleted unreleased, as `/api/types` does for its defaults: release undoes a prepare the probe skipped.
        if (MoonModule* probe = ModuleFactory::create(ModuleFactory::typeName(i))) {
            probe->defineControls();
            effectLabels(*probe, labels);
            delete probe;
        }
        // The palette and one color slot stay on: every effect takes the Drivers palette, and a color picked is matched to one. Flags say which dimensions the effect draws in.
        char entry[240];
        std::snprintf(entry, sizeof(entry), "%s;!;!;%s", labels, ModuleFactory::typeDim(i) >= 2 ? "12" : "1");
        if (!first) sink.append(",");
        sink.writeJsonString(entry);
        first = false;
    }
}

// A /win query is WLED's older HTTP API, `/win&A=128&FX=5`. It is turned into the JSON body the other routes take, so a key means the same on every route.
constexpr int kNoValue = -1;

// The number a /win key names, or kNoValue when it is absent, does not start with a digit, such as WLED's relative step `~10`, or runs past five digits.
int queryNumber(const char* args, const char* key) {
    const char* value = http::queryValue(args, key);
    size_t digits = 0;
    while (value && value[digits] >= '0' && value[digits] <= '9') digits++;
    return digits == 0 || digits > 5 ? kNoValue : parseIntStr(value);
}

struct WinKey { const char* key; const char* field; };
constexpr WinKey kWinDeviceKeys[] = {{"A", "bri"}, {"PL", "ps"}};
constexpr WinKey kWinSegmentKeys[] = {{"SB", "bri"}, {"FX", "fx"}, {"SX", "sx"}, {"IX", "ix"}, {"X1", "c1"}, {"X2", "c2"}, {"X3", "c3"},
                                      {"M1", "o1"}, {"M2", "o2"}, {"M3", "o3"}, {"FP", "pal"}};

// `,"field":number` for each key of `keys` the query names.
template <size_t N>
void appendWinKeys(JsonSink& sink, const char* args, const WinKey (&keys)[N]) {
    for (const WinKey& k : keys) {
        const int value = queryNumber(args, k.key);
        if (value != kNoValue) sink.appendf(",\"%s\":%d", k.field, value);
    }
}

// T is power: 0 off, 1 on, 2 toggle.
void appendWinPower(JsonSink& sink, const char* args) {
    static constexpr const char* kPower[] = {"false", "true", "\"t\""};
    const int power = queryNumber(args, "T");
    if (power >= 0 && power <= kToggle) sink.appendf(",\"on\":%s", kPower[power]);
}

int channelOr(int named, int current) { return named == kNoValue ? current : named; }

// CL is the whole color in hex, and R, G and B each change one channel of the `current` color, as WLED's do. Only hex digits reach the body.
void appendWinColor(JsonSink& sink, const char* args, const RGB& current) {
    const char* hex = http::queryValue(args, "CL");
    size_t digits = 0;
    while (hex && std::isxdigit(static_cast<unsigned char>(hex[digits]))) digits++;
    if (digits == 6 || digits == 8) { sink.appendf(",\"col\":[\"%.*s\"]", static_cast<int>(digits), hex); return; }
    const int r = queryNumber(args, "R"), g = queryNumber(args, "G"), b = queryNumber(args, "B");
    if (r == kNoValue && g == kNoValue && b == kNoValue) return;
    sink.appendf(",\"col\":[[%d,%d,%d]]", channelOr(r, current.r), channelOr(g, current.g), channelOr(b, current.b));
}

}  // namespace


// WLED-compatibility `/json/info`: the field set the WLED apps and HA validate after `_wled._tcp` discovery, with `brand:"WLED"` to interoperate and `product:"MoonModules"` to say what this is.
void HttpServerModule::serveWledInfo(platform::TcpConnection& conn) {
    JsonSink sink;
    writeWledInfoBody(sink);
    answerJson(conn, sink);
}

// /presets.json: look presets in WLED's format, keyed by 1-based slot (python-wled discards 0) with a `{"0":{}}` floor; Drivers and Layouts presets stay out since they rewire pins or geometry.
void HttpServerModule::serveWledPresets(platform::TcpConnection& conn) {
    auto* control = static_cast<ControlModule*>(findModuleByName("Control"));
    if (!control) { sendResponse(conn, 200, "application/json", "{\"0\":{}}"); return; }

    JsonSink sink;
    sink.append("{");
    bool any = false;
    for (uint8_t i = 0; i < control->presetCount(); i++) {
        if (!control->isLookOnly(i)) continue;
        const char* name = control->presetName(i);
        if (!name || !name[0]) continue;
        // Slot+1: WLED slot 0 is reserved, and python-wled drops it.
        sink.appendf("%s\"%u\":{\"n\":", any ? "," : "", static_cast<unsigned>(i + 1));
        sink.writeJsonString(name);
        sink.append("}");
        any = true;
    }
    if (!any) sink.append("\"0\":{}");   // non-empty, or python-wled reads it as no response at all
    sink.append("}");
    answerJson(conn, sink);
}

// The deviceName / IP / MAC lookup the WLED shim needs at four call sites, so a change to how identity is discovered updates one place.
void HttpServerModule::resolveWledIdentity(const char*& name, uint8_t mac[6], uint8_t* ip,
                                           const char* nameFallback) {
    name = nameFallback;
    if (MoonModule* sys = findModuleByName("System")) {
        const char* dn = static_cast<SystemModule*>(sys)->deviceName();
        if (dn && dn[0]) name = dn;
    }
    for (int i = 0; i < 6; i++) mac[i] = 0;
    platform::getMacAddress(mac);
    if (!ip) return;
    for (int i = 0; i < 4; i++) ip[i] = 0;
    platform::localIPv4(ip);
}

// The 💫 marker prefixes only the WLED-compat name HA reads, never the real deviceName.
void HttpServerModule::writeWledName(JsonSink& sink, const char* name) {
    char prefixed[80];
    std::snprintf(prefixed, sizeof(prefixed), "\xF0\x9F\x92\xAB %s", name ? name : "");
    sink.writeJsonString(prefixed);
}

// The WLED info object (no HTTP header), shared by /json, /json/info, /json/si and the WebSocket push. @xref{why-one-info-object-serves-every-route} @xref{why-the-wled-shim-reports-a-sentinel-version}
void HttpServerModule::writeWledInfoBody(JsonSink& sink) {
    const char* name; uint8_t mac[6]; uint8_t ip[4];
    resolveWledIdentity(name, mac, ip);
    const int rssi = platform::wifiStaRssi();
    uint8_t bssid[6] = {};
    platform::wifiStaBssid(bssid);
    int signal = (rssi == 0) ? 0 : (2 * (rssi + 100));
    if (signal < 0) signal = 0; else if (signal > 100) signal = 100;
    // The real pipeline shape and render rate.
    const LightSummary& ls = lightSummary();
    const LightOutput* out = LightOutput::active();
    const unsigned renderFps = scheduler_ ? scheduler_->fps() : 0;
    const char* rgbw = (ls.channelsPerLight >= 4) ? "true" : "false";
    sink.appendf("{\"ver\":\"99.0.0\",\"vid\":2410150,\"name\":");
    writeWledName(sink, name);
    sink.appendf(",\"mac\":\"%02x%02x%02x%02x%02x%02x\","
                 "\"ip\":\"%u.%u.%u.%u\",\"arch\":\"esp32\","
                 "\"brand\":\"WLED\",\"product\":\"MoonModules\",\"release\":\"MoonModules\","
                 // lc is a capability code, not a count: 1 (RGB_COLOR) gives HA a color picker, and a LED count would leave the light entity without color modes.
                 "\"leds\":{\"count\":%u,\"fps\":%u,\"rgbw\":%s,\"wv\":false,\"cct\":false,"
                 "\"maxpwr\":0,\"maxseg\":%d,\"pwr\":0,\"lc\":1},",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 ip[0], ip[1], ip[2], ip[3],
                 ls.lightCount, renderFps, rgbw, segmentCount(scheduler_));
    sink.appendf("\"wifi\":{\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                 "\"rssi\":%d,\"channel\":%d,\"signal\":%d},",
                 bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                 rssi, platform::wifiStaChannel(), signal);
    sink.appendf("\"fs\":{\"t\":256,\"u\":32,\"pmt\":%u},"
                 // uptime + pmt drive python-wled's presets change-detect: both non-zero and stable stop HA refetching /presets.json on every state update.
                 "\"freeheap\":%u,\"uptime\":%u,\"udpport\":21324,\"live\":false,"
                 // ws=-1 tells python-wled WebSocket updates are unsupported, so HA polls over HTTP instead of opening /ws, whose MoonLight-native frames flood its log with `MissingField: filesystem`.
                 "\"lm\":\"\",\"lip\":\"\",\"ws\":-1,"
                 // fxcount and palcount are the real counts, matching effects[] and palettes[]; cpal/umpal are 0.
                 "\"fxcount\":%d,\"palcount\":%u,\"cpalcount\":0,\"umpalcount\":0,\"str\":false}",
                 // Non-zero or HA rejects the device: the host's freeHeap() reports 0, so a nominal figure stands in.
                 presetsStamp(static_cast<ControlModule*>(findModuleByName("Control"))),
                 static_cast<unsigned>(platform::freeHeap() ? platform::freeHeap() : 32768u),
                 static_cast<unsigned>(platform::millis() / 1000u),
                 effectTypeCount(),
                 static_cast<unsigned>(out ? out->paletteCount() : 0));
}

// The WLED state object: `on`+`bri` mirror Drivers, and each `seg[i]` is a layer, whose `col[0]` is the active palette's full-value color, stable across consumers and never double-dimmed with HA's `state.bri × seg.bri`.
void HttpServerModule::writeWledStateBody(JsonSink& sink) {
    const uint8_t bri = driversBrightness(scheduler_);
    const LightOutput* out = LightOutput::active();
    const RGB pc = out ? out->paletteRgb(driversPalette(scheduler_)) : RGB{0, 0, 0};
    // python-wled parses this response through State.from_dict, so nl/udpn/lor/transition/ps/pl/mainseg are required, and each segment's .on must be present since HA reads segment .on, not top-level state.on.
    const char* onStr = driversOn(scheduler_) ? "true" : "false";
    // seg[i].pal is the active palette index, sharing the Drivers `palette` control with col[0], so HA's palette dropdown and color picker are two views of one value.
    const uint8_t pal = driversPalette(scheduler_);
    const int currentPs = currentPresetSlot(static_cast<ControlModule*>(findModuleByName("Control")));
    sink.appendf("{\"on\":%s,\"bri\":%u,\"transition\":7,\"ps\":%d,\"pl\":-1,"
                 "\"nl\":{},\"udpn\":{},\"lor\":0,\"mainseg\":0,\"seg\":[",
                 onStr, bri, currentPs);
    // fx pairs with pal as python-wled expects, else the light entity stays unavailable.
    const int segments = segmentCount(scheduler_);
    for (int i = 0; i < segments; i++) {
        if (i) sink.append(",");
        writeSegment(sink, scheduler_, layerAt(scheduler_, i), i, pal, pc);
    }
    sink.append("]}");
}

void HttpServerModule::serveWledState(platform::TcpConnection& conn) {
    JsonSink sink;
    writeWledStateBody(sink);
    answerJson(conn, sink);
}

// /json: the full blob HA's python-wled fetches once, then polls /json/si, so state and info come from the writers /json/si shares.
void HttpServerModule::serveWledDeviceJson(platform::TcpConnection& conn) {
    JsonSink sink;
    sink.appendf("{\"state\":");
    writeWledStateBody(sink);
    sink.appendf(",\"info\":");
    writeWledInfoBody(sink);
    // effects is every registered effect type, indexed to match a segment's fx; palettes is the real list, indexed to match seg.pal and the Drivers `palette` control.
    sink.appendf(",\"effects\":[");
    writeEffectNames(sink);
    sink.appendf("],\"palettes\":[");
    writePaletteNames(sink);
    sink.appendf("]}");
    answerJson(conn, sink);
}

// The combined {state, info} object, which /json/si answers and the WebSocket pushes.
void HttpServerModule::writeWledStateInfo(JsonSink& sink) {
    sink.appendf("{\"state\":");
    writeWledStateBody(sink);
    sink.appendf(",\"info\":");
    writeWledInfoBody(sink);
    sink.appendf("}");
}

// /json/si: the combined {state, info} the WLED app reads in one call for its card.
void HttpServerModule::serveWledStateInfo(platform::TcpConnection& conn) {
    JsonSink sink;
    writeWledStateInfo(sink);
    answerJson(conn, sink);
}

// Apply a WLED state-set body from POST /json/state and the inbound WebSocket: the device's preset, power and brightness, then each segment. `on` is master power, so toggling off keeps the brightness level.
void HttpServerModule::applyWledState(const char* body) {
    JsonDoc doc;
    if (!json::parse(body, doc)) return;
    const JsonNode* root = doc.rootNode();
    if (!root || root->type != JsonType::Object) return;
    int v;
    if (intField(doc, root, "ps", 1, 250, v)) applyWledPreset(v);
    if (switchField(doc, root, "on", true, v)) setFlag(*this, "Drivers", "on", v == kToggle ? !driversOn(scheduler_) : v != 0);
    if (intField(doc, root, "bri", 0, 255, v)) setNumber(*this, "Drivers", "brightness", v);
    applySegments(*this, scheduler_, doc, json::member(doc, root, "seg"));
}

// ps is a preset slot from HA's dropdown (1-based, as /presets.json); applyLookByName re-checks look-only, so a crafted request naming a Drivers or Layouts preset is refused here, not hidden.
void HttpServerModule::applyWledPreset(int slot) {
    auto* control = static_cast<ControlModule*>(findModuleByName("Control"));
    if (!control || slot > control->presetCount()) return;
    if (const char* name = control->presetName(static_cast<uint8_t>(slot - 1))) control->applyLookByName(name);
}

// POST /json/state: the WLED app's HTTP control channel; apply, then echo the resulting state, which the app expects.
void HttpServerModule::handleWledState(platform::TcpConnection& conn, const char* body) {
    applyWledState(body);
    serveWledState(conn);
}

// /json/effects: the effect names, in the order a segment's fx indexes.
void HttpServerModule::serveWledEffects(platform::TcpConnection& conn) {
    JsonSink sink;
    sink.append("[");
    writeEffectNames(sink);
    sink.append("]");
    answerJson(conn, sink);
}

// /json/palettes: the palette names, in the order a segment's pal indexes.
void HttpServerModule::serveWledPalettes(platform::TcpConnection& conn) {
    JsonSink sink;
    sink.append("[");
    writePaletteNames(sink);
    sink.append("]");
    answerJson(conn, sink);
}

// /json/fxdata: what each effect's sliders and options are called.
void HttpServerModule::serveWledEffectData(platform::TcpConnection& conn) {
    JsonSink sink;
    sink.append("[");
    writeEffectData(sink);
    sink.append("]");
    answerJson(conn, sink);
}

// POST /json: the same apply, then WLED's own answer: `{"success":true}`, or the whole device object when the body carries `"v":true`.
void HttpServerModule::handleWledJson(platform::TcpConnection& conn, const char* body) {
    applyWledState(body);
    if (mm::json::parseBool(body, "v")) serveWledDeviceJson(conn);
    else sendResponse(conn, 200, "application/json", "{\"success\":true}");
}

// GET /win: `/win&A=128&FX=5` or `/win?A=128&FX=5`. The keys are written as the JSON body they stand for, and applied as one; SS names the layer the segment keys address, the first by default.
void HttpServerModule::handleWledWin(platform::TcpConnection& conn, const char* args) {
    const LightOutput* out = LightOutput::active();
    const int layer = queryNumber(args, "SS");
    JsonSink body;
    body.append("{\"v\":false");
    appendWinPower(body, args);
    appendWinKeys(body, args, kWinDeviceKeys);
    body.appendf(",\"seg\":[{\"id\":%d", layer == kNoValue ? 0 : layer);
    appendWinKeys(body, args, kWinSegmentKeys);
    appendWinColor(body, args, out ? out->paletteRgb(driversPalette(scheduler_)) : RGB{0, 0, 0});
    body.append("}]}");
    applyWledState(body.data());
    serveWledState(conn);
}

// Build and push the WLED {state, info} object to every WS client, the same object /json/si answers.
void HttpServerModule::pushWledStateToWebSockets() {
    if (anyStreamActive()) return;   // never interleave with a streamed full state
    bool hasClients = false;
    for (int i = 0; i < kMaxWsClients; i++) if (wsClients_[i].valid() && !subs_[i].received) { hasClients = true; break; }
    if (!hasClients) return;   // only the web interface is connected: the frame is built for nobody

    JsonSink sink;
    writeWledStateInfo(sink);

    for (int i = 0; i < kMaxWsClients; i++) {
        auto& ws = wsClients_[i];
        // A client that subscribed is the web interface, which reads module state rather than this frame.
        if (!ws.valid() || subs_[i].received) continue;
        queueFrame(i, 0x81, sink.data(), sink.size());
    }
}

} // namespace mm
