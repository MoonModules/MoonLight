/// @module DeviceGroup
/// @also NameResolver
/// @also OscService

/// A device group's name in a receivers list: its members at their current addresses, an unheard one holding its place with none, this device left out, the bound enforced and said; and OSC sending to a member on the port it listens on.

#include "doctest.h"
#include "group_bench.h"
#include "osc_rig.h"
#include "core/system/ControlModule.h"
#include "core/system/Devices.h"
#include "core/system/DevicesModule.h"
#include "core/system/DeviceGroup.h"
#include "core/module/Scheduler.h"
#include "core/services/OscService.h"
#include "core/util/Addressing.h"
#include "core/util/NameResolver.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace mm;

namespace {

const uint8_t kMacA[6] = {0x08, 0xb6, 0x1f, 0x6f, 0xbd, 0x3c};
const uint8_t kMacB[6] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
const uint8_t kIpA[4] = {192, 168, 1, 20};
const uint8_t kIpB[4] = {192, 168, 1, 21};

using group_bench::hearWithMac;

/// The bench with the group "Stage", this device's own row listed.
struct Bench : group_bench::Bench {
    Bench() : group_bench::Bench("Stage") { discovery->tick1s(); }
    void add(const uint8_t mac[6]) { addMac(mac); }
};

/// The addresses a unicast send to `list` reaches, as dotted quads.
std::vector<std::string> sentTo(const Receiver* list, uint8_t n) {
    std::vector<std::string> out;
    sendAddressed(Addressing::Unicast, Traffic::Occasional, list, n, nullptr, [&](const uint8_t ip[4]) {
        char dotted[16];
        std::snprintf(dotted, sizeof(dotted), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        out.emplace_back(dotted);
    });
    return out;
}

}  // namespace

TEST_CASE("a group's name in a receivers list is its members at their current addresses, an unheard one holding its place with none") {
    Bench b;
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    b.add(kMacA);
    b.add(kMacB);   // a member Discovery has yet to hear
    Receiver list[kMaxReceivers];
    uint8_t n = 0;
    CHECK(resolveReceivers("Stage", list, n) == 0);
    REQUIRE(n == 2);
    CHECK(list[0].member);
    CHECK_FALSE(list[1].hasAddress());
    CHECK(sentTo(list, n) == std::vector<std::string>{"192.168.1.20"});
}

TEST_CASE("a group's name leaves this device out, and a typed address beside it keeps working") {
    Bench b;
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    uint8_t self[6];
    platform::getMacAddress(self);
    b.add(self);
    b.add(kMacA);
    Receiver list[kMaxReceivers];
    uint8_t n = 0;
    resolveReceivers("192.168.1.50, stage", list, n);   // a group's name, letter case aside as a host name's
    CHECK(sentTo(list, n) == std::vector<std::string>{"192.168.1.50", "192.168.1.20"});
}

TEST_CASE("a member on a new lease is at its new address on the next resolution") {
    Bench b;
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    b.add(kMacA);
    Receiver list[kMaxReceivers];
    uint8_t n = 0;
    resolveReceivers("Stage", list, n);
    CHECK(sentTo(list, n) == std::vector<std::string>{"192.168.1.20"});
    const uint8_t moved[4] = {192, 168, 1, 77};
    hearWithMac(*b.discovery, moved, "MM-A", kMacA);
    resolveReceivers("Stage", list, n);
    CHECK(sentTo(list, n) == std::vector<std::string>{"192.168.1.77"});
}

TEST_CASE("a member unheard for three announcements drops its address, and gets it back when heard again") {
    Bench b;
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    b.add(kMacA);
    Receiver list[kMaxReceivers];
    uint8_t n = 0;
    platform::setTestNowMs(1000 + 31000);   // past three of MoonLight's ten-second announcements
    resolveReceivers("Stage", list, n);
    CHECK(sentTo(list, n).empty());
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    resolveReceivers("Stage", list, n);
    CHECK(sentTo(list, n) == std::vector<std::string>{"192.168.1.20"});
}

TEST_CASE("receivers past the bound are left out and counted, the list holding as many as it can") {
    Bench b;
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    hearWithMac(*b.discovery, kIpB, "MM-B", kMacB);
    b.add(kMacA);
    b.add(kMacB);
    Receiver list[kMaxReceivers];
    uint8_t n = 0;
    CHECK(resolveReceivers("10.0.0.1-31, Stage", list, n) == 1);   // 31 typed, then a group of two
    CHECK(n == kMaxReceivers);
    CHECK(sentTo(list, n).back() == "192.168.1.20");
}

namespace {

/// OSC with its datagrams dropped instead of sent, so the test needs no socket.
struct QuietOsc : OscService {
    void transmit(const uint8_t*, const uint8_t*, size_t) override {}
};

/// The Stage bench with Control and OSC feedback going to the group.
struct OscBench : Bench, osc_rig::OscRig<QuietOsc> {
    OscBench() {
        REQUIRE(scheduler.setControl("Osc", "receivers", "{\"value\":\"Stage\"}") == Scheduler::SetControlResult::Ok);
    }
};

}  // namespace

TEST_CASE("OSC feedback to a group larger than the list says how many receivers were left out") {
    OscBench b;
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    hearWithMac(*b.discovery, kIpB, "MM-B", kMacB);
    b.add(kMacA);
    b.add(kMacB);
    REQUIRE(b.scheduler.setControl("Osc", "receivers", "{\"value\":\"10.0.0.1-31, Stage\"}") == Scheduler::SetControlResult::Ok);
    REQUIRE(b.osc->status() != nullptr);
    CHECK(std::strcmp(b.osc->status(), "1 receiver past 32 left out") == 0);
}

TEST_CASE("OSC sends to a group's member on the OSC port it listens on, and to a typed receiver on the feedback port") {
    OscBench b;
    hearWithMac(*b.discovery, kIpA, "MM-A", kMacA);
    b.add(kMacA);
    REQUIRE(b.scheduler.setControl("Osc", "receivers", "{\"value\":\"Stage, 192.168.1.99\"}") == Scheduler::SetControlResult::Ok);
    b.osc->tick1s();
    const uint8_t member[4] = {192, 168, 1, 20}, controller[4] = {192, 168, 1, 99};
    CHECK(b.osc->destinationPort(member) == b.osc->port);             // a MoonLight in the group, listening as this one does
    CHECK(b.osc->destinationPort(controller) == b.osc->feedbackPort);  // a controller, on its feedback port
    CHECK(b.osc->port != b.osc->feedbackPort);                         // the defaults differ, so the member port matters
}
