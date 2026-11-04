/// @module AudioService

#include "doctest.h"
#include "core/services/AudioService.h"

#include <cstring>

// The MoonModule lifecycle of the audio peripheral, host-side (hasI2sMic false: the platform-inert path of a mic-less board): setup/release repeats cleanly, latestFrame() stays coherent through any add/remove order, an unconfigured module idles.

// active_ is process-wide static and shared with the cases in unit_AudioLevel.cpp; each case here brackets its own setup() with a release() so it never leaks a live mic pointer into another test (the residue the robustness rule forbids).

TEST_CASE("AudioService: a fresh, unconfigured module is idle (pins default unset)") {
    mm::AudioService a;
    a.defineControls();
    // Pins default to -1 (unset, the Pin-control sentinel; not 0, so GPIO 0 stays usable): merely existing must never init a mic, the auto-init-on-boot path that hung a mic-less classic.
    CHECK(a.wsPin == -1);
    CHECK(a.sdPin == -1);
    CHECK(a.sckPin == -1);
    a.setup();           // settles the status; no I2S touched on host or unset pins
    a.tick();            // must be a quiet no-op, not a crash, with nothing inited
    a.release();
    CHECK(true);
}

TEST_CASE("AudioService: setup/release is repeatable with no residual state") {
    mm::AudioService a;
    a.defineControls();
    const char* afterFirst = nullptr;
    for (int cycle = 0; cycle < 4; cycle++) {
        a.setup();
        a.applyState();   // reinit, as the Scheduler does
        a.tick();           // a tick: no heap churn, no crash (ASAN across cycles)
        a.release();
        // The status settles to one stable value (a changing pointer would show a leak): the platform-inert note on host, the "set pins" note on a mic target.
        if (cycle == 0) afterFirst = a.status();
        else CHECK(a.status() == afterFirst);
    }
}

TEST_CASE("AudioService: release clears the active mic (latestFrame falls back to silence)") {
    {
        mm::AudioService a;
        a.defineControls();
        a.applyState();                             // build (enabled) registers itself as active_
        REQUIRE(mm::AudioService::latestFrame() == a.audioFrame());
        a.release();                               // must release the registration
    }
    // After release (and destruction) no mic is active: the accessor returns the static silent frame, never a dangling pointer into the destroyed module.
    const mm::AudioFrame* f = mm::AudioService::latestFrame();
    REQUIRE(f != nullptr);
    CHECK(f->level == 0);
    CHECK(f->peakHz == 0);
}

TEST_CASE("AudioService: two mics — first wins, survivor re-elects, any order stays coherent") {
    // Two AudioServices (two mics): removing the ACTIVE mic used to leave active_ null and silence every effect. First live module wins the seat, release() vacates it, a running module re-claims an empty seat in tick().
    mm::AudioService a, b;
    a.defineControls();
    b.defineControls();

    a.applyState();                                     // build (enabled) claims the seat
    CHECK(mm::AudioService::latestFrame() == a.audioFrame());
    b.applyState();                                     // second mic captured, but a keeps the seat
    CHECK(mm::AudioService::latestFrame() == a.audioFrame());   // FIRST wins, not last

    a.release();                                       // tearing down the ACTIVE one vacates the seat
    // Before the survivor's next tick the seat is empty; b re-claims it in tick() (self-election).
    b.tick();                                           // b (still live) takes over
    CHECK(mm::AudioService::latestFrame() == b.audioFrame());   // effects keep reading a live frame
    b.release();
    // Both gone: back to the static silence, no dangling pointer.
    CHECK(mm::AudioService::latestFrame()->level == 0);
}

TEST_CASE("AudioService: a DISABLED module does not win the mic election at boot") {
    // applyState() calls prepare() (acquire + election) only when effectively-enabled: a persisted DISABLED AudioService must NOT grab the I2S mic or win the active_ seat at boot, or it silences an enabled sibling.
    mm::AudioService dis, live;
    dis.defineControls();
    live.defineControls();
    // The seat starts empty: any module a prior case left holding it has since destructed, and ~AudioService vacates on destruction (no dangling static, see the destructor).
    REQUIRE(mm::AudioService::latestFrame()->level == 0);

    dis.setEnabled(false);        // persisted-disabled at boot
    dis.setup();                  // Phase 3: pure wiring, no election
    dis.applyState();             // Phase 4: disabled → routes to release, NOT the election
    // Still empty: the disabled module did NOT take the seat.
    CHECK(mm::AudioService::latestFrame() != dis.audioFrame());
    CHECK(mm::AudioService::latestFrame()->level == 0);

    live.setup();
    live.applyState();            // an enabled sibling boots (build) and wins the seat
    CHECK(mm::AudioService::latestFrame() == live.audioFrame());

    // Now enable the previously-disabled one and re-run its applyState (the post-toggle sweep): it must NOT steal the live seat (first live wins).
    dis.setEnabled(true);
    dis.applyState();
    CHECK(mm::AudioService::latestFrame() == live.audioFrame());   // live keeps the seat

    live.release();
    dis.release();
    CHECK(mm::AudioService::latestFrame()->level == 0);
}

// Regression: a tick that takes several blocks kept only the newest, so a hit in an earlier block of the same tick never reached an effect that reads the frame once per tick.
TEST_CASE("AudioService: a hit in an earlier block of one tick still reaches the effects") {
    mm::SpscRing<mm::AudioFrame, 8> ring;
    mm::AudioFrame hit, after, newest;
    hit.onset = 180;
    after.onset = 0;
    newest.onset = 0;
    newest.level = 42;
    ring.push(&hit, 1);
    ring.push(&after, 1);
    ring.push(&newest, 1);
    mm::AudioFrame out;
    REQUIRE(mm::AudioService::takeNewest(ring, out));
    CHECK(out.level == 42);    // the newest frame's analysis
    CHECK(out.onset == 180);   // with the hit the blocks before it heard
    CHECK_FALSE(mm::AudioService::takeNewest(ring, out));   // drained: nothing fresh until the next block
}
