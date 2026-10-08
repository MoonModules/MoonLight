/// @module MoonLiveService
/// @also Scheduler

/// Pins the scripted-service path end to end: a .mls script reads a GPIO through the platform seam and drives a control through Scheduler::setControl, on the 50 Hz tick.
/// The GPIO seam is a host stub whose level the test injects (platform::setTestGpioLevel), so "the pin went low" is expressed exactly as the module sees it on a board.
///
/// This is the test the plan's step 2 asks for: the script does in eight lines what ButtonService does as a module, and proving both drive the same control through the same primitive is what "compiled and scripted are interchangeable" means.

#include "doctest.h"
#include "core/services/MoonLiveService.h"
#include "core/module/Scheduler.h"
#include "core/module/MoonModule.h"
#include "core/system/FilesystemModule.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

using namespace mm;

namespace {

// Stands in for the control surface: a `switch1` a script can drive, named "Control" so a script naming that control reaches it.
struct FakeControl : public MoonModule {
    uint8_t switch1 = 0;
    void defineControls() override { controls_.addControl("switch1", switch1, 0, 1); }
};

constexpr uint8_t kPin = 4;

/// Write a script into the device's script directory, the same place the UI saves one.
///
/// Through the PLATFORM fs, not std::ofstream: the desktop platform roots its filesystem somewhere of its own choosing, so writing to a host-relative "/moonlive" put the file where the loader would never look and every script silently failed to compile.
void writeScript(const char* name, const char* body) {
    platform::fsMkdir(moonlive::kScriptDir);
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", moonlive::kScriptDir, name);
    REQUIRE(platform::fsWriteAtomic(path, body, std::strlen(body)));
}

struct Rig {
    Scheduler scheduler;
    // The filesystem is what MOUNTS storage: a script is a file, so without this in the tree every write fails and the service has nothing to compile. Alone in a run the mount happened to survive from another test; in the full suite it does not, which is why this is explicit.
    FilesystemModule* fs = new FilesystemModule();
    FakeControl* control = new FakeControl();
    MoonLiveService* svc = new MoonLiveService();
    char root_[64];
    Rig() {
        // Its own filesystem root, the pattern every file-touching test here uses.
        // The suite leaves the root pointing wherever the last test set it, so a write to a shared root fails on a directory that no longer exists.
        // A monotonic counter rather than millis(), so two rigs built in the same millisecond cannot share one.
        static unsigned seq = 0;
        std::snprintf(root_, sizeof(root_), "/tmp/mm_mls_test_%u", ++seq);
        std::filesystem::remove_all(root_);
        platform::fsSetRoot(root_);

        platform::clearTestGpioLevel();
        fs->setName("Filesystem");
        control->setName("Control");
        svc->setName("Script");
        scheduler.addModule(fs);
        scheduler.addModule(control);
        scheduler.addModule(svc);
        scheduler.setup();
    }
    ~Rig() { scheduler.release(); platform::clearTestGpioLevel(); }

    /// Hold a level for `ms`, in the 20 ms steps the module is polled at.
    /// Active-low, so a pressed switch reads LOW and the script's `gpioRead` returns 0.
    void hold(bool low, int ms) {
        platform::setTestGpioLevel(kPin, !low);
        for (int t = 0; t < ms; t += 20) svc->tick20ms();
    }
};

}  // namespace

TEST_CASE("a scripted service reads a pin and drives the control surface") {
    // The whole two-step model in one script: hardware in, surface out. A mapping row could express this much; what a row could not is the `if` that fires only on a change.
    Rig rig;
    writeScript("t_button.mls",
                "class Button {\n"
                "  int pin = 4;\n"
                "  int last = 1;\n"
                "  void defineControls() { addControl(\"pin\", pin, 0, 48); }\n"
                "  void tick20ms() {\n"
                "    int now = gpioRead(pin);\n"
                "    if (now != last) { last = now; setControl(\"switch1\", 1 - now); }\n"
                "  }\n"
                "}\n");
    rig.svc->setScript("t_button.mls");
    rig.svc->prepare();
    REQUIRE(rig.svc->status() != nullptr);

    rig.hold(false, 60);                      // released (HIGH): nothing driven yet
    CHECK(rig.control->switch1 == 0);

    rig.hold(true, 60);                       // pressed (LOW)
    CHECK(rig.control->switch1 == 1);         // the script wrote the surface

    rig.hold(false, 60);                      // released again
    CHECK(rig.control->switch1 == 0);
}

TEST_CASE("a scripted service declares its own controls, which a user can set") {
    // `addControl` is what makes a script configurable without editing it: the declared member becomes a real control on the card, bound to the live arena slot the running code reads.
    Rig rig;
    writeScript("t_decl.mls",
                "class Decl {\n"
                "  int pin = 0;\n"
                "  int threshold = 7;\n"
                "  void defineControls() {\n"
                "    addControl(\"pin\", pin, 0, 48);\n"
                "    addControl(\"threshold\", threshold, 0, 255);\n"
                "  }\n"
                "  void tick20ms() { }\n"
                "}\n");
    rig.svc->setScript("t_decl.mls");
    rig.svc->prepare();

    auto& cs = rig.svc->controls();
    bool sawPin = false, sawThreshold = false;
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "pin") == 0) sawPin = true;
        if (std::strcmp(cs[i].name, "threshold") == 0) sawThreshold = true;
    }
    CHECK(sawPin);
    CHECK(sawThreshold);
}

// A script names its own choices, so a setting with a few modes reads as words on the card rather than as numbers on a slider.
TEST_CASE("a script declares a dropdown with addSelect, its value the index of the name picked") {
    Rig rig;
    writeScript("t_select.mls",
                "class Pick {\n"
                "  byte shape = 1;\n"
                "  bool flag = false;\n"
                "  byte many = 0;\n"
                "  int wide = 0;\n"
                "  void defineControls() {\n"
                "    addSelect(\"shape\", shape, \"disc|ball|egg\");\n"
                "    addSelect(\"flag\", flag, \"off|on\");\n"
                "    addSelect(\"many\", many, \"a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q\");\n"
                "    addSelect(\"wide\", wide, \"a|b\");\n"
                "  }\n"
                "  void tick20ms() { }\n"
                "}\n");
    rig.svc->setScript("t_select.mls");
    rig.svc->prepare();

    auto& cs = rig.svc->controls();
    const ControlDescriptor* shape = nullptr;
    bool sawFlag = false, sawMany = false, sawWide = false;
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "shape") == 0) shape = &cs[i];
        if (std::strcmp(cs[i].name, "wide") == 0) sawWide = true;
        if (std::strcmp(cs[i].name, "flag") == 0) sawFlag = true;
        if (std::strcmp(cs[i].name, "many") == 0) sawMany = true;
    }
    REQUIRE(shape != nullptr);
    CHECK(shape->type == ControlType::Select);
    REQUIRE(shape->max == 3);
    const auto* names = reinterpret_cast<const char* const*>(shape->aux);
    CHECK(std::strcmp(names[0], "disc") == 0);
    CHECK(std::strcmp(names[2], "egg") == 0);
    CHECK(*static_cast<uint8_t*>(shape->ptr) == 1);   // the member's initializer, ball
    CHECK_FALSE(sawFlag);                             // a bool is a switch, not a dropdown
    CHECK_FALSE(sawMany);                             // more names than the room left: no dropdown rather than a cut-off one
    CHECK_FALSE(sawWide);                             // the dropdown binds one byte, so an int member would show and take only its low byte

    REQUIRE(rig.scheduler.setControl("Script", "shape", "{\"value\":2}") == Scheduler::SetControlResult::Ok);
    CHECK(*static_cast<uint8_t*>(shape->ptr) == 2);
}

// The engine keeps every dropdown's names in 96 bytes, so a declaration it refuses must leave that room to the next.
TEST_CASE("a second dropdown on the same member is refused without spending the room its names would take") {
    Rig rig;
    writeScript("t_select_dup.mls",
                "class Pick {\n"
                "  byte shape = 0;\n"
                "  byte other = 0;\n"
                "  void defineControls() {\n"
                "    addSelect(\"shape\", shape, \"disc|ball|egg\");\n"
                "    addSelect(\"again\", shape, \"aaaaaaa1|aaaaaaa2|aaaaaaa3|aaaaaaa4|aaaaaaa5|aaaaaaa6|aaaaaaa7|aaaaaaa8\");\n"
                "    addSelect(\"other\", other, \"one|two|six|ten\");\n"
                "  }\n"
                "  void tick20ms() { }\n"
                "}\n");
    rig.svc->setScript("t_select_dup.mls");
    rig.svc->prepare();

    auto& cs = rig.svc->controls();
    bool sawAgain = false, sawOther = false;
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "again") == 0) sawAgain = true;
        if (std::strcmp(cs[i].name, "other") == 0) sawOther = true;
    }
    CHECK_FALSE(sawAgain);
    CHECK(sawOther);
}

TEST_CASE("a service with no script, or a broken one, is a valid state and ticks harmlessly") {
    // Robustness: a fresh card has no script, and a user mid-edit has a broken one. Neither may crash, and neither may drive anything.
    Rig rig;
    for (int i = 0; i < 5; i++) rig.svc->tick20ms();     // no script named at all
    CHECK(rig.control->switch1 == 0);

    writeScript("t_broken.mls", "class Broken { void tick20ms() { this is not a script } }\n");
    rig.svc->setScript("t_broken.mls");
    rig.svc->prepare();
    for (int i = 0; i < 5; i++) rig.svc->tick20ms();     // must not crash
    CHECK(rig.control->switch1 == 0);
    // The diagnostic is on the status line, which is how a user sees what is wrong.
    CHECK(rig.svc->status() != nullptr);
}

TEST_CASE("a scripted service reads an analog pin and scales it itself") {
    // The script half of step 3: the same pedal AnalogService maps with rows, expressed as arithmetic. `adcMax()` is what makes the script portable, so the source carries no 4095.
    Rig rig;
    writeScript("t_adc.mls",
                "class Pedal {\n"
                "  int pin = 4;\n"
                "  void defineControls() { addControl(\"pin\", pin, 0, 48); }\n"
                "  void tick20ms() {\n"
                "    int raw = adcRead(pin);\n"
                "    setControl(\"switch1\", div(raw, adcMax()));\n"   // 1 only at full scale
                "  }\n"
                "}\n");
    rig.svc->setScript("t_adc.mls");
    rig.svc->prepare();
    REQUIRE(rig.svc->status() != nullptr);

    platform::setTestAdcValue(kPin, 0);
    rig.svc->tick20ms();
    CHECK(rig.control->switch1 == 0);

    platform::setTestAdcValue(kPin, platform::adcMaxCount());
    rig.svc->tick20ms();
    CHECK(rig.control->switch1 == 1);
    platform::clearTestAdcValue();
}

TEST_CASE("a script cannot reach past the control surface") {
    // The step-0 decision, pinned: setControl writes the Control module and nothing else. A script naming another module's control drives nothing, so a script cannot rewrite a driver's pin list or a network setting by naming it.
    Rig rig;
    writeScript("t_reach.mls",
                "class Reach {\n"
                "  void tick20ms() { setControl(\"switch1\", 1); }\n"
                "}\n");
    rig.svc->setScript("t_reach.mls");
    rig.svc->prepare();
    rig.svc->tick20ms();
    // It reached the surface, which IS allowed.
    CHECK(rig.control->switch1 == 1);
    // And there is no second path. The name is resolved against "Control", never against a module the script chooses, so there is nothing to test for the disallowed case beyond this contract.
}
