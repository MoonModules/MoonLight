/// @module MoonModule
/// @also ModuleFactory, Scheduler

/// The contract every registered module type keeps: a module that does not run costs flash and nothing else.
///
/// Each type is placed where its role belongs, as a device does, run for a few frames, then disabled the way the UI does.
///
/// Each list below holds the types that break its check, with the reason: a listed type that passes fails the test, so a list only shrinks. A new module never joins one.

#include "doctest.h"
#include "core/module/Scheduler.h"
#include "core/module/StateDocument.h"
#include "core/util/ModuleFactory.h"
#include "module_types.h"
#include "platform/platform.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// `hostDependent` marks a type whose result depends on the host: it is listed because it breaks where it is measurable, and passing elsewhere is no reason to leave the list.
struct Known { const char* type; const char* reason; bool hostDependent = false; };

// Types that cannot be placed on a desktop run, each skipped for the reason given.
const std::vector<Known> kNotPlaced = {
};

// Types still holding runtime memory after a disable.
const std::vector<Known> kKeepsMemory = {
    {"Effects", "its Layer keeps the pixel buffer while disabled"},
    {"Layer", "its pixel buffer_ is a plain Buffer that release() does not free"},
    {"Drivers", "outputBuffer_ is a plain Buffer that release() does not free"},
    {"NdiDriver", "release() is overridden without chaining to the base, so the rgb_ and lightScratch_ scratch buffers stay, where the NDI library loads and the buffers exist", true},
};

// Types whose enabled flag does not gate them, which the lifecycle then cannot switch off.
const std::vector<Known> kIgnoresEnabled = {
    {"HttpServerModule", "disabling the server would leave no way to re-enable it"},
    {"SystemModule", "the diagnostics keep ticking, since hiding uptime and heap serves nobody"},
    {"FirmwareUpdateModule", "a fixed module that keeps reporting whatever the toggle says"},
    {"MoonCloudModule", "the children carry the real consent, so disabling the container is ambiguous"},
    {"ImprovProvisioningModule", "keeps listening, as the other fixed services do"},
    {"NetworkModule", "the cascade keeps ticking or the device drops off the network"},
    {"EthernetModule", "the interface stays configured or the device drops off the network"},
    {"WiFiModule", "the station stays configured or the device drops off the network"},
    {"AccessPointModule", "the access point stays configured or a device that loses WiFi is unreachable"},
    {"FilesystemModule", "keeps flushing, so disabling it cannot lose changes"},
};

// Types holding an allocation made outside a ScratchBuffer once disabled, beyond the module objects themselves.
const std::vector<Known> kHoldsAllocation = {
    {"Layer", "its pixel buffer_ is a plain Buffer that release() does not free"},
    {"RandomMapModifier", "the permutation is taken with a raw platform::alloc and freed only in the destructor"},
    {"NetworkSendDriver", "corrected_ is a plain Buffer, outside the scratch list that release() frees"},
    {"PanelCardDriver", "corrected_ is a plain Buffer, outside the scratch list that release() frees"},
    {"NdiDriver", "release() is overridden without chaining to the base, so its scratch buffers stay, where the NDI library loads and the buffers exist", true},
};

// Types leaving an allocation behind once removed.
const std::vector<Known> kLeavesBehind = {
};

// Types over the inline budget.
const std::vector<Known> kTooBig = {
    {"Drivers", "the embedded palette script module and the palette catalog tables sit inline"},
    {"RadialSpectrumEffect", "the beats_ and fade_ history arrays of 128 entries each sit inline"},
    {"MoonLiveEffect", "the MoonLiveScript member holds the script state inline"},
    {"NetworkSendDriver", "two 32-entry destination tables (dest and dest_) sit inline"},
    {"RtspDriver", "the request, SDP and reply buffers (512 and 1024 bytes) sit inline"},
    {"PanelCardDriver", "the packet_ frame buffer sits inline, sized to the largest card frame"},
    {"ParallelLedDriver", "the pin and strand text fields and the in-flight state sit inline"},
    {"HttpServerModule", "the eight WebSocket client connections sit inline"},
    {"ControlModule", "the surface, touch and resend tables sit inline"},
    {"AudioService", "the 512-sample block, its windowed copy and the magnitude table sit inline"},
    {"PinsModule", "the 64-claim snapshot table sits inline"},
    {"FseqPlayerEffect", "its playlist of tracks sits inline, close enough to the budget that a host's layout decides which side it lands on", true},
};

// Two inline kilobytes, since nine in ten modules fit, so a larger table or buffer inside the object stands out as the exception.
constexpr size_t kInlineBudget = 2048;

struct Row {
    std::string type;
    bool placed = false;
    bool respects = false;
    size_t dynamicAfter = 0;
    size_t dynamicBefore = 0;
    size_t size = 0;
    long heldAfterDisable = 0;   // allocated bytes beyond the module objects, once disabled
    long leftAfterRemove = 0;    // allocated bytes beyond where the tree stood before the add, once removed
};

size_t objectBytes(const mm::MoonModule& m) {
    size_t n = m.classSize();
    for (uint8_t i = 0; i < m.childCount(); i++) n += objectBytes(*m.child(i));
    return n;
}

size_t dynamicTotal(const mm::MoonModule& m) {
    size_t n = m.dynamicBytes();
    for (uint8_t i = 0; i < m.childCount(); i++) n += dynamicTotal(*m.child(i));
    return n;
}

void applyDoc(mm::Scheduler& s, const std::string& doc) {
    mm::applyStateDocument(s, doc.c_str());
    s.prepareTree();
}

// Place one type where its role belongs, run frames, disable it the way the UI does, run again.
Row place(uint8_t index) {
    Row row;
    const char* type = mm::ModuleFactory::typeName(index);
    row.type = type;
    const mm::ModuleRole role = mm::ModuleFactory::typeRole(index);

    const size_t start = mm::platform::allocatedBytes();
    mm::Scheduler s;
    for (const char* top : {"Layouts", "Effects", "Drivers", "Services"}) s.addModule(mm::createTopLevel(top));
    size_t beforeAdd = mm::platform::allocatedBytes();
    mm::MoonModule* standalone = nullptr;
    if (role == mm::ModuleRole::Generic && !s.topOfType(type)) {
        standalone = mm::createTopLevel(type);
        if (standalone) s.addModule(standalone);
    }
    s.setup();
    applyDoc(s, R"({"Layouts":{"Grid":{"type":"GridLayout","width":16,"height":16}}})");
    applyDoc(s, R"({"Effects":{"Layer":{"type":"Layer"}}})");

    beforeAdd = mm::platform::allocatedBytes();
    const bool byDocument = role != mm::ModuleRole::Generic;
    const std::string member = std::string("\"X\":{\"type\":\"") + type + "\"}";
    switch (role) {
        case mm::ModuleRole::Effect:
        case mm::ModuleRole::Modifier: applyDoc(s, "{\"Effects\":{\"Layer\":{" + member + "}}}"); break;
        case mm::ModuleRole::Layout:   applyDoc(s, "{\"Layouts\":{" + member + "}}"); break;
        case mm::ModuleRole::Driver:   applyDoc(s, "{\"Drivers\":{" + member + "}}"); break;
        case mm::ModuleRole::Service:  applyDoc(s, "{\"Services\":{" + member + "}}"); break;
        case mm::ModuleRole::Layer:    applyDoc(s, "{\"Effects\":{" + member + "}}"); break;
        case mm::ModuleRole::Generic:  break;
    }
    mm::MoonModule* m = role == mm::ModuleRole::Generic ? (standalone ? standalone : s.topOfType(type)) : s.firstByName("X");
    if (m) {
        row.placed = true;
        row.respects = m->respectsEnabled();
        row.size = m->classSize();
        uint32_t now = 1000;
        for (int i = 0; i < 4; i++) { mm::platform::setTestNowMs(now); s.tick(); now += 1100; }
        row.dynamicBefore = dynamicTotal(*m);
        m->setEnabled(false);
        s.prepareTree();
        for (int i = 0; i < 3; i++) { mm::platform::setTestNowMs(now); s.tick(); now += 1100; }
        row.dynamicAfter = dynamicTotal(*m);
        // Measured from before the add: a module added by document less its own objects, a standalone one, already in that baseline with its setup allocations, by what it holds beyond them. Others have no add.
        if (byDocument || standalone)
            row.heldAfterDisable = static_cast<long>(mm::platform::allocatedBytes()) - static_cast<long>(beforeAdd) - (byDocument ? static_cast<long>(objectBytes(*m)) : 0L);
        // Removed the way the API's DELETE removes it, a top-level module leaving with its tree; a module the API refuses to remove (PreviewDriver) is out of this check.
        if (byDocument && m->userEditable()) {
            const char* parent = role == mm::ModuleRole::Layout ? "Layouts" : role == mm::ModuleRole::Driver ? "Drivers" : role == mm::ModuleRole::Service ? "Services" : role == mm::ModuleRole::Layer ? "Effects" : nullptr;
            const std::string path = parent ? std::string("{\"") + parent + "\":{\"X\":null}}" : "{\"Effects\":{\"Layer\":{\"X\":null}}}";
            applyDoc(s, path);
            mm::platform::setTestNowMs(now); s.tick();
            row.leftAfterRemove = static_cast<long>(mm::platform::allocatedBytes()) - static_cast<long>(beforeAdd);
        }
    }
    mm::platform::setTestNowMs(0);
    s.release();
    if (!byDocument && row.placed) row.leftAfterRemove = static_cast<long>(mm::platform::allocatedBytes()) - static_cast<long>(start);
    return row;
}

const std::vector<Row>& survey() {
    static const std::vector<Row> rows = [] {
        mm::registerModuleTypes();
        // No socket is opened for real: a server module binds, fails and stays idle.
        mm::platform::setTestBindFails(true);
        std::vector<Row> r;
        for (uint8_t i = 0; i < mm::ModuleFactory::typeCount(); i++) r.push_back(place(i));
        mm::platform::setTestBindFails(false);
        return r;
    }();
    return rows;
}

const Known* find(const std::vector<Known>& list, const std::string& type) {
    for (const Known& k : list) if (type == k.type) return &k;
    return nullptr;
}

// One check over every placed type: a failing type must be listed, a listed type must fail.
template <typename Pass>
void enforce(const std::vector<Known>& list, const char* what, Pass pass) {
    for (const Row& r : survey()) {
        if (!r.placed) continue;
        const bool ok = pass(r);
        const Known* k = find(list, r.type);
        if (!ok && !k) { CHECK_MESSAGE(false, r.type << " " << std::string(what)); }
        if (ok && k && !k->hostDependent) { CHECK_MESSAGE(false, r.type << " passes now, so it leaves its list"); }
    }
    for (const Known& k : list) {
        bool registered = false;
        for (const Row& r : survey()) registered = registered || r.type == k.type;
        CHECK_MESSAGE(registered, k.type << " is listed but not registered");
    }
}

}  // namespace

// Every registered type is placed and run, or named in the not-placed list with its reason.
TEST_CASE("every registered module type is placed or listed as not placeable") {
    // The survey registers the types the first time it runs, so it runs before the count is read: a compiler may evaluate either side of the comparison first.
    const std::vector<Row>& rows = survey();
    CHECK(rows.size() == mm::ModuleFactory::typeCount());
    CHECK(mm::ModuleFactory::typeCount() < 255);
    for (const Row& r : rows) {
        const Known* k = find(kNotPlaced, r.type);
        if (!r.placed && !k) { CHECK_MESSAGE(false, r.type << " could not be placed"); }
        if (r.placed && k) { CHECK_MESSAGE(false, r.type << " is placed now, so it leaves the list"); }
    }
}

// A disabled module holds no allocation beyond its own objects, however it was taken.
TEST_CASE("a disabled module holds nothing beyond its objects") {
    enforce(kHoldsAllocation, "holds an allocation beyond its objects after a disable", [](const Row& r) { return !r.respects || r.heldAfterDisable <= 0; });
}

// A removed module leaves nothing behind, a global taken on first use included.
TEST_CASE("a removed module leaves nothing behind") {
    enforce(kLeavesBehind, "leaves an allocation behind after removal", [](const Row& r) { return r.leftAfterRemove <= 0; });
}

// A disabled module holds no runtime memory, itself or in any child.
TEST_CASE("a disabled module holds no dynamic memory") {
    // A module that ignores the flag cannot be disabled, so this check does not apply to it.
    enforce(kKeepsMemory, "holds dynamic memory after a disable", [](const Row& r) { return !r.respects || r.dynamicAfter == 0; });
}

// The enabled flag gates every module, so the lifecycle can switch it off.
TEST_CASE("every module honors the enabled flag") {
    enforce(kIgnoresEnabled, "ignores the enabled flag", [](const Row& r) { return r.respects; });
}

// A module's object stays within the inline budget, with larger tables taken in prepare().
TEST_CASE("every module object stays within the inline budget") {
    enforce(kTooBig, "is over the inline budget", [](const Row& r) { return r.size <= kInlineBudget; });
}
