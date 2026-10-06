/// @module MoonStatsReport

#include "doctest.h"
#include "core/system/MoonStatsModule.h"
#include "core/services/AudioService.h"

#include <cstring>
#include <string>

#include "core/system/SystemModule.h"
#include "light/drivers/PreviewDriver.h"   // the one type the report excludes as boot wiring
#include "light/moonlive/MoonLiveEffect.h"   // kEffectPick: the picker the scripted fake offers

namespace {

/// A module carrying exactly the controls a real device would expose that MUST NEVER be reported: the identifying ones (device name, MAC), the secret ones (SSID, password), and free text the user typed. Built to look as much like the real System module as possible, because a builder that walked the tree rather than naming its fields would happily emit all of these.
class LeakyModule : public mm::MoonModule {
public:
    LeakyModule() { setName("System"); }

    void defineControls() override {
        std::strncpy(deviceName_, "ewoud-livingroom", sizeof(deviceName_) - 1);
        std::strncpy(mac_, "A4:CF:12:9B:33:07", sizeof(mac_) - 1);
        std::strncpy(ssid_, "Travelrouter", sizeof(ssid_) - 1);
        std::strncpy(password_, "hunter2-secret", sizeof(password_) - 1);
        std::strncpy(note_, "my bedroom wall, do not touch", sizeof(note_) - 1);
        std::strncpy(chip_, "ESP32-S3", sizeof(chip_) - 1);
        std::strncpy(flash_, "16MB", sizeof(flash_) - 1);

        controls_.addText("deviceName", deviceName_, sizeof(deviceName_));
        controls_.addReadOnly("mac", mac_, sizeof(mac_));
        controls_.addText("ssid", ssid_, sizeof(ssid_));
        controls_.addPassword("password", password_, sizeof(password_));
        controls_.addText("note", note_, sizeof(note_));
        controls_.addReadOnly("chip", chip_, sizeof(chip_));
        controls_.addReadOnly("flash", flash_, sizeof(flash_));
    }

private:
    char deviceName_[32] = {};
    char mac_[24] = {};
    char ssid_[32] = {};
    char password_[32] = {};
    char note_[40] = {};
    char chip_[16] = {};
    char flash_[8] = {};
};

/// A scripted module, the shape MoonLiveEffect and friends have: a `script` FilePath control holding a file NAME, plus whatever that script declared.
class ScriptedModule : public mm::MoonModule {
public:
    ScriptedModule(const char* name, const char* script, mm::ModuleRole role)
        : role_(role) {
        setName(name);
        std::snprintf(script_, sizeof(script_), "%s", script);
    }

    mm::ModuleRole role() const MM_NONBLOCKING override { return role_; }

    void defineControls() override {
        controls_.addFilePath("script", script_, sizeof(script_), mm::moonlive::kEffectPick);
    }

private:
    mm::ModuleRole role_;
    char script_[48] = {};
};

/// The report for a tree holding one scripted module.
std::string scriptedReport(const char* script, mm::ModuleRole role = mm::ModuleRole::Effect) {
    ScriptedModule mod("MoonLive", script, role);
    mod.defineControls();
    mm::MoonModule* tree[] = {&mod};
    mm::JsonSink sink;
    mm::buildMoonStatsReport(sink, tree, 1, mm::MoonStatsEvent::Install, nullptr, "4.0.0", nullptr);
    return std::string(sink.data(), sink.size());
}

std::string report(mm::MoonStatsEvent event = mm::MoonStatsEvent::Install,
                   const char* id = nullptr,
                   const char* version = "4.0.0",
                   const char* previous = nullptr) {
    LeakyModule sys;
    sys.defineControls();
    mm::MoonModule* tree[] = {&sys};
    mm::JsonSink sink;
    mm::buildMoonStatsReport(sink, tree, 1, event, id, version, previous);
    return std::string(sink.data(), sink.size());
}

}  // namespace

/// The report never carries anything that identifies the person or their network, however much of it the module tree holds.
///
/// This is the privacy policy made executable. The policy promises no device name, no network addresses, no credentials and no free text the user typed, and the tree here holds all four sitting beside the hardware fields that ARE reported. A builder that emitted what it found rather than naming each field would fail this the first time it ran.
TEST_CASE("the usage report cannot carry identifying or secret values") {
    const std::string json = report();

    // The VALUES, which is what would actually harm someone.
    CHECK(json.find("ewoud-livingroom") == std::string::npos);
    CHECK(json.find("A4:CF:12:9B:33:07") == std::string::npos);
    CHECK(json.find("Travelrouter") == std::string::npos);
    CHECK(json.find("hunter2-secret") == std::string::npos);
    CHECK(json.find("my bedroom wall") == std::string::npos);

    // The KEYS, so a later refactor cannot reintroduce the field with an empty value and look harmless while the next change fills it in.
    CHECK(json.find("deviceName") == std::string::npos);
    CHECK(json.find("\"mac\"") == std::string::npos);
    CHECK(json.find("ssid") == std::string::npos);
    CHECK(json.find("password") == std::string::npos);
    CHECK(json.find("note") == std::string::npos);
}

/// The hardware facts the report exists for do arrive, so the test above is not passing merely because the builder emits nothing.
TEST_CASE("the usage report carries the hardware facts it exists to collect") {
    const std::string json = report();
    CHECK(json.find("ESP32-S3") != std::string::npos);
    CHECK(json.find("16MB") != std::string::npos);
    CHECK(json.find("\"chip\"") != std::string::npos);
    CHECK(json.find("\"flash\"") != std::string::npos);
}

/// An install and an upgrade are told apart by the report itself, with no identifier involved: a previous version present means the firmware changed under an existing install.
TEST_CASE("an upgrade is distinguished from a fresh install by the previous version") {
    const std::string fresh = report(mm::MoonStatsEvent::Install, nullptr, "4.0.0", nullptr);
    CHECK(fresh.find("\"event\":\"install\"") != std::string::npos);
    CHECK(fresh.find("previousVersion") == std::string::npos);

    const std::string upgraded = report(mm::MoonStatsEvent::Upgrade, nullptr, "4.1.0", "4.0.0");
    CHECK(upgraded.find("\"event\":\"upgrade\"") != std::string::npos);
    CHECK(upgraded.find("\"previousVersion\":\"4.0.0\"") != std::string::npos);
}

/// The button's event. Install and Upgrade are decided by a version comparison, which cannot see a setup that changed without one.
/// Someone who reported a bare board and then wired up the fixtures they actually run. Distinct from the other two so the install count stays a count of installs.
TEST_CASE("a user-triggered refresh is its own event, carrying the same payload") {
    const std::string refreshed = report(mm::MoonStatsEvent::Refresh, nullptr, "4.0.0", nullptr);
    CHECK(refreshed.find("\"event\":\"refresh\"") != std::string::npos);
    // Same shape as any other report: the button re-sends, it does not send something smaller.
    CHECK(refreshed.find("\"chip\"") != std::string::npos);
    CHECK(refreshed.find("\"version\":\"4.0.0\"") != std::string::npos);
    // And it is none of the other two, so a legend cannot show it as an install.
    CHECK(refreshed.find("\"event\":\"install\"") == std::string::npos);
    CHECK(refreshed.find("\"event\":\"upgrade\"") == std::string::npos);
}

/// A refresh carries no previousVersion. The server reads that field's PRESENCE as what makes a row an upgrade, and the value it would carry (`reportedVersion`) is non-empty whenever the button is pressed: sending it would report every refresh as an upgrade from the version already running. Caught by CodeRabbit on PR #104.
TEST_CASE("a refresh names no previous version, so it cannot read as an upgrade") {
    // The GUARD is in MoonStatsModule::sendReport, which passes previousVersion() only on Upgrade: the value it would otherwise carry is `reportedVersion`, non-empty whenever the button is pressed, and the server reads that field's PRESENCE as what makes a row an upgrade. So a refresh would have reported as an upgrade from the version already running.
    //
    // This builder is a pure function over its arguments and rightly emits whatever it is handed, so passing a previous version here WOULD produce one. What it pins is the other half: with no predecessor supplied, a refresh carries none, and the field never appears by itself.
    const std::string refreshed = report(mm::MoonStatsEvent::Refresh, nullptr, "4.0.0", nullptr);
    CHECK(refreshed.find("\"event\":\"refresh\"") != std::string::npos);
    CHECK(refreshed.find("previousVersion") == std::string::npos);

    // An upgrade carries it: that is the one event the field describes.
    const std::string upgraded = report(mm::MoonStatsEvent::Upgrade, nullptr, "4.1.0", "4.0.0");
    CHECK(upgraded.find("\"previousVersion\":\"4.0.0\"") != std::string::npos);
}

/// A failed BUTTON press must not consume the automatic report. Pressing it before the install report has gone out, and having the send fail, used to mark the version reported anyway: the install was then never counted, and the user had been told the press failed. The automatic path still marks either way, because nobody is waiting for it.
TEST_CASE("a refresh that did not send leaves the automatic report still due") {
    // Documents the rule the code encodes (MoonStatsModule::sendReport): the mark is conditional on Refresh, unconditional otherwise. A build asserting it end to end needs a server.
    CHECK(mm::MoonStatsEvent::Refresh != mm::MoonStatsEvent::Install);
    CHECK(mm::MoonStatsEvent::Refresh != mm::MoonStatsEvent::Upgrade);
}

/// A report built without consent carries no installation id at all, rather than an empty or placeholder one: nothing is generated until the user says yes.
TEST_CASE("no installation id appears until one is supplied") {
    CHECK(report().find("installationId") == std::string::npos);

    const std::string withId = report(mm::MoonStatsEvent::Install,
                                      "66b1706d30ff5c0fb1c6fdd7f6fe1151");
    CHECK(withId.find("\"installationId\":\"66b1706d30ff5c0fb1c6fdd7f6fe1151\"")
          != std::string::npos);
}

/// Memory and light count ride the report as RAW numbers, for the server to bucket into ranges.
///
/// Nothing pinned them, and the worker's own `clean()` drops anything that is not a string unless a field has a branch of its own.
/// Exactly the regression that stored three zeros for every device.
TEST_CASE("the report carries memory and light count as numbers") {
    mm::SystemModule system;
    system.setName("System");

    mm::MoonModule* tree[] = {&system};
    mm::JsonSink sink;
    mm::buildMoonStatsReport(sink, tree, 1, mm::MoonStatsEvent::Install,
                             nullptr, "1.0.0", nullptr, 256, 282152, 84788);
    const std::string json = sink.data();

    CHECK(json.find("\"lightCount\":256") != std::string::npos);
    // Unquoted: a JSON number, not a string, which is what the server's numeric branch accepts.
    CHECK(json.find("\"lightCount\":\"") == std::string::npos);
    // The VALUES, unquoted: the server's numeric branch accepts a JSON number and its generic string test drops anything else, which is how three zeros were stored for every device.
    CHECK(json.find("\"totalHeap\":282152") != std::string::npos);
    CHECK(json.find("\"freeHeap\":84788") != std::string::npos);
}

/// The report names what the user ADDED, not the boot tree every device shares.
///
/// Counting main.cpp's wired modules made every slice read "2 of 2 devices", which says only that both booted. What varies between installations is what someone chose to run, so a wired module is skipped while its children are still walked: a user's effect hangs under a wired parent.
TEST_CASE("the report names modules by ROLE, not by how they were wired") {
    // A plain container. NOT wired by code, so only the ROLE rule excludes it: under the older isWiredByCode() test this one would have been reported.
    mm::MoonModule container;
    container.setName("Container");

    // Wired by code AND a real role. The mirror case, reported under the role rule and dropped under the old one. Together these two fail if the filter ever switches back.
    mm::AudioService added;
    added.setName("SomeService");
    added.markWiredByCode();

    mm::AudioService off;
    off.setName("Disabled");
    off.setEnabled(false);

    mm::MoonModule* tree[] = {&container, &added, &off};
    mm::JsonSink sink;
    mm::buildMoonStatsReport(sink, tree, 3, mm::MoonStatsEvent::Install,
                             nullptr, "1.0.0", nullptr);
    const std::string json = sink.data();

    CHECK(json.find("service:SomeService") != std::string::npos);   // a real role: reported
    CHECK(json.find("Container") == std::string::npos);             // generic: a structural container
    CHECK(json.find("Disabled") == std::string::npos);              // switched off
}

/// A second instance of a layout must not become a second SLICE. Scheduler uniquifies an instance name (`Ring`, `Ring-2`, `Ring-3`), and a user may rename a module to anything, so reporting `name()` counted one person's three rings as three layouts and would have sent whatever someone typed. The TYPE is the fixed vocabulary: displayNameFor turns the factory key into the same label the UI shows, so all three report `layout:Ring`.
TEST_CASE("a module reports its type, not the instance name a user sees") {
    mm::AudioService first;
    first.setName("Audio");
    first.setTypeName("AudioService");

    // The second instance, as Scheduler would name it.
    mm::AudioService second;
    second.setName("Audio-2");
    second.setTypeName("AudioService");

    // And one the user renamed outright: the report must carry none of that text.
    mm::AudioService renamed;
    renamed.setName("Ewoud bedroom");
    renamed.setTypeName("AudioService");

    mm::MoonModule* tree[] = {&first, &second, &renamed};
    mm::JsonSink sink;
    mm::buildMoonStatsReport(sink, tree, 3, mm::MoonStatsEvent::Install,
                             nullptr, "1.0.0", nullptr);
    const std::string json = sink.data();

    CHECK(json.find("service:Audio") != std::string::npos);
    CHECK(json.find("Audio-2") == std::string::npos);        // the suffix never reaches the report
    CHECK(json.find("Ewoud bedroom") == std::string::npos);  // nor does a name somebody typed
}

/// The preview driver is on every device, so counting it said only that a device booted. Excluded by TYPE, not by isWiredByCode(): that flag marks only children, and filtering on it once reported every top-level module as `generic:System`. The case above pins that distinction.
TEST_CASE("the preview driver is boot wiring, so it is never reported") {
    mm::PreviewDriver preview;
    preview.setName("Preview");
    preview.setTypeName("PreviewDriver");

    // A driver the user actually added, to prove the exclusion is by type and not by role.
    mm::AudioService chosen;
    chosen.setName("Audio");
    chosen.setTypeName("AudioService");

    mm::MoonModule* tree[] = {&preview, &chosen};
    mm::JsonSink sink;
    mm::buildMoonStatsReport(sink, tree, 2, mm::MoonStatsEvent::Install,
                             nullptr, "1.0.0", nullptr);
    const std::string json = sink.data();

    CHECK(json.find("Preview") == std::string::npos);
    CHECK(json.find("service:Audio") != std::string::npos);
}

/// A user's module hangs UNDER a wired parent, so skipping the parent must not skip the child.
TEST_CASE("a module added under a wired parent is still reported") {
    mm::SystemModule parent;
    parent.setName("Effects");
    parent.markWiredByCode();

    mm::AudioService child;
    child.setName("Lissajous");
    parent.addChild(&child);

    mm::MoonModule* tree[] = {&parent};
    mm::JsonSink sink;
    mm::buildMoonStatsReport(sink, tree, 1, mm::MoonStatsEvent::Install,
                             nullptr, "1.0.0", nullptr);
    const std::string json = sink.data();

    CHECK(json.find("service:Lissajous") != std::string::npos);
    CHECK(json.find("Effects") == std::string::npos);
}

/// A scripted module reports WHICH script it runs, because "MoonLive" alone says nothing: the interesting fact is that a device is running `aurora.mle`.
TEST_CASE("a scripted module reports the shipped script it runs") {
    const std::string json = scriptedReport("aurora.mle");
    CHECK(json.find("effect:MoonLive/aurora.mle") != std::string::npos);
}

/// The other half, and the one that matters.
/// A script a USER wrote is a name they invented, which is text they typed. The module still counts, under its bare type name.
///
/// Without this the feature would be a privacy regression wearing a usage-statistics hat: a script called "ewoud-bedroom-test.mle" would travel to the server exactly like a shipped name.
TEST_CASE("a script the user wrote is counted but never named") {
    const std::string json = scriptedReport("ewoud-bedroom-test.mle");
    CHECK(json.find("ewoud-bedroom-test") == std::string::npos);
    CHECK(json.find("effect:MoonLive") != std::string::npos);
    CHECK(json.find("effect:MoonLive/") == std::string::npos);
}

/// A shipped name under the WRONG extension is not a shipped script: the catalogs are per role, so a lookup that scanned them all would let `aurora.mle` through on a layout and, worse, would make "is this ours" depend on a name rather than a name plus its kind.
TEST_CASE("a catalog name is matched against its own role's catalog") {
    const std::string json = scriptedReport("grid.mll", mm::ModuleRole::Layout);
    CHECK(json.find("layout:MoonLive/grid.mll") != std::string::npos);
}

/// The status slot describes THE LAST ATTEMPT, so a verdict has to be retractable. Without this the failure text outlived the failure: a send that failed once painted "Could not reach the server" permanently, and the card kept reporting a send as outstanding long after the next one had arrived. Observed on a NanoPi, whose report HAD landed while the card still showed the error.
///
/// The retraction follows DriverBase's "clear only MY status" rule: a module that cleared unconditionally would wipe a line something else had every right to show.
TEST_CASE("a stats verdict is retracted before the next one is formed") {
    struct Probe : mm::MoonStatsModule {
        using mm::MoonStatsModule::setOwnStatus;
        using mm::MoonStatsModule::clearOwnStatus;
    };
    Probe m;

    static const char kFailed[] = "Could not reach the server.";
    static const char kSent[] = "Sent.";

    m.setOwnStatus(kFailed, mm::MoonModule::Severity::Error);
    CHECK(m.status() == kFailed);

    // A later success retracts the failure rather than leaving both truths on the card.
    m.clearOwnStatus();
    CHECK(m.status() == nullptr);

    m.setOwnStatus(kSent, mm::MoonModule::Severity::Status);
    CHECK(m.status() == kSent);
}

/// The other half of the rule, and the one that makes it safe.
/// A status set by SOMETHING ELSE is never cleared by this module. Retracting unconditionally would turn one fixed bug into another.
TEST_CASE("a stats retraction leaves a foreign status alone") {
    struct Probe : mm::MoonStatsModule {
        using mm::MoonStatsModule::setOwnStatus;
        using mm::MoonStatsModule::clearOwnStatus;
    };
    Probe m;

    static const char kMine[] = "Could not reach the server.";
    static const char kForeign[] = "Ethernet cable unplugged.";

    m.setOwnStatus(kMine, mm::MoonModule::Severity::Error);
    // Something else claims the slot: a driver, the network module, anything.
    m.setStatus(kForeign, mm::MoonModule::Severity::Warning);

    m.clearOwnStatus();
    CHECK(m.status() == kForeign);   // not ours to clear
}

/// The automatic report waits for a MEASURED frame rate. `Scheduler::fps()` divides by `tickTimeUs_`, which is computed only when the first 1-second timing window closes, and the housekeeping tick that sends the report runs inside that window. Without the guard every install and upgrade row carried `fps: 0` (verified on a NanoPi: the automatic row read 0 while a button press from the same device read 124), so the pie described only the rare user who pressed it.
///
/// Pinned at the builder, which is the layer that decides what a zero MEANS.
/// The report carries whatever fps it is handed, so the guard belongs in the caller and the zero must stay expressible.
TEST_CASE("a report carries the frame rate it is given, zero included") {
    mm::AudioService mod;
    mod.setName("Audio");
    mod.setTypeName("AudioService");

    mm::MoonModule* tree[] = {&mod};

    mm::JsonSink measured;
    mm::buildMoonStatsReport(measured, tree, 1, mm::MoonStatsEvent::Install,
                             nullptr, "1.0.0", nullptr, 0, 0, 0, 124);
    CHECK(std::string(measured.data()).find("\"fps\":124") != std::string::npos);

    // A cold start is what the tick1s guard exists to avoid sending, so the builder must still be able to express it: the guard is the policy, not the format.
    mm::JsonSink cold;
    mm::buildMoonStatsReport(cold, tree, 1, mm::MoonStatsEvent::Install,
                             nullptr, "1.0.0", nullptr, 0, 0, 0, 0);
    CHECK(std::string(cold.data()).find("\"fps\":0") != std::string::npos);
}
