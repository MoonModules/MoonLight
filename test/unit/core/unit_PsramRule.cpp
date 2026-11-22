/// @module PsramRule

/// Unit tests for src/platform/esp32/psram_rule.h: one image runs on every classic revision, so this rule keeps a revision 0 or 1 chip off PSRAM unless the build carries the cache workaround.

#include "doctest.h"
#include "platform/esp32/psram_rule.h"

using mm::platform::psramHeapSafe;

TEST_CASE("PSRAM serves the heap on revision 3 and later") {
    CHECK(psramHeapSafe(300, false));
    CHECK(psramHeapSafe(301, false));
}

TEST_CASE("PSRAM stays out of the heap on revision 0 and 1 unless the build carries the cache workaround") {
    CHECK_FALSE(psramHeapSafe(0, false));
    CHECK_FALSE(psramHeapSafe(100, false));
    CHECK_FALSE(psramHeapSafe(200, false));
}

TEST_CASE("a D0WD board without PSRAM gets GPIO 16 and 17 back for LEDs and the Ethernet clock") {
    constexpr uint64_t k16 = uint64_t{1} << 16, k17 = uint64_t{1} << 17;
    CHECK(mm::platform::psramPadsFreedWhenAbsent(0) == (k16 | k17));
    CHECK(mm::platform::psramPadsFreedWhenAbsent(1) == (k16 | k17));
    CHECK(mm::platform::psramPadsFreedWhenAbsent(7) == k16);
}

TEST_CASE("a package whose PSRAM pins are its built-in flash keeps them reserved") {
    CHECK(mm::platform::psramPadsFreedWhenAbsent(2) == 0);   // D2WD
    CHECK(mm::platform::psramPadsFreedWhenAbsent(4) == 0);   // PICO-D2 / U4WDH
    CHECK(mm::platform::psramPadsFreedWhenAbsent(5) == 0);   // PICO-D4
    CHECK(mm::platform::psramPadsFreedWhenAbsent(6) == 0);   // PICO-V3-02
}

TEST_CASE("a build with the cache workaround uses PSRAM on every revision") {
    CHECK(psramHeapSafe(0, true));
    CHECK(psramHeapSafe(100, true));
    CHECK(psramHeapSafe(301, true));
}
