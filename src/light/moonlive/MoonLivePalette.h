#pragma once

#include "core/moonlive/MoonLive.h"
#include "core/moonlive/MoonLiveScript.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include "light/util/Palette.h"

namespace mm {

namespace moonlive {

/// What a new palette script starts out as.
inline constexpr const char* kPaletteTemplate =
    "class NewPalette {\n"
    "  byte bpm = 20;\n"
    "\n"
    "  void defineControls() {\n"
    "    addControl(\"bpm\", bpm, 1, 120); // how fast the colors move\n"
    "  }\n"
    "\n"
    "  void tick() {\n"
    "    for (int i = 0; i < 16; i = i + 1) {\n"
    "      setPalEntryHSV(i, scale(beat(bpm, t), 256) + i * 16, 255, 255);\n"
    "    }\n"
    "  }\n"
    "}\n";

/// What the `script` control tells the UI: the directory, the extension and the new-file template.
inline constexpr const char* kPalettePick[3] = {kScriptDir, kPaletteExt, kPaletteTemplate};

}  // namespace moonlive

/// A palette authored as code rather than stored as data.
///
/// @moreinfo
///
/// This is the fifth binding, and the one shape the other four do not have: it writes sixteen entries once per frame, and every effect in that frame samples them.
///
/// ## Why a palette is worth computing
///
/// A stop list is frozen the moment it is saved, where a palette that is code can follow the music, drift, or be computed.
/// The audio builtins are already in the shared table, so `audioBand` in a palette costs nothing to expose, and it is the case that justifies the design.
///
/// ## Why it is cheap enough to run per frame
///
/// Cost is independent of rig size: a sixteen-iteration loop whatever the wall is.
/// It fills a scratch palette and assigns the 48 bytes once, so an effect sees either the previous palette or the new one and never a mixture.
///
/// ## Why a broken script keeps the last good palette
///
/// The other bindings degrade to dark, which is honest when the script is the picture.
/// Here the effects still run, so a black palette would blame them for a fault that is not theirs.
class MoonLivePalette {
public:
    /// Point the palette at a script. The next prepare() compiles it.
    void setScript(const char* name) { script_.setName(name); }
    /// The script file this palette runs.
    const char* scriptName() const { return script_.name(); }

    /// Compile if the named file changed, reporting status and dynamic bytes through `owner`.
    void prepare(MoonModule& owner) {
        script_.sync(moonlive::effectSysVars(), owner, moonlive::lightBuiltins());
    }

    /// Publish the script's controls, so a scripted palette is configurable without editing it.
    void publishControls(ControlList& controls) { script_.publishDeclaredControls(controls); }

    /// Whether a script is compiled and ready to run.
    bool ok() const { return script_.ok(); }

    /// Drop the compiled script and its memory.
    void release() {
        script_.engine().free();
        script_.invalidate();
    }

    // `t` is the elapsed milliseconds every other binding is handed, so `beat` means one thing.
    /// Run the script for this frame and install the result, returning false when nothing ran.
    bool tick(uint32_t nowMs) MM_NONBLOCKING {
        if (!script_.ok()) return false;
        if (!script_.engine().hasEntry(moonlive::kEntryTick)) return false;

        // Seeded from the active palette, so a script writing some entries leaves the rest alone.
        Palette scratch = *Palettes::active();
        moonlive::setPalSink([](void* ctx, uint8_t i, uint8_t r, uint8_t g, uint8_t b) {
            static_cast<Palette*>(ctx)->entry[i] = RGB{r, g, b};
        }, &scratch);
        // runValue rather than run(), which refuses a call with no light buffer: a palette paints none.
        script_.engine().runValue(moonlive::kEntryTick, moonlive::RetType::Void, 0,
                                  nullptr, 0, 0, nowMs);
        moonlive::setPalSink(nullptr, nullptr);

        // One assignment of 48 bytes: an effect samples a whole palette or the previous one.
        Palettes::setActiveDirect(scratch);
        return true;
    }

    // A static seam because Drivers owns the script but ticks after the effects that sample it.
    /// The palette script the frame should run, or nullptr.
    static MoonLivePalette* active() { return active_; }
    /// Publish the palette script the frame should run.
    static void setActiveInstance(MoonLivePalette* p) { active_ = p; }
    // Takes the caller's own instance, so a departing owner cannot unpublish somebody else's.
    /// Detach, for an owner whose storage is about to go away.
    static void clearActiveInstance(const MoonLivePalette* p) { if (active_ == p) active_ = nullptr; }

    /// Run the active palette script, if there is one. Called once per frame before the layers.
    static void tickActive(uint32_t nowMs) MM_NONBLOCKING {
        if (active_) active_->tick(nowMs);
    }

private:
    moonlive::MoonLiveScript script_;
    static inline MoonLivePalette* active_ = nullptr;
};

}  // namespace mm

