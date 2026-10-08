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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <fstream>
#include <iterator>

namespace {
/// A fresh filesystem root under /tmp, set as the root for one test, given back to the default and removed when the test ends however it ends.
struct TempRoot {
    char path[256];   ///< the folder, `/tmp/mm_<tag>_<millis>`
    /// Make the folder, `/.config` in it when `withConfig`, and point the filesystem at it.
    explicit TempRoot(const char* tag, bool withConfig = false) {
        std::snprintf(path, sizeof(path), "/tmp/mm_%s_%u", tag, static_cast<unsigned>(mm::platform::millis()));
        std::filesystem::remove_all(path);
        if (withConfig) std::filesystem::create_directories(std::string(path) + "/.config");
        mm::platform::fsSetRoot(path);
    }
    /// Point the filesystem back at its default and remove the folder.
    ~TempRoot() {
        mm::platform::fsSetRoot("");
        std::filesystem::remove_all(path);
    }
    /// Non-copyable: one test owns one folder.
    TempRoot(const TempRoot&) = delete;
    /// Non-assignable, for the same reason.
    TempRoot& operator=(const TempRoot&) = delete;
};
}  // namespace

// The settings directory is created when the filesystem mounts, not left for the first save to discover. A shipped binary starts in a directory that has never held one, and before this the first WRITE was what failed, then every write after it, once per save, forever.
TEST_CASE("A settings directory that does not exist yet is created when the filesystem mounts") {
    char root[256];
    std::snprintf(root, sizeof(root), "/tmp/mm_root_new_%u",
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
    std::snprintf(root, sizeof(root), "/tmp/mm_root_probe_%u",
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
    std::snprintf(want, sizeof(want), "/tmp/mm_root_env_%u",
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
    std::snprintf(root, sizeof(root), "/tmp/mm_root_blocked_%u",
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

// Persistence round-trip: set deviceName → save → recreate Scheduler+modules → load → assert.
// Uses fsSetRoot to isolate the test from any real /.config/ on disk.
// A control change (deviceName) saved with flush() reappears on the next boot once a fresh Scheduler loads the same path.
TEST_CASE("FilesystemModule round-trip") {
    TempRoot temp("persist_test");
    const char* tmpRoot = temp.path;

    // Scheduler::release() deletes its modules, they must be heap-allocated.
    // --- First run: set deviceName, save ---
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
// After scheduler.setup() runs the persistence load, the wired child must survive.
// A code-wired child (markWiredByCode) survives a load from older JSON that doesn't mention it, new firmware additions aren't trimmed for existing users.
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
// That member is skipped and the next save re-writes the file with the actual tree shape.
// When the saved document wants a different type under a code-wired child's name, the load keeps the wired child.
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

// Round-trip persistence with children.
// Write a Layer subtree that contains both controls and child modules with controls of their own, then read the file back as text and verify it parses as valid JSON.
// Regresses a missing comma between a child's "type" field and that child's first member (e.g.
// "type":"X""$patch":"replace" instead of "type":"X","$patch":"replace").
// Saving a Layer with multiple children produces valid JSON, comma separators between a child's `type` and its first member are present.
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
    // We verify it indirectly: mark Layer dirty + flush, and observe that the Layer.json file appears on disk.
    // If the singleton was lost, flush would be a no-op (flushPending() returns early when instance_ is null) and the file would not exist.
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

    // --- Load: fresh module (mode defaults to 0, param bound to paramA=10). The reload must end with
    //     mode=1 AND paramB=3, NOT paramB=10 (the bug: param landed on paramA, then the mode-1 rebuild
    //     showed paramB at its default). ---
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

// Regression: an UNKNOWN member BEFORE a boot-wired child must not spawn a DUPLICATE of the wired child.
// The unknown member is skipped, and the member naming the wired child applies to the existing instance.
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
// A document lists children in order, so a saved [B, A] loads as [B, A].
// Two user effects, no code-wired children, saved in a non-boot order.
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
// The renamed member must drop WITHOUT taking the user module after it, or the user's real driver vanishes on every reboot.
// This is the dominant cause on shiffy (distinct from the code-wired-reorder case above), pinned here.
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
