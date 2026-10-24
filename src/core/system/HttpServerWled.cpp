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
/// ## Why an Ethernet device sends no wifi object
///
/// The `wifi` block feeds HA's diagnostic sensors, and an Ethernet device has no AP, so `info.wifi` (optional in python-wled) is omitted.
///
/// Omitting it makes HA create no WiFi sensors, as a real WLED on Ethernet does, instead of greyed-out zero rows.

#include "core/system/HttpServerModule.h"

#include "core/module/Scheduler.h"
#include "core/util/JsonUtil.h"
#include "core/util/parse.h"             // parseIntStr: the app's numeric fields
#include "core/util/JsonSink.h"
#include "core/util/format.h"            // formatTo: nonblocking formatting into a fixed buffer
#include "core/system/ControlModule.h"   // look presets on /presets.json (HA WLED integration)
#include "core/system/SystemModule.h"      // deviceName() for the WLED /json/info shim
#include "core/util/LightOutput.h"   // the device shape and the palettes as colors, for the WLED shim
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <cstdint>

namespace mm {


// WLED-compatibility `/json/info`: the field set the WLED apps and HA validate after `_wled._tcp` discovery, with `brand:"WLED"` to interoperate and `product:"MoonModules"` to say what this is.
void HttpServerModule::serveWledInfo(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    // Identity: the deviceName (from SystemModule), the live IP, the MAC.
    const char* name; uint8_t mac[6];
    resolveWledIdentity(name, mac, nullptr);

    // The WLED-Android app requires only `name`, `leds{}`, `wifi{}` and a non-empty `mac` (model/wledapi/Info.kt), else it drops the device silently, so these are the smallest shapes.
    JsonSink sink(conn);
    writeWledInfoBody(sink, name, mac);
    sink.flush();
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
    sendResponse(conn, 200, "application/json", sink.data());
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

// The WLED info object (no HTTP header), shared by /json/info and /json/si; the 💫 marker prefixes only the WLED-compat name HA reads, never the real deviceName.
void HttpServerModule::writeWledName(JsonSink& sink, const char* name) {
    char prefixed[80];
    std::snprintf(prefixed, sizeof(prefixed), "\xF0\x9F\x92\xAB %s", name ? name : "");
    sink.writeJsonString(prefixed);
}

void HttpServerModule::writeWledInfoBody(JsonSink& sink, const char* name, const uint8_t mac[6]) {
    sink.appendf("{\"name\":");
    writeWledName(sink, name);
    // The real light count and the wifi signal, so the WLED app card shows the true device shape; signal maps rssi to 0-100 as WLED does.
    const unsigned ledCount = lightSummary().lightCount;
    const int rssi = platform::wifiStaRssi();
    int signal = (rssi == 0) ? 0 : (2 * (rssi + 100));
    if (signal < 0) signal = 0; else if (signal > 100) signal = 100;
    sink.appendf(",\"mac\":\"%02x%02x%02x%02x%02x%02x\","
                 "\"leds\":{\"count\":%u},\"wifi\":{\"rssi\":%d,\"signal\":%d},"
                 "\"brand\":\"WLED\",\"product\":\"MoonModules\"}",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 ledCount, rssi, signal);
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
                 "\"seg\":[{\"id\":0,\"on\":%s,\"bri\":255,\"fx\":0,\"pal\":%u,\"col\":[[%u,%u,%u]]}]}",
                 onStr, bri, currentPs, onStr, pal, pc.r, pc.g, pc.b);
}

void HttpServerModule::serveWledState(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));
    JsonSink sink(conn);
    writeWledStateBody(sink);
    sink.flush();
}

// /json: the full blob HA's python-wled fetches, requiring `info.fs`, `state.nl`, `state.udpn`, `state.lor` and `ver` >= 0.14.0; separate from /json/info and /json/state, which stay the Android-validated minimum.
void HttpServerModule::serveWledDeviceJson(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    const char* name; uint8_t mac[6]; uint8_t ip[4];
    resolveWledIdentity(name, mac, ip);

    JsonSink sink(conn);
    // state: reuse writeWledStateBody under "state" so one writer decides which seg[0] fields HA reads.
    sink.appendf("{\"state\":");
    writeWledStateBody(sink);
    // `ver` is a sentinel and an Ethernet device omits `wifi` entirely. @xref{why-the-wled-shim-reports-a-sentinel-version} @xref{why-an-ethernet-device-sends-no-wifi-object}
    const bool onEth = platform::ethConnected();
    const int rssi = platform::wifiStaRssi();
    uint8_t bssid[6] = {};
    platform::wifiStaBssid(bssid);
    const int channel = platform::wifiStaChannel();
    int signal = (rssi == 0) ? 0 : (2 * (rssi + 100));
    if (signal < 0) signal = 0; else if (signal > 100) signal = 100;
    // The real pipeline shape and render rate.
    const LightSummary& ls = lightSummary();
    const LightOutput* out = LightOutput::active();
    const unsigned ledCount = ls.lightCount;
    const unsigned renderFps = scheduler_ ? scheduler_->fps() : 0;
    const char* rgbw = (ls.channelsPerLight >= 4) ? "true" : "false";
    sink.appendf(",\"info\":{\"ver\":\"99.0.0\",\"vid\":2410150,\"name\":");
    writeWledName(sink, name);
    sink.appendf(",\"mac\":\"%02x%02x%02x%02x%02x%02x\","
                 "\"ip\":\"%u.%u.%u.%u\",\"arch\":\"esp32\","
                 "\"brand\":\"WLED\",\"product\":\"MoonModules\",\"release\":\"MoonModules\","
                 // lc and seglc are capability codes, not counts: 1 (RGB_COLOR) gives HA a brightness slider and color picker, and a LED count would leave the light entity without color modes.
                 "\"leds\":{\"count\":%u,\"fps\":%u,\"rgbw\":%s,\"wv\":false,\"cct\":false,"
                 "\"maxpwr\":0,\"maxseg\":1,\"pwr\":0,\"lc\":1,\"seglc\":[1]},",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 ip[0], ip[1], ip[2], ip[3],
                 ledCount, renderFps, rgbw);
    // wifi: only for a Wi-Fi device (omitted on Ethernet; see the comment above the getters).
    if (!onEth) {
        sink.appendf("\"wifi\":{\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
                     "\"rssi\":%d,\"channel\":%d,\"signal\":%d},",
                     bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
                     rssi, channel, signal);
    }
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
                 // Non-zero or HA rejects the device: the desktop's freeHeap() reports 0, so a nominal figure stands in.
                 pmt,
                 static_cast<unsigned>(platform::freeHeap() ? platform::freeHeap() : 32768u),
                 static_cast<unsigned>(platform::millis() / 1000u),
                 static_cast<unsigned>(out ? out->paletteCount() : 0));
    // effects stays one real entry ("Solid") since this shim drives a single Layer; palettes is the real list, indexed to match seg[0].pal and the Drivers `palette` control.
    sink.appendf(",\"effects\":[\"Solid\"],\"palettes\":[");
    if (out) out->writePaletteNames(sink);
    sink.appendf("]}");
    sink.flush();
}

// /json/si: the combined {state, info} the WLED app reads in one call for its card.
void HttpServerModule::serveWledStateInfo(platform::TcpConnection& conn) {
    const char* header =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Connection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n";
    conn.write(reinterpret_cast<const uint8_t*>(header), std::strlen(header));

    const char* name; uint8_t mac[6];
    resolveWledIdentity(name, mac, nullptr);

    JsonSink sink(conn);
    sink.appendf("{\"state\":");
    writeWledStateBody(sink);
    sink.appendf(",\"info\":");
    writeWledInfoBody(sink, name, mac);
    sink.appendf("}");
    sink.flush();
}

// Apply a WLED state-set body ({on?, bri?}) to Drivers through the apply-core, from POST /json/state and the inbound WebSocket; `on` is master power, so toggling off keeps the brightness level.
void HttpServerModule::applyWledState(const char* body) {
    // ps is a preset slot from HA's dropdown (1-based, as /presets.json); applyLookByName re-checks look-only, so a crafted request naming a Drivers or Layouts preset is refused here, not hidden.
    if (mm::json::hasKey(body, "ps")) {
        const int slot = mm::json::parseInt(body, "ps");
        auto* control = static_cast<ControlModule*>(findModuleByName("Control"));
        if (control && slot > 0 && slot <= control->presetCount()) {
            const char* name = control->presetName(static_cast<uint8_t>(slot - 1));
            if (name) control->applyLookByName(name);
        }
    }
    if (mm::json::hasKey(body, "on")) {
        applySetControl("Drivers", "on",
                        mm::json::parseBool(body, "on") ? "{\"value\":true}" : "{\"value\":false}");
    }
    if (mm::json::hasKey(body, "bri")) {
        int bri = mm::json::parseInt(body, "bri");
        if (bri < 0) bri = 0;
        if (bri > 255) bri = 255;
        char valueJson[32];
        std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%d}", bri);
        applySetControl("Drivers", "brightness", valueJson);
    }
    // seg[0].pal is the palette index HA writes from its dropdown, mapped to the Drivers `palette` control and parsed from the segment object so a top-level "pal" cannot hijack it.
    const LightOutput* out = LightOutput::active();
    const char* segStart = std::strstr(body, "\"seg\":");
    const char* palStart = segStart ? std::strstr(segStart, "\"pal\":") : nullptr;
    if (palStart && out) {
        int pal = mm::parseIntStr(palStart + 6);
        if (pal < 0) pal = 0;
        // Clamp to the full count, built-ins plus the scripted tail, the list served as `palettes[]`, so a scripted index the device offered is not snapped back.
        if (pal >= out->paletteCount()) pal = out->paletteCount() - 1;
        char valueJson[24];
        std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%d}", pal);
        applySetControl("Drivers", "palette", valueJson);
    }
    // seg[0].col[0] is [r,g,b] from HA's color picker, mapped to the nearest palette; the value channel is ignored since HA's brightness slider sends `bri`.
    const char* colStart = std::strstr(body, "\"col\":[[");
    if (colStart && out) {
        int r = 0, g = 0, b = 0;
        if (std::sscanf(colStart + 8, "%d,%d,%d", &r, &g, &b) == 3) {
            const uint8_t rc = static_cast<uint8_t>(r < 0 ? 0 : (r > 255 ? 255 : r));
            const uint8_t gc = static_cast<uint8_t>(g < 0 ? 0 : (g > 255 ? 255 : g));
            const uint8_t bc = static_cast<uint8_t>(b < 0 ? 0 : (b > 255 ? 255 : b));
            const uint8_t idx = out->nearestPalette(RGB{rc, gc, bc});
            char valueJson[24];
            std::snprintf(valueJson, sizeof(valueJson), "{\"value\":%u}", static_cast<unsigned>(idx));
            applySetControl("Drivers", "palette", valueJson);
        }
    }
}

// POST /json/state: the WLED app's HTTP control channel; apply, then echo the resulting state, which the app expects.
void HttpServerModule::handleWledState(platform::TcpConnection& conn, const char* body) {
    applyWledState(body);
    serveWledState(conn);
}

// Build and push the WLED {state, info} object to every WS client. Shares the same body writers as /json/si.
void HttpServerModule::pushWledStateToWebSockets() {
    if (anyStreamActive()) return;   // never interleave with a streamed full state
    bool hasClients = false;
    for (int i = 0; i < kMaxWsClients; i++) if (wsClients_[i].valid() && !subs_[i].received) { hasClients = true; break; }
    if (!hasClients) return;   // only the web interface is connected: the frame is built for nobody

    const char* name; uint8_t mac[6];
    resolveWledIdentity(name, mac, nullptr);

    JsonSink sink;
    sink.appendf("{\"state\":");
    writeWledStateBody(sink);
    sink.appendf(",\"info\":");
    writeWledInfoBody(sink, name, mac);
    sink.appendf("}");

    for (int i = 0; i < kMaxWsClients; i++) {
        auto& ws = wsClients_[i];
        // A client that subscribed is the web interface, which reads module state rather than this frame.
        if (!ws.valid() || subs_[i].received) continue;
        queueFrame(i, 0x81, sink.data(), sink.size());
    }
}

} // namespace mm
