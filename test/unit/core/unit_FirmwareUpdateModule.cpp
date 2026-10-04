/// @module FirmwareUpdateModule

#include "doctest.h"
#include "core/system/FirmwareUpdateModule.h"
#include <cstring>

// The `firmware` control always carries a name: the variant from build_info.h, or "unknown" on a local desktop build.
TEST_CASE("FirmwareUpdateModule firmware control populated") {
    // A release build gets the real variant, which the install picker reads.
    mm::FirmwareUpdateModule fw;
    fw.setup();
    fw.defineControls();

    bool found = false;
    for (uint8_t i = 0; i < fw.controls().count(); i++) {
        if (std::strcmp(fw.controls()[i].name, "firmware") == 0) {
            CHECK(fw.controls()[i].type == mm::ControlType::ReadOnly);
            const char* val = static_cast<const char*>(fw.controls()[i].ptr);
            CHECK(val != nullptr);
            CHECK(val[0] != '\0');  // non-empty (either a real key or "unknown")
            found = true;
        }
    }
    CHECK(found);

    // The partition bar exists only where the platform reports an app partition, so its type is checked when present.
    bool hasVersion = false, hasBuild = false, hasPartition = false;
    for (uint8_t i = 0; i < fw.controls().count(); i++) {
        const auto& c = fw.controls()[i];
        if (std::strcmp(c.name, "version") == 0) hasVersion = true;
        if (std::strcmp(c.name, "build") == 0) hasBuild = true;
        // `partition`, renamed from `firmwarePartition` when one control set began describing either image. The old name left this CHECK inside a condition that is never true, so it asserted nothing while looking like it did.
        if (std::strcmp(c.name, "partition") == 0) {
            hasPartition = true;
            CHECK(c.type == mm::ControlType::Progress);
        }
    }
    CHECK(hasVersion);
    CHECK(hasBuild);
    // Not asserted present: the control is added only when the platform reports a partition size, and desktop reports 0. What is asserted is its TYPE when it does exist, which is what the old `firmwarePartition` spelling silently stopped checking.
    (void)hasPartition;
}

// The install phase rides the status slot: idle clears it, an "error: " prefix is an error, anything else a plain status.
TEST_CASE("FirmwareUpdateModule OTA status routes through the status slot") {
    mm::FirmwareUpdateModule fw;

    // Boot state: g_otaStatus is "idle" → no banner.
    std::strncpy(mm::g_otaStatus, "idle", sizeof(mm::g_otaStatus));
    fw.setup();
    CHECK(fw.status() == nullptr);

    // An in-flight phase shows as a neutral status banner.
    std::strncpy(mm::g_otaStatus, "downloading", sizeof(mm::g_otaStatus));
    fw.tick1s();
    REQUIRE(fw.status() != nullptr);
    CHECK(std::strcmp(fw.status(), "downloading") == 0);
    CHECK(fw.severity() == mm::MoonModule::Severity::Status);

    // A failure (platform prefixes every failure with "error: ") is an error banner.
    std::strncpy(mm::g_otaStatus, "error: ota perform ESP_FAIL", sizeof(mm::g_otaStatus));
    fw.tick1s();
    REQUIRE(fw.status() != nullptr);
    CHECK(fw.severity() == mm::MoonModule::Severity::Error);

    // Returning to idle clears the banner again.
    std::strncpy(mm::g_otaStatus, "idle", sizeof(mm::g_otaStatus));
    fw.tick1s();
    CHECK(fw.status() == nullptr);
}

namespace {
const char* readOnly(mm::FirmwareUpdateModule& fw, const char* name) {
    for (uint8_t i = 0; i < fw.controls().count(); i++)
        if (std::strcmp(fw.controls()[i].name, name) == 0) return static_cast<const char*>(fw.controls()[i].ptr);
    return nullptr;
}
}  // namespace

// MoonBase's `<version>+<build id>` splits into its version and build rows; a mismatch with the app warns between installs and is published for the UI's signals.
TEST_CASE("a MoonBase that does not match the app says so, and only then") {
    std::strncpy(mm::g_otaStatus, "idle", sizeof(mm::g_otaStatus));
    // Removed however the case ends, so a failing REQUIRE does not leave a MoonBase behind for every later test.
    struct NoMoonBaseAfter { ~NoMoonBaseAfter() { mm::platform::setTestMoonBase(nullptr, nullptr); } } noMoonBaseAfter;
    mm::platform::setTestMoonBase("0.0.1+abc12345+", "Oct  4 2026 10:31:00");
    mm::FirmwareUpdateModule fw;
    fw.defineControls();
    fw.setup();
    REQUIRE(readOnly(fw, "appVersion") != nullptr);
    CHECK(std::strcmp(readOnly(fw, "appVersion"), mm::kVersion) == 0);
    REQUIRE(readOnly(fw, "moonbaseMismatch") != nullptr);
    CHECK(std::strcmp(readOnly(fw, "moonbaseMismatch"), "0.0.1") == 0);
    *static_cast<uint8_t*>(fw.controls()[2].ptr) = 1;   // the MoonBase tab: appVersion, moonbaseMismatch, image
    fw.rebuildControls();
    CHECK(std::strcmp(readOnly(fw, "version"), "0.0.1") == 0);
    CHECK(std::strcmp(readOnly(fw, "build"), "abc12345+ · Oct  4 2026 10:31:00") == 0);
    fw.tick1s();
    REQUIRE(fw.status() != nullptr);
    CHECK(std::strstr(fw.status(), "MoonBase does not match this app") != nullptr);
    CHECK(fw.severity() == mm::MoonModule::Severity::Warning);

    // The matching MoonBase says nothing.
    static char matching[48];
    std::snprintf(matching, sizeof(matching), "%s+abc12345", mm::kVersion);
    mm::platform::setTestMoonBase(matching, "Oct  4 2026 10:31:00");
    mm::FirmwareUpdateModule same;
    same.defineControls();
    same.setup();
    same.tick1s();
    CHECK(same.status() == nullptr);
    CHECK(readOnly(same, "moonbaseMismatch") == nullptr);
}
