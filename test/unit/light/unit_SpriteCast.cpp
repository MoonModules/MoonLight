/// @module SpriteFountainEffect

#include "doctest.h"
#include "light/effects/SpriteCast.h"
#include "light/effects/SpriteFountainEffect.h"
#include "audio_effect_test.h"   // seat and EffectOnGrid: the audio service and the grid
#include "platform/platform_clock.h"   // setTestNowMs

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

TEST_CASE("the audio fountain throws one sprite per drum hit, once however many ticks read the frame") {
    golden::ScopedTestClock clock(1000);
    AudioService& audio = seat();
    AudioFrame& f = audio.frameForTest();
    f = AudioFrame{};                 // no band loud and no sound heard: only a hit throws
    EffectOnGrid<SpriteFountainEffect> fx(32, 32);
    fx.e.audioReactive = true;
    uint32_t now = 1000;
    platform::setTestNowMs(now += 20); fx.layer.tick();
    CHECK(fx.e.inFlightForTest() == 0);
    f.seq = 1; f.onsetLow = 200; f.onsetMid = 120;   // a kick and a snare on one block
    for (int i = 0; i < 3; i++) { platform::setTestNowMs(now += 20); fx.layer.tick(); }
    CHECK(fx.e.inFlightForTest() == 2);
    f.seq = 2; f.onsetLow = 0; f.onsetMid = 0; f.onsetHigh = 90;   // a hat on the next
    platform::setTestNowMs(now += 20); fx.layer.tick();
    CHECK(fx.e.inFlightForTest() == 3);
    audio.release();
}

namespace {
// One sprite frame drawn on a canvas of its own size, each palette index as its own red value, so a pixel reads back as its index.
struct Drawn {
    Buffer buf;
    draw::Canvas cv;
    Drawn(const draw::sprites::Sprite& s, uint8_t frame, bool flip) {
        buf.allocate(static_cast<nrOfLightsType>(s.w) * s.h, 3);
        buf.clear();
        cv = draw::Canvas::of(buf, s.w, s.h, 1);
        draw::sprite(cv, s, frame, 0, 0, 1, flip);
    }
    uint8_t at(int x, int y) const { return buf.data()[(static_cast<size_t>(y) * cv.dims.x + x) * 3]; }
};

// How many pupil edges the frame shows with the white of the eye beside them, on the side `whiteDx` names; -1 when a pupil has something else there.
int pupilsBeside(const draw::sprites::Sprite& s, uint8_t frame, bool flip, uint8_t pupil, uint8_t white, int whiteDx) {
    const Drawn d(s, frame, flip);
    int n = 0;
    for (int y = 0; y < s.h; y++)
        for (int x = 0; x < s.w; x++) {
            if (d.at(x, y) != pupil) continue;
            const int wx = x + whiteDx;
            if (wx < 0 || wx >= s.w) return -1;
            if (d.at(wx, y) == pupil) continue;   // a wider pupil's own pixel
            if (d.at(wx, y) != white) return -1;
            n++;
        }
    return n;
}

// A palette whose every entry reads back as its index.
template <size_t N>
void indexPalette(RGB (&pal)[N]) {
    for (size_t i = 0; i < N; i++) pal[i] = RGB{static_cast<uint8_t>(i), 0, 0};
}
}  // namespace

TEST_CASE("An invader and a ghost look where they travel: the pupil leads, the white of the eye behind it") {
    RGB inv[invart::kPaletteCount], ghost[moonart::kPaletteCount];
    indexPalette(inv);
    indexPalette(ghost);
    struct Member { draw::sprites::Sprite sprite; uint8_t pupil, white; int edges; };
    const Member cast[] = {
        {{invart::kSquid, inv, invart::W, invart::H, invart::F, invart::kPaletteCount}, invart::kPupil, invart::kEye, 2},
        {{invart::kCrab, inv, invart::CW, invart::CH, invart::CF, invart::kPaletteCount}, invart::kPupil, invart::kEye, 2},
        {{invart::kOcto, inv, invart::OW, invart::OH, invart::OF, invart::kPaletteCount}, invart::kPupil, invart::kEye, 2},
        {{moonart::kGhostSmall, ghost, moonart::SW, moonart::SH, moonart::SF, moonart::kPaletteCount}, moonart::kPupil, moonart::kEye, 2},
        {{moonart::kGhost, ghost, moonart::GW, moonart::GH, moonart::GF, moonart::kPaletteCount}, moonart::kPupil, moonart::kEye, 4},   // two eyes, each pupil two rows high
    };
    for (const Member& m : cast)
        for (uint8_t f = 0; f < m.sprite.frames; f++) {
            CHECK(pupilsBeside(m.sprite, f, false, m.pupil, m.white, -1) == m.edges);   // traveling right: the white on the left
            CHECK(pupilsBeside(m.sprite, f, true, m.pupil, m.white, +1) == m.edges);    // traveling left: mirrored
        }
}

TEST_CASE("A fish looks where it swims with one white-and-pupil eye, and closes it at rest") {
    RGB idx[fishart::kPaletteCount];
    indexPalette(idx);
    const draw::sprites::Sprite fish[] = {
        {fishart::kFish, idx, fishart::W, fishart::H, fishart::F, fishart::kPaletteCount},
        {fishart::kSlim, idx, fishart::SW, fishart::SH, fishart::SF, fishart::kPaletteCount},
    };
    for (const auto& s : fish)
        for (uint8_t f = 0; f < s.frames; f++) {
            CHECK(pupilsBeside(s, f, false, fishart::kEye, fishart::kEyeWhite, -1) == 1);   // swimming right: the white behind the pupil
            CHECK(pupilsBeside(s, f, true, fishart::kEye, fishart::kEyeWhite, +1) == 1);    // swimming left: mirrored
        }
    RGB pal[fishart::kPaletteCount];
    fishart::fishPalette(pal, 40, /*moving=*/true);
    CHECK(pal[fishart::kEyeWhite].r == 255);
    CHECK(pal[fishart::kEye].r < 40);
    fishart::fishPalette(pal, 40, /*moving=*/false);
    CHECK(pal[fishart::kEyeWhite].r == pal[fishart::kEye].r);   // at rest the whole eye is the pupil's dark
}

TEST_CASE("FishTank's fish open their eyes while they swim and close them in a tank at rest") {
    struct ClockGuard { ~ClockGuard() { platform::setTestNowMs(0); } } guard;
    const auto whites = [](const EffectOnGrid<FishTankEffect>& t) {
        int n = 0;
        for (int y = 0; y < 48; y++)
            for (int x = 0; x < 96; x++) n += t.ch(x, y, 0) == 255 && t.ch(x, y, 1) == 255 && t.ch(x, y, 2) == 255;
        return n;
    };
    platform::setTestNowMs(1000);
    EffectOnGrid<FishTankEffect> swimming(96, 48);
    swimming.layer.tick();
    CHECK(whites(swimming) > 0);
    EffectOnGrid<FishTankEffect> resting(96, 48);
    resting.e.speed = 0;
    resting.e.onControlChanged("speed");
    resting.layer.applyState();
    resting.layer.tick();
    CHECK(whites(resting) == 0);
}
