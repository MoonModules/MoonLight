/// The fleet clock between devices: the lowest live device heard running the clock is the reference, a member's exchange with it brings the member onto its time, and a stray reply changes nothing.

#include "doctest.h"

#include "core/services/OscPacket.h"
#include "core/system/FleetClock.h"

#include <cstring>
#include <vector>

using namespace mm;

namespace {

/// A fleet clock on no network: its local clock set by the test, what it sends captured, what it publishes kept as its own.
struct TestClock : FleetClock {
    int64_t local = 0;
    ClockDiscipline own;
    struct Sent { uint8_t ip[4]; std::vector<uint8_t> data; };
    std::vector<Sent> sent;
    int64_t localUs() const override { return local; }
    bool ensureOpen() override { return true; }
    ClockDiscipline published() const override { return own; }
    void publish(const ClockDiscipline& d) override { own = d; }
    void transmit(const uint8_t ip[4], uint16_t, const uint8_t* data, size_t len) override {
        Sent s;
        std::memcpy(s.ip, ip, 4);
        s.data.assign(data, data + len);
        sent.push_back(s);
    }
    /// Hear device `id` say it runs the clock, at fleet time `fleetUs` (0: no time carried).
    void hear(uint32_t id, int64_t fleetUs = 0) {
        uint8_t msg[48];
        osc::Writer w(msg, sizeof(msg), FleetClock::kHereAddress, ",ih");
        w.i32(id);
        w.i64(static_cast<uint64_t>(fleetUs));
        const uint8_t ip[4] = {192, 168, 1, 1};
        handle(msg, w.size(), ip, FleetClock::kPort, local);
    }
    /// Choose among peers {id, last octet}, named "peer", each heard running the clock.
    void peers(uint32_t self, std::initializer_list<std::pair<uint32_t, uint8_t>> list) {
        for (const auto& p : list) hear(p.first);
        choose(self, list);
    }
    /// Choose among peers as Discovery offers them, whatever was heard.
    void choose(uint32_t self, std::initializer_list<std::pair<uint32_t, uint8_t>> list) {
        tick(self, [&](auto&& offer) {
            for (const auto& p : list) {
                const uint8_t ip[4] = {192, 168, 1, p.second};
                offer(p.first, ip, "peer");
            }
        });
    }
};

const uint8_t kMemberIp[4] = {192, 168, 1, 50};
const uint8_t kRefIp[4] = {192, 168, 1, 30};

}  // namespace

TEST_CASE("FleetClock: the lowest live device heard running the clock is the reference, this device when it is the lowest") {
    TestClock c;
    c.peers(50, {{70, 70}, {30, 30}});
    CHECK(c.referenceId() == 30);
    c.peers(10, {{70, 70}, {30, 30}});
    CHECK(c.referenceId() == 0);
    REQUIRE(c.note() != nullptr);
    CHECK(std::strcmp(c.note(), "clock: the reference") == 0);
}

TEST_CASE("FleetClock: a member's exchange with the reference brings it onto the reference's time, within half the round trip") {
    TestClock member, ref;
    member.peers(50, {{30, 30}});
    ref.peers(30, {{50, 50}});
    REQUIRE(ref.referenceId() == 0);
    ref.own.claim();   // the reference holds the fleet's time, which it answers with
    ref.local = 9000000;   // the reference booted earlier: 9 s on its clock when the member's reads 1 s
    member.local = 1000000;
    member.ask(member.local);
    REQUIRE(member.sent.size() == 1);
    CHECK(std::memcmp(member.sent[0].ip, kRefIp, 4) == 0);
    CHECK(std::strcmp(member.note(), "clock: syncing to peer") == 0);
    // 2 ms on the wire each way; the reference answers at once.
    ref.local += 2000;
    ref.handle(member.sent[0].data.data(), member.sent[0].data.size(), kMemberIp, FleetClock::kPort, ref.local);
    REQUIRE(ref.sent.size() == 1);
    CHECK(std::memcmp(ref.sent[0].ip, kMemberIp, 4) == 0);
    member.local += 4000;
    member.handle(ref.sent[0].data.data(), ref.sent[0].data.size(), kRefIp, FleetClock::kPort, member.local);
    REQUIRE(member.own.synced());
    CHECK(member.own.fleetAt(member.local) == ref.local + 2000);   // the reply's 2 ms back, on the reference's clock
    CHECK(std::strcmp(member.note(), "clock: peer, ±2.0 ms") == 0);
}

TEST_CASE("FleetClock: a device not yet holding the fleet's time stays silent on a request, so a member steps only onto the fleet's time") {
    TestClock member, ref;
    member.peers(50, {{30, 30}});
    ref.peers(30, {{50, 50}});
    member.local = 1000000;
    member.ask(member.local);
    REQUIRE(member.sent.size() == 1);
    ref.handle(member.sent[0].data.data(), member.sent[0].data.size(), kMemberIp, FleetClock::kPort, 2000);
    CHECK(ref.sent.empty());
}

TEST_CASE("FleetClock: a fleet past the heard table keeps the lowest ids, so a low device joining late still becomes the reference") {
    TestClock member;
    for (uint32_t id = 100; id < 140; id++) member.hear(id);   // more devices than the table holds
    member.hear(5);
    member.choose(200, {{120, 120}, {5, 5}});
    CHECK(member.referenceId() == 5);
}

TEST_CASE("FleetClock: a reply to no request, from another device or with another t1 changes nothing") {
    TestClock member;
    member.peers(50, {{30, 30}});
    member.local = 1000000;
    member.ask(member.local);
    uint8_t msg[64];
    const auto replyWith = [&](int64_t t1) {
        osc::Writer w(msg, sizeof(msg), FleetClock::kReplyAddress, ",hhh");
        w.i64(static_cast<uint64_t>(t1)); w.i64(5000000); w.i64(5000000);
        return w.size();
    };
    const uint8_t stranger[4] = {192, 168, 1, 99};
    member.handle(msg, replyWith(1000000), stranger, FleetClock::kPort, 1004000);   // the right t1, the wrong sender
    CHECK_FALSE(member.own.synced());
    member.handle(msg, replyWith(999), kRefIp, FleetClock::kPort, 1004000);   // the right sender, the wrong t1
    CHECK_FALSE(member.own.synced());
    member.handle(msg, replyWith(1000000), kRefIp, FleetClock::kPort, 1004000);   // the one it asked
    CHECK(member.own.synced());
    member.handle(msg, replyWith(1000000), kRefIp, FleetClock::kPort, 1004000);   // the same reply again: no request is pending
    CHECK(member.own.synced());
}

TEST_CASE("FleetClock: a device not heard running the clock is never asked, such as one on older firmware") {
    TestClock member;
    member.choose(50, {{30, 30}, {40, 40}});   // both live, neither heard
    CHECK(member.referenceId() == 0);
    member.hear(40);
    member.choose(50, {{30, 30}, {40, 40}});
    CHECK(member.referenceId() == 40);
    member.hear(30);
    member.choose(50, {{30, 30}, {40, 40}});
    CHECK(member.referenceId() == 30);
}

TEST_CASE("FleetClock: the reference sends no request, listens a beacon interval, then claims its own time as the fleet's") {
    TestClock ref;
    ref.peers(30, {{50, 50}});
    ref.ask(1000000);
    CHECK(ref.sent.empty());
    CHECK_FALSE(ref.own.synced());   // still listening for the fleet
    ref.ask(1000000 + FleetClock::kListenUs);
    CHECK(ref.own.synced());   // no one heard: its time is the fleet's, so its changes go out applying at a time
}

TEST_CASE("FleetClock: a rebooted lowest device takes the fleet's time from a beacon, so the fleet is not stepped back to its uptime") {
    TestClock ref;
    ref.local = 2000000;   // two seconds after its boot
    ref.hear(50, 3600000000);   // a member's beacon: the fleet is an hour in
    ref.choose(30, {{50, 50}});   // this device is the lowest
    REQUIRE(ref.referenceId() == 0);
    ref.ask(ref.local);
    REQUIRE(ref.own.synced());
    CHECK(ref.own.fleetAt(ref.local) == 3600000000);
}

TEST_CASE("FleetClock: a device's own beacon, looped back by the network, is not another device's time") {
    TestClock ref;
    ref.choose(30, {});   // alone, and it knows its id
    ref.local = 2000000;
    ref.hear(30, 3600000000);   // its own id, as Windows delivers a multicast send back to its sender
    ref.ask(ref.local);
    CHECK_FALSE(ref.own.synced());   // still listening: nobody else was heard
}
