/// @module DevicesModule

/// Pins the Hue-bridge listing: a HueDriver registers a bridge through upsertHueBridge() (the explicit, out-of-band entry, a bridge isn't a UDP-presence device), and DevicesModule lists it like any peer, carrying its color-light count for layout sizing.

#include "doctest.h"
#include "core/system/DevicesModule.h"
#include "core/util/JsonSink.h"

#include <cstring>

TEST_CASE("DevicesModule: a Hue bridge is listed with its color count") {
    mm::DevicesModule dev;
    const uint8_t ip[4] = {192, 168, 1, 143};
    dev.upsertHueBridge(ip, "Hue Ewoud", 7);

    REQUIRE(dev.listRowCount() == 1);
    mm::JsonSink sink;
    dev.writeListRow(sink, 0);
    const char* row = sink.data();
    CHECK(std::strstr(row, "\"name\":\"Hue Ewoud\"") != nullptr);
    CHECK(std::strstr(row, "\"type\":\"Hue bridge\"") != nullptr);
    CHECK(std::strstr(row, "\"color\":7") != nullptr);
}

TEST_CASE("DevicesModule: upsertHueBridge is idempotent, updates count in place") {
    mm::DevicesModule dev;
    const uint8_t ip[4] = {192, 168, 1, 143};
    dev.upsertHueBridge(ip, "Hue Ewoud", 7);
    dev.upsertHueBridge(ip, "Hue Ewoud", 9);   // same bridge, count changed

    REQUIRE(dev.listRowCount() == 1);           // not a second row
    mm::JsonSink sink;
    dev.writeListRow(sink, 0);
    CHECK(std::strstr(sink.data(), "\"color\":9") != nullptr);
}
