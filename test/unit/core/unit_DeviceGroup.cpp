/// @module DeviceGroup
///
/// Pins a group under Devices: members kept by their MAC through a rename and a new address, picked from Discovery's devices, marked missing when unheard, saved and restored, and named on each member's row for the map.

#include "doctest.h"
#include "discovery_rows.h"
#include "core/system/Devices.h"
#include "core/system/DevicesModule.h"
#include "core/system/DeviceGroup.h"
#include "core/system/WledPacket.h"
#include "core/util/JsonSink.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstring>
#include <string>

using namespace mm;

namespace {

struct ClockGuard { ~ClockGuard() { platform::setTestNowMs(0); } };
struct AddressGuard { ~AddressGuard() { platform::setTestEthIPv4(nullptr); } };

/// A MoonLight presence packet from `ip` named `name`, carrying its MAC as MoonLight's tail does.
void hearWithMac(DevicesModule& dev, const uint8_t ip[4], const char* name, const uint8_t mac[6]) {
    uint8_t pkt[64] = {};
    WledPacket::build(pkt, ip, name, /*boardType=*/34, /*lightsOn=*/true);
    WledPacket::stampMarker(pkt, WledPacket::kMmMarker);
    const size_t len = WledPacket::stampTail(pkt, 80, mac);
    dev.injectPacketForTest(pkt, len, ip);
}

/// Devices with Discovery and one group under it, Discovery live so the group reads it.
struct Bench {
    ClockGuard clock;
    AddressGuard address;
    Devices devices;
    DevicesModule* discovery = new DevicesModule;
    DeviceGroup* group = new DeviceGroup;
    Bench() {
        const uint8_t self[4] = {192, 168, 1, 2};
        platform::setTestEthIPv4(self);
        platform::setTestNowMs(1000);
        devices.addChild(discovery);
        devices.addChild(group);
        group->setName("Kitchen");
        discovery->setup();
        discovery->applyState();   // claims the seat a group reads Discovery through
        group->setup();
    }
    ~Bench() { discovery->release(); }
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
    JsonSink row;
    restored.writeListRow(row, 0);
    CHECK(std::string(row.data()).find("\"device\":\"MM-A\"") != std::string::npos);
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

TEST_CASE("DeviceGroup: a rename on one device is the name on the other, the newer stamp winning") {
    ClockGuard clock;
    Copy a;
    Copy b(a.group->groupId());
    platform::setTestNowMs(3000);
    a.group->setName("Hall");
    a.group->onModuleRenamed("Kitchen", "Hall");
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
    platform::setTestNowMs(3000);
    a.group->setName("Hall");
    a.group->onModuleRenamed("Kitchen", "Hall");
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
    s.release();
}
