/// @module RtpMidi

/// RTP-MIDI: the AppleMIDI session packets, the MIDI list of a data packet, and a session between two ports on this machine.

#include "doctest.h"
#include "core/util/RtpMidi.h"
#include "core/services/RtpMidiSession.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using namespace mm;

// Apple's layout: FF FF, the two letters, version 2, the token, the SSRC, then the name.
TEST_CASE("an invitation has AppleMIDI's exact bytes, and reads back with its name") {
    uint8_t p[64];
    const size_t len = rtpmidi::encodeExchange(p, sizeof(p), rtpmidi::Command::Invitation, 0x01020304, 0x0A0B0C0D, "legs");
    const uint8_t golden[] = {0xFF, 0xFF, 'I', 'N', 0, 0, 0, 2, 1, 2, 3, 4, 0x0A, 0x0B, 0x0C, 0x0D, 'l', 'e', 'g', 's', 0};
    REQUIRE(len == sizeof(golden));
    CHECK(std::memcmp(p, golden, len) == 0);

    rtpmidi::Exchange e;
    REQUIRE(rtpmidi::parseExchange(p, len, e));
    CHECK(e.command == rtpmidi::Command::Invitation);
    CHECK(e.token == 0x01020304);
    CHECK(e.ssrc == 0x0A0B0C0D);
    CHECK(std::strcmp(e.name, "legs") == 0);

    // A goodbye carries no name, and a name with no end is cut rather than read past.
    CHECK(rtpmidi::encodeExchange(p, sizeof(p), rtpmidi::Command::End, 1, 2, "ignored") == 16);
    const uint8_t noEnd[] = {0xFF, 0xFF, 'O', 'K', 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 'a', 'b'};
    REQUIRE(rtpmidi::parseExchange(noEnd, sizeof(noEnd), e));
    CHECK(std::strcmp(e.name, "ab") == 0);
    CHECK_FALSE(rtpmidi::parseExchange(golden, 15, e));   // too short for its header
}

TEST_CASE("a clock sync carries its count and three 64-bit timestamps") {
    rtpmidi::Sync s;
    s.ssrc = 7;
    s.count = 1;
    s.ts[0] = 0x0102030405060708ULL;
    s.ts[1] = 42;
    uint8_t p[rtpmidi::kSyncLen];
    REQUIRE(rtpmidi::encodeSync(p, sizeof(p), s) == rtpmidi::kSyncLen);
    CHECK((p[2] == 'C' && p[3] == 'K' && p[8] == 1));
    rtpmidi::Sync back;
    REQUIRE(rtpmidi::parseSync(p, sizeof(p), back));
    CHECK(back.ssrc == 7);
    CHECK(back.ts[0] == 0x0102030405060708ULL);
    CHECK(back.ts[1] == 42);
    CHECK(rtpmidi::commandOf(p, sizeof(p)) == rtpmidi::Command::Sync);
}

namespace {
/// Every message in a data packet, as byte strings.
std::vector<std::vector<uint8_t>> messagesIn(const uint8_t* p, size_t len) {
    std::vector<std::vector<uint8_t>> out;
    uint32_t ssrc = 0;
    rtpmidi::forEachMessage(p, len, ssrc, [&](const uint8_t* m, size_t n) { out.emplace_back(m, m + n); });
    return out;
}
}  // namespace

// What a board sends: no journal, and a zero delta time between messages.
TEST_CASE("a data packet holds its messages with a zero delta time between them, and reads back") {
    uint8_t p[64];
    rtpmidi::DataWriter w(p, sizeof(p));
    const uint8_t fader[3] = {0xE0, 0x00, 0x40}, light[3] = {0x90, 0x18, 0x7F};
    REQUIRE(w.add(fader, 3));
    REQUIRE(w.add(light, 3));
    const size_t len = w.finish(5, 1000, 0x11223344);
    const uint8_t golden[] = {0x80, 0x61, 0x00, 0x05, 0x00, 0x00, 0x03, 0xE8, 0x11, 0x22, 0x33, 0x44,
                              0x80, 0x07, 0xE0, 0x00, 0x40, 0x00, 0x90, 0x18, 0x7F};
    REQUIRE(len == sizeof(golden));
    CHECK(std::memcmp(p, golden, len) == 0);
    const auto m = messagesIn(p, len);
    REQUIRE(m.size() == 2);
    CHECK(m[1] == std::vector<uint8_t>{0x90, 0x18, 0x7F});
    CHECK(rtpmidi::DataWriter(p, sizeof(p)).finish(0, 0, 0) == 0);   // an empty packet is not sent
}

// What a Mac sends: delta times, running status, a journal after the list, and a SysEx split over packets.
TEST_CASE("a received MIDI list honors delta times and running status, and skips a journal and SysEx pieces") {
    const uint8_t p[] = {
        0x80, 0x61, 0, 1, 0, 0, 0, 0, 0, 0, 0, 9,
        0x60 | 13,                     // J and Z, a 13-byte list
        0x00, 0xB0, 0x10, 0x41,        // delta, then a knob turn
        0x81, 0x00, 0x10, 0x01,        // a two-byte delta, then the same status left out
        0x00, 0xF8,                    // a clock tick, real time, which keeps running status
        0x00, 0x11, 0x42,              // running status still applies
        0xAA, 0xBB, 0xCC,              // the journal, past the list
    };
    const auto m = messagesIn(p, sizeof(p));
    REQUIRE(m.size() == 4);
    CHECK(m[0] == std::vector<uint8_t>{0xB0, 0x10, 0x41});
    CHECK(m[1] == std::vector<uint8_t>{0xB0, 0x10, 0x01});
    CHECK(m[2] == std::vector<uint8_t>{0xF8});
    CHECK(m[3] == std::vector<uint8_t>{0xB0, 0x11, 0x42});

    const uint8_t sysex[] = {0x80, 0x61, 0, 1, 0, 0, 0, 0, 0, 0, 0, 9, 10,
                             0xF0, 0x47, 0x7F, 0xF7,   // complete
                             0x00, 0xF0, 0x01, 0xF0,   // the first piece of a longer one
                             0x00, 0x90};              // and a note cut short by the end of the list
    const auto s = messagesIn(sysex, sizeof(sysex));
    REQUIRE(s.size() == 1);
    CHECK(s[0] == std::vector<uint8_t>{0xF0, 0x47, 0x7F, 0xF7});
}

TEST_CASE("a data packet that lies about its length or has no RTP version is read no further than it goes") {
    const uint8_t longer[] = {0x80, 0x61, 0, 1, 0, 0, 0, 0, 0, 0, 0, 9, 0x8F, 0xFF, 0x90, 0x20, 0x7F};
    CHECK(messagesIn(longer, sizeof(longer)).size() == 1);
    const uint8_t notRtp[] = {0x40, 0x61, 0, 1, 0, 0, 0, 0, 0, 0, 0, 9, 3, 0x90, 0x20, 0x7F};
    uint32_t ssrc = 0;
    CHECK_FALSE(rtpmidi::forEachMessage(notRtp, sizeof(notRtp), ssrc, [](const uint8_t*, size_t) {}));
    CHECK_FALSE(rtpmidi::forEachMessage(notRtp, 5, ssrc, [](const uint8_t*, size_t) {}));
}

namespace {
/// Service both sides until `done`, or give up after a second.
template <class F>
bool runUntil(RtpMidiSession& a, RtpMidiSession& b, F done,
              std::vector<std::vector<uint8_t>>* atA = nullptr, std::vector<std::vector<uint8_t>>* atB = nullptr) {
    for (int i = 0; i < 200; i++) {
        a.service([&](const uint8_t* m, size_t n) { if (atA) atA->emplace_back(m, m + n); });
        b.service([&](const uint8_t* m, size_t n) { if (atB) atB->emplace_back(m, m + n); });
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
const uint8_t kLoopback[4] = {127, 0, 0, 1};
}  // namespace

// The handshake on both ports, a message each way, and a goodbye that sends the inviter back to inviting.
TEST_CASE("an RTP-MIDI session connects over both ports, carries messages both ways, and invites again after a goodbye") {
    RtpMidiSession host, desk;
    REQUIRE(host.open(25004, "legs"));
    REQUIRE(desk.open(25104, "bridge"));
    host.invite(kLoopback, 25104);
    REQUIRE(runUntil(host, desk, [&] { return host.connected() && desk.connected(); }));
    CHECK(std::strcmp(host.peerName(), "bridge") == 0);
    CHECK(std::strcmp(desk.peerName(), "legs") == 0);

    std::vector<std::vector<uint8_t>> atHost, atDesk;
    const uint8_t fader[3] = {0xE0, 0x00, 0x40}, hello[5] = {0xF0, 0x47, 0x01, 0x02, 0xF7};
    desk.send(fader, 3);
    host.send(hello, 5);
    REQUIRE(runUntil(host, desk, [&] { return !atHost.empty() && !atDesk.empty(); }, &atHost, &atDesk));
    CHECK(atHost[0] == std::vector<uint8_t>(fader, fader + 3));
    CHECK(atDesk[0] == std::vector<uint8_t>(hello, hello + 5));

    // The desk side stops accepting, as a bridge does once its desk is unplugged: the host invites again, and is refused.
    desk.setAccepting(false);
    REQUIRE(runUntil(host, desk, [&] { return host.refused(); }));
    CHECK_FALSE(host.connected());
    CHECK(host.state() == RtpMidiSession::State::Inviting);
    desk.setAccepting(true);
    REQUIRE(runUntil(host, desk, [&] { return host.connected() && desk.connected(); }));
    host.close();
    desk.close();
}

// Two hosts, one desk: the second is refused while the first holds the session.
TEST_CASE("an RTP-MIDI session takes one peer at a time") {
    RtpMidiSession first, second, desk;
    REQUIRE(first.open(25204, "first"));
    REQUIRE(second.open(25304, "second"));
    REQUIRE(desk.open(25404, "bridge"));
    first.invite(kLoopback, 25404);
    for (int i = 0; i < 200 && !desk.connected(); i++) {
        first.service([](const uint8_t*, size_t) {});
        desk.service([](const uint8_t*, size_t) {});
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(desk.connected());
    second.invite(kLoopback, 25404);
    for (int i = 0; i < 200 && !second.refused(); i++) {
        second.service([](const uint8_t*, size_t) {});
        desk.service([](const uint8_t*, size_t) {});
        first.service([](const uint8_t*, size_t) {});
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(second.refused());
    CHECK(std::strcmp(desk.peerName(), "first") == 0);
    first.close();
    second.close();
    desk.close();
}

// A refusal belongs to the host that refused, so a new host starts unanswered.
TEST_CASE("an RTP-MIDI invitation refused by one host is not reported as refused by the next") {
    RtpMidiSession host, closedDesk, openDesk;
    REQUIRE(host.open(26104, "legs"));
    REQUIRE(closedDesk.open(26204, "no desk"));
    REQUIRE(openDesk.open(26304, "desk"));
    closedDesk.setAccepting(false);
    host.invite(kLoopback, 26204);
    REQUIRE(runUntil(host, closedDesk, [&] { return host.refused(); }));
    host.invite(kLoopback, 26304);
    CHECK_FALSE(host.refused());
    REQUIRE(runUntil(host, openDesk, [&] { return host.connected(); }));
}

// Between a peer's two invitations, a second peer would otherwise take its place and leave it hanging.
TEST_CASE("an RTP-MIDI session half open with one peer refuses a second") {
    RtpMidiSession desk, second;
    REQUIRE(desk.open(26404, "desk"));
    REQUIRE(second.open(26504, "second"));
    platform::UdpSocket first;   // a peer that sends its control-port invitation and nothing more
    REQUIRE(first.open());
    REQUIRE(first.bind(26604));
    uint8_t pkt[64];
    const size_t len = rtpmidi::encodeExchange(pkt, sizeof(pkt), rtpmidi::Command::Invitation, 77, 1234, "first");
    first.sendToAddr(kLoopback, 26404, pkt, len);
    for (int i = 0; i < 50; i++) { desk.service([](const uint8_t*, size_t) {}); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    second.invite(kLoopback, 26404);
    REQUIRE(runUntil(second, desk, [&] { return second.refused(); }));
    CHECK_FALSE(desk.connected());
}

// A session gone silent ends with a goodbye, so the other side does not hold a session nobody serves.
TEST_CASE("an RTP-MIDI inviter whose peer went silent says goodbye, and the peer lets go") {
    struct RealClockAfter { ~RealClockAfter() { platform::setTestNowMs(0); } } realClockAfter;
    platform::setTestNowMs(1000);
    RtpMidiSession host, desk;
    REQUIRE(host.open(26704, "legs"));
    REQUIRE(desk.open(26804, "desk"));
    host.invite(kLoopback, 26804);
    REQUIRE(runUntil(host, desk, [&] { return host.connected() && desk.connected(); }));
    platform::setTestNowMs(1000 + 31000);   // longer than the inviter waits, shorter than the invited side does
    host.service([](const uint8_t*, size_t) {});   // its goodbye; it then invites again, so only the desk is served below
    for (int i = 0; i < 200 && desk.connected(); i++) {
        desk.service([](const uint8_t*, size_t) {});
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_FALSE(desk.connected());
    CHECK(desk.state() == RtpMidiSession::State::Listening);
}
