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

/// A MoonLive script that reads hardware and drives controls, the input twin of a scripted effect.
/// @card MoonLiveService.png
///
/// A mapping row is right for a button and wrong for anything with a condition in it. A script holds state and chooses between outcomes, so a sensor nobody wrote a module for needs a datasheet and eight lines, not a release.
///
/// @guide
///
/// ## Writing the script
///
/// Everything the script declares appears as a control on the card, and a slider move lands without a recompile. A compile error shows on the status line, and the service does nothing until it is fixed. A script runs on the 50 Hz poll rather than the render tick, so a heavy one costs its own tick.
///
/// ## Reading and sending JSON
///
/// A service reads JSON from the internet with `getJson`, and sends its own documents with `pushJson` to this device or `pushJsonTo` to another. A show builds its own look this way, live and never saved. The calls: [the MoonLive language](https://moonmodules.org/MoonLight/moonmodules/light/moonlive.html#json-documents-in-a-service).
///
/// @technical
///
/// **Same as effects.** A compiled effect and a scripted one are interchangeable, and so are the input services. The engine is identical, and the service is added under the container, then pointed at a file.
///
/// **Slow tick.** A contact closes for tens of milliseconds, so the render rate would sample thousands of times for one fact. A heavy script costs its own tick rather than stuttering the lights.
///
/// **Reach.** A script reaches the pins for hardware and the control surface for output, as the mapping rows do. It cannot rewrite a driver's pins by naming it.
class MoonLiveService : public MoonModule {
public:
    /// A service whose documents go with each program, so an edited script starts with none.
    MoonLiveService() {
        script_.setDefineHook([](void* self, moonlive::DefinePhase phase) {
            if (phase == moonlive::DefinePhase::Compiling) static_cast<MoonLiveService*>(self)->releaseDocuments();
        }, this);
    }

    /// A service, so the container accepts it as a child.
    ModuleRole role() const MM_NONBLOCKING override { return ModuleRole::Service; }

    /// Declare the script's name, then whatever controls the script itself declared.
    void defineControls() override {
        /// Which `.mls` file to run, picked from the script library.
        /// @guide Naming a different file recompiles it live. The name is stored, not the text, so a module costs bytes.
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

    /// A changed file may be this module's script, which the next prepare then reads again.
    void onFileChanged(const char* path) override { script_.fileChanged(path); }

    /// Free the compiled block and forget it, so re-enabling rebuilds from the file.
    void release() override {
        releaseDocuments();           // before the arena that names them goes
        script_.engine().free();      // release the exec block
        script_.invalidate();         // and forget what was compiled, so re-enabling rebuilds it
        script_.releaseReporting(*this);
        MoonModule::release();
    }

    /// Point the module at a script, which the next build compiles, as a UI edit would.
    void setScript(const char* name) { script_.setName(name); }

private:
    /// Free the JSON documents this script's program holds, named by its arena.
    void releaseDocuments() {
        if (const void* owner = script_.engine().arena()) JsonFetch::instance().release(owner);
    }

    moonlive::MoonLiveScript script_;
};

}  // namespace mm
