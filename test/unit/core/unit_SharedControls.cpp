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
#include "core/util/ClockSync.h"
#include "core/util/fnv.h"
#include "platform/platform_clock.h"   // setTestNowMs, millis, uptimeUs
#include "platform/platform_heap.h"   // setTestAllocRefusedFrom
#include "platform/platform_fs.h"   // fsWriteAtomic, fsRemove

#include <algorithm>
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

/// A look's advertisement for group `id`: its name, stamp, size, digest and the chunks the sender holds.
std::vector<uint8_t> lookAdvert(const char* id, const char* name, uint64_t hlc, uint32_t size, uint32_t digest, uint32_t have) {
    std::vector<uint8_t> p(256);
    osc::Writer w(p.data(), p.size(), SharedControls::kLookAddress, ",sshiiii");
    w.str(id);
    w.str(name);
    w.i64(hlc);
    w.i32(7);
    w.i32(size);
    w.i32(digest);
    w.i32(have);
    p.resize(w.size());
    return p;
}

/// Chunk `index` of document `doc`, look `name` for group `id`, stamped `hlc`; `digest` as given, the document's own by default.
std::vector<uint8_t> lookChunk(const char* id, const char* name, uint64_t hlc, const std::string& doc, uint32_t index, uint32_t digest = 0) {
    std::vector<uint8_t> p(2048);
    osc::Writer w(p.data(), p.size(), SharedControls::kChunkAddress, ",sshiiihib");
    w.str(id);
    w.str(name);
    w.i64(hlc);
    w.i32(7);
    w.i32(static_cast<uint32_t>(doc.size()));
    w.i32(digest ? digest : fnv1a(doc.data(), doc.size()));
    w.i64(0);
    w.i32(index);
    const size_t offset = size_t{index} * SharedControls::kChunkBytes;
    w.blob(reinterpret_cast<const uint8_t*>(doc.data()) + offset, std::min<size_t>(SharedControls::kChunkBytes, doc.size() - offset));
    p.resize(w.size());
    return p;
}

/// A request for the chunks in `missing` of the look stamped `hlc`, group `id`.
std::vector<uint8_t> lookNeed(const char* id, uint64_t hlc, uint32_t missing) {
    std::vector<uint8_t> p(128);
    osc::Writer w(p.data(), p.size(), SharedControls::kNeedAddress, ",shii");
    w.str(id);
    w.i64(hlc);
    w.i32(7);
    w.i32(missing);
    p.resize(w.size());
    return p;
}

/// A preset document setting the lamp's speed to 33, padded with spaces to `size` bytes: JSON whitespace, so it applies at any size.
std::string speedDoc(size_t size = 0) {
    std::string doc = R"({"Lamp":{"speed":33}})";
    if (doc.size() < size) doc.insert(1, size - doc.size(), ' ');
    return doc;
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
/// A preset file for a case, removed when it ends, so no later case's Control module loads it as a pad.
struct PresetFile {
    std::string path;
    PresetFile(const char* name, const std::string& doc) : path(std::string(ControlModule::kPresetDir) + "/" + name + ".json") {
        REQUIRE(platform::fsWriteAtomic(path.c_str(), doc.data(), doc.size()));
    }
    ~PresetFile() { platform::fsRemove(path.c_str()); }
};

/// A member and its group's sync for a look case, the markers loaded; chunks heard by index.
struct LookCase {
    Member m;
    RecordingSync sync{*m.devices};
    LookCase() { sync.tick(*m.devices, m.saved, sizeof(m.saved)); }
    ~LookCase() { sync.close(); }
    /// Hear chunks `indices` of `doc`, look `name` stamped `hlc`.
    void hear(const char* name, uint64_t hlc, const std::string& doc, std::initializer_list<uint32_t> indices) {
        for (uint32_t i : indices) { const auto c = lookChunk(m.id(), name, hlc, doc, i); sync.handle(*m.devices, c.data(), c.size()); }
    }
};

/// Read and apply what a member has: the 20 ms read, then the frame boundary where a live document applies.
void settleLook(RecordingSync& sync, Member& m) {
    for (int i = 0; i < 3; i++) {
        sync.receive(*m.devices);
        m.scheduler.preparePending();
    }
}
}  // namespace

TEST_CASE("a look applied here goes out as its document in chunks, to every group held") {
    Member m;
    const PresetFile preset("sunset", speedDoc());
    {
        RecordingSync sync(*m.devices);
        sync.tick(*m.devices, m.saved, sizeof(m.saved));
        sync.sent.clear();
        sync.lookApplied("sunset");
        const auto chunks = sync.to(SharedControls::kChunkAddress);
        REQUIRE(chunks.size() == 1);   // a small preset is one chunk
        osc::Reader r(chunks[0].data(), chunks[0].size(), SharedControls::kChunkAddress);
        CHECK(std::string(r.str()) == m.id());
        CHECK(std::string(r.str()) == "sunset");
        r.i64(); r.i32();
        CHECK(r.i32() == speedDoc().size());
        sync.close();
    }
}

TEST_CASE("a member takes a pushed look from its chunk and applies it once whole, once per stamp") {
    Member m;
    {
        RecordingSync sync(*m.devices);
        sync.tick(*m.devices, m.saved, sizeof(m.saved));
        const auto c = lookChunk(m.id(), "warm sunset", uint64_t{5000} << 16, speedDoc(), 0);
        sync.handle(*m.devices, c.data(), c.size());
        settleLook(sync, m);
        CHECK(m.lamp->speed == 33);
        m.lamp->speed = 1;
        sync.handle(*m.devices, c.data(), c.size());   // the same chunk repeated
        settleLook(sync, m);
        CHECK(m.lamp->speed == 1);
        sync.close();
    }
}

TEST_CASE("a large look whose middle chunk is lost is asked for, and applied once it arrives") {
    LookCase t;
    const std::string doc = speedDoc(2500);   // three chunks
    const uint64_t hlc = uint64_t{5000} << 16;
    t.hear("big", hlc, doc, {0, 2});
    t.sync.sent.clear();
    platform::setTestNowMs(platform::millis() + SharedControls::kSpreadMs);
    settleLook(t.sync, t.m);
    CHECK(t.m.lamp->speed == 1);   // not whole: not applied
    const auto needs = t.sync.to(SharedControls::kNeedAddress);
    REQUIRE(needs.size() == 1);
    osc::Reader r(needs[0].data(), needs[0].size(), SharedControls::kNeedAddress);
    r.str(); r.i64(); r.i32();
    CHECK(r.i32() == 0x2u);   // chunk 1 only
    t.hear("big", hlc, doc, {1});
    settleLook(t.sync, t.m);
    CHECK(t.m.lamp->speed == 33);
}

TEST_CASE("a member holding a look answers a request for its chunks at its offset, and stays quiet about one another sent first") {
    Member m;
    const std::string doc = speedDoc(1500);   // two chunks
    const uint64_t hlc = uint64_t{5000} << 16;
    {
        RecordingSync sync(*m.devices);
        sync.tick(*m.devices, m.saved, sizeof(m.saved));
        for (uint32_t i : {0u, 1u}) { const auto c = lookChunk(m.id(), "duo", hlc, doc, i); sync.handle(*m.devices, c.data(), c.size()); }
        settleLook(sync, m);
        sync.sent.clear();
        const auto need = lookNeed(m.id(), hlc, 0x3u);
        sync.handle(*m.devices, need.data(), need.size());
        const auto c0 = lookChunk(m.id(), "duo", hlc, doc, 0);   // another member answers chunk 0 first
        sync.handle(*m.devices, c0.data(), c0.size());
        platform::setTestNowMs(platform::millis() + SharedControls::kSpreadMs);
        settleLook(sync, m);
        const auto chunks = sync.to(SharedControls::kChunkAddress);
        REQUIRE(chunks.size() == 1);
        osc::Reader r(chunks[0].data(), chunks[0].size(), SharedControls::kChunkAddress);
        r.str(); r.str(); r.i64(); r.i32(); r.i32(); r.i32(); r.i64();
        CHECK(r.i32() == 1u);   // only the chunk nobody sent
        sync.close();
    }
}

TEST_CASE("a look whose document does not match its digest is refused, said so, and asked for again") {
    Member m;
    {
        RecordingSync sync(*m.devices);
        sync.tick(*m.devices, m.saved, sizeof(m.saved));
        const auto c = lookChunk(m.id(), "bad", uint64_t{5000} << 16, speedDoc(), 0, 0x12345678u);
        sync.handle(*m.devices, c.data(), c.size());
        settleLook(sync, m);
        CHECK(m.lamp->speed == 1);
        REQUIRE(sync.sharedNote() != nullptr);
        CHECK(std::string(sync.sharedNote()) == "look bad damaged");
        sync.close();
    }
}

TEST_CASE("a look reaching past the effects is refused whole, said so, so membership sets no network setting on a member") {
    Member m;
    {
        RecordingSync sync(*m.devices);
        sync.tick(*m.devices, m.saved, sizeof(m.saved));
        const std::string doc = R"({"$slot":3,"Lamp":{"speed":33},"WiFi":{"channel":11}})";
        const auto c = lookChunk(m.id(), "sneaky", uint64_t{5000} << 16, doc, 0);
        sync.handle(*m.devices, c.data(), c.size());
        settleLook(sync, m);
        CHECK(m.radio->channel == 6);
        CHECK(m.lamp->speed == 1);   // the whole look stays out, the effect's part included
        REQUIRE(sync.sharedNote() != nullptr);
        CHECK(std::string(sync.sharedNote()) == "look sneaky sets more than effects");
        sync.close();
    }
}

TEST_CASE("a look with the file's own pad and a look module's controls applies, as a preset applied here does") {
    Member m;
    {
        RecordingSync sync(*m.devices);
        sync.tick(*m.devices, m.saved, sizeof(m.saved));
        const auto c = lookChunk(m.id(), "padded", uint64_t{5000} << 16, R"({"$slot":3,"Lamp":{"speed":33}})", 0);
        sync.handle(*m.devices, c.data(), c.size());
        settleLook(sync, m);
        CHECK(m.lamp->speed == 33);
        sync.close();
    }
}

// The Reviewer's case: a large look on a board without room for it, its next chunk writing through a null document.
TEST_CASE("a look memory refuses leaves its slot empty, so its chunks are dropped safely and the next copy tries afresh") {
    LookCase t;
    const std::string doc = speedDoc(2500);   // three chunks
    const uint64_t hlc = uint64_t{5000} << 16;
    platform::setTestAllocRefusedFrom(2000);   // the document's 2501 bytes refused, every smaller allocation allowed
    t.hear("big", hlc, doc, {0, 1});           // the second of the same stamp: no document to write into
    platform::setTestAllocRefusedFrom(0);
    REQUIRE(t.sync.sharedNote() != nullptr);
    CHECK(std::string(t.sync.sharedNote()) == "look big too large");
    t.hear("big", hlc, doc, {0, 1, 2});
    settleLook(t.sync, t.m);
    CHECK(t.m.lamp->speed == 33);   // with room again, the same look is taken whole
}

TEST_CASE("an advertisement of a newer look makes a member ask for all of it") {
    Member m;
    const std::string doc = speedDoc(1500);
    {
        RecordingSync sync(*m.devices);
        sync.tick(*m.devices, m.saved, sizeof(m.saved));
        const auto a = lookAdvert(m.id(), "duo", uint64_t{5000} << 16, static_cast<uint32_t>(doc.size()), fnv1a(doc.data(), doc.size()), 0x3u);
        sync.handle(*m.devices, a.data(), a.size());
        sync.sent.clear();
        platform::setTestNowMs(platform::millis() + SharedControls::kSpreadMs);
        settleLook(sync, m);
        const auto needs = sync.to(SharedControls::kNeedAddress);
        REQUIRE(needs.size() == 1);
        osc::Reader r(needs[0].data(), needs[0].size(), SharedControls::kNeedAddress);
        r.str(); r.i64(); r.i32();
        CHECK(r.i32() == 0x3u);
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

TEST_CASE("a full group keeps its newest values, so two members holding the same newest set agree and stop asking for repeats") {
    Member m;
    SharedControls a(*m.devices), b(*m.devices);
    const uint64_t t0 = uint64_t{5000} << 16;
    // A holds the oldest full table, B the same values one newer: both end on B's newest set.
    for (int i = 0; i < SharedControls::kMaxPerGroup; i++) {
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
    CHECK(a.holds("Lamp.p32", 32));   // the newest is held, so it stays out of the outgoing changes
    CHECK_FALSE(a.holds("Lamp.p0", 0));
}

TEST_CASE("each group keeps its own share of the table, so a device in two groups holds the same values as a member of one") {
    Member m;
    SharedControls both(*m.devices), one(*m.devices);
    const uint32_t g1 = SharedControls::keyOf("g1"), g2 = SharedControls::keyOf("g2");
    const uint64_t t0 = uint64_t{5000} << 16;
    for (int i = 0; i < SharedControls::kMaxPerGroup; i++) {
        char param[24];
        std::snprintf(param, sizeof(param), "Lamp.p%d", i);
        both.setLocal(g1, param, i, Stamp{t0 + i, 7});
        one.setLocal(g1, param, i, Stamp{t0 + i, 7});
    }
    for (int i = 0; i <= SharedControls::kMaxPerGroup; i++) {   // the second group fills and evicts within itself, later than every value of the first
        char param[24];
        std::snprintf(param, sizeof(param), "Lamp.q%d", i);
        both.setLocal(g2, param, i, Stamp{t0 + 1000 + i, 7});
    }
    CHECK(both.digest(g1) == one.digest(g1));
}

TEST_CASE("a group that leaves this device takes its values and look with it, so nine groups made and dropped leave room for a look") {
    Member m;
    {
        RecordingSync sync(*m.devices);
        sync.tick(*m.devices, m.saved, sizeof(m.saved));
        for (int i = 0; i < 9; i++) {
            platform::setTestNowMs(2000 + 1000 * i);   // each group its own moment
            auto* g = new DeviceGroup;
            m.devices->addChild(g);
            g->setName("Passing");
            g->setup();
            sync.tick(*m.devices, m.saved, sizeof(m.saved));
            const auto look = lookAdvert(g->groupId(), "warm", static_cast<uint64_t>(50000 + 1000 * i) << 16, 20, 1, 1);   // a look and a value of its own
            const auto value = valuesMessage(g->groupId(), "Lamp.speed", 5, static_cast<uint64_t>(50000 + 1000 * i) << 16);
            sync.handle(*m.devices, look.data(), look.size());
            sync.handle(*m.devices, value.data(), value.size());
            m.devices->removeChild(g);   // deleted here
            g->release();
            Scheduler::deleteTree(g);
            sync.tick(*m.devices, m.saved, sizeof(m.saved));
        }
        sync.sent.clear();
        const PresetFile preset("look", speedDoc());
        sync.lookApplied("look");
        const auto looks = sync.to(SharedControls::kChunkAddress);
        REQUIRE(looks.size() == 1);   // the Stage group's look still has its place
        osc::Reader r(looks[0].data(), looks[0].size(), SharedControls::kChunkAddress);
        CHECK(std::string(r.str()) == m.id());
        CHECK(sync.sharedNote() == nullptr);
        sync.close();
    }
}

namespace {
/// Puts this process on a fleet clock `aheadUs` ahead of its own, for a case, and back to its own clock after.
struct OnFleetClock {
    explicit OnFleetClock(int64_t aheadUs = 0) { set(aheadUs); }
    void set(int64_t aheadUs) {
        ClockDiscipline d;
        d.steer(static_cast<int64_t>(platform::uptimeUs()), aheadUs);
        FleetTime::publish(d);
    }
    ~OnFleetClock() { FleetTime::reset(); }
};
}  // namespace

TEST_CASE("a change made here on the fleet clock goes out as a bundle applying 100 ms ahead; without the clock, plain") {
    Member m;
    {
        RecordingSync sync(*m.devices);
        sync.tick(*m.devices, m.saved, sizeof(m.saved));
        OnFleetClock fleet;
        sync.sent.clear();
        REQUIRE(m.scheduler.setControl("Control", "fader1", "{\"value\":77}") == Scheduler::SetControlResult::Ok);
        REQUIRE(sync.sent.size() == 1);
        uint64_t tag = 0;
        const uint8_t* inner = nullptr;
        size_t innerLen = 0;
        REQUIRE(osc::readBundle(sync.sent[0].data(), sync.sent[0].size(), tag, inner, innerLen));
        const int64_t lead = osc::usOfTimetag(tag) - FleetTime::nowUs();
        CHECK(lead <= DeviceGroupSync::kApplyLeadUs);   // virtual time: the uptime the fleet clock reads stands still,
        CHECK(lead >= DeviceGroupSync::kApplyLeadUs - 1);   // and a timetag's fraction rounds down by under a microsecond
        CHECK(readValues(std::vector<uint8_t>(inner, inner + innerLen)).value == 77);
        sync.close();
    }
}

TEST_CASE("a timed change waits for its moment on a member on the fleet clock; one stamped too far ahead, or reaching a member without the clock, applies on arrival") {
    Member m;
    {
        RecordingSync sync(*m.devices);
        sync.tick(*m.devices, m.saved, sizeof(m.saved));   // the markers loaded, so the 20 ms read runs
        const auto p = valuesMessage(m.id(), "Lamp.speed", 42, uint64_t{5000} << 16);
        uint8_t bundle[300];
        OnFleetClock fleet;
        const size_t n = osc::writeBundle(bundle, sizeof(bundle), osc::timetagOfUs(FleetTime::nowUs() + 500000), p.data(), p.size());   // half a second ahead
        sync.handle(*m.devices, bundle, n);
        sync.receive(*m.devices);
        CHECK(m.lamp->speed != 42);   // held
        fleet.set(2000000);           // the fleet clock moves past its moment
        sync.receive(*m.devices);
        CHECK(m.lamp->speed == 42);
        const auto far = valuesMessage(m.id(), "Lamp.speed", 44, uint64_t{5500} << 16);
        const size_t f = osc::writeBundle(bundle, sizeof(bundle), osc::timetagOfUs(FleetTime::nowUs() + 10000000), far.data(), far.size());   // past the furthest hold
        sync.handle(*m.devices, bundle, f);
        CHECK(m.lamp->speed == 44);
        FleetTime::reset();           // off the fleet clock: on arrival, whatever the timetag says
        const auto q = valuesMessage(m.id(), "Lamp.speed", 43, uint64_t{6000} << 16);
        const size_t k = osc::writeBundle(bundle, sizeof(bundle), osc::timetagOfUs(FleetTime::nowUs() + 10000000), q.data(), q.size());
        sync.handle(*m.devices, bundle, k);
        CHECK(m.lamp->speed == 43);
        sync.close();
    }
}
