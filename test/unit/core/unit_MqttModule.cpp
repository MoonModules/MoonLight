/// @module MqttModule
/// @also Scheduler
///
/// Pins MqttModule's inbound routing: a PUBLISH on a set topic drives the matching Drivers control.
///
/// @moreinfo
///
/// ## Delivering a PUBLISH
///
/// A suffix such as "on/set" is sent under the derived prefix, as the broker socket would deliver it.
/// A leading slash sends an absolute topic instead, which is what the wrong-prefix case needs.
///
/// ## The two command topics
///
/// Home Assistant drives the JSON-schema light with a state and an optional brightness on the ha/set topic.
/// Its brightness is already 0-255, where the mqttthing brightness/set topic carries 0-100 and is rescaled.
///
/// ## Why a preset change re-announces
///
/// Home Assistant re-reads the effect list only when the retained discovery message changes, so a mid-session preset would never appear without this.
/// All three mutations funnel through one revision, pinned in unit_ControlModule, so one path proves the mechanism.
///
/// ## The fake carries the real ranges
///
/// Drivers.palette binds 0..kCount-1, so the fake declares a Uint8 with the full 0..255 range, a superset of the sixty built-ins.
/// An artificially small Select would clamp away a nearest-palette index the MQTT map returns, and the test would pass for the wrong reason.
///
/// ## The MAC is derived, never written out
///
/// The desktop MAC is a per-install stored identity from platform_desktop.cpp's getMacAddress, so a literal would pin whatever this machine happened to generate.
/// The derivation is what these cases check: the topic identity is the MAC, not the device name, so a rename cannot move the topics.
/// A command on the MAC-based topic keeps working after a rename, where one on a name-based topic never matched at all.
///
/// ## Why a test reads the fixed header
///
/// Asserting the RETAIN bit, bit 0 of the PUBLISH fixed header per MQTT 3.1.1 section 3.3.1.3, catches what string-matching cannot.
/// A regression dropping retain=true flips that bit and leaves every substring in the payload intact.
///
/// ## The guard that stranded a buffer
///
/// The original guard bailed on a state other than Connected before reaching the free.
/// So a discovery-off toggle during a reconnect stranded 768 bytes until release, breaking the promise of no memory when discovery is off.
///
/// ## Why the state gate includes the look
///
/// Home Assistant otherwise keeps showing the previous effect after a look-only change, including one made on the device's own pad grid.
///
/// ## The routing is provable without a broker
///
/// A control is driven through Scheduler::setControl, the shared primitive IR and the WLED bridge also use, so the socket is never involved.
/// feedForTest() injects raw MQTT bytes built with the tested MqttPacket builders, exactly as a broker would deliver them, mirroring InfraredService::injectCodeForTest.
/// A FakeDrivers stands in for the real one, carrying the on, brightness and palette controls MQTT targets.

#include "doctest.h"
#include "core/system/MqttModule.h"
#include "core/system/MqttPacket.h"
#include "core/module/Scheduler.h"
#include "core/module/MoonModule.h"
#include "core/system/SystemModule.h"
#include "core/system/ControlModule.h"
#include "core/system/FilesystemModule.h"
#include "core/util/ModuleFactory.h"
#include "light/effects/NoiseEffect.h"
#include "light/layers/Layer.h"
#include "light/layers/Effects.h"
#include "platform/platform.h"

#include <filesystem>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace mm;

namespace {

// Stands in for Drivers: on (Bool), brightness (Uint8 0-255), palette (Select). Named "Drivers" so MqttModule's setControl("Drivers", …) resolves to it.
struct FakeDrivers : public MoonModule {
    bool on = true;
    uint8_t brightness = 100;
    uint8_t palette = 0;
    void defineControls() override {
        controls_.addControl("on", on);
        controls_.addControl("brightness", brightness, 0, 255);
        // The real 0..255 range, so a nearest-palette index is not clamped away by an artificially small Select: @xref{the-fake-carries-the-real-ranges}.
        controls_.addControl("palette", palette, 0, 255);
    }
};

// A scheduler with FakeDrivers, a SystemModule and an MqttModule, set up so controls are bound: @xref{the-mac-is-derived-never-written-out}.
/// The last six MAC hex digits alone, which is what the Home Assistant discovery topic and its unique_id are built from.
inline const char* macId() {
    static char buf[16] = {};
    if (!buf[0]) {
        uint8_t mac[6] = {};
        mm::platform::getMacAddress(mac);
        std::snprintf(buf, sizeof(buf), "%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
    return buf;
}

inline const char* macPrefix() {
    static char buf[32] = {};
    if (!buf[0]) {
        uint8_t mac[6] = {};
        mm::platform::getMacAddress(mac);
        std::snprintf(buf, sizeof(buf), "MoonLight/%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
    return buf;
}

struct Rig {
    const char* const kPrefix = macPrefix();
    Scheduler scheduler;
    FakeDrivers* drivers = new FakeDrivers();
    SystemModule* system = new SystemModule();
    MqttModule* mqtt = new MqttModule();
    Rig() {
        drivers->setName("Drivers");
        system->setName("System");
        mqtt->setName("Mqtt");
        mqtt->setSystemModule(system);   // for the published friendly-name (not the topic identity)
        scheduler.addModule(drivers);
        scheduler.addModule(system);
        scheduler.addModule(mqtt);
        scheduler.setup();   // binds controls, sets Scheduler::instance()
    }
    ~Rig() { scheduler.release(); }

        // A PUBLISH to `suffix` under the derived prefix, or to an absolute topic when it starts with a slash: @xref{delivering-a-publish}.
    void publish(const char* suffix, const char* payload) {
        char topic[128];
        if (suffix[0] == '/') std::snprintf(topic, sizeof(topic), "%s", suffix + 1);   // absolute
        else std::snprintf(topic, sizeof(topic), "%s/%s", kPrefix, suffix);
        uint8_t buf[160];
        const size_t n = buildMqttPublish(topic, reinterpret_cast<const uint8_t*>(payload),
                                          std::strlen(payload), buf, sizeof(buf));
        REQUIRE(n > 0);
        mqtt->feedForTest(buf, n);
    }
};

// The fixed-header first byte of the first PUBLISH on `wantTopic`, or -1, so a test can assert the RETAIN bit: @xref{why-a-test-reads-the-fixed-header}.
int publishFlagsForTopic(const uint8_t* buf, size_t len, const char* wantTopic) {
    size_t i = 0;
    while (i < len) {
        const uint8_t first = buf[i];
        // Remaining Length: a 1–4 byte varint (§2.2.3).
        size_t j = i + 1, mult = 1, remLen = 0;
        for (int b = 0; b < 4 && j < len; b++, j++) {
            remLen += (buf[j] & 0x7F) * mult;
            if (!(buf[j] & 0x80)) { j++; break; }
            mult *= 128;
        }
        const size_t body = j;                 // first byte after the fixed header
        if ((first & 0xF0) == 0x30 && body + 2 <= len) {   // PUBLISH
            const size_t topicLen = (size_t(buf[body]) << 8) | buf[body + 1];
            if (body + 2 + topicLen <= len &&
                std::strncmp(reinterpret_cast<const char*>(buf + body + 2), wantTopic, topicLen) == 0 &&
                std::strlen(wantTopic) == topicLen) {
                return first;
            }
        }
        i = body + remLen;
    }
    return -1;
}

}  // namespace

TEST_CASE("MqttModule: on/set drives Drivers.on") {
    Rig r;
    r.drivers->on = true;
    r.publish("on/set", "false");
    CHECK(r.drivers->on == false);
    r.publish("on/set", "true");
    CHECK(r.drivers->on == true);
    // "1"/"0" are accepted too (mqttthing integerValue mode).
    r.publish("on/set", "0");
    CHECK(r.drivers->on == false);
    r.publish("on/set", "1");
    CHECK(r.drivers->on == true);
}

TEST_CASE("MqttModule: brightness/set rescales 0-100 to 0-255") {
    Rig r;
    r.publish("brightness/set", "0");
    CHECK(r.drivers->brightness == 0);
    r.publish("brightness/set", "100");
    CHECK(r.drivers->brightness == 255);
    r.publish("brightness/set", "50");
    CHECK(r.drivers->brightness == 127);          // 50*255/100
    // Out-of-range clamps, not wraps.
    r.publish("brightness/set", "250");
    CHECK(r.drivers->brightness == 255);
}

TEST_CASE("MqttModule: hsv/set maps a hue to the nearest palette + value to brightness") {
    Rig r;
    // A blue-ish hue at full saturation should pick a blue-family palette (a non-zero index, not Rainbow at 0). We assert it moved off the default and that value drove brightness.
    r.drivers->palette = 0;
    r.publish("hsv/set", "210,100,40");      // blue, sat 100%, value 40%
    CHECK(r.drivers->palette != 0);               // snapped to some blue-family palette
    CHECK(r.drivers->brightness == (40 * 255) / 100);   // value → brightness
}

    // The HA-native ha/set topic, whose brightness needs no rescale: @xref{the-two-command-topics}.

TEST_CASE("MqttModule: ha/set {state} drives Drivers.on") {
    Rig r;
    r.drivers->on = true;
    r.publish("ha/set", "{\"state\":\"OFF\"}");
    CHECK(r.drivers->on == false);
    r.publish("ha/set", "{\"state\":\"ON\"}");
    CHECK(r.drivers->on == true);
}

TEST_CASE("MqttModule: ha/set {brightness} maps 0-255 with no rescale") {
    Rig r;
    r.publish("ha/set", "{\"state\":\"ON\",\"brightness\":128}");
    CHECK(r.drivers->on == true);
    CHECK(r.drivers->brightness == 128);          // HA is already 0-255 — no *255/100
    r.publish("ha/set", "{\"brightness\":255}");
    CHECK(r.drivers->brightness == 255);
    // Out-of-range clamps.
    r.publish("ha/set", "{\"brightness\":999}");
    CHECK(r.drivers->brightness == 255);
}

TEST_CASE("MqttModule: ha/set is key-order-independent") {
    Rig r;
    r.drivers->on = false;
    // brightness before state, mm::json's strstr lookup is order-independent (HA emits compact JSON; the flat helpers match `"key":` / `"key": `, so we feed the same no-inner-space shape HA sends).
    r.publish("ha/set", "{\"brightness\":64,\"state\":\"ON\"}");
    CHECK(r.drivers->on == true);
    CHECK(r.drivers->brightness == 64);
}

TEST_CASE("MqttModule: ha/set with only state leaves brightness untouched") {
    Rig r;
    r.drivers->brightness = 200;
    r.publish("ha/set", "{\"state\":\"OFF\"}");   // no brightness key
    CHECK(r.drivers->on == false);
    CHECK(r.drivers->brightness == 200);          // unchanged (hasKey guard)
}

// The discovery announce: on CONNACK the module publishes a RETAINED config to homeassistant/light/MoonLight_<mac6>/config. Assert via the outbound-capture seam (no live socket).
TEST_CASE("MqttModule: CONNACK publishes a retained HA discovery config") {
    Rig r;
    uint8_t cap[1024];
    r.mqtt->enableSendCaptureForTest(cap, sizeof(cap));
    // haDiscovery defaults OFF (opt-in; the WLED /json shim covers HA), so enable it first, this test asserts the announce shape when the user opts into MQTT discovery.
    Scheduler::instance()->setControl("Mqtt", "haDiscovery", "{\"value\":true}");
    // A CONNACK-accept: fixed header 0x20, len 2, session-present 0, return-code 0 (accepted).
    const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    r.mqtt->feedForTest(connack, sizeof(connack));
    const size_t len = r.mqtt->sentCaptureLenForTest();
    REQUIRE(len > 0);
    // The captured stream must contain the discovery topic + the key config fields.
    std::string sent(reinterpret_cast<const char*>(cap), len);
    CHECK(sent.find(std::string("homeassistant/light/MoonLight_") + macId() + "/config") != std::string::npos);
    CHECK(sent.find("\"schema\":\"json\"") != std::string::npos);
    CHECK(sent.find(std::string("\"uniq_id\":\"MoonLight_") + macId() + "\"") != std::string::npos);
    CHECK(sent.find(std::string("MoonLight/") + macId() + "/ha/set") != std::string::npos);    // cmd_t
    CHECK(sent.find(std::string("MoonLight/") + macId() + "/ha/state") != std::string::npos);  // stat_t
    CHECK(sent.find(std::string("MoonLight/") + macId() + "/status") != std::string::npos);    // avty_t
    CHECK(sent.find("online") != std::string::npos);                     // the retained availability publish
    // Both publishes must carry RETAIN, since a late-joining Home Assistant reads the retained config and state.
    const int cfgFlags = publishFlagsForTopic(cap, len, (std::string("homeassistant/light/MoonLight_") + macId() + "/config").c_str());
    REQUIRE(cfgFlags >= 0);
    CHECK((cfgFlags & 0x01) == 0x01);                                    // discovery config retained
    const int avtyFlags = publishFlagsForTopic(cap, len, (std::string("MoonLight/") + macId() + "/status").c_str());
    REQUIRE(avtyFlags >= 0);
    CHECK((avtyFlags & 0x01) == 0x01);                                   // availability "online" retained
}

    // Regression from P4 and S31 hardware: turning discovery off frees its buffers even with the socket down: @xref{the-guard-that-stranded-a-buffer}.
TEST_CASE("MqttModule: retract frees the discovery buffers even while disconnected") {
    Rig r;
    uint8_t cap[1024];
    r.mqtt->enableSendCaptureForTest(cap, sizeof(cap));
    // haDiscovery defaults OFF; opt in so CONNACK allocates the discovery buffers this test tracks.
    Scheduler::instance()->setControl("Mqtt", "haDiscovery", "{\"value\":true}");
    // CONNACK-accept → Connected → announce allocates the discovery buffers (448 + 320 = 768).
    const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    r.mqtt->feedForTest(connack, sizeof(connack));
    CHECK(r.mqtt->dynamicBytes() == MqttModule::kDiscoveryDynamicBytes);
    // A broker change re-homes the socket → resetConnection drops state to Idle, buffers still held (a reconnect must not churn the heap). Now we're "allocated but not Connected".
    Scheduler::instance()->setControl("Mqtt", "broker", "{\"value\":\"10.0.0.9\"}");
    CHECK(r.mqtt->dynamicBytes() == MqttModule::kDiscoveryDynamicBytes);          // reset kept the buffers (correct)
    // Toggle discovery OFF while disconnected, must free despite no live socket.
    Scheduler::instance()->setControl("Mqtt", "haDiscovery", "{\"value\":false}");
    CHECK(r.mqtt->dynamicBytes() == 0);            // the fix: retract freed even while not Connected
}

TEST_CASE("MqttModule: a PUBLISH on an unrelated topic is ignored, not a crash") {
    Rig r;
    const uint8_t beforeBri = r.drivers->brightness;
    const bool beforeOn = r.drivers->on;
    r.publish("unknown/set", "whatever");    // no matching suffix
    r.publish("/otherdevice/on/set", "false");     // wrong prefix
    CHECK(r.drivers->brightness == beforeBri);
    CHECK(r.drivers->on == beforeOn);
}

TEST_CASE("MqttModule: a PUBLISH split across feeds still routes (fragment reassembly)") {
    Rig r;
    r.drivers->on = true;
    uint8_t buf[128];
    const char* payload = "false";
    const size_t n = buildMqttPublish((std::string("MoonLight/") + macId() + "/on/set").c_str(), reinterpret_cast<const uint8_t*>(payload),
                                      std::strlen(payload), buf, sizeof(buf));
    REQUIRE(n > 0);
    // Feed one byte at a time, the parser holds partial state until the packet completes.
    for (size_t i = 0; i < n; i++) r.mqtt->feedForTest(&buf[i], 1);
    CHECK(r.drivers->on == false);
}

// Regression: the topic identity is the MAC rather than the device name, so a rename cannot move the topics: @xref{the-mac-is-derived-never-written-out}.
TEST_CASE("MqttModule: topic identity is MAC-stable, not affected by a device rename") {
    Rig r;
    r.drivers->on = true;
    // Command on the MAC topic works.
    r.publish("on/set", "false");
    CHECK(r.drivers->on == false);
    // Rename the device, topics must stay on the MAC prefix.
    Scheduler::instance()->setControl("System", "deviceName", "{\"value\":\"LivingRoom\"}");
    r.drivers->on = true;
    r.publish("on/set", "false");                 // still the MAC prefix (Rig::kPrefix)
    CHECK(r.drivers->on == false);                // rename didn't break routing
    // A command on a name-derived topic never matches (proves identity isn't the name).
    r.drivers->on = true;
    r.publish("/MoonLight/LivingRoom/on/set", "false");   // absolute, name-based
    CHECK(r.drivers->on == true);                 // ignored — not our (MAC) prefix
}


namespace {

/// Rig + a real preset stack (FilesystemModule, ControlModule, a Effects tree), so the MQTT<->preset seams are exercised against the real modules rather than a stub.
/// Filesystem isolated per fixture.
struct PresetRig : Rig {
    FilesystemModule* fs = nullptr;
    ControlModule* control = nullptr;
    MoonModule* layers = nullptr;
    char root_[256] = {};   // fixture-private fs root; restored in the destructor

    PresetRig() {
        static unsigned seq = 0;
        std::snprintf(root_, sizeof(root_), "/tmp/mm_mqtt_preset_%u", ++seq);
        std::filesystem::remove_all(root_);
        platform::fsSetRoot(root_);

        ModuleFactory::registerType<Effects>("Effects");
        ModuleFactory::registerType<Layer>("Layer");
        ModuleFactory::registerType<NoiseEffect>("NoiseEffect");
        ModuleFactory::registerType<ControlModule>("ControlModule");

        fs = new FilesystemModule();
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        layers = ModuleFactory::create("Effects");
        control = static_cast<ControlModule*>(ModuleFactory::create("ControlModule"));
        scheduler.addModule(fs);
        scheduler.addModule(layers);
        scheduler.addModule(control);
        fs->setup(); layers->setup(); control->defineControls(); control->setup();
        mqtt->setControlModule(control);
    }
    ~PresetRig() {
        // Restore the default root BEFORE the directory goes: fsSetRoot is global, so leaving it pointed at a deleted fixture directory would follow every later test in the run.
        platform::fsSetRoot("build");
        std::filesystem::remove_all(root_);
    }

    /// Drop a valid Effects look into the preset folder and rescan, as a save or an upload would.
    void addLook(const char* name) {
        platform::fsMkdir(ControlModule::kPresetDir);
        char path[160];
        std::snprintf(path, sizeof(path), "%s/%s.json", ControlModule::kPresetDir, name);
        const char* body = "{\"captures\":\"Effects\",\"Effects.enabled\":true}";
        REQUIRE(platform::fsWriteAtomic(path, body, std::strlen(body)));
        control->setup();   // rescan picks it up and bumps the revision
    }

    /// Bytes captured since `from`, as a string for content asserts.
    std::string capturedSince(size_t from, const uint8_t* cap) const {
        return std::string(reinterpret_cast<const char*>(cap) + from,
                           mqtt->sentCaptureLenForTest() - from);
    }
};

}  // namespace

    // A preset change while connected re-announces the retained config: @xref{why-a-preset-change-re-announces}.
TEST_CASE("MqttModule re-announces the effect list when a preset appears mid-session") {
    PresetRig r;
    static uint8_t cap[8192];
    r.mqtt->enableSendCaptureForTest(cap, sizeof(cap));
    // A broker must be configured or tick1s bails at its idle guard before reaching the Connected-state work this test exercises. TEST-NET address; capture mode never dials it.
    Scheduler::instance()->setControl("Mqtt", "broker", "{\"value\":\"192.0.2.1\"}");
    Scheduler::instance()->setControl("Mqtt", "haDiscovery", "{\"value\":true}");
    const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    r.mqtt->feedForTest(connack, sizeof(connack));
    r.mqtt->tick1s();                                    // settle: revision seen, nothing new
    const size_t before = r.mqtt->sentCaptureLenForTest();

    r.addLook("nightlook");
    r.mqtt->tick1s();                                    // the revision moved -> re-announce

    const std::string sent = r.capturedSince(before, cap);
    CHECK(sent.find("fx_list") != std::string::npos);
    CHECK(sent.find("nightlook") != std::string::npos);
}

// A look changes none of on, brightness or palette, so the state gate must include the look itself: @xref{why-the-state-gate-includes-the-look}.
TEST_CASE("MqttModule publishes state when only the applied look changed") {
    PresetRig r;
    r.addLook("only-look");
    static uint8_t cap[8192];
    r.mqtt->enableSendCaptureForTest(cap, sizeof(cap));
    Scheduler::instance()->setControl("Mqtt", "broker", "{\"value\":\"192.0.2.1\"}");   // see above
    Scheduler::instance()->setControl("Mqtt", "haDiscovery", "{\"value\":true}");
    const uint8_t connack[] = {0x20, 0x02, 0x00, 0x00};
    r.mqtt->feedForTest(connack, sizeof(connack));
    r.mqtt->tick1s();                                    // initial publishes committed
    const size_t before = r.mqtt->sentCaptureLenForTest();

    REQUIRE(r.control->applyLookByName("only-look"));    // drivers values untouched
    r.mqtt->tick1s();                                    // gate must open on the look signature

    const std::string sent = r.capturedSince(before, cap);
    CHECK(sent.find("\"effect\":\"only-look\"") != std::string::npos);
}
