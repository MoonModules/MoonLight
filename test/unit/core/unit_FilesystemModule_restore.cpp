/// @module FilesystemModule
/// What a saved document restores into a running tree: a value-dependent control, user modules among reordered or unknown siblings, a control that exists only after prepare(), a secret kept out of a shared document, and the flat format older builds wrote.

#include "doctest.h"
#include "core/system/FilesystemModule.h"
#include "core/util/ModuleFactory.h"
#include "core/module/Scheduler.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/modifiers/MultiplyModifier.h"
#include "light/layers/Layer.h"
#include "platform/platform.h"
#include "filesystem_fixture.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <fstream>
#include <iterator>

namespace {
struct ModeDependentMock : public mm::MoonModule {
    uint8_t mode = 0;         // the "peripheral" analogue: selects the control set
    uint8_t paramA = 10;      // bound when mode==0 (the "default backend" member, default 10)
    uint8_t paramB = 10;      // bound when mode==1 (the "swapped backend" member, default 10)
    static constexpr const char* kModes[2] = {"A", "B"};
    void defineControls() override {
        controls_.addSelect("mode", mode, kModes, 2);
        // `param` binds to a DIFFERENT variable depending on mode, exactly like clockPin binding to whichever peripheral backend is live. A rebuild after `mode` changes re-binds it.
        controls_.addControl("param", mode == 0 ? paramA : paramB, 0, 255);
    }
};
}  // namespace

TEST_CASE("FilesystemModule restores a value-dependent control across reload (the peripheral/clockPin bug)") {
    TempRoot temp("persist_modedep");
    const char* tmpRoot = temp.path;
    mm::ModuleFactory::registerType<ModeDependentMock>("ModeDependentMock");

    // --- Save: set mode=1 (the non-default control set) AND param=3 on the mode-1 variable ---
    {
        mm::Scheduler scheduler;
        auto* fs = new mm::FilesystemModule();
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        auto* m = new ModeDependentMock();
        m->setTypeName("ModeDependentMock");
        m->setName("ModeMock");
        scheduler.addModule(fs);
        scheduler.addModule(m);
        scheduler.setup();

        m->mode = 1;
        m->rebuildControls();      // mode change re-binds `param` to paramB (as the UI swap would)
        m->paramB = 3;             // the value the user sets (the "clockPin=3" analogue)
        m->markDirty();
        mm::FilesystemModule::noteDirty();
        fs->flush();
        scheduler.release();
    }

    // Load into a fresh module, mode 0 and param bound to paramA=10: the reload must end with mode=1 and paramB=3, where the bug landed param on paramA and the mode-1 rebuild showed paramB at its default.
    {
        mm::Scheduler scheduler;
        auto* fs = new mm::FilesystemModule();
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        auto* m = new ModeDependentMock();
        m->setTypeName("ModeDependentMock");
        m->setName("ModeMock");
        scheduler.addModule(fs);
        scheduler.addModule(m);
        scheduler.setup();

        CHECK(m->mode == 1);       // the value-dependent selector restored
        CHECK(m->paramB == 3);     // and the control it selects restored to the SAVED value, not default 10
        scheduler.release();
    }

    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// Regression: a user-added module recorded AFTER two code-wired siblings must survive a load even when the code-wired siblings' boot order differs from the saved order.
// This is shiffy's "spontaneously lost ParallelLedDriver" bug: the Drivers container boot-wires FixtureProfiles then Preview, but the file was saved as Preview, FixtureProfiles, ParallelLed.
// A document keys children by name, so the wired modules are found wherever they sit, the user module is created, and the saved order is restored.
// Modeled with two code-wired effects (singletons per container, like Preview/FixtureProfiles) swapped vs. the file, plus a user effect after them.
TEST_CASE("FilesystemModule restores a user module recorded after reordered code-wired siblings") {
    TempRoot temp("wired_reorder", /*withConfig=*/true);
    const char* tmpRoot = temp.path;

    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");
    mm::ModuleFactory::registerType<mm::MultiplyModifier>("MultiplyModifier");

    // Saved file: child order Noise(0), Rainbow(1), Multiply(2), the two effects are the code-wired singletons, the modifier is the user-added module recorded AFTER them. The Rainbow carries a DISTINCTIVE saved `speed` (137, not its default 20) so the test can prove a reordered code-wired child's own persisted controls are restored, not just its presence.
    {
        std::ofstream f(std::string(tmpRoot) + "/.config/Layer.json");
        f << "{\"Layer\":{\"$patch\":\"replace\",\"channelsPerLight\":3,\"enabled\":true,"
             "\"Noise\":{\"type\":\"NoiseEffect\",\"enabled\":true},"
             "\"Rainbow\":{\"type\":\"RainbowEffect\",\"speed\":137,\"enabled\":true},"
             "\"Multiply\":{\"type\":\"MultiplyModifier\",\"enabled\":true}}}";
    }

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);

    // Live boot tree: the two code-wired effects in the OPPOSITE order from the file, Rainbow(0), Noise(1), and NO user modifier yet (it is what the file must restore).
    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    layer->setName("Layer");
    auto* rainbow = new mm::RainbowEffect(); rainbow->setTypeName("RainbowEffect"); rainbow->setName("Rainbow"); rainbow->markWiredByCode();
    auto* noise   = new mm::NoiseEffect();   noise->setTypeName("NoiseEffect");     noise->setName("Noise");     noise->markWiredByCode();
    layer->addChild(rainbow);     // live index 0 (file says index 1)
    layer->addChild(noise);       // live index 1 (file says index 0)

    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    // After load: the user MultiplyModifier is restored (the BUG dropped it), both code-wired effects are kept, and the children stand in the saved order.
    REQUIRE(layer->childCount() == 3);
    CHECK(std::strcmp(layer->child(0)->typeName(), "NoiseEffect") == 0);
    CHECK(std::strcmp(layer->child(1)->typeName(), "RainbowEffect") == 0);
    CHECK(std::strcmp(layer->child(2)->typeName(), "MultiplyModifier") == 0);
    CHECK(layer->child(0)->isWiredByCode() == true);
    CHECK(layer->child(1)->isWiredByCode() == true);
    CHECK(layer->child(2)->isWiredByCode() == false);
    // The reordered code-wired Rainbow's OWN persisted control is restored, not just its presence (saved speed 137, not default 20).
    CHECK(static_cast<mm::RainbowEffect*>(layer->child(1))->speed == 137);

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// Regression: an UNKNOWN member BEFORE a boot-wired child must not spawn a DUPLICATE of the wired child. The unknown member is skipped, and the member naming the wired child applies to the existing instance.
TEST_CASE("FilesystemModule: an unknown entry before a wired child does not duplicate the wired child") {
    TempRoot temp("wired_dup", /*withConfig=*/true);
    const char* tmpRoot = temp.path;

    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");

    // Saved file: an UNKNOWN type first (GoneEffect never registers), then RainbowEffect. The live tree has ONE boot-wired RainbowEffect.
    {
        std::ofstream f(std::string(tmpRoot) + "/.config/Layer.json");
        f << "{\"Layer\":{\"$patch\":\"replace\",\"channelsPerLight\":3,\"enabled\":true,"
             "\"Gone\":{\"type\":\"GoneEffect\",\"enabled\":true},"
             "\"Rainbow\":{\"type\":\"RainbowEffect\",\"speed\":91,\"enabled\":true}}}";
    }

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);
    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    layer->setName("Layer");
    auto* rainbow = new mm::RainbowEffect(); rainbow->setTypeName("RainbowEffect"); rainbow->setName("Rainbow"); rainbow->markWiredByCode();
    layer->addChild(rainbow);       // the ONE boot-wired Rainbow at live index 0
    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    // EXACTLY ONE Rainbow, the unknown GoneEffect drops, and the Rainbow member restores the wired child's controls without spawning a second instance.
    uint8_t rainbows = 0;
    for (uint8_t k = 0; k < layer->childCount(); k++)
        if (std::strcmp(layer->child(k)->typeName(), "RainbowEffect") == 0) rainbows++;
    CHECK(rainbows == 1);
    CHECK(layer->childCount() == 1);
    // And the wired child's saved control was applied (not left at default).
    REQUIRE(layer->childCount() >= 1);
    CHECK(static_cast<mm::RainbowEffect*>(layer->child(0))->speed == 91);

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// User-module reorder must round-trip: the drag-reorder UI (moveChildTo) permutes children, saves the new order, and on reboot the load must restore THAT order (user-module order is meaningful, render/composite order).
// A document lists children in order, so a saved [B, A] loads as [B, A]. Two user effects, no code-wired children, saved in a non-boot order.
TEST_CASE("FilesystemModule restores a user-module reorder in the saved order") {
    TempRoot temp("user_reorder", /*withConfig=*/true);
    const char* tmpRoot = temp.path;

    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");

    // Saved file: the user reordered to Noise(0), Rainbow(1), neither is code-wired.
    {
        std::ofstream f(std::string(tmpRoot) + "/.config/Layer.json");
        f << "{\"Layer\":{\"$patch\":\"replace\",\"channelsPerLight\":3,\"enabled\":true,"
             "\"Noise\":{\"type\":\"NoiseEffect\",\"enabled\":true},"
             "\"Rainbow\":{\"type\":\"RainbowEffect\",\"enabled\":true}}}";
    }

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);
    // Live boot tree: an empty Layer (user effects are not boot-wired, the load creates them).
    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    layer->setName("Layer");
    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    // The saved order is restored exactly.
    REQUIRE(layer->childCount() == 2);
    CHECK(std::strcmp(layer->child(0)->typeName(), "NoiseEffect") == 0);
    CHECK(std::strcmp(layer->child(1)->typeName(), "RainbowEffect") == 0);

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// The EXACT shiffy scenario: an UNKNOWN/renamed type mid-list (a pre-consolidation MoonI80Peripheral that no longer registers) followed by a real USER module.
// The renamed member must drop WITHOUT taking the user module after it, or the user's real driver vanishes on every reboot. This is the dominant cause on shiffy (distinct from the code-wired-reorder case above), pinned here.
TEST_CASE("FilesystemModule skips an unknown type mid-list and keeps the user module after it") {
    TempRoot temp("unknown_midlist", /*withConfig=*/true);
    const char* tmpRoot = temp.path;

    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");
    mm::ModuleFactory::registerType<mm::MultiplyModifier>("MultiplyModifier");
    // Deliberately do NOT register "GoneEffect", it stands in for a renamed/removed type (MoonI80Peripheral).

    // Saved file: Rainbow(0), GoneEffect(1, unregistered), Multiply(2, a real user module AFTER the dead one).
    {
        std::ofstream f(std::string(tmpRoot) + "/.config/Layer.json");
        f << "{\"Layer\":{\"$patch\":\"replace\",\"channelsPerLight\":3,\"enabled\":true,"
             "\"Rainbow\":{\"type\":\"RainbowEffect\",\"enabled\":true},"
             "\"Gone\":{\"type\":\"GoneEffect\",\"enabled\":true},"
             "\"Multiply\":{\"type\":\"MultiplyModifier\",\"enabled\":true}}}";
    }

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);
    auto* layer = new mm::Layer();     // empty; the load creates the (known) children
    layer->setTypeName("Layer");
    layer->setName("Layer");
    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    // The unknown GoneEffect is dropped; Rainbow AND the user's Multiply (recorded AFTER the dead entry) both survive, the whole point of the fix.
    REQUIRE(layer->childCount() == 2);
    CHECK(std::strcmp(layer->child(0)->typeName(), "RainbowEffect") == 0);
    CHECK(std::strcmp(layer->child(1)->typeName(), "MultiplyModifier") == 0);

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// A module whose CONTROL SET only exists after prepare() has done work.
// The MoonLive bindings, whose scripted controls (`cols`, `rows`, an effect's `speed`) are declared by the script and therefore appear only once it has COMPILED, which is prepare()'s job.
// Boot order is defineControls → load → prepareTree, so at load time those controls are in no list at all and their saved values have nowhere to land.
// A module that declares its controls at prepare has the file's unknown keys held back and set right after the prepare, which is the deferred table; prepare() seeds the script's own defaults first.
// Symptom on the bench: a scripted grid layout came back 16x16 however it had been set, while .config/Layouts.json held the right numbers all along.
namespace {
class LateSchemaModule : public mm::MoonModule {
public:
    uint8_t always = 1;
    uint8_t late = 16;        // stands in for a script's declared control
    bool prepared = false;

    void defineControls() override {
        mm::MoonModule::defineControls();
        controls_.addControl("always", always, 0, 255);
        // Only published once prepare() has run, exactly as publishDeclaredControls is empty until the engine holds a compiled program.
        if (prepared) controls_.addControl("late", late, 0, 255);
    }
    bool declaresControlsAtPrepare() const override { return true; }
    void prepare() override {
        prepared = true;
        rebuildControls();
    }
    /// Derived from the late control, read on demand: the shape MoonLiveLayout has, where lightCount() runs the script each call rather than caching anything at prepare time.
    uint16_t derived() const { return static_cast<uint16_t>(late) * 2; }
};
}  // namespace

TEST_CASE("FilesystemModule restores a control that only exists after prepare()") {
    TempRoot temp("lateschema");
    const char* tmpRoot = temp.path;
    std::filesystem::create_directories(std::string(tmpRoot) + "/.config");
    {
        std::ofstream f(std::string(tmpRoot) + "/.config/LateSchemaModule.json");
        f << "{\"Late\":{\"enabled\":true,\"always\":7,\"late\":42}}";
    }

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);

    auto* late = new LateSchemaModule();
    late->setTypeName("LateSchemaModule");
    late->setName("Late");
    scheduler.addModule(late);
    scheduler.addModule(fs);
    scheduler.setup();

    CHECK(late->always == 7);    // an ordinary control: the first load pass carried it
    CHECK(late->late == 42);     // and one that did not exist until prepare() ran
    // And the state DERIVED from it, not just the backing member. A value restored after phase 4 is still the one the pipeline reads, because derived state here is computed on demand.
    CHECK(late->derived() == 84);
}

namespace {
/// A module holding a secret, the shape a network module's WiFi password has.
struct SecretHolder : public mm::MoonModule {
    char secret[32] = "hunter2";
    uint8_t plain = 5;
    void defineControls() override {
        controls_.addPassword("secret", secret, sizeof(secret));
        controls_.addControl("plain", plain, 0, 255);
    }
};
}  // namespace

TEST_CASE("The device's own config file holds a secret, and a document written for sharing leaves it out") {
    // The config file is the device's own, so a reboot must bring the password back; a document is shown, copied and shared, so it carries none.
    TempRoot temp("secret");
    const char* root = temp.path;

    mm::Scheduler sched;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&sched);
    auto* m = new SecretHolder();
    m->setName("Holder");
    m->setTypeName("SecretHolder");
    sched.addModule(fs);
    sched.addModule(m);
    sched.setup();

    m->markDirty();
    mm::FilesystemModule::noteDirty();
    fs->flush();
    std::ifstream f(std::string(root) + "/.config/SecretHolder.json");
    const std::string saved((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    CHECK(saved.find("\"secret\"") != std::string::npos);
    CHECK(saved.find("\"plain\"") != std::string::npos);

    mm::JsonSink shared;
    mm::writeStateMember(shared, *m, /*withSecrets=*/false);
    const std::string doc(shared.data(), shared.size());
    CHECK(doc.find("\"secret\"") == std::string::npos);
    CHECK(doc.find("\"plain\"") != std::string::npos);

    sched.release();
    mm::platform::fsSetRoot("");
    std::filesystem::remove_all(root);
}

// Temporary, until the release after 2026-10-08: a device updated from a build that saved flat files keeps its config.
// The boot load reads the flat file as the document it describes, names a code-wired child as main.cpp named it, keeps a saved `$name`, and saves the result as a document.
TEST_CASE("A config file in the flat format older builds wrote loads at boot and is saved back as a document") {
    TempRoot temp("flat_convert_test", /*withConfig=*/true);
    const char* tmpRoot = temp.path;

    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");

    {
        std::ofstream f(std::string(tmpRoot) + "/.config/Layer.json");
        f << R"({"channelsPerLight":3,"enabled":true,"0.type":"RainbowEffect","0.enabled":false,)"
             R"("1.type":"NoiseEffect","1.$name":"Sky","1.scale":77,"1.enabled":true,)"
             R"("2.type":"NoiseEffect","2.scale":5,"2.enabled":true,"3.type":"NoiseEffect","3.scale":6,"3.enabled":true})";
    }

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);
    // main.cpp wires a Rainbow under a name of its own, which the flat file never recorded.
    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    layer->setName("Layer");
    auto* bow = new mm::RainbowEffect();
    bow->setTypeName("RainbowEffect");
    bow->setName("Bow");
    layer->addChild(bow);
    bow->markWiredByCode();
    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    REQUIRE(layer->childCount() == 4);
    CHECK(layer->child(0) == bow);                      // the wired child, found rather than created twice
    CHECK_FALSE(bow->enabled());                        // with the value the file held for it
    CHECK(std::strcmp(layer->child(1)->name(), "Sky") == 0);
    CHECK(static_cast<mm::NoiseEffect*>(layer->child(1))->scale == 77);
    CHECK(std::strcmp(layer->child(2)->name(), "Noise") == 0);
    CHECK(std::strcmp(layer->child(3)->name(), "Noise-2") == 0);   // siblings keep distinct names
    CHECK(static_cast<mm::NoiseEffect*>(layer->child(3))->scale == 6);

    fs->flush();   // the conversion marked it for saving
    std::ifstream f(std::string(tmpRoot) + "/.config/Layer.json");
    const std::string saved((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(saved.rfind("{\"Layer\":{", 0) == 0);
    CHECK(saved.find("\"Bow\":{\"type\":\"RainbowEffect\"") != std::string::npos);
    CHECK(saved.find("1.type") == std::string::npos);

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}
