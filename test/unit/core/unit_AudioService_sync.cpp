/// @module AudioService
/// @also WledAudioSyncPacket
///
/// Drives AudioService's WLED audio-sync socket lifecycle through the public tick(), the entry the scheduler calls on a device.
///
/// @technical
///
/// ## What it covers
///
/// The lazy open, once per mode, where syncEnsureSocket latches; the send path reaching "sending", and the throttle that paces it.
///
/// The receive path over a real localhost round trip: a frame replaced, then the fresh-to-stale fallback a pure sink falls back to.
///
/// platform::networkReady() is true on desktop, so the lazy open fires on the first tick, as on a device once its interface is up.
///
/// ## Leaving local clears the mic status
///
/// The other modes report through the separate sync row and have no input to diagnose, so a stale mic message there points at nothing.
///
/// What local leaves depends on the host: nothing on a capture-capable desktop, "capture init failed" on a locked-down one, a request for pins on an I2S target without them. Whatever it left, leaving local clears.
///
/// ## Capture and send coexist
///
/// Send fires from the same tick() that runs the capture path, so the capture gate cannot starve the sender.
///
/// ## Forcing a bind to fail
///
/// Hogging the port with a second socket is not portable: on Linux, SO_REUSEADDR on a UDP socket bound to INADDR_ANY permits the overlapping bind, so the failure never happens.
///
/// A privileged port is no better, since macOS lets a non-root process bind port 80.
///
/// ## Why the clock is fake and the socket is real
///
/// Time comes from platform::setTestNowMs(), so a throttle window is exact and the suite never sleeps a real second; only the delivery is real, polled with a bounded retry.
///
/// The port is a test one rather than 11988, since a running desktop app holds the real sync port.

#include "doctest.h"
#include "core/services/AudioService.h"
#include "core/util/MoonLightAudioSyncPacket.h"
#include "core/util/WLEDAudioSyncPacket.h"
#include "platform/platform.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>   // the braced list below; clang pulls it in transitively, GCC does not

using namespace mm;

namespace {
constexpr uint16_t kTestSyncPort = 21988;   // a free high port, not the real 11988

// The status read-out is published by tick1s(); call it after a tick() so the assertions below see the current state string rather than the setup() baseline.
const char* status(AudioService& a) { a.tick1s(); return a.syncStatusForTest(); }

// A guard that freezes virtual time for a case and restores the real clock on scope exit, so a thrown assertion can't leak frozen time into the next case.
struct FrozenClock {
    FrozenClock(uint32_t ms) : now_(ms) { platform::setTestNowMs(ms); }
    ~FrozenClock() { platform::setTestNowMs(0); }
    void advance(uint32_t ms) { now_ += ms; platform::setTestNowMs(now_); }
    uint32_t now_ = 1;
};
// A receiver on the test port, its socket opened by the first tick.
void openReceiver(AudioService& a) {
    a.mode = AudioService::kReceiveMode;
    a.syncPort = kTestSyncPort;
    a.applyState();
    a.tick();
}
// Send one WLED packet of `level` to the receiver over loopback and tick until it lands; false when it never does.
bool sendLevel(AudioService& a, uint8_t level) {
    AudioFrame peer;
    peer.level = level;
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, peer);
    platform::UdpSocket tx;
    const uint8_t loopback[4] = {127, 0, 0, 1};
    if (!tx.open() || !tx.sendToAddr(loopback, kTestSyncPort, pkt, WLED_SYNC_PACKET_SIZE)) return false;
    for (int i = 0; i < 100; i++) {   // loopback delivery is asynchronous in real time
        a.tick();
        if (a.audioFrame()->level == level) return true;
        platform::delayMs(1);
    }
    return false;
}
// Send `pkt` to the receiver over loopback and tick until `landed` holds; false when it never does.
template <typename Pred>
bool sendUntil(AudioService& a, const uint8_t* pkt, size_t n, Pred landed) {
    platform::UdpSocket tx;
    const uint8_t loopback[4] = {127, 0, 0, 1};
    if (!tx.open() || !tx.sendToAddr(loopback, kTestSyncPort, pkt, n)) return false;
    for (int i = 0; i < 100; i++) {   // loopback delivery is asynchronous in real time
        a.tick();
        if (landed()) return true;
        platform::delayMs(1);
    }
    return false;
}
// The first byte a sending board puts on the wire, read by a socket of the test's own on the port.
int firstByteSent(AudioService& a, platform::UdpSocket& rx) {
    uint8_t buf[kMlSyncMaxPacket + 8];
    uint8_t src[4];
    for (int i = 0; i < 100; i++) {
        a.tick();
        const int n = rx.recvFrom(buf, sizeof(buf), src);
        if (n > 0) return buf[0];
        platform::delayMs(1);
    }
    return -1;
}
}  // namespace

// Regression: the mic status is a local-mode read-out, so leaving local must clear it: @xref{leaving-local-clears-the-mic-status}.
TEST_CASE("AudioService: switching out of Local mode clears the mic status") {
    AudioService a;
    a.mode = AudioService::kLocalMode;                      // Local audio
    a.applyState();
    // Any of the three Local outcomes above is legitimate; only note which one happened.
    const bool localLeftStatus = a.status() != nullptr && a.status()[0] != 0;
    (void)localLeftStatus;

    // The mic message must not survive; the line itself may hold a sync message, which reports there too.
    auto noMicMessage = [&]() {
        const char* s = a.status();
        return s == nullptr || std::strstr(s, "mic") == nullptr;
    };

    a.mode = AudioService::kReceiveMode;                      // receive network
    a.applyState();                  // prepare() non-Local branch must clear the stale mic status
    CHECK(noMicMessage());

    // And back to Simulate, same rule (no mic there either).
    a.mode = AudioService::kSimMode;
    a.applyState();
    CHECK(noMicMessage());
    // Simulate has no sync either, so here the line IS empty. Null or empty: a module that has never reported anything has a null status, which is the same "nothing to say".
    CHECK((a.status() == nullptr || a.status()[0] == 0));

    a.release();
}

TEST_CASE("AudioService Local+send: lazy-opens once and reports sending") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = AudioService::kLocalMode;
    a.send = true;   // local audio, broadcasting
    a.syncPort = kTestSyncPort;
    a.applyState();                  // build: syncReinit(), socket NOT opened here (boot-safe)
    CHECK(std::strstr(status(a), "waiting") != nullptr);   // no tick() yet → still waiting

    a.tick();                        // networkReady() true on desktop → opens now
    CHECK(std::strcmp(status(a), "sending") == 0);
    CHECK(a.syncOpenForTest());

    // Idempotent: a second tick doesn't re-open (the latch holds).
    a.tick();
    CHECK(a.syncOpenForTest());
    CHECK(std::strcmp(status(a), "sending") == 0);

    a.release();
    CHECK_FALSE(a.syncOpenForTest());
}

// A rebuild anywhere in the tree prepared the audio service too, which closed its socket and reopened its microphone every time.
TEST_CASE("AudioService keeps its socket through a prepare that changed none of its settings") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = AudioService::kLocalMode;
    a.send = true;
    a.syncPort = kTestSyncPort;
    a.applyState();
    a.tick();
    REQUIRE(a.syncOpenForTest());
    a.applyState();                  // as a tree rebuild for another module does
    CHECK(a.syncOpenForTest());
    a.syncPort = static_cast<uint16_t>(kTestSyncPort + 1);
    a.applyState();                  // a new port rebinds
    CHECK_FALSE(a.syncOpenForTest());
    a.release();
}

TEST_CASE("AudioService Local+send: broadcasts are throttled to ~kSyncSendIntervalMs") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = AudioService::kLocalMode;
    a.send = true;   // local audio, broadcasting
    a.syncPort = kTestSyncPort;
    a.applyState();
    a.tick();                        // opens + first send
    REQUIRE(a.syncOpenForTest());

    // More ticks within the same interval must not each emit: the send count does not advance while the throttle window is open.
    const uint32_t c0 = a.syncSendCountForTest();
    a.tick();
    a.tick();
    CHECK(a.syncSendCountForTest() == c0);   // throttled: no send per tick

    // After the interval elapses, exactly one more send is allowed.
    clk.advance(AudioService::syncSendIntervalMsForTest() + 5);
    a.tick();
    CHECK(a.syncSendCountForTest() - c0 == 1);

    a.release();
}

// The fleet-source contract: capture and broadcast coexist on one host: @xref{capture-and-send-coexist}.
TEST_CASE("AudioService Local+send on a capture host: capture and broadcast coexist") {
    if constexpr (!platform::hasAudioCapture) return;
    FrozenClock clk(1);
    AudioService a;
    a.mode = AudioService::kLocalMode;
    a.send = true;
    a.syncPort = kTestSyncPort;
    a.applyState();   // may or may not bring capture up (host permission dependent), both fine
    a.tick();         // opens the socket + first send, then runs the local capture path
    REQUIRE(a.syncOpenForTest());
    CHECK(std::strcmp(status(a), "sending") == 0);
    const uint8_t c0 = a.syncSendCountForTest();
    clk.advance(AudioService::syncSendIntervalMsForTest() + 5);
    a.tick();         // capture read + throttled send in one tick, no early return
    CHECK(a.syncSendCountForTest() - c0 == 1);
    a.release();
}

TEST_CASE("AudioService Receive: a localhost WLED packet drives frame_, then holds it and reports listening") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = AudioService::kReceiveMode;   // receive network
    a.syncPort = kTestSyncPort;
    a.applyState();
    a.tick();                        // binds kTestSyncPort
    REQUIRE(a.syncOpenForTest());
    CHECK(std::strncmp(status(a), "listening", 9) == 0);   // with or without the group, which the host's network decides

    // Send a real WLED v2 packet to the bound port over loopback.
    AudioFrame peer;
    peer.level = 222; peer.levelSmoothed = 111; peer.peakHz = 660; peer.peakMag = 55;
    for (int i = 0; i < 16; i++) peer.bands[i] = static_cast<uint8_t>(i * 8);
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, peer);

    platform::UdpSocket tx;
    REQUIRE(tx.open());
    const uint8_t loopback[4] = {127, 0, 0, 1};
    REQUIRE(tx.sendToAddr(loopback, kTestSyncPort, pkt, WLED_SYNC_PACKET_SIZE));

    // Loopback delivery is async in real time, poll tick() until the peer frame lands (bounded, ≤100 iterations). Virtual time stays frozen, so the frame counts as fresh.
    bool landed = false;
    for (int i = 0; i < 100 && !landed; i++) {
        a.tick();
        landed = a.audioFrame()->level == 222 && a.audioFrame()->peakHz == 660;
        if (!landed) platform::delayMs(1);   // real wait for the datagram, not virtual time
    }
    CHECK(landed);
    CHECK(a.audioFrame()->levelSmoothed == 111);
    // The ballistic is ours rather than the packet's, and it survives the whole-frame copy each packet makes.
    CHECK(a.audioFrame()->bandsSmoothed[15] > 80);   // peer.bands[15] is 120; one block of rise
    AudioFrame quiet;                                  // then the peer goes silent
    buildWledAudioSync(pkt, quiet);
    REQUIRE(tx.sendToAddr(loopback, kTestSyncPort, pkt, WLED_SYNC_PACKET_SIZE));
    bool fell = false;
    for (int i = 0; i < 100 && !fell; i++) {
        a.tick();
        fell = a.audioFrame()->level == 0;
        if (!fell) platform::delayMs(1);
    }
    CHECK(fell);
    CHECK(a.audioFrame()->bandsSmoothed[15] > 40);   // still falling, not reset by the copy
    // Named, not just "receiving": the packet came from loopback in WLED's format, so the status has to say both. A receiver that cannot name its source looks identical to one locked onto the wrong device.
    CHECK(std::strcmp(status(a), "receiving WLED from 127.0.0.1") == 0);

    // A pure sink with no packet goes stale and falls back to "listening", holding its last frame.
    clk.advance(AudioService::syncFallbackMsForTest() + 20);
    a.tick();
    CHECK(std::strncmp(status(a), "listening", 9) == 0);   // with or without the group, which the host's network decides

    tx.close();
    a.release();
}

// The receiver numbers the frames itself, so two identical packets are two frames: the `seq` the Drum Kit reads to take each hit once.
TEST_CASE("AudioService Receive: each packet is a new frame number, the same audio twice too") {
    FrozenClock clk(1);
    AudioService a;
    openReceiver(a);
    REQUIRE(a.syncOpenForTest());
    REQUIRE(sendLevel(a, 77));
    const uint16_t first = a.audioFrame()->seq;
    AudioFrame peer;
    peer.level = 77;   // the same packet again
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, peer);
    platform::UdpSocket tx;
    const uint8_t loopback[4] = {127, 0, 0, 1};
    REQUIRE(tx.open());
    REQUIRE(tx.sendToAddr(loopback, kTestSyncPort, pkt, WLED_SYNC_PACKET_SIZE));
    bool next = false;
    for (int i = 0; i < 100 && !next; i++) {   // loopback delivery is asynchronous in real time
        a.tick();
        next = a.audioFrame()->seq != first;
        if (!next) platform::delayMs(1);
    }
    CHECK(next);
    CHECK(a.audioFrame()->seq == static_cast<uint16_t>(first + 1));
    tx.close();
    a.release();
}

TEST_CASE("AudioService Receive: a failed bind backs off instead of retrying every tick") {
    FrozenClock clk(1);
    // Force the bind to fail deterministically, which is harder than it looks: @xref{forcing-a-bind-to-fail}.
    platform::setTestBindFails(true);

    AudioService a;
    a.mode = AudioService::kReceiveMode;   // receive network
    a.syncPort = kTestSyncPort;
    a.applyState();
    a.tick();                        // first bring-up attempt → bind fails
    CHECK_FALSE(a.syncOpenForTest());
    CHECK(std::strcmp(status(a), "receive: bind failed") == 0);

    // Within the backoff window a tick must not retry, and the unchanged string is the observable proof.
    a.tick();
    a.tick();
    CHECK_FALSE(a.syncOpenForTest());
    CHECK(std::strcmp(a.syncStatusForTest(), "receive: bind failed") == 0);

    // Let the bind succeed and advance past the backoff, the next tick retries and succeeds.
    platform::setTestBindFails(false);
    clk.advance(AudioService::syncOpenRetryMsForTest() + 5);
    a.tick();
    CHECK(a.syncOpenForTest());
    CHECK(std::strncmp(status(a), "listening", 9) == 0);   // with or without the group, which the host's network decides

    a.release();
}

// A network without multicast still delivers unicast, so the receiver listens and says what it is missing rather than refusing to open.
TEST_CASE("AudioService Receive: a failed group join still listens for unicast, and says so") {
    platform::setTestJoin(platform::TestJoin::Fail);
    FrozenClock clk(1);
    AudioService a;
    openReceiver(a);
    REQUIRE(a.syncOpenForTest());
    CHECK(std::strcmp(status(a), "listening, unicast only: multicast unavailable") == 0);
    CHECK(sendLevel(a, 77));

    // Gone stale, it falls back to the same warning rather than a plain "listening".
    clk.advance(AudioService::syncFallbackMsForTest() + 20);
    a.tick();
    CHECK(std::strcmp(status(a), "listening, unicast only: multicast unavailable") == 0);
    a.release();
}

// A router can stop forwarding the group to a member it has not heard from, so the socket announces itself again while silent, paced, and audio arriving holds it off.
TEST_CASE("AudioService Receive: a silent receiver joins the group again every rejoin interval, a receiving one does not") {
    FrozenClock clk(1);
    AudioService a;
    openReceiver(a);
    REQUIRE(a.syncOpenForTest());
    CHECK(a.syncJoinCountForTest() == 1);

    const uint32_t rejoin = platform::UdpSocket::kRejoinMs;
    clk.advance(rejoin - 10);
    a.tick();
    CHECK(a.syncJoinCountForTest() == 1);   // inside the interval
    clk.advance(10);
    a.tick();
    CHECK(a.syncJoinCountForTest() == 2);   // silent for the whole interval
    a.tick();
    CHECK(a.syncJoinCountForTest() == 2);   // paced: one join per interval

    clk.advance(rejoin - 10);
    REQUIRE(sendLevel(a, 99));
    clk.advance(rejoin - 10);   // past the last join's interval, but audio arrived within it
    a.tick();
    CHECK(a.syncJoinCountForTest() == 2);
    a.release();
}

// A join that failed is retried the same way, so a network that comes to allow multicast later is joined without a mode toggle.
TEST_CASE("AudioService Receive: a failed group join is retried once silent, and the warning clears") {
    platform::setTestJoin(platform::TestJoin::Fail);
    FrozenClock clk(1);
    AudioService a;
    openReceiver(a);
    REQUIRE(a.syncOpenForTest());
    CHECK(std::strcmp(status(a), "listening, unicast only: multicast unavailable") == 0);

    platform::setTestJoin(platform::TestJoin::Succeed);
    clk.advance(platform::UdpSocket::kRejoinMs);
    a.tick();
    CHECK(std::strcmp(status(a), "listening") == 0);
    a.release();
}

TEST_CASE("AudioService Local (not sending): no socket, reports off") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = AudioService::kLocalMode;   // local audio, not sending (sync == off)
    a.applyState();
    a.tick();
    CHECK_FALSE(a.syncOpenForTest());
    // Nothing about sync on the line; a mic that cannot open still reports there, and that must survive.
    const char* s = status(a);
    CHECK(std::strstr(s, "waiting for network") == nullptr);
    CHECK(std::strstr(s, "listening on") == nullptr);
    CHECK(std::strstr(s, "from ") == nullptr);
    a.release();
}

// Regression: a persisted `send` must NOT broadcast once the module switches to Simulate mode, Simulate has no captured frame worth sending, so sync() (and thus the socket) must go quiet. Pins the local-mode guard on the send leg of sync().
TEST_CASE("AudioService Local+send → Simulate: send stops, no socket") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = AudioService::kLocalMode; a.send = true;   // local audio, broadcasting
    a.syncPort = kTestSyncPort;
    a.applyState();
    a.tick();                    // opens the send socket
    REQUIRE(a.syncOpenForTest());

    // Switch to Simulate WITHOUT clearing the persisted send flag.
    a.mode = AudioService::kSimMode;
    a.applyState();              // re-prepare: syncReinit closes the socket for the new (no-socket) mode
    a.tick();
    CHECK_FALSE(a.syncOpenForTest());   // socket closed, nothing broadcasting
    CHECK(status(a)[0] == 0);           // and quiet on the status line in Simulate
    a.release();
}

// Regression: a mic diagnosis is a local-mode read-out, so a mode without a mic must clear it, and one left standing makes a working receive look dead: @xref{leaving-local-clears-the-mic-status}.
TEST_CASE("AudioService: leaving local clears an outstanding mic diagnosis, in every mic-less mode") {
    FrozenClock clk(1);
    for (const uint8_t micLess : {AudioService::kSimMode, AudioService::kReceiveMode}) {
        if (micLess == AudioService::kReceiveMode && !mm::platform::hasNetwork) continue;
        AudioService a;
        a.syncPort = kTestSyncPort;
        a.mode = micLess;
        a.applyState();
        a.setMicStatusStaleForTest(true);   // what a local-mode diagnosis leaves behind
        a.tick1s();
        CHECK_FALSE(a.micStatusStaleForTest());
        a.release();
    }
}

// Unicast is the faster choice for a few MoonLight boards on WiFi, so it sends to the hosts, and says so when there are none.
TEST_CASE("AudioService Local+send unicast: sends to its hosts, and asks for them when the list is empty") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = AudioService::kLocalMode;
    a.send = true;
    a.addressing = AudioService::kAddressUnicast;
    a.syncPort = kTestSyncPort;
    a.applyState();
    a.tick();
    CHECK(std::strstr(a.syncStatusForTest(), "set hosts") != nullptr);   // read before tick1s, whose mic retry rewrites the status
    a.release();

    AudioService b;
    b.mode = AudioService::kLocalMode;
    b.send = true;
    b.addressing = AudioService::kAddressUnicast;
    std::snprintf(b.hosts, sizeof(b.hosts), "%s", "127.0.0.1");
    b.syncPort = kTestSyncPort;
    b.applyState();
    b.tick();
    CHECK(std::strcmp(b.syncStatusForTest(), "sending") == 0);
    b.release();
}

// Between MoonLight boards the whole frame travels: the thirds, the keys, the hits, and the sender's own smoothing, so both boards show the same frame.
TEST_CASE("AudioService Receive: a MoonLight packet drives the whole frame, the sender's smoothed values kept") {
    FrozenClock clk(1);
    AudioService a;
    openReceiver(a);
    REQUIRE(a.syncOpenForTest());
    AudioFrame peer;
    peer.level = 180; peer.levelSmoothed = 120; peer.gate = 30; peer.gateOpen = true;
    peer.onsetLow = 99; peer.onset = 99;
    for (uint8_t i = 0; i < 16; i++) { peer.bands[i] = 200; peer.bandsSmoothed[i] = 17; }
    peer.thirdsCount = 28;
    peer.thirds[2] = 210;
    peer.seq = 500;
    uint8_t pkt[kMlSyncMaxPacket];
    const size_t n = buildMoonLightAudioSync(pkt, peer);
    REQUIRE(sendUntil(a, pkt, n, [&] { return a.audioFrame()->level == 180; }));
    const AudioFrame& f = *a.audioFrame();
    CHECK(f.levelSmoothed == 120);
    CHECK(f.bandsSmoothed[7] == 17);   // the sender's, not a ballistic run again here
    CHECK(f.thirdsCount == 28);
    CHECK(f.thirds[2] == 210);
    CHECK(f.onsetLow == 99);
    CHECK(f.heard());                  // by the sender's gate
    CHECK(f.bass == 210);              // derived here from the thirds, which is why the ranges do not travel
    CHECK(std::strcmp(status(a), "receiving MoonLight from 127.0.0.1") == 0);   // the format named, so a mixed network says which board sends what
    a.release();
}

// UDP may deliver a frame twice or late, and a sender with no new frame repeats its last; each is one frame, its hits taken once.
TEST_CASE("AudioService Receive: a repeated or late MoonLight frame number is dropped, a restarted sender is taken") {
    FrozenClock clk(1);
    AudioService a;
    openReceiver(a);
    REQUIRE(a.syncOpenForTest());
    AudioFrame peer;
    peer.level = 60; peer.seq = 1000;
    uint8_t pkt[kMlSyncMaxPacket];
    size_t n = buildMoonLightAudioSync(pkt, peer);
    REQUIRE(sendUntil(a, pkt, n, [&] { return a.audioFrame()->level == 60; }));
    const uint16_t first = a.audioFrame()->seq;

    peer.level = 61;                   // the same frame number again, and one behind it
    n = buildMoonLightAudioSync(pkt, peer);
    CHECK_FALSE(sendUntil(a, pkt, n, [&] { return a.audioFrame()->seq != first; }));
    peer.seq = 999;
    n = buildMoonLightAudioSync(pkt, peer);
    CHECK_FALSE(sendUntil(a, pkt, n, [&] { return a.audioFrame()->seq != first; }));

    peer.seq = 3;                      // far behind: the sender restarted
    n = buildMoonLightAudioSync(pkt, peer);
    CHECK(sendUntil(a, pkt, n, [&] { return a.audioFrame()->level == 61; }));
    a.release();
}

// One packet a frame in the format the network needs: the select picks it, and nothing else is sent.
TEST_CASE("AudioService Local+send: syncFormat picks the one format sent") {
    FrozenClock clk(1);
    platform::UdpSocket rx;
    REQUIRE(rx.open());
    REQUIRE(rx.bind(kTestSyncPort));
    AudioService a;
    a.mode = AudioService::kLocalMode;
    a.send = true;
    a.addressing = AudioService::kAddressUnicast;
    std::snprintf(a.hosts, sizeof(a.hosts), "%s", "127.0.0.1");
    a.syncPort = kTestSyncPort;
    a.applyState();
    CHECK(a.syncFormat == AudioService::kFormatMoonLight);   // the default
    clk.advance(AudioService::syncSendIntervalMsForTest() + 5);
    CHECK(firstByteSent(a, rx) == 'M');

    a.syncFormat = AudioService::kFormatWled;
    clk.advance(AudioService::syncSendIntervalMsForTest() + 5);
    CHECK(firstByteSent(a, rx) == '0');                      // WLED's "00002"
    a.release();
    rx.close();
}

// A board switching from a MoonLight sender to a WLED one shows the WLED frame alone, its thirds and keys gone (bench, 2026-11-06).
TEST_CASE("AudioService Receive: a WLED packet after a MoonLight one carries no thirds or keys of it") {
    FrozenClock clk(1);
    AudioService a;
    openReceiver(a);
    REQUIRE(a.syncOpenForTest());
    AudioFrame peer;
    peer.level = 90; peer.thirdsCount = 28; peer.thirds[4] = 200; peer.notesCount = kAudioNoteKeys; peer.key = 5;
    uint8_t ml[kMlSyncMaxPacket];
    REQUIRE(sendUntil(a, ml, buildMoonLightAudioSync(ml, peer), [&] { return a.audioFrame()->level == 90; }));
    REQUIRE(a.audioFrame()->thirdsCount == 28);
    AudioFrame wled;
    wled.level = 91;
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, wled);
    REQUIRE(sendUntil(a, pkt, WLED_SYNC_PACKET_SIZE, [&] { return a.audioFrame()->level == 91; }));
    CHECK(a.audioFrame()->thirdsCount == 0);
    CHECK(a.audioFrame()->notesCount == 0);
    CHECK(a.audioFrame()->key == 0);
    a.release();
}

// Turning send off leaves nothing on the card claiming the board still sends (bench, 2026-11-06).
TEST_CASE("AudioService Local+send: turning send off clears the sending status") {
    FrozenClock clk(1);
    AudioService a;
    a.mode = AudioService::kLocalMode;
    a.send = true;
    a.syncPort = kTestSyncPort;
    a.applyState();
    a.tick();
    REQUIRE(std::strcmp(status(a), "sending") == 0);
    a.send = false;
    a.applyState();
    a.tick();
    CHECK(std::strstr(status(a), "sending") == nullptr);
    a.release();
}

// A MoonLight-format sender without the smoothed bands gets them smoothed here, as a WLED one does, so the bars rise rather than read zero.
TEST_CASE("AudioService Receive: a MoonLight packet without the smoothed bands is smoothed here") {
    FrozenClock clk(1);
    AudioService a;
    openReceiver(a);
    REQUIRE(a.syncOpenForTest());
    AudioFrame peer;
    peer.level = 70; peer.seq = 9;
    for (uint8_t i = 0; i < 16; i++) peer.bands[i] = 200;
    uint8_t pkt[kMlSyncMaxPacket];
    buildMoonLightAudioSync(pkt, peer);
    pkt[6] = 1u << kMlSectionCore;   // the core alone
    REQUIRE(sendUntil(a, pkt, kMlSyncHeaderSize + 1 + kMlSyncCoreSize, [&] { return a.audioFrame()->level == 70; }));
    CHECK(a.audioFrame()->bandsSmoothed[5] > 80);   // one block of the ballistic's rise toward 200
    a.release();
}

// A late frame's hits went in no other packet, since the sender clears them each send, so the next frame carries them rather than losing a drum (review, 2026-11-06).
TEST_CASE("AudioService Receive: a late MoonLight frame's hit rides in the next frame taken") {
    FrozenClock clk(1);
    AudioService a;
    openReceiver(a);
    REQUIRE(a.syncOpenForTest());
    AudioFrame peer;
    peer.level = 50; peer.seq = 100;
    uint8_t pkt[kMlSyncMaxPacket];
    REQUIRE(sendUntil(a, pkt, buildMoonLightAudioSync(pkt, peer), [&] { return a.audioFrame()->level == 50; }));
    peer.seq = 99; peer.onsetLow = 180; peer.onset = 180; peer.level = 51;   // late, with a kick
    const size_t lateN = buildMoonLightAudioSync(pkt, peer);
    CHECK_FALSE(sendUntil(a, pkt, lateN, [&] { return a.audioFrame()->level == 51; }));   // not taken as a frame
    peer.seq = 101; peer.onsetLow = 0; peer.onset = 0; peer.level = 52;   // the next frame, no hit of its own
    REQUIRE(sendUntil(a, pkt, buildMoonLightAudioSync(pkt, peer), [&] { return a.audioFrame()->level == 52; }));
    CHECK(a.audioFrame()->onsetLow == 180);
    a.release();
}
