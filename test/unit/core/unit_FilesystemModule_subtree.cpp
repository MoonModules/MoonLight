/// @module FilesystemModule
/// @also Scheduler, Layer, ControlModule

/// The save/apply seams a preset is built on: `saveSubtreeTo` writes a subtree into a caller's buffer rather than to /.config/<TypeName>.json, and `applySubtree` puts those bytes back onto a LIVE tree at runtime, rebuilding whatever shape they describe.
///
/// Boot already does both halves, but only as one fused operation at startup. What these pin is the runtime behavior a preset needs and boot never exercises: applying onto a tree that is already set up, with children that must be created, replaced or destroyed while the device runs.

#include "doctest.h"
#include "core/system/FilesystemModule.h"
#include "core/util/ModuleFactory.h"
#include "core/module/Scheduler.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/layers/Layer.h"
#include "light/layers/Effects.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

// A Effects tree the same way production builds one: through the factory, so every module carries a real typeName(). That matters here rather than being ceremony, reconciliation matches children BY TYPE, and a module constructed with `new` has an empty type name that can never match.
//
// Children are added AFTER scheduler.setup(), because the boot load trims a live tree down to what its saved file describes: with no file, anything added before setup is deleted during it.
struct Tree {
    mm::Scheduler scheduler;
    mm::FilesystemModule* fs = nullptr;
    mm::MoonModule* layers = nullptr;
    char root_[256] = {};

    Tree() {
        // Isolate the filesystem: without this the boot load reads the developer's real /.config/Effects.json and the tree arrives with whatever that machine happened to have, so the assertions below would depend on the box the tests run on. A monotonic counter, not millis(): two fixtures built in the same millisecond would share a root and read each other's files.
        static unsigned seq = 0;
        std::snprintf(root_, sizeof(root_), "/tmp/mm_subtree_test_%u", ++seq);
        std::filesystem::remove_all(root_);
        mm::platform::fsSetRoot(root_);

        mm::ModuleFactory::registerType<mm::Effects>("Effects");
        mm::ModuleFactory::registerType<mm::Layer>("Layer");
        mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");
        mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");

        fs = new mm::FilesystemModule();
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        layers = mm::ModuleFactory::create("Effects");
        scheduler.addModule(fs);
        scheduler.addModule(layers);
        scheduler.setup();
    }

    ~Tree() { std::filesystem::remove_all(root_); }   // don't leave a directory per test behind

    /// Add a child at runtime, driving the lifecycle the caller owns (MoonModule.h's contract).
    mm::MoonModule* add(mm::MoonModule* parent, const char* type) {
        auto* m = mm::ModuleFactory::create(type);
        REQUIRE(m != nullptr);
        parent->addChild(m);
        m->defineControls();
        m->setup();
        return m;
    }

    // The effect type at Effects→Layer[0]→child[0], or "" when there is none.
    const char* effectType() const {
        auto* layer = layers->child(0);
        if (!layer) return "";
        auto* fx = layer->child(0);
        return fx ? fx->typeName() : "";
    }
};

std::string serialize(mm::FilesystemModule* fs, mm::MoonModule* m) {
    mm::JsonSink sink;
    REQUIRE(fs->saveSubtreeTo(m, sink));
    return std::string(sink.data(), sink.size());
}

}  // namespace

// A subtree serializes into a caller's buffer, so a preset file can hold the same bytes the persistence engine writes rather than needing a second serializer that could drift from it.
TEST_CASE("saveSubtreeTo writes a subtree a caller can keep") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");

    const std::string json = serialize(t.fs, t.layers);

    // The flat dotted-key form the loader reads back: positional child types, enabled per node.
    CHECK(json.find("\"0.type\":\"Layer\"") != std::string::npos);
    CHECK(json.find("\"0.0.type\":\"NoiseEffect\"") != std::string::npos);
    CHECK(json.find("\"enabled\":") != std::string::npos);
}

// The round trip a preset IS: capture a tree, change it live, put the capture back. This is the whole feature in one assertion.
TEST_CASE("applySubtree restores a tree that changed since it was captured") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");
    REQUIRE(std::strcmp(t.effectType(), "NoiseEffect") == 0);

    const std::string preset = serialize(t.fs, t.layers);

    // The user changes the look: a different effect entirely.
    auto* old = layer->replaceChildAt(0, mm::ModuleFactory::create("RainbowEffect"));
    if (old) { old->release(); mm::Scheduler::deleteTree(old); }
    REQUIRE(std::strcmp(t.effectType(), "RainbowEffect") == 0);

    CHECK(t.fs->applySubtree(t.layers, preset.c_str()));
    CHECK(std::strcmp(t.effectType(), "NoiseEffect") == 0);   // the captured look is back
}

// A preset that carries MORE than the device has must add what is missing: applying it on a tree whose children were deleted rebuilds them, which is what makes a preset a restore rather than a value overlay.
TEST_CASE("applySubtree recreates children the live tree no longer has") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");

    const std::string preset = serialize(t.fs, t.layers);

    // Everything under Effects is removed, as a user clearing their setup would.
    auto* gone = t.layers->child(0);
    t.layers->removeChild(gone);
    gone->release();
    mm::Scheduler::deleteTree(gone);
    REQUIRE(t.layers->childCount() == 0);

    CHECK(t.fs->applySubtree(t.layers, preset.c_str()));
    REQUIRE(t.layers->childCount() == 1);
    CHECK(std::strcmp(t.effectType(), "NoiseEffect") == 0);
}

// And the reverse: a preset captured from a smaller tree must REMOVE what it does not describe, or applying it would leave the previous look layered underneath.
TEST_CASE("applySubtree removes children the preset does not describe") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");

    const std::string preset = serialize(t.fs, t.layers);   // captured with an EMPTY layer

    t.add(layer, "NoiseEffect");
    REQUIRE(layer->childCount() == 1);

    CHECK(t.fs->applySubtree(t.layers, preset.c_str()));
    CHECK(layer->childCount() == 0);
}

// A module type this build does not have is skipped and the rest of the preset still applies: a preset from a newer firmware, or from a board with a driver this one lacks, must degrade rather than refuse to load. Same tolerance the boot loader already has.
TEST_CASE("applySubtree skips an unknown module type and applies the rest") {
    Tree t;

    // Hand-written: a Layer whose first child is a type no factory knows.
    const char* preset =
        "{\"enabled\":true,"
        "\"0.type\":\"Layer\",\"0.enabled\":true,"
        "\"0.0.type\":\"NoSuchEffectXyz\",\"0.0.enabled\":true,"
        "\"0.1.type\":\"NoiseEffect\",\"0.1.enabled\":true}";

    CHECK(t.fs->applySubtree(t.layers, preset));

    REQUIRE(t.layers->childCount() == 1);                       // the Layer applied
    CHECK(std::strcmp(t.effectType(), "NoiseEffect") == 0);      // and so did the effect after it
}

// Malformed JSON leaves the tree alone rather than half-applying or crashing. A truncated or corrupted preset file is the realistic case (an interrupted upload), and the device must survive it with the look it already had.
TEST_CASE("applySubtree survives a corrupt preset") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");

    // Rejected, and it SAYS so: a caller reporting success to a user must be able to tell a real apply from a body that was never credible.
    CHECK_FALSE(t.fs->applySubtree(t.layers, "{\"0.type\":\"Lay"));   // truncated mid-key
    CHECK(t.layers->childCount() == 1);
    CHECK(std::strcmp(t.effectType(), "NoiseEffect") == 0);

    CHECK_FALSE(t.fs->applySubtree(t.layers, ""));                    // and an empty body
    CHECK(t.layers->childCount() == 1);
}

// Several subtrees share one flat object, each under its own "<TypeName>." prefix, and each reads back independently. This is the shape a preset file uses: it is what lets one file carry a selectable set of captures without a nested-object parser.
TEST_CASE("a prefixed subtree round-trips inside a larger object") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");

    mm::JsonSink sink;
    sink.append("{\"captures\":\"Effects\",");
    REQUIRE(t.fs->saveSubtreeTo(t.layers, sink, "Effects."));
    sink.append("}");
    const std::string preset(sink.data(), sink.size());

    CHECK(preset.find("\"Effects.0.type\":\"Layer\"") != std::string::npos);
    CHECK(preset.find("\"Effects.0.0.type\":\"NoiseEffect\"") != std::string::npos);

    // Change the look, then restore it from the prefixed body.
    auto* old = layer->replaceChildAt(0, mm::ModuleFactory::create("RainbowEffect"));
    if (old) { old->release(); mm::Scheduler::deleteTree(old); }
    REQUIRE(std::strcmp(t.effectType(), "RainbowEffect") == 0);

    CHECK(t.fs->applySubtree(t.layers, preset.c_str(), "Effects."));
    CHECK(std::strcmp(t.effectType(), "NoiseEffect") == 0);
}

// The live-reconfiguration rule extended to the file-upload path: writing /.config/<Type>.json (the File Manager upload, a config restore) applies onto the RUNNING tree, no reboot. Found as a real gap when the config-restore flow ended in a "reboot device" button.
TEST_CASE("a written config file applies to the running tree without a reboot") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");

    // The bytes a restore would upload: the same tree shape but with a different effect.
    std::string json = serialize(t.fs, t.layers);
    const auto at = json.find("NoiseEffect");
    REQUIRE(at != std::string::npos);
    json.replace(at, std::strlen("NoiseEffect"), "RainbowEffect");
    REQUIRE(mm::platform::fsWriteAtomic("/.config/Effects.json", json.data(), json.size()));

    CHECK(t.fs->applyConfigFile("/.config/Effects.json"));
    CHECK(std::strcmp(t.effectType(), "RainbowEffect") == 0);

    // Not config: a preset path and an unknown type leave the tree alone and say so.
    CHECK_FALSE(t.fs->applyConfigFile("/.config/presets/p1.json"));
    CHECK_FALSE(t.fs->applyConfigFile("/.config/NoSuchModule.json"));
    // Every guard rejects explicitly: null, outside /.config/, no .json, oversized stem.
    CHECK_FALSE(t.fs->applyConfigFile(nullptr));
    CHECK_FALSE(t.fs->applyConfigFile("/scripts/Effects.json"));
    CHECK_FALSE(t.fs->applyConfigFile("/.config/Effects"));
    CHECK_FALSE(t.fs->applyConfigFile(
        "/.config/A23456789012345678901234567890123456789012345678901234567890123456789.json"));
}

// The runtime twin of boot's phase 5: a restored config can carry a value for a control that only exists once prepare() has run (a MoonLive script's declared controls). The write requests a values-reapply that fires right after the next prepared tick, so the saved value lands.
TEST_CASE("a restored value for a prepare-time control lands after the next prepare") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    auto* fx = t.add(layer, "NoiseEffect");

    // A config whose subtree carries a key for a control the module does not have YET: overlayControls skips it on apply, exactly like a script control before compile.
    std::string json = serialize(t.fs, t.layers);
    json.insert(json.rfind('}'), ",\"0.0.laterControl\":42");
    REQUIRE(mm::platform::fsWriteAtomic("/.config/Effects.json", json.data(), json.size()));
    REQUIRE(t.fs->applyConfigFile("/.config/Effects.json"));

    // "prepare() creates the control": it appears after the apply, default 0.
    static uint8_t later = 0;
    later = 0;
    fx->controls().addControl("laterControl", later, 0, 100);
    REQUIRE(later == 0);

    // The tick that consumes the requested prepare also runs the values reapply.
    t.scheduler.requestPrepareTree();
    t.scheduler.tick();
    CHECK(later == 42);
}

namespace {
/// A module with one control present from the start and one that appears later, counting the changes it is told about.
struct ChangeWatcher : mm::MoonModule {
    uint8_t steady = 7;
    uint8_t later = 0;
    bool hasLater = false;   // stands in for a script declaring the control once it compiles
    int laterChanges = 0;
    int otherChanges = 0;
    void defineControls() override {
        MoonModule::defineControls();
        controls_.addControl("steady", steady, 0, 100);
        if (hasLater) controls_.addControl("laterControl", later, 0, 100);
    }
    void onControlChanged(const char* name) override {
        if (std::strcmp(name, "laterControl") == 0) laterChanges++;
        else otherChanges++;
    }
};
}  // namespace

// A value resolving only at the reapply gets the reaction any write gets, and one already in place gets none: a scripted palette's name resolves there.
TEST_CASE("a value that lands at the reapply reaches the module, and an unchanged one does not") {
    Tree t;
    mm::ModuleFactory::registerType<ChangeWatcher>("ChangeWatcher");
    auto* layer = t.add(t.layers, "Layer");
    auto* w = static_cast<ChangeWatcher*>(t.add(layer, "ChangeWatcher"));

    std::string json = serialize(t.fs, t.layers);
    json.insert(json.rfind('}'), ",\"0.0.laterControl\":42");
    REQUIRE(mm::platform::fsWriteAtomic("/.config/Effects.json", json.data(), json.size()));
    REQUIRE(t.fs->applyConfigFile("/.config/Effects.json"));

    w->hasLater = true;
    w->rebuildControls();
    t.scheduler.requestPrepareTree();
    t.scheduler.tick();
    CHECK(w->later == 42);
    CHECK(w->laterChanges == 1);
    CHECK(w->otherChanges == 0);   // `steady` was already in place, so boot side effects stay where a value moved
}

// The upload path queues, the render tick applies: nothing mutates the tree on the caller's task (re-running a system module's setup() on the web-server task crashed the ESP32), and a multi-file upload coalesces to one apply per module.
TEST_CASE("a requested config apply lands on the next tick, not on the requesting task") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");
    std::string json = serialize(t.fs, t.layers);
    const auto at = json.find("NoiseEffect");
    json.replace(at, std::strlen("NoiseEffect"), "RainbowEffect");
    REQUIRE(mm::platform::fsWriteAtomic("/.config/Effects.json", json.data(), json.size()));

    REQUIRE(t.fs->requestConfigApply("/.config/Effects.json"));
    CHECK(std::strcmp(t.effectType(), "NoiseEffect") == 0);   // nothing applied yet...
    t.fs->tick20ms();
    CHECK(std::strcmp(t.effectType(), "RainbowEffect") == 0); // ...the tick applies it

    // Not config: the same refusals as the synchronous entry.
    CHECK_FALSE(t.fs->requestConfigApply("/.config/presets/p1.json"));
    CHECK_FALSE(t.fs->requestConfigApply("/.config/NoSuchModule.json"));
}
