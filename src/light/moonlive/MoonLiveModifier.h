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

/// A coordinate transform authored live rather than compiled in.
///
/// @moreinfo
///
/// This is the second binding of the engine, and what shows the engine is domain-neutral: it needed no engine, IR, grammar or backend change.
/// An effect script writes a color per light and a modifier a position, and both are three values stored at an index.
///
/// ## Why the script does not loop
///
/// The Layer already does: `modifyLogical` is called once per physical light while the mapping builds, so a script transforms one coordinate.
///
/// A coordinate arriving outside the representable range passes through untransformed rather than wrapping to a wrong position.
/// An out-of-range result is the script's own.
class MoonLiveModifier : public ModifierBase {
public:
    // 📝 marks a script declaring nothing of its own, which is all a module can say about one.
    /// The tags the script declares, or the scripted-module mark when it declares none.
    const char* tags() const override {
        const char* t = script_.tags();
        return t ? t : "📝";
    }

    // Advisory here rather than functional, since extrude reads the effect's dimensions.
    /// The axes this modifier declares, which the card and the picker show.
    Dim dimensions() const override { return script_.dimensions(); }

    /// Publish the script name, and every control the script declares.
    void defineControls() override {
        // The script name rather than its text, so a module costs bytes instead of a kilobyte.
        controls_.addFilePath("script", script_.buffer(), script_.bufferSize(),
                              moonlive::kModifierPick);
        // Every control the script declared: a system variable is not one, so none appear here.
        script_.publishDeclaredControls(controls_);
    }

    /// The script's controls appear once prepare has compiled it.
    bool declaresControlsAtPrepare() const override { return true; }

    // Every control rebuilds: a source edit and a control move both change where lights land.
    /// Compile the script as written.
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
    // Empty on a fresh card, passing coordinates through untouched until one is named.
    mutable moonlive::MoonLiveScript script_;

    bool needsRebuild_ = false;   // a recompile happened; the Layer's mapping is stale
    Coord3D box_{0, 0, 0};        // the logical box, from modifyLogicalSize
};

}  // namespace mm

