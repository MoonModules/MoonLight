/// @module DrumKitEffect

/// A drum kit seen from the audience: each drum lights its own place as it is played, and the kit rests dim between hits.

#include "doctest.h"
#include "golden_frame.h"
#include "light/effects/DrumKitEffect.h"
#include "core/services/AudioService.h"

#include <vector>

using namespace mm;

namespace {

/// The effect on a `w` by `h` grid.
struct Kit {
    Layouts layouts; GridLayout grid; Layer layer; DrumKitEffect e;
    explicit Kit(int w = 32, int h = 32) {
        grid.width = w; grid.height = h; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&e);
        layer.applyState();
    }
    /// A light's brightness, its three channels summed.
    int at(int x, int y) const {
        const auto* px = layer.buffer().data();
        const size_t o = (static_cast<size_t>(y) * grid.width + x) * 3;
        return px[o] + px[o + 1] + px[o + 2];
    }
    /// The brightest light in rows `from` up to `to`.
    int brightest(int from, int to) const {
        int peak = 0;
        for (int y = from; y < to; y++) for (int x = 0; x < grid.width; x++) peak = std::max(peak, at(x, y));
        return peak;
    }
    /// Where the brightest light sits, its column.
    int brightestColumn() const {
        int best = -1, peak = 0;
        for (int y = 0; y < grid.height; y++) for (int x = 0; x < grid.width; x++) if (at(x, y) > peak) { peak = at(x, y); best = x; }
        return best;
    }
};

AudioService& seat() {
    static AudioService audio;   // the frame the effect reads
    audio.defineControls();
    audio.setup();
    audio.tick();   // claims the seat latestFrame reads; the frames below are written straight into it
    return audio;
}

/// One frame carrying `f`'s hits at `ms`, then the hits cleared.
void play(Kit& kit, AudioService& audio, const AudioFrame& f, uint32_t ms) {
    const uint16_t seq = audio.frameForTest().seq;
    audio.frameForTest() = f;
    audio.frameForTest().seq = static_cast<uint16_t>(seq + 1);   // a new frame, as a source numbers it
    platform::setTestNowMs(ms);
    kit.layer.tick();
    audio.frameForTest() = AudioFrame{};
    audio.frameForTest().seq = static_cast<uint16_t>(seq + 2);
}

}  // namespace

TEST_CASE("the kit rests dim until a drum hits, and dark with idle at 0") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Kit kit;
    play(kit, audio, AudioFrame{}, 1000);
    CHECK(kit.brightest(0, 32) > 0);
    CHECK(kit.brightest(0, 32) < 3 * 64);
    kit.e.idle = 0;
    play(kit, audio, AudioFrame{}, 1020);
    CHECK(kit.brightest(0, 32) == 0);
    audio.release();
}

TEST_CASE("a kick lights the bass drum, a snare the snare and a hi-hat the stars over the top, each fading back") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Kit kit;
    kit.e.idle = 0;
    AudioFrame f;
    f.onsetLow = 200;
    play(kit, audio, f, 1000);
    CHECK(kit.at(16, 23) > 0);            // the bass drum's face, low in the middle
    CHECK(kit.brightest(0, 8) == 0);      // the top stays dark
    platform::setTestNowMs(1400);         // past the 300 ms glow
    kit.layer.tick();
    CHECK(kit.brightest(0, 32) == 0);

    f = AudioFrame{};
    f.onsetMid = 200;
    play(kit, audio, f, 2000);
    CHECK(kit.at(7, 18) > 0);             // the snare, left of the bass drum
    CHECK(kit.at(16, 23) == 0);

    f = AudioFrame{};
    f.onsetHigh = 200;
    play(kit, audio, f, 3000);
    CHECK(kit.brightest(0, 8) > 0);       // the stars
    CHECK(kit.at(3, 12) > 0);             // the hi-hat
    CHECK(kit.at(16, 23) == 0);
    audio.release();
}

// The render loop runs faster than the audio blocks, so two ticks often read one frame: that is one hit, its stars staying put.
TEST_CASE("a frame two ticks read is one hit: the glow fades on and the stars stay where they fell") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Kit kit;
    kit.e.idle = 0;
    AudioFrame f;
    f.onsetHigh = 200;
    f.seq = 7;
    audio.frameForTest() = f;
    platform::setTestNowMs(1000);
    kit.layer.tick();
    std::vector<int> stars;
    for (int x = 0; x < 32; x++) for (int y = 0; y < 8; y++) if (kit.at(x, y)) stars.push_back(y * 32 + x);
    const int first = kit.brightest(0, 8);
    platform::setTestNowMs(1080);   // the same frame, read again
    kit.layer.tick();
    std::vector<int> again;
    for (int x = 0; x < 32; x++) for (int y = 0; y < 8; y++) if (kit.at(x, y)) again.push_back(y * 32 + x);
    CHECK(again == stars);
    CHECK(kit.brightest(0, 8) < first);   // fading, not started again
    audio.frameForTest() = AudioFrame{};
    audio.release();
}

TEST_CASE("each tom lights its own drum, the floor tom right of the high one") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    Kit kit;
    kit.e.idle = 0;
    AudioFrame f;
    f.thirdsHits[0] = 1u << 6;           // band 6, 80 Hz: the floor tom
    play(kit, audio, f, 1000);
    const int floorTom = kit.brightestColumn();
    f = AudioFrame{};
    f.thirdsHits[1] = 1u << (13 - 8);    // band 13, 400 Hz: the high tom
    play(kit, audio, f, 2000);
    const int highTom = kit.brightestColumn();
    REQUIRE(floorTom >= 0);
    REQUIRE(highTom >= 0);
    CHECK(floorTom > highTom);
    audio.release();
}

TEST_CASE("the kit survives a 1x1 and a 0x0 grid with every drum hitting") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    AudioFrame f;
    f.onsetLow = f.onsetMid = f.onsetHigh = 200;
    f.thirdsHits[0] = 0xFF; f.thirdsHits[1] = 0xFF;
    for (int side : {1, 0}) {
        Kit kit(side, side);
        play(kit, audio, f, 1000);
        platform::setTestNowMs(1100);
        kit.layer.tick();
    }
    audio.release();
}
