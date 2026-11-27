#pragma once

#include "light/modifiers/ModifierBase.h"
#include "core/services/AudioService.h"   // the beat a beat-synced turn lands on
#include "platform/platform_clock.h"   // millis

namespace mm {

/// Turns the 2D image around its center, continuously at the speed you set, or a step a beat.
/// @card RotateModifier.gif
///
/// `sync` on `beat` turns a step on each beat a source states, so `beats` 8 lands on 0, 45, 90 … 315° and back at 0 every eight beats, after troyhacks' idea; `ease` makes each step speed up and slow down into the beat rather than turn at one speed. With no beat running, or in silence, it holds still, as Beat Sweep goes dark.
///
/// @attribution Brandon502, Rotation matrix, troyhacks
///
/// @technical
///
/// The one dynamic modifier: it overrides modifyLive, so the Layer applies it every frame for a smooth turn rather than a stepped rebuild.
///
/// The live pass maps backward: for each destination cell it computes the source with the inverse rotation. That leaves no destination unfilled, the textbook reason image warping samples backward. A rotation is affine, so it hands the Layer one LiveAffine a frame and the Layer steps the source along each row.
class RotateModifier : public ModifierBase {
public:
    /// The catalog tags: 💫, a MoonLight origin.
    const char* tags() const override { return "💫"; }
    /// A rotation in the plane, so 2D, advisory for the UI chip.
    Dim dimensions() const override { return Dim::D2; }
    /// It animates, so it also transforms every frame.
    bool hasModifyLive() const MM_NONBLOCKING override { return true; }

    /// What drives the turn, in option order.
    enum Sync : uint8_t { kFree, kBeat, kSyncCount };

    /// A free turn, or a step on each beat.
    uint8_t sync = kFree;
    /// How fast the image turns on `free`.
    uint8_t speed = 1;
    /// The beats a turn takes on `beat`: 8 is 45° a beat.
    uint8_t beats = 8;
    /// How each step speeds up and slows down: 0 even, 255 a pause a beat.
    uint8_t ease = 128;

    /// The controls a user sets on the card: the sync first, then the speed on `free` or the beats and the ease on `beat`.
    void defineControls() override {
        static constexpr const char* kSyncOptions[] = {"free", "beat"};
        controls_.addSelect("sync", sync, kSyncOptions, kSyncCount);
        // defineControls reruns on every control change, so picking a sync shows its own controls.
        controls_.addControl("speed", speed, 1, 255);
        controls_.setHidden(controls_.count() - 1, sync != kFree);
        controls_.addControl("beats", beats, 1, 64);
        controls_.setHidden(controls_.count() - 1, sync != kBeat);
        controls_.addControl("ease", ease, 0, 255);
        controls_.setHidden(controls_.count() - 1, sync != kBeat);
    }

    /// No control forces a rebuild: the rotation is applied live rather than baked in.
    bool affectsPrepare(const char* /*controlName*/) const override { return false; }

    /// This frame's inverse rotation about the box's center, the map the Layer steps along each row.
    bool liveAffine(LiveAffine& out, const Coord3D& logical) const override {
        const int64_t c = static_cast<int64_t>(cos16(angle_)) * 65536 / 32767;   // exactly 1.0 at angle 0, so no turn is the identity
        const int64_t s = static_cast<int64_t>(sin16(angle_)) * 65536 / 32767;
        const int64_t cx = static_cast<int64_t>(logical.x - 1) * 32768;   // the center, (w − 1) / 2 in 16.16
        const int64_t cy = static_cast<int64_t>(logical.y - 1) * 32768;
        out.a = static_cast<int32_t>(c);
        out.b = static_cast<int32_t>(s);
        out.d = static_cast<int32_t>(-s);
        out.e = static_cast<int32_t>(c);
        out.c = static_cast<int32_t>(cx - ((c * cx + s * cy) >> 16));
        out.f = static_cast<int32_t>(cy - ((-s * cx + c * cy) >> 16));
        return true;
    }

    /// A backward map for one light, the same rotation the Layer steps per row; z passes through.
    void modifyLive(Coord3D& pos, const Coord3D& logical) const override {
        LiveAffine t;
        liveAffine(t, logical);
        t.apply(pos.x, pos.y);
    }

    /// The angle for this frame: on `beat` the beat's place with each step eased, held while no beat runs; on `free` the timer.
    void tick() MM_NONBLOCKING override {
        const uint32_t now = platform::millis();
        if (lastElapsed_ == 0) lastElapsed_ = now;
        const uint32_t dt = now - lastElapsed_;
        lastElapsed_ = now;
        if (sync == kBeat) {
            const BeatNow& beat = AudioService::beatNow();
            if (beat.live() && AudioService::latestFrame()->heard()) angle_ = beatAngle(beat.count, beat.phase, beats, ease);   // a tracker can hold its beat through a silence; the image holds too
            return;
        }
        phaseNum_ += static_cast<uint64_t>(dt) * speed;
        angle_ = static_cast<angle16>(phaseNum_ << 2);   // 16384 accumulator units a full turn, the rate it had, in 16-bit angle units
    }

    /// The angle on beat `count` at `phase` with `perTurn` beats a turn: whole steps on the beats, each step eased by `ease`.
    static angle16 beatAngle(uint16_t count, uint8_t phase, uint8_t perTurn, uint8_t ease) {
        const uint32_t n = perTurn ? perTurn : 1;
        const int32_t t = phase;                                       // 0..255 through the step
        const int32_t smooth = t * t * (765 - 2 * t) / 65025;          // smoothstep, 3t² − 2t³, on 0..255
        const int32_t step = t + (smooth - t) * ease / 255;            // the even step blended toward the smooth one
        return static_cast<angle16>(((count % n) * 256u + static_cast<uint32_t>(step)) * 65536u / (n * 256u));
    }

    /// This frame's angle, for the tests.
    angle16 angleForTest() const { return angle_; }

private:
    /// Current rotation angle, 65536 a full turn.
    angle16  angle_ = 0;
    /// Dt·speed accumulator.
    uint64_t phaseNum_ = 0;
    uint32_t lastElapsed_ = 0;
};

} // namespace mm
