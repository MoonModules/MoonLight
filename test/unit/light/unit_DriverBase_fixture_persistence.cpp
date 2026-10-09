/// @module DriverBase
/// @also FixtureProfilesModule, FilesystemModule, Drivers

/// A driver keeps its fixture profile across a reboot and a restore by the profile's name.

#include "doctest.h"
#include "core/module/Scheduler.h"
#include "core/module/StateDocument.h"
#include "core/system/FilesystemModule.h"
#include "light/drivers/Drivers.h"
#include "light/drivers/FixtureProfilesModule.h"
#include "light/drivers/NetworkSendDriver.h"
#include "platform/platform.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
// The wiring a driver sends, as its green, red, blue and white offsets: what the strip shows, not what the card says.
std::string wiring(const mm::Correction& c) {
    const auto at = [](uint8_t off) { return off == mm::Correction::kAbsent ? std::string("-") : std::to_string(off); };
    return "G" + at(c.offGreen) + " R" + at(c.offRed) + " B" + at(c.offBlue) + " W" + at(c.offWhite);
}
const std::string kGrbw = "G0 R1 B2 W3";
const std::string kRgb = "G1 R0 B2 W-";

// Boot the Drivers container with its library and one driver from whatever `/.config` holds, as the device does, and report the driver's wiring.
std::string bootFixture(const char* root, bool save = false, const char* restore = nullptr) {
    mm::platform::fsSetRoot(root);
    std::string got;
    {
        mm::Scheduler scheduler;
        auto* fs = new mm::FilesystemModule();
        fs->setTypeName("FilesystemModule");
        fs->setScheduler(&scheduler);
        auto* drivers = new mm::Drivers();
        drivers->setTypeName("Drivers");
        drivers->setName("Drivers");
        auto* lib = new mm::FixtureProfilesModule();
        lib->setTypeName("FixtureProfilesModule");
        lib->setName("FixtureProfiles");
        auto* drv = new mm::NetworkSendDriver();
        drv->setTypeName("NetworkSendDriver");
        drv->setName("NetworkSend");
        drivers->addChild(lib);
        drivers->addChild(drv);
        scheduler.addModule(fs);
        scheduler.addModule(drivers);
        scheduler.setup();
        if (restore) REQUIRE(mm::applyStateDocument(scheduler, restore, mm::StateSource::Stored).ok);   // a backup restored on the running device
        got = wiring(drv->correction());
        if (save) {
            drivers->markDirty();
            mm::FilesystemModule::noteDirty();
            fs->flush();
        }
        scheduler.release();
    }
    mm::platform::fsSetRoot(nullptr);
    return got;
}
void writeDrivers(const char* root, const char* driverKeys) {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(std::string(root) + "/.config");
    std::ofstream(std::string(root) + "/.config/Drivers.json")
        << R"({"Drivers":{"$patch":"replace","enabled":true,"FixtureProfiles":{"type":"FixtureProfilesModule","enabled":true},"NetworkSend":{"type":"NetworkSendDriver","enabled":true,)" << driverKeys << "}}}";
}
std::string readDrivers(const char* root) {
    std::ifstream f(std::string(root) + "/.config/Drivers.json");
    return std::string(std::istreambuf_iterator<char>(f), {});
}
}  // namespace

// An older config saved the row number beside the name, and the number now points elsewhere: the name wins, on the wire as on the card.
TEST_CASE("A driver's fixture profile survives a reboot by name") {
    const char* root = "/tmp/mm_fixture_by_name";
    writeDrivers(root, R"("fixture":4,"fixtureRef":"GRBW")");   // row 4 of the built-in list is GBR
    CHECK(bootFixture(root, true) == kGrbw);
    CHECK(readDrivers(root).find(R"("fixture":"GRBW")") != std::string::npos);   // the name, never the row number
    CHECK(bootFixture(root) == kGrbw);
    // A name no profile has any more, such as a renamed custom one, leaves the driver on its default.
    writeDrivers(root, R"("fixture":"gone")");
    CHECK(bootFixture(root) == kRgb);
    std::filesystem::remove_all(root);
}

// A backup from before this release, restored on a running device, takes effect by name at once and saves the name.
TEST_CASE("A restored older backup applies its fixture profile by name") {
    const char* root = "/tmp/mm_fixture_restore";
    writeDrivers(root, R"("fixture":"RGB")");
    const char* backup = R"({"Drivers":{"$patch":"replace","enabled":true,"FixtureProfiles":{"type":"FixtureProfilesModule","enabled":true},"NetworkSend":{"type":"NetworkSendDriver","enabled":true,"fixture":4,"fixtureRef":"GRBW"}}})";
    CHECK(bootFixture(root, true, backup) == kGrbw);
    CHECK(readDrivers(root).find(R"("fixture":"GRBW")") != std::string::npos);
    CHECK(bootFixture(root) == kGrbw);
    std::filesystem::remove_all(root);
}
