/// @module SpriteFountainEffect

#include "doctest.h"
#include "light/effects/SpriteCast.h"
#include "light/effects/SpriteFountainEffect.h"
#include "audio_effect_test.h"   // seat and EffectOnGrid: the audio service and the grid

using namespace mm;

namespace {
constexpr lengthType kSide = 40;   // room for the largest member, 24x24, with a margin on every side

bool lit(RGB c) { return c.r || c.g || c.b; }

/// A kSide square canvas, cleared, and one cast member drawn at its center.
struct Stage {
    Buffer buf;
    draw::Canvas cv;
    static constexpr lengthType c = kSide / 2;
    Stage() {
        REQUIRE(buf.allocate(static_cast<nrOfLightsType>(kSide) * kSide, 3));
        cv = draw::Canvas::of(buf, kSide, kSide, 1);
    }
    void draw(uint8_t kind, uint8_t entry) {
        buf.clear();
        spritecast::draw(cv, kind, entry, c, c, 1, false, 0);
    }
    RGB at(lengthType x, lengthType y) {
        const uint8_t* d = buf.data() + (static_cast<size_t>(y) * kSide + x) * 3;
        return {d[0], d[1], d[2]};
    }
};
}  // namespace

// Every species the fountain throws and Pong's ball can be: each draws something, centered on its position.
TEST_CASE("Every member of the sprite cast draws, centered and inside 24x24") {
    Stage st;
    const lengthType c = Stage::c;
    for (uint8_t kind = 0; kind < spritecast::kKindCount; kind++) {
        CAPTURE(kind);
        st.draw(kind, 0);
        lengthType minX = kSide, minY = kSide, maxX = -1, maxY = -1;
        for (lengthType y = 0; y < kSide; y++)
            for (lengthType x = 0; x < kSide; x++)
                if (lit(st.at(x, y))) {
                    minX = x < minX ? x : minX; maxX = x > maxX ? x : maxX;
                    minY = y < minY ? y : minY; maxY = y > maxY ? y : maxY;
                }
        REQUIRE(maxX >= 0);                               // it drew something
        CHECK(minX >= c - 12); CHECK(maxX < c + 12);      // within 24 wide, centered
        CHECK(minY >= c - 12); CHECK(maxY < c + 12);      // within 24 high, centered
    }
}

// Luna and Momo are characters, not palette swatches: the active palette never recolors them.
TEST_CASE("Luna and Momo keep their own colors") {
    Stage st;
    const lengthType c = Stage::c;
    // The art's (9,6) is the left lens, drawn 12 left and 12 up of the center.
    const RGB lens = mascotart::kPalette[5];
    for (uint8_t kind : {uint8_t(spritecast::kLuna), uint8_t(spritecast::kMomo)}) {
        CAPTURE(kind);
        for (uint8_t entry : {uint8_t(0), uint8_t(128)}) {
            st.draw(kind, entry);
            const RGB got = st.at(c - 12 + 9, c - 12 + 6);
            CHECK(got.r == lens.r); CHECK(got.g == lens.g); CHECK(got.b == lens.b);
        }
    }
}

// A gated frame is heard by its gate whatever its level, and a band throws a sprite from 40 up: 39 throws none, 40 throws.
TEST_CASE("the audio fountain, heard by the gate at a low level, throws a band from 40 up") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    AudioFrame& f = audio.frameForTest();
    f = AudioFrame{};
    f.gate = 40; f.gateOpen = true; f.levelSmoothed = 0;   // under kHeardLevel: only the gate says sound
    f.bands[0] = 39;

    EffectOnGrid<SpriteFountainEffect> fx(32, 32);
    fx.e.audioReactive = true;
    fx.e.rate = 6;   // every loud band throws: the odds are rate * 3 in 16
    uint32_t now = 1000;
    for (int i = 0; i < 4; i++) { platform::setTestNowMs(now += 300); fx.layer.tick(); }   // past several emit beats
    CHECK(fx.e.inFlightForTest() == 0);
    f.bands[0] = 40;
    for (int i = 0; i < 4; i++) { platform::setTestNowMs(now += 300); fx.layer.tick(); }
    CHECK(fx.e.inFlightForTest() > 0);
    audio.release();
}
