#pragma once

#include "light/effects/EffectBase.h"
#include "core/moonlive/MoonLive.h"
#include "core/moonlive/MoonLiveScript.h"
#include "light/moonlive/MoonLiveParticles.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include <cstring>
#include <cstdio>

namespace mm {

namespace moonlive {

/// What a new effect script starts out as: a working example, since an empty file fails to parse the moment it is made.
inline constexpr const char* kEffectTemplate =
    "class NewEffect {\n"
    "  byte bpm = 60;\n"
    "\n"
    "  void defineControls() {\n"
    "    addControl(\"bpm\", bpm, 1, 255);\n"
    "  }\n"
    "\n"
    "  void tick() {\n"
    "    fill(scale(beat(bpm, t), 256), 0, 100);\n"
    "  }\n"
    "}\n";

/// What the `script` control tells the UI: the directory, the extension and the new-file template.
inline constexpr const char* kEffectPick[3] = {kScriptDir, kEffectExt, kEffectTemplate};

}  // namespace moonlive

/// An effect written as text on the running device, compiled to native code on the next tick.
/// @card MoonLiveEffect.gif
///
/// Pick a script from the library or write your own, and it renders at the speed of a compiled effect. Every control the script declares appears on the card and is editable live without a recompile. The language is [MoonLive](https://moonmodules.org/MoonLight/moonmodules/light/moonlive.html).
///
/// @attribution Yves Bazin
///
/// @moreinfo
///
/// The effect names a script file, `prepare` compiles it, and `tick` runs the emitted native code over this effect's own buffer. A source edit recompiles live, and a parse error shows in the module status while the layer renders dark.
class MoonLiveEffect : public EffectBase {
public:
    /// The tags the script declares, or 📝 for one declaring none, which is all a module can say about it.
    const char* tags() const override {
        const char* t = script_.tags();
        return t ? t : "📝";
    }

    /// The axes this script paints, which a silent script leaves at two, and which the layer extrudes on.
    Dim dimensions() const override { return script_.dimensions(); }

    /// Publish the script name, and every control the script declares, each bound by reference to its arena slot so a slider write needs no recompile.
    void defineControls() override {
        // The script name rather than its text, so a module costs bytes instead of a kilobyte.
        controls_.addFilePath("script", script_.buffer(), script_.bufferSize(),
                              moonlive::kEffectPick);
        // Every control the script declared: a system variable is not one, so none appear here.
        script_.publishDeclaredControls(controls_);
    }

    /// Whether a control change needs the prepare sweep: only the script name does, as a scripted control updates an arena byte the next tick reads.
    bool affectsPrepare(const char* controlName) const override {
        return std::strcmp(controlName, "script") == 0;
    }

    /// The script's controls appear once prepare has compiled it.
    bool declaresControlsAtPrepare() const override { return true; }

    /// Compile the script if the file changed, then surface whatever it declares; a failed compile leaves tick a no-op, so the effect renders dark.
    void prepare() override {
        // The next tick is the first, so the idle interval is not handed to the flow and the decay.
        tickStarted_ = false;
        // The script sizes its own pool and trail from defineControls(), which sync() runs after a compile.
        script_.setDefineHook(&MoonLiveEffect::attachSizers, this);
        script_.sync(moonlive::effectSysVars(), *this, moonlive::lightBuiltins());
        // The planes follow the fixture, so a resize re-sizes them though the script did not.
        if (trailWanted_) resizeTrail(true);
        // Unconditional, since a walk costs little and keeps the card correct after any prepare.
        rebuildControls();
    }

    void tick() MM_NONBLOCKING override {
        // The emitter writes three channels at a stride, so a sub-RGB layout renders dark instead.
        const auto cpl = channelsPerLight();
        if (cpl < 3) return;
        if (!script_.ok()) return;
        // Refreshed before the run, since a layer resizes live and stale width draws the old box.
        writeSysVar(moonlive::kSysWidth,  width());
        writeSysVar(moonlive::kSysHeight, height());
        writeSysVar(moonlive::kSysDepth,  depth());
        // Installed for one run and detached after, so a script draws only into its own layer.
        moonlive::setDrawCanvas(canvas());
        // fade(amt) asks the layer, which collects the request and applies it once per frame.
        moonlive::setFadeSink([](void* ctx, uint8_t amt) {
            if (Layer* l = static_cast<MoonLiveEffect*>(ctx)->layer()) l->fadeToBlackBy(amt);
        }, this);
        // Routed through EffectBase's own setters, so a script aims a head as a compiled effect does.
        moonlive::setMotionSink([](void* ctx, moonlive::MotionAxis axis, uint32_t index,
                                   uint8_t value) {
            auto* self = static_cast<MoonLiveEffect*>(ctx);
            const auto i = static_cast<nrOfLightsType>(index);
            switch (axis) {
                case moonlive::MotionAxis::Pan:    self->setPan(i, value);    break;
                case moonlive::MotionAxis::Tilt:   self->setTilt(i, value);   break;
                case moonlive::MotionAxis::Zoom:   self->setZoom(i, value);   break;
                case moonlive::MotionAxis::Rotate: self->setRotate(i, value); break;
                case moonlive::MotionAxis::Gobo:   self->setGobo(i, value);   break;
            }
        }, this);
        // Framerate independence is the system's property rather than the script author's.
        if (particles_.count() > 0)
            moonlive::setPoolSink(&particles_.pool(), particles_.advance(elapsed()));
        // The binding owns the geometry, the ping-pong and the delta, none of which is the author's.
        const uint32_t nowMs = elapsed();
        // A zero delta on the first tick, since the whole uptime would teleport the trail.
        const uint32_t dt = tickStarted_ ? nowMs - lastTickMs_ : 0u;
        lastTickMs_ = nowMs;
        tickStarted_ = true;
        if (trailA_) {
            moonlive::FlowSink f{};
            f.a = trailA_.data();
            f.b = trailB_.data();
            f.front = &trailFront_;
            f.frame = &frameCount_;
            f.w = width(); f.h = height(); f.d = depth();
            f.dtMs = dt;
            moonlive::setFlowSink(f);
        }
        // Run `tick` when the script defined one: a script defining only a fold renders nothing.
        if (script_.engine().hasEntry(moonlive::kEntryTick))
            script_.engine().run(buffer(), nrOfLights(), cpl, elapsed(), moonlive::kEntryTick);
        // The one narrowing step a script cannot do itself, without which the plane is never read.
        if (trailA_) {
            moonlive::setFlowSink({});
            blitTrail();
        }
        // Outside the trail branch, since `fieldRate(n)` is a rate limiter any script may use.
        frameCount_++;
        moonlive::setPoolSink(nullptr, 0);
        moonlive::setMotionSink(nullptr, nullptr);
        moonlive::setFadeSink(nullptr, nullptr);
        moonlive::setDrawCanvas({});
    }

    /// Drop the compiled program, the trail planes and the particle pool.
    void release() override {
        particles_.release();      // zero the pool BEFORE the base frees its buffers, or it would
                                   // be left naming freed memory
        script_.engine().free();   // release the exec block: the destructor role
        script_.invalidate();     // and forget what was compiled, so re-enabling rebuilds it
        script_.releaseReporting(*this);
        // Forget the shape as well as the memory, or the next prepare compares against a dead one.
        releaseTrail();
        EffectBase::release();
    }

    /// Replace the script, which the next prepare compiles by the path a UI edit takes.
    void setScript(const char* name) { script_.setName(name); }

private:
    /// Write a system variable at full width, since saturating to a byte made a 768-wide wall report 255.
    void writeSysVar(uint8_t offset, uint32_t value) {
        moonlive::writeSysVarSlot(script_.engine().controlSlot(offset), value);
    }

    /// Free the pool and trail before a compile, so it has their memory (two 16-bit planes are 96 KB on a 20-cube). Then attach pool(n) and trail(1) for its defineControls run, or detach them.
    static void attachSizers(void* ctx, moonlive::DefinePhase phase) {
        auto* self = static_cast<MoonLiveEffect*>(ctx);
        if (phase == moonlive::DefinePhase::Compiling) {
            self->particles_.release();
            self->trailWanted_ = false;
            self->releaseTrail();
            return;
        }
        if (phase == moonlive::DefinePhase::Detach) {
            moonlive::setPoolSizeSink(nullptr, nullptr);
            moonlive::setTrailSizeSink(nullptr, nullptr);
            return;
        }
        moonlive::setPoolSizeSink([](void* c, uint16_t n) -> uint16_t {
            return static_cast<MoonLiveEffect*>(c)->particles_.resize(n);
        }, ctx);
        moonlive::setTrailSizeSink([](void* c, bool want) -> bool {
            return static_cast<MoonLiveEffect*>(c)->resizeTrail(want);
        }, ctx);
    }

    moonlive::MoonLiveScript script_;   ///< Which `.mle` script runs, picked from the library and edited here.
    moonlive::MoonLiveParticles particles_{*this};
    ScratchBuffer<uint16_t> trailA_{*this};   ///< the trail planes, two since advection reads one and writes the other
    ScratchBuffer<uint16_t> trailB_{*this};   ///< the second trail plane
    ScratchBuffer<uint8_t>  trailCarry_{*this};   ///< the dither's per-channel error
    bool                    trailFront_ = true;
    uint32_t                frameCount_ = 0;   ///< the counter fieldRate reads
    bool                    trailWanted_ = false;
    uint32_t                lastTickMs_ = 0;
    bool                    tickStarted_ = false;   ///< has a frame been timed yet
    lengthType              trailW_ = 0, trailH_ = 0, trailD_ = 0;  ///< the shape the planes hold

    /// Blit the trail plane onto the layer through draw::blit16, like the compiled effects.
    void blitTrail() {
        const lengthType w = width(), h = height(), d = depth();
        if (!trailA_ || !trailB_) return;
        const uint16_t* live = trailFront_ ? trailA_.data() : trailB_.data();
        draw::blit16(canvas(), live, w, h, d, trailCarry_ ? trailCarry_.data() : nullptr);
    }

    /// Size or free the trail planes, returning whether one is available for the script to use.
    bool resizeTrail(bool want) {
        trailWanted_ = want;
        // Every exit frees all three together, since the carry is as much the trail as the planes.
        if (!want) { releaseTrail(); return false; }
        const lengthType w = width(), h = height(), d = depth();
        const size_t n = static_cast<size_t>(w) * h * d * 3;
        if (n == 0) { releaseTrail(); return false; }
        const size_t had = trailA_.count();
        if (!trailA_.resize(n) || !trailB_.resize(n) || !trailCarry_.resize(n)) {
            releaseTrail();                             // a half-allocated set is worse than none
            return false;
        }
        // A same-count reshape keeps samples laid out for the old geometry, so both planes clear.
        if (n == had && (w != trailW_ || h != trailH_ || d != trailD_)) {
            std::memset(trailA_.data(), 0, trailA_.bytes());
            std::memset(trailB_.data(), 0, trailB_.bytes());
            std::memset(trailCarry_.data(), 0, trailCarry_.bytes());   // per light, so it reshapes too
        }
        trailW_ = w; trailH_ = h; trailD_ = d;
        return true;
    }

    /// Free the trail set. One place, so a new buffer cannot be forgotten on one exit path.
    void releaseTrail() {
        trailA_.resize(0); trailB_.resize(0); trailCarry_.resize(0);
        trailW_ = trailH_ = trailD_ = 0;
    }
};

}  // namespace mm

