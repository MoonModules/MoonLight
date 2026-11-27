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
/// python-wled reads `/json` once, then polls `/json/si`, and merges each reply into the first, so `/json/si` needs every field `/json` carries: a reply that omits `fs` fails the update and HA marks the device unavailable.
///
/// The WLED-Android app requires `leds`, `wifi`, `name` and `mac` in the same object, and real WLED always sends `wifi`.
///
/// One writer therefore emits the full `info` for `/json`, `/json/info`, `/json/si` and the WebSocket push, and an Ethernet device reports `wifi` with a zero signal.

#include "core/system/HttpServerModule.h"

#include "core/module/Scheduler.h"
#include "core/util/JsonUtil.h"
#include "core/util/JsonSink.h"
#include "core/util/format.h"            // formatTo: nonblocking formatting into a fixed buffer
#include "core/system/ControlModule.h"   // look presets on /presets.json (HA WLED integration)
#include "core/system/SystemModule.h"      // deviceName() for the WLED /json/info shim
#include "core/util/LightOutput.h"   // the device shape and the palettes as colors, for the WLED shim
#include "platform/platform_clock.h"   // millis
#include "platform/platform_heap.h"   // freeHeap
#include "platform/platform_system.h"   // getMacAddress
#include "platform/platform_net.h"   // TcpConnection, localIPv4, wifiStaRssi

#include <cstdio>
#include <cstring>
#include <cstdint>

namespace mm {

namespace {

using json::JsonDoc;
using json::JsonNode;
using json::JsonType;

constexpr int kToggle = 2;   ///< a flag's "t": the opposite of what it is now

// The integer member `key` of `obj` held to [lo, hi]; true only for an integer of `lo` or more, so a negative number is no value and a string such as "~10", WLED's relative step, is left alone.
bool intField(const JsonDoc& doc, const JsonNode* obj, const char* key, int lo, int hi, int& out) {
    const JsonNode* n = json::member(doc, obj, key);
    if (!n || n->type != JsonType::Int || n->intValue < lo) return false;
    out = n->intValue > hi ? hi : static_cast<int>(n->intValue);
    return true;
}

// A flag member as 0 or 1, or kToggle for the string "t"; false when it is absent or of another kind.
bool switchField(const JsonDoc& doc, const JsonNode* obj, const char* key, int& out) {
    const JsonNode* n = json::member(doc, obj, key);
    if (!n) return false;
    if (n->type == JsonType::String && n->str && n->str[0] == 't') { out = kToggle; return true; }
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

// A Drivers control set to a number or a flag, through the apply path every client shares.
void setNumber(HttpServerModule& http, const char* control, int value) {
    char valueJson[24];
    std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%d}", value);
    http.applySetControl("Drivers", control, valueJson);
}

void setFlag(HttpServerModule& http, const char* control, bool value) {
    http.applySetControl("Drivers", control, value ? "{\"value\":true}" : "{\"value\":false}");
}

// Power and brightness from `obj`, the body or its segment: `on` is master power, so toggling off keeps the brightness level.
void applyPower(HttpServerModule& http, const JsonDoc& doc, const JsonNode* obj, bool on) {
    int v;
    if (switchField(doc, obj, "on", v)) setFlag(http, "on", v == kToggle ? !on : v != 0);
    if (intField(doc, obj, "bri", 0, 255, v)) setNumber(http, "brightness", v);
}

// A segment's pal, or else its col mapped to the nearest palette; HA's brightness slider sends `bri`, so the color's value channel is ignored.
void applyPalette(HttpServerModule& http, const JsonDoc& doc, const JsonNode* seg) {
    const LightOutput* out = LightOutput::active();
    if (!out || out->paletteCount() == 0) return;
    int pal;
    uint8_t rgb[3];
    // Clamped to the full count, built-ins plus the scripted tail, the list served as `palettes[]`, so a scripted index the device offered is not snapped back.
    if (intField(doc, seg, "pal", 0, out->paletteCount() - 1, pal)) setNumber(http, "palette", pal);
    else if (colorField(doc, seg, rgb)) setNumber(http, "palette", out->nearestPalette(RGB{rgb[0], rgb[1], rgb[2]}));
}

// The device's one segment, segment 0: an object, or the array element whose id is 0, or the first element when it names no id.
const JsonNode* mainSegment(const JsonDoc& doc, const JsonNode* seg) {
    if (!seg || seg->type == JsonType::Object) return seg;
    for (int i = 0; i < json::arraySize(doc, seg); i++) {
        const JsonNode* element = json::element(doc, seg, i);
        if (!element || element->type != JsonType::Object) continue;
        int id;
        if ((intField(doc, element, "id", 0, 255, id) ? id : i) == 0) return element;
    }
    return nullptr;
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

// The 💫 marker prefixes the WLED-compat name HA reads, and no other name.
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
    // lightCount is a uint32_t, which is unsigned long on the boards and so needs the cast for %u.
    sink.appendf(",\"mac\":\"%02x%02x%02x%02x%02x%02x\","
                 "\"ip\":\"%u.%u.%u.%u\",\"arch\":\"esp32\","
                 "\"brand\":\"WLED\",\"product\":\"MoonModules\",\"release\":\"MoonModules\","
                 // lc is a capability code: 1 (RGB_COLOR) gives HA a color picker, and a LED count would drop the light entity's color modes.
                 "\"leds\":{\"count\":%u,\"fps\":%u,\"rgbw\":%s,\"wv\":false,\"cct\":false,"
                 "\"maxpwr\":0,\"maxseg\":1,\"pwr\":0,\"lc\":1},",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 ip[0], ip[1], ip[2], ip[3],
                 static_cast<unsigned>(ls.lightCount), renderFps, rgbw);
    sink.appendf("\"wifi\":{\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                 "\"rssi\":%d,\"channel\":%d,\"signal\":%d},",
                 bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                 rssi, platform::wifiStaChannel(), signal);
    // pmt is the presets-modified time HA uses to re-fetch /presets.json, stamped by ControlModule on every preset change, with 1 as the no-ControlModule fallback.
    unsigned pmt = 1;
    if (auto* control = static_cast<ControlModule*>(findModuleByName("Control")))
        pmt = static_cast<unsigned>(control->presetsRevision());   // >= 1 once setup's rescan ran
    if (pmt == 0) pmt = 1;   // python-wled treats 0 as "no presets support"
    sink.appendf("\"fs\":{\"t\":256,\"u\":32,\"pmt\":%u},"
                 // uptime + pmt drive python-wled's presets change-detect: both non-zero and stable stop HA refetching /presets.json on every state update.
                 "\"freeheap\":%u,\"uptime\":%u,\"udpport\":21324,\"live\":false,"
                 // ws=-1 tells python-wled WebSocket updates are unsupported, so HA polls over HTTP instead of opening /ws, whose MoonLight-native frames flood its log with `MissingField: filesystem`.
                 "\"lm\":\"\",\"lip\":\"\",\"ws\":-1,"
                 // palcount is the real count, built-ins plus the scripted tail, matching palettes[] below; fxcount stays 1 and cpal/umpal are 0.
                 "\"fxcount\":1,\"palcount\":%u,\"cpalcount\":0,\"umpalcount\":0,\"str\":false}",
                 // Non-zero or HA rejects the device: the host's freeHeap() reports 0, so a nominal figure stands in.
                 pmt,
                 static_cast<unsigned>(platform::freeHeap() ? platform::freeHeap() : 32768u),
                 static_cast<unsigned>(platform::millis() / 1000u),
                 static_cast<unsigned>(out ? out->paletteCount() : 0));
}

// The WLED state object: `on`+`bri` mirror Drivers, and `seg[0].col[0]` is the active palette's full-value color, stable across consumers and never double-dimmed with HA's `state.bri × seg.bri`.
void HttpServerModule::writeWledStateBody(JsonSink& sink) {
    const uint8_t bri = driversBrightness(scheduler_);
    const LightOutput* out = LightOutput::active();
    const RGB pc = out ? out->paletteRgb(driversPalette(scheduler_)) : RGB{0, 0, 0};
    // python-wled parses this response through State.from_dict, so nl/udpn/lor/transition/ps/pl/mainseg are required, and seg[0].on must be present since HA reads segment .on, not top-level state.on.
    const char* onStr = driversOn(scheduler_) ? "true" : "false";
    // seg[0].pal is the active palette index, sharing the Drivers `palette` control with col[0], so HA's palette dropdown and color picker are two views of one value.
    const uint8_t pal = driversPalette(scheduler_);
    // The applied look as a WLED preset slot (1-based, as /presets.json), or -1; without it HA's preset dropdown reads unknown even while a preset is active.
    int currentPs = -1;
    if (auto* control = static_cast<ControlModule*>(findModuleByName("Control"))) {
        const char* look = control->currentLook();
        if (look && look[0]) {
            for (uint8_t i = 0; i < control->presetCount(); i++) {
                if (!control->isLookOnly(i)) continue;
                const char* n = control->presetName(i);
                if (n && std::strcmp(n, look) == 0) { currentPs = i + 1; break; }
            }
        }
    }
    sink.appendf("{\"on\":%s,\"bri\":%u,\"transition\":7,\"ps\":%d,\"pl\":-1,"
                 "\"nl\":{},\"udpn\":{},\"lor\":0,\"mainseg\":0,"
                 // seg[0].bri = 255 so HA's segment.bri × state.bri / 255 shows the master value; fx=0 ("Solid") pairs with pal as python-wled expects, else the light entity stays unavailable.
                 "\"seg\":[{\"id\":0,\"on\":%s,\"bri\":255,\"lc\":1,\"fx\":0,\"pal\":%u,\"col\":[[%u,%u,%u]]}]}",
                 onStr, bri, currentPs, onStr, pal, pc.r, pc.g, pc.b);
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
    // effects stays one real entry ("Solid") since this shim drives a single Layer; palettes is the real list, indexed to match seg[0].pal and the Drivers `palette` control.
    const LightOutput* out = LightOutput::active();
    sink.appendf(",\"effects\":[\"Solid\"],\"palettes\":[");
    if (out) out->writePaletteNames(sink);
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

// A WLED state body from POST /json/state or the WebSocket, parsed as JSON so a segment's key stays the segment's: preset, power, brightness, then segment 0, the device itself.
void HttpServerModule::applyWledState(const char* body) {
    JsonDoc doc;
    if (!body || !json::parse(body, doc)) return;
    const JsonNode* root = doc.rootNode();
    if (!root || root->type != JsonType::Object) return;
    int slot;
    // ps is a preset slot from HA's dropdown (1-based, as /presets.json); applyLookByName re-checks look-only, so a crafted request naming a Drivers or Layouts preset is refused here, not hidden.
    auto* control = static_cast<ControlModule*>(findModuleByName("Control"));
    // A slot past the last preset names no preset, so it applies none, as WLED does: presetName answers null there, so the slot is read up to 255 rather than held to the last.
    if (control && intField(doc, root, "ps", 1, 255, slot))
        if (const char* name = control->presetName(static_cast<uint8_t>(slot - 1))) control->applyLookByName(name);
    const auto driversOn = [this] { const MoonModule* d = findModuleByName("Drivers"); return d && d->readBool("on", true); };
    applyPower(*this, doc, root, driversOn());
    if (const JsonNode* seg = mainSegment(doc, json::member(doc, root, "seg"))) {
        applyPower(*this, doc, seg, driversOn());
        applyPalette(*this, doc, seg);
    }
}

// POST /json/state: the WLED app's HTTP control channel; apply, then echo the resulting state, which the app expects.
void HttpServerModule::handleWledState(platform::TcpConnection& conn, const char* body) {
    applyWledState(body);
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
