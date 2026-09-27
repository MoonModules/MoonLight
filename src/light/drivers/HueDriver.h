#pragma once

#include "light/drivers/DriverBase.h"

#include "core/util/JsonUtil.h"          // parse the bridge's JSON responses
#include "core/system/FilesystemModule.h"  // noteDirty: persist the app key after pairing
#include "core/system/DevicesModule.h"     // DevicesModule::active(): list the bridge as a device
#include "platform/platform.h"

namespace mm {

/// Output driver: sends the buffer to Philips Hue bulbs as pixels, so a small grid runs any effect. It reads its window of the shared buffer and pushes each light's color to the bridge, the same shape as NetworkSendDriver, over the Hue v1 HTTP API rather than UDP.
///
/// It is HTTP rather than a wire protocol, so the rate is bounded by connection churn. Each PUT opens a fresh connection. That gives smooth ambient color, not real time.
///
/// Prior art: the Hue v1 CLIP API public documentation; the effect-as-output mapping is our own.
///
/// @moreinfo
///
/// ## The wire contract
///
/// Plain HTTP, no TLS, bench-confirmed on a BSB002 bridge:
///
/// ```text
///     POST /api                          pair, once the link button is pressed
///     GET  /api/<key>/lights             the lights, keyed by bridge id
///     GET  /api/<key>/groups             the rooms, each listing its lights
///     PUT  /api/<key>/lights/<id>/state  on, brightness, hue, saturation, transition
/// ```
///
/// ## Which lights are driven
///
/// Only color-capable, reachable lights, narrowed further by the room and light selectors. The shared output Correction applies as it does on the LED drivers, so brightness reaches the Hue lights too.
///
/// @card HueDriver.png
class HueDriver : public DriverBase {
public:
    /// Default to the RGB preset: this driver reads the output back as RGB to make an HSV.
    HueDriver() { setDefaultPresetName("RGB"); }

    /// The bridge's LAN IP, entered in the UI (4 octets).
    uint8_t  bridgeIp[4] = {};
    /// The Hue username/app key: filled by the Pair button, then persisted.
    char     appKey[48] = {};

    /// Hue converts to HSV, RGB-fixed, so there is no correction UI to show.
    bool hasCorrectionControls() const override { return false; }

    /// Bind the bridge address, the pairing button, the room and light filters, and the window.
    void defineDriverControls() override {
        controls_.addIPv4("bridgeIp", bridgeIp);
        controls_.addText("appKey", appKey, sizeof(appKey));   // persisted credential
        controls_.addButton("pair");                            // link-button pairing
        // Rebuilt in place into stable member buffers, so they reflect the current room.
        buildRoomOptions();
        buildLightOptions();
        controls_.addSelect("room", room_, roomOptions_, roomOptionCount_);
        controls_.addSelect("light", light_, lightOptions_, lightOptionCount_);
        addWindowControls();                                    // start / count: its slice of the buffer
        // One status line carries the pairing state and the driven-of-total light count.
        refreshStatus();
    }

    /// Take the shared source buffer this driver reads its window from.
    void setSourceBuffer(Buffer* buf) override { sourceBuffer_ = buf; }

    /// React to a control change: start pairing, re-point the bridge, or re-derive the subset.
    void onControlChanged(const char* controlName) override {
        if (controlName && std::strcmp(controlName, "pair") == 0) {
            pairTicksLeft_ = kPairWindowTicks;   // begin: poll the bridge for ~30 s on tick1s
            std::snprintf(statusBuf_, sizeof(statusBuf_), "pairing: press the bridge button");
            setStatus(statusBuf_);
        } else if (controlName &&
                   (std::strcmp(controlName, "bridgeIp") == 0 || std::strcmp(controlName, "appKey") == 0)) {
            resetLightCache();   // re-fetch the light list + groups for the new bridge/key
        } else if (controlName && std::strcmp(controlName, "room") == 0) {
            // The old light index may point past the new, shorter list, so clamp it back to All.
            if (light_ >= lightOptionCount_) light_ = 0;
            rebuildDriven();
            refreshStatus();
        } else if (controlName && std::strcmp(controlName, "light") == 0) {
            rebuildDriven();     // a different single light (or back to room's all)
            refreshStatus();     // the driven-of-total count changed → refresh the status line
        }
        DriverBase::onControlChanged(controlName);
    }

    // A synchronous round trip every tick would stall the single-threaded render loop.
    /// Do at most one bounded PUT, and only once the rate-limit interval has elapsed.
    void tick() MM_NONBLOCKING override {
        if (pairTicksLeft_ > 0) return;            // pairing owns the bridge during its window
        if (!appKey[0] || !haveBridge() || lightCount_ == 0) return;
        const uint32_t now = platform::millis();
        if (now - lastPutMs_ < kPutIntervalMs) return;   // not time yet: return instantly, no I/O
        lastPutMs_ = now;
        pushOneChangedLight();                     // exactly one bounded PUT this tick
    }

    /// Run the slower bridge work: the pairing poll, the light fetch, and the announce.
    void tick1s() MM_NONBLOCKING override {
        if (pairTicksLeft_ > 0) { pollPairing(); DriverBase::tick1s(); return; }
        if (!appKey[0] || !haveBridge()) { DriverBase::tick1s(); return; }
        if (!sawLights_) { fetchLights(); DriverBase::tick1s(); return; }
        if (!sawGroups_) { fetchGroups(); DriverBase::tick1s(); return; }
        if (++reportTick_ >= kReportEverySec) { reportTick_ = 0; reportBridge(); }
        DriverBase::tick1s();
    }

    /// Stop any in-flight pairing and release the dropdown-name heap, then chain to the base.
    void release() override {
        pairTicksLeft_ = 0;
        freeNameBuffers();   // release the dropdown-name heap; a re-add re-fetches and re-allocs
        DriverBase::release();
    }

    /// Test seam: whether a light's RGB would be sent, and the body it would send.
    bool wouldPushForTest(uint8_t idx, uint8_t r, uint8_t g, uint8_t b, char* outBody, size_t cap) {
        if (!diffAndFormat(idx, r, g, b, outBody, cap)) return false;
        if (idx < kMaxLights) {
            lastRgb_[idx][0] = r; lastRgb_[idx][1] = g; lastRgb_[idx][2] = b;
            sent_[idx] = true;
        }
        return true;
    }

    /// Test seam: parse a real /lights JSON body through fetchLights' color-light extractor.
    void parseLightsForTest(const char* json) { parseLights(json); rebuildDriven(); }
    /// Count of kept color+reachable lights.
    uint8_t lightCountForTest() const { return lightCount_; }
    /// The bridge light id at window index `i`, or 0 when out of range.
    uint16_t hueIdForTest(uint8_t i) const { return i < kMaxLights ? hueId_[i] : 0; }
    /// The same count as the read-only control / bridge field; 0 before any fetch.
    int8_t colorCountForTest() const { return colorCount_; }

    // Parse the lights FIRST: room membership resolves against the known color lights.
    /// Test seam: parse a real groups body through the room extractor.
    void parseGroupsForTest(const char* json) { parseGroups(json); rebuildDriven(); }
    /// Count of kept Rooms (bridge groups with type=="Room").
    uint8_t roomCountForTest() const { return roomCount_; }

    /// Select a room and re-derive the driven subset, as the dropdown does.
    void setRoomForTest(uint8_t r) { room_ = r; if (light_ >= lightOptionCount_) light_ = 0; rebuildDriven(); refreshStatus(); }
    /// Select light `l` within the chosen room and re-derive the driven subset.
    void setLightForTest(uint8_t l) { light_ = l; rebuildDriven(); refreshStatus(); }
    /// Recompute the status line without waiting for a tick.
    void refreshStatusForTest() { refreshStatus(); }
    /// How many lights survive the room+light filter: the set pushOneChangedLight walks.
    uint8_t drivenCountForTest() const { return drivenLightCount_; }
    /// The bridge light id at filtered index `i`, or 0 when out of range.
    uint16_t drivenIdForTest(uint8_t i) const { return i < drivenLightCount_ ? hueId_[drivenIdx_[i]] : 0; }

    /// Test seam for the RGB→HSV mapping (no bridge needed).
    static void rgbToHsvForTest(uint8_t r, uint8_t g, uint8_t b, uint16_t& h, uint8_t& s, uint8_t& v) {
        rgbToHsv(r, g, b, h, s, v);
    }

    /// Test seam: the truncation signal fetchLights grows against (a complete /lights body ends '}').
    static bool bodyLooksCompleteForTest(const char* body) { return bodyLooksComplete(body); }

private:
    static constexpr uint8_t kMaxLights = 32;        // a LAN's worth of Hue bulbs; bounded, no heap
    static constexpr uint8_t kMaxRooms  = 16;        // bounded room count; option index 0 is "All"
    static constexpr uint8_t kNameLen   = 24;        // per-light / per-room friendly-name buffer
    // A room's membership fits one bitmask, which is the textbook small-set membership.
    static_assert(kMaxLights == 32, "Room membership bitmask (roomMask_) assumes 32 color lights");
    // Bounded by connection CHURN, not the command budget: faster piles up TIME_WAIT sockets.
    static constexpr uint32_t kPutIntervalMs = 500;
    static constexpr int     kPairWindowTicks = 30;  // ~30 s pairing window (link-button press)
    static constexpr uint16_t kReportEverySec = 30;  // re-announce the bridge to DevicesModule
    // Bounds the WORST case only: a real PUT to a LAN bridge returns in tens of milliseconds.
    static constexpr uint32_t kHttpTimeoutMs = 200;
    static constexpr uint32_t kSlowTimeoutMs = 400;

    /// The shared layer buffer this driver reads its window from; null until setSourceBuffer.
    Buffer* sourceBuffer_ = nullptr;

    /// Window index to bridge light id, holding only the color-capable lights.
    uint16_t hueId_[kMaxLights] = {};
    /// The last RGB pushed per light: what the changed-only filter compares against.
    uint8_t  lastRgb_[kMaxLights][3] = {};
    /// Whether this light has been pushed at least once (the first send is never "unchanged").
    bool     sent_[kMaxLights] = {};
    /// How many color-capable lights survived the filter: the length of hueId_.
    uint8_t  lightCount_ = 0;
    /// The same count as the read-only control / bridge field.
    int8_t   colorCount_ = 0;
    /// fetchLights has run, so the list is trustworthy.
    bool     sawLights_ = false;
    // Heap, not inline, so an unconfigured driver pays nothing and the object stays small.
    /// Friendly light names for the dropdown, allocated lazily on the first parse.
    char*    lightNames_ = nullptr;
    /// Friendly room names, same shape: `kMaxRooms × kNameLen`, indexed by roomNameAt().
    char*    roomNames_  = nullptr;
    /// Pointer to light `i`'s name inside the lightNames_ block, or null before it is allocated.
    char* lightNameAt(uint8_t i) { return lightNames_ ? lightNames_ + static_cast<size_t>(i) * kNameLen : nullptr; }
    /// Pointer to room `i`'s name inside the roomNames_ block, or null before it is allocated.
    char* roomNameAt(uint8_t i)  { return roomNames_  ? roomNames_  + static_cast<size_t>(i) * kNameLen : nullptr; }
    /// Allocate the two name blocks lazily, so an unconfigured driver pays nothing.
    void ensureNameBuffers() {
        if (!lightNames_) lightNames_ = static_cast<char*>(platform::alloc(static_cast<size_t>(kMaxLights) * kNameLen));
        if (!roomNames_)  roomNames_  = static_cast<char*>(platform::alloc(static_cast<size_t>(kMaxRooms)  * kNameLen));
    }
    /// Release both name blocks and null the pointers: a re-add re-fetches and re-allocates.
    void freeNameBuffers() {
        platform::free(lightNames_); lightNames_ = nullptr;
        platform::free(roomNames_);  roomNames_  = nullptr;
    }

    /// Room membership, one bitmask per room over the known color lights.
    uint32_t roomMask_[kMaxRooms] = {};
    /// Number of Rooms kept.
    uint8_t  roomCount_ = 0;
    /// fetchGroups has run, so the room list is trustworthy.
    bool     sawGroups_ = false;

    /// Room filter (a Select index, persisted as uint8): 0 = "All", else roomName_[room_-1].
    uint8_t  room_ = 0;
    /// Light filter: 0 = "All", else the n-th light of the current option list.
    uint8_t  light_ = 0;
    /// Color-light array indices driven, after the room+light filter.
    uint8_t  drivenIdx_[kMaxLights] = {};
    /// Size of drivenIdx_: the set pushOneChangedLight walks.
    uint8_t  drivenLightCount_ = 0;

    /// Stable option pointers for the room selector, which borrows rather than copies them.
    const char* roomOptions_[kMaxRooms + 1] = {};
    /// How many entries of roomOptions_ are live.
    uint8_t     roomOptionCount_ = 1;
    /// The same borrowed-pointer arrangement for the light Select, refilled by buildLightOptions.
    const char* lightOptions_[kMaxLights + 1] = {};
    /// How many entries of lightOptions_ are live.
    uint8_t     lightOptionCount_ = 1;

    /// Round-robin position across the lights: one bounded PUT per tick, not a whole sweep.
    uint8_t  pushCursor_ = 0;
    /// Lights driven this pass (n), the basis for the fade time.
    uint8_t  drivenCount_ = 0;
    /// millis() of the last PUT: the rate gate tick() checks before doing any work.
    uint32_t lastPutMs_ = 0;
    /// Remaining 1 Hz ticks to keep polling for the link-button press; 0 = not pairing.
    int      pairTicksLeft_ = 0;
    /// Counts tick1s ticks toward kReportEverySec, for the periodic DevicesModule announce.
    uint16_t reportTick_ = 0;
    /// The status line shown on the driver's card; refreshStatus() rewrites it in place.
    char     statusBuf_[40] = "unpaired";
    // Grow-and-retry from a small first try, and heap-allocated: an inline member would overflow.
    static constexpr size_t kLightsBufInitial = 2048;
    static constexpr size_t kLightsBufMax     = 16384;

    /// True once a bridge IP has been set: any non-zero octet counts.
    bool haveBridge() const { return bridgeIp[0] || bridgeIp[1] || bridgeIp[2] || bridgeIp[3]; }

    /// Whether the JSON span contains a key, used to read a light's capabilities.
    static bool containsKey(const char* begin, const char* end, const char* key) {
        const size_t kl = std::strlen(key);
        for (const char* s = begin; s + kl <= end; s++)
            if (std::strncmp(s, key, kl) == 0) return true;
        return false;
    }

    // Span-bounded, so one light's name cannot match the first name elsewhere in the response.
    /// Read a string value from within one JSON object span.
    static void parseStringIn(const char* begin, const char* end, const char* key, char* out, size_t cap) {
        if (cap == 0) return;
        out[0] = 0;
        char search[24];
        std::snprintf(search, sizeof(search), "\"%s\":\"", key);
        const size_t sl = std::strlen(search);
        for (const char* s = begin; s + sl <= end; s++) {
            if (std::strncmp(s, search, sl) != 0) continue;
            const char* v = s + sl;
            size_t oi = 0;
            for (; v < end && *v != '"' && oi + 1 < cap; v++) out[oi++] = *v;
            out[oi] = 0;
            return;
        }
    }

    /// Format bridgeIp as a dotted quad into `out`, for the HTTP host argument.
    void bridgeStr(char out[16]) const {
        std::snprintf(out, 16, "%u.%u.%u.%u", bridgeIp[0], bridgeIp[1], bridgeIp[2], bridgeIp[3]);
    }

    /// Rebuild the status line: the pairing state, and the light count as driven of total.
    void refreshStatus() {
        if (!appKey[0]) std::snprintf(statusBuf_, sizeof(statusBuf_), "unpaired");
        else if (!lightCount_) std::snprintf(statusBuf_, sizeof(statusBuf_), "paired");
        else if (drivenLightCount_ < lightCount_)
            std::snprintf(statusBuf_, sizeof(statusBuf_), "paired, %u-%u lights", drivenLightCount_, lightCount_);
        else std::snprintf(statusBuf_, sizeof(statusBuf_), "paired, %u lights", lightCount_);
        setStatus(statusBuf_);
    }

    /// One pairing attempt, keeping the app key if the link button has been pressed.
    void pollPairing() {
        if (!haveBridge()) { pairTicksLeft_ = 0; std::snprintf(statusBuf_, sizeof(statusBuf_), "set bridge IP first"); setStatus(statusBuf_); return; }
        char host[16]; bridgeStr(host);
        // Headers and body share one buffer, and the bridge's headers alone run ~700 bytes.
        char resp[1024];
        int st = platform::httpRequest("POST", host, 80, "/api",
                                       "{\"devicetype\":\"MoonLight#device\"}", kSlowTimeoutMs,
                                       resp, sizeof(resp));
        if (st == 200 && std::strstr(resp, "\"username\"")) {
            // [{"success":{"username":"<key>"}}]: extract the username.
            char key[48] = {};
            mm::json::parseString(resp, "username", key, sizeof(key));
            if (key[0]) {
                std::snprintf(appKey, sizeof(appKey), "%s", key);
                pairTicksLeft_ = 0;
                resetLightCache();                // clear the light list + the per-light push cache
                                                  // (sent_/lastRgb_) so the new session re-sends all
                refreshStatus();
                markDirty();                      // persist the new app key
                FilesystemModule::noteDirty();
                return;
            }
        }
        // "link button not pressed" → keep polling until the window elapses.
        if (--pairTicksLeft_ <= 0) {
            std::snprintf(statusBuf_, sizeof(statusBuf_), "pairing timed out");
            setStatus(statusBuf_);
        }
    }

    /// Drop the learned light list + room list + push cache so tick1s re-fetches (bridge/key change).
    void resetLightCache() {
        lightCount_ = 0;
        colorCount_ = 0;
        sawLights_ = false;
        roomCount_ = 0;
        sawGroups_ = false;
        pushCursor_ = 0;
        for (uint8_t i = 0; i < kMaxLights; i++) sent_[i] = false;
        freeNameBuffers();   // drop the old bridge's names; the re-fetch re-allocs for the new one
        // Rebuilt NOW, or the option arrays dangle into the name buffers freed above.
        buildRoomOptions();
        buildLightOptions();
        rebuildDriven();   // empty caches → empty driven set, until the re-fetch repopulates them
    }

    /// Whether a response looks whole rather than cut off, which the fetch grows against.
    static bool bodyLooksComplete(const char* body) {
        size_t len = std::strlen(body);
        while (len > 0 && (body[len - 1] == '\n' || body[len - 1] == '\r'
                           || body[len - 1] == ' ' || body[len - 1] == '\t')) len--;
        return len > 0 && body[len - 1] == '}';
    }

    // The range bound is the point: an oversized id would narrow into a DIFFERENT valid light.
    /// The bridge id a quoted JSON key opens with, or 0 when it is not an id.
    static uint16_t parseId(const char* s) {
        // Digits only: accepting a sign would let a malformed response address a REAL light.
        if (!s || *s < '0' || *s > '9') return 0;
        const int v = json::parseIntStr(s);
        return (v > 0 && v <= 0xFFFF) ? static_cast<uint16_t>(v) : 0;
    }

    /// --- Learn the bridge's light ids (window index → hue id, in id order).
    void fetchLights() {
        char host[16]; bridgeStr(host);
        char path[80]; std::snprintf(path, sizeof(path), "/api/%s/lights", appKey);
        // Sized dynamically: a truncated read would silently drop the trailing lights.
        for (size_t cap = kLightsBufInitial; cap <= kLightsBufMax; cap *= 2) {
            char* buf = static_cast<char*>(platform::alloc(cap));
            if (!buf) return;
            const int st = platform::httpRequest("GET", host, 80, path, "", kSlowTimeoutMs, buf, cap);
            if (st != 200) { platform::free(buf); return; }
            // Detected by the body's SHAPE, not its length: the headers are stripped in place.
            const bool truncated = !bodyLooksComplete(buf) && (cap < kLightsBufMax);
            if (!truncated) {
                parseLights(buf);
                rebuildControls();   // the light-dropdown options changed → re-bind for the UI
                refreshStatus();
                reportBridge();
                platform::free(buf);
                return;
            }
            platform::free(buf);
        }
    }

    /// List the bridge alongside the discovered peers, through the devices seam.
    void reportBridge() {
        auto* dev = DevicesModule::active();
        if (!dev || !haveBridge()) return;
        char host[16]; bridgeStr(host);
        // Sized for the headers too, which alone run ~700 bytes, or the body is squeezed out.
        char cfg[1024], name[24] = {};
        if (platform::httpRequest("GET", host, 80, "/api/0/config", "", kSlowTimeoutMs, cfg, sizeof(cfg)) == 200)
            mm::json::parseString(cfg, "name", name, sizeof(name));   // the bridge's friendly name
        dev->upsertHueBridge(bridgeIp, name, static_cast<uint8_t>(colorCount_));
    }

    // A forward scan, not the recursive reader: the response exceeds its node arena.
    /// Extract the color-capable, reachable light ids from a lights body.
    void parseLights(const char* resp) {
        ensureNameBuffers();
        lightCount_ = 0;
        const char* p = resp;
        int pendingId = 0;               // a light id seen, not yet committed (need its span first)
        const char* pendingStart = nullptr;
        auto commit = [&](const char* objEnd) {
            if (pendingId > 0 && pendingStart && lightCount_ < kMaxLights && lightNames_
                && containsKey(pendingStart, objEnd, "\"hue\"")
                && containsKey(pendingStart, objEnd, "\"reachable\":true")) {
                hueId_[lightCount_] = static_cast<uint16_t>(pendingId);
                // Read from this light's own object span, falling back to the id if absent.
                char* name = lightNameAt(lightCount_);
                parseStringIn(pendingStart, objEnd, "name", name, kNameLen);
                if (!name[0]) std::snprintf(name, kNameLen, "%d", pendingId);
                lightCount_++;
            }
        };
        while (true) {
            const char* q = std::strchr(p, '"');           // next key open-quote
            if (!q) break;
            int id = parseId(q + 1);                        // light id is a quoted integer key
            const char* close = std::strchr(q + 1, '"');
            // A top-level light-id key: a quoted positive integer followed by ':'.
            if (id > 0 && close && close[1] == ':') {
                commit(q);                                  // the PREVIOUS light's object ends here
                pendingId = id;
                pendingStart = close + 1;
            }
            p = close ? close + 1 : q + 1;
        }
        commit(resp + std::strlen(resp));                   // the last light runs to the end
        sawLights_ = true;
        colorCount_ = static_cast<int8_t>(lightCount_ > 127 ? 127 : lightCount_);
        rebuildDriven();   // the color-light set changed → re-derive the filtered driven subset
    }

    /// Learn the bridge's rooms, growing the read buffer until the response parses whole.
    void fetchGroups() {
        char host[16]; bridgeStr(host);
        char path[80]; std::snprintf(path, sizeof(path), "/api/%s/groups", appKey);
        for (size_t cap = kLightsBufInitial; cap <= kLightsBufMax; cap *= 2) {
            char* buf = static_cast<char*>(platform::alloc(cap));
            if (!buf) return;
            const int st = platform::httpRequest("GET", host, 80, path, "", kSlowTimeoutMs, buf, cap);
            if (st != 200) { platform::free(buf); return; }
            const bool truncated = !bodyLooksComplete(buf) && (cap < kLightsBufMax);
            if (!truncated) {
                parseGroups(buf);
                rebuildControls();   // the room dropdown options changed → re-bind for the UI
                platform::free(buf);
                return;
            }
            platform::free(buf);
        }
    }

    // Rooms only: zones, light groups and entertainment areas are dropped.
    /// Extract the rooms from a groups body, each with its name and member lights.
    void parseGroups(const char* resp) {
        ensureNameBuffers();
        roomCount_ = 0;
        const char* p = resp;
        int pendingId = 0;
        const char* pendingStart = nullptr;
        auto commit = [&](const char* objEnd) {
            if (pendingId > 0 && pendingStart && roomCount_ < kMaxRooms && roomNames_
                && containsKey(pendingStart, objEnd, "\"type\":\"Room\"")) {
                char* name = roomNameAt(roomCount_);
                parseStringIn(pendingStart, objEnd, "name", name, kNameLen);
                if (!name[0]) std::snprintf(name, kNameLen, "%d", pendingId);
                roomMask_[roomCount_] = roomMaskFor(pendingStart, objEnd);
                roomCount_++;
            }
        };
        while (true) {
            const char* q = std::strchr(p, '"');
            if (!q) break;
            int id = parseId(q + 1);
            const char* close = std::strchr(q + 1, '"');
            if (id > 0 && close && close[1] == ':') {
                commit(q);                                  // the PREVIOUS group's object ends here
                pendingId = id;
                pendingStart = close + 1;
            }
            p = close ? close + 1 : q + 1;
        }
        commit(resp + std::strlen(resp));                   // the last group runs to the end
        sawGroups_ = true;
    }

    // Bounded to this room's own array, so a later zone's list cannot bleed in.
    /// Resolve a room's light array into a membership bitmask over the color lights.
    uint32_t roomMaskFor(const char* begin, const char* end) const {
        const char* s = begin;
        const size_t kl = std::strlen("\"lights\":[");
        for (; s + kl <= end; s++) if (std::strncmp(s, "\"lights\":[", kl) == 0) { s += kl; break; }
        uint32_t mask = 0;
        for (const char* q = s; q < end && *q != ']'; ) {
            if (*q == '"') {
                const int id = parseId(q + 1);
                for (uint8_t i = 0; i < lightCount_; i++)        // map the id to its color-light bit
                    if (hueId_[i] == id) { mask |= (1u << i); break; }
                const char* c = std::strchr(q + 1, '"');         // skip to the value's closing quote
                if (!c || c >= end) break;
                q = c + 1;
            } else q++;
        }
        return mask;
    }

    // One source of truth for both the dropdown and the driven set, so they cannot disagree.
    /// The color-light indices the current room selection exposes, and how many.
    uint8_t roomColorLights(uint8_t (&out)[kMaxLights]) const {
        uint8_t n = 0;
        if (room_ == 0 || room_ > roomCount_) {              // "All" (or a stale index) → every color light
            for (uint8_t i = 0; i < lightCount_ && n < kMaxLights; i++) out[n++] = i;
            return n;
        }
        const uint32_t mask = roomMask_[room_ - 1];
        for (uint8_t i = 0; i < lightCount_ && n < kMaxLights; i++)   // keep color lights in this Room's bitmask
            if (mask & (1u << i)) out[n++] = i;
        return n;
    }

    /// Rebuild the room dropdown options: {"All", room0, room1, …}, pointing into roomName_.
    void buildRoomOptions() {
        roomOptions_[0] = "All";
        uint8_t n = 1;
        for (uint8_t i = 0; i < roomCount_ && n <= kMaxRooms; i++) roomOptions_[n++] = roomNameAt(i);
        roomOptionCount_ = n;
    }

    /// Rebuild the light dropdown from the current room's color lights.
    void buildLightOptions() {
        lightOptions_[0] = "All";
        uint8_t idx[kMaxLights];
        const uint8_t m = roomColorLights(idx);
        uint8_t n = 1;
        for (uint8_t i = 0; i < m && n <= kMaxLights; i++) lightOptions_[n++] = lightNameAt(idx[i]);
        lightOptionCount_ = n;
    }

    /// Derive the driven subset from the current room and light filters.
    void rebuildDriven() {
        drivenLightCount_ = 0;
        uint8_t idx[kMaxLights];
        const uint8_t m = roomColorLights(idx);
        if (light_ == 0 || light_ > m) {                     // "All" within the (possibly room-narrowed) set
            for (uint8_t i = 0; i < m; i++) drivenIdx_[drivenLightCount_++] = idx[i];
        } else {                                             // a single light: the (light_-1)-th listed one
            drivenIdx_[drivenLightCount_++] = idx[light_ - 1];
        }
        if (pushCursor_ >= drivenLightCount_) pushCursor_ = 0;
    }

    // At most one lap per call, so an all-unchanged frame costs no PUT and returns fast.
    /// Push at most one changed light, round-robining the cursor across successive calls.
    void pushOneChangedLight() {
        if (!sourceBuffer_ || !sourceBuffer_->data()) return;
        nrOfLightsType winStart, winLen;
        windowSlice(sourceBuffer_->count(), winStart, winLen);
        const uint8_t cpl = sourceBuffer_->channelsPerLight();
        if (cpl < 3) return;
        const uint8_t* base = sourceBuffer_->data();
        // Walks the FILTERED set, which is every color light only while nothing is picked.
        const uint8_t n = drivenLightCount_ < winLen ? drivenLightCount_ : static_cast<uint8_t>(winLen);
        if (n == 0) return;
        drivenCount_ = n;   // the round-robin size: drives the Hue fade time (transitionDeciseconds)

        for (uint8_t step = 0; step < n; step++) {
            const uint8_t i = (pushCursor_ + step) % n;        // position within the driven window
            const uint8_t li = drivenIdx_[i];                  // the color-light array index it maps to
            const uint8_t* px = base + static_cast<size_t>(winStart + i) * cpl;
            // Corrected in place only while the buffer provably holds the whole light.
            uint8_t rgb[4] = { px[0], px[1], px[2], 0 };
            if (correction_.outChannels <= sizeof(rgb)) correction_.apply(px, rgb, cpl);
            char body[80];
            if (diffAndFormat(li, rgb[0], rgb[1], rgb[2], body, sizeof(body))) {
                char host[16]; bridgeStr(host);
                char path[96];
                std::snprintf(path, sizeof(path), "/api/%s/lights/%u/state", appKey, hueId_[li]);
                const int st = platform::httpRequest("PUT", host, 80, path, body, kHttpTimeoutMs, nullptr, 0);
                // Marked sent only on success, so the next lap retries a failed light.
                if (st == 200) {
                    lastRgb_[li][0] = rgb[0]; lastRgb_[li][1] = rgb[1]; lastRgb_[li][2] = rgb[2];
                    sent_[li] = true;
                }
                pushCursor_ = static_cast<uint8_t>((i + 1) % n);   // resume after this one next time
                return;                                            // ONE PUT attempt: done
            }
        }
        // No light changed this lap: nothing to send. Cursor stays put.
    }

    /// Whether light `idx` changed since its last push, and the state body to send.
    bool diffAndFormat(uint8_t idx, uint8_t r, uint8_t g, uint8_t b, char* out, size_t cap) {
        if (idx >= kMaxLights) return false;
        if (sent_[idx] && lastRgb_[idx][0] == r && lastRgb_[idx][1] == g && lastRgb_[idx][2] == b)
            return false;   // unchanged: skip
        const uint8_t tt = transitionDeciseconds();
        if ((r | g | b) == 0) { std::snprintf(out, cap, "{\"on\":false,\"transitiontime\":%u}", tt); return true; }
        uint16_t hue; uint8_t sat, val;
        rgbToHsv(r, g, b, hue, sat, val);
        std::snprintf(out, cap, "{\"on\":true,\"bri\":%u,\"hue\":%u,\"sat\":%u,\"transitiontime\":%u}",
                      val, hue, sat, tt);
        return true;
    }

    /// The fade time, matched to how often this light's turn comes round.
    uint8_t transitionDeciseconds() const {
        // The count driven this pass: a partial window refreshes each of its lights sooner.
        const uint8_t driven = drivenCount_ ? drivenCount_ : (lightCount_ ? lightCount_ : 1);
        const uint32_t intervalMs = static_cast<uint32_t>(driven) * kPutIntervalMs;
        const uint32_t ds = intervalMs / 100;
        return static_cast<uint8_t>(ds < 1 ? 1 : (ds > 30 ? 30 : ds));
    }

    /// Textbook RGB to HSV in integer maths, mapped to the bridge's own ranges.
    static void rgbToHsv(uint8_t r, uint8_t g, uint8_t b, uint16_t& hueOut, uint8_t& satOut, uint8_t& valOut) {
        const uint8_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        const uint8_t mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        const uint8_t chroma = mx - mn;
        valOut = static_cast<uint8_t>(mx > 254 ? 254 : mx);                 // value → bri
        satOut = mx == 0 ? 0 : static_cast<uint8_t>((chroma * 254u) / mx);  // saturation
        if (chroma == 0) { hueOut = 0; return; }                           // grey → hue irrelevant
        // Hue in sixths of the wheel, scaled to 0..65535. h6 is the position within [0,6).
        int32_t h6;  // numerator over chroma, in units where a full sixth = chroma*... see below
        if (mx == r)      h6 = ((g - b) * 65535) / (6 * chroma) + (g < b ? 65535 : 0);
        else if (mx == g) h6 = ((b - r) * 65535) / (6 * chroma) + 65535 / 3;
        else              h6 = ((r - g) * 65535) / (6 * chroma) + (65535 * 2) / 3;
        if (h6 < 0) h6 += 65535;
        hueOut = static_cast<uint16_t>(h6 % 65536);
    }
};

}  // namespace mm
