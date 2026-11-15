/// @module MoonFlockEffect

#include "doctest.h"
#include "golden_frame.h"
#include "light/effects/MoonFlockEffect.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace {

/// A MoonFlock on a grid, Aurora's 32 by 32 unless asked, held across ticks, with the audio source whose frames it hears, or with none when `listening` is off.
struct KeysRig {
    mm::golden::ScopedTestClock clock{1000};
    mm::AudioService audio;
    mm::golden::LayerFixture fx;
    mm::Layer& layer = fx.layer;
    mm::MoonFlockEffect flock;
    bool listening;
    uint32_t now = 1000;

    explicit KeysRig(uint8_t growth, mm::lengthType w = 32, mm::lengthType h = 32, uint8_t cpl = 3, bool listening = true)
        : fx(w, h, 1, cpl), listening(listening) {
        if (listening) {
            audio.defineControls();
            audio.setup();
            audio.tick();   // claims the seat latestFrame reads
        }
        flock.growth = growth;
        flock.dock = mm::MoonFlockEffect::kDockBottom;   // a fixed edge; the docks have tests of their own
        fx.add(flock);
    }
    ~KeysRig() {
        if (listening) audio.release();
    }

    /// Strike `keys` loud on a new frame, as the core reports a kept note's attack, and tick once to hear them.
    void play(std::initializer_list<uint8_t> keys) {
        mm::AudioFrame& f = audio.frameForTest();
        std::memset(f.notesKeptHits, 0, sizeof(f.notesKeptHits));
        f.notesCount = mm::kAudioNoteKeys;
        for (uint8_t k : keys) {
            f.notes[k] = 255;
            f.notesKept[k >> 3] |= static_cast<uint8_t>(1u << (k & 7));
            f.notesKeptHits[k >> 3] |= static_cast<uint8_t>(1u << (k & 7));
        }
        f.seq++;
        run(1, 20);
    }
    /// Strike key `k` alone.
    void strike(uint8_t k) { play({k}); }

    /// Set key `k`'s third-octave band in the frame to `level`, full unless asked, read on the next new one.
    void loudBand(uint8_t k, uint8_t level = 255) {
        mm::AudioFrame& f = audio.frameForTest();
        f.thirdsCount = 31;
        f.thirds[mm::MoonFlockEffect::thirdForTest(k)] = level;
    }

    /// A frame with drum hits of these strengths in the bass, the mids and the treble, ticked once to hear it.
    void drums(uint8_t low, uint8_t mid, uint8_t high) {
        mm::AudioFrame& f = audio.frameForTest();
        std::memset(f.notesKeptHits, 0, sizeof(f.notesKeptHits));
        f.onsetLow = low; f.onsetMid = mid; f.onsetHigh = high;
        f.seq++;
        run(1, 20);
        f.onsetLow = f.onsetMid = f.onsetHigh = 0;
    }

    /// Tick `frames` times `stepMs` apart.
    void run(uint32_t frames, uint32_t stepMs) {
        for (uint32_t i = 0; i < frames; i++) {
            now += stepMs;
            mm::platform::setTestNowMs(now);
            layer.tick();
        }
    }

    /// The brightest channel on the grid.
    uint8_t brightest() {
        auto& buf = layer.buffer();
        uint8_t top = 0;
        for (size_t i = 0; i < buf.bytes(); i++) top = std::max(top, buf.data()[i]);
        return top;
    }
};

/// How many lights of a rig's grid carry a channel at 200 or more: the hot boids and the predator, not the cold flock.
size_t brightOf(KeysRig& r) {
    auto& buf = r.layer.buffer();
    const uint8_t cpl = r.layer.channelsPerLight();
    size_t bright = 0;
    for (size_t i = 0; i + cpl <= buf.bytes(); i += cpl) {
        bool on = false;
        for (uint8_t c = 0; c < cpl; c++) on = on || buf.data()[i + c] >= 200;
        bright += on;
    }
    return bright;
}

/// How many lights of a rig's grid carry any color, whatever its channels.
size_t litOf(KeysRig& r) {
    auto& buf = r.layer.buffer();
    const uint8_t cpl = r.layer.channelsPerLight();
    size_t lit = 0;
    for (size_t i = 0; i + cpl <= buf.bytes(); i += cpl) {
        bool on = false;
        for (uint8_t c = 0; c < cpl; c++) on = on || buf.data()[i + c] != 0;
        lit += on;
    }
    return lit;
}

/// A kick releases the held keyboard: each bar shrinks back to its boid, at once in silence, where every bar is one light, and within 3 s the boids fly.
void checkKickReleases(KeysRig& rig) {
    rig.drums(255, 0, 0);
    CHECK(rig.flock.phaseForTest() != mm::MoonFlockEffect::kHolding);
    rig.run(150, 20);
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kFlying);
}

}  // namespace

// The trail plane (w*h*cpl), the flock's own room, a boid of six floats for each of the 79 keys, and each key's state, all held from the prepare.
TEST_CASE("MoonFlockEffect sizes the trail, a boid a key and each key's state") {
    KeysRig rig(64);
    CHECK(rig.flock.dynamicBytes() == 32 * 32 * 3 + 79 * 6 * sizeof(float) + 79 * sizeof(mm::MoonFlockEffect::KeyState));
}

// Disabled, the effect holds its settings and none of its per-key state, as a module that does not run.
TEST_CASE("MoonFlockEffect holds no per-key state once disabled") {
    KeysRig rig(64);
    rig.run(5, 20);
    rig.flock.setEnabled(false);
    rig.layer.applyState();
    CHECK(rig.flock.dynamicBytes() == 0);
    CHECK(sizeof(mm::MoonFlockEffect) < sizeof(mm::PianoNotesBase) + 79 * sizeof(float));
}

// The flock is one boid a key, each glowing dim until its key is played.
TEST_CASE("MoonFlockEffect flies one dim boid a key before any note") {
    KeysRig rig(64);
    rig.run(30, 20);
    CHECK(rig.brightest() > 0);
    CHECK(rig.brightest() <= 96);
    CHECK(rig.flock.hunterForTest() == mm::boids::kNone);
}

// Each strike heats its key's boid, so it glows brighter than the cold flock.
TEST_CASE("MoonFlockEffect makes a struck key's boid glow") {
    KeysRig rig(128);
    rig.run(10, 20);
    rig.strike(39);
    rig.run(5, 20);
    CHECK(rig.brightest() > 100);
}

// Crossing trigger makes a boid the predator; a newer key crossing it takes the slot, and a cooled predator leaves it empty.
TEST_CASE("MoonFlockEffect keeps one predator, the key most recently heated past trigger") {
    KeysRig rig(255);   // one loud strike heats a key to full, past the default trigger of 192
    rig.run(5, 20);
    rig.strike(10);
    CHECK(rig.flock.hunterForTest() == 10);
    rig.strike(50);
    CHECK(rig.flock.hunterForTest() == 50);
    rig.run(250, 20);   // five seconds: 255 halves past 96, half of trigger, within two `fade`s of 3 s
    CHECK(rig.flock.hunterForTest() == mm::boids::kNone);
}

// A frame's strike counts once however many ticks read it, so one note held over many frames heats its key once.
TEST_CASE("MoonFlockEffect heats a key once per strike, not per tick") {
    KeysRig rig(100);
    rig.strike(20);
    rig.run(3, 1);   // three more ticks of the same frame
    CHECK(rig.flock.hunterForTest() == mm::boids::kNone);   // 100 heat stays under the trigger of 192
    rig.strike(20);
    CHECK(rig.flock.hunterForTest() == 20);    // a second strike crosses it
}

// A drum hit lights its own range and no other, and adds no heat: the predator's slot stays empty.
TEST_CASE("MoonFlockEffect flashes the boids of a drum's range on its hit") {
    KeysRig rig(64);
    rig.run(5, 20);
    rig.drums(255, 0, 0);
    CHECK(rig.flock.glowForTest(0) > 150);
    CHECK(rig.flock.glowForTest(1) == 0);
    CHECK(rig.flock.glowForTest(2) == 0);
    CHECK(rig.brightest() > 100);
    CHECK(rig.flock.hunterForTest() == mm::boids::kNone);
    rig.run(250, 20);   // five seconds, over twelve of a full hit's 400 ms half-lives: the flash is gone and every boid is cold
    CHECK(rig.brightest() <= 96);
}

// A full-strength hit halves in 400 ms and a quarter-strength one in 175 ms, so a hit's energy extends its fade. One rig at a time: the rigs' audio services share the one seat the flock reads.
TEST_CASE("MoonFlockEffect lets a harder drum hit glow longer") {
    auto halved = [](uint8_t strength, uint32_t frames, uint32_t stepMs) {
        KeysRig rig(64);
        rig.flock.onsetGlow = 255;
        rig.drums(0, strength, 0);
        const float start = rig.flock.glowForTest(1);
        rig.run(frames, stepMs);
        return rig.flock.glowForTest(1) / start;
    };
    CHECK(halved(255, 20, 20) == doctest::Approx(0.5).epsilon(0.1));   // 400 ms
    CHECK(halved(64, 35, 5) == doctest::Approx(0.5).epsilon(0.1));     // 175 ms
}

// A flash swells its boids as well as lighting them, so the same flight covers more lights in the step after a hit. One rig at a time, as above.
TEST_CASE("MoonFlockEffect swells the boids a drum flashes") {
    auto after = [](uint8_t strength) {
        KeysRig rig(64);
        rig.run(5, 20);
        rig.drums(strength, strength, strength);
        return litOf(rig);
    };
    CHECK(after(255) > after(0));
}

// The predator pulses with the bass: under the same loud bass, and so the same open mouth, a predator whose flash counts is drawn bigger than one whose does not, so more lights carry its full color. One rig at a time, as above.
TEST_CASE("MoonFlockEffect swells the predator with the bass") {
    auto bright = [](uint8_t onsetGlow) {
        KeysRig rig(255, 64, 64);   // at size 0 the predator sits on its 2-light floor and swells above it
        rig.flock.onsetGlow = onsetGlow;
        rig.strike(30);
        REQUIRE(rig.flock.hunterForTest() == 30);
        rig.audio.frameForTest().bass = 255;
        rig.run(1, 20);
        return brightOf(rig);
    };
    CHECK(bright(255) > bright(0));
}

// The predator's shape: a disc with its mouth cut out along the heading, so a light straight ahead is open and one behind is covered.
TEST_CASE("MoonFlockEffect's predator is a disc with a mouth facing its heading") {
    const float r = 3.0f, half = 0.6f;   // heading along +x
    CHECK(mm::boids::mouthCovers(2, 0, 1, 0, r, half).body == 0.0f);    // ahead: inside the mouth
    CHECK(mm::boids::mouthCovers(-2, 0, 1, 0, r, half).body == 1.0f);   // behind: the disc
    CHECK(mm::boids::mouthCovers(0, 2, 1, 0, r, half).body == 1.0f);    // beside: the disc
    CHECK(mm::boids::mouthCovers(0, 5, 1, 0, r, half).body == 0.0f);    // past the rim
    // A wider mouth takes in a light a narrow one covers.
    CHECK(mm::boids::mouthCovers(2, 1.5f, 1, 0, r, 0.2f).body > mm::boids::mouthCovers(2, 1.5f, 1, 0, r, 0.8f).body);
}

/// Bring a rig to a held keyboard: a short flight, then forming until the hold, which draws its wait from `holdSeconds`.
void formKeyboard(KeysRig& rig, uint16_t holdSeconds) {
    rig.flock.formEvery = 1;   // a flight of 0.75 to 1.25 s
    rig.run(70, 20);           // 1.4 s: past the longest flight, so the keyboard is forming
    rig.flock.formEvery = holdSeconds;
    for (int i = 0; i < 1100 && rig.flock.phaseForTest() != mm::MoonFlockEffect::kHolding; i++) rig.run(1, 20);   // within 22 s: every boid lands within 20
}

// On its timer the flock forms the keyboard: every boid reaches its key's place, and the keyboard holds.
TEST_CASE("MoonFlockEffect forms the keyboard on its timer and holds it") {
    KeysRig rig(64);
    rig.flock.formEvery = 1;
    rig.run(35, 20);           // 0.7 s: inside the shortest flight
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kFlying);
    rig.run(35, 20);           // 1.4 s: past the longest
    CHECK(rig.flock.phaseForTest() != mm::MoonFlockEffect::kFlying);
    rig.flock.formEvery = 60;
    rig.run(1100, 20);         // 22 s: every boid has landed, and the hold lasts 45 s or more
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
}

// A bass hit breaks the held keyboard and the boids fly again; a snare and hats do not.
TEST_CASE("MoonFlockEffect breaks the keyboard on a bass hit") {
    KeysRig rig(64);
    rig.flock.hold = 0;
    formKeyboard(rig, 60);
    REQUIRE(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
    rig.drums(0, 255, 255);
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
    checkKickReleases(rig);
}

// With no bass hit the keyboard still breaks after a wait of its own, so quiet music keeps it moving.
TEST_CASE("MoonFlockEffect breaks a quiet keyboard after a wait") {
    KeysRig rig(64);
    rig.flock.hold = 0;
    formKeyboard(rig, 10);     // a hold of 7.5 to 12.5 s
    REQUIRE(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
    rig.run(750, 20);          // 15 s: past the longest wait and the release after it
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kFlying);
}

// A formEvery of 0 keeps the flock flying.
TEST_CASE("MoonFlockEffect with formEvery 0 keeps flying") {
    KeysRig rig(64);
    rig.flock.formEvery = 0;
    rig.run(500, 20);
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kFlying);
}

// A held keyboard is a spectrum: the bar of a key whose band is loud rises toward the top, a silent one's stays at the bottom.
TEST_CASE("MoonFlockEffect holds each key as a bar as tall as the key is loud") {
    KeysRig rig(64);
    formKeyboard(rig, 60);
    REQUIRE(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
    rig.loudBand(40);   // key 40's band, 250 Hz; its column 40 * 32 / 79 = 16
    rig.audio.frameForTest().seq++;
    rig.run(5, 20);
    const uint8_t* px = rig.layer.buffer().data();
    auto lit = [&](int x, int y) { return (px[(y * 32 + x) * 3] | px[(y * 32 + x) * 3 + 1] | px[(y * 32 + x) * 3 + 2]) != 0; };
    CHECK(lit(16, 1));         // the loud key's bar reaches the top rows
    CHECK_FALSE(lit(2, 1));    // a silent key's does not
}

// formEvery applies live: shortening it shortens the flight already under way.
TEST_CASE("MoonFlockEffect applies a new formEvery to the wait under way") {
    KeysRig rig(64);
    rig.flock.formEvery = 330;
    rig.run(100, 20);          // 2 s into a flight of four minutes or more
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kFlying);
    rig.flock.formEvery = 1;   // now a wait of 0.75 to 1.25 s, already passed
    rig.run(1, 20);
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kForming);
}

// The mouth is painted dark, not left as it was, so what was drawn there does not show through.
TEST_CASE("MoonFlockEffect's predator paints its mouth dark over what was drawn there") {
    std::vector<uint8_t> plane(32 * 32 * 3, 200);
    const mm::boids::Plane trail{mm::draw::Canvas{plane.data(), plane.size(), mm::Coord3D{32, 32, 1}, 3}};   // a flock unit to a light
    trail.mouth(16.5f, 16.5f, 1, 0, mm::RGB{0, 0, 255}, 4, 0.6f);   // centered on light 16, heading along +x
    auto at = [&](int x, int y) { return &plane[(y * 32 + x) * 3]; };
    CHECK(at(19, 16)[0] == 0);    // ahead, inside the mouth: dark
    CHECK(at(19, 16)[2] == 0);
    CHECK(at(13, 16)[2] == 255);  // behind: the body
    CHECK(at(13, 16)[0] == 0);
    CHECK(at(0, 0)[0] == 200);    // outside the disc: untouched
}

// Only the notes the `notes` choice keeps heat a key: a key heard but not kept heats under all and not under no overtones.
TEST_CASE("MoonFlockEffect heats only the notes its notes choice keeps") {
    auto hunterAfterRawStrike = [](uint8_t notesShown) {
        KeysRig rig(255);
        rig.flock.notes = notesShown;
        mm::AudioFrame& f = rig.audio.frameForTest();
        f.notesCount = mm::kAudioNoteKeys;
        f.notes[30] = 255;
        f.notesHits[30 >> 3] |= static_cast<uint8_t>(1u << (30 & 7));   // heard and struck, not kept
        f.seq++;
        rig.run(1, 20);
        return rig.flock.hunterForTest();
    };
    CHECK(hunterAfterRawStrike(mm::AudioNotesBase::kNotesAll) == 30);
    CHECK(hunterAfterRawStrike(mm::AudioNotesBase::kNotesNoOvertones) == 255);
}

// The boids fly to their places: the keyboard completes well inside the 20 s after which stragglers are put there, and not on the first step.
TEST_CASE("MoonFlockEffect's boids fly to their keys rather than being put there") {
    KeysRig rig(64);
    rig.flock.formEvery = 1;
    rig.run(70, 20);           // 1.4 s: forming
    REQUIRE(rig.flock.phaseForTest() == mm::MoonFlockEffect::kForming);
    CHECK(rig.flock.placedForTest() < 79);
    rig.flock.formEvery = 60;
    uint32_t ms = 0;
    while (rig.flock.phaseForTest() != mm::MoonFlockEffect::kHolding && ms < 22000) { rig.run(1, 20); ms += 20; }
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
    CHECK(ms < 10000);
}

// The drums split the keys by the band each sounds in, as the audio core's rangeOfThird does: C#3 is the kick's last key, D3 the snare's first, C#7 the snare's last and D7 the hats' first.
TEST_CASE("MoonFlockEffect gives each key to the drum whose band it sounds in") {
    CHECK(mm::MoonFlockEffect::rangeOfForTest(0) == 0);
    CHECK(mm::MoonFlockEffect::rangeOfForTest(28) == 0);
    CHECK(mm::MoonFlockEffect::rangeOfForTest(29) == 1);
    CHECK(mm::MoonFlockEffect::rangeOfForTest(76) == 1);
    CHECK(mm::MoonFlockEffect::rangeOfForTest(77) == 2);
    CHECK(mm::MoonFlockEffect::rangeOfForTest(78) == 2);
}

// Any grid and any channel count, strikes, drums and the keyboard included, draw safely; a strip draws its flock along the row.
TEST_CASE("MoonFlockEffect survives any grid and any channel count") {
    auto play = [](mm::lengthType w, mm::lengthType h, uint8_t cpl) {
        KeysRig rig(255, w, h, cpl);
        rig.flock.formEvery = 1;
        rig.flock.speed = 255;
        rig.strike(40);
        rig.drums(255, 255, 255);
        rig.audio.frameForTest().bass = 255;
        rig.run(200, 20);
        auto& buf = rig.layer.buffer();
        size_t lit = 0;
        for (size_t i = 0; i < buf.bytes(); i++) lit += buf.data()[i] != 0;
        return lit;
    };
    CHECK_NOTHROW(play(1, 1, 3));
    CHECK(play(120, 1, 3) > 0);
    CHECK_NOTHROW(play(32, 32, 1));
    CHECK_NOTHROW(play(32, 32, 4));
    CHECK_NOTHROW(play(256, 192, 3));
}

// The predator keeps its slot while its key cools between trigger and half of it, and leaves it below half.
TEST_CASE("MoonFlockEffect keeps the predator until its key cools below half of trigger") {
    KeysRig rig(255);          // one strike heats a key to full; trigger 192, fade 3 s
    rig.strike(30);
    REQUIRE(rig.flock.hunterForTest() == 30);
    rig.run(115, 20);          // 2.3 s: about 150, under trigger and over half of it
    CHECK(rig.flock.hunterForTest() == 30);
    rig.run(135, 20);          // 5 s: about 80, under half
    CHECK(rig.flock.hunterForTest() == mm::boids::kNone);
}

// Three keys struck at once, each more than a hand's span from the next: two hands cannot play them all, so under hands one stays cold, where under no overtones all three heat.
TEST_CASE("MoonFlockEffect heats only the keys two hands could play under hands") {
    auto heated = [](uint8_t notesShown) {
        KeysRig rig(64);
        rig.flock.notes = notesShown;
        rig.play({5, 40, 75});
        int count = 0;
        for (uint8_t k : {5, 40, 75}) count += rig.flock.heatForTest(k) > 0;
        return count;
    };
    CHECK(heated(mm::AudioNotesBase::kNotesNoOvertones) == 3);
    CHECK(heated(mm::AudioNotesBase::kNotesHands) == 2);
}

// With chord on, a struck key outside the named chord does not heat, and a chord tone does.
TEST_CASE("MoonFlockEffect heats only chord tones with chord on") {
    auto chordOnStrike = [](uint8_t key) {
        KeysRig rig(255);
        rig.flock.notes = mm::AudioNotesBase::kNotesAll;
        rig.flock.chord = true;
        mm::AudioFrame& f = rig.audio.frameForTest();
        f.chordRoot = 0;                       // C
        f.chordQuality = mm::kChordMajor;      // C, E, G
        f.notesCount = mm::kAudioNoteKeys;
        f.notes[key] = 255;
        f.notesHits[key >> 3] |= static_cast<uint8_t>(1u << (key & 7));
        f.seq++;
        rig.run(1, 20);
        return rig.flock.hunterForTest();
    };
    CHECK(chordOnStrike(3) == 3);              // key 3 is C1, a chord tone
    CHECK(chordOnStrike(0) == 255);            // key 0 is A0, outside C major
}

// At speed 0 the boids do not move, so the stragglers are put on their places at 20 s and the keyboard still completes.
TEST_CASE("MoonFlockEffect completes the keyboard at speed 0 when the stragglers land") {
    KeysRig rig(64);
    rig.flock.speed = 0;
    rig.flock.formEvery = 1;
    rig.run(70, 20);
    REQUIRE(rig.flock.phaseForTest() == mm::MoonFlockEffect::kForming);
    rig.flock.formEvery = 60;
    rig.run(1050, 20);         // 21 s
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
    CHECK(rig.flock.placedForTest() == 79);
}

// The finished keyboard ignores a kick for `hold` seconds, then the next kick breaks it.
TEST_CASE("MoonFlockEffect holds the keyboard for hold seconds before a kick breaks it") {
    KeysRig rig(64);
    rig.flock.hold = 5;
    formKeyboard(rig, 60);
    REQUIRE(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
    rig.drums(255, 0, 0);
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
    rig.run(300, 20);          // 6 s
    checkKickReleases(rig);
}

// The snare lights the middle range and the hats the top, each its own.
TEST_CASE("MoonFlockEffect flashes the snare's and the hats' ranges on their hits") {
    KeysRig rig(64);
    rig.drums(0, 255, 0);
    CHECK(rig.flock.glowForTest(1) > 150);
    CHECK(rig.flock.glowForTest(0) == 0);
    rig.drums(0, 0, 255);
    CHECK(rig.flock.glowForTest(2) > 150);
}

// At speed 0 from a prepare the flock still shows, where it starts.
TEST_CASE("MoonFlockEffect at speed 0 shows the flock where it is") {
    KeysRig rig(64);
    rig.flock.speed = 0;
    rig.run(10, 20);
    CHECK(litOf(rig) > 0);
}

// A layer whose lights change under the effect before it is prepared again keeps its picture, rather than the effect writing past its trail.
TEST_CASE("MoonFlockEffect leaves the layer as it was while the layer has changed under it and prepare has not caught up") {
    KeysRig rig(64);
    rig.run(5, 20);
    const std::vector<uint8_t> before(rig.layer.buffer().data(), rig.layer.buffer().data() + 32 * 32 * 3);
    rig.layer.setChannelsPerLight(4);
    rig.now += 20;
    mm::platform::setTestNowMs(rig.now);
    rig.flock.tick();
    CHECK(std::vector<uint8_t>(rig.layer.buffer().data(), rig.layer.buffer().data() + 32 * 32 * 3) == before);
}

// The keyboard holds for `hold` seconds even when the wait for a kick is shorter: the wait counts from the end of the hold.
TEST_CASE("MoonFlockEffect holds the keyboard for hold seconds however short formEvery is") {
    KeysRig rig(64);
    rig.flock.hold = 10;
    formKeyboard(rig, 2);      // a wait of 1.5 to 2.5 s
    REQUIRE(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
    rig.run(400, 20);          // 8 s: inside the hold
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
    rig.run(250, 20);          // 13 s: past the hold and the longest wait after it; the silent bars release at once, and the next flight is under way
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kFlying);
}

// On a strip the held keyboard shows each key as one light, as bright as the key is loud and at least the cold glow, so silence still shows the keys.
TEST_CASE("MoonFlockEffect's keyboard on a strip shows in silence") {
    KeysRig rig(64, 120, 1);
    rig.flock.hold = 60;
    formKeyboard(rig, 60);
    REQUIRE(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
    rig.run(200, 20);          // long after every trail has faded
    CHECK(litOf(rig) >= 79);
}

// On a strip held as a keyboard, with every key cold, a key whose band is loud shows brighter than a silent one of the same pitch class, along a strip lying down and one standing on end.
TEST_CASE("MoonFlockEffect's keyboard on a strip shows a loud key brighter than a silent one") {
    for (const bool standing : {false, true}) {
        CAPTURE(standing);
        KeysRig rig(64, standing ? 1 : 79, standing ? 79 : 1);
        rig.flock.hold = 60;
        formKeyboard(rig, 60);
        REQUIRE(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
        rig.run(200, 20);          // long after every trail has faded
        rig.loudBand(40);          // key 40, C sharp 4, loud; key 4, C sharp 1, silent
        rig.audio.frameForTest().seq++;
        rig.run(5, 20);
        const uint8_t* px = rig.layer.buffer().data();
        auto light = [&](uint8_t k) {   // a strip's 79 lights are its 79 keys: A0 at the left lying down, at the bottom standing
            const size_t at = standing ? 78 - k : k;
            return std::max({px[at * 3], px[at * 3 + 1], px[at * 3 + 2]});
        };
        CHECK(light(40) > light(4));
    }
}

// A kick releases the keyboard: the bars shrink back to their boids at the flock's own pace, so at speed 0 the release holds still, and at speed 60 it ends with the boids flying.
TEST_CASE("MoonFlockEffect releases the keyboard at the flock's pace") {
    KeysRig rig(64);
    rig.flock.hold = 0;
    formKeyboard(rig, 60);
    REQUIRE(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
    rig.flock.speed = 0;
    rig.drums(255, 0, 0);
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kReleasing);
    rig.run(250, 20);          // 5 s at speed 0: still shrinking, not a step taken
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kReleasing);
    CHECK(rig.flock.placedForTest() == 79);
    rig.flock.speed = 60;
    rig.run(150, 20);          // 3 s at speed 60
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kFlying);
}

// The keyboard's bars follow the bands as they are now, whatever the speed: at speed 0 a band that goes quiet lets its bar fall as a meter does.
TEST_CASE("MoonFlockEffect's keyboard bars follow the keys live at speed 0") {
    KeysRig rig(64);
    rig.flock.hold = 60;
    formKeyboard(rig, 60);
    REQUIRE(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
    rig.flock.speed = 0;
    rig.loudBand(40);
    rig.audio.frameForTest().seq++;
    rig.run(2, 20);
    const uint8_t* px = rig.layer.buffer().data();
    auto lit = [&](int x, int y) { return (px[(y * 32 + x) * 3] | px[(y * 32 + x) * 3 + 1] | px[(y * 32 + x) * 3 + 2]) != 0; };
    CHECK(lit(16, 1));
    rig.loudBand(40, 0);
    rig.audio.frameForTest().seq++;
    rig.run(50, 20);          // a second: the bar falls as a meter does, 150 ms to halve
    CHECK_FALSE(lit(16, 1));
}

// Heat cools by its half-life whatever the frame rate: five seconds at 4 ms ticks leave the same heat as at 20 ms.
TEST_CASE("MoonFlockEffect cools a key the same at any frame rate") {
    auto heatAfter = [](uint32_t stepMs) {
        KeysRig rig(255);
        rig.flock.fade = 40;
        rig.strike(30);
        rig.run(5000 / stepMs, stepMs);
        return rig.flock.heatForTest(30);
    };
    const float slow = heatAfter(20), fast = heatAfter(4);
    CHECK(slow < 255.0f * 0.95f);              // 40 s halves 5 s by about 8%
    CHECK(fast == doctest::Approx(slow).epsilon(0.01));
}

// A key heats in proportion to how loud it is played.
TEST_CASE("MoonFlockEffect heats a key in proportion to how loud it is struck") {
    auto heatAt = [](uint8_t loud) {
        KeysRig rig(200);
        mm::AudioFrame& f = rig.audio.frameForTest();
        f.notesCount = mm::kAudioNoteKeys;
        f.notes[30] = loud;
        f.notesKept[30 >> 3] |= static_cast<uint8_t>(1u << (30 & 7));
        f.notesKeptHits[30 >> 3] |= static_cast<uint8_t>(1u << (30 & 7));
        f.seq++;
        rig.run(1, 20);
        return rig.flock.heatForTest(30);
    };
    CHECK(heatAt(255) > heatAt(128) * 1.8f);
}

// A tall bar takes its time: from the full height of a loud key it shrinks a boid's step at a time, so the release lasts beyond half a second at speed 60 and ends within two.
TEST_CASE("MoonFlockEffect shrinks a tall bar back to its boid over many steps") {
    KeysRig rig(64);
    rig.flock.hold = 0;
    formKeyboard(rig, 60);
    REQUIRE(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
    rig.loudBand(40);
    rig.drums(255, 0, 0);     // the kick, with key 40's bar at full height
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kReleasing);
    rig.run(25, 20);          // 0.5 s: about 30 steps, 11 of the bar's 32 lights
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kReleasing);
    rig.run(75, 20);          // 2 s in all
    CHECK(rig.flock.phaseForTest() == mm::MoonFlockEffect::kFlying);
}

// Each boid takes its key's pitch class: every A is one hue, and A and A# are two.
TEST_CASE("MoonFlockEffect colors each boid by its key's pitch class") {
    CHECK(mm::MoonFlockEffect::pitchHueForTest(0) == mm::MoonFlockEffect::pitchHueForTest(12));
    CHECK(mm::MoonFlockEffect::pitchHueForTest(0) == mm::MoonFlockEffect::pitchHueForTest(72));
    CHECK(mm::MoonFlockEffect::pitchHueForTest(0) != mm::MoonFlockEffect::pitchHueForTest(1));
}

// The wait before the keyboard falls within a quarter either side of formEvery.
TEST_CASE("MoonFlockEffect forms the keyboard within a quarter of formEvery") {
    KeysRig rig(64);
    rig.flock.formEvery = 4;
    uint32_t ms = 0;
    while (rig.flock.phaseForTest() == mm::MoonFlockEffect::kFlying && ms < 8000) { rig.run(1, 20); ms += 20; }
    CHECK(ms >= 3000);
    CHECK(ms <= 5020);
}

// Each key sounds in one third-octave band, the nearest by its frequency, and four neighboring keys share one.
TEST_CASE("MoonFlockEffect maps each key to the third-octave band it sounds in") {
    CHECK(mm::MoonFlockEffect::thirdForTest(48) == 13);   // A4, 440 Hz: the 400 Hz band
    CHECK(mm::MoonFlockEffect::thirdForTest(40) == 11);   // C#4, 277 Hz: the 250 Hz band
    CHECK(mm::MoonFlockEffect::thirdForTest(0) == 1);     // A0, 27.5 Hz: the 25 Hz band
    CHECK(mm::MoonFlockEffect::thirdForTest(78) == 21);   // D#7, 2.49 kHz: the 2.5 kHz band
    int sharing = 0;
    for (uint8_t k = 0; k < 79; k++) sharing += mm::MoonFlockEffect::thirdForTest(k) == 11;
    CHECK(sharing == 4);
}

// A frame that carries only the sixteen bands, as audio sync sends, still draws the spectrum.
TEST_CASE("MoonFlockEffect draws the spectrum from the sixteen bands when the frame has no thirds") {
    KeysRig rig(64);
    rig.flock.hold = 60;
    formKeyboard(rig, 60);
    REQUIRE(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
    mm::AudioFrame& f = rig.audio.frameForTest();
    f.thirdsCount = 0;
    for (uint8_t& b : f.bands) b = 255;
    f.seq++;
    rig.run(2, 20);
    const uint8_t* px = rig.layer.buffer().data();
    CHECK((px[(1 * 32 + 16) * 3] | px[(1 * 32 + 16) * 3 + 1] | px[(1 * 32 + 16) * 3 + 2]) != 0);
}

// Each dock lines the keys up at its own place: every boid lands, and the spectrum grows from there.
TEST_CASE("MoonFlockEffect forms the keyboard at the dock it is given") {
    for (uint8_t dock : {mm::MoonFlockEffect::kDockTop, mm::MoonFlockEffect::kDockLeft, mm::MoonFlockEffect::kDockRight, mm::MoonFlockEffect::kDockRing}) {
        CAPTURE(int(dock));
        KeysRig rig(64);
        rig.flock.dock = dock;
        rig.flock.hold = 60;
        formKeyboard(rig, 60);
        REQUIRE(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
        CHECK(rig.flock.edgeForTest() == dock);
        CHECK(rig.flock.placedForTest() == 79);
        mm::AudioFrame& f = rig.audio.frameForTest();
        f.thirdsCount = 31;
        for (uint8_t& t : f.thirds) t = 255;   // every band loud: every bar at full reach
        f.seq++;
        rig.run(2, 20);
        const uint8_t* px = rig.layer.buffer().data();
        auto lit = [&](int x, int y) { return (px[(y * 32 + x) * 3] | px[(y * 32 + x) * 3 + 1] | px[(y * 32 + x) * 3 + 2]) != 0; };
        if (dock == mm::MoonFlockEffect::kDockTop) { CHECK(lit(16, 0)); CHECK(lit(16, 30)); }
        if (dock == mm::MoonFlockEffect::kDockLeft) { CHECK(lit(0, 16)); CHECK(lit(30, 16)); }
        if (dock == mm::MoonFlockEffect::kDockRight) { CHECK(lit(31, 16)); CHECK(lit(1, 16)); }
        if (dock == mm::MoonFlockEffect::kDockRing) { CHECK(lit(16, 6)); CHECK_FALSE(lit(16, 16)); }   // the ring's top, and its empty middle
    }
}

// At random the keyboard forms at more than one place over a few formations.
TEST_CASE("MoonFlockEffect forms the keyboard at a random dock each time") {
    KeysRig rig(64);
    rig.flock.dock = mm::MoonFlockEffect::kDockRandom;
    rig.flock.hold = 0;
    bool seen[6] = {};
    for (int cycle = 0; cycle < 6; cycle++) {
        formKeyboard(rig, 60);
        REQUIRE(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
        seen[rig.flock.edgeForTest()] = true;
        rig.drums(255, 0, 0);
        rig.run(150, 20);   // released and flying
    }
    int places = 0;
    for (bool s : seen) places += s;
    CHECK(places >= 2);
}

// With no audio service at all the flock still flies, forms the keyboard and releases it after its wait.
TEST_CASE("MoonFlockEffect runs its keyboard with no audio source") {
    KeysRig rig(64, 32, 32, 3, false);
    rig.flock.formEvery = 1;
    rig.flock.hold = 0;
    bool held = false, flewAgain = false;
    for (int f = 0; f < 2000 && !flewAgain; f++) {
        rig.run(1, 20);
        held = held || rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding;
        flewAgain = held && rig.flock.phaseForTest() == mm::MoonFlockEffect::kFlying;
    }
    CHECK(held);
    CHECK(flewAgain);
}

// The clock wraps after 49 days; the timers and the steps carry on across it.
TEST_CASE("MoonFlockEffect carries on across the clock's wrap") {
    mm::golden::ScopedTestClock clock(0xFFFFF000u);
    KeysRig rig(64);
    rig.now = 0xFFFFF000u;
    rig.flock.formEvery = 1;
    rig.run(300, 20);          // six seconds: zero is crossed after 4.1, and the timers run on past it
    REQUIRE(rig.now < 0x1000u);
    CHECK(rig.flock.phaseForTest() != mm::MoonFlockEffect::kFlying);
    CHECK(rig.brightest() > 0);
}

// With chord on under hands, a hand holding a chord keeps only its chord tones, and a hand playing a single line keeps its note.
TEST_CASE("MoonFlockEffect under hands snaps a chord hand and keeps a line") {
    auto heats = [](std::initializer_list<uint8_t> keys) {
        KeysRig rig(64);
        rig.flock.notes = mm::AudioNotesBase::kNotesHands;
        rig.flock.chord = true;
        mm::AudioFrame& f = rig.audio.frameForTest();
        f.chordRoot = 0;                     // C
        f.chordQuality = mm::kChordMajor;    // C, E, G
        rig.play(keys);
        return rig.flock.heatForTest(41) > 0;   // D4, outside C major
    };
    CHECK_FALSE(heats({39, 41, 43, 46}));   // C4 D4 E4 G4 in one hand: a chord, so D4 goes
    CHECK(heats({41}));                     // D4 alone: a line, so it stays
}

// A grid too small for the ring forms the keyboard on an edge instead.
TEST_CASE("MoonFlockEffect forms on an edge where the ring has no room") {
    KeysRig rig(64, 6, 6);
    rig.flock.dock = mm::MoonFlockEffect::kDockRing;
    rig.flock.hold = 60;
    formKeyboard(rig, 60);
    REQUIRE(rig.flock.phaseForTest() == mm::MoonFlockEffect::kHolding);
    CHECK(rig.flock.edgeForTest() != mm::MoonFlockEffect::kDockRing);
}

// A strip standing on end draws its flock and its keyboard down its one column.
TEST_CASE("MoonFlockEffect draws on a vertical strip") {
    KeysRig rig(255, 1, 60);
    rig.flock.formEvery = 1;
    rig.strike(40);
    rig.run(400, 20);
    CHECK(litOf(rig) > 0);
}
