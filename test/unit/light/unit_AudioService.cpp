/// @module AudioService

#include "doctest.h"
#include "core/services/AudioService.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

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
    REQUIRE(mm::takeNewest(ring, out));
    CHECK(out.level == 42);    // the newest frame's analysis
    CHECK(out.onset == 180);   // with the hit the blocks before it heard
    CHECK_FALSE(mm::takeNewest(ring, out));   // drained: nothing fresh until the next block
}

namespace {

// `seconds` of a quiet hiss, or of these tones, through the service's own analysis at 24 kHz; answers the last block's frame.
const mm::AudioFrame& analyze(mm::AudioService& audio, std::initializer_list<float> hz, float seconds, double& t) {
    static int32_t block[mm::AudioService::kBlock];
    static uint32_t seed = 1;
    const mm::AudioFrame* f = nullptr;
    for (float done = 0; done < seconds; done += static_cast<float>(mm::AudioService::kBlock) / 24000.0f) {
        for (size_t i = 0; i < mm::AudioService::kBlock; i++) {
            seed = seed * 1664525u + 1013904223u;
            double v = static_cast<double>(static_cast<int32_t>(seed) >> 26);   // a hiss of a few 24-bit counts
            for (const float h : hz) v += 100000.0 * std::sin(2.0 * 3.14159265358979 * static_cast<double>(h) * (t + static_cast<double>(i) / 24000.0));
            block[i] = static_cast<int32_t>(v) * 256;   // left-justified, as the input delivers 24-bit samples
        }
        t += static_cast<double>(mm::AudioService::kBlock) / 24000.0;
        f = &audio.analyzeForTest(block);
    }
    return *f;
}

}  // namespace

TEST_CASE("AudioService: harmony off measures no notes, and on again they come back") {
    static mm::AudioService audio;
    audio.defineControls();
    audio.resetAnalysisForTest();
    double t = 0;
    analyze(audio, {}, 1.0f, t);   // the room's floor learned
    const std::initializer_list<float> cMajor = {261.63f, 329.63f, 392.0f};
    const mm::AudioFrame& on = analyze(audio, cMajor, 1.5f, t);
    CHECK(on.gateOpen);
    CHECK(on.notesCount == 79);
    CHECK(on.chroma[0] > 200);   // C
    audio.harmony = false;
    const mm::AudioFrame& off = analyze(audio, cMajor, 0.2f, t);
    CHECK(off.notesCount == 0);
    for (const uint8_t v : off.chroma) CHECK(v == 0);
    CHECK_FALSE(off.harmonyChange);
    audio.harmony = true;
    const mm::AudioFrame& again = analyze(audio, cMajor, 1.5f, t);
    CHECK(again.notesCount == 79);
    CHECK(again.chroma[0] > 200);
}

// The analysis keeps the stream's time, so audio fed faster than real time, a file through the host or a backlog on a busy core, reads every hit the music has.
TEST_CASE("AudioService: hits fed faster than real time each read as an onset, the stream keeping its own time") {
    struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } } guard;
    mm::platform::setTestNowMs(5000);   // the wall clock stands still for the whole run
    static mm::AudioService audio;
    audio.defineControls();
    audio.resetAnalysisForTest();
    static int32_t block[mm::AudioService::kBlock];
    uint32_t seed = 7;
    int onsets = 0;
    for (int b = 0; b < 47 * 3; b++) {   // 3 s of 21 ms blocks: a second of room, then a hit every 12 blocks, a quarter second apart
        const bool hit = b >= 47 && (b - 47) % 12 == 0;
        for (auto& s : block) {
            seed = seed * 1664525u + 1013904223u;
            s = (static_cast<int32_t>(seed) >> (hit ? 9 : 26)) * 256;   // a loud broadband burst, or the room's hiss
        }
        if (audio.analyzeForTest(block).onset) onsets++;
    }
    CHECK(onsets == 8);   // every hit, though not a millisecond of wall time passed
}

TEST_CASE("AudioService: a chord change in an earlier block of one tick still reaches the effects") {
    mm::SpscRing<mm::AudioFrame, 8> ring;
    mm::AudioFrame changed, after;
    changed.harmonyChange = true;
    ring.push(&changed, 1);
    ring.push(&after, 1);
    mm::AudioFrame out;
    REQUIRE(mm::takeNewest(ring, out));
    CHECK(out.harmonyChange);   // a palette following the chords misses none at a slow frame rate
}

// A recording tuned off A440 smears each note across two semitone bands; once the music has sat off long enough, the bank retunes to it.
TEST_CASE("AudioService: music 40 cents sharp retunes the semitone bank toward it") {
    static mm::AudioService audio;
    audio.defineControls();
    audio.resetAnalysisForTest();
    double t = 0;
    analyze(audio, {}, 1.0f, t);   // the room
    const float sharp = 440.0f * std::exp2(40.0f / 1200.0f);
    analyze(audio, {sharp, sharp * 1.25992f, sharp * 1.49831f}, 45.0f, t);   // an A major triad, 40 cents sharp
    CHECK(audio.tuningForTest() > 30.0f);   // retuned, within the reading's three quarters per step
    CHECK(audio.tuningForTest() < 50.0f);
}

// Regression: the grid followed the tuned reference, so a flat retune rounded A4's band to 53 and every note read a semitone up, C major as C# F G# (pre-commit review, 2026-11-04).
TEST_CASE("AudioService: music 40 cents flat retunes the bank and every note keeps its name") {
    static mm::AudioService audio;
    audio.defineControls();
    audio.resetAnalysisForTest();
    double t = 0;
    analyze(audio, {}, 1.0f, t);
    const float flat = 440.0f * std::exp2(-40.0f / 1200.0f);
    const std::initializer_list<float> chord = {flat, flat * 1.25992f, flat * 1.49831f};   // A major, 40 cents flat
    for (int phrase = 0; phrase < 4; phrase++) {   // phrases with a breath between, so the floor stays on the room
        analyze(audio, chord, 10.0f, t);
        analyze(audio, {}, 0.5f, t);
    }
    const mm::AudioFrame& f = analyze(audio, chord, 3.0f, t);
    REQUIRE(f.gateOpen);
    CHECK(audio.tuningForTest() < -30.0f);
    CHECK(f.notesCount == 79);                                // every key still measured
    uint8_t loudest = 0;
    for (uint8_t c = 1; c < 12; c++) if (f.chroma[c] > f.chroma[loudest]) loudest = c;
    CHECK(((loudest == 9) || (loudest == 1) || (loudest == 4)));   // A, C# or E: the chord's own notes, not a semitone up
}

// Every frame a source makes carries a new `seq`, so an effect that reads one frame on two ticks, the Drum Kit, takes its hits once and misses none.
TEST_CASE("AudioService: each analyzed block and each simulated frame is a new frame number") {
    static mm::AudioService audio;
    audio.defineControls();
    audio.resetAnalysisForTest();
    static int32_t block[mm::AudioService::kBlock] = {};
    const uint16_t first = audio.analyzeForTest(block).seq;
    CHECK(audio.analyzeForTest(block).seq == static_cast<uint16_t>(first + 1));   // the same silence, a new frame
    audio.release();
    static mm::AudioService sim;
    sim.defineControls();
    sim.mode = mm::AudioService::kSimMode;
    sim.setup();
    sim.tick();
    const uint16_t simFirst = sim.audioFrame()->seq;
    sim.tick();
    CHECK(sim.audioFrame()->seq == static_cast<uint16_t>(simFirst + 1));
    sim.release();
}

// Memory follows the mode: the analysis is held only while an input is open, the harmony's state only while `harmony` is on, so a receiver or a simulation spends no internal RAM on either.
TEST_CASE("AudioService: simulate and receive hold no analysis and no harmony state") {
    for (const uint8_t mode : {mm::AudioService::kSimMode, mm::AudioService::kReceiveMode}) {
        CAPTURE(int(mode));
        static mm::AudioService audio;
        audio.defineControls();
        audio.mode = mode;
        audio.setup();
        audio.prepare();
        CHECK_FALSE(audio.analysisHeldForTest());
        CHECK_FALSE(audio.harmonyHeldForTest());
        audio.release();
    }
}

TEST_CASE("AudioService: the harmony state goes with harmony off and comes back on, the analysis staying") {
    static mm::AudioService audio;
    audio.defineControls();
    audio.resetAnalysisForTest();   // an input opening, with harmony on by default
    REQUIRE(audio.analysisHeldForTest());
    CHECK(audio.harmonyHeldForTest());
    double t = 0;
    audio.harmony = false;
    analyze(audio, {}, 0.1f, t);
    CHECK(audio.analysisHeldForTest());
    CHECK_FALSE(audio.harmonyHeldForTest());
    audio.harmony = true;
    analyze(audio, {}, 0.1f, t);
    CHECK(audio.harmonyHeldForTest());
}

TEST_CASE("AudioService: leaving local mode frees the analysis and the harmony state") {
    static mm::AudioService audio;
    audio.defineControls();
    audio.resetAnalysisForTest();
    REQUIRE(audio.analysisHeldForTest());
    audio.mode = mm::AudioService::kSimMode;   // a live switch away from the input
    audio.prepare();
    CHECK_FALSE(audio.analysisHeldForTest());
    CHECK_FALSE(audio.harmonyHeldForTest());
    audio.release();
}

namespace {

// A drum hit through the analysis after a second of the room: a kick's decaying 60 Hz with a soft 5 ms attack, so no click, or a hi-hat's noise tilted toward the top; answers each range's strongest onset.
void hit(mm::AudioService& audio, bool kick, uint8_t (&fired)[3]) {
    double t = 0;
    analyze(audio, {}, 1.0f, t);
    static int32_t block[mm::AudioService::kBlock];
    uint32_t seed = 11;
    float hpState = 0.0f, prev = 0.0f;
    fired[0] = fired[1] = fired[2] = 0;
    for (size_t b = 0; b < 24; b++) {   // half a second
        for (size_t i = 0; i < mm::AudioService::kBlock; i++) {
            const double s = static_cast<double>(b * mm::AudioService::kBlock + i) / 24000.0;
            seed = seed * 1664525u + 1013904223u;
            double v = static_cast<double>(static_cast<int32_t>(seed) >> 26);   // the room
            const double attack = s < 0.005 ? 0.5 * (1.0 - std::cos(3.14159265358979 * s / 0.005)) : 1.0;   // a click would reach the treble first: part 4's case
            if (kick) v += 2.0e6 * attack * std::exp(-s * 12.0) * std::sin(2.0 * 3.14159265358979 * 60.0 * s);
            else {
                const float noise = static_cast<float>(static_cast<int32_t>(seed) >> 12);
                const float diff = noise - prev;              // a second difference: noise tilted 12 dB an octave toward the top, a hat's
                v += static_cast<double>(diff - hpState) * std::exp(-s * 30.0);
                hpState = diff;
                prev = noise;
            }
            block[i] = static_cast<int32_t>(v) * 256;
        }
        const mm::AudioFrame& f = audio.analyzeForTest(block);
        fired[0] = f.onsetLow > fired[0] ? f.onsetLow : fired[0];
        fired[1] = f.onsetMid > fired[1] ? f.onsetMid : fired[1];
        fired[2] = f.onsetHigh > fired[2] ? f.onsetHigh : fired[2];
    }
}

}  // namespace

// Step 5 of the audio plan: an onset per range, so a kick and a hi-hat each fire their own.
TEST_CASE("AudioService: a kick without its click fires the low onset and not the high one, a hi-hat the reverse") {
    static mm::AudioService audio;
    audio.defineControls();
    audio.resetAnalysisForTest();
    uint8_t fired[3];
    hit(audio, true, fired);
    CHECK(fired[0] > 0);
    CHECK(fired[2] == 0);
    audio.resetAnalysisForTest();
    hit(audio, false, fired);
    CHECK(fired[2] > 0);
    CHECK(fired[0] == 0);
}
