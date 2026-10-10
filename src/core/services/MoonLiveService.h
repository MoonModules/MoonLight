#pragma once

#include "core/module/MoonModule.h"
#include "core/moonlive/MoonLive.h"
#include "core/moonlive/MoonLiveBuiltins_service.h"
#include "core/moonlive/MoonLiveScript.h"   // the file, the compile and the declared controls

#include <cstring>

namespace mm {

namespace moonlive {

/// A service template: poll a pin on the 50 Hz tick, and write the control surface on a change.
inline constexpr const char* kServiceTemplate =
    "class NewService {\n"
    "  int pin = 0;\n"
    // 1 is the level an idle pull-up reads, so the first tick sees no change that never happened.
    "  int last = 1;\n"
    "\n"
    "  void defineControls() {\n"
    "    addControl(\"pin\", pin, 0, 48);\n"
    "  }\n"
    "\n"
    "  void tick20ms() {\n"
    "    int now = gpioRead(pin);\n"
    "    if (now != last) {\n"
    "      last = now;\n"
    // Inverted, since active-low wiring means a pressed button reads 0.
    "      setControl(\"switch1\", 1 - now);\n"
    "    }\n"
    "  }\n"
    "}\n";

/// What the `script` control tells the UI: the directory, the extension and the new-file template.
inline constexpr const char* kServicePick[3] = {kScriptDir, kServiceExt, kServiceTemplate};

}  // namespace moonlive

/// A scripted service: the input twin of a scripted effect, and the flexible half of input.
///
/// The compiled services are lists of mappings, wrong for anything with a condition in it.
/// A script holds the edge state a row cannot, and decides between outcomes.
/// So a sensor nobody wrote a module for needs a datasheet and eight lines, not a release.
/// @card MoonLiveService.png
///
/// @moreinfo
///
/// ## The same relationship effects already have
///
/// A compiled effect and a scripted one are interchangeable, and so are the input services.
/// The engine is identical, so a script author already knows the language.
/// Factory-registered like the others: added under the container, then pointed at a file.
///
/// ## It runs on the slow tick
///
/// A contact closes for tens of milliseconds and a sensor answers at its own rate.
/// The render rate would sample thousands of times to learn the same thing.
/// It also means a heavy script costs its own tick rather than stuttering the lights.
///
/// What it reaches: the pins for hardware, and the control surface for output.
/// That is the two-step model the mapping rows use, so a script drives the surface alone.
/// A script therefore cannot rewrite a driver's pins by naming it.
class MoonLiveService : public MoonModule {
public:
    /// A service, so the container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// Declare the script's name, then whatever controls the script itself declared.
    void defineControls() override {
        // The name, not the text: that lives in a file, so a module costs bytes not a kilobyte.
        controls_.addFilePath("script", script_.buffer(), script_.bufferSize(),
                              moonlive::kServicePick);
        // Each bound to its live slot, so a slider move lands with no recompile.
        script_.publishDeclaredControls(controls_);
        MoonModule::defineControls();
    }

    /// The script's controls exist once prepare has compiled it, so a saved value waits for that.
    bool declaresControlsAtPrepare() const override { return true; }

    /// Only naming a different script recompiles, a value change updating a byte the tick reads.
    bool affectsPrepare(const char* controlName) const override {
        return std::strcmp(controlName, "script") == 0;
    }

    /// Compile the script where the file has changed, then surface what it declares.
    void prepare() override {
        // A hash answers whether what is compiled still matches the file.
        script_.sync(moonlive::serviceSysVars(), *this, moonlive::serviceBuiltins());
        // The compile re-derives the declared set, so rebuild the list to surface it.
        rebuildControls();
    }

    /// The service moment, where a press and a sensor reading both live.
    void tick20ms() MM_NONBLOCKING override {
        if (!script_.ok()) return;
        if (!script_.engine().hasEntry(moonlive::kEntryTick20ms)) return;
        // Not the painting entry, which refuses a service for having no light buffer.
        script_.engine().runValue(moonlive::kEntryTick20ms, moonlive::RetType::Void, 0,
                                  nullptr, 0, 0, platform::millis());
    }

    /// Free the compiled block and forget it, so re-enabling rebuilds from the file.
    void release() override {
        script_.engine().free();      // release the exec block
        script_.invalidate();         // and forget what was compiled, so re-enabling rebuilds it
        script_.releaseReporting(*this);
        MoonModule::release();
    }

    /// Point the module at a script, which the next build compiles, as a UI edit would.
    void setScript(const char* name) { script_.setName(name); }

private:
    moonlive::MoonLiveScript script_;
};

}  // namespace mm
