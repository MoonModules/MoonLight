/// @module SystemModule

#include "doctest.h"
#include "core/system/SystemModule.h"

#include <cstring>
#include <string>

namespace {
// Stand-in wired-by-code child: counts the lifecycle callbacks a real fixed System child (Tasks, I2cBus) would use (setup to init, tick20ms/tick1s to poll + format). Pins that SystemModule's overridden setup()/tick1s() chain to base, without that, a child would never initialize or poll.
class CountingChild : public mm::MoonModule {
public:
    uint32_t setupCalls = 0, tick20msCalls = 0, tick1sCalls = 0;
    void setup() override { setupCalls++; }
    void tick20ms() MM_NONBLOCKING override { tick20msCalls++; }
    void tick1s() MM_NONBLOCKING override { tick1sCalls++; }
};
} // namespace

/// The derived name is "MM-" plus the last two MAC bytes in hex, whatever those bytes are.
///
/// Pinned by SHAPE rather than against one literal: the desktop MAC is a stored per-install identity now (platform_desktop.cpp, getMacAddress), so a fresh install generates its own and asserting "MM-CAFE" would pin the old hardcoded constant rather than the derivation.
bool looksLikeMacName(const char* name) {
    uint8_t mac[6] = {};
    mm::platform::getMacAddress(mac);
    char expect[8] = {};
    std::snprintf(expect, sizeof(expect), "MM-%02X%02X", mac[4], mac[5]);
    return std::strcmp(name, expect) == 0;
}

// The auto-generated device name is "MM-" plus the last two MAC bytes (see looksLikeMacName).
TEST_CASE("SystemModule MAC-to-deviceName") {
    // Desktop platform returns MAC DE:AD:BE:EF:CA:FE deviceName follows the MAC, whatever this install's stored identity is
    mm::SystemModule sys;
    sys.setup();
    CHECK(looksLikeMacName(sys.deviceName()));
}

// deviceName is bound as a Text control to the MAC-derived default (see looksLikeMacName).
TEST_CASE("SystemModule deviceName control") {
    mm::SystemModule sys;
    sys.setup();
    sys.defineControls();

    bool found = false;
    for (uint8_t i = 0; i < sys.controls().count(); i++) {
        if (std::strcmp(sys.controls()[i].name, "deviceName") == 0) {
            CHECK(sys.controls()[i].type == mm::ControlType::Text);
            CHECK(looksLikeMacName(static_cast<char*>(sys.controls()[i].ptr)));
            found = true;
        }
    }
    CHECK(found);
}

namespace {
// Overwrite SystemModule's deviceName buffer through its bound control pointer, the same buffer the persistence overlay and an /api/control write target. Lets a test seed an invalid name and then drive the module's sanitisation.
void writeDeviceName(mm::SystemModule& sys, const char* value) {
    for (uint8_t i = 0; i < sys.controls().count(); i++) {
        if (std::strcmp(sys.controls()[i].name, "deviceName") == 0) {
            char* buf = static_cast<char*>(sys.controls()[i].ptr);
            std::strncpy(buf, value, 23);
            buf[23] = 0;
            return;
        }
    }
    // No `deviceName` control found, a setup regression. Fail loudly rather than silently no-op, which would let the calling test "pass" against a stale buffer.
    REQUIRE_MESSAGE(false, "writeDeviceName: no 'deviceName' control on SystemModule");
}
} // namespace

// deviceName is the single network identity, so SystemModule keeps it a valid hostname. A live edit to an invalid value ("My Room!") is coerced on the next tick1s tick (mm::sanitizeHostname), the same path mDNS/AP/DHCP read, so they never see spaces.
TEST_CASE("SystemModule sanitises a live deviceName edit") {
    mm::SystemModule sys;
    sys.setup();
    sys.defineControls();
    writeDeviceName(sys, "My Living Room!");
    sys.tick1s();                                   // the tick that coerces it
    CHECK(std::strcmp(sys.deviceName(), "My-Living-Room") == 0);
}

// An all-invalid name collapses to empty after sanitising; the MAC fallback then fills it, so deviceName is never empty (mDNS/AP/DHCP always have a name to register).
TEST_CASE("SystemModule falls back to the MAC name when deviceName is all-invalid") {
    mm::SystemModule sys;
    sys.setup();
    sys.defineControls();
    writeDeviceName(sys, "!@#$");
    sys.tick1s();
    CHECK(looksLikeMacName(sys.deviceName()));   // the MAC-derived fallback
}

// An already-valid name is left untouched (idempotent), a normal user name survives.
TEST_CASE("SystemModule leaves a valid deviceName unchanged") {
    mm::SystemModule sys;
    sys.setup();
    sys.defineControls();
    writeDeviceName(sys, "Bench-S3");
    sys.tick1s();
    CHECK(std::strcmp(sys.deviceName(), "Bench-S3") == 0);
}

// (firmware identity controls, version / build / firmware, moved to FirmwareUpdateModule; see test/unit/core/unit_FirmwareUpdateModule.cpp.)

// The `bootReason` control is populated from platform::resetReason; on desktop it reports "OK".
TEST_CASE("SystemModule bootReason control populated") {
    // The bootReason control is wired in setup() (from platform::resetReason).
    // On desktop the platform stub always returns "OK".
    // The UI uses this to set the reboot button's crashed-state styling, see ui-spec.md.
    mm::SystemModule sys;
    sys.setup();
    sys.defineControls();

    bool found = false;
    for (uint8_t i = 0; i < sys.controls().count(); i++) {
        if (std::strcmp(sys.controls()[i].name, "bootReason") == 0) {
            CHECK(sys.controls()[i].type == mm::ControlType::ReadOnly);
            const char* val = static_cast<const char*>(sys.controls()[i].ptr);
            CHECK(val != nullptr);
            CHECK(val[0] != '\0');  // non-empty
            // Desktop stub always reports "OK"
            CHECK(std::strcmp(val, "OK") == 0);
            found = true;
        }
    }
    CHECK(found);
}

// System is fixed infrastructure, it accepts no user-added children (they live under the Services container). Its own children (Tasks, I2cBus) are wired by code.
TEST_CASE("SystemModule accepts no user-added children") {
    // System is fixed infrastructure: its children (Tasks, I2cBus) are wired by code, so it accepts no user-added role. User-added capability modules live under the Services container instead.
    mm::SystemModule sys;
    CHECK(std::strcmp(sys.acceptsChildRoles(), "") == 0);
}

// Regression: SystemModule overrides setup() and tick1s(); both must chain to MoonModule's base so a wired-by-code child's setup()/tick1s() fire. Without the chain a fixed child (Tasks/I2cBus) would never init or poll (the "children miss callbacks" trap). tick20ms() isn't overridden, so the base default already propagates it.
TEST_CASE("SystemModule propagates lifecycle to a wired-by-code child") {
    mm::SystemModule sys;
    CountingChild child;
    sys.addChild(&child);

    sys.setup();
    CHECK(child.setupCalls == 1);   // setup() chained to base

    sys.tick1s();
    CHECK(child.tick1sCalls == 1);  // tick1s() chained to base

    sys.tick20ms();
    CHECK(child.tick20msCalls == 1); // base default (not overridden) propagates
}

// roleName maps the Service enum to its lowercase API string.
TEST_CASE("Service role name") {
    CHECK(std::strcmp(mm::roleName(mm::ModuleRole::Service), "service") == 0);
}

// The persisted `firmware` text must not outlive the image that wrote it.
// A device flashed from one variant to another loaded the old name back over the compile-time one (bench, MHC P4 shield, 2026-09-08) and MoonBase would have offered the wrong flash layout.
// The constant wins on every write of the control, which is the moment a stale value lands.
TEST_CASE("SystemModule: a persisted firmware name is overwritten by the compile-time one") {
    mm::SystemModule m;
    m.defineControls();
    // The same access the deviceName test uses: the control's bound pointer IS the module's buffer.
    char* text = nullptr;
    for (uint8_t i = 0; i < m.controls().count(); i++)
        if (std::strcmp(m.controls()[i].name, "firmware") == 0) text = static_cast<char*>(m.controls()[i].ptr);
    REQUIRE(text != nullptr);
    // Simulate the config load writing a stale variant, then the change notification it triggers.
    std::snprintf(text, 32, "%s", "some-other-variant");
    m.onControlChanged("firmware");
    CHECK(std::string(text) == mm::kFirmwareName);
}
