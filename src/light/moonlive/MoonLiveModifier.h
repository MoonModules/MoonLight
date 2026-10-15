#pragma once

#include "core/moonlive/MoonLive.h"
#include "core/moonlive/MoonLiveScript.h"
#include "light/modifiers/ModifierBase.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include <cstdio>
#include <cstring>

namespace mm {

namespace moonlive {

/// What a new modifier script starts out as.
inline constexpr const char* kModifierTemplate =
    "class NewModifier {\n"
    "  void modifyLogical() {\n"
    "    setXYZ(width - 1 - xPos, yPos, zPos);\n"
    "  }\n"
    "}\n";

/// What the `script` control tells the UI: the directory, the extension and the new-file template.
inline constexpr const char* kModifierPick[3] = {kScriptDir, kModifierExt, kModifierTemplate};

}  // namespace moonlive

/// The coordinate transform written as text on the running device: mirror the pattern, shift it, swap its axes.
/// @card MoonLiveModifier.gif
///
/// A compiled modifier is a class and a reflash, where a script is a line of text applied as you type, and every control it declares appears on the card. The language is [MoonLive](https://moonmodules.org/MoonLight/moonmodules/light/moonlive.html).
///
/// @guide
///
/// ## One coordinate at a time
///
/// A script transforms one coordinate and needs no loop, because the Layer already calls it once per physical light while it builds the mapping.
///
/// `setXYZ(x, y, z)` writes the transformed position, as `setRGB(index, r, g, b)` writes a color: a mirror is `void modifyLogical() { setXYZ(width - 1 - xPos, yPos, zPos); }`.
///
/// ## Read the grid rather than assuming it
///
/// A mirror written against a fixed 255 sends every light of a 16-wide grid outside it. The Layer drops each one, and the fixture goes black with no error, because the script ran perfectly.
///
/// A computed coordinate is full width: `setXYZ` hands its values to the binding as a call, so `setXYZ(767 - xPos, ...)` on a 768-wide wall arrives as 767.
///
/// ## What a script can transform
///
/// A modifier has two hooks: one reshapes the logical box once per rebuild, and one folds each coordinate. A script drives the second, so a transform keeping the box its size works, and one halving it, as the built-in Mirror does, needs the compiled modifier.
///
/// A script that fails to compile shows the parse error, and the mapping passes coordinates straight through, so the device keeps rendering until the script parses again.
///
/// @moreinfo
///
/// The engine is domain-neutral: an effect script writes a color per light and a modifier a position, both three values stored at an index.
///
/// A coordinate arriving outside the representable range passes through untransformed rather than wrapping to a wrong position; an out-of-range result is the script's own.
class MoonLiveModifier : public ModifierBase {
public:
    /// The tags the script declares, or 📝 for one declaring none, which is all a module can say about it.
    const char* tags() const override {
        const char* t = script_.tags();
        return t ? t : "📝";
    }

    /// The axes the script declares, for the card and the picker only, since extrude reads the effect's dimensions.
    Dim dimensions() const override { return script_.dimensions(); }

    /// Publish the script name, and every control the script declares.
    void defineControls() override {
        // The script's name rather than its text, so a module costs bytes instead of a kilobyte.
        controls_.addFilePath("script", script_.buffer(), script_.bufferSize(),
                              moonlive::kModifierPick);
        // Every control the script declared: a system variable is not one, so none appear here.
        script_.publishDeclaredControls(controls_);
    }

    /// The script's controls appear once prepare has compiled it.
    bool declaresControlsAtPrepare() const override { return true; }

    /// Compile the script as written, on every control change, since a source edit and a control move both change where lights land.
    void prepare() override {
        // Only when a new program was installed, since the Layer's rebuild calls prepare() again.
        if (script_.sync(moonlive::modifierSysVars(), *this, moonlive::lightBuiltins())) needsRebuild_ = true;
        rebuildControls();
    }

    /// The Layer polls this after ticking its modifiers and rebuilds its mapping once if any asks.
    bool consumeNeedsRebuild() override {
        const bool r = needsRebuild_;
        needsRebuild_ = false;
        return r;
    }

    /// Stash the running logical box, so the script can read `width`, `height` and `depth`.
    void modifyLogicalSize(Coord3D& size) override { box_ = size; }

    /// Transform one coordinate, called once per physical light while the mapping builds.
    bool modifyLogical(Coord3D& pos) const override {
        if (!script_.ok()) return true;   // a broken script passes coordinates through unchanged
        // Negatives only: the slots are 32-bit, so the whole rig is scriptable.
        if (pos.x < 0 || pos.y < 0 || pos.z < 0) return true;

        auto* self = const_cast<MoonLiveModifier*>(this);
        uint8_t* sx = self->script_.engine().controlSlot(moonlive::kSysX);
        uint8_t* sy = self->script_.engine().controlSlot(moonlive::kSysY);
        uint8_t* sz = self->script_.engine().controlSlot(moonlive::kSysZ);
        if (!sx || !sy || !sz) return true;
        moonlive::writeSysVarSlot(sx, static_cast<uint32_t>(pos.x));
        moonlive::writeSysVarSlot(sy, static_cast<uint32_t>(pos.y));
        moonlive::writeSysVarSlot(sz, static_cast<uint32_t>(pos.z));
        // The box at full width, so a grid wider than 255 reports its real size.
        moonlive::writeSysVarSlot(self->script_.engine().controlSlot(moonlive::kSysWidth),
                                  static_cast<uint32_t>(box_.x < 0 ? 0 : box_.x));
        moonlive::writeSysVarSlot(self->script_.engine().controlSlot(moonlive::kSysHeight),
                                  static_cast<uint32_t>(box_.y < 0 ? 0 : box_.y));
        moonlive::writeSysVarSlot(self->script_.engine().controlSlot(moonlive::kSysDepth),
                                  static_cast<uint32_t>(box_.z < 0 ? 0 : box_.z));

        // Run `modifyLogical` when the script defined one, leaving the coordinate untouched.
        if (!script_.engine().hasEntry(moonlive::kEntryModify)) return true;

        // Seeded with the input, so a script writing nothing leaves the coordinate untouched.
        struct Out { uint32_t x, y, z; } out{static_cast<uint32_t>(pos.x),
                                             static_cast<uint32_t>(pos.y),
                                             static_cast<uint32_t>(pos.z)};
        moonlive::setCoordSink([](void* ctx, uint32_t x, uint32_t y, uint32_t z) {
            auto* o = static_cast<Out*>(ctx);
            o->x = x; o->y = y; o->z = z;
        }, &out);
        uint8_t scratch[3] = {0, 0, 0};   // the run buffer: unused by a modifier, which writes
                                          // only through the sink above
        self->script_.engine().run(scratch, 1, 3, 0, moonlive::kEntryModify);
        moonlive::setCoordSink(nullptr, nullptr);

        pos.x = static_cast<lengthType>(out.x);
        pos.y = static_cast<lengthType>(out.y);
        pos.z = static_cast<lengthType>(out.z);
        return true;
    }

    /// Drop the compiled program and its memory.
    void release() override {
        script_.engine().free();
        // Forget what was compiled, so the next prepare is a first compile.
        script_.invalidate();
        script_.releaseReporting(*this);
        ModifierBase::release();
    }

    /// Replace the script, which the next prepare compiles by the path a UI edit takes.
    void setScript(const char* name) { script_.setName(name); }

private:
    /// Which `.mlm` script runs, picked from the library and edited here.
    mutable moonlive::MoonLiveScript script_;

    bool needsRebuild_ = false;   ///< a recompile happened; the Layer's mapping is stale
    Coord3D box_{0, 0, 0};        ///< the logical box, from modifyLogicalSize
};

}  // namespace mm

