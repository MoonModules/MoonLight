#pragma once
// Author: MoonLight original (Space Invaders, Taito 1978, is the inspiration)

#include "core/services/AudioService.h"   // latestFrame: the beat the march steps on
#include "core/util/math16.h"
#include "light/powerfunctions/draw.h"
#include "light/effects/EffectBase.h"

namespace mm {

namespace invart {

/// The sprites index these and the effect fills them, so one drawing serves every rank in a color.
enum : uint8_t { kClear = 0, kBody = 1, kDark = 2, kEye = 3 };
inline constexpr uint8_t kPaletteCount = 4;

// The squid, top rank: 8x8, and the arcade's own two frames are what the march looks like.
inline constexpr uint8_t W = 8, H = 8, F = 2;
inline constexpr uint8_t kSquid[] = {
    // frame 0: tentacles out
    0,0,0,1,1,0,0,0,
    0,0,1,1,1,1,0,0,
    0,1,1,1,1,1,1,0,
    1,1,3,1,1,3,1,1,
    1,1,1,1,1,1,1,1,
    0,0,1,0,0,1,0,0,
    0,1,0,1,1,0,1,0,
    1,0,1,0,0,1,0,1,
    // frame 1: tentacles tucked
    0,0,0,1,1,0,0,0,
    0,0,1,1,1,1,0,0,
    0,1,1,1,1,1,1,0,
    1,1,3,1,1,3,1,1,
    1,1,1,1,1,1,1,1,
    0,1,0,1,1,0,1,0,
    1,0,0,0,0,0,0,1,
    0,1,0,0,0,0,1,0,
};
static_assert(sizeof(kSquid) == static_cast<size_t>(W) * H * F, "squid: 2 frames of 8x8");

// The crab (middle ranks): 11x8, the widest of the three, arms up and arms down.
inline constexpr uint8_t CW = 11, CH = 8, CF = 2;
inline constexpr uint8_t kCrab[] = {
    // frame 0: arms up
    0,0,1,0,0,0,0,0,1,0,0,
    0,0,0,1,0,0,0,1,0,0,0,
    0,0,1,1,1,1,1,1,1,0,0,
    0,1,1,3,1,1,1,3,1,1,0,
    1,1,1,1,1,1,1,1,1,1,1,
    1,0,1,1,1,1,1,1,1,0,1,
    1,0,1,0,0,0,0,0,1,0,1,
    0,0,0,1,1,0,1,1,0,0,0,
    // frame 1: arms down
    0,0,1,0,0,0,0,0,1,0,0,
    1,0,0,1,0,0,0,1,0,0,1,
    1,0,1,1,1,1,1,1,1,0,1,
    1,1,1,3,1,1,1,3,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,
    0,1,1,1,1,1,1,1,1,1,0,
    0,0,1,0,0,0,0,0,1,0,0,
    0,1,0,0,0,0,0,0,0,1,0,
};
static_assert(sizeof(kCrab) == static_cast<size_t>(CW) * CH * CF, "crab: 2 frames of 11x8");

// The octopus (bottom ranks): 12x8, the squat one worth the fewest points and seen the longest.
inline constexpr uint8_t OW = 12, OH = 8, OF = 2;
inline constexpr uint8_t kOcto[] = {
    // frame 0: legs apart
    0,0,0,0,1,1,1,1,0,0,0,0,
    0,1,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,3,3,1,1,3,3,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,
    0,0,0,1,1,0,0,1,1,0,0,0,
    0,0,1,1,0,1,1,0,1,1,0,0,
    1,1,0,0,0,0,0,0,0,0,1,1,
    // frame 1: legs together
    0,0,0,0,1,1,1,1,0,0,0,0,
    0,1,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,3,3,1,1,3,3,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,
    0,0,1,1,1,0,0,1,1,1,0,0,
    0,1,1,0,0,1,1,0,0,1,1,0,
    0,0,1,1,0,0,0,0,1,1,0,0,
};
static_assert(sizeof(kOcto) == static_cast<size_t>(OW) * OH * OF, "octopus: 2 frames of 12x8");

// The cannon: 13x8, one frame. It does not animate; it moves.
inline constexpr uint8_t GW = 13, GH = 8, GF = 1;
inline constexpr uint8_t kCannon[] = {
    0,0,0,0,0,0,1,0,0,0,0,0,0,
    0,0,0,0,0,1,1,1,0,0,0,0,0,
    0,0,0,0,0,1,1,1,0,0,0,0,0,
    0,1,1,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,
};
static_assert(sizeof(kCannon) == static_cast<size_t>(GW) * GH * GF, "cannon: one 13x8 frame");

}  // namespace invart

/// Effect: Space Invaders in attract mode, marching and firing on its own.
/// @card SpaceInvadersEffect.gif
///
/// The arcade's attract mode is the model: a game playing itself is what reads across a room.
/// The cannon tracks and fires on its own, the ranks march and drop, and a hit thins the formation.
///
/// @moreinfo
///
/// ## The march is the signature
///
/// In the arcade the formation speeds up as the ranks thin.
/// The machine stepped every invader once per frame, so fewer invaders meant a shorter loop.
/// That accident of 1978 hardware became the game's defining tension.
/// Our loop has no such limit, so `stepInterval` reproduces the relationship deliberately.
class SpaceInvadersEffect : public EffectBase {
public:
    /// Catalog tags: the audio glyph applies when `audioReactive` is set.
    const char* tags() const override { return "💫🎵👾"; }
    /// A formation needs a floor and a height, so it is a 2D effect.
    Dim dimensions() const override { return Dim::D2; }

    /// Steps per minute with a full formation. Thinning ranks raise it from here.
    uint8_t marchBpm = 60;
    /// Pixels the formation slides per step.
    uint8_t stepX = 2;
    /// Rows the formation drops when it reaches an edge.
    uint8_t dropY = 3;
    /// Pixels per art pixel. A 64-wide panel wants 1, and a wall can afford 2.
    uint8_t size = 1;

    /// March on the beat and fire on a transient. Silence holds the invasion still.
    bool audioReactive = false;

    /// Publish the tempo, the step and drop distances, the sprite scale and the audio switch.
    void defineControls() override {
        controls_.addControl("marchBpm", marchBpm, 10, 240);
        controls_.addControl("stepX", stepX, 1, 8);
        controls_.addControl("dropY", dropY, 1, 12);
        controls_.addControl("size", size, 1, 4);
        controls_.addControl("audioReactive", audioReactive);
    }

    /// Reset the march clock and deal a fresh formation.
    void prepare() override {
        formation_ = BeatPhase{};
        reset();
    }

    /// Advance the march, then draw the formation, the shots and the cannon.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        if (width() == 0 || height() == 0) return;
        draw::fill(cv, RGB{0, 0, 0});

        const AudioFrame* audio = audioReactive ? AudioService::latestFrame() : nullptr;
        const bool live = audio && audio->levelSmoothed >= kSilence;
        // `level` against its own smoothed average: the same beat test the moving-head effect uses.
        const bool beat = live && audio->level > audio->levelSmoothed + kBeatMargin;

        advance(beat, audio != nullptr, live);
        drawFormation(cv);
        drawShots(cv);
        drawCannon(cv);
    }

private:
    // The arcade's own layout: one rank of squids, two of crabs, two of octopuses.
    static constexpr uint8_t kCols = 8;
    static constexpr uint8_t kRows = 5;
    static constexpr uint8_t kShots = 6;
    static constexpr uint16_t kSilence = 8;
    static constexpr uint16_t kBeatMargin = 8;

    /// One rank's species. Rank 0 is the squid at the top, the way the machine dealt them.
    enum : uint8_t { kSquid = 0, kCrab, kOcto };
    static uint8_t speciesFor(uint8_t row) { return row == 0 ? kSquid : (row < 3 ? kCrab : kOcto); }

    void reset() {
        alive_ = 0;
        for (uint8_t r = 0; r < kRows; r++)
            for (uint8_t c = 0; c < kCols; c++) { grid_[r][c] = 1; alive_++; }
        ox_ = 0;
        oy_ = 0;
        dir_ = 1;
        frame_ = 0;
        // The latch zeroes with the clock it tracks, or the first step after a reset fires early.
        lastPhase_ = 0;
        cannonX_ = 0;
        for (uint8_t i = 0; i < kShots; i++) shotTtl_[i] = 0;
    }

    /// Step the whole formation one march tick, and run everything that happens between steps.
    void advance(bool beat, bool audioOn, bool live) {
        // Shots fly every frame and only the march is quantized, which is the stop-motion look.
        moveShots();

        if (audioOn) {
            if (!live) return;                       // silence holds the invasion still
            if (!beat) return;                       // the beat is the clock
        } else {
            formation_.advanceTo(elapsed(), stepInterval());
            const uint32_t phase = formation_.phase(2);
            if (phase == lastPhase_) return;
            lastPhase_ = phase;
        }
        stepFormation();
    }

    /// Steps per minute, rising as the ranks thin, which is the game's defining mechanic.
    uint8_t stepInterval() const {
        const uint16_t total = static_cast<uint16_t>(kRows) * kCols;
        const uint16_t left = alive_ == 0 ? 1 : alive_;
        // Four times the base tempo at one invader, roughly the arcade's own ratio.
        const uint32_t bpm = static_cast<uint32_t>(marchBpm) * (total + 3 * (total - left)) / total;
        return static_cast<uint8_t>(bpm > 240 ? 240 : bpm);
    }

    void stepFormation() {
        frame_ ^= 1;                                 // the two-frame wiggle IS the step
        const int16_t span = formationWidth();
        const int16_t nx = static_cast<int16_t>(ox_ + dir_ * static_cast<int16_t>(stepX));
        // Turn and drop at the wall, which inverts on a panel narrower than the formation.
        const int16_t rightWall = span > static_cast<int16_t>(width())
                                      ? static_cast<int16_t>(width())   // scroll the block past
                                      : static_cast<int16_t>(width()) - span;
        const int16_t leftWall = span > static_cast<int16_t>(width())
                                     ? static_cast<int16_t>(width() - span)   // negative: keep going
                                     : 0;
        if (nx < leftWall || nx > rightWall) {
            dir_ = static_cast<int8_t>(-dir_);
            oy_ = static_cast<int16_t>(oy_ + dropY);
            // Landed: the invasion succeeds and the board resets, which is the attract loop.
            if (oy_ + formationHeight() >= static_cast<int16_t>(height())) reset();
        } else {
            ox_ = nx;
        }
        fireInvaderShot();
        aimCannon();
    }

    // Cell size comes from the widest sprite, so the ranks line up however their art differs.
    uint8_t cell() const { return static_cast<uint8_t>((invart::OW + 2) * scale()); }
    uint8_t rowPitch() const { return static_cast<uint8_t>((invart::OH + 2) * scale()); }
    uint8_t scale() const { return size == 0 ? 1 : size; }
    int16_t formationWidth() const { return static_cast<int16_t>(cell() * kCols); }
    int16_t formationHeight() const { return static_cast<int16_t>(rowPitch() * kRows); }

    /// Draw every live invader, one palette entry per rank.
    void drawFormation(const draw::Canvas& cv) {
        RGB pal[invart::kPaletteCount];
        for (uint8_t r = 0; r < kRows; r++) {
            // One entry per rank, so they read as different creatures from one drawing.
            paletteFor(pal, static_cast<uint8_t>(r * 40 + 30));
            for (uint8_t c = 0; c < kCols; c++) {
                if (!grid_[r][c]) continue;
                const lengthType px = static_cast<lengthType>(ox_ + c * cell());
                const lengthType py = static_cast<lengthType>(oy_ + r * rowPitch());
                drawInvader(cv, speciesFor(r), pal, px, py);
            }
        }
    }

    void drawInvader(const draw::Canvas& cv, uint8_t species,
                     const RGB (&pal)[invart::kPaletteCount], lengthType px, lengthType py) {
        const uint8_t sc = scale();
        if (species == kSquid) {
            const draw::sprites::Sprite s{invart::kSquid, pal, invart::W, invart::H,
                                          invart::F, invart::kPaletteCount};
            draw::sprite(cv, s, frame_, px, py, sc);
        } else if (species == kCrab) {
            const draw::sprites::Sprite s{invart::kCrab, pal, invart::CW, invart::CH,
                                          invart::CF, invart::kPaletteCount};
            draw::sprite(cv, s, frame_, px, py, sc);
        } else {
            const draw::sprites::Sprite s{invart::kOcto, pal, invart::OW, invart::OH,
                                          invart::OF, invart::kPaletteCount};
            draw::sprite(cv, s, frame_, px, py, sc);
        }
    }

    /// Draw the cannon, standing on the floor in its own green.
    void drawCannon(const draw::Canvas& cv) {
        RGB pal[invart::kPaletteCount];
        // Green in every version of this game, and a cannon in the formation's color reads as an invader.
        pal[invart::kClear] = RGB{0, 0, 0};
        pal[invart::kBody]  = RGB{40, 230, 60};
        pal[invart::kDark]  = RGB{20, 120, 30};
        pal[invart::kEye]   = RGB{200, 255, 200};
        const uint8_t sc = scale();
        // Clamped: a panel shorter than the scaled cannon would put it off the top edge.
        const int32_t top = static_cast<int32_t>(height()) - invart::GH * sc;
        const lengthType py = static_cast<lengthType>(top < 0 ? 0 : top);
        const draw::sprites::Sprite s{invart::kCannon, pal, invart::GW, invart::GH,
                                      invart::GF, invart::kPaletteCount};
        draw::sprite(cv, s, 0, static_cast<lengthType>(cannonX_), py, sc);
    }

    // A shot is three numbers rather than a particle: it flies straight, needing no forces or drag.

    /// Fly every live shot one step, and test the rising ones against the formation.
    void moveShots() {
        for (uint8_t i = 0; i < kShots; i++) {
            if (shotTtl_[i] == 0) continue;
            shotY_[i] = static_cast<int16_t>(shotY_[i] + shotDir_[i] * kShotSpeed);
            if (shotY_[i] < 0 || shotY_[i] >= static_cast<int16_t>(height())) { shotTtl_[i] = 0; continue; }
            if (shotTtl_[i] > 0) shotTtl_[i]--;
            if (shotDir_[i] < 0) hitTest(i);          // only the cannon's shot can hit an invader
        }
    }

    /// A rising shot takes the invader whose cell it lands in, the formation being a grid.
    void hitTest(uint8_t i) {
        const int16_t rx = static_cast<int16_t>(shotX_[i] - ox_);
        const int16_t ry = static_cast<int16_t>(shotY_[i] - oy_);
        if (rx < 0 || ry < 0) return;
        const int16_t c = static_cast<int16_t>(rx / cell());
        const int16_t r = static_cast<int16_t>(ry / rowPitch());
        if (c >= kCols || r >= kRows) return;
        if (!grid_[r][c]) return;
        grid_[r][c] = 0;
        if (alive_ > 0) alive_--;
        shotTtl_[i] = 0;
        // Cleared: the attract loop starts over rather than leaving an empty sky.
        if (alive_ == 0) reset();
    }

    /// Draw every live shot, the cannon's pale and the invaders' amber.
    void drawShots(const draw::Canvas& cv) {
        for (uint8_t i = 0; i < kShots; i++) {
            if (shotTtl_[i] == 0) continue;
            const RGB col = shotDir_[i] < 0 ? RGB{200, 255, 200} : RGB{255, 200, 80};
            for (uint8_t k = 0; k < 3; k++)
                draw::pixel(cv, {static_cast<lengthType>(shotX_[i]),
                                 static_cast<lengthType>(shotY_[i] + k), 0}, col);
        }
    }

    /// Take the first free shot slot, or do nothing when all are in flight.
    void spawnShot(int16_t x, int16_t y, int8_t dir) {
        for (uint8_t i = 0; i < kShots; i++) {
            if (shotTtl_[i] != 0) continue;
            shotX_[i] = x; shotY_[i] = y; shotDir_[i] = dir; shotTtl_[i] = 255;
            return;
        }
    }

    /// The lowest invader in a random column fires, so no shot passes through its own ranks.
    void fireInvaderShot() {
        const uint8_t c = static_cast<uint8_t>(hashInt(tickSeed_++, 3) % kCols);
        for (int8_t r = kRows - 1; r >= 0; r--) {
            if (!grid_[r][c]) continue;
            spawnShot(static_cast<int16_t>(ox_ + c * cell() + cell() / 2),
                      static_cast<int16_t>(oy_ + r * rowPitch() + rowPitch()), +1);
            return;
        }
    }

    /// The cannon slides toward the nearest live column and fires when lined up.
    void aimCannon() {
        int16_t target = cannonX_;
        for (uint8_t r = 0; r < kRows; r++) {
            bool found = false;
            for (uint8_t c = 0; c < kCols; c++) {
                if (!grid_[r][c]) continue;
                target = static_cast<int16_t>(ox_ + c * cell());
                found = true;
                break;
            }
            if (found) break;
        }
        const int16_t step = static_cast<int16_t>(cell() / 2 + 1);
        if (cannonX_ < target) cannonX_ = static_cast<int16_t>(cannonX_ + step);
        else if (cannonX_ > target) cannonX_ = static_cast<int16_t>(cannonX_ - step);
        const int16_t maxX = static_cast<int16_t>(width() - invart::GW * scale());
        if (cannonX_ < 0) cannonX_ = 0;
        if (cannonX_ > maxX) cannonX_ = maxX > 0 ? maxX : 0;

        // One shot in four: hitting on every step clears the board in seconds, and the watching is the point.
        if (target - cannonX_ < step && cannonX_ - target < step
            && (hashInt(tickSeed_++, 9) & 3) == 0)
            spawnShot(static_cast<int16_t>(cannonX_ + (invart::GW * scale()) / 2),
                      static_cast<int16_t>(height() - invart::GH * scale()), -1);
    }

    /// Fill a rank's four palette slots from one palette entry.
    void paletteFor(RGB (&pal)[invart::kPaletteCount], uint8_t entry) const {
        const RGB body = colorFromPalette(*Palettes::active(), entry);
        pal[invart::kClear] = RGB{0, 0, 0};
        pal[invart::kBody]  = body;
        pal[invart::kDark]  = blend(body, RGB{0, 0, 0}, 140);
        pal[invart::kEye]   = RGB{10, 10, 14};
    }

    static constexpr int16_t kShotSpeed = 2;

    uint8_t  grid_[kRows][kCols] = {};
    uint16_t alive_ = 0;
    int16_t  ox_ = 0, oy_ = 0;
    int8_t   dir_ = 1;
    uint8_t  frame_ = 0;
    uint32_t lastPhase_ = 0;   // the full counter: a narrower copy stops matching past 255 beats
    int16_t  cannonX_ = 0;
    int16_t  shotX_[kShots] = {}, shotY_[kShots] = {};
    int8_t   shotDir_[kShots] = {};
    uint8_t  shotTtl_[kShots] = {};
    uint32_t tickSeed_ = 0;
    BeatPhase formation_;
};

}  // namespace mm
