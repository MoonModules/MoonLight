/// @module ControlModule
/// @also FilesystemModule, Scheduler

/// Presets end to end: a preset is a file, saving writes one, selecting reads it back, and a bad file must not cost the device its state.

#include "doctest.h"
#include "core/services/MidiService.h"

#include <vector>
#include "core/system/ControlModule.h"
#include "core/system/FilesystemModule.h"
#include "core/util/ModuleFactory.h"
#include "core/module/Scheduler.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/layers/Layer.h"
#include "light/layers/Effects.h"
#include "light/drivers/Drivers.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

// A device with Effects and a ControlModule, built the way production builds one. Through the factory, with an isolated filesystem so the assertions do not depend on what is on this machine.
struct Device {
    mm::Scheduler scheduler;
    mm::FilesystemModule* fs = nullptr;
    mm::ControlModule* control = nullptr;
    mm::MoonModule* layers = nullptr;
    mm::MoonModule* drivers = nullptr;
    char root_[256] = {};

    Device() {
        // A monotonic counter, not millis(): two fixtures constructed in the same millisecond would otherwise share a root and see each other's preset files.
        static unsigned seq = 0;
        std::snprintf(root_, sizeof(root_), "/tmp/mm_preset_test_%u", ++seq);
        std::filesystem::remove_all(root_);
        mm::platform::fsSetRoot(root_);

        mm::ModuleFactory::registerType<mm::Effects>("Effects");
        mm::ModuleFactory::registerType<mm::Layer>("Layer");
        mm::ModuleFactory::registerType<mm::NoiseEffect>("NoiseEffect");
        mm::ModuleFactory::registerType<mm::RainbowEffect>("RainbowEffect");
        mm::ModuleFactory::registerType<mm::ControlModule>("ControlModule");
        mm::ModuleFactory::registerType<mm::Drivers>("Drivers");

        fs = new mm::FilesystemModule();
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        layers = mm::ModuleFactory::create("Effects");
        drivers = mm::ModuleFactory::create("Drivers");
        control = static_cast<mm::ControlModule*>(mm::ModuleFactory::create("ControlModule"));
        scheduler.addModule(fs);
        scheduler.addModule(layers);
        scheduler.addModule(drivers);
        scheduler.addModule(control);
        scheduler.setup();
    }

    // The surface seat is static: freeing this ControlModule vacates it, or the first test's would answer active() for every later one.
    ~Device() { delete control; std::filesystem::remove_all(root_); }

    mm::MoonModule* add(mm::MoonModule* parent, const char* type) {
        auto* m = mm::ModuleFactory::create(type);
        REQUIRE(m != nullptr);
        parent->addChild(m);
        m->defineControls();
        m->setup();
        return m;
    }

    /// Set a control's text value without firing the change hook, which a test fires with press() as the UI does.
    void setText(const char* controlName, const char* value) {
        auto& cs = control->controls();
        for (uint8_t i = 0; i < cs.count(); i++) {
            if (std::strcmp(cs[i].name, controlName) != 0) continue;
            std::snprintf(static_cast<char*>(cs[i].ptr), static_cast<size_t>(cs[i].max), "%s", value);
            return;
        }
        FAIL("no control named ", controlName);
    }

    /// What the next save captures: exactly one of Layouts / Effects / Drivers / Services.
    void setCapture(const char* typeName) {
        auto& cs = control->controls();
        for (uint8_t i = 0; i < cs.count(); i++) {
            if (std::strcmp(cs[i].name, "captures") != 0) continue;
            for (uint8_t r = 0; r < mm::ControlModule::kCaptureCount; r++) {
                if (std::strcmp(mm::ControlModule::kCapturable[r], typeName) != 0) continue;
                *static_cast<uint8_t*>(cs[i].ptr) = r;
                return;
            }
            FAIL("no capturable type named ", typeName);
        }
        FAIL("no captures control");
    }

    void press(const char* button) { control->onControlChanged(button); }

    /// The row JSON for a preset by name, which is how the pad grid is inspected.
    std::string rowNamed(const char* name) const {
        for (uint8_t i = 0; i < control->listRowCount(); i++) {
            mm::JsonSink sink;
            control->writeListRow(sink, i);
            std::string row(sink.data(), sink.size());
            if (row.find(std::string("\"name\":\"") + name + "\"") != std::string::npos) return row;
        }
        return "";
    }

    /// Apply a preset by name, the way a pad click does.
    void activate(const char* name) {
        const std::string row = rowNamed(name);
        REQUIRE_MESSAGE(!row.empty(), "no preset named ", name);
        const uint32_t id = static_cast<uint32_t>(std::stoul(row.substr(row.find("\"id\":") + 5)));
        control->setListRowField(id, "activate", "{}");
    }

    /// The module's own status, the one every module reports through MoonModule::setStatus and the UI shows in the card's status chip.
    const char* status() const { return control->status() ? control->status() : ""; }

    const char* effectType() const {
        auto* layer = layers->child(0);
        if (!layer) return "";
        auto* fx = layer->child(0);
        return fx ? fx->typeName() : "";
    }

    /// The stable id of the one preset row, or 0 when the list is empty.
    uint32_t firstRowId() const {
        if (control->listRowCount() == 0) return 0;
        mm::JsonSink sink;
        control->writeListRow(sink, 0);
        const std::string row(sink.data(), sink.size());
        const size_t at = row.find("\"id\":");
        return at == std::string::npos ? 0 : static_cast<uint32_t>(std::stoul(row.substr(at + 5)));
    }
};

}  // namespace

// The feature in one test: keep a look, change it, bring it back.
TEST_CASE("ControlModule saves a look and puts it back") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setText("name", "sunset");
    d.press("save");
    REQUIRE(d.control->listRowCount() == 1);

    // The user moves on to something else.
    auto* old = layer->replaceChildAt(0, mm::ModuleFactory::create("RainbowEffect"));
    if (old) { old->release(); mm::Scheduler::deleteTree(old); }
    REQUIRE(std::strcmp(d.effectType(), "RainbowEffect") == 0);

    d.control->setListRowField(d.firstRowId(), "apply", "{}");
    CHECK(std::strcmp(d.effectType(), "NoiseEffect") == 0);
}

// A preset is a file, so the folder is the list: what is on disk is what the user sees.
TEST_CASE("ControlModule lists one row per preset file") {
    Device d;
    d.add(d.layers, "Layer");

    d.setText("name", "one");
    d.press("save");
    d.setText("name", "two");
    d.press("save");

    CHECK(d.control->listRowCount() == 2);
}

// Deleting a preset deletes its file. Nothing else holds preset state, so there is no second copy that could disagree with the folder.
TEST_CASE("ControlModule deletes a preset by deleting its file") {
    Device d;
    d.add(d.layers, "Layer");
    d.setText("name", "throwaway");
    d.press("save");
    REQUIRE(d.control->listRowCount() == 1);

    CHECK(d.control->deleteListRow(d.firstRowId()));
    CHECK(d.control->listRowCount() == 0);
}

// Saving without a name would write ".json" and produce a nameless row, so it is refused with a message rather than silently creating something the user cannot identify.
TEST_CASE("ControlModule refuses to save a preset with no name") {
    Device d;
    d.add(d.layers, "Layer");

    d.press("save");
    CHECK(d.control->listRowCount() == 0);
    CHECK(std::string(d.status()).find("name") != std::string::npos);
}

// A preset carries exactly one role, so a file from an older build naming several is listed and deletable but never applied.
TEST_CASE("ControlModule refuses to apply a preset carrying several roles") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    const char* body =
        "{\"captures\":\"Layouts,Effects\","
        "\"Effects.enabled\":true,"
        "\"Effects.0.type\":\"Layer\",\"Effects.0.enabled\":true}";
    mm::platform::fsMkdir(mm::ControlModule::kPresetDir);
    char path[160];
    std::snprintf(path, sizeof(path), "%s/legacy.json", mm::ControlModule::kPresetDir);
    REQUIRE(mm::platform::fsWriteAtomic(path, body, std::strlen(body)));
    d.control->setup();

    REQUIRE(d.control->listRowCount() == 1);                 // visible, so it can be deleted
    CHECK(d.control->roleOf(0) == mm::ControlModule::kCaptureCount);   // but it holds no single role
    CHECK_FALSE(d.control->setListRowField(d.firstRowId(), "apply", "{}"));
    CHECK(std::string(d.status()).find("several roles") != std::string::npos);
}

// A preset carrying a module this build does not have applies what it can and says what it skipped. A preset from another board must degrade rather than refuse or crash.
TEST_CASE("ControlModule refuses a preset whose subtree this build does not have") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    // Hand-written: names a single module type no build registers, so the apply must refuse with a reason rather than report success for a preset that changed nothing.
    const std::string body =
        "{\"captures\":\"NoSuchTopLevelXyz\","
        "\"Effects.enabled\":true,"
        "\"Effects.0.type\":\"Layer\",\"Effects.0.enabled\":true,"
        "\"Effects.0.0.type\":\"NoiseEffect\",\"Effects.0.0.enabled\":true}";
    mm::platform::fsMkdir(mm::ControlModule::kPresetDir);
    char path[128];
    std::snprintf(path, sizeof(path), "%s/mixed.json", mm::ControlModule::kPresetDir);
    REQUIRE(mm::platform::fsWriteAtomic(path, body.c_str(), body.size()));

    d.control->setup();                      // rescan picks the file up
    REQUIRE(d.control->listRowCount() == 1);

    CHECK_FALSE(d.control->setListRowField(d.firstRowId(), "apply", "{}"));
    CHECK(std::string(d.status()).find("nothing this build knows") != std::string::npos);
}

// A truncated file (an interrupted upload) must leave the device with the look it already had.
TEST_CASE("ControlModule survives a corrupt preset file") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    mm::platform::fsMkdir(mm::ControlModule::kPresetDir);
    char path[128];
    std::snprintf(path, sizeof(path), "%s/broken.json", mm::ControlModule::kPresetDir);
    const char* truncated = "{\"captures\":\"Effects\",\"Effects.0.ty";
    REQUIRE(mm::platform::fsWriteAtomic(path, truncated, std::strlen(truncated)));

    d.control->setup();
    REQUIRE(d.control->listRowCount() == 1);                         // the file IS listed
    // Apply returns false: the row was attempted but nothing was usable. Asserting the return as well as the tree tells a rejected bad file from doing nothing.
    CHECK_FALSE(d.control->setListRowField(d.firstRowId(), "apply", "{}"));

    CHECK(d.layers->childCount() == 1);                              // the tree is untouched
    CHECK(std::strcmp(d.effectType(), "NoiseEffect") == 0);
}

// The row says what the preset carries, so a user can tell a portable look from a device snapshot before applying it.
TEST_CASE("ControlModule shows what each preset captures") {
    Device d;
    d.add(d.layers, "Layer");
    d.setText("name", "look");
    d.press("save");

    mm::JsonSink sink;
    d.control->writeListRow(sink, 0);
    const std::string row(sink.data(), sink.size());
    CHECK(row.find("\"name\":\"look\"") != std::string::npos);
    CHECK(row.find("Effects") != std::string::npos);
}

// The pad grid answers "what is on right now" without a click, so the applied preset marks itself active and any other preset does not.
TEST_CASE("ControlModule marks the applied preset as the active pad") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setText("name", "first");
    d.press("save");
    d.setText("name", "second");
    d.press("save");
    REQUIRE(d.control->listRowCount() == 2);

    auto rowJson = [&](uint8_t i) {
        mm::JsonSink sink;
        d.control->writeListRow(sink, i);
        return std::string(sink.data(), sink.size());
    };
    // Nothing applied yet: no pad is lit.
    CHECK(rowJson(0).find("\"active\":true") == std::string::npos);
    CHECK(rowJson(1).find("\"active\":true") == std::string::npos);

    // Apply one, and exactly that one lights up.
    mm::JsonSink idSink;
    d.control->writeListRow(idSink, 0);
    const std::string first(idSink.data(), idSink.size());
    const uint32_t id = static_cast<uint32_t>(std::stoul(first.substr(first.find("\"id\":") + 5)));
    d.control->setListRowField(id, "activate", "{}");

    CHECK(rowJson(0).find("\"active\":true") != std::string::npos);
    CHECK(rowJson(1).find("\"active\":true") == std::string::npos);
}

// A pad click and a row button are the same action from two presentations, so both reach apply.
TEST_CASE("ControlModule applies a preset from either the pad or the row button") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");
    d.setText("name", "look");
    d.press("save");

    auto swapToRainbow = [&] {
        auto* old = layer->replaceChildAt(0, mm::ModuleFactory::create("RainbowEffect"));
        if (old) { old->release(); mm::Scheduler::deleteTree(old); }
    };

    swapToRainbow();
    CHECK(d.control->setListRowField(d.firstRowId(), "activate", "{}"));   // the pad
    CHECK(std::strcmp(d.effectType(), "NoiseEffect") == 0);

    swapToRainbow();
    CHECK(d.control->setListRowField(d.firstRowId(), "apply", "{}"));      // the row button
    CHECK(std::strcmp(d.effectType(), "NoiseEffect") == 0);
}

// Pads can be dragged into the user's order, and that order survives a rescan because it lives in the files, not in the filesystem's order.
TEST_CASE("ControlModule keeps the pad order the user arranged") {
    Device d;
    d.add(d.layers, "Layer");
    for (const char* n : {"a", "b", "c"}) { d.setText("name", n); d.press("save"); }
    REQUIRE(d.control->listRowCount() == 3);

    auto nameAt = [&](uint8_t i) {
        mm::JsonSink sink;
        d.control->writeListRow(sink, i);
        const std::string row(sink.data(), sink.size());
        const size_t at = row.find("\"name\":\"") + 8;
        return row.substr(at, row.find('"', at) - at);
    };
    auto idAt = [&](uint8_t i) {
        mm::JsonSink sink;
        d.control->writeListRow(sink, i);
        const std::string row(sink.data(), sink.size());
        return static_cast<uint32_t>(std::stoul(row.substr(row.find("\"id\":") + 5)));
    };

    // Move the last pad to the front, as a drag would.
    const std::string last = nameAt(2);
    REQUIRE(d.control->moveListRow(idAt(2), 0));
    CHECK(nameAt(0) == last);

    // And it is still first after a rescan, because the order lives in the files.
    d.control->setup();
    CHECK(nameAt(0) == last);
}

// A preset says which roles it covers, so a pad shows the matching emoji and a user can tell a portable look from a device snapshot.
TEST_CASE("ControlModule reports the roles a preset covers") {
    Device d;
    d.add(d.layers, "Layer");
    d.setText("name", "look");
    d.press("save");

    mm::JsonSink sink;
    d.control->writeListRow(sink, 0);
    const std::string row(sink.data(), sink.size());
    CHECK(row.find("\"roles\":[\"effects\"]") != std::string::npos);   // Effects is captured by default
}

// Fader 1 rides the global brightness through the same setControl primitive IR and the network bridges use.
TEST_CASE("ControlModule fader 1 drives the global brightness") {
    Device d;
    auto brightness = [&] {
        auto& cs = d.drivers->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, "brightness") == 0) return *static_cast<uint8_t*>(cs[i].ptr);
        return static_cast<uint8_t>(0);
    };
    const uint8_t before = brightness();

    // Move the fader the way the UI does: set its value, then fire its change hook.
    auto& cs = d.control->controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "fader1") != 0) continue;
        *static_cast<uint8_t*>(cs[i].ptr) = 200;
        d.control->onControlChanged("fader1");
        break;
    }
    CHECK(brightness() == 200);
    CHECK(before != 200);   // the value really moved, rather than already being there
}

// The unassigned faders have no target yet, so moving one must be inert rather than driving something by accident.
TEST_CASE("ControlModule an unassigned fader drives nothing") {
    Device d;
    auto brightness = [&] {
        auto& cs = d.drivers->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, "brightness") == 0) return *static_cast<uint8_t*>(cs[i].ptr);
        return static_cast<uint8_t>(0);
    };
    const uint8_t before = brightness();

    auto& cs = d.control->controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "fader3") != 0) continue;
        *static_cast<uint8_t*>(cs[i].ptr) = 99;
        d.control->onControlChanged("fader3");
        break;
    }
    CHECK(brightness() == before);
}

// A save lands on the pad the user right-clicked, not in the first free cell. On a surface, WHERE something goes is the user's choice, and a save that ignored it would scatter presets.
TEST_CASE("ControlModule saves a preset onto the chosen pad") {
    Device d;
    d.add(d.layers, "Layer");

    auto setU8 = [&](const char* name, uint8_t v) {
        auto& cs = d.control->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, name) == 0) { *static_cast<uint8_t*>(cs[i].ptr) = v; return; }
        FAIL("no control named ", name);
    };

    d.setText("name", "corner");
    setU8("slot", 20);                 // a pad well away from the first free cell
    d.press("save");

    mm::JsonSink sink;
    d.control->writeListRow(sink, 0);
    const std::string row(sink.data(), sink.size());
    CHECK(row.find("\"slot\":20") != std::string::npos);
}

// A surface control's own name, not the "<name>Target" assignment control beside it.
static bool isSurfaceControl(const char* name, const char* prefix) {
    const size_t plen = std::strlen(prefix);
    if (std::strncmp(name, prefix, plen) != 0) return false;
    return std::strstr(name, "Target") == nullptr;
}

// encoder1 selects the palette and the rest are unbound, so they are inert; brightness belongs to fader1 and no encoder may touch it.
TEST_CASE("only the bound encoder drives anything") {
    Device d;
    auto brightness = [&] {
        auto& cs = d.drivers->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, "brightness") == 0) return *static_cast<uint8_t*>(cs[i].ptr);
        return static_cast<uint8_t>(0);
    };
    const uint8_t before = brightness();

    auto& cs = d.control->controls();
    uint8_t found = 0;
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (!isSurfaceControl(cs[i].name, "enc")) continue;
        found++;
        *static_cast<uint8_t*>(cs[i].ptr) = 123;
        d.control->onControlChanged(cs[i].name);
    }
    CHECK(found == mm::ControlModule::kEncoderCount);
    CHECK(brightness() == before);     // nothing was driven
}


// Presets hold their roles independently, so a layout pad and a look pad stay lit together and applying a layer preset leaves the layout role alone.
TEST_CASE("ControlModule keeps one active preset per captured role") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    // A driver-only preset and a layer-only preset: two roles, two pads.
    d.setCapture("Drivers");
    d.setText("name", "geometry");
    d.press("save");

    d.setCapture("Effects");
    d.setText("name", "look");
    d.press("save");
    REQUIRE(d.control->listRowCount() == 2);

    d.activate("geometry");
    CHECK(d.rowNamed("geometry").find("\"active\":true") != std::string::npos);

    // The layer preset claims only the layer role, so the driver pad stays lit.
    d.activate("look");
    CHECK(d.rowNamed("look").find("\"active\":true") != std::string::npos);
    CHECK(d.rowNamed("geometry").find("\"active\":true") != std::string::npos);

    // And each reports WHICH role it holds, which is what the pad colors itself by.
    CHECK(d.rowNamed("geometry").find("\"activeRoles\":[\"driver\"]") != std::string::npos);
    CHECK(d.rowNamed("look").find("\"activeRoles\":[\"effects\"]") != std::string::npos);
}

// Each role is held independently, so applying a look replaces the look and leaves the geometry alone. With one role per preset this is the whole supersede rule.
TEST_CASE("ControlModule replaces only the role a preset carries") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setCapture("Drivers");
    d.setText("name", "hardware");  d.press("save");
    d.setCapture("Effects");
    d.setText("name", "lookA");     d.press("save");
    d.setText("name", "lookB");     d.press("save");

    d.activate("hardware");
    d.activate("lookA");
    CHECK(d.rowNamed("hardware").find("\"active\":true") != std::string::npos);
    CHECK(d.rowNamed("lookA").find("\"active\":true") != std::string::npos);

    // A second look takes the layer role from the first; the driver preset is untouched.
    d.activate("lookB");
    CHECK(d.rowNamed("lookB").find("\"active\":true") != std::string::npos);
    CHECK(d.rowNamed("lookA").find("\"active\":true") == std::string::npos);
    CHECK(d.rowNamed("hardware").find("\"active\":true") != std::string::npos);
}


// Renaming a preset renames its file, so the pad keeps its contents under the new name. The name IS the identity here (there is no id inside the file), which is what makes a rename a file move rather than an edit.
TEST_CASE("ControlModule renames a preset by renaming its file") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setText("name", "before");
    d.press("save");
    REQUIRE(d.control->listRowCount() == 1);

    REQUIRE(d.control->setListRowField(d.firstRowId(), "name", "{\"value\":\"after\"}"));
    REQUIRE(d.control->listRowCount() == 1);                  // still one preset, not two
    CHECK(!d.rowNamed("after").empty());
    CHECK(d.rowNamed("before").empty());

    // The renamed preset still applies, so the rename moved the contents rather than just the label.
    auto* old = layer->replaceChildAt(0, mm::ModuleFactory::create("RainbowEffect"));
    if (old) { old->release(); mm::Scheduler::deleteTree(old); }
    REQUIRE(std::strcmp(d.effectType(), "RainbowEffect") == 0);
    d.activate("after");
    CHECK(std::strcmp(d.effectType(), "NoiseEffect") == 0);
}

// A preset capturing nothing the device has still lists, and applying it reports failure, so "applied" and "nothing applied" differ for the status line.
TEST_CASE("ControlModule reports an apply that changed nothing") {
    Device d;
    d.add(d.layers, "Layer");

    d.setCapture("Effects");
    d.setText("name", "look");
    d.press("save");
    REQUIRE(d.control->listRowCount() == 1);

    // A file whose captures name a subtree this build does not have.
    char path[160];
    std::snprintf(path, sizeof(path), "%s/ghost.json", mm::ControlModule::kPresetDir);
    const char* body = "{\"captures\":\"NoSuchModuleXyz\",\"NoSuchModuleXyz.enabled\":true}";
    REQUIRE(mm::platform::fsWriteAtomic(path, body, std::strlen(body)));
    d.control->setup();
    REQUIRE(d.control->listRowCount() == 2);

    const std::string row = d.rowNamed("ghost");
    REQUIRE(!row.empty());
    const uint32_t id = static_cast<uint32_t>(std::stoul(row.substr(row.find("\"id\":") + 5)));
    CHECK_FALSE(d.control->setListRowField(id, "activate", "{}"));   // nothing applied, and it says so
}


// A preset name becomes a file name, and ESP32's filesystem does no path normalization, so the name validator must refuse anything that could escape the folder.
TEST_CASE("ControlModule refuses a preset name that could escape its folder") {
    Device d;
    d.add(d.layers, "Layer");

    auto* nameCtrl = [&]() -> const mm::ControlDescriptor* {
        auto& cs = d.control->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, "name") == 0) return &cs[i];
        return nullptr;
    }();
    REQUIRE(nameCtrl != nullptr);
    REQUIRE(nameCtrl->validate != nullptr);          // the guard is bound, not just written

    // Anything that could steer a path is refused...
    CHECK_FALSE(nameCtrl->validate("../escape"));
    CHECK_FALSE(nameCtrl->validate("../../etc/passwd"));
    CHECK_FALSE(nameCtrl->validate("sub/dir"));
    CHECK_FALSE(nameCtrl->validate("back\\slash"));
    CHECK_FALSE(nameCtrl->validate("dot.name"));     // the .json suffix is ours to add
    CHECK_FALSE(nameCtrl->validate(""));

    // ...while ordinary names a user would pick still work.
    CHECK(nameCtrl->validate("sunset"));
    CHECK(nameCtrl->validate("Warm White 2"));

    // And a rename cannot smuggle one in through the row-edit path either.
    d.setText("name", "safe");
    d.press("save");
    REQUIRE(d.control->listRowCount() == 1);
    CHECK_FALSE(d.control->setListRowField(d.firstRowId(), "name", "{\"value\":\"../escape\"}"));
    CHECK(!d.rowNamed("safe").empty());              // the preset is untouched
}


// Renaming onto an existing name refuses, since the write would clobber the other preset and the remove would then delete the source.
TEST_CASE("ControlModule refuses to rename a preset over an existing one") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setText("name", "keep");
    d.press("save");
    d.setText("name", "other");
    d.press("save");
    REQUIRE(d.control->listRowCount() == 2);

    const std::string row = d.rowNamed("other");
    REQUIRE(!row.empty());
    const uint32_t id = static_cast<uint32_t>(std::stoul(row.substr(row.find("\"id\":") + 5)));

    CHECK_FALSE(d.control->setListRowField(id, "name", "{\"value\":\"keep\"}"));
    CHECK(d.control->listRowCount() == 2);         // both survive
    CHECK(!d.rowNamed("keep").empty());
    CHECK(!d.rowNamed("other").empty());
}


// The saved file is valid JSON, not merely readable by our lenient helpers, because users download, edit and re-upload presets.
TEST_CASE("ControlModule writes a preset that is well-formed JSON") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setCapture("Effects");
    d.setText("name", "wellformed");
    d.press("save");

    char path[160];
    std::snprintf(path, sizeof(path), "%s/wellformed.json", mm::ControlModule::kPresetDir);
    const long size = mm::platform::fsSize(path);
    REQUIRE(size > 0);
    std::string body(static_cast<size_t>(size) + 1, '\0');
    REQUIRE(mm::platform::fsRead(path, body.data(), body.size()) > 0);
    body.resize(std::strlen(body.c_str()));

    // A minimal structural check: balanced braces, no empty pair, and no doubled separator -- the three ways a hand-assembled object breaks.
    CHECK(body.front() == '{');
    CHECK(body.back() == '}');
    CHECK(body.find(",,") == std::string::npos);
    CHECK(body.find("{,") == std::string::npos);
    CHECK(body.find(",}") == std::string::npos);
    int depth = 0;
    for (char c : body) { if (c == '{') depth++; else if (c == '}') depth--; }
    CHECK(depth == 0);
}


// An applied preset survives a reboot: applying marks the tree dirty, else the boot loader restores the previous look from the config file.
TEST_CASE("ControlModule persists the look a preset applied") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setCapture("Effects");
    d.setText("name", "keeper");
    d.press("save");

    // Change the look, and let that change reach the config file.
    auto* old = layer->replaceChildAt(0, mm::ModuleFactory::create("RainbowEffect"));
    if (old) { old->release(); mm::Scheduler::deleteTree(old); }
    d.fs->flush();
    REQUIRE(std::strcmp(d.effectType(), "RainbowEffect") == 0);

    d.activate("keeper");
    REQUIRE(std::strcmp(d.effectType(), "NoiseEffect") == 0);   // applied to the LIVE tree

    // The config file must now describe the applied look, not the one it replaced.
    d.fs->flush();
    char path[160];
    std::snprintf(path, sizeof(path), "/.config/%s.json", d.layers->typeName());
    const long size = mm::platform::fsSize(path);
    REQUIRE(size > 0);
    std::string body(static_cast<size_t>(size) + 1, '\0');
    REQUIRE(mm::platform::fsRead(path, body.data(), body.size()) > 0);
    body.resize(std::strlen(body.c_str()));
    CHECK(body.find("NoiseEffect") != std::string::npos);
    CHECK(body.find("RainbowEffect") == std::string::npos);
}


// Only a pure look is reachable from outside: a preset carrying Drivers or Layouts rewires pins or geometry, which a voice assistant must not do.
TEST_CASE("ControlModule exposes only look-only presets to external surfaces") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setCapture("Effects");
    d.setText("name", "purelook");  d.press("save");

    d.setCapture("Drivers");
    d.setText("name", "pinsonly");  d.press("save");
    REQUIRE(d.control->presetCount() == 2);

    auto lookOnlyNamed = [&](const char* n) {
        for (uint8_t i = 0; i < d.control->presetCount(); i++)
            if (std::strcmp(d.control->presetName(i), n) == 0) return d.control->isLookOnly(i);
        FAIL("no preset named ", n);
        return false;
    };
    CHECK(lookOnlyNamed("purelook"));
    CHECK_FALSE(lookOnlyNamed("pinsonly"));    // hardware, not a look

    // And the apply entry point enforces it, not just the listing: naming a hardware-carrying preset through the external path is refused outright.
    CHECK(d.control->applyLookByName("purelook"));
    CHECK_FALSE(d.control->applyLookByName("pinsonly"));
    CHECK_FALSE(d.control->applyLookByName("nosuchpreset"));

    CHECK(std::string(d.control->currentLook()) == "purelook");
}


// The Home Assistant look list is sized from the presets that exist, since a fixed cap wastes RAM or silently publishes nothing once outgrown.
TEST_CASE("ControlModule sizes the Home Assistant look list to the presets that exist") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    auto lookNames = [&]() {
        size_t bytes = 0, count = 0;
        for (uint8_t i = 0; i < d.control->presetCount(); i++) {
            if (!d.control->isLookOnly(i)) continue;
            bytes += std::strlen(d.control->presetName(i));
            count++;
        }
        return std::make_pair(count, bytes);
    };

    // Nothing to publish yet.
    CHECK(lookNames().first == 0);

    d.setCapture("Effects");
    d.setText("name", "one");       d.press("save");
    const auto afterOne = lookNames();
    CHECK(afterOne.first == 1);

    d.setText("name", "twotwotwo"); d.press("save");
    const auto afterTwo = lookNames();
    CHECK(afterTwo.first == 2);
    CHECK(afterTwo.second > afterOne.second);      // the requirement GREW with the longer name

    // A hardware-carrying preset must not enlarge the list at all: it is never published.
    d.setCapture("Drivers");
    d.setText("name", "hardware");  d.press("save");
    CHECK(lookNames().first == 2);
    CHECK(lookNames().second == afterTwo.second);
}


// HA re-fetches the preset list only when the reported revision changes, so the revision is a counter that moves on every mutation, even within one second.
TEST_CASE("ControlModule bumps its revision on every preset-set change") {
    Device d;
    d.add(d.layers, "Layer");

    d.setCapture("Effects");
    d.setText("name", "first");
    d.press("save");
    const uint32_t afterFirst = d.control->presetsRevision();
    CHECK(afterFirst > 0);                  // setup's rescan already counts as revision 1

    // Two mutations back-to-back, same second, and each must still read as a change.
    d.setText("name", "second");
    d.press("save");
    const uint32_t afterSecond = d.control->presetsRevision();
    CHECK(afterSecond > afterFirst);

    REQUIRE(d.control->deleteListRow(d.firstRowId()));
    CHECK(d.control->presetsRevision() > afterSecond);
}


// A pad holds one preset: a different name onto an occupied pad refuses and names the holder, while the holder saving over its own pad works.
TEST_CASE("ControlModule refuses to save a new preset onto an occupied pad") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    auto setU8 = [&](const char* name, uint8_t v) {
        auto& cs = d.control->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, name) == 0) { *static_cast<uint8_t*>(cs[i].ptr) = v; return; }
        FAIL("no control named ", name);
    };

    d.setCapture("Effects");
    d.setText("name", "holder");
    setU8("slot", 5);
    d.press("save");
    REQUIRE(d.control->listRowCount() == 1);

    // A different name aimed at the same pad: refused, nothing new created.
    d.setText("name", "intruder");
    setU8("slot", 5);
    d.press("save");
    CHECK(d.control->listRowCount() == 1);
    CHECK(std::string(d.status()).find("taken") != std::string::npos);

    // The holder itself saving onto its own pad is the overwrite flow.
    d.setText("name", "holder");
    setU8("slot", 5);
    d.press("save");
    CHECK(d.control->listRowCount() == 1);
    CHECK(!d.rowNamed("holder").empty());
}


// A deleted preset's pad goes dark: active-role slots refer to a name, so they are cleared, else a reused name inherits the lit state.
TEST_CASE("ControlModule stops showing a deleted preset as active") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setCapture("Effects");
    d.setText("name", "doomed");
    d.press("save");
    d.activate("doomed");
    REQUIRE(std::string(d.control->currentLook()) == "doomed");

    REQUIRE(d.control->deleteListRow(d.firstRowId()));
    CHECK(std::string(d.control->currentLook()).empty());
}

// A renamed preset keeps its lit pad: the active-role slot follows the new name rather than pointing at a name that no longer exists.
TEST_CASE("ControlModule keeps a renamed preset active under its new name") {
    Device d;
    auto* layer = d.add(d.layers, "Layer");
    d.add(layer, "NoiseEffect");

    d.setCapture("Effects");
    d.setText("name", "before");
    d.press("save");
    d.activate("before");
    REQUIRE(std::string(d.control->currentLook()) == "before");

    REQUIRE(d.control->setListRowField(d.firstRowId(), "name", "{\"value\":\"after\"}"));
    CHECK(std::string(d.control->currentLook()) == "after");
}

// The switch row comes first, above the encoders and faders, because control order is render order and a fader can only say on as 0 or 255.
TEST_CASE("ControlModule exposes eight switches, ahead of the encoders and faders") {
    Device d;

    auto& cs = d.control->controls();
    int firstSwitch = -1, firstEncoder = -1, firstFader = -1, switches = 0;
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (isSurfaceControl(cs[i].name, "switch")) {
            switches++;
            if (firstSwitch < 0) firstSwitch = i;
        } else if (isSurfaceControl(cs[i].name, "encoder") && firstEncoder < 0) {
            firstEncoder = i;
        } else if (isSurfaceControl(cs[i].name, "fader") && firstFader < 0) {
            firstFader = i;
        }
    }
    CHECK(switches == 8);
    REQUIRE(firstSwitch >= 0);
    REQUIRE(firstEncoder >= 0);
    REQUIRE(firstFader >= 0);
    CHECK(firstSwitch < firstEncoder);   // the row is the TOP row
    CHECK(firstEncoder < firstFader);

    // A switch is a BOOLEAN, so the UI renders a checkbox and a target gets a definite on/off rather than a threshold someone has to pick.
    bool found = false;
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "switch1") != 0) continue;
        CHECK(cs[i].type == mm::ControlType::Bool);
        found = true;
        break;
    }
    REQUIRE(found);   // else a rename makes this test pass by never running its check
}

// A surface mirrors ControlModule's state rather than owning any, so two can attach at once and stay in step.

namespace {
/// Records what a surface was told, so a test can assert the mirror's decisions rather than a wire.
struct RecordingSurface : mm::ControlSurface {
    struct Call { mm::SurfaceControl kind; uint8_t index; uint8_t value; };
    std::vector<Call> calls;
    void sendValue(mm::SurfaceControl kind, uint8_t index, uint8_t value) override {
        calls.push_back({kind, index, value});
    }
    int countFor(mm::SurfaceControl kind, uint8_t index) const {
        int n = 0;
        for (const auto& c : calls) if (c.kind == kind && c.index == index) n++;
        return n;
    }
    void clear() { calls.clear(); }
};

/// Set a fader the way any writer does, then run the mirror.
void setFader(Device& d, uint8_t index, uint8_t value) {
    auto& cs = d.control->controls();
    char name[16];
    std::snprintf(name, sizeof(name), "fader%u", static_cast<unsigned>(index + 1));
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, name) != 0) continue;
        *static_cast<uint8_t*>(cs[i].ptr) = value;
        return;
    }
}
}  // namespace

// A surface that attaches mid-show is seeded from the target, not the mirror's last value, so it is correct before anything changes.
TEST_CASE("attaching a surface seeds it with the current state") {
    Device d;
    REQUIRE(d.scheduler.setControl("Drivers", "brightness", "{\"value\":200}")
            == mm::Scheduler::SetControlResult::Ok);
    RecordingSurface s;
    d.control->addSurface(&s);
    CHECK(s.countFor(mm::SurfaceControl::Fader, 0) == 1);
    for (const auto& c : s.calls)
        if (c.kind == mm::SurfaceControl::Fader && c.index == 0) CHECK(c.value == 200);
    d.control->removeSurface(&s);
}

// The two-way half: a surface that only writes drifts, so switch1 follows Drivers.on and reads the rig's state at boot.
TEST_CASE("a switch follows the control it drives, including at startup") {
    Device d;
    RecordingSurface s;

    // Drivers.on is on by default, and switch1 (bound to it) starts false. Before the follow this disagreement survived forever: the surface said off while the rig was on.
    d.control->addSurface(&s);
    d.control->mirrorToSurfaces();

    auto& cs = d.control->controls();
    bool found = false;
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "switch1") != 0) continue;
        CHECK(*static_cast<bool*>(cs[i].ptr) == true);   // caught up to Drivers.on
        found = true;
        break;
    }
    REQUIRE(found);
    d.control->removeSurface(&s);
}

// The same for a fader, driven from the OTHER side. Turning brightness down in the web UI must move the fader that drives it, or the surface shows a value the rig is not running on.
TEST_CASE("a fader follows its target when something else moves it") {
    Device d;
    RecordingSurface s;
    d.control->addSurface(&s);
    d.control->mirrorToSurfaces();          // settle the startup catch-up
    s.clear();

    // Anything else writes the target: the web UI, a preset recall, an audio-reactive effect.
    REQUIRE(d.scheduler.setControl("Drivers", "brightness", "{\"value\":42}")
            == mm::Scheduler::SetControlResult::Ok);
    d.control->mirrorToSurfaces();

    // The surface was told, and its own fader now reads what the device is running on.
    CHECK(s.countFor(mm::SurfaceControl::Fader, 0) == 1);
    auto& cs = d.control->controls();
    for (uint8_t i = 0; i < cs.count(); i++)
        if (std::strcmp(cs[i].name, "fader1") == 0)
            CHECK(*static_cast<uint8_t*>(cs[i].ptr) == 42);
    d.control->removeSurface(&s);
}

// Only CHANGES go out. This is the first half of the echo guard: a value a surface just sent us already matches what we would send back, so it never bounces.
TEST_CASE("the mirror sends a control only when its value changed") {
    Device d;
    RecordingSurface s;
    d.control->addSurface(&s);
    s.clear();

    // The first mirror after an attach is not silent: the surface FOLLOWS its targets, and switch1's own default (off) has never met Drivers.on (on), so it corrects itself. Settle that, then assert the steady state, which is what this case is about.
    d.control->mirrorToSurfaces();
    s.clear();
    d.control->mirrorToSurfaces();
    CHECK(s.calls.empty());              // nothing moved, nothing sent

    setFader(d, 2, 128);
    d.control->mirrorToSurfaces();
    CHECK(s.countFor(mm::SurfaceControl::Fader, 2) == 1);

    s.clear();
    d.control->mirrorToSurfaces();
    CHECK(s.calls.empty());              // same value: silent
    d.control->removeSurface(&s);
}

// A hand on a control suppresses feedback so the device does not fight the user, and on release the control resyncs.
TEST_CASE("a touched control is not driven, and resyncs when released") {
    Device d;
    RecordingSurface s;
    d.control->addSurface(&s);
    s.clear();

    d.control->setTouched(mm::SurfaceControl::Fader, 3, true);
    setFader(d, 3, 90);
    d.control->mirrorToSurfaces();
    CHECK(s.countFor(mm::SurfaceControl::Fader, 3) == 0);   // hands off

    d.control->setTouched(mm::SurfaceControl::Fader, 3, false);
    d.control->mirrorToSurfaces();
    CHECK(s.countFor(mm::SurfaceControl::Fader, 3) == 1);   // and it catches up
    d.control->removeSurface(&s);
}

// An endless encoder reports movement, so a detent steps whatever it targets and the target's own type and bounds decide the result.
TEST_CASE("an encoder detent steps its target, which owns the value and its bounds") {
    Device d;
    // encoder1 targets Drivers.palette. Reading the TARGET, not the encoder: the encoder has no value to read, and a test asserting on one would be asserting the old contract.
    auto palette = [&] {
        auto& cs = d.drivers->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, "palette") == 0) return *static_cast<uint8_t*>(cs[i].ptr);
        return static_cast<uint8_t>(0);
    };
    uint8_t max = 0;
    {
        auto& cs = d.drivers->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, "palette") == 0) max = cs[i].max;
    }
    REQUIRE(max > 4);

    const uint8_t start = palette();
    d.control->applyEncoderDelta(0, 3);
    CHECK(palette() == start + 3);
    d.control->applyEncoderDelta(0, -2);
    CHECK(palette() == start + 1);

    // The bound is the control's, so the target stops at its range rather than wrapping. `max` on a Select or Palette is the option count, so the last index is one below.
    for (int i = 0; i < 200; i++) d.control->applyEncoderDelta(0, 5);
    CHECK(palette() == max - 1);
    for (int i = 0; i < 200; i++) d.control->applyEncoderDelta(0, -5);
    CHECK(palette() == 0);
}

// Switch 1 is the master on/off every driver honours. It sends a BOOL body, because parseBool reads `true`/`1` but not the 255 a byte path produces, which is how the OSC switches failed.
TEST_CASE("ControlModule switch 1 drives the global on/off") {
    Device d;
    auto driversOn = [&] {
        auto& cs = d.drivers->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, "on") == 0) return *static_cast<bool*>(cs[i].ptr);
        return false;
    };

    auto flip = [&](bool to) {
        auto& cs = d.control->controls();
        for (uint8_t i = 0; i < cs.count(); i++) {
            if (std::strcmp(cs[i].name, "switch1") != 0) continue;
            *static_cast<bool*>(cs[i].ptr) = to;
            d.control->onControlChanged("switch1");
            return;
        }
    };

    flip(false);
    CHECK_FALSE(driversOn());     // the rig goes dark from the surface
    flip(true);
    CHECK(driversOn());           // and comes back
}


// The display strip names what a knob selected, not its number. The name lives in the light domain, so this also pins the JsonSink::requestName seam end to end.
TEST_CASE("the display strip names what an encoder selected, not its number") {
    Device d;

    // Find the palette control's option count, so the test picks a valid index rather than assuming how many palettes ship.
    uint8_t paletteCount = 0;
    auto& dcs = d.drivers->controls();
    for (uint8_t i = 0; i < dcs.count(); i++)
        if (std::strcmp(dcs[i].name, "palette") == 0) paletteCount = dcs[i].max;
    REQUIRE(paletteCount > 1);

    auto& cs = d.control->controls();
    auto strip = [&]() -> const char* {
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, "display") == 0) return static_cast<const char*>(cs[i].ptr);
        return "";
    };

    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "encoder1") != 0) continue;
        *static_cast<uint8_t*>(cs[i].ptr) = 1;
        d.control->onControlChanged("encoder1");
    }

    // "palette <name>": what moved AND what it moved to, which fits now the strip is 28 cells. Not the bare number the fallback prints.
    const char* s = strip();
    INFO(s);
    CHECK(std::strstr(s, "palette ") == s);
    CHECK(std::strcmp(s, "palette 1") != 0);
    // A real name: letters, not just digits.
    bool hasLetter = false;
    for (const char* c = s; *c; c++) if ((*c | 32) >= 'a' && (*c | 32) <= 'z') hasLetter = true;
    CHECK(hasLetter);
}

// Surface bindings are assignments: a string per control, settable like any control, persisted, and reaching anything the REST API can set.
TEST_CASE("a surface control drives whatever it is assigned to") {
    Device d;
    auto palette = [&] {
        auto& cs = d.drivers->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, "palette") == 0) return *static_cast<uint8_t*>(cs[i].ptr);
        return static_cast<uint8_t>(0);
    };

    // fader2 ships unassigned, so moving it drives nothing.
    auto setFader = [&](const char* name, uint8_t v) {
        auto& cs = d.control->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, name) == 0) {
                *static_cast<uint8_t*>(cs[i].ptr) = v;
                d.control->onControlChanged(name);
            }
    };
    const uint8_t before = palette();
    setFader("fader2", 9);
    CHECK(palette() == before);

    // Assign it, and the same move lands on the palette.
    auto assign = [&](const char* which, const char* target) {
        auto& cs = d.control->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, which) == 0) {
                std::snprintf(static_cast<char*>(cs[i].ptr), 40, "%s", target);
                d.control->onControlChanged(which);
            }
    };
    assign("fader2Target", "Drivers.palette");
    setFader("fader2", 9);
    CHECK(palette() == 9);

    // Clearing it stops the binding: the control stays, and drives nothing.
    assign("fader2Target", "");
    setFader("fader2", 21);
    CHECK(palette() == 9);            // unchanged by the cleared fader
}

TEST_CASE("a surface control follows the control it drives, so the two never disagree") {
    // Two-way: something else moving the target (web UI, MQTT, preset recall) must move the fader, or the surface shows a value the rig is not running.
    Device d;
    auto& dcs = d.drivers->controls();
    uint8_t* palettePtr = nullptr;
    for (uint8_t i = 0; i < dcs.count(); i++)
        if (std::strcmp(dcs[i].name, "palette") == 0) palettePtr = static_cast<uint8_t*>(dcs[i].ptr);
    REQUIRE(palettePtr != nullptr);

    auto faderValue = [&](const char* name) {
        auto& cs = d.control->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, name) == 0) return *static_cast<uint8_t*>(cs[i].ptr);
        return static_cast<uint8_t>(0);
    };
    auto assign = [&](const char* which, const char* target) {
        auto& cs = d.control->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, which) == 0) {
                std::snprintf(static_cast<char*>(cs[i].ptr), 40, "%s", target);
                d.control->onControlChanged(which);
            }
    };

    assign("fader4Target", "Drivers.palette");
    *palettePtr = 17;                 // moved from somewhere that is not the surface
    d.control->tick20ms();            // the sampling tick that mirrors and follows
    CHECK(faderValue("fader4") == 17);
}

// A button treats every write as a press, so a switch driving one fires on the way down only, or one press of a pad button fires twice.
TEST_CASE("a switch drives a button on the press alone") {
    struct ButtonGame : mm::MoonModule {
        int presses = 0;
        void defineControls() override { controls_.addButton("fire"); }
        void onControlChanged(const char* name) override { if (std::strcmp(name, "fire") == 0) presses++; }
    };
    Device d;
    auto* game = new ButtonGame();
    game->setName("Game");
    d.scheduler.addModule(game);
    game->defineControls();

    auto set = [&](const char* which, auto apply) {
        auto& cs = d.control->controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (std::strcmp(cs[i].name, which) == 0) { apply(cs[i].ptr); d.control->onControlChanged(which); }
    };
    set("switch1Target", [](void* p) { std::snprintf(static_cast<char*>(p), 40, "%s", "Game.fire"); });
    set("switch1", [](void* p) { *static_cast<bool*>(p) = true; });
    CHECK(game->presses == 1);
    set("switch1", [](void* p) { *static_cast<bool*>(p) = false; });
    CHECK(game->presses == 1);   // the release drives nothing
}

// A written surface control reaches the attached surfaces at once, not at the next once-a-second pass, so a forwarded move is not a second late.
TEST_CASE("a written fader reaches the surfaces without waiting for the mirror pass") {
    Device d;
    RecordingSurface s;
    d.control->addSurface(&s);
    s.clear();
    REQUIRE(d.scheduler.setControl("Control", "fader3", "{\"value\":77}") == mm::Scheduler::SetControlResult::Ok);
    REQUIRE(s.countFor(mm::SurfaceControl::Fader, 2) == 1);   // no mirrorToSurfaces() call in between
    for (const auto& c : s.calls)
        if (c.kind == mm::SurfaceControl::Fader && c.index == 2) CHECK(c.value == 77);
    d.control->removeSurface(&s);
}

// A target changing underneath, such as a self-playing game moving its own paddle, reaches the surfaces on the next 20 ms tick.
TEST_CASE("a target changing underneath reaches the surfaces within a 20 ms tick") {
    Device d;
    RecordingSurface s;
    d.control->addSurface(&s);
    s.clear();
    // fader1 follows Drivers.brightness; write the target directly, the way an effect writes its own control.
    auto& cs = d.drivers->controls();
    for (uint8_t i = 0; i < cs.count(); i++)
        if (std::strcmp(cs[i].name, "brightness") == 0) *static_cast<uint8_t*>(cs[i].ptr) = 123;
    d.control->tick20ms();
    REQUIRE(s.countFor(mm::SurfaceControl::Fader, 0) == 1);
    for (const auto& c : s.calls)
        if (c.kind == mm::SurfaceControl::Fader && c.index == 0) CHECK(c.value == 123);
    d.control->removeSurface(&s);
}

namespace {
/// A surface control's current byte, read the way a surface sees it.
uint8_t surfaceValue(Device& d, const char* name) {
    uint8_t v = 0;
    d.scheduler.getControl("Control", name, v);
    return v;
}
}  // namespace

// Mackie Control: a fader is 14-bit pitch bend on its own channel, landing on the surface's fader of that number.
TEST_CASE("a MIDI desk's fader moves the surface's fader") {
    Device d;
    mm::MidiService midi;
    const uint8_t top[3] = {0xE2, 0x7F, 0x7F}, middle[3] = {0xE2, 0x00, 0x40}, bottom[3] = {0xE2, 0x00, 0x00};
    midi.decode(top, 3);
    CHECK(surfaceValue(d, "fader3") == 255);
    midi.decode(middle, 3);
    CHECK(surfaceValue(d, "fader3") == 128);
    midi.decode(bottom, 3);
    CHECK(surfaceValue(d, "fader3") == 0);
}

// A knob turn is relative: bit 6 is the direction and the low six bits the steps.
TEST_CASE("a MIDI desk's knob turns the surface's encoder by its steps") {
    Device d;
    mm::MidiService midi;
    const uint8_t start = surfaceValue(d, "encoder1");
    const uint8_t up3[3] = {0xB0, 0x10, 0x03}, down1[3] = {0xB0, 0x10, 0x41};
    midi.decode(up3, 3);
    midi.decode(down1, 3);
    CHECK(surfaceValue(d, "encoder1") == start + 2);
}

// A channel's SELECT button is momentary, so each press flips its switch and the release does nothing.
TEST_CASE("a MIDI desk's SELECT button flips the surface's switch") {
    Device d;
    mm::MidiService midi;
    const uint8_t before = surfaceValue(d, "switch2");
    const uint8_t press[3] = {0x90, 0x19, 0x7F}, release[3] = {0x90, 0x19, 0x00};
    midi.decode(press, 3);
    midi.decode(release, 3);
    CHECK(surfaceValue(d, "switch2") != before);
    midi.decode(press, 3);
    CHECK(surfaceValue(d, "switch2") == before);
}

// A hand on a fader holds the surface's push for that fader back, so a motor never fights it, and letting go releases it.
TEST_CASE("a MIDI desk's fader touch holds the surface's push for that fader") {
    Device d;
    RecordingSurface s;
    d.control->addSurface(&s);
    mm::MidiService midi;
    const uint8_t touch[3] = {0x90, 0x68, 0x7F}, letGo[3] = {0x90, 0x68, 0x00};
    midi.decode(touch, 3);
    s.clear();
    REQUIRE(d.scheduler.setControl("Control", "fader1", "{\"value\":42}") == mm::Scheduler::SetControlResult::Ok);
    CHECK(s.countFor(mm::SurfaceControl::Fader, 0) == 0);
    midi.decode(letGo, 3);
    d.control->tick20ms();
    CHECK(s.countFor(mm::SurfaceControl::Fader, 0) == 1);
    d.control->removeSurface(&s);
}

// The browser writes a batch, each message in hex, and the service decodes it as it is written.
TEST_CASE("a MIDI batch written by the browser is decoded on the next tick") {
    Device d;
    auto* midi = new mm::MidiService();
    midi->setName("Midi");
    d.scheduler.addModule(midi);
    midi->defineControls();
    REQUIRE(d.scheduler.setControl("Midi", "midi", "{\"value\":\"e47f7f\"}") == mm::Scheduler::SetControlResult::Ok);
    CHECK(surfaceValue(d, "fader5") == 255);
}

// An echo of a value the target already holds is not written again, so a self-playing game never reads it as a player.
TEST_CASE("a surface does not rewrite a target that already holds the value") {
    struct Game : mm::MoonModule {
        uint8_t level = 50;
        int writes = 0;
        void defineControls() override { controls_.addControl("level", level, 0, 255); }
        void onControlChanged(const char* name) override { if (std::strcmp(name, "level") == 0) writes++; }
    };
    Device d;
    auto* game = new Game();
    game->setName("Game");
    d.scheduler.addModule(game);
    game->defineControls();
    REQUIRE(d.scheduler.setControl("Control", "fader6Target", "{\"value\":\"Game.level\"}") == mm::Scheduler::SetControlResult::Ok);
    REQUIRE(d.scheduler.setControl("Control", "fader6", "{\"value\":50}") == mm::Scheduler::SetControlResult::Ok);
    CHECK(game->writes == 0);   // the echo of what it holds
    REQUIRE(d.scheduler.setControl("Control", "fader6", "{\"value\":51}") == mm::Scheduler::SetControlResult::Ok);
    CHECK(game->writes == 1);   // a real move still lands
}

