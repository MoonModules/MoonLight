#pragma once
// Author: MoonLight original

#include "core/services/AudioService.h"   // latestFrame: the spectrum the audio-reactive mode reads
#include "core/util/math16.h"
#include "light/powerfunctions/draw.h"
#include "light/effects/EffectBase.h"
#include "light/effects/SpriteCast.h"           // the shared cast: every sprite, one draw call
#include "light/powerfunctions/particles.h"

namespace mm {

/// Effect: a fountain of sprites, thrown up from the floor and falling back under gravity.
/// @card SpriteFountainEffect.gif
///
/// A fountain is a particle system, and the sprite effects already own a cast.
/// This effect draws that cast through SpriteCast.h rather than copying the pixel art.
/// A fix to a fish fixes it here and in FishTankEffect alike.
///
/// @moreinfo
///
/// ## The `hue` byte carries the sprite kind
///
/// The pool holds one byte per particle, a palette index in every other particle effect.
/// Here it picks the sprite, chosen once at launch so a particle keeps its species for its flight.
/// A sprite id of its own would cost every particle system memory for a field only this effect reads.
/// The color index comes from the particle's slot instead, stable and spread across the palette.
class SpriteFountainEffect : public EffectBase {
public:
    /// Catalog tags: the audio glyph applies when `audioReactive` is set.
    const char* tags() const override { return "💫🎶✨👾"; }
    /// A fountain needs a floor and a height, so it is a 2D effect.
    Dim dimensions() const override { return Dim::D2; }

    /// How hard the nozzle throws. Scales with the grid, so one setting suits a panel and a wall.
    uint8_t lift = 45;
    /// How hard gravity pulls back. Gentle enough that a 12x8 sprite stays legible for its whole arc.
    uint8_t pull = 3;
    /// Sprites launched per beat of the emit clock, so the frame rate leaves the rate alone.
    uint8_t rate = 2;
    /// Launches per minute: the nozzle's own tempo, and the plume's density with it.
    uint8_t emitBpm = 120;
    /// Pixels per art pixel. 1 on a small panel, and a wall can afford 2.
    uint8_t size = 1;

    /// Let the music decide what comes out: a loud band launches a sprite, and picks which one.
    bool audioReactive = false;

    /// Publish the nozzle, the emit clock, the sprite scale and the audio switch.
    void defineControls() override {
        controls_.addControl("lift", lift, 20, 200);
        controls_.addControl("pull", pull, 2, 60);   // the tuned 3 needs a minimum under it
        controls_.addControl("rate", rate, 1, 6);
        controls_.addControl("emitBpm", emitBpm, 10, 240);
        controls_.addControl("size", size, 1, 4);
        controls_.addControl("audioReactive", audioReactive);
    }

    /// Size the pool's storage, wire the view over it, and reset both beat clocks.
    void prepare() override {
        // particles::Pool is a view, so the effect owns one ScratchBuffer per field.
        px_.resize(kPoolSize); py_.resize(kPoolSize);
        vx_.resize(kPoolSize); vy_.resize(kPoolSize);
        ttl_.resize(kPoolSize); hue_.resize(kPoolSize);
        pool_ = particles::Pool{};
        if (px_ && py_ && vx_ && vy_ && ttl_ && hue_) {
            pool_.x = px_.data(); pool_.y = py_.data();
            pool_.vx = vx_.data(); pool_.vy = vy_.data();
            pool_.ttl = ttl_.data(); pool_.hue = hue_.data();
            pool_.count = kPoolSize;
            pool_.clear();
        }
        launch_ = BeatPhase{};
        emit_ = BeatPhase{};
        lastEmit_ = 0;
        seed_ = 0;
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        if (width() == 0 || height() == 0) return;
        if (!pool_.valid()) return;

        draw::fill(cv, RGB{0, 0, 0});

        // 47152 is a shade under vertical in angle16, the nozzle the MoonLive fountain script uses.
        launch_.advanceTo(elapsed(), 9);
        const angle16 aim = static_cast<angle16>(
            47152 + (sin16(static_cast<uint16_t>(launch_.phase(65536))) / 8));
        const draw::pos_t speed = static_cast<draw::pos_t>(lift) * height() / 4;
        const draw::pos_t ox = static_cast<draw::pos_t>(width() / 2) * draw::kSubOne;
        const draw::pos_t oy = static_cast<draw::pos_t>(height() - 1) * draw::kSubOne;
        // Once per frame: the spectrum is the same for every sprite this frame.
        const AudioFrame* audio = audioReactive ? AudioService::latestFrame() : nullptr;
        // The emit clock is separate from the nozzle's sweep, so `emitBpm` holds on any frame rate.
        emit_.advanceTo(elapsed(), emitBpm);
        const uint32_t tick = emit_.phase(2);
        const bool due = tick != lastEmit_;
        lastEmit_ = tick;
        if (audio) { if (due) emitByBand(*audio, ox, oy, aim, speed); }
        else if (due) emitSteady(ox, oy, aim, speed);

        // Physics by elapsed time: the pool's verbs take a FrameTime scale, 256 being one 60 fps frame.
        const uint32_t slice = time_.advance(elapsed());
        if (slice > 0) {
            pool_.gravity(static_cast<draw::pos_t>(pull) * height() / 16, slice);
            pool_.drag(2, slice);
            pool_.step(slice);
        }
        // Free what left the panel, with a margin that spares a big sprite still half on screen.
        pool_.killOutside(static_cast<draw::pos_t>(width()) * draw::kSubOne,
                          static_cast<draw::pos_t>(height()) * draw::kSubOne,
                          static_cast<draw::pos_t>(24) * draw::kSubOne);
        pool_.age(1, slice);   // ttl counts reference frames, so it ages by time like the physics

        drawSprites(cv);
    }

private:
    /// The steady fountain: `rate` sprites a frame, species picked at random.
    void emitSteady(draw::pos_t ox, draw::pos_t oy, angle16 aim, draw::pos_t speed) {
        for (uint8_t k = 0; k < rate; k++) {
            // `hue` carries the sprite kind, picked once so a particle keeps its species in flight.
            const uint8_t kind = static_cast<uint8_t>(hashInt(seed_, k, 7) % spritecast::kKindCount);
            // One at a time: angleEmit's own loop would give a frame's sprites one species.
            pool_.angleEmit(ox, oy, aim, speed, /*cone=*/3000, /*n=*/1, /*life=*/160, kind, seed_);
            seed_++;
        }
    }

    /// The reactive fountain: one sprite per band, thrown when that band is loud.
    void emitByBand(const AudioFrame& audio, draw::pos_t ox, draw::pos_t oy,
                    angle16 aim, draw::pos_t speed) {
        if (audio.levelSmoothed < kSilence) return;      // a quiet room is a still fountain
        for (uint8_t b = 0; b < kBandCount; b++) {
            const uint8_t mag = audio.bands[b];
            if (mag < kBandFloor) continue;              // that band is not playing
            // Rate throttles the reactive mode too: the odds of a loud band firing.
            if ((hashInt(seed_, b, 5) & 0x0F) >= rate * 3) continue;
            const uint8_t kind = static_cast<uint8_t>((b * spritecast::kKindCount) / kBandCount);
            // Loud throws high: 50% of the nozzle's speed at the floor, full speed at the top.
            const draw::pos_t v = static_cast<draw::pos_t>(speed / 2 + (speed * mag) / 512);
            pool_.angleEmit(ox, oy, aim, v, /*cone=*/3000, /*n=*/1, /*life=*/160, kind, seed_);
            seed_++;
        }
    }

    static constexpr uint16_t kSilence   = 8;    ///< below this the room is quiet, not playing
    static constexpr uint8_t  kBandFloor = 40;   ///< a band under this is not throwing anything
    static constexpr uint8_t  kBandCount = 16;   ///< AudioFrame's spectrum width

    /// Draw each live particle as its sprite, centered on the particle's position.
    void drawSprites(const draw::Canvas& cv) {
        const uint8_t sc = size == 0 ? 1 : size;
        const uint8_t beat = static_cast<uint8_t>(launch_.phase(4) & 0xFF);
        for (uint16_t i = 0; i < pool_.count; i++) {
            if (pool_.ttl[i] == 0) continue;
            const lengthType px = static_cast<lengthType>(pool_.x[i] / draw::kSubOne);
            const lengthType py = static_cast<lengthType>(pool_.y[i] / draw::kSubOne);
            // Facing follows travel, as in the source effects.
            const bool flip = pool_.vx[i] < 0;
            // `hue` is spent on the kind, so the color index comes from the particle's own slot.
            const uint8_t entry = static_cast<uint8_t>(hashInt(i, 11) & 0xFF);
            spritecast::draw(cv, pool_.hue[i], entry, px, py, sc, flip, beat);
        }
    }

    /// Headroom enough that emission never stalls: the pool holds every sprite still in flight.
    static constexpr uint16_t kPoolSize = 192;
    ScratchBuffer<draw::pos_t> px_{*this}, py_{*this}, vx_{*this}, vy_{*this};
    ScratchBuffer<uint16_t> ttl_{*this};
    ScratchBuffer<uint8_t> hue_{*this};
    particles::Pool pool_;
    BeatPhase launch_;
    BeatPhase emit_;
    // The full phase counter: phase() keeps counting, so a narrower latch wraps and fires every frame.
    uint32_t  lastEmit_ = 0;
    particles::FrameTime time_;   // the physics clock: one step per reference frame, not per frame
    uint32_t seed_ = 0;
};

}  // namespace mm
