/// @module PanelCardDriver
/// @also Drivers, Correction

/// End-to-end tests for the raw-Ethernet panel driver. The desktop platform records every frame ethSendRaw() emits instead of putting it on a wire, so these pin what the driver would actually send, the geometry, the chunking, and the sync-last ordering, with no hardware and no privileges. The bench then only has to confirm the wire itself.

#include "doctest.h"
#include "light/drivers/PanelCardDriver.h"
#include "light/drivers/Correction.h"
#include "correction_presets.h"
#include "light/layers/Buffer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/PanelsLayout.h"
#include "core/util/JsonSink.h"   // writeControlMetadata: the schema the UI parses
#include <string>   // status() substring checks

namespace {

// Build a driver over a `count`-light source with a plain RGB correction, ready to tick.
// Build a driver over a `count`-light source. With no Layer wired the driver falls back to the panel arrangement for the wall size, which is exactly what a bare unit test wants: geometry it states rather than geometry it has to construct a Layout for.
// A wall of `width` x `height`, wired the way production wires it: a Layout gives the Layer its physical size, and the driver reads that. The driver has no geometry of its own to set.
struct Wall {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;

    Wall(uint16_t width, uint16_t height) {
        grid.width = width;
        grid.height = height;
        grid.depth = 1;
        layouts.addChild(&grid);
        layouts.applyState();
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.applyState();
    }
};

void setUp(mm::PanelCardDriver& driver, mm::Buffer& source, Wall& wall,
           mm::nrOfLightsType count) {
    REQUIRE(source.allocate(count, 3));
    mm::Correction correction;
    mm::test::rebuildFromPreset(correction, 255, mm::test::PresetOrder::RGB);
    driver.setLayer(&wall.layer);
    driver.setSourceBuffer(&source);
    driver.correctionForTest() = correction;
    driver.applyState();
    mm::platform::ethTestClearFrames();
}

// Drop any raw-L2 claim left by an earlier test's driver. Test drivers are stack objects that are destroyed without release() being called, so the claim count would otherwise carry across cases.
void clearClaims() {
    while (mm::platform::ethRawL2Claimed()) mm::platform::ethClaimRawL2(false);
    // The platform's simulated-failure switches are process-global: a test that leaves one set poisons whichever test runs next, which is how a passing-alone/failing-in-sequence result appears. Reset them here so every case starts from a known platform state.
    mm::platform::setTestEthSendFails(false);
    mm::platform::setTestEthRestartFails(false);
    mm::platform::ethTestClearFrames();
}

// The packet-type byte of a captured frame (offset 12), which says row / sync / brightness.
uint8_t frameType(size_t i) {
    return mm::platform::ethTestFrameData(i)[12];
}

}  // namespace

// A panel is sent as one frame per row followed by a single sync, the sync is what latches the image, so it must arrive after every row it applies to.
TEST_CASE("PanelCardDriver sends one frame per row then one sync") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 4);
    setUp(driver, source, wall, 256);
    driver.firmware = mm::PanelCardDriver::kFirmwareV13;               // "v13 and newer": acts on the SECOND copy, so both go out

    mm::platform::setTestNowMs(1000);
    driver.tick();

    REQUIRE(mm::platform::ethTestFrameCount() == 8);   // 2 brightness + 4 rows + 2 sync
    CHECK(frameType(0) == mm::COLORLIGHT_TYPE_BRIGHTNESS);
    CHECK(frameType(1) == mm::COLORLIGHT_TYPE_BRIGHTNESS);
    for (size_t i = 2; i <= 5; i++) CHECK(frameType(i) == mm::COLORLIGHT_TYPE_ROW);
    CHECK(frameType(6) == mm::COLORLIGHT_TYPE_SYNC);
    CHECK(frameType(7) == mm::COLORLIGHT_TYPE_SYNC);
}

// A card on v12-or-older firmware acts on the FIRST copy and reads a second sync as another latch, aborting the refresh already running: the wall then updates once every few seconds. That generation gets exactly one brightness and one sync, which is also what FPP sends such a card.
TEST_CASE("PanelCardDriver sends a pre-v13 card one brightness and one sync") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 4);
    setUp(driver, source, wall, 256);
    // No assignment: "v12 and older" is index 0 and the DEFAULT, because the documented path downgrades the card to clear the v13 flicker. This test also pins that default.

    mm::platform::setTestNowMs(1000);
    driver.tick();

    REQUIRE(mm::platform::ethTestFrameCount() == 6);   // 1 brightness + 4 rows + 1 sync
    CHECK(frameType(0) == mm::COLORLIGHT_TYPE_BRIGHTNESS);
    for (size_t i = 1; i <= 4; i++) CHECK(frameType(i) == mm::COLORLIGHT_TYPE_ROW);
    CHECK(frameType(5) == mm::COLORLIGHT_TYPE_SYNC);
}

// A buffer smaller than the wall sends only the rows it covers, then latches, rather than reading past the buffer for the rows it does not have.
TEST_CASE("PanelCardDriver stops at the last row its buffer covers") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 8);            // wall is 8 rows high
    setUp(driver, source, wall, 128);   // buffer holds only 2 rows of it

    mm::platform::setTestNowMs(1000);
    driver.tick();

    REQUIRE(mm::platform::ethTestFrameCount() == 4);   // brightness + 2 rows + sync, not 8 rows
    CHECK(frameType(3) == mm::COLORLIGHT_TYPE_SYNC);
}

// A row wider than one packet splits into several, each carrying its own pixel offset, the wide-panel case, where 497 pixels is the per-packet ceiling.
TEST_CASE("PanelCardDriver splits a row wider than one packet") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(600, 1);           // one row of 600 > the 497-pixel packet ceiling
    setUp(driver, source, wall, 600);

    mm::platform::setTestNowMs(1000);
    driver.tick();

    REQUIRE(mm::platform::ethTestFrameCount() == 4);   // brightness + 2 row packets + sync
    // First chunk: offset 0, a full 497 pixels. Frame 1: one brightness precedes the rows.
    const uint8_t* a = mm::platform::ethTestFrameData(1);
    CHECK(((a[15] << 8) | a[16]) == 0);
    CHECK(((a[17] << 8) | a[18]) == mm::COLORLIGHT_MAX_PIXELS_PER_PACKET);
    // Second chunk: continues at 497, carrying the remaining 103.
    const uint8_t* b = mm::platform::ethTestFrameData(2);
    CHECK(((b[15] << 8) | b[16]) == mm::COLORLIGHT_MAX_PIXELS_PER_PACKET);
    CHECK(((b[17] << 8) | b[18]) == 600 - mm::COLORLIGHT_MAX_PIXELS_PER_PACKET);
}

// Pixel bytes reach the wire unchanged, so what an effect rendered is what the panel receives.
TEST_CASE("PanelCardDriver puts rendered pixels on the wire") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(2, 1);
    setUp(driver, source, wall, 2);
    uint8_t* px = source.data();
    px[0] = 10; px[1] = 20; px[2] = 30;
    px[3] = 40; px[4] = 50; px[5] = 60;

    mm::platform::setTestNowMs(1000);
    driver.tick();

    REQUIRE(mm::platform::ethTestFrameCount() == 3);   // brightness + row + sync
    const uint8_t* row = mm::platform::ethTestFrameData(1);
    CHECK(row[mm::COLORLIGHT_ROW_PREFIX + 0] == 10);
    CHECK(row[mm::COLORLIGHT_ROW_PREFIX + 1] == 20);
    CHECK(row[mm::COLORLIGHT_ROW_PREFIX + 2] == 30);
    CHECK(row[mm::COLORLIGHT_ROW_PREFIX + 3] == 40);
}

// The fps control caps the send rate, so a render loop faster than the panel needs doesn't saturate the link.
TEST_CASE("PanelCardDriver rate-limits to its fps setting") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);
    driver.fps = 40;   // one frame every 25 ms

    mm::platform::setTestNowMs(1000);
    driver.tick();
    const size_t after1 = mm::platform::ethTestFrameCount();
    CHECK(after1 == 3);   // brightness + row + sync

    mm::platform::setTestNowMs(1010);   // too soon
    driver.tick();
    CHECK(mm::platform::ethTestFrameCount() == after1);

    mm::platform::setTestNowMs(1030);   // past the interval
    driver.tick();
    CHECK(mm::platform::ethTestFrameCount() > after1);
}

// Nothing is latched when the window is empty: an all-zero geometry must not emit a bare sync, which would blank a panel that another driver is feeding.
TEST_CASE("PanelCardDriver sends nothing when the window is empty") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);
    driver.fps = 0;   // nothing to send

    mm::platform::setTestNowMs(1000);
    driver.tick();

    CHECK(mm::platform::ethTestFrameCount() == 0);
}

// A dropped frame doesn't stall the driver: the cards give no acknowledgement, so a failed send is tolerated exactly as a dropped UDP packet is, and the next tick proceeds.
TEST_CASE("PanelCardDriver survives a failing link") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);

    mm::platform::setTestEthSendFails(true);
    mm::platform::setTestNowMs(1000);
    driver.tick();                                   // must not hang or crash
    CHECK(mm::platform::ethTestFrameCount() == 0);   // nothing reached the wire

    mm::platform::setTestEthSendFails(false);
    mm::platform::setTestNowMs(1100);
    driver.tick();
    CHECK(mm::platform::ethTestFrameCount() == 3);   // recovers on the next tick
}

// The correction-applied buffer is sized off the hot path, so tick() never allocates, the same contract the other sinks hold.
TEST_CASE("PanelCardDriver sizes its buffer in prepare, not in tick") {
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);

    REQUIRE(driver.correctedBuffer().data() != nullptr);
    CHECK(driver.correctedBuffer().count() == 64);

    const uint8_t* before = driver.correctedBuffer().data();
    mm::platform::setTestNowMs(1000);
    driver.tick();
    CHECK(driver.correctedBuffer().data() == before);
    CHECK(driver.correctedBuffer().count() == 64);
}

// The driver CLAIMS the ethernet interface while it is enabled, which is how NetworkModule tells "the cable is broken" apart from "a driver is using the wire below IP". Stated at prepare rather than inferred from traffic, so the claim is in place before the first frame and cannot race the network cascade's DHCP timeout.
TEST_CASE("PanelCardDriver claims the ethernet link while enabled") {
    clearClaims();
    mm::Buffer source;
    mm::PanelCardDriver driver;
    CHECK_FALSE(mm::platform::ethRawL2Claimed());     // nothing claimed before setup

    Wall wall(64, 1);
    setUp(driver, source, wall, 64);                  // applyState() runs prepare()
    CHECK(mm::platform::ethRawL2Claimed());           // claimed without a frame being sent

    driver.release();
    CHECK_FALSE(mm::platform::ethRawL2Claimed());     // released with the driver
}

// Re-preparing (a geometry or window edit) must not stack claims, or the link would stay claimed after the driver is gone.
TEST_CASE("PanelCardDriver claim survives a re-prepare without stacking") {
    clearClaims();
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);

    driver.prepare();
    driver.prepare();
    CHECK(mm::platform::ethRawL2Claimed());

    driver.release();
    CHECK_FALSE(mm::platform::ethRawL2Claimed());   // one release is enough
}

// The wall a PanelsLayout describes is what reaches the card: two 128x64 panels stacked give 128 rows of 128 pixels, and the driver reads that from the Layout rather than from controls of its own. This is the setup a ColorLight card is normally configured for.
TEST_CASE("PanelCardDriver sends the wall a PanelsLayout describes") {
    clearClaims();
    mm::Layouts layouts;
    mm::PanelsLayout panels;
    panels.horizontalPanels = 1;
    panels.verticalPanels = 2;        // stacked
    panels.panelWidth = 128;
    panels.panelHeight = 64;
    layouts.addChild(&panels);
    layouts.applyState();

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.applyState();
    REQUIRE(layer.physicalWidth() == 128);
    REQUIRE(layer.physicalHeight() == 128);

    mm::Buffer source;
    REQUIRE(source.allocate(128 * 128, 3));
    mm::Correction correction;
    mm::test::rebuildFromPreset(correction, 255, mm::test::PresetOrder::RGB);
    mm::PanelCardDriver driver;
    driver.setLayer(&layer);
    driver.setSourceBuffer(&source);
    driver.correctionForTest() = correction;
    driver.applyState();
    mm::platform::ethTestClearFrames();

    mm::platform::setTestNowMs(1000);
    driver.tick();

    // brightness + 128 rows (one packet each, 128 <= 497) + sync
    REQUIRE(mm::platform::ethTestFrameCount() == 130);
    // Rows are numbered across the whole card, not restarted per panel.
    const uint8_t* firstRow = mm::platform::ethTestFrameData(1);
    REQUIRE(firstRow != nullptr);
    CHECK(((firstRow[13] << 8) | firstRow[14]) == 0);
    const uint8_t* lastRow = mm::platform::ethTestFrameData(128);
    REQUIRE(lastRow != nullptr);
    CHECK(((lastRow[13] << 8) | lastRow[14]) == 127);
}

// Panels chained side by side widen each card row instead of adding rows, the same Layout control the user already sets, with no second place to state it.
TEST_CASE("PanelCardDriver widens the row when a PanelsLayout chains panels across") {
    clearClaims();
    mm::Layouts layouts;
    mm::PanelsLayout panels;
    panels.horizontalPanels = 2;      // chained: 256 px per row
    panels.verticalPanels = 1;
    panels.panelWidth = 128;
    panels.panelHeight = 64;
    layouts.addChild(&panels);
    layouts.applyState();

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.applyState();
    REQUIRE(layer.physicalWidth() == 256);

    mm::Buffer source;
    REQUIRE(source.allocate(256 * 64, 3));
    mm::Correction correction;
    mm::test::rebuildFromPreset(correction, 255, mm::test::PresetOrder::RGB);
    mm::PanelCardDriver driver;
    driver.setLayer(&layer);
    driver.setSourceBuffer(&source);
    driver.correctionForTest() = correction;
    driver.applyState();
    mm::platform::ethTestClearFrames();

    mm::platform::setTestNowMs(1000);
    driver.tick();

    // brightness + 64 rows + sync: still one packet per row, since 256 <= 497.
    REQUIRE(mm::platform::ethTestFrameCount() == 66);
    const uint8_t* row = mm::platform::ethTestFrameData(2);
    REQUIRE(row != nullptr);
    CHECK(((row[17] << 8) | row[18]) == 256);   // pixels in this packet
}

// A transmit path that refuses every frame must SAY so. It reads as a healthy link otherwise, the IDF driver keeps reporting the negotiated speed while rejecting every frame, so "0 packets/s at 1 Gbit" would look identical to an idle effect.
TEST_CASE("PanelCardDriver reports a failing transmit path instead of a healthy link") {
    clearClaims();
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);

    mm::platform::setTestEthSendFails(true);
    mm::platform::setTestNowMs(1000);
    driver.tick();
    driver.tick1s();
    CHECK(mm::platform::ethSendFailStreak() > 0);

    mm::platform::setTestEthSendFails(false);
    mm::platform::setTestNowMs(1100);
    driver.tick();
    CHECK(mm::platform::ethSendFailStreak() == 0);   // a success clears it: back-pressure is not a fault
}

// A link that fails every send must not latch: the sync frame tells the cards to show what they have, so emitting one after a frame where nothing arrived would blank a wall that was previously showing a good image.
TEST_CASE("PanelCardDriver does not latch a frame that never reached the wire") {
    clearClaims();
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);

    mm::platform::setTestEthSendFails(true);
    mm::platform::setTestNowMs(1000);
    driver.tick();
    mm::platform::setTestEthSendFails(false);

    // Nothing was recorded at all, in particular no sync frame slipped through after the failures.
    CHECK(mm::platform::ethTestFrameCount() == 0);
}

// A window covering no whole row sends nothing, rather than putting the brightness pair on the wire every tick for a wall it cannot fill.
TEST_CASE("PanelCardDriver sends nothing when the buffer covers no row") {
    clearClaims();
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 4);            // wall rows are 64 wide
    setUp(driver, source, wall, 10);   // buffer holds 10 lights: less than one row

    mm::platform::setTestNowMs(1000);
    driver.tick();

    CHECK(mm::platform::ethTestFrameCount() == 0);
}

// A transmit path that refuses everything gets ONE recovery attempt, not one per second: a restart cannot fix an unplugged cable, and retrying would tear the interface down repeatedly under a user who is watching the card to find out what is wrong. Drive the failure streak past the wedge threshold. Split out because a test that does not REBUILD the streak between ticks proves nothing: the restart clears it, so the wedge branch is simply not re-entered and the guard under test never runs.
static void wedge(mm::PanelCardDriver& driver, uint32_t fromMs) {
    mm::platform::setTestEthSendFails(true);
    // Enough TICKS to clear the driver's 500-consecutive-failure wedge threshold. Each tick sends one brightness + rows + one sync, so the frame count per tick depends on the wall: 250 ticks is comfortably past 500 for the small walls these tests build, with headroom rather than an exact figure, because the point is that a long run of failures accumulates.
    for (int i = 0; i < 250; i++) {
        mm::platform::setTestNowMs(fromMs + i * 30);
        driver.tick();
    }
    REQUIRE(mm::platform::ethSendFailStreak() >= 500);
}

// A wedge earns ONE recovery attempt. The second and third wedges in the same episode must NOT fire again: a restart cannot fix an unplugged cable, and retrying would tear the interface down every tick under a user reading the card to find out what is wrong.
TEST_CASE("PanelCardDriver attempts recovery once per wedge") {
    clearClaims();
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);

    wedge(driver, 1000);
    const uint32_t before = mm::platform::ethRestartCountForTest();
    driver.tick1s();
    CHECK(mm::platform::ethRestartCountForTest() == before + 1);   // fired

    // Rebuild the streak so the wedge branch is genuinely re-entered; without this the guard is never reached and the assertion below would pass even with the guard deleted.
    wedge(driver, 20000);
    driver.tick1s();
    wedge(driver, 40000);
    driver.tick1s();
    CHECK(mm::platform::ethRestartCountForTest() == before + 1);   // and still only once

    mm::platform::setTestEthSendFails(false);
}

// A wedge that survives its restart is REPORTED, with the refusal count, rather than retried.
TEST_CASE("PanelCardDriver reports a wedge that survives its restart") {
    clearClaims();
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);

    wedge(driver, 5000);
    driver.tick1s();                       // fires the one restart
    wedge(driver, 25000);                  // still wedged afterwards
    driver.tick1s();

    CHECK(std::string(driver.status()).find("transmit wedged") != std::string::npos);
    mm::platform::setTestEthSendFails(false);
}

// A recovery that FAILS leaves the interface stopped, which no user action short of a power cycle resolves. That must not be reported as "no ethernet link", the message for an unplugged cable.
TEST_CASE("PanelCardDriver keeps a failed restart visible") {
    clearClaims();
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);

    mm::platform::setTestEthRestartFails(true);
    wedge(driver, 60000);
    driver.tick1s();
    CHECK(std::string(driver.status()).find("restart failed") != std::string::npos);

    // Latched: the next tick must not soften it to the link warning.
    driver.tick1s();
    CHECK(std::string(driver.status()).find("restart failed") != std::string::npos);

    mm::platform::setTestEthRestartFails(false);
    mm::platform::setTestEthSendFails(false);
}

// Failures are counted by CAUSE, because a down link and a full TX ring are different faults with different fixes; one total cannot tell them apart, which is what made a real bug unreadable.
TEST_CASE("PanelCardDriver counts send failures by cause") {
    clearClaims();
    mm::platform::ethTestClearFrames();
    mm::Buffer source;
    mm::PanelCardDriver driver;
    Wall wall(64, 1);
    setUp(driver, source, wall, 64);

    uint32_t linkDown = 0, ringFull = 0;
    mm::platform::ethSendFailCounts(linkDown, ringFull);
    CHECK(ringFull == 0);

    mm::platform::setTestEthSendFails(true);
    mm::platform::setTestNowMs(30000);
    driver.tick();
    mm::platform::setTestEthSendFails(false);

    mm::platform::ethSendFailCounts(linkDown, ringFull);
    CHECK(ringFull > 0);       // cumulative, not the streak; it survives a later success
    mm::platform::setTestNowMs(30100);
    driver.tick();
    uint32_t after = 0;
    mm::platform::ethSendFailCounts(linkDown, after);
    CHECK(after == ringFull);  // a success does not erase the history
}

// The `interface` Select lists the DETECTED host NICs (none-first) and persists by label, the fix for the Windows index-mismatch trap: whatever the OS or Npcap renumbers, the name the user picked keeps meaning that adapter. Enumeration is faked through the test seam.
TEST_CASE("the interface Select lists detected NICs, none first, bind names behind the rows") {
    static const char* kFake[] = {"en-test0", "en-test1"};
    mm::platform::setTestRawInterfaces(kFake, 2);

    mm::PanelCardDriver driver;
    driver.defineControls();

    // Find the control and pin its shape: 3 rows (none + 2 NICs), label persistence on.
    bool found = false;
    auto& cs = driver.controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "interface") != 0) continue;
        found = true;
        CHECK(cs[i].max == 3);
        CHECK(cs[i].persistLabel);
        auto* options = reinterpret_cast<const char* const*>(cs[i].aux);
        REQUIRE(options != nullptr);
        CHECK(std::strcmp(options[0], "none (capture only)") == 0);
        CHECK(std::strcmp(options[1], "en-test0") == 0);
    }
    CHECK(found);

    // The bind name behind each row: none maps to capture (nullptr), rows map to their NIC.
    CHECK(mm::platform::rawInterfaceName(0) == nullptr);
    CHECK(std::strcmp(mm::platform::rawInterfaceName(1), "en-test0") == 0);
    CHECK(std::strcmp(mm::platform::rawInterfaceName(2), "en-test1") == 0);

    mm::platform::setTestRawInterfaces(nullptr, 0);   // restore real enumeration
}

// A persisted adapter NAME whose NIC is gone must degrade to capture-only without crashing: the label no longer matches any option, the Select stays at row 0 (none), and prepare()'s nullptr bind is today's blank-interface capture path.
TEST_CASE("a vanished persisted NIC degrades to capture-only, never a crash") {
    static const char* kFake[] = {"en-test0"};
    mm::platform::setTestRawInterfaces(kFake, 1);

    mm::PanelCardDriver driver;
    driver.defineControls();
    auto& cs = driver.controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "interface") != 0) continue;
        // The config remembers an adapter this machine no longer has.
        const auto r = mm::applyControlValue(cs[i], "{\"interface\":\"en-vanished\"}",
                                             "interface", mm::ApplyPolicy::Clamp);
        CHECK(r == mm::ApplyResult::Ok);   // Lenient-shaped: the tree is left alone, no error
        CHECK(*static_cast<uint8_t*>(cs[i].ptr) == 0);   // still row 0: none (capture only)
    }
    CHECK(mm::platform::rawInterfaceName(0) == nullptr);   // and row 0 binds nothing

    mm::platform::setTestRawInterfaces(nullptr, 0);
}

// The NIC list is re-enumerated on every rebuild and the OS does not promise a stable order. The selection must follow the ADAPTER, not the row it happened to occupy: a NIC appearing ahead of the chosen one would otherwise silently move panel output to a different adapter.
TEST_CASE("a reordered NIC list keeps the selected adapter, not its old row") {
    static const char* kFirst[]  = {"en-alpha", "en-beta"};
    mm::platform::setTestRawInterfaces(kFirst, 2);

    mm::PanelCardDriver driver;
    driver.defineControls();
    {
        auto& cs = driver.controls();
        for (uint8_t i = 0; i < cs.count(); i++) {
            if (std::strcmp(cs[i].name, "interface") != 0) continue;
            const auto r = mm::applyControlValue(cs[i], "{\"interface\":\"en-beta\"}",
                                                 "interface", mm::ApplyPolicy::Clamp);
            CHECK(r == mm::ApplyResult::Ok);
        }
    }
    driver.prepare();   // picking an interface re-prepares, which is what records the choice

    // A NIC is hot-plugged AHEAD of the chosen one, so every later row shifts down.
    static const char* kAfter[] = {"en-new", "en-alpha", "en-beta"};
    mm::platform::setTestRawInterfaces(kAfter, 3);
    driver.defineControls();   // the rebuild that re-enumerates

    auto& cs = driver.controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "interface") != 0) continue;
        const uint8_t sel = *static_cast<uint8_t*>(cs[i].ptr);
        const char* bound = mm::platform::rawInterfaceName(sel);
        // Row 0 is "none (capture only)" and binds nothing, so landing there is the failure this guards against, not a reason to crash the run.
        REQUIRE(bound != nullptr);
        CHECK(std::strcmp(bound, "en-beta") == 0);
    }

    mm::platform::setTestRawInterfaces(nullptr, 0);
}

// Interface labels come from the OS, and on Windows they are free-form descriptions. One containing a quote or a backslash must not be able to break the JSON the whole UI loads from.
TEST_CASE("an interface label with JSON metacharacters keeps the schema parseable") {
    static const char* kOdd[] = {"Realtek \"Gaming\" 2.5GbE", "Intel\\Wi-Fi 6"};
    mm::platform::setTestRawInterfaces(kOdd, 2);

    mm::PanelCardDriver driver;
    driver.defineControls();
    auto& cs = driver.controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "interface") != 0) continue;
        char buf[512];
        mm::JsonSink sink(buf, sizeof(buf));
        mm::writeControlMetadata(sink, cs[i]);
        const std::string json(buf);
        // Every quote and backslash inside a label must arrive escaped.
        CHECK(json.find("\\\"Gaming\\\"") != std::string::npos);
        CHECK(json.find("Intel\\\\Wi-Fi") != std::string::npos);
    }

    mm::platform::setTestRawInterfaces(nullptr, 0);
}

// The documented reset for the test seam is (nullptr, 0); it must clear the override rather than form a range from a null pointer.
TEST_CASE("resetting the interface test seam with nullptr restores real enumeration") {
    static const char* kFake[] = {"en-test0"};
    mm::platform::setTestRawInterfaces(kFake, 1);
    const char* const* opts = nullptr;
    CHECK(mm::platform::rawInterfaces(&opts) == 2);   // row 0 + the fake

    mm::platform::setTestRawInterfaces(nullptr, 0);
    CHECK(mm::platform::rawInterfaces(&opts) >= 1);   // back to the host's own list
}

// An interface label carries the adapter's live link speed ("Realtek PCIe GbE, 1 Gb") so a user can tell a 1 Gb NIC from a Wi-Fi radio or a virtual switch. That detail CHANGES: a renegotiated link, or the same cable at 100 Mb, rewrites the label. The selection must survive it, or the driver silently falls back to capture-only the first time a link renegotiates.
TEST_CASE("a NIC whose link speed changed is still the same NIC") {
    static const char* kFast[] = {"en-alpha, 1 Gb", "en-beta, 2.5 Gb"};
    mm::platform::setTestRawInterfaces(kFast, 2);

    mm::PanelCardDriver driver;
    driver.defineControls();
    {
        auto& cs = driver.controls();
        for (uint8_t i = 0; i < cs.count(); i++) {
            if (std::strcmp(cs[i].name, "interface") != 0) continue;
            const auto r = mm::applyControlValue(cs[i], "{\"interface\":\"en-beta, 2.5 Gb\"}",
                                                 "interface", mm::ApplyPolicy::Clamp);
            CHECK(r == mm::ApplyResult::Ok);
        }
    }
    driver.prepare();

    // The link renegotiates: same adapter, different speed in its label.
    static const char* kSlow[] = {"en-alpha, 1 Gb", "en-beta, 100 Mb"};
    mm::platform::setTestRawInterfaces(kSlow, 2);
    driver.defineControls();

    auto& cs = driver.controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "interface") != 0) continue;
        const uint8_t sel = *static_cast<uint8_t*>(cs[i].ptr);
        const char* bound = mm::platform::rawInterfaceName(sel);
        REQUIRE(bound != nullptr);                    // row 0 binds nothing: the failure case
        CHECK(std::strncmp(bound, "en-beta", 7) == 0);
    }

    mm::platform::setTestRawInterfaces(nullptr, 0);
}

// The same rule on the APPLY path: a persisted label whose speed has since changed still selects its adapter, so a config restored onto a machine whose link renegotiated keeps working.
TEST_CASE("a persisted interface label matches its adapter despite a changed speed") {
    static const char* kNow[] = {"en-alpha, 100 Mb"};
    mm::platform::setTestRawInterfaces(kNow, 1);

    mm::PanelCardDriver driver;
    driver.defineControls();
    auto& cs = driver.controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "interface") != 0) continue;
        // The config was written when the link was up at 1 Gb.
        const auto r = mm::applyControlValue(cs[i], "{\"interface\":\"en-alpha, 1 Gb\"}",
                                             "interface", mm::ApplyPolicy::Clamp);
        CHECK(r == mm::ApplyResult::Ok);
        CHECK(*static_cast<uint8_t*>(cs[i].ptr) == 1);   // row 1, not the capture-only row 0
    }

    mm::platform::setTestRawInterfaces(nullptr, 0);
}

// An adapter that DISAPPEARS between rebuilds (unplugged USB NIC, a driver uninstall) must not leave the selection pointing at whatever now occupies that row: the driver would send panel data out of a NIC the user never chose. No match means capture-only, explicitly.
TEST_CASE("a NIC that disappeared falls back to capture-only, not to whoever took its row") {
    static const char* kBefore[] = {"en-alpha", "en-beta"};
    mm::platform::setTestRawInterfaces(kBefore, 2);

    mm::PanelCardDriver driver;
    driver.defineControls();
    {
        auto& cs = driver.controls();
        for (uint8_t i = 0; i < cs.count(); i++) {
            if (std::strcmp(cs[i].name, "interface") != 0) continue;
            const auto r = mm::applyControlValue(cs[i], "{\"interface\":\"en-beta\"}",
                                                 "interface", mm::ApplyPolicy::Clamp);
            CHECK(r == mm::ApplyResult::Ok);
        }
    }
    driver.prepare();   // records en-beta as the chosen adapter

    // en-beta is gone; en-gamma now sits at the row en-beta used to occupy.
    static const char* kAfter[] = {"en-alpha", "en-gamma"};
    mm::platform::setTestRawInterfaces(kAfter, 2);
    driver.defineControls();

    bool sawInterface = false;
    auto& cs = driver.controls();
    for (uint8_t i = 0; i < cs.count(); i++) {
        if (std::strcmp(cs[i].name, "interface") != 0) continue;
        sawInterface = true;
        CHECK(*static_cast<uint8_t*>(cs[i].ptr) == 0);            // row 0: none (capture only)
        CHECK(mm::platform::rawInterfaceName(0) == nullptr);      // and it binds nothing
    }
    CHECK(sawInterface);

    mm::platform::setTestRawInterfaces(nullptr, 0);
}
