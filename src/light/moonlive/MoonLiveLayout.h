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

/// Where the lights physically are, written as text on a running device.
///
/// @moreinfo
///
/// ## Why a layout is worth scripting
///
/// A layout is the one part of the pipeline that differs for every physical build, so each one has meant a new class, a rebuild and a reflash.
/// A script means the person who hung the lights describes where they went and sees it immediately.
///
/// ## The binding that needed the language to grow
///
/// A modifier transforms one coordinate because the Layer calls it per light, where a layout places every light itself, which takes a loop.
///
/// ## Why it still allocates nothing
///
/// The script calls `addLight` per light, and the binding points that call at a counter on the sizing pass and at the sink on the walk.
/// Staging the coordinates instead would cost 48 KB on a 16k-light fixture.
/// The count and the coordinates come from the same code, so the two answers cannot drift apart.
class MoonLiveLayout : public LayoutBase {
public:
    // 📝 marks a script declaring nothing of its own, which is all a module can say about one.
    /// The tags the script declares, or the scripted-module mark when it declares none.
    const char* tags() const override {
        const char* t = script_.tags();
        return t ? t : "📝";
    }

    // Advisory here rather than functional, since extrude reads the effect's dimensions.
    /// The axes this layout declares, which the card and the picker show.
    Dim dimensions() const override { return script_.dimensions(); }

    /// Publish the script name, and every control the script declares.
    void defineControls() override {
        // The script name rather than its text, so a module costs bytes instead of a kilobyte.
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

    // The Layer sizes its buffer from this before asking for a coordinate.
    /// Run the script, counting what it places.
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
    // Called from lightCount and placeLights too, because applyState runs parent-first.
    /// Compile if the source has changed since the program that is loaded.
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

    // Given a single scratch light, which satisfies run()'s precondition without staging anything.
    /// Point addLight at `fn` and run the script once.
    void runScript(moonlive::AddLightFn fn, void* ctx) const {
        uint8_t scratch[3] = {0, 0, 0};
        // Checked before the sink is installed, which would otherwise point at a dead frame.
        if (!script_.engine().hasEntry(moonlive::kEntryPlaceLights)) return;
        moonlive::setAddLightSink(fn, ctx);
        script_.engine().run(scratch, 1, 3, 0, moonlive::kEntryPlaceLights);
        moonlive::setAddLightSink(nullptr, nullptr);
    }

    // Empty on a fresh card, placing no lights until one is named.
    mutable moonlive::MoonLiveScript script_;

};

}  // namespace mm

