/// @module DeviceGroup
///
/// Pins a group under Devices: members kept by their MAC through a rename and a new address, picked from Discovery's devices, marked missing when unheard, saved and restored, and named on each member's row for the map; and the definitions and deletions DeviceGroupSync carries between devices, merged whole or refused.

#include "doctest.h"
#include "discovery_rows.h"
#include "group_bench.h"
#include "core/system/Devices.h"
#include "core/system/DevicesModule.h"
#include "core/system/DeviceGroup.h"
#include "core/system/WledPacket.h"
#include "core/util/JsonSink.h"
#include "platform/platform.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using namespace mm;

namespace {

using group_bench::AddressGuard;
using group_bench::ClockGuard;
using group_bench::hearWithMac;

/// The bench with the group "Kitchen", and a member's row and the offered devices read back.
struct Bench : group_bench::Bench {
    Bench() : group_bench::Bench("Kitchen") {}
    std::string member(ListRow row) const { JsonSink s; group->writeListRow(s, row); return s.data(); }
    std::string options() const { JsonSink s; group->writeListOptionSets(s); return s.data(); }
};

const uint8_t kMacA[6] = {0x08, 0xb6, 0x1f, 0x6f, 0xbd, 0x3c};
const uint8_t kIpA[4] = {192, 168, 1, 20};

/// Add a member and pick Discovery's first offered device for it.
uint32_t pickFirst(DeviceGroup& g) {
    uint32_t id = 0;
    REQUIRE(g.addListRow(id));
    REQUIRE(g.setListRowField(id, "device", "{\"value\":0}"));
    return id;
}

}  // namespace

TEST_CASE("DeviceGroup: a new group takes an id from this device's MAC once, and keeps it") {
    Bench b;
    const std::string id = b.group->groupId();
    CHECK(id.size() == 20);
    b.group->setup();
    CHECK(id == b.group->groupId());
}

// After a restart the uptime starts over, so a group made then could take the id of one deleted before it, and its own marker would remove it.
TEST_CASE("DeviceGroup: a new group takes no id a deletion marker names, its moment the fleet clock's") {
    Bench b;
    platform::setTestNowMs(2000);
    uint8_t mac[6];
    platform::getMacAddress(mac);
    char id[DeviceGroup::kIdLen + 1];
    std::snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x%08lx", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], 2000ul);
    b.discovery->defineControls();
    for (uint8_t i = 0; i < b.discovery->controls().count(); i++)   // Discovery's saved markers name the id a group made now would take
        if (std::strcmp(b.discovery->controls()[i].name, "deletedGroups") == 0) std::snprintf(static_cast<char*>(b.discovery->controls()[i].ptr), 400, "%s:1:1;", id);
    b.discovery->tick1s();   // the markers load
    REQUIRE(b.discovery->groupDeleted(id));
    auto* hall = new DeviceGroup;
    b.devices.addChild(hall);
    hall->setName("Hall");
    hall->setup();
    const std::string taken = hall->groupId();
    CHECK(taken != id);
    CHECK(taken.substr(0, 12) == std::string(id).substr(0, 12));   // still this device's MAC
    CHECK(taken.substr(12) == "000007d1");   // the next millisecond
}

TEST_CASE("DeviceGroup: Devices accepts a group as a child, and a group is a group") {
    Devices devices;
    DeviceGroup g;
    CHECK(devices.acceptsRole(g.role()));
    CHECK(std::strcmp(roleName(g.role()), "devicegroup") == 0);
}

TEST_CASE("DeviceGroup: a member is picked from Discovery's devices and kept by its MAC") {
    Bench b;
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    CHECK(b.options().find("MM-A (192.168.1.20)") != std::string::npos);
    pickFirst(*b.group);
    CHECK(b.group->hasMember(kMacA));
    CHECK(b.member(0).find("\"device\":\"MM-A\"") != std::string::npos);
    CHECK(b.member(0).find("\"status\":\"present\"") != std::string::npos);
}

TEST_CASE("DeviceGroup: a member keeps its place through a rename and a new address") {
    Bench b;
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    pickFirst(*b.group);
    const uint8_t moved[4] = {192, 168, 1, 77};
    hearWithMac(*b.discovery, moved, "MM-Hall", kMacA);   // renamed and on a new lease, the same board
    CHECK(b.group->hasMember(kMacA));
    CHECK(b.member(0).find("\"device\":\"MM-Hall\"") != std::string::npos);
}

TEST_CASE("DeviceGroup: a member unheard for three announcements is missing, and its row says so for the map") {
    Bench b;
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    pickFirst(*b.group);
    platform::setTestNowMs(1000 + 31000);   // past three of MoonLight's ten-second announcements
    CHECK(b.member(0).find("\"status\":\"missing\"") != std::string::npos);
    const std::string row = discovery_rows::rowAt(*b.discovery, "192.168.1.20", /*detail=*/true);
    CHECK(row.find("\"groups\":[{\"id\":\"" + std::string(b.group->groupId()) + "\",\"name\":\"Kitchen\"}]") != std::string::npos);
    CHECK(row.find("\"missing\":true") != std::string::npos);
}

TEST_CASE("DeviceGroup: a member heard running MoonBase is missing, since it follows no group, and present again once its app runs") {
    Bench b;
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    pickFirst(*b.group);
    CHECK(b.member(0).find("\"status\":\"present\"") != std::string::npos);
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA, WledPacket::kMbMarker);   // restarted into MoonBase: heard, but not as MoonLight
    CHECK(b.member(0).find("\"status\":\"missing\"") != std::string::npos);
    CHECK(discovery_rows::rowAt(*b.discovery, "192.168.1.20", /*detail=*/true).find("\"missing\":true") != std::string::npos);
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);   // its app back
    CHECK(b.member(0).find("\"status\":\"present\"") != std::string::npos);
}

TEST_CASE("DeviceGroup: a device is a member once, and a MAC from the API joins as the picker's index does") {
    Bench b;
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    pickFirst(*b.group);
    uint32_t second = 0;
    REQUIRE(b.group->addListRow(second));
    CHECK_FALSE(b.group->setListRowField(second, "mac", "{\"value\":\"08:b6:1f:6f:bd:3c\"}"));   // a member already
    const uint8_t macB[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    CHECK(b.group->setListRowField(second, "mac", "{\"value\":\"10:20:30:40:50:60\"}"));
    CHECK(b.group->hasMember(macB));
    CHECK_FALSE(b.group->setListRowField(second, "mac", "{\"value\":\"not a mac\"}"));
    // The picker's field takes its index only: a MAC sent to it is refused rather than read as its leading digits, "00:4b:…" as the first device.
    const uint8_t macC[6] = {0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c};
    const uint8_t ipC[4] = {192, 168, 1, 30};
    hearWithMac(*b.discovery, ipC, "MM-C", macC);   // a device on offer, no member yet
    uint32_t third = 0;
    REQUIRE(b.group->addListRow(third));
    for (const char* sent : {"{\"value\":\"00:4b:12:bd:8e:14\"}", "{\"value\":\"01:4b:12:bd:8e:14\"}", "{\"value\":\"02:4b:12:bd:8e:14\"}", "{\"value\":\"1\"}"})
        CHECK_FALSE(b.group->setListRowField(third, "device", sent));
    CHECK_FALSE(b.group->hasMember(macC));
}

TEST_CASE("DeviceGroup: the members survive a save and a restore, the name kept for a member Discovery dropped") {
    Bench b;
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    pickFirst(*b.group);
    JsonSink saved;
    saved.append("{\"members\":[");
    b.group->writeListRowSaved(saved, 0);
    saved.append("]}");
    DeviceGroup restored;
    CHECK(restored.restoreList(saved.data(), "members"));
    CHECK(restored.listRowCount() == 1);
    CHECK(restored.hasMember(kMacA));
    b.discovery->release();   // Discovery drops every row, so the name shown is the one restored
    JsonSink row;
    restored.writeListRow(row, 0);
    CHECK(std::string(row.data()).find("\"device\":\"MM-A\"") != std::string::npos);
}

TEST_CASE("DeviceGroup: a list write that is not a list leaves the members as they were, and an empty list clears them") {
    Bench b;
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    pickFirst(*b.group);
    CHECK_FALSE(b.group->restoreList("{\"members\":false}", "members"));
    CHECK(b.group->hasMember(kMacA));
    CHECK(b.group->restoreList("{\"members\":[]}", "members"));
    CHECK_FALSE(b.group->hasMember(kMacA));
}

TEST_CASE("DeviceGroup: a restored list memory cannot hold is refused whole, the members kept") {
    struct AllocGuard { ~AllocGuard() { platform::setTestAllocRefusedFrom(0); } } alloc;
    Bench b;
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    pickFirst(*b.group);
    std::string json = "{\"members\":[";
    for (int i = 0; i < 64; i++) {
        char row[80];
        std::snprintf(row, sizeof(row), "%s{\"mac\":\"10:00:00:00:00:%02x\",\"stamp\":\"10000:1\"}", i ? "," : "", i);
        json += row;
    }
    json += "]}";
    platform::setTestAllocRefusedFrom(1024);   // the first rows fit the room held, the rest need more than memory gives
    CHECK_FALSE(b.group->restoreList(json.c_str(), "members"));
    platform::setTestAllocRefusedFrom(0);
    CHECK(b.group->listRowCount() == 1);
    CHECK(b.group->hasMember(kMacA));
}

TEST_CASE("DeviceGroup: this device keeps one row through a new address, so a group lists it once and present") {
    ClockGuard clock;
    AddressGuard address;
    const uint8_t first[4] = {192, 168, 1, 2}, moved[4] = {192, 168, 1, 9};
    platform::setTestEthIPv4(first);
    platform::setTestNowMs(1000);
    DevicesModule dev;
    dev.tick1s();
    platform::setTestEthIPv4(moved);
    platform::setTestNowMs(1000 + 31000);   // past three announcements, so a row left at the old address reads missing
    dev.tick1s();
    uint8_t self[6];
    platform::getMacAddress(self);
    CHECK(discovery_rows::rowAt(dev, "192.168.1.2").empty());
    CHECK_FALSE(discovery_rows::rowAt(dev, "192.168.1.9").empty());
    CHECK(dev.present(self));
    dev.release();
}

TEST_CASE("DeviceGroup: a deleted member leaves the group") {
    Bench b;
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    const uint32_t id = pickFirst(*b.group);
    CHECK(b.group->deleteListRow(id));
    CHECK(b.group->listRowCount() == 0);
    CHECK_FALSE(b.group->hasMember(kMacA));
}

// --- Replication: two devices' copies of one group, exchanged as the network would carry them ---------------------------------

#include "core/services/OscPacket.h"
#include "core/system/DeviceGroupSync.h"
#include "core/module/Scheduler.h"
#include "core/util/ModuleFactory.h"
#include "module_types.h"

namespace {

/// One device's copy of a group: its own Devices container, so its own clock.
struct Copy {
    Devices devices;
    DeviceGroup* group = new DeviceGroup;
    /// The device that makes the group when `id` is null; otherwise another device's copy of it, set up first and then given the maker's id, as a received copy's document creates it.
    explicit Copy(const char* id = nullptr) {
        devices.addChild(group);
        group->setName("Kitchen");
        group->defineControls();
        group->setup();
        if (!id) return;
        for (uint8_t i = 0; i < group->controls().count(); i++)
            if (std::strcmp(group->controls()[i].name, "id") == 0) std::snprintf(static_cast<char*>(group->controls()[i].ptr), 21, "%s", id);
        group->onControlChanged("id");
    }
    /// This copy renamed by its user, "Kitchen" to `to`, at `ms`: an edit stamped then.
    void rename(const char* to, uint32_t ms) { platform::setTestNowMs(ms); group->setName(to); group->onModuleRenamed("Kitchen", to); }
    void add(const char* mac) { uint32_t row = 0; REQUIRE(group->addListRow(row)); REQUIRE(group->setListRowField(row, "mac", (std::string("{\"value\":\"") + mac + "\"}").c_str())); }
    /// Every chunk of this copy, merged into `to`; true when any changed it.
    bool sendTo(Copy& to, const char** wantName = nullptr) {
        bool changed = false;
        uint8_t buf[1472];
        for (uint16_t k = 0; k < group->chunkCount(16); k++) {
            const size_t len = group->writeChunk(buf, sizeof(buf), k, 16);
            REQUIRE(len > 0);
            osc::Reader r(buf, len, DeviceGroup::kAddress);
            REQUIRE(std::string(r.str()) == group->groupId());
            const char* want = nullptr;
            changed |= to.group->merge(r, want);
            if (wantName) *wantName = want;
        }
        return changed;
    }
};

}  // namespace

TEST_CASE("DeviceGroup: two members added at once on two devices both stay once the copies meet") {
    ClockGuard clock;
    Copy a;
    Copy b(a.group->groupId());
    a.add("10:00:00:00:00:01");
    b.add("10:00:00:00:00:02");
    a.sendTo(b);
    b.sendTo(a);
    const uint8_t m1[6] = {0x10, 0, 0, 0, 0, 1}, m2[6] = {0x10, 0, 0, 0, 0, 2};
    CHECK(a.group->hasMember(m1)); CHECK(a.group->hasMember(m2));
    CHECK(b.group->hasMember(m1)); CHECK(b.group->hasMember(m2));
    CHECK(a.group->digest() == b.group->digest());   // the same definition, held in another order
}

TEST_CASE("DeviceGroup: a removal outlives a late copy that still holds the member") {
    ClockGuard clock;
    Copy a;
    Copy b(a.group->groupId());
    a.add("10:00:00:00:00:01");
    a.sendTo(b);
    platform::setTestNowMs(5000);
    REQUIRE(a.group->deleteListRow(1));   // removed on A, a newer stamp
    CHECK_FALSE(b.sendTo(a));             // B's older copy adds nothing back
    const uint8_t m1[6] = {0x10, 0, 0, 0, 0, 1};
    CHECK_FALSE(a.group->hasMember(m1));
    a.sendTo(b);
    CHECK_FALSE(b.group->hasMember(m1));  // and B takes the removal
    CHECK(a.group->listSavedRowCount() == 1);   // the marker is saved, so a reboot keeps it
}

TEST_CASE("DeviceGroup: a member replaced in its row stays removed though a copy still holds it") {
    ClockGuard clock;
    Copy a;
    Copy b(a.group->groupId());
    a.add("10:00:00:00:00:01");
    a.sendTo(b);
    platform::setTestNowMs(5000);
    REQUIRE(a.group->setListRowField(1, "mac", "{\"value\":\"10:00:00:00:00:02\"}"));   // the row's device changed in place
    const uint8_t m1[6] = {0x10, 0, 0, 0, 0, 1}, m2[6] = {0x10, 0, 0, 0, 0, 2};
    CHECK_FALSE(b.sendTo(a));   // B's older copy leaves the replaced member removed
    CHECK_FALSE(a.group->hasMember(m1));
    CHECK(a.group->hasMember(m2));
    CHECK(a.group->listRowCount() == 1);
    CHECK(a.group->listSavedRowCount() == 2);   // the replaced member's marker is saved
    CHECK_FALSE(a.group->setListRowField(2, "mac", "{\"value\":\"10:00:00:00:00:03\"}"));   // a marker is no row to edit
    a.sendTo(b);
    CHECK_FALSE(b.group->hasMember(m1));
    CHECK(b.group->hasMember(m2));
    CHECK(a.group->digest() == b.group->digest());
    platform::setTestNowMs(6000);
    REQUIRE(a.group->setListRowField(1, "mac", "{\"value\":\"10:00:00:00:00:01\"}"));   // and back: the marker swaps sides
    CHECK(a.group->hasMember(m1));
    CHECK_FALSE(a.group->hasMember(m2));
    CHECK(a.group->listSavedRowCount() == 2);
    a.sendTo(b);
    CHECK(b.group->hasMember(m1));
    CHECK_FALSE(b.group->hasMember(m2));
}

TEST_CASE("DeviceGroup: a rename on one device is the name on the other, the newer stamp winning") {
    ClockGuard clock;
    Copy a;
    Copy b(a.group->groupId());
    a.rename("Hall", 3000);
    const char* want = nullptr;
    a.sendTo(b, &want);
    REQUIRE(want);
    CHECK(std::string(want) == "Hall");
    platform::setTestNowMs(4000);
    b.group->setName("Hall");                 // the rename the merge asked for, as the sync applies it
    b.group->onModuleRenamed("Kitchen", "Hall");
    CHECK(a.group->digest() == b.group->digest());   // B keeps A's stamp rather than writing one of its own
}

TEST_CASE("DeviceGroup: a rename the tree refuses still converges: the shared name holds it, and the copies agree") {
    ClockGuard clock;
    Copy a;
    Copy b(a.group->groupId());
    a.sendTo(b);
    a.rename("Hall", 3000);
    const char* want = nullptr;
    CHECK(a.sendTo(b, &want));            // B's tree has "Hall" taken elsewhere: B keeps its module name
    REQUIRE(want);
    CHECK(std::string(b.group->groupName()) == "Hall");
    CHECK(std::string(b.group->name()) == "Kitchen");
    CHECK(a.group->digest() == b.group->digest());
    CHECK_FALSE(a.sendTo(b));             // the next copy changes nothing, so neither side resets Trickle
}

TEST_CASE("Hlc: a counter past 16 bits carries into the milliseconds, so readings keep growing") {
    ClockGuard clock;
    platform::setTestNowMs(1000);
    Hlc c;
    Stamp prev = c.now(1);
    for (int i = 0; i < 70000; i++) c.observe(c.last());   // the clock standing still while copies arrive
    const Stamp next = c.now(1);
    CHECK(next.newerThan(prev));
    CHECK(next.hlc > (static_cast<uint64_t>(1000) << 16) + 0xFFFF);   // carried into the millisecond part
}

TEST_CASE("DeviceGroup: a received copy takes the maker's name stamp, not one of its own") {
    ClockGuard clock;
    Copy a;
    platform::setTestNowMs(2000);   // the copy is made later than the group, so a stamp of its own would be newer
    Copy b(a.group->groupId());
    a.sendTo(b);
    CHECK(a.group->digest() == b.group->digest());
}

TEST_CASE("DeviceGroup: Trickle sends at once after an edit, and stays quiet while a copy that agrees is heard") {
    ClockGuard clock;
    Copy a;
    Copy b(a.group->groupId());
    a.add("10:00:00:00:00:01");
    a.group->tick1s();
    CHECK(a.group->sendDue());            // an edit brings the interval back to one second
    a.sendTo(b);
    b.sendTo(a);                          // agreeing copies heard
    bool sent = false;
    for (int s = 0; s < 4; s++) { a.group->tick1s(); sent |= a.group->sendDue(); }
    CHECK_FALSE(sent);                    // heard the same: silence, the interval doubling
}

TEST_CASE("DeviceGroupSync: a device a group names creates it, and a deletion marker removes it") {
    ClockGuard clock;
    registerModuleTypes();
    Scheduler s;
    auto* devices = ModuleFactory::create("Devices");
    devices->setName("Devices");
    s.addModule(devices);
    s.setup();
    {   // the sync registers its buffers with Devices, so it ends before the release frees Devices, as the module's own member does
        DeviceGroupSync sync(*devices);

        Copy origin;
        const std::string kId = origin.group->groupId();
        uint8_t self[6];
        platform::getMacAddress(self);
        char mac[18];
        std::snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", self[0], self[1], self[2], self[3], self[4], self[5]);
        origin.add(mac);   // this device is a member
        uint8_t buf[1472];
        size_t len = origin.group->writeChunk(buf, sizeof(buf), 0, 16);
        sync.handle(*devices, buf, len);
        REQUIRE(devices->childCount() == 1);
        CHECK(static_cast<DeviceGroup*>(devices->child(0))->groupId() == kId);
        CHECK(std::string(devices->child(0)->name()) == "Kitchen");

        platform::setTestNowMs(60000);   // deleted later than every edit
        osc::Writer w(buf, sizeof(buf), DeviceGroupSync::kDeletedAddress, ",shi");
        w.str(kId.c_str());
        w.i64(static_cast<uint64_t>(60000) << 16);
        w.i32(1);
        sync.handle(*devices, buf, w.size());
        CHECK(devices->childCount() == 0);
        CHECK(sync.deleted(kId.c_str()));
        len = origin.group->writeChunk(buf, sizeof(buf), 0, 16);
        sync.handle(*devices, buf, len);   // a late copy brings nothing back
        CHECK(devices->childCount() == 0);
        sync.close();                      // a release frees the markers: the next tick reloads them from the saved text
        CHECK_FALSE(sync.deleted(kId.c_str()));
    }
    s.release();
}

TEST_CASE("Hlc: a reading with no room above it is ignored, so the clock never wraps") {
    ClockGuard clock;
    platform::setTestNowMs(1000);
    Hlc c;
    const Stamp prev = c.now(1);
    c.observe(UINT64_MAX);   // a corrupt or crafted stamp
    CHECK(c.now(1).newerThan(prev));
}

TEST_CASE("Hlc: a clock driven to the top by a crafted stamp still stamps edits every peer accepts") {
    ClockGuard clock;
    platform::setTestNowMs(1000);
    Hlc c;
    c.observe(Stamp::kMaxHlc);   // valid, and as high as a reading goes
    CHECK(c.now(1).valid());
    CHECK(c.now(1).valid());
    CHECK(c.now(1).hlc == Stamp::kMaxHlc);
}

TEST_CASE("Stamp: a saved stamp reads whole, and anything else is no stamp") {
    Stamp s;
    CHECK(Stamp::parse("1388:2a", s));
    CHECK(s.hlc == 0x1388);
    CHECK(s.writer == 0x2a);
    CHECK_FALSE(Stamp::parse(":1", s));             // no reading
    CHECK_FALSE(Stamp::parse("1388", s));           // no writer
    CHECK_FALSE(Stamp::parse("1388:", s));
    CHECK_FALSE(Stamp::parse("1388:2a junk", s));
    CHECK_FALSE(Stamp::parse("1388:100000000", s));   // a writer wider than 32 bits
    CHECK_FALSE(Stamp::parse("8000000000000000:1", s));   // past the largest valid reading
}

TEST_CASE("DeviceGroup: a chunk cut short anywhere changes nothing, and the whole chunk merges") {
    ClockGuard clock;
    Copy a;
    Copy b(a.group->groupId());
    a.rename("Hall", 3000);   // a newer name, which a cut chunk applies no more than its members
    a.add("10:00:00:00:00:01");
    a.add("10:00:00:00:00:02");
    uint8_t buf[1472];
    const size_t len = a.group->writeChunk(buf, sizeof(buf), 0, 16);
    REQUIRE(len > 0);
    const uint32_t before = b.group->digest();
    unsigned merged = 0;
    for (size_t cut = 0; cut < len; cut++) {
        osc::Reader r(buf, cut, DeviceGroup::kAddress);
        r.str();   // the id, which the sync reads first
        const char* want = nullptr;
        if (b.group->merge(r, want) || want) merged++;
    }
    CHECK(merged == 0);
    CHECK(b.group->digest() == before);
    osc::Reader r(buf, len, DeviceGroup::kAddress);
    r.str();
    const char* want = nullptr;
    CHECK(b.group->merge(r, want));
    CHECK(b.group->digest() == a.group->digest());
}

TEST_CASE("DeviceGroup: a member stamped with no room above it is refused, since no later edit could beat it") {
    ClockGuard clock;
    Copy b;
    uint8_t buf[256];
    osc::Writer w(buf, sizeof(buf), DeviceGroup::kAddress, ",sshiishiis");
    w.str(b.group->groupId()); w.str("Kitchen"); w.i64(1); w.i32(1); w.i32(0);
    w.str("100000000001"); w.i64(UINT64_MAX); w.i32(1); w.i32(0); w.str("A");
    osc::Reader r(buf, w.size(), DeviceGroup::kAddress);
    r.str();
    const char* want = nullptr;
    CHECK_FALSE(b.group->merge(r, want));
    const uint8_t m1[6] = {0x10, 0, 0, 0, 0, 1};
    CHECK_FALSE(b.group->hasMember(m1));
}

namespace {

int g_saves = 0;   ///< saves asked for through a Scheduler's dirty hook

}  // namespace

TEST_CASE("DeviceGroup: a received change is saved soon, and a repeated copy asks for no save") {
    ClockGuard clock;
    Scheduler s;
    s.setNoteDirtyHook([] { g_saves++; });
    s.setup();
    Copy a;
    Copy b(a.group->groupId());
    a.add("10:00:00:00:00:01");
    b.group->clearDirty();
    g_saves = 0;
    CHECK(a.sendTo(b));
    CHECK(b.group->dirty());
    CHECK(g_saves > 0);
    g_saves = 0;
    CHECK_FALSE(a.sendTo(b));
    CHECK(g_saves == 0);
}

// --- The sync: definitions and deletions arriving at a device's tree ------------------------------------------------------------

#include "core/module/StateDocument.h"

#include <memory>

namespace {

/// This device's MAC as an API caller writes it, and as a chunk carries it.
std::string selfMac(bool colons) {
    uint8_t m[6];
    platform::getMacAddress(m);
    char text[18];
    std::snprintf(text, sizeof(text), colons ? "%02x:%02x:%02x:%02x:%02x:%02x" : "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return text;
}

/// A definition of one member, written by hand, so its id and stamps can be what no device writes.
size_t rawChunk(uint8_t* buf, size_t cap, const char* id, const char* name, const char* memberHex, uint64_t hlc, bool removed) {
    osc::Writer w(buf, cap, DeviceGroup::kAddress, ",sshiishiis");
    w.str(id); w.str(name); w.i64(hlc); w.i32(1); w.i32(0);
    w.str(memberHex); w.i64(hlc); w.i32(1); w.i32(removed ? 1 : 0); w.str("");
    return w.size();
}

/// One device's tree, Devices under a Scheduler, and the sync carrying definitions and deletions into it.
struct Tree {
    struct BindGuard { ~BindGuard() { platform::setTestBindFails(false); } };
    ClockGuard clock;
    BindGuard bind;
    Scheduler s;
    MoonModule* devices = nullptr;
    std::unique_ptr<DeviceGroupSync> sync;   ///< ends before the release frees Devices, which holds its buffers
    char saved[400] = "";                    ///< the markers' saved text, Discovery's hidden control on a device
    Tree() {
        registerModuleTypes();
        devices = ModuleFactory::create("Devices");
        devices->setName("Devices");
        s.addModule(devices);
        s.setNoteDirtyHook([] { g_saves++; });
        s.setup();
        sync = std::make_unique<DeviceGroupSync>(*devices);
        tick();   // the first second loads the saved markers, before anything arrives, as on a device
        g_saves = 0;
    }
    ~Tree() { sync.reset(); s.release(); }
    /// Every chunk of a copy, arriving here.
    void hear(Copy& from) {
        uint8_t buf[1472];
        for (uint16_t k = 0; k < from.group->chunkCount(16); k++) sync->handle(*devices, buf, from.group->writeChunk(buf, sizeof(buf), k, 16));
    }
    /// A deletion marker for `id`, arriving here.
    void hearDeletion(const char* id, uint64_t hlc) {
        uint8_t buf[128];
        osc::Writer w(buf, sizeof(buf), DeviceGroupSync::kDeletedAddress, ",shi");
        w.str(id); w.i64(hlc); w.i32(1);
        sync->handle(*devices, buf, w.size());
    }
    /// One second of the sync with its socket refused, so nothing reaches the network.
    void tick() {
        platform::setTestBindFails(true);
        sync->tick(*devices, saved, sizeof(saved));
    }
    DeviceGroup* byId(const char* id) {
        for (uint8_t c = 0; c < devices->childCount(); c++)
            if (devices->child(c)->role() == ModuleRole::DeviceGroup && std::strcmp(static_cast<DeviceGroup*>(devices->child(c))->groupId(), id) == 0)
                return static_cast<DeviceGroup*>(devices->child(c));
        return nullptr;
    }
};

const char* const kOtherId = "0123456789abcdef0123";   ///< a group id no copy in these tests holds

}  // namespace

TEST_CASE("DeviceGroupSync: a definition whose id is not a group id creates nothing and edits no group") {
    Tree t;
    Copy origin;
    origin.add(selfMac(true).c_str());
    t.hear(origin);   // the tree holds "Kitchen"
    REQUIRE(t.devices->childCount() == 1);
    uint8_t buf[256];
    const uint64_t later = uint64_t{1} << 62;
    t.sync->handle(*t.devices, buf, rawChunk(buf, sizeof(buf), "ab", "Kitchen", selfMac(false).c_str(), later, false));   // short, its name taken
    const std::string longer = std::string(origin.group->groupId()) + "ff";   // its first twenty digits name the group held
    t.sync->handle(*t.devices, buf, rawChunk(buf, sizeof(buf), longer.c_str(), "Kitchen", selfMac(false).c_str(), later, true));
    CHECK(t.devices->childCount() == 1);
    uint8_t self[6];
    platform::getMacAddress(self);
    CHECK(static_cast<DeviceGroup*>(t.devices->child(0))->hasMember(self));
}

TEST_CASE("DeviceGroupSync: a group arriving under a name the tree holds gets a free one, and no group held changes") {
    Tree t;
    platform::setTestNowMs(1000);
    Copy first;
    first.add(selfMac(true).c_str());
    platform::setTestNowMs(2000);
    Copy second;
    second.group->setName("Kitchen 1234");
    second.group->onModuleRenamed("Kitchen", "Kitchen 1234");
    second.add(selfMac(true).c_str());
    platform::setTestNowMs(0x11234);   // an id ending in the digits the second group's name carries
    Copy third;
    third.add(selfMac(true).c_str());
    REQUIRE(std::string(third.group->groupId()).substr(16) == "1234");
    t.hear(first);
    t.hear(second);
    t.hear(third);
    CHECK(t.devices->childCount() == 3);
    CHECK(t.byId(first.group->groupId()));
    CHECK(t.byId(second.group->groupId()));
    CHECK(t.byId(third.group->groupId()));
}

TEST_CASE("DeviceGroupSync: a rename the tree refuses keeps the module's name, and the shared name takes the new one") {
    Tree t;
    platform::setTestNowMs(1000);
    Copy kitchen;
    kitchen.add(selfMac(true).c_str());
    platform::setTestNowMs(2000);
    Copy hall;
    hall.group->setName("Hall");
    hall.group->onModuleRenamed("Kitchen", "Hall");
    hall.add(selfMac(true).c_str());
    t.hear(kitchen);
    t.hear(hall);   // the tree holds "Kitchen" and "Hall"
    platform::setTestNowMs(3000);
    kitchen.group->setName("Hall");
    kitchen.group->onModuleRenamed("Kitchen", "Hall");
    t.hear(kitchen);
    DeviceGroup* g = t.byId(kitchen.group->groupId());
    REQUIRE(g);
    CHECK(std::string(g->groupName()) == "Hall");
    CHECK(std::string(g->name()) == "Kitchen");
}

TEST_CASE("DeviceGroupSync: a marker only grows newer, and a changed one is saved") {
    Tree t;
    t.hearDeletion(kOtherId, uint64_t{5000} << 16);
    t.hearDeletion(kOtherId, uint64_t{4000} << 16);   // an older deletion of the same group
    t.devices->clearDirty();
    g_saves = 0;
    t.tick();
    char want[64];
    std::snprintf(want, sizeof(want), "%s:%" PRIx64 ":", kOtherId, uint64_t{5000} << 16);
    CHECK(std::string(t.saved).find(want) != std::string::npos);
    CHECK(t.devices->dirty());
    CHECK(g_saves > 0);
}

TEST_CASE("DeviceGroupSync: a group held under a marker goes at the next second, as a removal memory refused is retried") {
    Tree t;
    t.hearDeletion(kOtherId, uint64_t{5000} << 16);
    REQUIRE(t.sync->deleted(kOtherId));
    // The group back in the tree, as a removal memory refused leaves it, or a saved file brings it at boot.
    const std::string doc = std::string("{\"Devices\":{\"Kitchen\":{\"type\":\"DeviceGroup\",\"id\":\"") + kOtherId + "\"}}}";
    applyStateDocument(t.s, doc.c_str());
    REQUIRE(t.byId(kOtherId));
    t.tick();
    CHECK_FALSE(t.byId(kOtherId));
}

TEST_CASE("DeviceGroupSync: a deletion memory refuses a marker for leaves no marker, and nothing is written") {
    Tree t;
    struct AllocGuard { ~AllocGuard() { platform::setTestAllocRefusedFrom(0); } } alloc;
    platform::setTestAllocRefusedFrom(16);   // the markers' first block refused
    t.hearDeletion(kOtherId, uint64_t{5000} << 16);
    platform::setTestAllocRefusedFrom(0);
    CHECK_FALSE(t.sync->deleted(kOtherId));
    t.tick();
    CHECK(std::string(t.saved).empty());
    CHECK(g_saves == 0);
}

TEST_CASE("DeviceGroupSync: saved markers memory refuses load the next second, and no second runs before them") {
    Tree t;
    struct AllocGuard { ~AllocGuard() { platform::setTestAllocRefusedFrom(0); } } alloc;
    t.sync->close();   // a release: the next second reads the markers from the saved text
    std::snprintf(t.saved, sizeof(t.saved), "%s:%" PRIx64 ":1;", kOtherId, uint64_t{5000} << 16);
    platform::setTestAllocRefusedFrom(16);
    t.tick();
    CHECK_FALSE(t.sync->deleted(kOtherId));
    platform::setTestAllocRefusedFrom(0);
    t.tick();
    CHECK(t.sync->deleted(kOtherId));
}

TEST_CASE("DeviceGroupSync: a saved marker whose stamp does not read is no marker") {
    Tree t;
    t.sync->close();
    const char* const kReadable = "00112233445566778899";
    std::snprintf(t.saved, sizeof(t.saved), "%s::1;%s:1388:1;", kOtherId, kReadable);
    t.tick();
    CHECK_FALSE(t.sync->deleted(kOtherId));
    CHECK(t.sync->deleted(kReadable));
}

TEST_CASE("DeviceGroupSync: a deletion wins over an edit stamped after it, so no device keeps the group") {
    Tree t;
    platform::setTestNowMs(60000);   // the edit, later than the deletion below
    Copy origin;
    origin.add(selfMac(true).c_str());
    t.hear(origin);
    const std::string id = origin.group->groupId();
    REQUIRE(t.byId(id.c_str()));
    t.hearDeletion(id.c_str(), uint64_t{1000} << 16);
    CHECK_FALSE(t.byId(id.c_str()));
    CHECK(t.sync->deleted(id.c_str()));
}

TEST_CASE("DeviceGroupSync: the newest eight markers are kept: an older one arriving leaves them, a newer one replaces the oldest") {
    Tree t;
    char ids[10][21];
    for (int i = 0; i < 10; i++) std::snprintf(ids[i], sizeof(ids[i]), "0123456789abcdef01%02x", i);
    for (int i = 0; i < 8; i++) t.hearDeletion(ids[i], uint64_t(100 + i) << 16);
    t.hearDeletion(ids[8], uint64_t{99} << 16);   // older than every marker kept
    CHECK_FALSE(t.sync->deleted(ids[8]));
    CHECK(t.sync->deleted(ids[0]));
    t.hearDeletion(ids[9], uint64_t{108} << 16);
    CHECK(t.sync->deleted(ids[9]));
    CHECK_FALSE(t.sync->deleted(ids[0]));
    for (int i = 1; i < 8; i++) CHECK(t.sync->deleted(ids[i]));
}
