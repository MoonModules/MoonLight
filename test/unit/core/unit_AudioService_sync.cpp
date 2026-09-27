/// @module AudioService
/// @also WledAudioSyncPacket
///
/// Drives AudioService's WLED audio-sync socket lifecycle through the public tick(), the entry the scheduler calls on a device.
///
/// @moreinfo
///
/// ## What it covers
///
/// The lazy open, once per mode, where syncEnsureSocket latches.
/// The send path reaching "sending", and the throttle that paces it.
/// The receive path over a real localhost round-trip: a frame replaced, then the fresh-to-stale fallback a pure sink falls back to.
/// platform::networkReady() is true on desktop, so the lazy open fires on the first tick the way it does once a device's interface is up.
///
/// ## Leaving local clears the mic status
///
/// The other modes report through the separate sync row and have no input to diagnose, so a stale mic message there points at nothing.
/// Before the fix, prepare()'s non-local branch deinitialized the peripheral and left the string set.
/// What local leaves behind depends on the host: a capture-capable desktop says nothing, a locked-down one says "capture init failed", an I2S target with unset pins asks for its pins.
/// The rule is the same in every case: whatever local left, leaving local clears.
///
/// ## Capture and send coexist
///
/// Send fires from the same tick() that runs the capture path, so the capture gate cannot starve the sender.
/// Before capture existed, tick() returned before ever sending, which made local-mode sending impossible.
///
/// ## Forcing a bind to fail
///
/// Hogging the port with a second socket is not portable.
/// On Linux, SO_REUSEADDR on a UDP socket bound to INADDR_ANY permits the overlapping bind, so the hog succeeds and the failure never happens.
/// That silently broke this test on Linux for as long as it existed, and nothing caught it because CI did not compile the C++ tests until the sanitizer job.
/// A privileged port is no better, since modern macOS lets a non-root process bind port 80.
///
/// ## Why the clock is fake and the socket is real
///
/// Time comes from platform::setTestNowMs(), the animation-test idiom, so a throttle window is exact and the suite never sleeps a real second.
/// Only the delivery is real, polled with a bounded retry the way the NetworkReceiveEffect round-trip does.
/// The port is a test one rather than 11988, since a running desktop app would hold the real sync port.

#include "doctest.h"
#include "core/services/AudioService.h"
#include "light/util/WLEDAudioSyncPacket.h"
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
    FrozenClock(uint32_t ms) { platform::setTestNowMs(ms); }
    ~FrozenClock() { platform::setTestNowMs(0); }
    void advance(uint32_t ms) { now_ += ms; platform::setTestNowMs(now_); }
    uint32_t now_ = 1;
};
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
    CHECK(std::strcmp(status(a), "listening") == 0);

    // Send a real WLED v2 packet to the bound port over loopback.
    AudioFrame peer;
    peer.level = 222; peer.levelSmoothed = 111; peer.peakHz = 660; peer.peakMag = 55;
    for (int i = 0; i < 16; i++) peer.bands[i] = static_cast<uint8_t>(i * 8);
    uint8_t pkt[WLED_SYNC_PACKET_SIZE];
    buildWledAudioSync(pkt, peer, /*peak=*/false);

    platform::UdpSocket tx;
    REQUIRE(tx.open());
    REQUIRE(tx.connect("127.0.0.1", kTestSyncPort));
    REQUIRE(tx.sendTo(pkt, WLED_SYNC_PACKET_SIZE));

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
    buildWledAudioSync(pkt, quiet, /*peak=*/false);
    REQUIRE(tx.sendTo(pkt, WLED_SYNC_PACKET_SIZE));
    bool fell = false;
    for (int i = 0; i < 100 && !fell; i++) {
        a.tick();
        fell = a.audioFrame()->level == 0;
        if (!fell) platform::delayMs(1);
    }
    CHECK(fell);
    CHECK(a.audioFrame()->bandsSmoothed[15] > 40);   // still falling, not reset by the copy
    // Named, not just "receiving": the packet came from loopback, so the status has to say so. A receiver that cannot name its source looks identical to one locked onto the wrong device.
    CHECK(std::strcmp(status(a), "receiving from 127.0.0.1") == 0);

    // A pure sink with no packet goes stale and falls back to "listening", holding its last frame.
    clk.advance(AudioService::syncFallbackMsForTest() + 20);
    a.tick();
    CHECK(std::strcmp(status(a), "listening") == 0);

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
