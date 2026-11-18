/// @module DeviceGroupSync
/// @also SharedControls, DeviceGroup, ControlModule

/// A device group carries its members' shared controls: a slot's change goes out as what it drives, by name, value and stamp, the newer stamp winning on every member; a value the group set is not sent on; a look is fetched from its maker once per stamp; only what an effects preset may set applies; a full table keeps the newest values.

#include "doctest.h"
#include "core/module/Scheduler.h"
#include "core/services/OscPacket.h"
#include "core/system/ControlModule.h"
#include "core/system/DeviceGroup.h"
#include "core/system/DeviceGroupSync.h"
#include "core/system/Devices.h"
#include "core/util/JsonFetch.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace mm;

namespace {

struct ClockGuard { ~ClockGuard() { platform::setTestNowMs(0); } };

/// An effect with what a slot drives: a brightness, a speed no slot drives, a switch, and a pin no group may set.
struct Lamp : MoonModule {
    uint8_t brightness = 10;
    uint8_t speed = 1;
    bool on = false;
    int8_t pin = 4;
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Effect; }
    void defineControls() override {
        MoonModule::defineControls();
        controls_.addControl("brightness", brightness);
        controls_.addControl("speed", speed);
        controls_.addControl("on", on);
        controls_.addPin("pin", pin);
    }
};

/// A system module, as WiFi is: its channel is the device's own.
struct Radio : MoonModule {
    uint8_t channel = 6;
    void defineControls() override {
        MoonModule::defineControls();
        controls_.addControl("channel", channel);
    }
};

/// The sync with its datagrams recorded instead of sent.
struct RecordingSync : DeviceGroupSync {
    using DeviceGroupSync::DeviceGroupSync;
    std::vector<std::vector<uint8_t>> sent;
    /// The datagrams sent to `address`.
    std::vector<std::vector<uint8_t>> to(const char* address) const {
        std::vector<std::vector<uint8_t>> out;
        for (const auto& p : sent) if (osc::Reader(p.data(), p.size(), address).ok()) out.push_back(p);
        return out;
    }
protected:
    void transmit(const uint8_t* data, size_t len) override { sent.emplace_back(data, data + len); }
};

/// A values message for group `id`: one parameter, its value and stamp, and the sender's digest, which a test leaves unequal.
std::vector<uint8_t> valuesMessage(const char* id, const char* param, int32_t value, uint64_t hlc, uint32_t writer = 7) {
    std::vector<uint8_t> p(256);
    osc::Writer w(p.data(), p.size(), SharedControls::kValuesAddress, ",sisihi");
    w.str(id);
    w.i32(0);
    w.str(param);
    w.i32(static_cast<uint32_t>(value));
    w.i64(hlc);
    w.i32(writer);
    p.resize(w.size());
    return p;
}

/// A recall of look `name` for group `id`, served at 10.0.0.5:8080.
std::vector<uint8_t> lookMessage(const char* id, const char* name, uint64_t hlc) {
    std::vector<uint8_t> p(256);
    osc::Writer w(p.data(), p.size(), SharedControls::kLookAddress, ",sshiii");
    w.str(id);
    w.str(name);
    w.i64(hlc);
    w.i32(7);
    const uint8_t ip[4] = {10, 0, 0, 5};
    uint32_t packed = 0;
    std::memcpy(&packed, ip, 4);
    w.i32(packed);
    w.i32(8080);
    p.resize(w.size());
    return p;
}

/// A device in the group "Stage" it made, a Lamp, and the Control module with fader1 driving the lamp's brightness.
struct Member {
    ClockGuard clock;
    Scheduler scheduler;
    Devices* devices = new Devices;
    DeviceGroup* group = new DeviceGroup;
    ControlModule* control = new ControlModule;
    Lamp* lamp = new Lamp;
    Radio* radio = new Radio;
    char saved[256] = "";
    Member() {
        platform::setTestNowMs(1000);
        devices->setName("Devices");
        control->setName("Control");
        lamp->setName("Lamp");
        radio->setName("WiFi");
        scheduler.addModule(devices);
        scheduler.addModule(lamp);
        scheduler.addModule(radio);
        scheduler.addModule(control);
        scheduler.setup();
        devices->addChild(group);
        group->setName("Stage");
        group->setup();
        REQUIRE(scheduler.setControl("Control", "fader1Target", "{\"value\":\"Lamp.brightness\"}") == Scheduler::SetControlResult::Ok);
    }
    ~Member() { scheduler.release(); }
    const char* id() const { return group->groupId(); }
};

/// A values message's one parameter, value and stamp, read back.
struct Read { std::string id, param; int32_t value = 0; Stamp stamp{}; };
Read readValues(const std::vector<uint8_t>& p) {
    osc::Reader r(p.data(), p.size(), SharedControls::kValuesAddress);
    Read out;
    out.id = r.str();
    r.i32();
    out.param = r.str();
    out.value = static_cast<int32_t>(r.i32());
    out.stamp = Stamp{r.i64(), r.i32()};
    return out;
}

}  // namespace

TEST_CASE("a slot change on a member sends what it drives, by name, with its value and stamp") {
    Member m;
    {
        RecordingSync sync(*m.devices);
        sync.tick(*m.devices, m.saved, sizeof(m.saved));   // attaches to the Control module
        sync.sent.clear();
        REQUIRE(m.scheduler.setControl("Control", "fader1", "{\"value\":77}") == Scheduler::SetControlResult::Ok);
        const auto values = sync.to(SharedControls::kValuesAddress);
        REQUIRE(values.size() == 1);
        const Read r = readValues(values[0]);
        CHECK(r.id == m.id());
        CHECK(r.param == "Lamp.brightness");
        CHECK(r.value == 77);
        CHECK(r.stamp.hlc > 0);
        sync.close();
    }
}

TEST_CASE("a parameter no slot drives never leaves the board, and what a member held at boot is not sent") {
    Member m;
    {
        RecordingSync sync(*m.devices);
        sync.tick(*m.devices, m.saved, sizeof(m.saved));
        CHECK(sync.to(SharedControls::kValuesAddress).size() <= 1);   // the Trickle repeat's digest at most, holding no value
        sync.sent.clear();
        REQUIRE(m.scheduler.setControl("Lamp", "speed", "{\"value\":9}") == Scheduler::SetControlResult::Ok);
        m.control->tick20ms();
        CHECK(sync.to(SharedControls::kValuesAddress).empty());
        sync.close();
    }
}

TEST_CASE("a member applies a parameter by name though no slot of its own drives it") {
    Member m;
    {
        RecordingSync sync(*m.devices);
        const auto p = valuesMessage(m.id(), "Lamp.speed", 42, uint64_t{5000} << 16);
        sync.handle(*m.devices, p.data(), p.size());
        CHECK(m.lamp->speed == 42);
        sync.close();
    }
}

TEST_CASE("a newer stamp wins and an older one loses, so a crossing repeat keeps the newer value and asks for a repeat") {
    Member m;
    {
        RecordingSync sync(*m.devices);
        const auto newer = valuesMessage(m.id(), "Lamp.speed", 50, uint64_t{6000} << 16);
        const auto older = valuesMessage(m.id(), "Lamp.speed", 20, uint64_t{5000} << 16);
        sync.handle(*m.devices, newer.data(), newer.size());
        for (int s = 0; s < 7; s++) m.group->tick1s();   // Trickle's interval grows past a second
        (void)m.group->sendDue();
        sync.handle(*m.devices, older.data(), older.size());
        CHECK(m.lamp->speed == 50);
        m.group->tick1s();
        CHECK(m.group->sendDue());   // this device holds newer: its interval went back to a second
        sync.close();
    }
}

TEST_CASE("two writers' equal readings converge on the higher writer's value") {
    Member m;
    {
        RecordingSync sync(*m.devices);
        const auto low = valuesMessage(m.id(), "Lamp.speed", 30, uint64_t{5000} << 16, 3);
        const auto high = valuesMessage(m.id(), "Lamp.speed", 60, uint64_t{5000} << 16, 9);
        sync.handle(*m.devices, high.data(), high.size());
        sync.handle(*m.devices, low.data(), low.size());
        CHECK(m.lamp->speed == 60);
        sync.close();
    }
}

TEST_CASE("a member back from a restart takes the group's newer value, and a change the group made is not sent on") {
    Member m;
    {
        RecordingSync sync(*m.devices);
        sync.tick(*m.devices, m.saved, sizeof(m.saved));   // its saved brightness, 10, is seen and not sent
        sync.sent.clear();
        const auto p = valuesMessage(m.id(), "Lamp.brightness", 99, uint64_t{5000} << 16);
        sync.handle(*m.devices, p.data(), p.size());
        CHECK(m.lamp->brightness == 99);
        m.control->tick20ms();   // fader1 follows the brightness the group set
        CHECK(sync.to(SharedControls::kValuesAddress).empty());
        sync.close();
    }
}

namespace {
/// The preset document the maker serves, counted.
int gFetches = 0;
std::string gUrl;
int servePreset(const char*, const char* url, const char*, char* body, size_t cap) {
    gFetches++;
    gUrl = url;
    const char doc[] = R"({"$slot":3,"Lamp":{"speed":33}})";
    if (cap < sizeof(doc)) return 0;
    std::memcpy(body, doc, sizeof(doc));
    return 200;
}
}  // namespace

TEST_CASE("an effects preset recall fetches the document from its maker and applies it, once per stamp") {
    Member m;
    gFetches = 0;
    platform::setTestHttpFetch(&servePreset);
    {
        RecordingSync sync(*m.devices);
        sync.tick(*m.devices, m.saved, sizeof(m.saved));
        const auto p = lookMessage(m.id(), "warm sunset", uint64_t{5000} << 16);
        sync.handle(*m.devices, p.data(), p.size());
        for (int i = 0; i < 4; i++) {   // fetch, then apply between frames, a step of the 20 ms read each
            sync.receive(*m.devices);
            JsonFetch::instance().settle(2000);
            m.scheduler.preparePending();
        }
        CHECK(gFetches == 1);
        CHECK(gUrl == "http://10.0.0.5:8080/api/file?path=/.config/presets/warm%20sunset.json");
        CHECK(m.lamp->speed == 33);
        m.lamp->speed = 1;
        sync.handle(*m.devices, p.data(), p.size());   // the same recall repeated
        for (int i = 0; i < 4; i++) {
            sync.receive(*m.devices);
            JsonFetch::instance().settle(2000);
            m.scheduler.preparePending();
        }
        CHECK(gFetches == 1);
        CHECK(m.lamp->speed == 1);
        sync.close();
    }
    platform::setTestHttpFetch(nullptr);
}

TEST_CASE("a look applied here goes out as a recall naming it, to every group held") {
    Member m;
    {
        RecordingSync sync(*m.devices);
        sync.tick(*m.devices, m.saved, sizeof(m.saved));
        sync.sent.clear();
        sync.lookApplied("sunset");
        const auto looks = sync.to(SharedControls::kLookAddress);
        REQUIRE(looks.size() == 1);
        osc::Reader r(looks[0].data(), looks[0].size(), SharedControls::kLookAddress);
        CHECK(std::string(r.str()) == m.id());
        CHECK(std::string(r.str()) == "sunset");
        sync.close();
    }
}

TEST_CASE("a datagram naming a system module's control or a pin leaves it as it is") {
    Member m;
    {
        RecordingSync sync(*m.devices);
        const auto wifi = valuesMessage(m.id(), "WiFi.channel", 11, uint64_t{5000} << 16);
        const auto pin = valuesMessage(m.id(), "Lamp.pin", 13, uint64_t{5000} << 16);
        sync.handle(*m.devices, wifi.data(), wifi.size());
        sync.handle(*m.devices, pin.data(), pin.size());
        CHECK(m.radio->channel == 6);
        CHECK(m.lamp->pin == 4);
        sync.close();
    }
}

namespace {
/// A values message for group "g" carrying `count` values, Lamp.p0 onward, each stamped `hlc` plus its index.
std::vector<uint8_t> manyValues(int count, uint64_t hlc) {
    std::vector<uint8_t> p(4096);
    std::string tags = ",si";
    for (int i = 0; i < count; i++) tags += "sihi";
    osc::Writer w(p.data(), p.size(), SharedControls::kValuesAddress, tags.c_str());
    w.str("g");
    w.i32(0);
    for (int i = 0; i < count; i++) {
        char param[24];
        std::snprintf(param, sizeof(param), "Lamp.p%d", i);
        w.str(param);
        w.i32(static_cast<uint32_t>(i));
        w.i64(hlc + static_cast<uint64_t>(i));
        w.i32(7);
    }
    p.resize(w.size());
    return p;
}
/// Merge `p` into `shared` as group "g"; whether the sender's digest agrees afterwards.
bool merge(SharedControls& shared, const std::vector<uint8_t>& p) {
    osc::Reader r(p.data(), p.size(), SharedControls::kValuesAddress);
    r.str();
    return shared.mergeValues(r, SharedControls::keyOf("g"), nullptr);
}
}  // namespace

TEST_CASE("a message past the per-message bound merges only the bound") {
    Member m;
    SharedControls shared(*m.devices);
    merge(shared, manyValues(SharedControls::kPerMessage + 4, uint64_t{5000} << 16));
    CHECK(shared.valueCount() == SharedControls::kPerMessage);
}

TEST_CASE("a full table keeps the newest values, so two members holding the same newest set agree and stop asking for repeats") {
    Member m;
    SharedControls a(*m.devices), b(*m.devices);
    const uint64_t t0 = uint64_t{5000} << 16;
    // A holds the oldest full table, B the same values one newer: both end on B's newest set.
    for (int i = 0; i < SharedControls::kMaxValues; i++) {
        char param[24];
        std::snprintf(param, sizeof(param), "Lamp.p%d", i);
        CHECK(a.setLocal(SharedControls::keyOf("g"), param, i, Stamp{t0 + i, 7}));
        std::snprintf(param, sizeof(param), "Lamp.p%d", i + 1);
        CHECK(b.setLocal(SharedControls::keyOf("g"), param, i + 1, Stamp{t0 + i + 1, 7}));
    }
    CHECK(a.note() == nullptr);
    uint8_t buf[1472];
    for (uint16_t from = 0; from < b.valueCount();) {   // B's repeat, message by message
        const size_t n = b.writeValues(buf, sizeof(buf), "g", from);
        osc::Reader r(buf, n, SharedControls::kValuesAddress);
        r.str();
        a.mergeValues(r, SharedControls::keyOf("g"), nullptr);
    }
    CHECK(a.digest(SharedControls::keyOf("g")) == b.digest(SharedControls::keyOf("g")));
    REQUIRE(a.note() != nullptr);
    CHECK(std::string(a.note()) == "shared controls full");
    CHECK(a.holds("Lamp.p48", 48));   // the newest is held, so it stays out of the outgoing changes
    CHECK_FALSE(a.holds("Lamp.p0", 0));
}
