/// Drives the MQTT client: the socket lifecycle, the topic tree, Home Assistant discovery and the inbound command routing.
///
/// @moreinfo
///
/// ## The topic identity is the MAC
///
/// The prefix is derived live from a stable hardware id, `MoonLight/<last6-of-MAC>` in lowercase hex, so nothing is stored in a buffer.
/// The MAC is fixed for the chip's life, so a rename never changes the topics and Homebridge or Home Assistant stay pinned.
/// This is the WLED, Tasmota and HA-discovery convention, and the last 6 hex is the same short id WLED uses in `wled/<last6>`.
/// The friendly display name is a separate concern, published retained on the `name` topic and read from `deviceName()`.
/// That retained topic is the human-facing label a hub reads for its accessory title, the WLED `serverDescription` role, published on connect and on a rename.
/// SystemModule guarantees the name non-empty and per-device unique, falling back to `MM-<last4MAC>` rather than a shared literal, so N devices never collide as one name.
/// A missing `systemModule_` is a wiring bug, so the publish is skipped rather than inventing a non-unique fallback.
/// A rename is change-detected by a rolling djb2 signature, so no name buffer is stored.
/// The clientId is `MoonLight-<last6-of-MAC>`, slash-free because MQTT-3.1.3-5 allows only `[0-9a-zA-Z]` and a broker may reject a `/`.
///
/// ## The discovery topics
///
/// The config topic is independent of `topicPrefix()`: the discovery prefix is HA's `homeassistant`, not our `MoonLight` root.
/// The object id carries the `MoonLight_` prefix so the id is unique across vendors on a shared broker.
/// The update entity uses the same object id, so both components register under one HA device card: one device, two entities.
/// Diverging the id would produce a second device card reading as "two MoonLights", which is the wrong grouping.
///
/// ## The discovery payload
///
/// The light is a JSON-schema MQTT light with HA's documented abbreviated keys. Brightness sits at the default 0-255 scale, so no scale key is needed, and `dev{}` groups the entity under a device card.
/// The entity's own `name` is null, HA's documented convention for "this entity IS the device, no sub-label", so the slug is `light.<device>` rather than the doubled `light.<device>_<device>`.
/// Rename the device and `dev.name` follows on the next publish, `uniq_id` stays MAC-pinned, and the slug locked at creation is unchanged.
/// Documented at https://www.home-assistant.io/integrations/mqtt/#name, where `name: null` is the recommended way.
/// The `deviceName` is user-editable, so a quote or backslash would produce invalid JSON: the shared `jsonEscape` doubles the 32-char name worst case.
/// The update entity's `device_class:"firmware"` makes HA render the label as `<device> Firmware` rather than the bare device name. The bare name collided visually with the light in HA's entity list, which the "two MM-P4 rows" bench symptom pinned.
/// It also picks the correct icon and the up-to-date versus update-available wording, and `entity_category:"diagnostic"` parks it in the diagnostic section of the device card, matching ESPHome and Tasmota.
///
/// ## The effect list is looks only
///
/// The `effect_list` is the light's menu of LOOKS, and only look-only presets appear (`ControlModule::isLookOnly`).
/// One that also carries Drivers or Layouts would rewire pins or geometry. That must not be reachable from an automation or a voice command that believes it is choosing a color scheme.
/// `applyLookByName` re-checks look-only, so a crafted message naming a hardware-carrying preset is refused at the entry point rather than merely hidden from the list.
/// The applied look rides the published state too, so HA's dropdown shows the look that is on, including a change made from the device's own pad grid.
/// It is therefore part of the change gate: applying a preset moves neither on, brightness nor palette, so without its signature a look-only change would never publish.
///
/// ## The discovery scratch buffers
///
/// Both buffers are allocated from the heap the first time discovery publishes and reported via `setDynamicBytes`, so the UI's per-module memory line accounts for them.
/// A device that never enables discovery never calls this, so it costs zero bytes; OOM returns false and the caller skips the publish rather than dereferencing a null.
/// They are sized to what THIS device publishes: the fixed config plus the measured effect list.
/// No cap on the preset count, because a cap would either reserve RAM a three-preset device never uses, or silently publish nothing once the list outgrew it.
/// Saving or deleting a preset changes the required size, so they are reallocated when the revision moves rather than held at whatever the first announce needed.
/// The light and update announces share the pair (448 + 320 bytes) because they run serially and are never in flight simultaneously.
/// The update announce needs them at all because its ~300-byte payload plus framing exceeds the on-stack `kSendBufLen`.
/// The effect list is built into `discoveryBuf_` as scratch, untouched until `buildMqttPublish` by which time the list is already consumed into the payload.
/// It must NOT be a region of `discoveryPayload_`. Snprintf writing the payload while reading the list from inside its own destination tramples the list once the fixed prefix grows past the scratch offset.
///
/// ## Availability and retract
///
/// The Last Will is declared at CONNECT: retained "offline" on `<prefix>/status`, which the broker publishes on an ungraceful drop so HA's `avty_t` greys the entity out.
/// The retained "online" counterpart is published by the module itself once CONNACK lands, and must precede the discovery announce so HA sees the entity available the instant its config arrives.
/// A retract is an empty retained payload to the config topic, which removes the HA entity. It frames into a small LOCAL buffer, because a tombstone is topic plus empty body.
/// That local buffer matters because `discoveryBuf_` may already be freed, and the OFF path must not allocate under the "no memory when discovery is off" rule.
/// Disconnected there is nothing to send, so the retract is deferred to the next CONNACK, where the broker keeps the last retained config until then; freeing always runs either way.
/// CONNACK retracts when `haDiscovery_` is false, because a config retained from a session that turned discovery off while offline would otherwise keep HA's entity alive across the reconnect.
/// Both components are announced or retracted together by one gate, so HA sees both or neither, never a device card with a light beside a dangling stale update entity.
///
/// ## The update entity
///
/// `installed_version` is the compile-time `MM_VERSION`, and `latest_version` equals it today since there is no on-device release check yet, so HA shows up-to-date and disables Install.
/// When the release-check component lands it becomes the caller of `publishUpdateState` with a fresher `latest_version`, and the wire shape does not change.
/// `release_url` is the GitHub releases page, which HA renders as a "Release notes" link. It is retained and read by a person, so a card left behind by a rename is a dead link.
/// The state block is written once at CONNACK and on a mid-session discovery turn-on: nothing is per-tick, because both versions are compile-time constants.
/// The install payload is the target version string via HA's `payload_install_template`, whose default `{{ latest_version }}` renders empty when no newer version is known.
/// An empty payload therefore means "install latest", falling back to this build's own version so the command re-installs the current release rather than silently doing nothing.
/// A leading `v` is stripped because the template can send either shape, and the URL is built from `kReleaseAssetUrlFormat` naming both repositories so a rename cannot strand it.
/// It hands off to `platform::http_fetch_to_ota`, the same OTA path `POST /api/firmware/url` takes, and `otaInFlight()` guards a second install mid-flash rather than corrupting the running task.
/// On desktop that platform call is a stub returning false, so the install safely reports failure via `g_otaStatus`, and HA polls the retained state rather than any reply.
///
/// ## Sending never blocks the render loop
///
/// `sendPacket` uses non-blocking `writeSome`: a control packet is at most 256 bytes, far under the socket send buffer, so a healthy socket accepts it all in one call.
/// A partial or zero write means the buffer is backing up (a zero-window or stalled broker), so it returns false and the caller resets rather than spin-retrying forever inside `tick1s`.
/// That is the hot-path violation a blocking `write()` would commit, and every path (ping, subscribe, state, discovery) shares the same "reset on a failed send" contract.
/// A partial write means a wedged socket, so dropping the connection beats leaving a truncated frame on the stream.
/// `resetConnection` is the single reset path, so every caller (reconfig, disable, timeout, protocol error, peer close) leaves the same clean Idle state.
/// State is stamped only on a SUCCESSFUL send, so `last*` and `havePublished_` are committed after every send succeeded. A failure then republishes after the reconnect, rather than leaving the hub stale forever.
/// The name signature follows the same rule, so a failed name publish is retried on the next tick rather than lost until the next reconnect.
/// A test seam mirrors the outbound bytes into a capture buffer, null in production, because ctest has no live socket and `writeSome` returns -1 there.
/// Capture mode always reports success so the connect and publish flow proceeds in tests.
///
/// ## Reconnect, keepalive and protocol errors
///
/// Connecting is non-blocking throughout: `connectStart` kicks off getaddrinfo and connect and returns, and `tick1s` polls it in `ConnectingTcp` so an unreachable broker never stalls the render loop.
/// Failure is assumed until a full connect succeeds, and cleared only in the CONNACK-accepted path. Every failure route resets to Idle without clearing it, so the next attempt uses the longer backoff.
/// Backoff keeps a down broker from being hammered every tick. A prior failure, an unreachable broker or a bad hostname costing a synchronous getaddrinfo each try, backs off harder to keep that DNS stall rare.
/// A broker that accepts TCP but never CONNACKs, the silent-broker wedge, is bounded by the same connect timeout.
/// PINGREQ goes out at half the keepalive, and a broker silent past 1.5 times the keepalive is dropped.
/// A malformed or oversize packet desyncs the byte stream for the connection's life, and MQTT 3.1.1 section 4.8 requires closing on a protocol violation. So it reconnects, rather than reading mid-body garbage as fixed headers.
/// A CONNACK body is `[session-present][return-code]` per section 3.2, so a short body is a violation treated as a failed connect rather than falling through and subscribing on a malformed accept.
/// A non-zero return code means the broker refused: bad auth, unavailable, and the rest.
///
/// ## Inbound command routing
///
/// The mqttthing set-topics are three explicit filters under `<prefix>/+/set`, one SUBSCRIBE each because that is simpler than a wildcard.
/// `update/set` is checked before `ha/set` so the shared prefix parse fires exactly once.
/// The HA-native JSON command is `{"state":"ON"|"OFF"[,"brightness":0-255]}`, parsed with the same flat `mm::json` helpers `HttpServerModule::applyWledState` uses: key-order-independent and whitespace-safe.
/// It needs a bigger NUL-terminated buffer than the scalar `value[32]`, and its brightness is already 0-255 so no rescale, unlike the mqttthing `brightness/set` 0-100 path.
/// HA sends exactly "ON" or "OFF" and only those are acted on. A malformed or truncated value is ignored rather than treated as OFF, so a bad payload never turns the light off.
/// A short fixed payload is copied NUL-terminated so the `strcmp` and `atoi` parsers are safe on the non-terminated socket slice, and a malformed or oversized one is refused.
/// `hsv/set` is the mqttthing HSV shape `"h,s,v"`, hue 0-359 with sat and value 0-100. Hue and sat pick the nearest palette, and value maps to brightness so the color wheel's brightness ring still dims.
/// `hsv/get` publishes the chosen palette's representative hue, full sat, and value as brightness percent.
/// Subscriptions otherwise happen only at CONNACK, so a mid-session discovery turn-on subscribes as well as announcing.
///
/// ## The one reach into the light domain
///
/// `Palettes::nearestForHue` is a pure hue-and-saturation to index CONVERSION with no light state or objects, the one narrow reach this core module makes into the light domain.
/// PO-accepted: routing a HomeKit color to a palette needs the palette set, which is inherently light-domain.
/// A format conversion is the least-coupling way to bridge it, since the module still drives the palette via `Scheduler::setControl` rather than a light object.
/// This is a deliberate divergence from the plan's "no light include" line, made with the trade-off understood rather than by precedent.
///
#include "core/system/MqttModule.h"

#include "core/module/Scheduler.h"     // setControl: the shared apply-core
#include "core/util/JsonUtil.h"      // json::hasKey/parseBool/parseInt/parseString: the inbound ha/set parse
#include "core/system/ControlModule.h"  // look-only presets -> the HA effect list
#include "core/util/JsonSink.h"      // jsonEscape: escape the editable deviceName into the discovery JSON
#include "core/util/build_info.h"    // kVersion / kFirmwareName: reported to HA's update entity
#include "core/system/FirmwareUpdateModule.h"  // g_otaStatus / g_otaBytesTotal / otaInFlight: shared with the OTA task
#include "light/util/Palette.h"      // Palettes::nearestForHue: the one reach into the light domain, @xref{the-one-reach-into-the-light-domain}

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mm {

namespace {
// A scratch send buffer big enough for any control packet we build (CONNECT with auth is the largest at well under 200 bytes). Sends go through sendPacket (non-blocking writeSome).
constexpr size_t kSendBufLen = 256;
}  // namespace

// The topic prefix, derived live from a STABLE hardware id: MoonLight/<last6-of-MAC> (lowercase hex), e.g. MoonLight/563cfe. @xref{the-topic-identity-is-the-mac}
void MqttModule::topicPrefix(char* out, size_t cap) const {
    uint8_t mac[6] = {};
    platform::getMacAddress(mac);
    std::snprintf(out, cap, "%s/%02x%02x%02x", kPrefixRoot, mac[3], mac[4], mac[5]);
}

// A full topic: <prefix>/<suffix>, e.g. MoonLight/563cfe/on/set.
void MqttModule::buildTopic(char* out, size_t cap, const char* suffix) const {
    char prefix[kPrefixLen];              // exactly what topicPrefix can produce, no slack to truncate into
    topicPrefix(prefix, sizeof(prefix));
    std::snprintf(out, cap, "%s/%s", prefix, suffix);
}

// The HA MQTT-discovery config topic: homeassistant/light/MoonLight_<mac6>/config. @xref{the-discovery-topics}
void MqttModule::buildDiscoveryTopic(char* out, size_t cap) const {
    uint8_t mac[6] = {};
    platform::getMacAddress(mac);
    std::snprintf(out, cap, "homeassistant/light/%s_%02x%02x%02x/config",
                  kPrefixRoot, mac[3], mac[4], mac[5]);
}

// The availability (LWT) topic: <prefix>/status, which HA's avty_t points at so the entity greys out. @xref{availability-and-retract}
void MqttModule::buildStatusTopic(char* out, size_t cap) const {
    buildTopic(out, cap, "status");
}

// Lazily allocate the two discovery scratch buffers, sized to what this device publishes, and report them via setDynamicBytes. @xref{the-discovery-scratch-buffers}
bool MqttModule::ensureDiscoveryBuffers() {
    const size_t effects = haEffectListBytes();
    const size_t wantPayload = kDiscoveryPayloadBase + effects;
    const size_t wantBuf     = kDiscoveryBufBase + effects;
    if (discoveryBuf_ && discoveryPayload_ &&
        discoveryPayloadLen_ == wantPayload && discoveryBufLen_ == wantBuf) return true;
    freeDiscoveryBuffers();
    discoveryBuf_     = static_cast<uint8_t*>(platform::alloc(wantBuf));
    discoveryPayload_ = static_cast<char*>(platform::alloc(wantPayload));
    if (!discoveryBuf_ || !discoveryPayload_) { freeDiscoveryBuffers(); return false; }
    discoveryPayloadLen_ = wantPayload;
    discoveryBufLen_     = wantBuf;
    setDynamicBytes(wantBuf + wantPayload);
    return true;
}

// The bytes the effect_list needs, counting only look-only presets. @xref{the-effect-list-is-looks-only}
size_t MqttModule::haEffectListBytes() const {
    if (!controlModule_) return 0;
    size_t n = 0, count = 0;
    for (uint8_t i = 0; i < controlModule_->presetCount(); i++) {
        if (!controlModule_->isLookOnly(i)) continue;
        const char* nm = controlModule_->presetName(i);
        if (!nm) continue;
        n += std::strlen(nm) * 2 + 3;   // worst-case escaping, plus two quotes and a comma
        count++;
    }
    return count ? n + 20 : 0;          // ,"effect":true,"fx_list":[]
}

size_t MqttModule::writeHaEffectList(char* out, size_t cap) const {
    if (!controlModule_ || cap == 0) return 0;
    size_t n = 0;
    bool first = true;
    for (uint8_t i = 0; i < controlModule_->presetCount(); i++) {
        if (!controlModule_->isLookOnly(i)) continue;
        const char* nm = controlModule_->presetName(i);
        if (!nm) continue;
        char esc[72];
        jsonEscape(nm, esc, sizeof(esc));
        const int w = std::snprintf(out + n, cap - n, "%s\"%s\"", first ? "" : ",", esc);
        if (w <= 0 || static_cast<size_t>(w) >= cap - n) return 0;   // refuse a partial list
        n += static_cast<size_t>(w);
        first = false;
    }
    return first ? 0 : n;
}

void MqttModule::freeDiscoveryBuffers() {
    if (discoveryBuf_)     { platform::free(discoveryBuf_);     discoveryBuf_ = nullptr; }
    if (discoveryPayload_) { platform::free(discoveryPayload_); discoveryPayload_ = nullptr; }
    setDynamicBytes(0);
}

void MqttModule::publishDiscovery(bool announce) {
    // Retract (OFF): an empty retained payload to the config topic, framed into a small LOCAL buffer, then free. @xref{availability-and-retract}
    if (!announce) {
        if (state_ == Conn::Connected) {
            char topic[96];
            buildDiscoveryTopic(topic, sizeof(topic));
            uint8_t buf[kSendBufLen];
            const size_t n = buildMqttPublish(topic, nullptr, 0, buf, sizeof(buf), /*retain=*/true);
            if (n == 0 || !sendPacket(buf, n)) resetConnection("error: discovery retract failed");
        }
        freeDiscoveryBuffers();
        return;
    }

    // Announce path needs a live socket (there's nothing to publish to otherwise) and allocates.
    if (state_ != Conn::Connected) return;
    if (!ensureDiscoveryBuffers()) { setStatusLine("error: discovery alloc failed"); return; }

    char topic[96];
    buildDiscoveryTopic(topic, sizeof(topic));

    // Identity + display name: uniq_id/object_id derive from the stable MAC, the friendly deviceName rides only on `dev.name`. @xref{the-discovery-payload}
    uint8_t mac[6] = {};
    platform::getMacAddress(mac);
    char id[24];
    std::snprintf(id, sizeof(id), "%s_%02x%02x%02x", kPrefixRoot, mac[3], mac[4], mac[5]);
    const char* dn = systemModule_ ? systemModule_->deviceName() : nullptr;
    if (!dn || !dn[0]) dn = id;
    // The deviceName is user-editable, so escape it (dev.name only) with the shared jsonEscape.
    char dnEsc[72];
    jsonEscape(dn, dnEsc, sizeof(dnEsc));

    char cmd[80], stat[80], avty[80];
    buildTopic(cmd,  sizeof(cmd),  "ha/set");
    buildTopic(stat, sizeof(stat), "ha/state");
    buildStatusTopic(avty, sizeof(avty));

    // The looks this device offers, built into discoveryBuf_ as scratch, which must NOT be a region of discoveryPayload_. @xref{the-discovery-scratch-buffers}
    char* fxScratch = reinterpret_cast<char*>(discoveryBuf_);
    const size_t fxLen = writeHaEffectList(fxScratch, discoveryBufLen_);
    char fxKey[24] = "";
    if (fxLen) std::snprintf(fxKey, sizeof(fxKey), "\"effect\":true,");

    // A JSON-schema MQTT light in HA's abbreviated keys, with `name:null` collapsing the entity slug. @xref{the-discovery-payload}
    const int pn = std::snprintf(discoveryPayload_, discoveryPayloadLen_,
        "{\"schema\":\"json\",\"name\":null,\"uniq_id\":\"%s\",\"cmd_t\":\"%s\","
        "\"stat_t\":\"%s\",\"avty_t\":\"%s\",\"brightness\":true,%s%s%s%s"
        "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"MoonModules\",\"mdl\":\"MoonLight\"}}",
        id, cmd, stat, avty,
        fxKey,
        fxLen ? "\"fx_list\":[" : "", fxLen ? fxScratch : "", fxLen ? "]," : "",
        id, dnEsc);
    if (pn <= 0 || static_cast<size_t>(pn) >= discoveryPayloadLen_) return;   // truncated → don't send a broken config

    const size_t n = buildMqttPublish(topic, reinterpret_cast<const uint8_t*>(discoveryPayload_),
                                      static_cast<size_t>(pn), discoveryBuf_, discoveryBufLen_,
                                      /*retain=*/true);
    if (n == 0) { setStatusLine("error: discovery config too large"); return; }
    // The same "reset on a failed send" contract as the ping / subscribe / state paths. @xref{sending-never-blocks-the-render-loop}
    if (!sendPacket(discoveryBuf_, n)) resetConnection("error: discovery publish failed");
}

// SUBSCRIBE to <prefix>/ha/set, the HA-native JSON command topic, at CONNACK and on a mid-session haDiscovery turn-on. @xref{inbound-command-routing}
void MqttModule::subscribeHaSet() {
    if (state_ != Conn::Connected) return;
    char topic[96];
    buildTopic(topic, sizeof(topic), "ha/set");
    uint8_t buf[kSendBufLen];
    const size_t n = buildMqttSubscribe(nextPacketId_++, topic, buf, sizeof(buf));
    if (n == 0 || !sendPacket(buf, n)) resetConnection("error: ha subscribe failed");
}

// The HA update entity, the second HA-discovery component alongside the light. @xref{the-update-entity}

// Mirror of buildDiscoveryTopic for the `update` component type, with the SAME object id. @xref{the-discovery-topics}
void MqttModule::buildUpdateDiscoveryTopic(char* out, size_t cap) const {
    uint8_t mac[6] = {};
    platform::getMacAddress(mac);
    std::snprintf(out, cap, "homeassistant/update/%s_%02x%02x%02x/config",
                  kPrefixRoot, mac[3], mac[4], mac[5]);
}

// Announce or retract the update entity, reusing the light's scratch pair for the announce. @xref{the-discovery-scratch-buffers}
void MqttModule::publishUpdateDiscovery(bool announce) {
    if (state_ != Conn::Connected) return;

    char topic[96];
    buildUpdateDiscoveryTopic(topic, sizeof(topic));

    // Retract: an empty retained payload removes the entity, and fits in kSendBufLen because a tombstone has no body. @xref{availability-and-retract}
    if (!announce) {
        uint8_t tomb[kSendBufLen];
        const size_t n = buildMqttPublish(topic, nullptr, 0, tomb, sizeof(tomb), /*retain=*/true);
        if (n == 0 || !sendPacket(tomb, n)) resetConnection("error: update discovery retract failed");
        return;
    }

    if (!ensureDiscoveryBuffers()) { setStatusLine("error: discovery alloc failed"); return; }

    uint8_t mac[6] = {};
    platform::getMacAddress(mac);
    char id[24];
    std::snprintf(id, sizeof(id), "%s_%02x%02x%02x", kPrefixRoot, mac[3], mac[4], mac[5]);
    const char* dn = systemModule_ ? systemModule_->deviceName() : nullptr;
    if (!dn || !dn[0]) dn = id;
    char dnEsc[72];
    jsonEscape(dn, dnEsc, sizeof(dnEsc));

    char stat[80], cmd[80], avty[80];
    buildTopic(stat, sizeof(stat), "update/state");
    buildTopic(cmd,  sizeof(cmd),  "update/set");
    buildStatusTopic(avty, sizeof(avty));

    // device_class:"firmware" and entity_category:"diagnostic" are what shape HA's label, icon and placement. @xref{the-discovery-payload}
    const int pn = std::snprintf(discoveryPayload_, discoveryPayloadLen_,
        "{\"name\":null,\"uniq_id\":\"%s_update\",\"stat_t\":\"%s\",\"cmd_t\":\"%s\","
        "\"avty_t\":\"%s\",\"entity_category\":\"diagnostic\",\"device_class\":\"firmware\","
        "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"MoonModules\",\"mdl\":\"MoonLight\"}}",
        id, stat, cmd, avty, id, dnEsc);
    if (pn <= 0 || static_cast<size_t>(pn) >= discoveryPayloadLen_) return;   // truncated → don't send

    const size_t n = buildMqttPublish(topic, reinterpret_cast<const uint8_t*>(discoveryPayload_),
                                      static_cast<size_t>(pn), discoveryBuf_, discoveryBufLen_,
                                      /*retain=*/true);
    if (n == 0) { setStatusLine("error: update discovery too large"); return; }
    if (!sendPacket(discoveryBuf_, n)) resetConnection("error: update discovery publish failed");
}

// Retained state on <prefix>/update/state, carrying the two versions and the release URL. @xref{the-update-entity}
void MqttModule::publishUpdateState() {
    if (state_ != Conn::Connected) return;
    char topic[128];
    buildTopic(topic, sizeof(topic), "update/state");
    char payload[256];
    // RETAINED and read by a person, so it names where releases will live: a card left behind by a rename is a dead link.
    const int pn = std::snprintf(payload, sizeof(payload),
        "{\"installed_version\":\"%s\",\"latest_version\":\"%s\","
        "\"release_url\":\"https://github.com/%s/releases\","
        "\"title\":\"%s firmware\"}",
        kVersion, kVersion, kReleaseRepo, kProjectImageName);
    if (pn <= 0 || static_cast<size_t>(pn) >= sizeof(payload)) return;
    uint8_t buf[kSendBufLen];
    const size_t n = buildMqttPublish(topic, reinterpret_cast<const uint8_t*>(payload),
                                      static_cast<size_t>(pn), buf, sizeof(buf), /*retain=*/true);
    if (n == 0 || !sendPacket(buf, n)) resetConnection("error: update state publish failed");
}

void MqttModule::subscribeUpdateSet() {
    if (state_ != Conn::Connected) return;
    char topic[96];
    buildTopic(topic, sizeof(topic), "update/set");
    uint8_t buf[kSendBufLen];
    const size_t n = buildMqttSubscribe(nextPacketId_++, topic, buf, sizeof(buf));
    if (n == 0 || !sendPacket(buf, n)) resetConnection("error: update subscribe failed");
}

// HA's install command: the payload is the target version, and the device builds the release URL from it. @xref{the-update-entity}
void MqttModule::handleUpdateInstall(const char* payload, size_t payloadLen) {
    if (otaInFlight()) return;   // matches the /api/firmware/url 409 guard's intent

    // Copy the payload into a bounded local buffer for null-termination and shape checks, refusing a malformed or oversized one. @xref{inbound-command-routing}
    char version[32] = {};
    const size_t vlen = payloadLen < sizeof(version) - 1 ? payloadLen : sizeof(version) - 1;
    std::memcpy(version, payload, vlen);
    version[vlen] = '\0';
    // Strip an optional leading 'v', HA's payload_install_template can send either shape.
    const char* v = (version[0] == 'v') ? version + 1 : version;
    // Empty payload means "install latest", so fall back to this build's own version rather than doing nothing. @xref{the-update-entity}
    if (v[0] == '\0') v = (kVersion[0] == 'v') ? kVersion + 1 : kVersion;

    char url[256];
    const int un = std::snprintf(url, sizeof(url), kReleaseAssetUrlFormat,
                                 kReleaseRepo, v, kFirmwareName, v);
    if (un <= 0 || static_cast<size_t>(un) >= sizeof(url)) return;
    // Today's repository, tried where the address above does not answer (FirmwareUpdateModule names why both).
    char altUrl[256];
    const int an = std::snprintf(altUrl, sizeof(altUrl), kReleaseAssetUrlFormat,
                                 kFallbackRepo, v, kFirmwareName, v);
    const bool haveAlt = an > 0 && static_cast<size_t>(an) < sizeof(altUrl);

    // Seed the shared globals so the first WS push shows "starting" rather than a stale string from a prior URL-triggered OTA, same seed the HTTP path does.
    std::snprintf(g_otaStatus, sizeof(g_otaStatus), "starting");
    g_otaBytesRead = 0;
    g_otaBytesTotal = 0;

    (void)platform::http_fetch_to_ota(url, g_otaStatus, sizeof(g_otaStatus),
                                      &g_otaBytesRead, &g_otaBytesTotal,
                                      haveAlt ? altUrl : nullptr);
    // No response to publish, HA polls the retained update/state. @xref{the-update-entity}
}

void MqttModule::setup() {
    setStatusLine(enabled() ? "idle" : "disabled");
    MoonModule::setup();
}

// Release the lazily-allocated discovery buffers on teardown, so no memory outlives the module. @xref{the-discovery-scratch-buffers}
void MqttModule::release() {
    freeDiscoveryBuffers();
    MoonModule::release();
}

void MqttModule::defineControls() {
    controls_.addText("broker", broker_, sizeof(broker_));
    controls_.addControl("port", port_, 1, 65535);
    controls_.addText("username", username_, sizeof(username_));
    controls_.addPassword("password", password_, sizeof(password_));
    controls_.addControl("haDiscovery", haDiscovery_);   // announce a HA MQTT-discovery light (default off; WLED /json covers HA)
    controls_.addReadOnly("mqtt_status", statusStr_, sizeof(statusStr_));
    MoonModule::defineControls();
}

// A broker/port/credentials change re-homes the connection, so drop the socket and let tick1s reconnect live on the next tick. @xref{reconnect-keepalive-and-protocol-errors}
void MqttModule::onControlChanged(const char* controlName) {
    if (std::strcmp(controlName, "broker") == 0 || std::strcmp(controlName, "port") == 0 ||
        std::strcmp(controlName, "username") == 0 || std::strcmp(controlName, "password") == 0) {
        resetConnection(enabled() ? "reconnecting" : "disabled");
    } else if (std::strcmp(controlName, "haDiscovery") == 0) {
        // Announce or retract live and only when connected, with NO reset, one gate driving both components. @xref{availability-and-retract}
        publishDiscovery(haDiscovery_);
        publishUpdateDiscovery(haDiscovery_);
        if (haDiscovery_) {
            subscribeHaSet(); publishState(true);
            subscribeUpdateSet(); publishUpdateState();
        }
    }
}

// The enable/disable transition, the only place a disable can act: a clean DISCONNECT beats a dangling socket the broker must time out.
void MqttModule::onEnabled(bool enabled) {
    if (!enabled && conn_.valid()) {
        uint8_t buf[4];
        const size_t n = buildMqttDisconnect(buf, sizeof(buf));
        if (n) sendPacket(buf, n);   // best-effort courtesy DISCONNECT
    }
    if (!enabled) freeDiscoveryBuffers();   // MQTT turned off → not used → reclaim the discovery heap
    resetConnection(enabled ? "idle" : "disabled");
}

// Send a whole MQTT packet without EVER blocking the render loop, returning false so the caller resets on a partial write. @xref{sending-never-blocks-the-render-loop}
bool MqttModule::sendPacket(const uint8_t* data, size_t len) {
    if (len == 0) return true;
    // Test seam: mirror the outbound bytes into the capture buffer, null in production. @xref{sending-never-blocks-the-render-loop}
    if (sendCapture_) {
        if (sendCaptureLen_ + len <= sendCaptureCap_) {
            std::memcpy(sendCapture_ + sendCaptureLen_, data, len);
            sendCaptureLen_ += len;
        }
        return true;   // capture mode always "succeeds" so the connect/publish flow proceeds in tests
    }
    const int sent = conn_.writeSome(data, len);
    return sent == static_cast<int>(len);   // all-or-fail; a partial/0/-1 is a connection problem
}

void MqttModule::enableSendCaptureForTest(uint8_t* buf, size_t cap) {
    sendCapture_ = buf; sendCaptureCap_ = cap; sendCaptureLen_ = 0;
}

// Close the socket and return to Idle with a status line, the single reset path every caller shares. @xref{sending-never-blocks-the-render-loop}
void MqttModule::resetConnection(const char* status) {
    conn_.close();
    state_ = Conn::Idle;
    havePublished_ = false;
    setStatusLine(status);
}

void MqttModule::tick1s() MM_NONBLOCKING {
    if constexpr (!platform::hasNetwork) { MoonModule::tick1s(); return; }

    if (!enabled() || broker_[0] == '\0') {
        if (conn_.valid()) resetConnection(enabled() ? "idle" : "disabled");
        MoonModule::tick1s();
        return;
    }
    if (!platform::networkReady()) { MoonModule::tick1s(); return; }

    // Presets changed while connected: re-announce so the retained config carries the current effect list, since HA only re-reads it when the message changes. @xref{the-effect-list-is-looks-only}
    if (haDiscovery_ && state_ == Conn::Connected && controlModule_) {
        const uint32_t rev = controlModule_->presetsRevision();
        if (rev != lastPresetsRev_) {
            lastPresetsRev_ = rev;
            publishDiscovery(true);
        }
    }

    const uint32_t now = platform::millis();
    switch (state_) {
        case Conn::Idle: {
            // Backoff between connect attempts, harder after a prior failure. @xref{reconnect-keepalive-and-protocol-errors}
            const uint32_t backoff = lastConnectFailed_ ? kFailedBackoffMs : kReconnectBackoffMs;
            if (now - lastConnectTry_ >= backoff || lastConnectTry_ == 0) {
                lastConnectTry_ = now;
                startConnect();
            }
            break;
        }
        case Conn::ConnectingTcp: {
            // Poll the non-blocking TCP connect, never blocks the tick.
            const auto r = conn_.connectPoll();
            if (r == platform::TcpConnection::ConnectResult::Connected) sendConnectPacket();
            else if (r == platform::TcpConnection::ConnectResult::Failed) resetConnection("error: connect failed");
            else if (now - connectStartedMs_ >= kConnectTimeoutMs) resetConnection("error: connect timeout");
            break;
        }
        case Conn::Connecting:
            // TCP up, CONNECT sent, waiting for CONNACK, and the silent-broker wedge is bounded by the same timeout. @xref{reconnect-keepalive-and-protocol-errors}
            serviceConnected();
            if (state_ == Conn::Connecting && now - connectStartedMs_ >= kConnectTimeoutMs)
                resetConnection("error: no CONNACK");
            break;
        case Conn::Connected:
            serviceConnected();
            break;
    }
    MoonModule::tick1s();
}

// Begin a NON-BLOCKING TCP connect, which tick1s polls in ConnectingTcp. @xref{reconnect-keepalive-and-protocol-errors}
void MqttModule::startConnect() {
    setStatusLine("connecting");
    // Assume failure until a full connect succeeds, so every failure route leaves the longer backoff armed. @xref{reconnect-keepalive-and-protocol-errors}
    lastConnectFailed_ = true;
    if (!conn_.connectStart(broker_, port_)) {   // immediate failure (DNS / socket)
        resetConnection("error: connect failed");
        return;
    }
    connectStartedMs_ = platform::millis();
    state_ = Conn::ConnectingTcp;
}

// TCP is up, send CONNECT and wait for CONNACK.
void MqttModule::sendConnectPacket() {
    uint8_t buf[kSendBufLen];
    const char* user = username_[0] ? username_ : nullptr;
    const char* pass = password_[0] ? password_ : nullptr;
    // A stable, slash-free clientId: "MoonLight-<last6-of-MAC>", NOT topicPrefix() which carries a slash. @xref{the-topic-identity-is-the-mac}
    uint8_t mac[6] = {};
    platform::getMacAddress(mac);
    char clientId[32];
    std::snprintf(clientId, sizeof(clientId), "%s-%02x%02x%02x", kPrefixRoot, mac[3], mac[4], mac[5]);
    // Last Will, declared here at CONNECT: retained "offline" on <prefix>/status. @xref{availability-and-retract}
    char willTopic[96];
    buildStatusTopic(willTopic, sizeof(willTopic));
    const size_t n = buildMqttConnect(clientId, user, pass, kKeepaliveSec, buf, sizeof(buf),
                                      willTopic, "offline", /*willRetain=*/true);
    if (n == 0 || !sendPacket(buf, n)) {
        resetConnection("error: connect send failed");
        return;
    }
    parser_ = MqttInboundParser{};       // fresh parser per connection
    state_ = Conn::Connecting;           // waiting for CONNACK
    connectStartedMs_ = platform::millis();
    lastActivity_ = connectStartedMs_;
    lastPingSent_ = connectStartedMs_;
}

void MqttModule::serviceConnected() {
    // Drain whatever the socket has (non-blocking). A bounded read per tick keeps this cheap.
    uint8_t rx[256];
    for (int pass = 0; pass < 8; pass++) {
        const int n = conn_.read(rx, sizeof(rx));
        if (n > 0) {
            lastActivity_ = platform::millis();
            for (int i = 0; i < n; i++) handleInboundByte(rx[i]);
            if (state_ == Conn::Idle) return;   // handleInboundByte reset us (refused / malformed)
        } else if (n == 0) {                    // peer closed
            resetConnection("disconnected");
            return;
        } else {
            break;                              // -1 = nothing pending right now
        }
    }

    if (state_ == Conn::Connected) {
        publishState(false);                 // emit any changed get topics
        // Re-publish the friendly name if the device was renamed while connected, change-detected without a stored name buffer. @xref{the-topic-identity-is-the-mac}
        maybeRepublishName();

        // Keepalive: PINGREQ at keepalive/2, and a broker silent past ~keepalive*1.5 is dropped. @xref{reconnect-keepalive-and-protocol-errors}
        const uint32_t now = platform::millis();
        if (now - lastPingSent_ >= (kKeepaliveSec * 1000u) / 2) {
            uint8_t ping[2];
            const size_t pn = buildMqttPingreq(ping, sizeof(ping));
            if (pn == 0 || !sendPacket(ping, pn)) { resetConnection("error: ping failed"); return; }
            lastPingSent_ = now;
        }
        if (now - lastActivity_ >= kKeepaliveSec * 1500u)    // 1.5× keepalive with no traffic
            resetConnection("timeout");
    }
}

void MqttModule::handleInboundByte(uint8_t byte) {
    const MqttFeedResult r = parser_.feed(byte);
    // A malformed or oversize packet desyncs the stream for the connection's life, so drop and reconnect. @xref{reconnect-keepalive-and-protocol-errors}
    if (r == MqttFeedResult::Malformed) { resetConnection("error: bad packet"); return; }
    if (r != MqttFeedResult::PacketReady) return;

    const uint8_t type = parser_.lastType();
    if (type == static_cast<uint8_t>(MqttPacketType::Connack)) {
        // A CONNACK body shorter than [session-present][return-code] is a protocol violation, not a malformed accept to subscribe on. @xref{reconnect-keepalive-and-protocol-errors}
        if (parser_.bodyLen() < 2) { resetConnection("error: bad CONNACK"); return; }
        // Non-zero return code = the broker refused (bad auth, unavailable, …).
        if (parser_.body()[1] != 0) {
            resetConnection("error: broker refused");
            return;
        }
        // Subscribe to <prefix>/+/set with three explicit filters, one SUBSCRIBE each. @xref{inbound-command-routing}
        static const char* kSets[] = {"on/set", "brightness/set", "hsv/set"};
        char topic[128];
        for (const char* suffix : kSets) {
            buildTopic(topic, sizeof(topic), suffix);
            uint8_t buf[kSendBufLen];
            const size_t n = buildMqttSubscribe(nextPacketId_++, topic, buf, sizeof(buf));
            if (n == 0 || !sendPacket(buf, n)) { resetConnection("error: subscribe failed"); return; }
        }
        state_ = Conn::Connected;
        lastConnectFailed_ = false;          // full success → next reconnect uses the short backoff
        setStatusLine("connected");
        havePublished_ = false;
        publishName();                       // retained friendly name so a hub shows the display name
        // Availability: retained "online" on <prefix>/status, which must precede the discovery announce. @xref{availability-and-retract}
        {
            char st[96]; buildStatusTopic(st, sizeof(st));
            uint8_t sb[kSendBufLen];
            const size_t sn = buildMqttPublish(st, reinterpret_cast<const uint8_t*>("online"), 6,
                                               sb, sizeof(sb), /*retain=*/true);
            if (sn == 0 || !sendPacket(sb, sn)) { resetConnection("error: availability publish failed"); return; }
        }
        // On connect: announce and subscribe when discovery is on, and retract when it is off. @xref{availability-and-retract}
        if (haDiscovery_) {
            publishDiscovery(true);       subscribeHaSet();
            publishUpdateDiscovery(true); subscribeUpdateSet();
            publishUpdateState();
        } else {
            publishDiscovery(false);
            publishUpdateDiscovery(false);
        }
        publishState(true);                  // publish initial state so mqttthing + HA show it
    } else if (type == static_cast<uint8_t>(MqttPacketType::Publish)) {
        const char* topic = nullptr; const uint8_t* payload = nullptr; size_t plLen = 0;
        if (parser_.publish(&topic, &payload, &plLen)) routePublish(topic, payload, plLen);
    }
    // PINGRESP / SUBACK: nothing to do beyond the activity timestamp already stamped.
}

void MqttModule::routePublish(const char* topic, const uint8_t* payload, size_t payloadLen) {
    // Match the topic suffix after our derived prefix, copying a short payload NUL-terminated for the parsers below. @xref{inbound-command-routing}
    char prefix[kPrefixLen];              // exactly what topicPrefix can produce, no slack to truncate into
    topicPrefix(prefix, sizeof(prefix));
    const size_t prefixLen = std::strlen(prefix);
    if (std::strncmp(topic, prefix, prefixLen) != 0 || topic[prefixLen] != '/') return;
    const char* suffix = topic + prefixLen + 1;

    // The HA update-entity install command, checked BEFORE ha/set so the shared prefix parse fires exactly once. @xref{the-update-entity}
    if (std::strcmp(suffix, "update/set") == 0) {
        handleUpdateInstall(reinterpret_cast<const char*>(payload), payloadLen);
        return;
    }

    // The HA-native JSON command, parsed with the same flat mm::json helpers the WLED state route uses. @xref{inbound-command-routing}
    if (std::strcmp(suffix, "ha/set") == 0) {
        char body[128];
        const size_t blen = payloadLen < sizeof(body) - 1 ? payloadLen : sizeof(body) - 1;
        std::memcpy(body, payload, blen);
        body[blen] = '\0';
        if (json::hasKey(body, "state")) {
            char st[8] = "";
            json::parseString(body, "state", st, sizeof(st));
            // Act only on the exact "ON"/"OFF" HA sends, so a bad payload never turns the light off. @xref{inbound-command-routing}
            if (std::strcmp(st, "ON") == 0)       setControlValue("on", "{\"value\":true}");
            else if (std::strcmp(st, "OFF") == 0) setControlValue("on", "{\"value\":false}");
        }
        if (json::hasKey(body, "brightness")) {
            int bri = json::parseInt(body, "brightness");
            if (bri < 0) bri = 0;
            if (bri > 255) bri = 255;
            char json[24];
            std::snprintf(json, sizeof(json), "{\"value\":%d}", bri);
            setControlValue("brightness", json);
        }
        // A look chosen from the effect dropdown, re-checked for look-only at the entry point. @xref{the-effect-list-is-looks-only}
        if (controlModule_ && json::hasKey(body, "effect")) {
            char fx[40] = {};
            json::parseString(body, "effect", fx, sizeof(fx));
            if (fx[0]) controlModule_->applyLookByName(fx);
        }
        return;
    }

    char value[32];
    const size_t vlen = payloadLen < sizeof(value) - 1 ? payloadLen : sizeof(value) - 1;
    std::memcpy(value, payload, vlen);
    value[vlen] = '\0';

    if (std::strcmp(suffix, "on/set") == 0) {
        const bool on = (std::strcmp(value, "true") == 0 || std::strcmp(value, "1") == 0);
        setControlValue("on", on ? "{\"value\":true}" : "{\"value\":false}");
    } else if (std::strcmp(suffix, "brightness/set") == 0) {
        // mqttthing sends 0..100; rescale to 0..255.
        int pct = mm::json::parseIntStr(value);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        const int bri = (pct * 255) / 100;
        char json[24];
        std::snprintf(json, sizeof(json), "{\"value\":%d}", bri);
        setControlValue("brightness", json);
    } else if (std::strcmp(suffix, "hsv/set") == 0) {
        // The mqttthing HSV triple: hue and sat pick the nearest palette, value maps to brightness. @xref{inbound-command-routing}
        int h = 0, s = 0, v = -1;
        std::sscanf(value, "%d,%d,%d", &h, &s, &v);
        const uint8_t idx = Palettes::nearestForHue(static_cast<uint16_t>(h < 0 ? 0 : h),
                                                    static_cast<uint8_t>(s < 0 ? 0 : (s > 100 ? 255 : s * 255 / 100)));
        char json[24];
        std::snprintf(json, sizeof(json), "{\"value\":%u}", static_cast<unsigned>(idx));
        setControlValue("palette", json);
        if (v >= 0) {
            int bri = (v > 100 ? 100 : v) * 255 / 100;
            std::snprintf(json, sizeof(json), "{\"value\":%d}", bri);
            setControlValue("brightness", json);
        }
    }
}

// A cheap rolling signature of the current deviceName (djb2), so a rename is detected without storing the name. @xref{the-topic-identity-is-the-mac}
static uint32_t nameSignature(const char* s) {
    uint32_t h = 5381;
    for (; s && *s; s++) h = h * 33u + static_cast<uint8_t>(*s);
    return h;
}

// Publish the friendly display name on the retained `<prefix>/name` topic, on connect and on a rename. @xref{the-topic-identity-is-the-mac}
void MqttModule::publishName() {
    if (state_ != Conn::Connected) return;
    // Always the device's own name, which SystemModule guarantees non-empty and per-device unique. @xref{the-topic-identity-is-the-mac}
    const char* dn = systemModule_ ? systemModule_->deviceName() : nullptr;
    if (!dn || !dn[0]) return;
    char topic[128];
    buildTopic(topic, sizeof(topic), "name");
    uint8_t buf[kSendBufLen];
    const size_t n = buildMqttPublish(topic, reinterpret_cast<const uint8_t*>(dn), std::strlen(dn),
                                      buf, sizeof(buf), /*retain=*/true);
    // Stamp the signature only on a SUCCESSFUL send, so a failed publish is retried next tick. @xref{sending-never-blocks-the-render-loop}
    if (n && sendPacket(buf, n)) nameSig_ = nameSignature(dn);
}

// Re-publish the name only if it changed since the last publish (a rename while connected).
void MqttModule::maybeRepublishName() {
    const char* dn = systemModule_ ? systemModule_->deviceName() : nullptr;
    if (dn && dn[0] && nameSignature(dn) != nameSig_) publishName();
}

void MqttModule::publishState(bool force) {
    if (state_ != Conn::Connected) return;
    Scheduler* s = Scheduler::instance();
    const bool on = driversOn(s);
    const uint8_t bri = driversBrightness(s);
    const uint8_t pal = driversPalette(s);
    // The applied look is published state, so it is part of the change gate. @xref{the-effect-list-is-looks-only}
    const char* lookNow = controlModule_ ? controlModule_->currentLook() : "";
    if (!force && havePublished_ && on == lastOn_ && bri == lastBri_ && pal == lastPalette_ &&
        std::strncmp(lookNow, lastLook_, sizeof(lastLook_) - 1) == 0) return;

    char topic[128];
    uint8_t buf[kSendBufLen];

    // One of the three get-topics, where any send failure resets without committing last*/havePublished_. @xref{sending-never-blocks-the-render-loop}
    auto publish = [&](const char* suffix, const char* payload) -> bool {
        buildTopic(topic, sizeof(topic), suffix);
        const size_t n = buildMqttPublish(topic, reinterpret_cast<const uint8_t*>(payload),
                                          std::strlen(payload), buf, sizeof(buf));
        return n != 0 && sendPacket(buf, n);
    };

    char briStr[8];
    std::snprintf(briStr, sizeof(briStr), "%d", (bri * 100) / 255);
    // hsv/get: the chosen palette's representative hue, full sat, value = brightness%. @xref{inbound-command-routing}
    char hsvStr[16];
    std::snprintf(hsvStr, sizeof(hsvStr), "%u,100,%d",
                  static_cast<unsigned>(Palettes::representativeHue(pal)), (bri * 100) / 255);

    if (!publish("on/get", on ? "true" : "false") ||
        !publish("brightness/get", briStr) ||
        !publish("hsv/get", hsvStr)) {
        resetConnection("error: state publish failed");
        return;
    }

    // HA-native state on <prefix>/ha/state: retained, 0-255 brightness, and inside this change-gated block. @xref{inbound-command-routing}
    if (haDiscovery_) {
        // The applied look rides along, so HA's dropdown shows the look that is on. @xref{the-effect-list-is-looks-only}
        char fxEsc[72] = "";
        const char* look = controlModule_ ? controlModule_->currentLook() : nullptr;
        if (look && look[0]) jsonEscape(look, fxEsc, sizeof(fxEsc));
        char haState[136];
        std::snprintf(haState, sizeof(haState), "{\"state\":\"%s\",\"brightness\":%u%s%s%s}",
                      on ? "ON" : "OFF", static_cast<unsigned>(bri),
                      fxEsc[0] ? ",\"effect\":\"" : "", fxEsc, fxEsc[0] ? "\"" : "");
        char haTopic[128];
        buildTopic(haTopic, sizeof(haTopic), "ha/state");
        const size_t n = buildMqttPublish(haTopic, reinterpret_cast<const uint8_t*>(haState),
                                          std::strlen(haState), buf, sizeof(buf), /*retain=*/true);
        if (n == 0 || !sendPacket(buf, n)) { resetConnection("error: state publish failed"); return; }
    }

    lastOn_ = on; lastBri_ = bri; lastPalette_ = pal;
    std::snprintf(lastLook_, sizeof(lastLook_), "%s", lookNow);   // committed only after every send succeeded
    havePublished_ = true;
}

void MqttModule::setControlValue(const char* control, const char* valueJson) {
    if (Scheduler* s = Scheduler::instance()) s->setControl("Drivers", control, valueJson);
}

void MqttModule::feedForTest(const uint8_t* bytes, size_t len) {
    for (size_t i = 0; i < len; i++) handleInboundByte(bytes[i]);
}

void MqttModule::setStatusLine(const char* msg) {
    std::snprintf(statusStr_, sizeof(statusStr_), "%s", msg);
}

} // namespace mm
