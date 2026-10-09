/// @module FilesystemModule
/// @also Scheduler, Layer

/// The seams a config written to a running device uses: `writeStateMember` serializes a subtree as a state document, and `applyStateDocument` rebuilds a tree that is already running from those bytes.

#include "doctest.h"
#include "core/system/FilesystemModule.h"
#include "core/util/ModuleFactory.h"
#include "core/module/Scheduler.h"
#include "core/module/StateDocument.h"
#include "core/util/JsonSink.h"
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

// An Effects tree built through the factory, since reconciliation matches children by type, with children added after setup, since the boot load trims what no file describes.
struct Tree {
    mm::Scheduler scheduler;
    mm::FilesystemModule* fs = nullptr;
    mm::MoonModule* layers = nullptr;
    char root_[256] = {};

    Tree() {
        // A private root per fixture, by a counter rather than millis(), so the boot load never reads this machine's own /.config files or another fixture's.
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
        REQUIRE(std::string(layers->name()) == "Effects");   // a document addresses the module by this name
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

std::string serialize(mm::MoonModule* m) {
    mm::JsonSink sink;
    sink.append("{");
    mm::writeStateMember(sink, *m, /*withSecrets=*/true);
    sink.append("}");
    return std::string(sink.data(), sink.size());
}

mm::StateDocumentResult applyStored(Tree& t, const std::string& doc) {
    return mm::applyStateDocument(t.scheduler, doc.c_str(), mm::StateSource::Stored);
}

}  // namespace

// A subtree serializes into a caller's buffer, the same bytes the persistence engine writes, so no second serializer can drift from it.
TEST_CASE("writeStateMember writes a subtree a caller can keep") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");

    const std::string json = serialize(t.layers);

    // The document form the loader reads back: members keyed by module name, a type on each child, enabled per node, children listed exactly.
    CHECK(json.rfind("{\"Effects\":{", 0) == 0);
    CHECK(json.find("\"Layer\":{\"type\":\"Layer\",\"$patch\":\"replace\"") != std::string::npos);
    CHECK(json.find("\"Noise\":{\"type\":\"NoiseEffect\"") != std::string::npos);
    CHECK(json.find("\"enabled\":") != std::string::npos);
}

// The round trip: capture a tree, change it live, put the capture back.
TEST_CASE("a stored document restores a tree that changed since it was captured") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");
    REQUIRE(std::strcmp(t.effectType(), "NoiseEffect") == 0);

    const std::string saved = serialize(t.layers);

    // The user changes the look: a different effect entirely.
    auto* old = layer->replaceChildAt(0, mm::ModuleFactory::create("RainbowEffect"));
    if (old) { old->release(); mm::Scheduler::deleteTree(old); }
    REQUIRE(std::strcmp(t.effectType(), "RainbowEffect") == 0);

    CHECK(applyStored(t, saved).ok);
    CHECK(std::strcmp(t.effectType(), "NoiseEffect") == 0);   // the captured look is back
}

// A document or a surface addresses a module by name, so the name a user gave it must outlive a save and a reload.
TEST_CASE("a saved subtree keeps the names its modules were given") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    layer->setName("Sky");
    t.add(layer, "NoiseEffect")->setName("clouds");
    const std::string saved = serialize(t.layers);
    CHECK(saved.find("\"Sky\":{\"type\":\"Layer\"") != std::string::npos);

    auto* gone = t.layers->child(0);
    t.layers->removeChild(gone);
    gone->release();
    mm::Scheduler::deleteTree(gone);
    REQUIRE(applyStored(t, saved).ok);
    REQUIRE(t.layers->childCount() == 1);
    CHECK(std::string(t.layers->child(0)->name()) == "Sky");
    CHECK(std::string(t.layers->child(0)->child(0)->name()) == "clouds");
}

// A stored file whose module name is already used elsewhere in the tree still loads: the module takes the next free name.
TEST_CASE("a stored document whose name clashes elsewhere takes a free name") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");
    const std::string doc = R"({"Effects":{"Other":{"type":"Layer","Noise":{"type":"NoiseEffect"}}}})";

    // A client's document is refused whole, since the name would be renamed on creation and the same document would never find it again.
    CHECK_FALSE(mm::applyStateDocument(t.scheduler, doc.c_str()).ok);
    REQUIRE(t.layers->childCount() == 1);

    CHECK(applyStored(t, doc).ok);
    REQUIRE(t.layers->childCount() == 2);
    CHECK(std::string(t.layers->child(1)->name()) == "Other");
    CHECK(std::string(t.layers->child(1)->child(0)->name()) == "Noise-2");
}

// A subtree carrying more than the device has adds what is missing, so applying it is a restore rather than a value overlay.
TEST_CASE("a stored document recreates children the live tree no longer has") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");

    const std::string saved = serialize(t.layers);

    // Everything under Effects is removed, as a user clearing their setup would.
    auto* gone = t.layers->child(0);
    t.layers->removeChild(gone);
    gone->release();
    mm::Scheduler::deleteTree(gone);
    REQUIRE(t.layers->childCount() == 0);

    CHECK(applyStored(t, saved).ok);
    REQUIRE(t.layers->childCount() == 1);
    CHECK(std::strcmp(t.effectType(), "NoiseEffect") == 0);
}

// And the reverse: a subtree captured from a smaller tree REMOVES what it does not describe, or applying it would leave the previous look layered underneath.
TEST_CASE("a stored document removes children it does not describe") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");

    const std::string saved = serialize(t.layers);   // captured with an EMPTY layer

    t.add(layer, "NoiseEffect");
    REQUIRE(layer->childCount() == 1);

    CHECK(applyStored(t, saved).ok);
    CHECK(layer->childCount() == 0);
}

// A type this build lacks is skipped and the rest applies, so a config from a newer firmware or another board degrades rather than refusing to load.
TEST_CASE("a stored document skips an unknown module type and applies the rest") {
    Tree t;

    // Hand-written: a Layer whose first child is a type no factory knows.
    const std::string body =
        R"({"Effects":{"Layer":{"type":"Layer","enabled":true,)"
        R"("NoSuch":{"type":"NoSuchEffectXyz","enabled":true},)"
        R"("Noise":{"type":"NoiseEffect","enabled":true}}}})";

    const mm::StateDocumentResult r = applyStored(t, body);
    CHECK(r.ok);
    CHECK(r.skipped == 1);                                      // counted, and logged

    REQUIRE(t.layers->childCount() == 1);                       // the Layer applied
    CHECK(std::strcmp(t.effectType(), "NoiseEffect") == 0);      // and so did the effect after it
}

// Malformed JSON leaves the tree alone rather than half-applying or crashing. A truncated or corrupted file is the realistic case (an interrupted upload), and the device must survive it with the look it already had.
TEST_CASE("a stored document survives a corrupt document") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");

    // Rejected, and it SAYS so: a caller reporting success to a user must be able to tell a real apply from a body that was never credible.
    CHECK_FALSE(applyStored(t, "{\"Effects\":{\"Lay").ok);   // truncated mid-key
    CHECK(t.layers->childCount() == 1);
    CHECK(std::strcmp(t.effectType(), "NoiseEffect") == 0);

    CHECK_FALSE(applyStored(t, "").ok);                       // and an empty body
    CHECK(t.layers->childCount() == 1);
}

// Writing /.config/<Type>.json, an upload or a restore, applies onto the running tree without a reboot.
TEST_CASE("a written config file applies to the running tree without a reboot") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");

    // The bytes a restore would upload: the same tree shape but with a different effect.
    std::string json = serialize(t.layers);
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

namespace {
/// A module with one control present from the start and one that appears at prepare, as a script's do once compiled, counting the changes it is told about.
struct ChangeWatcher : mm::MoonModule {
    uint8_t steady = 7;
    uint8_t later = 0;
    bool hasLater = false;   // stands in for a script declaring the control once it compiles
    int laterChanges = 0;
    int otherChanges = 0;
    bool declaresControlsAtPrepare() const override { return true; }
    void defineControls() override {
        MoonModule::defineControls();
        controls_.addControl("steady", steady, 0, 100);
        if (hasLater) controls_.addControl("laterControl", later, 0, 100);
    }
    void prepare() override {
        hasLater = true;
        rebuildControls();
    }
    void onControlChanged(const char* name) override {
        if (std::strcmp(name, "laterControl") == 0) laterChanges++;
        else otherChanges++;
    }
};

/// A ChangeWatcher named `Watch` in a Layer under the tree's layers.
ChangeWatcher* addWatcher(Tree& t) {
    mm::ModuleFactory::registerType<ChangeWatcher>("ChangeWatcher");
    auto* w = static_cast<ChangeWatcher*>(t.add(t.add(t.layers, "Layer"), "ChangeWatcher"));
    w->setName("Watch");
    return w;
}

const char* const kLaterDocument = R"({"Effects":{"Layer":{"Watch":{"steady":7,"laterControl":42}}}})";
}  // namespace

// A restored value for a control that exists only after prepare(), such as a script's, lands once the next prepare has declared it.
TEST_CASE("a restored value for a prepare-time control lands after the next prepare") {
    Tree t;
    ChangeWatcher* w = addWatcher(t);

    const mm::StateDocumentResult r = applyStored(t, kLaterDocument);
    REQUIRE(r.ok);
    CHECK(r.deferred == 1);       // the control does not exist yet, so it waits for the prepare
    REQUIRE(w->later == 0);

    // The tick that consumes the requested prepare also sets what waited for it.
    t.scheduler.tick();
    CHECK(w->later == 42);
}

// A value resolving only after the prepare gets the reaction any write gets, and one already in place gets none: a scripted palette's name resolves there.
TEST_CASE("a value that lands after the prepare reaches the module, and an unchanged one does not") {
    Tree t;
    ChangeWatcher* w = addWatcher(t);

    REQUIRE(applyStored(t, kLaterDocument).ok);
    t.scheduler.tick();
    CHECK(w->later == 42);
    CHECK(w->laterChanges == 1);
    CHECK(w->otherChanges == 0);   // `steady` was already in place, so boot side effects stay where a value moved
}

// A stored file sets its values in two passes around a rebuild, and the first pass already moves the value, so the reaction follows what either pass changed: a restored WiFi network has to reconnect.
TEST_CASE("a restored value that differs from the live one reaches the module once") {
    Tree t;
    ChangeWatcher* w = addWatcher(t);

    const mm::StateDocumentResult r = applyStored(t, R"({"Effects":{"Layer":{"Watch":{"steady":9}}}})");
    REQUIRE(r.ok);
    CHECK(w->steady == 9);
    CHECK(w->otherChanges == 1);
    CHECK(r.changes == 1);
}

// The upload path queues and the render tick applies, since setup() on the web-server task crashed the ESP32, and a multi-file upload applies once per module.
TEST_CASE("a requested config apply lands on the next tick, not on the requesting task") {
    Tree t;
    auto* layer = t.add(t.layers, "Layer");
    t.add(layer, "NoiseEffect");
    std::string json = serialize(t.layers);
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

// --- Live state is not configuration ------------------------------------------------------------

namespace {
/// A module with one saved setting and one live value, the shape a control surface has: the assignment is configuration, the position mirrors whatever it drives.
struct LiveAndSaved : public mm::MoonModule {
    uint8_t position = 0;      // live: something drives this continuously
    uint8_t setting  = 7;      // ordinary configuration
    void defineControls() override {
        controls_.addControl("position", position, 0, 255);
        controls_.setLive(controls_.count() - 1);
        controls_.addControl("setting", setting, 0, 255);
    }
};
}  // namespace

TEST_CASE("A live control is left out of the saved file, and its neighbors still save") {
    // A surface control's position MIRRORS its target, and that target persists in its own module. Writing the position too would store the same fact twice and let the two disagree on load.
    LiveAndSaved m;
    m.setName("Surface");
    m.rebuildControls();
    m.position = 200;
    m.setting  = 42;

    mm::JsonSink sink;
    sink.append("{");
    mm::writeStateMember(sink, m);
    sink.append("}");
    const std::string json(sink.data(), sink.size());
    CHECK(json.find("\"setting\"")  != std::string::npos);   // configuration is written
    CHECK(json.find("\"position\"") == std::string::npos);   // live state is not
}

TEST_CASE("Writing a live control does not mark its module dirty, so a slow save still lands") {
    // The defect this exists to remove.
    // FilesystemModule waits two seconds after the LAST dirty mark, so a control written at 50 Hz re-stamped the timer forever and the module's file was never written AT ALL.
    // Measured on an ESP32-P4: lastSaved only aged, across minutes.
    // Everything else in that file, the assignments included, was lost on a power cut.
    mm::Scheduler sched;
    auto* m = new LiveAndSaved();
    m->setName("Surface");
    sched.addModule(m);
    sched.setup();

    m->clearDirty();
    // A live write: applied, but not configuration.
    CHECK(sched.setControl("Surface", "position", "{\"value\":123}") ==
          mm::Scheduler::SetControlResult::Ok);
    CHECK(m->position == 123);          // the value DID apply
    CHECK_FALSE(m->dirty());          // and did not ask to be saved

    // An ordinary setting still does.
    CHECK(sched.setControl("Surface", "setting", "{\"value\":9}") ==
          mm::Scheduler::SetControlResult::Ok);
    CHECK(m->dirty());

    sched.release();
}

TEST_CASE("A continuous writer cannot defer a pending save forever") {
    // The starvation the deferral ceiling exists to remove, and the reason `live` alone was not enough: marking a surface control live stops IT from marking dirty, but the moment that control is ASSIGNED to something (a fader driving Drivers.brightness, two clicks in the UI) the write lands on an ordinary persisted control and the 50 Hz stream of dirty marks resumes one module downstream.
    // Measured on an ESP32-P4: an unrelated setting changed while a sweep ran was still unsaved 56 seconds later.
    //
    // So the fix belongs in the mechanism: however often marks arrive, a pending save lands within MAX_DEFER_MS.
    // The debounce still coalesces a burst; it just cannot be extended without end.
    CHECK(mm::FilesystemModule::MAX_DEFER_MS > mm::FilesystemModule::DEBOUNCE_MS);

    Tree t;
    auto* m = static_cast<mm::NoiseEffect*>(t.add(t.add(t.layers, "Layer"), "NoiseEffect"));

    // A change worth saving, then a continuous stream of marks arriving faster than the debounce, which is what a 50 Hz writer produces.
    CHECK(t.scheduler.setControl(m->name(), "scale", "{\"value\":99}") ==
          mm::Scheduler::SetControlResult::Ok);
    REQUIRE(m->dirty());

    // Drive the module's clock past the ceiling, re-marking throughout so the debounce never expires on its own.
    // Each tick1s is a second of device time.
    // Re-claim the static instance: an earlier test in this binary may still own it, and noteDirty() is a static that routes to whoever holds it.
    t.fs->setScheduler(&t.scheduler);

    // A continuous writer: marks arrive faster than the debounce, so the debounce alone would never expire. The ceiling clock is AGED rather than waited out, because what is under test is the comparison in tick1s, not the host's ability to sleep for ten seconds.
    for (int s = 0; s < 30 && m->dirty(); s++) {
        mm::FilesystemModule::noteDirty();          // the continuous writer, faster than the debounce
        t.fs->ageDirtyForTest(1000);                // ... and a second of device time goes by
        t.fs->tick1s();
    }
    // Within the ceiling the save must have landed, despite marks never stopping.
    CHECK_FALSE(m->dirty());
}
