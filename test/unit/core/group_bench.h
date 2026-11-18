#pragma once

/// Helpers the device-group tests share: a presence packet carrying a MAC, and Devices with Discovery and one group under it.

#include "doctest.h"
#include "core/system/Devices.h"
#include "core/system/DeviceGroup.h"
#include "core/system/DevicesModule.h"
#include "core/system/WledPacket.h"
#include "platform/platform.h"

#include <cstdio>

namespace group_bench {

/// Restores the real clock when a test ends.
struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } };
/// Restores the real address when a test ends.
struct AddressGuard { ~AddressGuard() { mm::platform::setTestEthIPv4(nullptr); } };

/// A MoonLight presence packet from `ip` named `name`, carrying its MAC as MoonLight's tail does.
inline void hearWithMac(mm::DevicesModule& dev, const uint8_t ip[4], const char* name, const uint8_t mac[6]) {
    uint8_t pkt[80] = {};
    mm::WledPacket::build(pkt, ip, name, /*boardType=*/34, /*lightsOn=*/true);
    mm::WledPacket::stampMarker(pkt, mm::WledPacket::kMmMarker);
    const size_t len = mm::WledPacket::stampTail(pkt, sizeof(pkt), mac);
    dev.injectPacketForTest(pkt, len, ip);
}

/// Devices with Discovery and the group `name` under it, this device at 192.168.1.2, Discovery live so the group reads it.
struct Bench {
    ClockGuard clock;
    AddressGuard address;
    mm::Devices devices;
    mm::DevicesModule* discovery = new mm::DevicesModule;
    mm::DeviceGroup* group = new mm::DeviceGroup;
    explicit Bench(const char* name) {
        const uint8_t self[4] = {192, 168, 1, 2};
        mm::platform::setTestEthIPv4(self);
        mm::platform::setTestNowMs(1000);
        devices.addChild(discovery);
        devices.addChild(group);
        group->setName(name);
        discovery->setup();
        discovery->applyState();   // claims the seat a group reads Discovery through
        group->setup();
    }
    ~Bench() { discovery->release(); }
    Bench(const Bench&) = delete;
    Bench& operator=(const Bench&) = delete;

    /// Add the device with this MAC to the group, as the API's MAC field does.
    void addMac(const uint8_t mac[6]) {
        char value[48];
        std::snprintf(value, sizeof(value), "{\"value\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        uint32_t id = 0;
        REQUIRE(group->addListRow(id));
        REQUIRE(group->setListRowField(id, "mac", value));
    }
};

}  // namespace group_bench
