/// @module BeatSweepEffect
/// @also golden_frame

/// Where the light is for a beat, the way it travels, and that it draws with no beat at all.

#include "doctest.h"
#include "golden_frame.h"
#include "light/effects/BeatSweepEffect.h"
#include "light/effects/BeatRipplesEffect.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"

#include <string>

using namespace mm;

namespace {
/// A running beat: `count`, `phase` and the bar's `inBar`.
BeatNow beatAt(uint16_t count, uint8_t phase, uint8_t inBar = 0) {
    BeatNow b;
    b.bpm100 = 12000;
    b.count = count;
    b.phase = phase;
    b.inBar = inBar;
    return b;
}
}  // namespace

TEST_CASE("the light crosses once a beat, and every other crossing comes back") {
    BeatSweepEffect e;
    CHECK(e.sweepAt(beatAt(0, 0), 0).pos == 0);
    const uint16_t mid = e.sweepAt(beatAt(0, 128), 0).pos;
    CHECK(mid > 30000);
    CHECK(mid < 35000);
    const BeatSweepEffect::Sweep back = e.sweepAt(beatAt(1, 0), 0);   // the second beat starts at the far side
    CHECK_FALSE(back.forward);
    CHECK(back.pos == 65535);
    e.bounce = false;
    CHECK(e.sweepAt(beatAt(1, 0), 0).pos == 0);   // bounce off: every crossing starts over
}

TEST_CASE("on bar, a crossing takes four beats and lands with the bar's end") {
    BeatSweepEffect e;
    e.cycle = BeatSweepEffect::kBar;
    CHECK(e.sweepAt(beatAt(0, 0, 1), 0).pos == 0);
    const uint16_t third = e.sweepAt(beatAt(2, 0, 3), 0).pos;   // the bar's third beat: half way
    CHECK_FALSE(e.sweepAt(beatAt(4, 0, 1), 0).forward);          // the next bar comes back
    CHECK(third > 32000);
    CHECK(third < 33500);
    CHECK(e.sweepAt(beatAt(4, 0, 1), 0).downbeat);
    CHECK_FALSE(e.sweepAt(beatAt(5, 0, 2), 0).downbeat);
    // A source whose count is not a multiple of four at the bar's start: the direction still turns with its bar.
    CHECK(e.sweepAt(beatAt(3, 255, 3), 0).forward == e.sweepAt(beatAt(4, 0, 4), 0).forward);
    CHECK(e.sweepAt(beatAt(4, 0, 4), 0).forward != e.sweepAt(beatAt(5, 0, 1), 0).forward);
}

TEST_CASE("with no beat the light crosses at the effect's own tempo") {
    BeatSweepEffect e;
    e.tempo = 120;   // 500 ms a crossing
    const BeatNow none;
    CHECK(e.sweepAt(none, 0).pos == 0);
    const uint16_t quarter = e.sweepAt(none, 125).pos;
    CHECK(quarter > 15000);
    CHECK(quarter < 17500);
    CHECK_FALSE(e.sweepAt(none, 500).forward);   // the second crossing comes back
    CHECK_FALSE(e.sweepAt(none, 0).downbeat);
}

TEST_CASE("the light is full across its glow and fades behind it, never ahead") {
    CHECK(BeatSweepEffect::lightAt(0, true, 2, 10) == 255);
    CHECK(BeatSweepEffect::lightAt(2, true, 2, 10) == 255);
    CHECK(BeatSweepEffect::lightAt(5, true, 2, 10) == 0);       // ahead of a light moving forward
    const uint8_t near = BeatSweepEffect::lightAt(-4, true, 2, 10);
    const uint8_t far = BeatSweepEffect::lightAt(-10, true, 2, 10);
    CHECK(near > far);
    CHECK(far > 0);
    CHECK(BeatSweepEffect::lightAt(-13, true, 2, 10) == 0);      // past the tail
    CHECK(BeatSweepEffect::lightAt(4, false, 2, 10) > 0);        // traveling back, its tail is on the other side
    CHECK(BeatSweepEffect::lightAt(-6, true, 2, 0) == 0);        // no trail
}

TEST_CASE("in silence the light goes out, so a stopped song is dark") {
    golden::ScopedTestClock clock(1000);
    golden::LayerFixture fx(32, 8);
    fx.add(fx.own<BeatSweepEffect>());
    for (uint32_t ms = 1000; ms < 2000; ms += 20) { platform::setTestNowMs(ms); fx.layer.tick(); }
    CHECK(golden::litLights(fx.frame()) == 0);   // no audio source: no bands and no light
}

TEST_CASE("the light comes back quickly with sound and fades slowly in silence") {
    CHECK(BeatSweepEffect::presenceAfter(0, true, 120) == 65535);
    CHECK(BeatSweepEffect::presenceAfter(0, true, 60) > 25000);
    CHECK(BeatSweepEffect::presenceAfter(65535, false, 100) > 46000);   // a fifth of the way out after 100 ms
    CHECK(BeatSweepEffect::presenceAfter(65535, false, 500) == 0);
    CHECK(BeatSweepEffect::presenceAfter(65535, false, 60000) == 0);    // a long gap clamps, no wrap
    uint16_t p = 65535;
    for (int i = 0; i < 500; i++) p = BeatSweepEffect::presenceAfter(p, false, 1);   // 1 ms frames
    CHECK(p < 300);   // still fades out in about 500 ms
}

TEST_CASE("the beat's text names the tempo and the beat in the bar") {
    char out[12];
    CHECK(std::string(BeatSweepEffect::beatText(beatAt(0, 0, 3), out, sizeof(out))) == "120 3/4");
    CHECK(std::string(BeatSweepEffect::beatText(beatAt(0, 0, 0), out, sizeof(out))) == "120");
}

namespace {
/// An active AudioService playing the simulated song, whose beat is 100 BPM: a beat every 600 ms.
struct SimulatedSong {
    struct ClockGuard { ~ClockGuard() { platform::setTestNowMs(0); } } guard;
    AudioService audio;
    SimulatedSong() {
        platform::setTestNowMs(1000);
        audio.defineControls();
        audio.simulate = AudioService::kSimMusic;
        audio.setup();
        audio.tick();
    }
    ~SimulatedSong() { audio.release(); }
};
}  // namespace

TEST_CASE("MoonLive's beat builtins read the beat the effects follow") {
    SimulatedSong song;
    CHECK(moonlive::mm_light_bpm(nullptr, 0, nullptr) == 100);
    const uint32_t bar = moonlive::mm_light_beatInBar(nullptr, 0, nullptr);
    CHECK(bar >= 1);
    CHECK(bar <= 4);
    int beats = 0;
    for (uint32_t ms = 1000; ms < 2800; ms += 20) {   // three beats of the song
        platform::setTestNowMs(ms);
        song.audio.tick();
        beats += static_cast<int>(moonlive::mm_light_onBeat(nullptr, 0, nullptr));
    }
    CHECK(beats == 3);
}

TEST_CASE("BeatRipples on beat drops a stone on each beat of the song") {
    SimulatedSong song;
    golden::LayerFixture fx(32, 32);
    auto& e = fx.own<BeatRipplesEffect>();
    e.trigger = BeatRipplesEffect::kBeat;
    e.rain = 0;
    fx.add(e);
    for (uint32_t ms = 1000; ms < 2800; ms += 20) {
        platform::setTestNowMs(ms);
        song.audio.tick();
        fx.layer.tick();
    }
    CHECK(e.stonesForTest() == 3);
}
