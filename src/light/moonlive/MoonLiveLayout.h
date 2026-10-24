#pragma once

#include "core/moonlive/MoonLive.h"
#include "core/moonlive/MoonLiveScript.h"
#include "light/layouts/LayoutBase.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include <cstdio>
#include <cstring>

namespace mm {

namespace moonlive {

/// What a new layout script starts out as.
inline constexpr const char* kLayoutTemplate =
    "class NewLayout {\n"
    "  byte cols = 16;\n"
    "  byte rows = 16;\n"
    "\n"
    "  void defineControls() {\n"
    "    addControl(\"cols\", cols, 1, 64);\n"
    "    addControl(\"rows\", rows, 1, 64);\n"
    "  }\n"
    "\n"
    "  void placeLights() {\n"
    "    for (y = 0; y < rows; y = y + 1) {\n"
    "      for (x = 0; x < cols; x = x + 1) {\n"
    "        addLight(x, y, 0);\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "}\n";

/// What the `script` control tells the UI: the directory, the extension and the new-file template.
inline constexpr const char* kLayoutPick[3] = {kScriptDir, kLayoutExt, kLayoutTemplate};

}  // namespace moonlive

/// Where the lights physically are, written as text on the running device.
/// @card MoonLiveLayout.gif
///
/// A layout is the one part of the pipeline that differs for every build: a ring, a spiral staircase, a costume sewn last night. A script means the person who hung the lights describes where they went, and sees it at once. Everything the script declares appears as a real control. The language is [MoonLive](https://moonmodules.org/MoonLight/moonmodules/light/moonlive.html).
///
/// @guide
///
/// ## How the light count is known
///
/// The Layer sizes its buffer from the light count before it asks for any position, so the script runs twice: the first pass counts `addLight` calls, the second emits positions. Nothing is stored between passes, which saves 48 KB at 16,384 lights. A `random16` loop bound makes the passes disagree on the count.
///
/// ## What a layout script is given
///
/// `addLight(x, y, z)` places the next light, in call order. `t` is always 0 and `width`, `height` and `depth` read 0, since a layout is upstream of the grid. Name your own size controls, such as `cols` and `rows`. [The MoonLive guide](https://moonmodules.org/MoonLight/moonmodules/light/moonlive.html#system-variables) lists the vocabulary.
///
/// @technical
///
/// A layout script places every light itself in a loop, unlike a modifier the Layer calls per light. `addLight` binds to a counter on the sizing pass and to the sink on the walk.
class MoonLiveLayout : public LayoutBase {
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
        // The script's name rather than its text, so a module costs bytes instead of a kilobyte; empty on a fresh card, it places no lights until one is named.
        controls_.addFilePath("script", script_.buffer(), script_.bufferSize(),
                              moonlive::kLayoutPick);
        // A layout receives no width: the pipeline derives the box from the coordinates placed.
        script_.publishDeclaredControls(controls_);
    }

    /// The script's controls appear once prepare has compiled it.
    bool declaresControlsAtPrepare() const override { return true; }

    /// Compile the script, where the lights themselves are placed by whoever asks.
    void prepare() override {
        compile();
        rebuildControls();
    }

    /// Run the script, counting what it places: the Layer sizes its buffer from this before asking for a coordinate.
    nrOfLightsType lightCount() const override {
        compile();
        if (!script_.ok()) return 0;
        Counter c{0};
        runScript(&addToCounter, &c);
        return c.n;
    }

    /// Run the script again, emitting each light into the consumer's sink.
    void placeLights(const CoordSink& sink) const override {
        compile();
        if (!script_.ok()) return;
        Emitter e{&sink, 0};
        runScript(&addToSink, &e);
    }

    /// A changed file may be this module's script, which the next prepare then reads again.
    void onFileChanged(const char* path) override { script_.fileChanged(path); }

    /// Drop the compiled program and its memory.
    void release() override {
        script_.engine().free();
        script_.invalidate();     // forget what was compiled, so re-enabling rebuilds it
        script_.releaseReporting(*this);
        LayoutBase::release();
    }

    /// Nothing to do on a control write, since compile re-derives from the file every time.
    void onControlChanged(const char* name) override {
        // Nothing to invalidate: a content hash notices a write that lands in the name buffer.
        (void)name;
    }

    /// Point the layout at a script in the shared script directory; the next prepare() compiles it.
    void setScript(const char* name) { script_.setName(name); }

private:
    /// Compile if the source has changed since the program that is loaded, called from lightCount and placeLights too, because applyState runs parent-first.
    void compile() const {
        // A content hash makes an unchanged call cost a read rather than a recompile.
        auto* self = const_cast<MoonLiveLayout*>(this);
        self->script_.sync(moonlive::layoutSysVars(), *self, moonlive::lightBuiltins());
    }

    struct Counter { nrOfLightsType n; };
    struct Emitter { const CoordSink* sink; nrOfLightsType idx; };

    static void addToCounter(void* ctx, uint16_t, uint16_t, uint16_t) {
        static_cast<Counter*>(ctx)->n++;
    }
    static void addToSink(void* ctx, uint16_t x, uint16_t y, uint16_t z) {
        auto* e = static_cast<Emitter*>(ctx);
        e->sink->pixel(e->idx++, static_cast<lengthType>(x),
                       static_cast<lengthType>(y), static_cast<lengthType>(z));
    }

    /// Point addLight at `fn` and run the script once, given a single scratch light, which satisfies run()'s precondition without staging anything.
    void runScript(moonlive::AddLightFn fn, void* ctx) const {
        uint8_t scratch[3] = {0, 0, 0};
        // Checked before the sink is installed, which would otherwise point at a dead frame.
        if (!script_.engine().hasEntry(moonlive::kEntryPlaceLights)) return;
        moonlive::setAddLightSink(fn, ctx);
        script_.engine().run(scratch, 1, 3, 0, moonlive::kEntryPlaceLights);
        moonlive::setAddLightSink(nullptr, nullptr);
    }

    /// Which `.mll` script runs, picked from the library and edited here.
    mutable moonlive::MoonLiveScript script_;

};

}  // namespace mm

