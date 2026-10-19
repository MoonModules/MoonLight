/// @module MoonModule

#include "doctest.h"
#include "core/module/MoonModule.h"
#include "core/util/JsonSink.h"

#include <cstring>   // std::strcpy, for the in-place option-rename schemaSignature tests
#include <string>    // std::string: named explicitly, GCC does not pull it in transitively

namespace {

class TestModule : public mm::MoonModule {
public:
    uint8_t brightness = 128;
    uint8_t speed = 60;
    bool enabled = true;

    bool setupCalled = false;
    bool releaseCalled = false;

    void setup() override { setupCalled = true; }
    void release() override { releaseCalled = true; }

    void defineControls() override {
        controls_.addControl("brightness", brightness, 0, 255);
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("enabled", enabled);
    }
};

// A module that hides from the UI (the FilesystemModule / HttpServerModule pattern): the state serializer skips any module whose appearsInUi() is false.
class HiddenModule : public mm::MoonModule {
public:
    bool appearsInUi() const override { return false; }
};

} // namespace

// setup() and release() each fire exactly when called and update their respective state flags.
TEST_CASE("MoonModule lifecycle") {
    TestModule mod;
    CHECK_FALSE(mod.setupCalled);
    mod.setup();
    CHECK(mod.setupCalled);
    mod.release();
    CHECK(mod.releaseCalled);
}

// name() starts empty; setName() copies the string into the internal 16-byte buffer.
TEST_CASE("MoonModule name") {
    TestModule mod;
    CHECK(mod.name()[0] == '\0');
    mod.setName("TestModule");
    CHECK(std::strcmp(mod.name(), "TestModule") == 0);
}

// typeName (set by the factory) is independent of name; setName doesn't touch typeName so a human-renamed module still serializes under its real type.
TEST_CASE("MoonModule typeName is independent of name") {
    TestModule mod;
    // Both empty by default
    CHECK(mod.name()[0] == '\0');
    CHECK(mod.typeName()[0] == '\0');

    // Factory sets typeName then name; later setName doesn't touch typeName
    mod.setTypeName("NoiseEffect");
    mod.setName("NoiseEffect");
    CHECK(std::strcmp(mod.typeName(), "NoiseEffect") == 0);

    mod.setName("Noise");   // human label override (matches main.cpp pattern)
    CHECK(std::strcmp(mod.name(), "Noise") == 0);
    CHECK(std::strcmp(mod.typeName(), "NoiseEffect") == 0);
}

// dirty()/markDirty()/clearDirty() round-trip cleanly (the bit FilesystemModule polls for save scheduling).
TEST_CASE("MoonModule dirty flag") {
    TestModule mod;
    CHECK_FALSE(mod.dirty());
    mod.markDirty();
    CHECK(mod.dirty());
    mod.clearDirty();
    CHECK_FALSE(mod.dirty());
}

// parent() starts null; setParent() records the upstream container for tree walks.
TEST_CASE("MoonModule parent") {
    TestModule parent;
    TestModule child;
    CHECK(child.parent() == nullptr);
    child.setParent(&parent);
    CHECK(child.parent() == &parent);
}

// Adding Uint8/Bool controls stores live pointers to the module's fields, so changes propagate either direction (field ↔ control->ptr).
TEST_CASE("Control binding via ControlList") {
    TestModule mod;
    mod.defineControls();

    CHECK(mod.controls().count() == 3);
    CHECK(std::strcmp(mod.controls()[0].name, "brightness") == 0);
    CHECK(mod.controls()[0].type == mm::ControlType::Uint8);
    CHECK(mod.controls()[0].ptr == &mod.brightness);

    // Verify binding: changing the variable is visible via the pointer
    mod.brightness = 200;
    CHECK(*static_cast<uint8_t*>(mod.controls()[0].ptr) == 200);

    // Verify binding: changing via pointer updates the variable
    *static_cast<uint8_t*>(mod.controls()[0].ptr) = 50;
    CHECK(mod.brightness == 50);
}

// controls().clear() empties the list; calling defineControls() again repopulates it (the standard rebuild path).
TEST_CASE("ControlList clear and rebuild") {
    TestModule mod;
    mod.defineControls();
    CHECK(mod.controls().count() == 3);

    mod.controls().clear();
    CHECK(mod.controls().count() == 0);

    mod.defineControls();
    CHECK(mod.controls().count() == 3);
}

// schemaSignature() drives the WS full-resync gate: it must change when the schema changes and hold steady on a value-only change. These pin the two subtle cases a naive node-only-pointer hash misses.
namespace {
// A module with a Select whose option STRINGS live in a member array that can be rewritten in place (the stable-address bind pattern DriverBase/HueDriver use for preset/room dropdowns).
class SelectModule : public mm::MoonModule {
public:
    uint8_t value = 5;
    uint8_t sel = 0;
    char opt0[16] = "alpha";
    char opt1[16] = "beta";
    const char* options[2] = {opt0, opt1};
    void defineControls() override {
        controls_.addControl("value", value, 0, 255);
        controls_.addSelect("preset", sel, options, 2);
    }
};
}  // namespace

TEST_CASE("schemaSignature: value change is invisible, schema change is not") {
    SelectModule m; m.defineControls();
    const uint32_t base = m.schemaSignature();
    // A bound-value change (the slider-drag path) must NOT move the signature, it rides the value patch.
    m.value = 200;
    CHECK(m.schemaSignature() == base);
    // An in-place option RENAME (same array pointer, same count, changed string) MUST move it, the signature hashes the option strings, not the array pointer (F2: the stable-address bind pattern).
    std::strcpy(m.opt0, "gamma");
    CHECK(m.schemaSignature() != base);
}

namespace {
// A module whose Progress total changes at runtime, the way a track change moves FseqPlayerEffect's frameCount_.
class ProgressModule : public mm::MoonModule {
public:
    uint32_t position = 0;
    uint32_t total = 100;
    void defineControls() override { controls_.addProgress("position", position, total); }
};
}  // namespace

// Pins the fix for the PR #148 follow-up: a Progress total is a value, not schema, so changing it and rebuilding must not trip a full browser resync.
TEST_CASE("schemaSignature: a Progress total change alone is invisible, same as its value") {
    ProgressModule m; m.defineControls();
    const uint32_t base = m.schemaSignature();
    m.total = 9999;
    m.rebuildControls();   // re-snapshots aux for the new total, the same call loadTrack() makes on a track change
    CHECK(m.schemaSignature() == base);
    CHECK(m.controls()[0].aux == 9999);   // the total did refresh; only the resync-trigger is suppressed
}

TEST_CASE("schemaSignature: recurses into children (a child schema change is caught)") {
    auto* parent = new mm::MoonModule();
    auto* child = new SelectModule();
    parent->addChild(child);
    child->defineControls();
    const uint32_t base = parent->schemaSignature();
    // The parent's own controls are unchanged; only the CHILD's Select is renamed. The parent's signature must still change (F1: rebuildControls rebuilds the subtree, so the signature recurses).
    std::strcpy(child->opt1, "delta");
    CHECK(parent->schemaSignature() != base);
    delete parent;   // deletes the child too (owns the subtree)
}

// addReadOnly binds a char buffer the UI can render; updating the buffer is visible through control.ptr.
TEST_CASE("ReadOnly control binding") {
    mm::MoonModule mod;
    char statusBuf[32] = "idle";
    mod.controls().addReadOnly("status", statusBuf, sizeof(statusBuf));

    CHECK(mod.controls().count() == 1);
    auto& ctrl = mod.controls()[0];
    CHECK(std::strcmp(ctrl.name, "status") == 0);
    CHECK(ctrl.type == mm::ControlType::ReadOnly);
    CHECK(std::strcmp(static_cast<char*>(ctrl.ptr), "idle") == 0);

    // Update the buffer, control reflects it
    std::strcpy(statusBuf, "running");
    CHECK(std::strcmp(static_cast<char*>(ctrl.ptr), "running") == 0);
}

// addSelect binds a uint8 + an options array (stored in aux), control.max carries the option count.
TEST_CASE("Select control binding") {
    mm::MoonModule mod;
    uint8_t mode = 0;
    static constexpr const char* options[] = {"Off", "Auto", "Manual"};
    mod.controls().addSelect("mode", mode, options, 3);

    CHECK(mod.controls().count() == 1);
    auto& ctrl = mod.controls()[0];
    CHECK(std::strcmp(ctrl.name, "mode") == 0);
    CHECK(ctrl.type == mm::ControlType::Select);
    CHECK(*static_cast<uint8_t*>(ctrl.ptr) == 0);
    CHECK(ctrl.max == 3); // option count

    // Options pointer is stored in aux
    auto* opts = reinterpret_cast<const char* const*>(ctrl.aux);
    CHECK(std::strcmp(opts[0], "Off") == 0);
    CHECK(std::strcmp(opts[1], "Auto") == 0);
    CHECK(std::strcmp(opts[2], "Manual") == 0);

    // Change via pointer
    mode = 2;
    CHECK(*static_cast<uint8_t*>(ctrl.ptr) == 2);
}

// addProgress binds a uint32 plus a "total" value (in aux), the UI renders value/total as a progress bar.
TEST_CASE("Progress control binding") {
    mm::MoonModule mod;
    uint32_t used = 1000;
    mod.controls().addProgress("heap", used, 4096);

    CHECK(mod.controls().count() == 1);
    auto& ctrl = mod.controls()[0];
    CHECK(std::strcmp(ctrl.name, "heap") == 0);
    CHECK(ctrl.type == mm::ControlType::Progress);
    CHECK(*static_cast<uint32_t*>(ctrl.ptr) == 1000);
    CHECK(ctrl.aux == 4096); // total

    // Update value
    used = 2048;
    CHECK(*static_cast<uint32_t*>(ctrl.ptr) == 2048);
}

// The UI sizes a gauge's label from the unit it declares, so a KiB gauge must say KiB or a 32 GB card reads as 32 MB.
TEST_CASE("a progress control declares its unit to the UI, bytes unless told otherwise") {
    mm::MoonModule mod;
    uint32_t used = 1, usedKiB = 1;
    mod.controls().addProgress("flash", used, 4096);
    mod.controls().addProgress("card", usedKiB, 31166976, mm::ProgressUnit::KiB);
    const auto meta = [&](uint8_t i) { mm::JsonSink sink; mm::writeControlMetadata(sink, mod.controls()[i]); return std::string(sink.data(), sink.size()); };
    CHECK(meta(0) == ",\"total\":4096,\"unit\":\"B\"");
    CHECK(meta(1) == ",\"total\":31166976,\"unit\":\"KiB\"");
}

// enabled defaults to true; setEnabled flips the universal enable gate (Scheduler and parent containers respect it).
TEST_CASE("Module enabled property") {
    mm::MoonModule mod;
    CHECK(mod.enabled() == true); // default enabled
    mod.setEnabled(false);
    CHECK(mod.enabled() == false);
    mod.setEnabled(true);
    CHECK(mod.enabled() == true);
}

// a bool addControl binds a bool field, toggling the field updates control.ptr's view.
TEST_CASE("Bool control binding") {
    TestModule mod;
    mod.defineControls();

    auto& ctrl = mod.controls()[2];
    CHECK(std::strcmp(ctrl.name, "enabled") == 0);
    CHECK(ctrl.type == mm::ControlType::Bool);
    CHECK(*static_cast<bool*>(ctrl.ptr) == true);

    mod.enabled = false;
    CHECK(*static_cast<bool*>(ctrl.ptr) == false);
}

// appearsInUi() defaults to true (every module shows); overridable to false so infrastructure (FilesystemModule, HttpServerModule) hides, the flag the state serializer reads to skip a module.
TEST_CASE("MoonModule appearsInUi defaults true, overridable false") {
    TestModule visible;
    CHECK(visible.appearsInUi());       // ordinary module: shown

    HiddenModule hidden;
    CHECK_FALSE(hidden.appearsInUi());  // infrastructure module: skipped by the serializer

    // Through the base-class pointer the virtual still dispatches to the override, the path the serializer actually takes (it holds MoonModule*).
    mm::MoonModule* asBase = &hidden;
    CHECK_FALSE(asBase->appearsInUi());
}

// readBool/readUint8, the shared generic control reader (reviewer #8): one implementation, so HttpServerModule and MqttModule can't disagree on Drivers.on's absent-control default.
TEST_CASE("MoonModule readBool/readUint8 return the value, or the default when absent") {
    TestModule mod;
    mod.defineControls();   // binds brightness(Uint8), speed(Uint8), enabled(Bool)

    // Present controls read their live value.
    CHECK(mod.readUint8("brightness", 99) == 128);   // TestModule brightness default
    CHECK(mod.readBool("enabled", false) == true);

    // The live field updates flow through (the reader dereferences the bound pointer).
    mod.brightness = 42;
    CHECK(mod.readUint8("brightness", 99) == 42);
    mod.enabled = false;
    CHECK(mod.readBool("enabled", true) == false);

    // Absent control → the caller's default, not a hard-coded one (the bug this shared reader fixes).
    CHECK(mod.readBool("on", true) == true);         // no "on" control → default true
    CHECK(mod.readBool("on", false) == false);       // ...and the OTHER default, from the same call
    CHECK(mod.readUint8("missing", 7) == 7);
}
