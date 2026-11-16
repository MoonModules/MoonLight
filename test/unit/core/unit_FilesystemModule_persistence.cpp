/// @module FilesystemModule
/// @also Scheduler, Layer

#include "doctest.h"
#include "core/system/FilesystemModule.h"
#include "core/util/ModuleFactory.h"
#include "core/module/Scheduler.h"
#include "core/module/StateDocument.h"
#include "core/system/SystemModule.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/modifiers/MultiplyModifier.h"
#include "light/modifiers/RegionModifier.h"
#include "light/layers/Layer.h"
#include "light/drivers/FixtureProfilesModule.h"
#include "platform/platform.h"
#include "filesystem_fixture.h"
#include "unit/system_temp_dir.h"   // mm::systemTempDir: the system temp folder, which Android and Windows place elsewhere than /tmp

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <fstream>
#include <iterator>

namespace {
/// A FilesystemModule and a Layer named "Layer" on `scheduler`, set up: the boot a saved Layer.json is read into.
void bootLayer(mm::Scheduler& scheduler, mm::FilesystemModule*& fs, mm::Layer*& layer) {
    fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);
    layer = new mm::Layer();
    layer->setTypeName("Layer");
    layer->setName("Layer");
    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();
}

}  // namespace

// The settings directory is created when the filesystem mounts, not left for the first save to discover. A shipped binary starts in a directory that has never held one, and before this the first WRITE was what failed, then every write after it, once per save, forever.
TEST_CASE("A settings directory that does not exist yet is created when the filesystem mounts") {
    char root[256];
    std::snprintf(root, sizeof(root), "%s/mm_root_new_%u", mm::systemTempDir().c_str(),
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(root);
    REQUIRE_FALSE(std::filesystem::exists(root));

    mm::platform::fsSetRoot(root);
    CHECK(mm::platform::fsMount());
    CHECK(std::filesystem::is_directory(root));
    // Reported back, so a failure can name the directory it actually tried.
    CHECK(std::string(mm::platform::fsRootPath()) == root);

    mm::platform::fsSetRoot("");
    std::filesystem::remove_all(root);
}

// A root that exists and is a directory can still reject writes, which is the case the probe exists for and the one a read-only extraction produces. A DIRECTORY where the probe file belongs blocks its creation while leaving the root itself perfectly valid, so this reaches the probe instead of failing earlier at the is_directory check the case above covers.
TEST_CASE("A settings directory that rejects a write fails the mount, not just a missing one") {
    char root[256];
    std::snprintf(root, sizeof(root), "%s/mm_root_probe_%u", mm::systemTempDir().c_str(),
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(root);
    std::error_code ec;
    const auto blocker = std::filesystem::path(root) / ".mm-write-probe";
    std::filesystem::create_directories(blocker, ec);
    // Non-empty, so the pre-remove cannot clear it out of the way either.
    { std::ofstream f(blocker / "occupied.txt"); f << "x"; }
    REQUIRE(std::filesystem::is_directory(root));
    REQUIRE(std::filesystem::is_directory(blocker));

    mm::platform::fsSetRoot(root);
    CHECK_FALSE(mm::platform::fsMount());

    mm::platform::fsSetRoot("");
    std::filesystem::remove_all(root);
}

// MM_DATA_DIR wins over every other rule. This is the contract the test suite itself relies on: ctest sets it so a test can never write into the developer's real settings directory.
TEST_CASE("MM_DATA_DIR chooses the settings directory over any other rule") {
    const char* prior = std::getenv("MM_DATA_DIR");
    const std::string saved = prior ? prior : "";

    char want[256];
    std::snprintf(want, sizeof(want), "%s/mm_root_env_%u", mm::systemTempDir().c_str(),
                  static_cast<unsigned>(mm::platform::millis()));
#ifdef _WIN32
    _putenv_s("MM_DATA_DIR", want);
#else
    setenv("MM_DATA_DIR", want, 1);
#endif
    // Empty restores the DEFAULT, which is where the resolution rule is applied.
    mm::platform::fsSetRoot("");
    CHECK(std::string(mm::platform::fsRootPath()) == want);

    // Restore what ctest pinned. Leaving this set would hand every later case a root of its own choosing, quietly undoing the isolation this very case exists to prove.
#ifdef _WIN32
    _putenv_s("MM_DATA_DIR", saved.c_str());
#else
    if (saved.empty()) unsetenv("MM_DATA_DIR");
    else                setenv("MM_DATA_DIR", saved.c_str(), 1);
#endif
    mm::platform::fsSetRoot("");
    // Not an equality check against `saved`: with the variable unset the root correctly falls through to the next rule, so the thing worth asserting is that the override is gone.
    CHECK(std::string(mm::platform::fsRootPath()) != want);
    std::filesystem::remove_all(want);
}

// An unusable location is refused at mount, which is what lets the caller say "persistence disabled" once instead of emitting a failed save per module per change. A plain file where the directory belongs stands in for the real cases (a read-only extraction, a protected folder, a directory owned by someone else): every one of them accepts the path and rejects the writes.
TEST_CASE("A settings location that cannot be written to fails the mount, not every later save") {
    char root[256];
    std::snprintf(root, sizeof(root), "%s/mm_root_blocked_%u", mm::systemTempDir().c_str(),
                  static_cast<unsigned>(mm::platform::millis()));
    std::filesystem::remove_all(root);
    // ofstream does not create parents, and on Windows "/tmp" is <drive>:\tmp, which need not exist. Without this the case depends on another test having created it first.
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(root).parent_path(), ec);
    { std::ofstream f(root); f << "a file, not a directory"; }
    REQUIRE(std::filesystem::is_regular_file(root));

    mm::platform::fsSetRoot(root);
    CHECK_FALSE(mm::platform::fsMount());

    mm::platform::fsSetRoot("");
    std::filesystem::remove_all(root);
}

// A module listing its scripts while binding its controls, as Drivers does so a saved palette resolves at load, needs the files: storage mounts when persistence attaches, before the first phase.
TEST_CASE("the storage mounts when persistence attaches, before any module binds its controls") {
    TempRoot temp("mount_early");
    const std::string config = std::string(temp.path) + mm::FilesystemModule::CONFIG_DIR;
    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    CHECK_FALSE(std::filesystem::exists(config));
    fs->setScheduler(&scheduler);
    CHECK(std::filesystem::is_directory(config));   // mounted, before the scheduler's first phase
    scheduler.addModule(fs);
    scheduler.setup();
    scheduler.release();
}

// Persistence round-trip: set deviceName → save → recreate Scheduler+modules → load → assert.
// Uses fsSetRoot to isolate the test from any real /.config/ on disk. A control change (deviceName) saved with flush() reappears on the next boot once a fresh Scheduler loads the same path.
TEST_CASE("FilesystemModule round-trip") {
    TempRoot temp("persist_test");
    const char* tmpRoot = temp.path;

    // Scheduler::release() deletes its modules, so they are heap-allocated. First run: set deviceName, save.
    {
        mm::Scheduler scheduler;
        auto* fs = new mm::FilesystemModule();
        auto* sys = new mm::SystemModule();
        sys->setTypeName("SystemModule");
        sys->setName("System");
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        sys->setScheduler(&scheduler);
        scheduler.addModule(fs);
        scheduler.addModule(sys);
        scheduler.setup();

        // setup() derived MAC-based default; mutate it as if the UI did:
        for (uint8_t i = 0; i < sys->controls().count(); i++) {
            auto& c = sys->controls()[i];
            if (std::strcmp(c.name, "deviceName") == 0) {
                std::snprintf(static_cast<char*>(c.ptr), 9, "%s", "MM-ROUND");
                sys->markDirty();
                mm::FilesystemModule::noteDirty();
                break;
            }
        }

        // flush() does the same work as tick1s() does once the debounce expires, but synchronously, used here to keep the test deterministic without wall-clock waits.
        fs->flush();

        char path[512];
        std::snprintf(path, sizeof(path), "%s/.config/SystemModule.json", tmpRoot);
        CHECK(std::filesystem::exists(path));

        scheduler.release();
    }

    // --- Second run: fresh modules, load from disk ---
    {
        mm::Scheduler scheduler;
        auto* fs = new mm::FilesystemModule();
        auto* sys = new mm::SystemModule();
        sys->setTypeName("SystemModule");
        sys->setName("System");
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        sys->setScheduler(&scheduler);
        scheduler.addModule(fs);
        scheduler.addModule(sys);
        scheduler.setup();

        CHECK(std::strcmp(sys->deviceName(), "MM-ROUND") == 0);

        scheduler.release();
    }

    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot("."); // restore default
}

// Structural persistence: hand-write a Layer.json describing a different tree shape than the one main.cpp builds, then load and verify the live tree matches the document: a child it names is created, children it leaves out are removed.
// A document lists a module's children exactly (`$patch` replace), keyed by name.
TEST_CASE("FilesystemModule structural load makes the live tree match the document") {
    TempRoot temp("struct_test", /*withConfig=*/true);
    const char* tmpRoot = temp.path;

    // ModuleFactory must know the types before reconciliation can construct them.
    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");
    mm::ModuleFactory::registerType<mm::MultiplyModifier>("MultiplyModifier");

    // Write a Layer.json that asks for one child (RainbowEffect at position 0).
    {
        std::ofstream f(std::string(tmpRoot) + "/.config/Layer.json");
        f << "{\"Layer\":{\"$patch\":\"replace\",\"channelsPerLight\":3,\"enabled\":true,"
             "\"Rainbow\":{\"type\":\"RainbowEffect\",\"enabled\":true}}}";
    }

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);

    // Build a live tree: Layer with Noise and Multiply. The document lists only Rainbow, so we expect both removed and Rainbow created.
    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    layer->setName("Layer");
    auto* noise = new mm::NoiseEffect();
    noise->setTypeName("NoiseEffect");
    noise->setName("Noise");
    auto* mirror = new mm::MultiplyModifier();
    mirror->setTypeName("MultiplyModifier");
    mirror->setName("Multiply");
    layer->addChild(noise);
    layer->addChild(mirror);

    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    REQUIRE(layer->childCount() == 1);
    CHECK(std::strcmp(layer->child(0)->typeName(), "RainbowEffect") == 0);

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// Pins the wiredByCode-preserves-child contract that lets a new firmware revision add a code-created child (e.g.
// ImprovProvisioning under NetworkModule) without the child getting trimmed on every boot for users whose saved Network.json predates the addition.
//
// Setup: an on-disk file describes Layer with zero children.
// Live tree has Layer with a RainbowEffect child that main.cpp would have wired and marked.
// After scheduler.setup() runs the persistence load, the wired child must survive. A code-wired child (markWiredByCode) survives a load from older JSON that doesn't mention it, new firmware additions aren't trimmed for existing users.
TEST_CASE("FilesystemModule preserves code-wired children when JSON predates them") {
    TempRoot temp("wired_test", /*withConfig=*/true);
    const char* tmpRoot = temp.path;

    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");

    // Saved file: Layer with no children, the "old release" state.
    {
        std::ofstream f(std::string(tmpRoot) + "/.config/Layer.json");
        f << "{\"Layer\":{\"$patch\":\"replace\",\"channelsPerLight\":3,\"enabled\":true}}";
    }

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);

    // Live tree: Layer with a code-wired RainbowEffect child. This mirrors what main.cpp does for NetworkModule + ImprovProvisioningModule.
    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    layer->setName("Layer");
    auto* rainbow = new mm::RainbowEffect();
    rainbow->setTypeName("RainbowEffect");
    rainbow->setName("Rainbow");
    layer->addChild(rainbow);
    rainbow->markWiredByCode();

    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    // The code-wired RainbowEffect must still be there after persistence load.
    REQUIRE(layer->childCount() == 1);
    CHECK(std::strcmp(layer->child(0)->typeName(), "RainbowEffect") == 0);
    CHECK(layer->child(0)->isWiredByCode() == true);

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// Companion to the wiredByCode case above: when the document names a different type under the name of a code-wired child, the replacement must NOT kill the code-wired child.
// That member is skipped and the next save re-writes the file with the actual tree shape. When the saved document wants a different type under a code-wired child's name, the load keeps the wired child.
TEST_CASE("FilesystemModule does not replace code-wired child on type mismatch") {
    TempRoot temp("wired_replace_test", /*withConfig=*/true);
    const char* tmpRoot = temp.path;

    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");

    // Saved file: a NoiseEffect under the name the firmware now wires a RainbowEffect to, a stale shape from before the firmware moved a code-wired effect there.
    {
        std::ofstream f(std::string(tmpRoot) + "/.config/Layer.json");
        f << "{\"Layer\":{\"$patch\":\"replace\",\"channelsPerLight\":3,\"enabled\":true,"
             "\"Rainbow\":{\"type\":\"NoiseEffect\",\"enabled\":true}}}";
    }

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);

    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    layer->setName("Layer");
    auto* rainbow = new mm::RainbowEffect();
    rainbow->setTypeName("RainbowEffect");
    rainbow->setName("Rainbow");
    layer->addChild(rainbow);
    rainbow->markWiredByCode();

    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    // Code-wired child stays, the type mismatch did not trigger a replacement.
    REQUIRE(layer->childCount() == 1);
    CHECK(std::strcmp(layer->child(0)->typeName(), "RainbowEffect") == 0);
    CHECK(layer->child(0)->isWiredByCode() == true);

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// The board leaves migration to MIGRATING: a module it cannot place is named with where to look, and the entry says per release whether a backup is worth taking; the board itself prescribes no remedy.
TEST_CASE("the first module skipped at boot is named with its reason and a pointer to MIGRATING") {
    TempRoot temp("skipped_at_boot_test", /*withConfig=*/true);

    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");

    // Saved file: a child of a type this build does not carry, as a release that renamed it leaves behind.
    std::ofstream(std::string(temp.path) + "/.config/Layer.json") << "{\"Layer\":{\"$patch\":\"replace\",\"enabled\":true,\"Old\":{\"type\":\"NoSuchEffectXyz\",\"enabled\":true},\"Noise\":{\"type\":\"NoiseEffect\",\"enabled\":true}}}";

    mm::Scheduler scheduler;
    mm::FilesystemModule* fs = nullptr;
    mm::Layer* layer = nullptr;
    bootLayer(scheduler, fs, layer);

    REQUIRE(layer->childCount() == 1);   // the rest of the file applied
    REQUIRE(fs->skippedAtBoot() != nullptr);
    CHECK(std::strcmp(fs->skippedAtBoot(), "Layer.Old skipped at boot: unknown type; MIGRATING says what to do, before anything is saved") == 0);

    scheduler.release();
}

// Round-trip persistence with children.
// Write a Layer subtree that contains both controls and child modules with controls of their own, then read the file back as text and verify it parses as valid JSON.
// Regresses a missing comma between a child's "type" field and that child's first member (e.g.
// "type":"X""$patch":"replace" instead of "type":"X","$patch":"replace"). Saving a Layer with multiple children produces valid JSON, comma separators between a child's `type` and its first member are present.
TEST_CASE("FilesystemModule writes valid JSON with children") {
    TempRoot temp("write_test", /*withConfig=*/true);
    const char* tmpRoot = temp.path;

    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");
    mm::ModuleFactory::registerType<mm::MultiplyModifier>("MultiplyModifier");

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);
    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    layer->setName("Layer");
    auto* mirror = new mm::MultiplyModifier();
    mirror->setTypeName("MultiplyModifier");
    mirror->setName("Multiply");
    auto* noise = new mm::NoiseEffect();
    noise->setTypeName("NoiseEffect");
    noise->setName("Noise");
    layer->addChild(mirror);
    layer->addChild(noise);

    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    // Mark dirty and flush so the file appears immediately.
    layer->markDirty();
    fs->flush();

    // Read back the raw file and verify both child "type" fields are followed by a comma before the next field.
    std::ifstream f(std::string(tmpRoot) + "/.config/Layer.json");
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();  // Windows holds an exclusive lock on open files — close before remove_all.
    CHECK(content.find("\"MultiplyModifier\",") != std::string::npos);
    CHECK(content.find("\"NoiseEffect\",") != std::string::npos);
    // The file is one document rooted at the module's name.
    CHECK(content.rfind("{\"Layer\":{", 0) == 0);
    // And the catastrophic "}{ or "X""Y syntactic shape must not appear.
    CHECK(content.find("\"\"") == std::string::npos);

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// No size cap: a config LARGER than the old fixed 2 KB save buffer round-trips in full.
// The save serializes into a growable JsonSink and the load reads a file-sized heap buffer, so neither side truncates.
// Built from a FixtureProfilesModule with many custom profiles, its persisted array of role wirings comfortably exceeds 2048 bytes, which the old fixed buffer would have silently dropped (returning false → nothing written → config lost on reboot).
TEST_CASE("FilesystemModule round-trips a config larger than the old 2 KB cap") {
    TempRoot temp("bigcfg_test", /*withConfig=*/true);
    const char* tmpRoot = temp.path;
    mm::ModuleFactory::registerType<mm::FixtureProfilesModule>("FixtureProfilesModule");

    uint32_t markerId = 0;
    {
        mm::Scheduler scheduler;
        auto* fs = new mm::FilesystemModule();
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        auto* lp = new mm::FixtureProfilesModule();
        lp->setTypeName("FixtureProfilesModule");
        lp->setName("FixtureProfiles");
        scheduler.addModule(fs);
        scheduler.addModule(lp);
        scheduler.setup();   // seeds the built-ins

        // 15 wide (24-channel) custom profiles serialize far past the old 2 KB cap, and fit beside the built-ins under kMaxProfiles.
        for (int k = 0; k < 15; k++) {
            uint32_t id = 0;
            REQUIRE(lp->addListRow(id));
            lp->setListRowField(id, "channels", "{\"value\":24}");
            if (k == 0) {
                markerId = id;
                lp->setListRowField(id, "name", "{\"value\":\"MARKER\"}");
                lp->setListRowField(id, "ch5", "{\"value\":5}");   // Pan at ch5 — a distinctive pick
            }
        }
        lp->markDirty();
        mm::FilesystemModule::noteDirty();
        fs->flush();

        // The saved file must exist AND be larger than the old 2048 cap (proving the cap is gone).
        const std::string path = std::string(tmpRoot) + "/.config/FixtureProfilesModule.json";
        REQUIRE(std::filesystem::exists(path));
        CHECK(std::filesystem::file_size(path) > 2048u);
        scheduler.release();
    }
    {
        // Fresh boot: load the big file (file-sized heap read) and confirm the wiring survived.
        mm::Scheduler scheduler;
        auto* fs = new mm::FilesystemModule();
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        auto* lp = new mm::FixtureProfilesModule();
        lp->setTypeName("FixtureProfilesModule");
        lp->setName("FixtureProfiles");
        scheduler.addModule(fs);
        scheduler.addModule(lp);
        scheduler.setup();

        CHECK(lp->listRowCount() == 35);   // 20 built-ins + 15 custom, all restored
        mm::Correction c;
        REQUIRE(lp->deriveCorrection(markerId, 255, c));   // the marker profile resolves after reload
        CHECK(c.outChannels == 24);                        // its 24-channel width survived
        scheduler.release();
    }

    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// Singleton survives probe lifecycle: /api/types factory-creates a probe of every registered type (including FilesystemModule) to capture defaults, then deletes it.
// The probe's destructor must NOT clear the singleton, otherwise every save path (noteDirty, debounced tick1s, flushPending on reboot) silently no-ops for the rest of the device's life.
// The fix is to register the singleton in setScheduler(), not in the constructor.
// This test catches that singleton-clear regression. /api/types factory-creates a temporary FilesystemModule probe; its destruction must NOT clear the static singleton (otherwise every later save silently no-ops).
TEST_CASE("FilesystemModule singleton survives probe construct+destruct") {
    TempRoot temp("singleton_test", /*withConfig=*/true);
    const char* tmpRoot = temp.path;

    mm::ModuleFactory::registerType<mm::FilesystemModule>("FilesystemModule");
    mm::ModuleFactory::registerType<mm::Layer>("Layer");
    mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");

    mm::Scheduler scheduler;

    // 1) Real FS instance, registered via setScheduler (this is the singleton-binding path).
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);

    auto* layer = new mm::Layer();
    layer->setTypeName("Layer");
    layer->setName("Layer");
    scheduler.addModule(fs);
    scheduler.addModule(layer);
    scheduler.setup();

    // 2) Mimic /api/types: factory-construct a probe FilesystemModule, then delete it. Before the fix, the probe's destructor cleared the static singleton because `instance_ == this` for the probe at destruction time.
    {
        auto* probe = mm::ModuleFactory::create("FilesystemModule");
        REQUIRE(probe != nullptr);
        delete probe;
    }

    // 3) After the probe died, noteDirty() must still reach the real singleton.
    // We verify it indirectly: mark Layer dirty + flush, and observe that the Layer.json file appears on disk. If the singleton was lost, flush would be a no-op (flushPending() returns early when instance_ is null) and the file would not exist.
    layer->markDirty();
    mm::FilesystemModule::flushPending();

    std::ifstream f(std::string(tmpRoot) + "/.config/Layer.json");
    CHECK(f.is_open());
    f.close();  // Windows holds an exclusive lock on open files — close before remove_all.

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// Regression.
// Int16 controls (GridLayout's width/height/depth, Layer's start/end) round-tripped through the filesystem load path were clamped to c.min/c.max, which default to 0,0 because ControlDescriptor.min/max are uint8_t and can't represent an int16 range.
// Every Int16 control loaded as 0, so a 128×128 grid became 0×0×0 after restart and the whole pipeline allocated no buffers.
// Int16 controls (GridLayout width/height, RegionModifier start/end) preserve their saved value across load, no zero-clamping from uint8 min/max bounds.
TEST_CASE("FilesystemModule Int16 controls round-trip preserves the saved value") {
    TempRoot temp("int16_test", /*withConfig=*/true);
    const char* tmpRoot = temp.path;

    // Hand-write a RegionModifier.json with non-zero Int16 values (including negatives, which are legal on the wire) so the load path is exercised without needing a save-side step.
    std::ofstream out(std::string(tmpRoot) + "/.config/RegionModifier.json");
    out << "{\"Region\":{\"enabled\":true,\"startX\":42,\"startY\":-17,\"startZ\":0,"
        << "\"endX\":100,\"endY\":-100,\"endZ\":0}}";
    out.close();

    mm::Scheduler scheduler;
    auto* fs = new mm::FilesystemModule();
    fs->setTypeName("FilesystemModule");
    fs->setScheduler(&scheduler);
    auto* region = new mm::RegionModifier();
    region->setTypeName("RegionModifier");
    region->setName("Region");
    scheduler.addModule(fs);
    scheduler.addModule(region);
    scheduler.setup();

    CHECK(region->startX == 42);
    CHECK(region->startY == -17);
    CHECK(region->endX == 100);
    CHECK(region->endY == -100);

    scheduler.release();
    std::filesystem::remove_all(tmpRoot);
    mm::platform::fsSetRoot(".");
}

// Regression: a module whose CONTROL SET depends on one of its own control VALUES must restore its value-dependent controls across a reboot.
// The canonical case is ParallelLedDriver's `peripheral` Select, which swaps the bus backend and with it the backend-owned controls (clockPin, ring cluster).
// Without the fix, a single overlay writes the saved `clockPin` onto the DEFAULT backend's member, and the later swap to the saved peripheral discards it, clockPin (and the ring geometry) silently revert to their defaults on any reload that rebuilds the control set (a device reboot, or an INT_WDT restart). applyNode's overlay → rebuildControls → overlay-again fixes it: the second overlay lands on the now-correct backend member.
// This mock reproduces the structure minimally: `mode` picks which of two backing variables `param` binds to, so a naive single-overlay writes param to the wrong one.
