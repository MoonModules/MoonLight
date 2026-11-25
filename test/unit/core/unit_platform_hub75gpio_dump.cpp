/// @module platform

/// Pins what the GPIO engine's refresh thread submits against the stub backend: the held steps and the dump's order of writes for a known frame, how brightness and dither reach the codes, and the carry's re-encodes. No hardware is touched; the stub captures every submit and every GPIO write.
///
/// @technical
///
/// ## Dump decoding
///
/// A clock edge is a clock-bit TRANSITION: SET carrying the clock bit while it was low (rising-edge panels) or CLR carrying it while it was high (falling-edge panels); the color bits in the shadow at the edge are the data that clock clocked in.
///
/// The color clear after each shift also carries the clock bit in its mask, but the clock is already at its resting level there, so it is not an edge. Groups are delimited by the pulse fire that ends each one.
///
/// ## Dump stream
///
/// The dump test pins the loop's write order and grouping: the address changes only between rows, the data is re-presented per clock, the latch follows the row's clocks, the OE wait (which blanks the panel) precedes the address change and the pulse fire follows the strobe.
///
/// The sign's column frame (one canvas column red) makes the per-clock data check crisp: clocks 7 and 71 carry red, every other clock is dark.
///
/// What only the sign can confirm is that this sequence cures the spreading: the VM has no panel, no slow-decay rows and no OE timing to observe.
///
/// The hold words and the clocked shift words are in hub75_gpio_dump.h (Row-major dump).

#include "doctest.h"
#include "platform/platform_base.h"   // the target's flags and MM_NONBLOCKING
#include "platform/rpi/hub75_gpio_backend.h"
#include "platform/rpi/hub75_gpio_backend_stub.h"
#include "platform/rpi/hub75_gpio_dither.h"
#include "platform/rpi/hub75_gpio_engine.h"
#include "platform/rpi/hub75_gpio_pins.h"
#include "hub75_engine_bench.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

using mm::test::gpioSignOptions;
using mm::test::gpioSmall;

namespace {

// The red bits (R1|R2) on all three lanes, as one GPIO mask.
uint32_t redMaskAllLanes() {
    uint32_t m = 0;
    for (int c = 0; c < 3; c++)
        m |= (1u << mm::platform::kLanePins[c].r1) |
             (1u << mm::platform::kLanePins[c].r2);
    return m;
}

/// One decoded dump group: the data the panel saw clocked in, the address it was latched at, and where the pulse wait and fire sat around it.
struct DecodedGroup {
    std::vector<uint32_t> clocked;   // color bits at each clock edge, in order
    int address = -1;                // the address shadow at the strobe
    int plane = -1;                  // the bitplane of the pulse fire
    size_t waitIdx = 0;              // stream index of the pulse wait
    size_t addrIdx = 0;              // stream index of the last address write (0 when unchanged)
    bool addrChanged = false;
    size_t strobeIdx = 0;            // stream index of the strobe's rising edge
    size_t fireIdx = 0;              // stream index of the pulse fire
};

// The row an address mask selects: A is bit 0, E is bit 4.
int rowFromAddressMask(uint32_t m) {
    int row = 0;
    if (m & (1u << mm::platform::shared::kA)) row |= 1;
    if (m & (1u << mm::platform::shared::kB)) row |= 2;
    if (m & (1u << mm::platform::shared::kC)) row |= 4;
    if (m & (1u << mm::platform::shared::kD)) row |= 8;
    if (m & (1u << mm::platform::shared::kE)) row |= 16;
    return row;
}

// The dump stream's decoder: a shadow of the GPIO outputs (CLR clears bits, SET sets them) and the group being built; the clock-edge rule is in @xref{dump-decoding|Technical → Dump decoding}.
class DumpDecoder {
  public:
    // A decoder for panels sampling on the falling clock edge when `falling`, else the rising one.
    explicit DumpDecoder(bool falling) : falling_(falling) {}

    // The groups the stream holds, each ended by the pulse fire that follows it.
    std::vector<DecodedGroup> decode(const std::vector<mm::platform::GpioDumpEvent>& events) {
        for (size_t i = 0; i < events.size(); i++) apply(events[i], i);
        return std::move(groups_);
    }

  private:
    using Kind = mm::platform::GpioDumpEvent::Kind;

    void apply(const mm::platform::GpioDumpEvent& e, size_t i) {
        switch (e.kind) {
            case Kind::Set: set(e.value, i); break;
            case Kind::Clr: clear(e.value, i); break;
            case Kind::PulseWait: cur_.waitIdx = i; break;
            case Kind::PulseFire: fire(e.value, i); break;
        }
    }

    // A SET: a rising edge when the clock bit goes high, then the address and the strobe it carries.
    void set(uint32_t mask, size_t i) {
        if (!falling_ && clockMoves(mask, false)) cur_.clocked.push_back(shadow_ & mm::platform::kGpioColorMask);
        shadow_ |= mask;
        noteAddress(mask, i);
        if ((mask & mm::platform::kGpioStbBit) != 0) {
            cur_.strobeIdx = i;
            cur_.address = rowFromAddressMask(shadow_ & mm::platform::kGpioAddressMask);
        }
    }

    // A CLR: a falling edge when the clock bit goes low, then the address it carries.
    void clear(uint32_t mask, size_t i) {
        if (falling_ && clockMoves(mask, true)) cur_.clocked.push_back(shadow_ & mm::platform::kGpioColorMask);
        shadow_ &= ~mask;
        noteAddress(mask, i);
    }

    // The pulse fire ends the group, which names the plane it fired.
    void fire(uint32_t plane, size_t i) {
        cur_.plane = static_cast<int>(plane);
        cur_.fireIdx = i;
        groups_.push_back(std::move(cur_));
        cur_ = DecodedGroup{};
    }

    // Whether `mask` carries the clock bit while the clock sits at `high`: the transition, not a rewrite of its resting level.
    bool clockMoves(uint32_t mask, bool high) const {
        return (mask & mm::platform::kGpioClkBit) != 0 && ((shadow_ & mm::platform::kGpioClkBit) != 0) == high;
    }

    // An address write moves the address index and marks the group's address as changed.
    void noteAddress(uint32_t mask, size_t i) {
        if ((mask & mm::platform::kGpioAddressMask) == 0) return;
        cur_.addrIdx = i;
        cur_.addrChanged = true;
    }

    bool falling_;
    uint32_t shadow_ = 0;
    DecodedGroup cur_;
    std::vector<DecodedGroup> groups_;
};

// Decode a dump stream into its groups.
std::vector<DecodedGroup> decodeDumpStream(const std::vector<mm::platform::GpioDumpEvent>& events, bool falling) {
    return DumpDecoder(falling).decode(events);
}

// A 16-column panel at 1/16 scan and the given depth: the row narrow enough that the encoder holds words past its shift words.
mm::platform::Hub75GpioOptions narrowAt(uint8_t bitDepth) {
    mm::platform::Hub75GpioOptions o;
    o.rows = 32;
    o.cols = 16;
    o.panelsPerLane = 1;
    o.parallel = 1;
    o.scanRate = 16;
    o.bitDepth = bitDepth;
    o.pwmLsbNs = 130;
    o.slowdown = -1;   // auto: the stub resolves it to 1
    return o;
}

// The stub's last frame as `slots` words, empty when the stub holds none.
std::vector<uint32_t> capturedFrame(size_t slots) {
    std::vector<uint32_t> words(slots);
    if (mm::platform::gpioStubCopyLastFrame(words.data(), words.size()) != slots) words.clear();
    return words;
}

// Once the stub holds a frame, publish `frame`, wait for a frame other than that one, then two more dumps, so the case reads the new frame and its dump stream.
void publishAndSettle(mm::platform::Hub75GpioEngine& engine, const std::vector<uint8_t>& frame) {
    const size_t slots = engine.frameSlots();
    REQUIRE(mm::test::waitUntil([&] { return !capturedFrame(slots).empty(); }, 5000));
    const std::vector<uint32_t> before = capturedFrame(slots);
    engine.publishRgb(frame.data());
    REQUIRE(mm::test::waitUntil([&] { const auto now = capturedFrame(slots); return !now.empty() && now != before; }, 5000));
    const uint64_t submitted = engine.framesSubmitted();
    REQUIRE(mm::test::waitUntil([&] { return engine.framesSubmitted() >= submitted + 2; }, 5000));
}

// The stub's last frame, which must hold exactly `slots` words.
std::vector<uint32_t> lastFrameOf(size_t slots) {
    std::vector<uint32_t> words(slots);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(words.data(), words.size()) == slots);
    return words;
}

// Every word of the sign's last frame against `lit`: the words it names equal `value`, the rest are dark, and `litCount` of them are not.
template <class Lit>
void checkSignFrame(Lit lit, uint32_t value, size_t litCount) {
    std::vector<uint32_t> last(6144);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) == last.size());
    size_t nonZero = 0;
    for (size_t k = 0; k < last.size(); k++) {   // 48 groups of 128 words
        nonZero += last[k] != 0;
        CHECK(last[k] == (lit(static_cast<int>(k / 128), static_cast<int>(k % 128)) ? value : 0u));
    }
    CHECK(nonZero == litCount);
}

constexpr bool kRisingEdge = false;    // the clock edge a panel samples on: the SET of the clock bit
constexpr bool kFallingEdge = true;    // or the CLR of it

// The stub's last dump stream, which must hold exactly `events` events, decoded into its groups; `capacity` is the buffer the stub copies into.
std::vector<DecodedGroup> decodedStream(size_t capacity, size_t events, bool falling) {
    std::vector<mm::platform::GpioDumpEvent> stream(capacity);
    REQUIRE(mm::platform::gpioStubCopyLastDumpStream(stream.data(), stream.size()) == events);
    stream.resize(events);
    return decodeDumpStream(stream, falling);
}

// The dark order of a group: the pulse wait (which disables OE) precedes the address change, the address change precedes the strobe, and the pulse fire follows the strobe.
void checkDarkOrder(const DecodedGroup& g) {
    CHECK(g.waitIdx < g.strobeIdx);
    CHECK(g.strobeIdx < g.fireIdx);
    if (g.addrChanged) {
        CHECK(g.waitIdx < g.addrIdx);
        CHECK(g.addrIdx < g.strobeIdx);
    }
}

// A group's place and dark order, for the three cases whose title is about something else; the walks and the falling-edge cases write the same checks inline, since their title promises them.
void checkGroupPlace(const DecodedGroup& g, int row, int plane) {
    CAPTURE(row);
    CAPTURE(plane);
    CHECK(g.plane == plane);
    CHECK(g.address == row);
    CHECK(g.addrChanged == (plane == 0));
    checkDarkOrder(g);
}

// The group's clocked data is `shiftWords` words of the frame from `base`, color bits only, word for word.
void checkClockedWords(const DecodedGroup& g, const std::vector<uint32_t>& words, size_t base, size_t shiftWords) {
    REQUIRE(g.clocked.size() == shiftWords);
    for (size_t i = 0; i < shiftWords; i++)
        CHECK(g.clocked[i] == (words[base + i] & mm::platform::kGpioColorMask));
}

}  // namespace

TEST_CASE("Hub75GpioEngine: the sign's held steps encode one row / one column exactly") {
    // BENCH-V4's lab steps, pinned as words: the probe that cleared the encode path (one canvas row lit, one canvas column lit, the even-fill control), so an encode, Stripe or repack change that smears a held step fails here instead of on the sign.
    mm::test::GpioRun run(gpioSignOptions());
    auto& engine = run.engine;
    REQUIRE(engine.frameSlots() == 6144);

    const uint32_t red = redMaskAllLanes();
    const uint32_t greenCh0 = 1u << mm::platform::kLanePins[0].g1;

    // Frame A: canvas row 5 (lane 0, logical row 5), all 64 cols, green 255; under Stripe it lands in step 5 of every plane, driven columns 64..127, on lane 0's G1 line only: 6 groups x 64 words.
    {
        std::vector<uint8_t> frame(64 * 96 * 3, 0);
        for (int x = 0; x < 64; x++) frame[(5 * 64 + x) * 3 + 1] = 255;
        publishAndSettle(engine, frame);
        checkSignFrame([](int g, int i) { return g % 8 == 5 && i >= 64; }, greenCh0, 384);
    }

    // Frame B: canvas column 7, all 96 rows, red 255: every group's clocks 7 and 71 carry red on all lanes, every other clock is dark.
    {
        std::vector<uint8_t> frame(64 * 96 * 3, 0);
        for (int y = 0; y < 96; y++) frame[(y * 64 + 7) * 3 + 0] = 255;
        publishAndSettle(engine, frame);
        checkSignFrame([](int, int i) { return i == 7 || i == 71; }, red, 96);
    }

    // Frame C: full red fill, the even-fill control -- every word red.
    {
        std::vector<uint8_t> frame(64 * 96 * 3, 0);
        for (size_t i = 0; i < frame.size(); i += 3) frame[i] = 255;
        publishAndSettle(engine, frame);
        checkSignFrame([](int, int) { return true; }, red, 6144);
    }
}

TEST_CASE("Hub75GpioEngine: the dump walks rows outer, planes inner, and latches in the dark") {
    // BENCH-V4's fix, pinned: the dump sequence the hardware backend drives, read from the stub's recorder (the same shared loop, hub75_gpio_dump.h, not a copy); see @xref{dump-stream|Technical → Dump stream}.
    mm::test::GpioRun run(gpioSignOptions());
    auto& engine = run.engine;
    REQUIRE(engine.frameSlots() == 6144);

    std::vector<uint8_t> frame(64 * 96 * 3, 0);
    for (int y = 0; y < 96; y++) frame[(y * 64 + 7) * 3 + 0] = 255;
    publishAndSettle(engine, frame);

    // The stream: 48 groups x (128 clocks x 3 writes + color clear + pulse wait + strobe set/clear + pulse fire) + 8 address writes (one per row, CLR + SET) = 18,688 events, exactly, which pins the write sequence as well as the grouping.
    const auto groups = decodedStream(20000, 18688, kRisingEdge);
    const auto words = lastFrameOf(6144);
    REQUIRE(groups.size() == 48);   // 8 rows x 6 planes

    const uint32_t red = redMaskAllLanes();
    const uint32_t color = mm::platform::kGpioColorMask;
    for (int k = 0; k < 48; k++) {
        const int row = k / 6, plane = k % 6;
        const DecodedGroup& g = groups[k];
        CAPTURE(row);
        CAPTURE(plane);
        // Row-major: the planes cycle within each row, and the strobe's address is the row, each address held for 6 groups.
        CHECK(g.plane == plane);
        CHECK(g.address == row);
        // The address is written only at the row's first plane; a row's later planes share it.
        CHECK(g.addrChanged == (plane == 0));
        // The row's full clock count, and the data re-presented per clock: the clocked data is the frame's group for this (row, plane), group (plane * 8 + row) of the plane-major frame, word for word.
        REQUIRE(g.clocked.size() == 128);
        const size_t base = (static_cast<size_t>(plane) * 8 + row) * 128;
        for (int i = 0; i < 128; i++)
            CHECK(g.clocked[i] == (words[base + i] & color));
        // The column frame's crisp pattern, in the clocked data itself: red at clocks 7 and 71, dark everywhere else.
        CHECK(g.clocked[7] == red);
        CHECK(g.clocked[71] == red);
        CHECK(g.clocked[0] == 0u);
        CHECK(g.clocked[127] == 0u);
        // The dark order: the pulse wait (which disables OE) precedes the address change, the address change precedes the strobe, and the pulse fire follows the strobe.
        checkDarkOrder(g);
    }
    // The address sequence, deduped: 0..7, each held for one row.
    {
        std::vector<int> addrs;
        for (const DecodedGroup& g : groups)
            if (addrs.empty() || addrs.back() != g.address) addrs.push_back(g.address);
        CHECK(addrs.size() == 8);
        for (int r = 0; r < 8; r++) CHECK(addrs[r] == r);
    }
}

TEST_CASE("Hub75GpioEngine: the falling-edge dump clocks on the falling edge, in the same order") {
    // The inverted branch of the shared loop (hub75_gpio_dump.h), on the small geometry: the clock edge is the CLR, the data is set with the clock high, and the row-major order, the dark latch and the per-clock data hold as in the rising case.
    mm::platform::Hub75GpioOptions o = gpioSmall();
    o.clockFallingEdge = kFallingEdge;
    mm::test::GpioRun run(o);
    auto& engine = run.engine;
    REQUIRE(engine.frameSlots() == 2048);   // 2 planes x 16 rows x 64 columns

    // One red pixel at (0,0): the top plane's first word carries red.
    std::vector<uint8_t> frame(64 * 32 * 3, 0);
    frame[0] = 255;
    publishAndSettle(engine, frame);

    // 32 groups x (64 clocks x 3 writes + 5) + 16 address writes = 6,336 events.
    const auto groups = decodedStream(8000, 6336, kFallingEdge);
    const auto words = lastFrameOf(2048);
    REQUIRE(groups.size() == 32);   // 16 rows x 2 planes
    const uint32_t color = mm::platform::kGpioColorMask;
    for (int k = 0; k < 32; k++) {
        const int row = k / 2, plane = k % 2;
        const DecodedGroup& g = groups[k];
        CAPTURE(row);
        CAPTURE(plane);
        CHECK(g.plane == plane);
        CHECK(g.address == row);
        CHECK(g.addrChanged == (plane == 0));
        REQUIRE(g.clocked.size() == 64);
        const size_t base = (static_cast<size_t>(plane) * 16 + row) * 64;
        for (int i = 0; i < 64; i++)
            CHECK(g.clocked[i] == (words[base + i] & color));
        checkDarkOrder(g);
    }
}

TEST_CASE("Hub75GpioEngine: the dump strides by the pass and clocks only the shift words, skipping the hold") {
    // A 16-column row at 6 bits holds 17 words: the frame's groups are 33 words, and the dump clocks only the 16 shift words of each, word for word past the hold (hub75_gpio_dump.h, Row-major dump).
    mm::test::GpioRun run(narrowAt(6));
    auto& engine = run.engine;
    REQUIRE(engine.frameSlots() == 3168);   // 6 planes x 16 rows x a 33-word pass

    std::vector<uint8_t> frame(16 * 32 * 3, 0);
    for (int y = 0; y < 32; y++) frame[(y * 16 + 7) * 3 + 0] = 255;   // one canvas column red
    publishAndSettle(engine, frame);

    // The stream: 96 groups x (16 clocks x 3 writes + color clear + pulse wait + strobe set/clear + pulse fire) + 16 address writes (one per row, CLR + SET) = 5,120 events; clocking the hold too would make each group 33 clocks and the count 10,016.
    const auto groups = decodedStream(6000, 5120, kRisingEdge);
    const auto words = lastFrameOf(3168);
    REQUIRE(groups.size() == 96);   // 16 rows x 6 planes

    const uint32_t red = (1u << mm::platform::kLanePins[0].r1) | (1u << mm::platform::kLanePins[0].r2);
    const uint32_t color = mm::platform::kGpioColorMask;
    for (int k = 0; k < 96; k++) {
        const int row = k / 6, plane = k % 6;
        const DecodedGroup& g = groups[k];
        checkGroupPlace(g, row, plane);
        // The clocked data is the frame's group for this (row, plane) -- the frame is plane-major, so group (plane * 16 + row) -- past its 17 hold words, word for word.
        REQUIRE(g.clocked.size() == 16);
        const size_t base = (static_cast<size_t>(plane) * 16 + row) * 33;
        for (int i = 0; i < 16; i++)
            CHECK(g.clocked[i] == (words[base + 17 + i] & color));
        // The column frame's crisp pattern: red at clock 7, dark everywhere else.
        CHECK(g.clocked[7] == red);
        CHECK(g.clocked[0] == 0u);
        CHECK(g.clocked[15] == 0u);
    }
}

TEST_CASE("Hub75GpioEngine: the sign at 8-bit lights every plane, no hold, vectors exact") {
    // Stripe doubles the sign's 64 columns to 128 a row group, above the 8-bit hold trigger of 64: the frame is 8 x 8 x 128 shift words (8,192); all-white lights every word, with no zero plane and no floor, and the dump clocks all eight planes row-major.
    auto o = gpioSignOptions();
    o.bitDepth = 8;
    mm::test::GpioRun run(o);
    auto& engine = run.engine;
    REQUIRE(engine.frameSlots() == 8192);   // 8 planes x 8 rows x 128 columns, no hold words
    CHECK(std::abs(engine.refreshHz() - 1e9f / 2055424.0f) < 1.0f);   // slowdown 2, ~487 Hz

    std::vector<uint8_t> frame(64 * 96 * 3, 255);   // all white: every bit of every byte set
    publishAndSettle(engine, frame);

    std::vector<uint32_t> words(8192);
    REQUIRE(mm::platform::gpioStubCopyLastFrame(words.data(), words.size()) == 8192);
    const uint32_t color = mm::platform::kGpioColorMask;
    for (int p = 0; p < 8; p++) {
        for (int r = 0; r < 8; r++) {
            const size_t base = (static_cast<size_t>(p) * 8 + r) * 128;
            uint32_t any = 0;
            for (int i = 0; i < 128; i++) {
                CHECK(words[base + i] == color);   // every word lit, every plane
                any |= words[base + i];
            }
            CHECK(any != 0u);   // no zero plane
        }
    }

    // The stream: 64 groups x (128 clocks x 3 writes + color clear + pulse wait + strobe set/clear + pulse fire) + 8 address writes (one per row, CLR + SET) = 24,912 events, exactly.
    const auto groups = decodedStream(26000, 24912, kRisingEdge);
    REQUIRE(groups.size() == 64);   // 8 rows x 8 planes
    for (int k = 0; k < 64; k++) {
        const int row = k / 8, plane = k % 8;
        const DecodedGroup& g = groups[k];
        checkGroupPlace(g, row, plane);
        checkClockedWords(g, words, (static_cast<size_t>(plane) * 8 + row) * 128, 128);
    }
}

TEST_CASE("Hub75GpioEngine: the dump at 8-bit strides by the pass and clocks only the shift words, past a 113-word hold") {
    // A 16-column row at 8 bits holds 113 words: the groups are 129 words, and the dump clocks only the 16 shift words of each on all eight planes, the red column lighting every one, so no plane is dead on a narrow row (hub75_gpio_dump.h, Row-major dump).
    mm::test::GpioRun run(narrowAt(8));
    auto& engine = run.engine;
    REQUIRE(engine.frameSlots() == 16512);   // 8 planes x 16 rows x a 129-word pass

    std::vector<uint8_t> frame(16 * 32 * 3, 0);
    for (int y = 0; y < 32; y++) frame[(y * 16 + 7) * 3 + 0] = 255;   // one canvas column red
    publishAndSettle(engine, frame);

    // The stream: 128 groups x (16 clocks x 3 writes + color clear + pulse wait + strobe set/clear + pulse fire) + 16 address writes (one per row, CLR + SET) = 6,816 events; clocking the hold too would make each group 129 clocks and the count 50,208.
    const auto groups = decodedStream(8000, 6816, kRisingEdge);
    const auto words = lastFrameOf(16512);
    REQUIRE(groups.size() == 128);   // 16 rows x 8 planes

    const uint32_t red = (1u << mm::platform::kLanePins[0].r1) | (1u << mm::platform::kLanePins[0].r2);
    const uint32_t color = mm::platform::kGpioColorMask;
    for (int k = 0; k < 128; k++) {
        const int row = k / 8, plane = k % 8;
        const DecodedGroup& g = groups[k];
        checkGroupPlace(g, row, plane);
        // The clocked data is the frame's group for this (row, plane) -- the frame is plane-major, so group (plane * 16 + row) -- past its 113 hold words, word for word.
        REQUIRE(g.clocked.size() == 16);
        const size_t base = (static_cast<size_t>(plane) * 16 + row) * 129;
        for (int i = 0; i < 16; i++)
            CHECK(g.clocked[i] == (words[base + 113 + i] & color));
        // The column frame's crisp pattern: red at clock 7, dark everywhere else, on every plane.
        CHECK(g.clocked[7] == red);
        CHECK(g.clocked[0] == 0u);
        CHECK(g.clocked[15] == 0u);
    }
}

namespace {

// The dither table at a brightness, depth and fraction bits.
std::array<uint16_t, 256> stepsAt(uint8_t brightness, uint8_t bitDepth, uint8_t bits) {
    std::array<uint16_t, 256> t{};
    mm::platform::hub75DitherTable(t, brightness, bitDepth, bits);
    return t;
}

// The shown level of one code after one dithered refresh, its remainder carried in `rem`.
uint32_t ditherOnce(uint8_t code, uint8_t& rem, const std::array<uint16_t, 256>& t, uint8_t bitDepth, uint8_t bits) {
    uint8_t c = code;
    mm::platform::hub75DitherScale(std::span<uint8_t>(&c, 1), std::span<uint8_t>(&rem, 1), t, bitDepth, bits);
    return c >> (8 - bitDepth);
}

// The red levels of pixels (0,0) and (1,0) in one last frame of gpioSmall(), 2-bit: plane p's row 0 starts at slot p x 1024, column x at slot x, R1 of lane 0 is GPIO 11.
std::array<int, 2> redLevelsAt0010() {
    std::vector<uint32_t> last(2048);
    if (mm::platform::gpioStubCopyLastFrame(last.data(), last.size()) != 2048) return {-1, -1};
    auto level = [&](size_t x) { return static_cast<int>(((last[1024 + x] >> 11) & 1u) * 2 + ((last[x] >> 11) & 1u)); };
    return {level(0), level(1)};
}

// Pixel (0,0)'s red level in the last frame of gpioSmall().
int redLevelAt00() { return redLevelsAt0010()[0]; }

// Wait until the engine has submitted `n` more frames.
bool waitSubmits(mm::platform::Hub75GpioEngine& e, uint64_t n) {
    const uint64_t target = e.framesSubmitted() + n;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (e.framesSubmitted() < target && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return e.framesSubmitted() >= target;
}

// Whether the engine encoded at most a few over once a refresh since (`encoded`, `submitted`): a free-running encode thread breaks the bound, and a late encode only lowers the count.
bool encodedOncePerRefresh(mm::platform::Hub75GpioEngine& e, uint64_t encoded, uint64_t submitted) {
    constexpr uint64_t kSlack = 4;
    const uint64_t encodes = e.framesEncoded() - encoded;   // read before the submits, which only grow, so a preempted test thread leaves the bound true
    return encodes <= e.framesSubmitted() - submitted + kSlack;
}

// Wait for the changed setting to settle, then for 16 more refreshes, and say whether they were encoded about once each.
bool encodesOncePerRefresh(mm::platform::Hub75GpioEngine& engine) {
    REQUIRE(waitSubmits(engine, 2));
    const uint64_t encoded = engine.framesEncoded();
    const uint64_t submitted = engine.framesSubmitted();
    REQUIRE(waitSubmits(engine, 16));
    return encodedOncePerRefresh(engine, encoded, submitted);
}

// gpioSmall()'s frame with pixel (0,0) red `red`, the rest dark.
std::vector<uint8_t> redPixelFrame(uint8_t red) {
    std::vector<uint8_t> f(64 * 32 * 3, 0);
    f[0] = red;
    return f;
}

// gpioSmall()'s frame with pixels (0,0) and (1,0) both red `red`, which the seed starts half a cycle apart at 2 and 3 bits.
std::vector<uint8_t> redPairFrame(uint8_t red) {
    auto f = redPixelFrame(red);
    f[3] = red;
    return f;
}

// Whether frames sampled across refreshes each show the pair at levels 1 and 2 in opposite phase, both orders seen within 5 s; one frame out of phase fails at once.
bool pairInOppositePhase() {
    bool first = false, second = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!(first && second) && std::chrono::steady_clock::now() < deadline) {
        const auto lv = redLevelsAt0010();
        if (lv[0] + lv[1] != 3 || (lv[0] != 1 && lv[0] != 2)) return false;
        (lv[0] == 1 ? first : second) = true;
        std::this_thread::sleep_for(std::chrono::microseconds(700));
    }
    return first && second;
}

// gpioSmall() at another depth.
mm::platform::Hub75GpioOptions smallAt(uint8_t bitDepth) {
    auto o = gpioSmall();
    o.bitDepth = bitDepth;
    return o;
}

}  // namespace

TEST_CASE("Hub75GpioDither: at brightness 255 and 8 bits every code is a whole level, so nothing carries") {
    for (uint8_t bits = 0; bits <= 3; bits++) {
        const auto t = stepsAt(255, 8, bits);
        for (uint32_t v = 0; v < 256; v++) CHECK(t[v] == (v << bits));
    }
}

namespace {

// Why `code` at `seed` does not cycle, or empty when over 2^bits refreshes its shown levels sum to its steps and the remainder returns to the seed.
std::string cycleFailure(uint8_t code, uint8_t seed, const std::array<uint16_t, 256>& t, uint8_t depth, uint8_t bits) {
    uint8_t rem = seed;
    uint32_t sum = 0;
    for (uint32_t r = 0; r < (1u << bits); r++) sum += ditherOnce(code, rem, t, depth, bits);
    if (sum == t[code] && rem == seed) return {};
    return "code " + std::to_string(code) + " seed " + std::to_string(seed) + " summed " + std::to_string(sum) + " of " + std::to_string(t[code]) + ", remainder " + std::to_string(rem);
}

// The first code and seed at `brightness` that do not cycle, with the brightness, or empty when all do; a code pinned at the top is its own case.
std::string brightnessFailure(uint8_t brightness, uint8_t depth, uint8_t bits) {
    const auto t = stepsAt(brightness, depth, bits);
    const uint32_t period = 1u << bits;
    const uint32_t top = (1u << depth) - 1;
    for (uint32_t v = 0; v < 256; v++) {
        if (t[v] + period - 1 >= (top + 1) * period) continue;
        for (uint8_t seed = 0; seed < period; seed++) {
            const std::string why = cycleFailure(static_cast<uint8_t>(v), seed, t, depth, bits);
            if (!why.empty()) return "brightness " + std::to_string(brightness) + ": " + why;
        }
    }
    return {};
}

// The first brightness at `depth` and `bits` with a code that does not cycle, or empty when none does.
std::string cycleFailureAt(uint8_t depth, uint8_t bits) {
    for (uint32_t b = 0; b < 256; b++) {
        const std::string why = brightnessFailure(static_cast<uint8_t>(b), depth, bits);
        if (!why.empty()) return why;
    }
    return {};
}

}  // namespace

TEST_CASE("Hub75GpioDither: over 2^bits refreshes the shown levels sum to the steps each code holds, at every depth, brightness, bit count and seed") {
    for (uint8_t depth = 2; depth <= 8; depth++) {
        for (uint8_t bits = 1; bits <= 3; bits++) {
            CAPTURE(depth);
            CAPTURE(bits);
            CHECK(cycleFailureAt(depth, bits) == "");   // the pattern repeats every 2^bits refreshes
        }
    }
}

TEST_CASE("Hub75GpioDither: a pixel pinned at the top shows the top level and keeps its phase, so a dim area after white still shimmers as grain") {
    // A 4x4 patch at 6 bits, 3 bits of fraction: one refresh of white, then a flat dim code at brightness 255.
    const uint8_t bits = 3;
    std::vector<uint8_t> rem(4 * 4 * 3);
    mm::platform::hub75DitherSeed(rem, 4, bits);
    const auto t = stepsAt(255, 6, bits);
    std::vector<uint8_t> white(rem.size(), 255);
    mm::platform::hub75DitherScale(white, rem, t, 6, bits);
    for (const uint8_t c : white) CHECK(c == 252);   // level 63 of 6 bits, the top
    for (int r = 0; r < 8; r++) {
        std::vector<uint8_t> dim(rem.size(), 2);   // half a level at 6 bits: 4 of 8 steps
        mm::platform::hub75DitherScale(dim, rem, t, 6, bits);
        int lit = 0;
        for (const uint8_t c : dim) lit += c > 0 ? 1 : 0;
        CAPTURE(r);
        CHECK(lit == 24);   // half the 48 channels on every refresh, not all of them on alternate ones
    }
}

TEST_CASE("Hub75GpioDither: at brightness 64 two codes truncation shows alike stay apart with the dither, at 8 and at 6 bits") {
    for (const uint8_t depth : {static_cast<uint8_t>(8), static_cast<uint8_t>(6)}) {
        CAPTURE(depth);
        const uint8_t bits = 2;
        const auto t = stepsAt(64, depth, bits);
        const double levelCodes = static_cast<double>(1u << (8 - depth));
        auto average = [&](uint8_t v) {
            uint8_t rem = 2;
            uint32_t sum = 0;
            for (int r = 0; r < 4; r++) sum += ditherOnce(v, rem, t, depth, bits);
            return sum / 4.0;
        };
        // Two codes three steps apart, which truncation shows alike: 16 and 19 at 8 bits, 16 and 28 at 6.
        const uint8_t lo = 16, hi = static_cast<uint8_t>(16 + 3 * static_cast<int>(levelCodes));
        const auto off = stepsAt(64, depth, 0);
        uint8_t truncated[2] = {lo, hi};
        uint8_t none[2] = {0, 0};
        mm::platform::hub75DitherScale(truncated, none, off, depth, 0);
        CHECK(truncated[0] == truncated[1]);   // truncation shows them as one code
        CHECK(average(hi) > average(lo));      // with it they stay apart
    }
}

TEST_CASE("Hub75GpioDither: the seed staggers neighbors on a 4x4 pattern in steps of the fraction, the channels a column apart") {
    std::vector<uint8_t> rem(4 * 4 * 3);
    mm::platform::hub75DitherSeed(rem, 4, 3);
    CHECK(rem[0] == 0);       // (0,0) red: Bayer 0
    CHECK(rem[1] == 4);       // (0,0) green: one column on, Bayer 8
    CHECK(rem[3] == 4);       // (1,0) red: Bayer 8
    CHECK(rem[4 * 3] == 6);   // (0,1) red: Bayer 12
    for (const uint8_t r : rem) CHECK(r < 8);
    mm::platform::hub75DitherSeed(rem, 4, 0);
    for (const uint8_t r : rem) CHECK(r == 0);   // at 0 bits every remainder starts at 0
}

TEST_CASE("Hub75GpioEngine: brightness scales the codes, and 0 carry bits drop the fraction") {
    mm::test::GpioRun run(gpioSmall());
    auto& engine = run.engine;
    engine.setCorrection(128, 0);
    const auto frame = redPixelFrame(200);   // 200 x 128 / 255 is 100.4 codes, 1.57 levels of 64
    engine.publishRgb(frame.data());
    REQUIRE(waitSubmits(engine, 4));
    CHECK(redLevelAt00() == 1);
}

TEST_CASE("Hub75GpioEngine: a brightness change with no new frame re-encodes the held frame once") {
    mm::test::GpioRun run(gpioSmall());
    auto& engine = run.engine;
    engine.setCorrection(255, 0);
    const auto frame = redPixelFrame(255);
    engine.publishRgb(frame.data());
    REQUIRE(waitSubmits(engine, 4));
    CHECK(redLevelAt00() == 3);
    const uint64_t encoded = engine.framesEncoded();
    engine.setCorrection(100, 0);   // 255 x 100 / 255 is 100 codes: level 1
    REQUIRE(waitSubmits(engine, 8));
    CHECK(engine.framesEncoded() == encoded + 1);
    CHECK(redLevelAt00() == 1);
}

TEST_CASE("Hub75GpioEngine: turning the carry on below 255 re-encodes once a refresh with the pair seeded apart, and turning it off shows both truncated") {
    mm::test::GpioRun run(gpioSmall());
    auto& engine = run.engine;
    engine.setCorrection(128, 0);
    const auto frame = redPairFrame(200);   // 6 quarters, 1.5 levels each
    engine.publishRgb(frame.data());
    REQUIRE(waitSubmits(engine, 4));
    uint64_t encoded = engine.framesEncoded();
    REQUIRE(waitSubmits(engine, 8));
    CHECK(engine.framesEncoded() == encoded);   // 0 bits: the held frame only re-submits
    engine.setCorrection(128, 2);
    CHECK(encodesOncePerRefresh(engine));
    CHECK(pairInOppositePhase());   // the change reseeded the pair apart, after 0 bits left every remainder at 0
    engine.setCorrection(128, 0);
    REQUIRE(waitSubmits(engine, 2));
    encoded = engine.framesEncoded();
    REQUIRE(waitSubmits(engine, 8));
    CHECK(engine.framesEncoded() == encoded);
    CHECK(redLevelsAt0010() == std::array<int, 2>{1, 1});   // both back to the truncated level, the fraction dropped
}

TEST_CASE("Hub75GpioEngine: at 8 bits and brightness 255 the dither re-encodes nothing, a change of bits re-encodes once, and below 255 it re-encodes once a refresh") {
    mm::test::GpioRun run(smallAt(8));
    auto& engine = run.engine;
    engine.setCorrection(255, 2);
    const auto frame = redPixelFrame(200);
    engine.publishRgb(frame.data());
    REQUIRE(waitSubmits(engine, 4));
    uint64_t encoded = engine.framesEncoded();
    REQUIRE(waitSubmits(engine, 8));
    CHECK(engine.framesEncoded() == encoded);   // every code a whole level: the held frame re-submits
    engine.setCorrection(255, 0);   // a change of bits with whole codes on both sides: one re-encode, with the reseed
    REQUIRE(waitSubmits(engine, 8));
    CHECK(engine.framesEncoded() == encoded + 1);
    engine.setCorrection(128, 2);
    CHECK(encodesOncePerRefresh(engine));
}

TEST_CASE("Hub75GpioEngine: the dither re-encodes once a refresh, and a pair seeded apart shows 1 and 2 in turn, averaging the scaled 1.5") {
    mm::test::GpioRun run(gpioSmall());
    auto& engine = run.engine;
    engine.setCorrection(128, 3);
    const auto frame = redPairFrame(200);   // 12 eighths, 1.5 levels: the seed starts (0,0) at 0 and (1,0) at 4, half a cycle apart
    engine.publishRgb(frame.data());
    REQUIRE(waitSubmits(engine, 4));
    const uint64_t encoded = engine.framesEncoded();
    const uint64_t submitted = engine.framesSubmitted();
    CHECK(pairInOppositePhase());
    CHECK(encodedOncePerRefresh(engine, encoded, submitted));
}

TEST_CASE("Hub75GpioEngine: at brightness 255 below 8 bits the default carry re-encodes once a refresh and shows the bits the planes drop, seeded apart from the open") {
    mm::test::GpioRun run(gpioSmall());
    auto& engine = run.engine;
    engine.setCorrection(255, mm::platform::kHub75GpioCarryBitsDefault);
    const auto frame = redPairFrame(96);   // 1.5 levels of 64: truncation shows 1, the dither alternates 1 and 2, the pair half a cycle apart
    engine.publishRgb(frame.data());
    REQUIRE(waitSubmits(engine, 4));
    const uint64_t encoded = engine.framesEncoded();
    const uint64_t submitted = engine.framesSubmitted();
    CHECK(pairInOppositePhase());   // the bits stay at the default here, so the seed at open alone sets the pair apart
    CHECK(encodedOncePerRefresh(engine, encoded, submitted));
}

TEST_CASE("Hub75GpioEngine: carry bits above the maximum are clamped into range") {
    mm::test::GpioRun run(gpioSmall());
    auto& engine = run.engine;
    engine.setCorrection(255, 9);   // as 3: 12 eighths, 1.5 levels; unclamped, the seed would shift by a negative count
    const auto frame = redPairFrame(96);
    engine.publishRgb(frame.data());
    REQUIRE(waitSubmits(engine, 4));
    CHECK(pairInOppositePhase());
}

// The sign's last frame: 6 planes x 8 address rows x 128 driven columns, empty when the stub holds none.
std::vector<uint32_t> signLastFrame() {
    std::vector<uint32_t> f(6144);
    if (mm::platform::gpioStubCopyLastFrame(f.data(), f.size()) != f.size()) f.clear();
    return f;
}

// Lane `lane`'s red level at driven row `row`, column `x` of a sign frame, from its two lowest planes: rows 0 to 7 on R1, 8 to 15 on R2.
int signRedLevel(const std::vector<uint32_t>& f, int lane, int row, int x) {
    const auto& pins = mm::platform::kLanePins[lane];
    const uint8_t pin = row < 8 ? pins.r1 : pins.r2;
    const size_t at = static_cast<size_t>(row % 8) * 128 + static_cast<size_t>(x);
    return static_cast<int>(((f[1024 + at] >> pin) & 1u) * 2 + ((f[at] >> pin) & 1u));
}

// How many driven pixels of a sign frame the three lanes show at different red levels.
int lanesDiffering(const std::vector<uint32_t>& f) {
    int differ = 0;
    for (int k = 0; k < 16 * 128; k++) {
        const int row = k / 128, x = k % 128;
        const int level = signRedLevel(f, 0, row, x);
        if (signRedLevel(f, 1, row, x) != level || signRedLevel(f, 2, row, x) != level) differ++;
    }
    return differ;
}

TEST_CASE("Hub75GpioEngine: on the sign's three Stripe lanes each lane carries its own fractions, and the seed staggers the driven rows") {
    mm::test::GpioRun run(gpioSignOptions());
    auto& engine = run.engine;
    engine.setCorrection(255, mm::platform::kHub75GpioCarryBitsDefault);
    std::vector<uint8_t> frame(64 * 96 * 3, 0);
    for (size_t i = 0; i < frame.size(); i += 3) frame[i] = 6;   // 6 quarters of a 6-bit level: every pixel shows 1 and 2 in turn
    engine.publishRgb(frame.data());
    REQUIRE(waitSubmits(engine, 4));
    for (int k = 0; k < 20; k++) {
        const auto f = signLastFrame();
        REQUIRE(f.size() == 6144);
        const int differ = lanesDiffering(f);
        CHECK(differ == 0);   // one remainder shared by the lanes would put lane 1 half a cycle from lane 0
        const int top = signRedLevel(f, 0, 0, 0);
        const int next = signRedLevel(f, 0, 1, 0);
        CHECK(top + next == 3);   // Bayer rows 0 and 1 seed 0 and 3 when the seed runs on the driven width, half a cycle apart
        std::this_thread::sleep_for(std::chrono::microseconds(700));
    }
}

TEST_CASE("Hub75GpioEngine: brightness 0 is dark and re-encodes nothing while dithering") {
    mm::test::GpioRun run(gpioSmall());
    auto& engine = run.engine;
    engine.setCorrection(0, 2);
    const auto frame = redPixelFrame(255);
    engine.publishRgb(frame.data());
    REQUIRE(waitSubmits(engine, 4));
    const uint64_t encoded = engine.framesEncoded();
    REQUIRE(waitSubmits(engine, 8));
    CHECK(engine.framesEncoded() == encoded);
    CHECK(redLevelAt00() == 0);
}

TEST_CASE("Hub75GpioEngine: a reopen shows the next frame at the brightness last set") {
    mm::test::GpioRun run(gpioSmall());
    auto& engine = run.engine;
    engine.setCorrection(128, 0);
    engine.shutdown();
    REQUIRE(engine.open(gpioSmall(), run.error, true));
    const auto frame = redPixelFrame(200);
    engine.publishRgb(frame.data());
    REQUIRE(waitSubmits(engine, 4));
    CHECK(redLevelAt00() == 1);
}

TEST_CASE("Hub75GpioEngine: a frame whose encode fails keeps the last good frame submitting, the held frame dumping again") {
    mm::test::GpioRun run(gpioSmall());
    auto& engine = run.engine;
    engine.setCorrection(128, 2);   // the carry moves a fraction, so every refresh wants an encode
    const auto frame = redPixelFrame(200);
    engine.publishRgb(frame.data());
    REQUIRE(waitSubmits(engine, 4));
    engine.failEncodes(-1);   // every encode fails from here
    const uint64_t submitted = engine.framesSubmitted();
    REQUIRE(waitSubmits(engine, 3));
    CHECK(engine.framesSubmitted() >= submitted + 3);
    CHECK(engine.submitErrors() == 0);   // a stale picture beats a torn one: the held frame dumps, no dump fails
    engine.failEncodes(0);
}

TEST_CASE("Hub75GpioEngine: while the carry moves a fraction the engine encodes about once a refresh, no faster, and with the carry off it encodes nothing more") {
    mm::test::GpioRun run(gpioSmall());
    auto& engine = run.engine;
    engine.setCorrection(128, 0);   // 0 bits: the fraction drops
    const auto frame = redPixelFrame(200);
    engine.publishRgb(frame.data());
    REQUIRE(waitSubmits(engine, 4));
    const uint64_t encoded = engine.framesEncoded();
    REQUIRE(waitSubmits(engine, 16));
    CHECK(engine.framesEncoded() == encoded);   // the held frame re-submits as it is
    engine.setCorrection(128, 2);
    CHECK(encodesOncePerRefresh(engine));   // the encode follows the refresh once the carry moves, paced on the take
}
