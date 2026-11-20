/// @module platform

/// Pins the rp1 PIO backend (hub75_pio_backend_rp1.cpp) against a fake `/dev/pio0` (fake_rp1_pio0.h) that answers the ioctls as the driver does: the order of the backend's calls, what it says when each one fails, what a stop leaves, what a close releases, the register packing and the ioctl numbers.
///
/// What this cannot show is the real driver and the RP1 firmware behind it: the firmware's refusals and their errno, a signal in a submit, a 6.12 timeout and a wedged state machine are the Pi's to prove (the bench).

#include "doctest.h"

#include "fake_rp1_pio0.h"
#include "platform/rpi/hub75_pio_backend.h"
#include "platform/rpi/hub75_pio_backend_rp1.h"
#include "platform/rpi/hub75_pio_pins.h"
#include "platform/rpi/hub75_pio_rp1_uapi.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using mm::platform::PioStartConfig;
using mm::FakeRp1Pio0;
namespace rp1 = mm::platform::rp1;

const uint16_t kProgram[3] = {1, 2, 3};
constexpr uint32_t kOutMask = ((1u << 26) - 1u) << 2;   // GPIO 2 to 27

PioStartConfig config() {
    PioStartConfig c;
    c.programInstructions = kProgram;
    c.programLength = 3;
    c.wrapTarget = 0;
    c.wrap = 2;
    c.clkdivInt = 4;
    c.xferBufSize = 8192;
    c.xferBufCount = 4;
    return c;
}

// A backend on a fresh fake kernel; the fake is declared first, so it outlives the backend.
struct Rig {
    std::unique_ptr<FakeRp1Pio0> owned = std::make_unique<FakeRp1Pio0>();   // on the heap: the fake is larger than a stack frame may be
    FakeRp1Pio0& fake = *owned;
    std::unique_ptr<mm::platform::PioBackend> backend;
    std::string error;
    Rig() : backend(mm::platform::createRp1Backend(fake)) {}
};

// Releases a hung transfer and joins its thread when the scope ends, so a failed check cannot leave a thread blocked in the fake.
struct Submitter {
    FakeRp1Pio0& fake;
    std::thread thread;
    ~Submitter() {
        fake.releaseTransfers();
        if (thread.joinable()) thread.join();
    }
};

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

uint32_t command(unsigned long request) { return rp1::ioctlCommand(request); }

using Calls = std::vector<std::string>;

}  // namespace

TEST_CASE("Hub75PioRp1: an open that fails says why from the errno, the missing node apart from the permission") {
    Rig r;
    r.fake.failOpen(ENOENT);
    CHECK_FALSE(r.backend->open(r.error));
    CHECK(contains(r.error, "no /dev/pio0"));
    CHECK(contains(r.error, std::strerror(ENOENT)));
    r.fake.failOpen(EACCES);
    CHECK_FALSE(r.backend->open(r.error));
    CHECK(contains(r.error, "cannot open /dev/pio0"));
    CHECK(contains(r.error, "Permission denied"));
    CHECK(r.fake.opens() == 2);
    CHECK(r.fake.calls().empty());   // no claim after a failed open, and no access() pre-check
}

TEST_CASE("Hub75PioRp1: the permission message names the node's mode, owner and group") {
    // A node as the bench Pi's reads: crw-rw---- root gpio.
    mm::platform::PioNode node;
    node.characterDevice = true;
    node.mode = 0660;
    node.owner = "root";
    node.group = "gpio";
    const std::string why = mm::platform::pioRp1OpenFailure(EACCES, &node);
    CHECK(contains(why, "crw-rw---- root gpio"));
    CHECK(contains(why, "must be in group gpio"));
    node.group = "pio";   // a machine that names another group is told that one
    CHECK(contains(mm::platform::pioRp1OpenFailure(EACCES, &node), "must be in group pio"));
    CHECK_FALSE(contains(mm::platform::pioRp1OpenFailure(EBUSY, &node), "group"));   // only a permission error names the group
    // A group is named only when the mode gives it read and write: a node for the owner alone, or one the group may only read, is no group's to fix.
    for (unsigned mode : {0600u, 0640u, 0620u, 0604u}) {
        node.mode = mode;
        const std::string closed = mm::platform::pioRp1OpenFailure(EACCES, &node);
        CAPTURE(mode);
        CHECK_FALSE(contains(closed, "must be in group"));
        CHECK(contains(closed, "grants its group no read and write"));
        CHECK(contains(closed, "udev rule"));
    }
    CHECK(contains(mm::platform::pioRp1OpenFailure(ENOENT, nullptr), "no /dev/pio0"));
    CHECK(contains(mm::platform::pioRp1OpenFailure(ENODEV, nullptr), "RP1 PIO driver"));
    CHECK(contains(mm::platform::pioRp1OpenFailure(ENXIO, nullptr), "no /dev/pio0"));

    Rig r;   // and the backend reads the node from its device
    node.mode = 0660;
    node.group = "gpio";
    r.fake.setNode(node);
    r.fake.failOpen(EACCES);
    CHECK_FALSE(r.backend->open(r.error));
    CHECK(contains(r.error, "crw-rw---- root gpio"));
}

TEST_CASE("Hub75PioRp1: a claim that fails says why and closes the device") {
    Rig r;
    for (int sm = 0; sm < 4; sm++) r.fake.otherClientClaims(sm);
    CHECK_FALSE(r.backend->open(r.error));
    CHECK(contains(r.error, "cannot claim a PIO state machine"));
    CHECK(contains(r.error, std::strerror(EBUSY)));
    CHECK(r.fake.calls() == Calls{"claim"});
    CHECK(r.fake.closes() == 1);
    CHECK_FALSE(r.fake.isOpen());
}

TEST_CASE("Hub75PioRp1: a start before the open is refused and calls nothing") {
    Rig r;
    CHECK_FALSE(r.backend->start(config(), r.error));
    CHECK(contains(r.error, "not opened"));
    CHECK(r.fake.calls().empty());
}

TEST_CASE("Hub75PioRp1: a DMA ring another client holds is named, and nothing is loaded after it") {
    Rig r;
    r.fake.otherClientHoldsDma(0);
    REQUIRE(r.backend->open(r.error));
    CHECK_FALSE(r.backend->start(config(), r.error));
    CHECK(contains(r.error, "buf_size=8192 buf_count=4"));
    CHECK(contains(r.error, "another client holds the DMA"));
    CHECK(r.fake.calls() == Calls{"claim", "config_xfer"});
    r.backend->close();
    CHECK(r.fake.programWordsUsed() == 0);   // no program was loaded to remove
    CHECK(r.fake.clientClaimedMask() == 0);   // and the close gave the claim back
}

TEST_CASE("Hub75PioRp1: a program that will not load says why, and the DMA ring is already configured") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    r.fake.failCommand(command(rp1::kIocAddProgram), -ENOSPC);
    CHECK_FALSE(r.backend->start(config(), r.error));
    CHECK(contains(r.error, "cannot load the PIO program"));
    CHECK(contains(r.error, "3 of the 32 instruction words"));
    CHECK(r.fake.calls() == Calls{"claim", "config_xfer", "add_program"});
    r.backend->close();
    CHECK(r.fake.count("remove_program") == 0);
}

TEST_CASE("Hub75PioRp1: a program longer than the instruction memory is refused before any ioctl") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    PioStartConfig c = config();
    const std::vector<uint16_t> longProgram(33, 1);
    c.programInstructions = longProgram.data();
    c.programLength = 33;
    CHECK_FALSE(r.backend->start(c, r.error));
    CHECK(contains(r.error, "33 of the 32 instruction words"));
    CHECK(r.fake.count("add_program") == 0);
}

TEST_CASE("Hub75PioRp1: a start runs the DMA ring, the program, the pins, the init and the enable in that order, once each") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    PioStartConfig c = config();
    c.xferBufSize = 65536;   // the engine's ring, which needs CONFIG_XFER32
    REQUIRE(r.backend->start(c, r.error));
    CHECK(r.fake.calls(true) == Calls{"claim", "config_xfer", "add_program", "set_pins", "set_pindirs", "gpio_function", "init", "clear_fifos", "enable"});
    CHECK(r.fake.count("gpio_function") == 26);   // one call a pin
    CHECK(r.fake.dmaBufSize(0) == 65536);
    CHECK(r.fake.dmaBufCount(0) == 4);
    CHECK(r.fake.programWords(0) == std::vector<uint16_t>(kProgram, kProgram + 3));   // the instructions went to the kernel as given
    CHECK(r.fake.smEnabled(0));
    CHECK(r.fake.smInitialPc(0) == r.fake.programOffsets().front());
    CHECK(r.fake.pinDirections() == kOutMask);    // GPIO 2 to 27 are outputs
    CHECK(r.fake.pinValues() == (mm::platform::kBlankWideWord << 2));   // the pins come up dark
    for (int gpio = 0; gpio < 28; gpio++) {
        CAPTURE(gpio);
        CHECK(r.fake.gpioFunction(gpio) == (gpio >= 2 ? rp1::kGpioFunctionPio : 0));
    }
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: a start never asks the kernel whether a state machine is claimed, and no call goes unknown or wrong-sized") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));
    std::vector<uint32_t> frame(64, 0);
    CHECK(r.backend->submitFrame(frame.data(), 256) == 0);
    CHECK(r.backend->stop(true, r.error));
    r.backend->close();
    CHECK(r.fake.count("unknown") == 0);
    CHECK(r.fake.count("wrong_size") == 0);
}

namespace {

// One refused step of the start: the start fails naming the step, a submit finds the backend not started, and a close drops the program.
void refusedStepCase(unsigned long request, const char* said) {
    CAPTURE(said);
    Rig r;
    REQUIRE(r.backend->open(r.error));
    r.fake.failCommand(command(request), -EIO);
    CHECK_FALSE(r.backend->start(config(), r.error));
    CHECK(contains(r.error, said));
    CHECK(contains(r.error, std::strerror(EIO)));
    std::vector<uint32_t> frame(64, 0);
    CHECK(r.backend->submitFrame(frame.data(), 256) == -ENODEV);   // the engine closes a backend whose start failed
    r.backend->close();
    CHECK(r.fake.programWordsUsed() == 0);
}

}  // namespace

TEST_CASE("Hub75PioRp1: a refused step of the start fails it, names the step, and leaves the backend not started") {
    refusedStepCase(rp1::kIocSmSetPins, "set the PIO pins to the idle word");
    refusedStepCase(rp1::kIocSmSetPindirs, "set the PIO pin directions");
    refusedStepCase(rp1::kIocGpioSetFunction, "hand a GPIO to the PIO");
    refusedStepCase(rp1::kIocSmInit, "initialize the PIO state machine");
    refusedStepCase(rp1::kIocSmClearFifos, "clear the PIO FIFOs");
    refusedStepCase(rp1::kIocSmSetEnabled, "enable the PIO state machine");
}

TEST_CASE("Hub75PioRp1: the pin word and mask are the same at the start and at the stop") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));
    CHECK(r.backend->stop(false, r.error));
    const auto writes = r.fake.pinWrites();
    REQUIRE(writes.size() == 2);
    CHECK(writes[0].first == (mm::platform::kBlankWideWord << 2));   // bit 0 of the word is GPIO 2
    CHECK(writes[0].second == kOutMask);
    CHECK(writes[1] == writes[0]);   // the start's idle word and the stop's dark word are one call
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: a submit before the start answers -ENODEV, and after it hands the whole frame to the kernel in one call") {
    Rig r;
    std::vector<uint32_t> frame(65536, 0xABCD1234u);   // 256 KiB, four times the 64 KiB buffer
    CHECK(r.backend->submitFrame(frame.data(), 256) == -ENODEV);
    REQUIRE(r.backend->open(r.error));
    CHECK(r.backend->submitFrame(frame.data(), 256) == -ENODEV);   // opened is not started
    PioStartConfig c = config();
    c.xferBufSize = 65536;
    REQUIRE(r.backend->start(c, r.error));
    CHECK(r.backend->submitFrame(frame.data(), 262144) == 0);
    const auto transfers = r.fake.transfers();
    REQUIRE(transfers.size() == 1);   // one call for the whole frame, which the driver cuts into ring buffers
    CHECK(transfers[0].bytes == 262144);
    CHECK(transfers[0].chunks == 4);
    CHECK(transfers[0].sm == 0);
    CHECK(transfers[0].dir == rp1::kDirToSm);
    CHECK(std::memcmp(transfers[0].data.data(), frame.data(), 262144) == 0);
    for (const int err : {EINTR, ETIMEDOUT, EIO}) {   // the kernel's errno comes back negated, whatever it is
        CAPTURE(err);
        r.fake.failCommand(command(rp1::kIocSmXferData32), -err);
        CHECK(r.backend->submitFrame(frame.data(), 256) == -err);
    }
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: a stop with drain waits, polls the FIFO, then disables the SM, darkens the pins, clears the FIFOs and removes the program, with no transfer") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));
    const size_t before = r.fake.calls().size();
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(r.backend->stop(true, r.error));
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::microseconds(1000));   // the ring's wire time, doubled: 8,192 words at 10 MHz is 0.8 ms before the doubling
    const Calls all = r.fake.calls();
    CHECK(Calls(all.begin() + static_cast<long>(before), all.end()) == Calls{"fifo_state", "disable", "set_pins", "clear_fifos", "remove_program"});   // no xfer, and no config_xfer
    CHECK(r.fake.lastFifoTx() == 1);   // the TX FIFO
    CHECK(r.fake.pinValues() == (mm::platform::kBlankWideWord << 2));
    CHECK_FALSE(r.fake.smEnabled(0));
    CHECK(r.fake.programWordsUsed() == 0);
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: a stop with drain clear skips the wait and the poll, and runs the same four calls") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    PioStartConfig c = config();
    c.clkdivInt = 4000;   // 10 kHz: the ring's 8,192 words are 819 ms on the wire, 1.6 s doubled, which a stop with the wait would sleep
    REQUIRE(r.backend->start(c, r.error));
    const size_t before = r.fake.calls().size();
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(r.backend->stop(false, r.error));
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(800));   // half the wait it skips: far from it, under any load
    const Calls all = r.fake.calls();
    CHECK(Calls(all.begin() + static_cast<long>(before), all.end()) == Calls{"disable", "set_pins", "clear_fifos", "remove_program"});
    CHECK(r.fake.fifoPolls() == 0);
    r.backend->close();
}

namespace {

// A stop whose disable the kernel refuses, with the wait for the drain and with that wait off.
void disableFailureCase(bool drain) {
    CAPTURE(drain);
    Rig r;
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));
    r.fake.failCommand(command(rp1::kIocSmSetEnabled), -EIO);
    CHECK_FALSE(r.backend->stop(drain, r.error));
    CHECK(contains(r.error, "cannot disable the PIO state machine"));
    CHECK(contains(r.error, std::strerror(EIO)));
    CHECK_FALSE(contains(r.error, "dark word"));   // the pins were set
    const Calls all = r.fake.calls();
    CHECK(Calls(all.end() - 4, all.end()) == Calls{"disable", "set_pins", "clear_fifos", "remove_program"});   // the others ran
    r.backend->close();
}

}  // namespace

TEST_CASE("Hub75PioRp1: a stop whose disable fails says so, returns false, and still sets the pins, clears the FIFOs and removes the program") {
    disableFailureCase(false);
    disableFailureCase(true);
}

TEST_CASE("Hub75PioRp1: a stop whose pins will not set says so, returns false, and has disabled the SM and removed the program") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));
    r.fake.failCommand(command(rp1::kIocSmSetPins), -ETIMEDOUT);
    const size_t before = r.fake.calls().size();
    CHECK_FALSE(r.backend->stop(false, r.error));
    CHECK(contains(r.error, "cannot set the pins to the dark word"));
    CHECK(contains(r.error, std::strerror(ETIMEDOUT)));
    CHECK_FALSE(contains(r.error, "cannot disable"));
    const Calls all = r.fake.calls();
    CHECK(Calls(all.begin() + static_cast<long>(before), all.end()) == Calls{"disable", "set_pins", "clear_fifos", "remove_program"});
    CHECK_FALSE(r.fake.smEnabled(0));
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: a stop with every step failing names each, once, and a later stop that finds them working starts from no error") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));
    r.fake.failCommand(command(rp1::kIocSmSetEnabled), -EIO);
    r.fake.failCommand(command(rp1::kIocSmSetPins), -EIO);
    r.fake.setTxFifoEmpty(false);   // the drain times out too
    CHECK_FALSE(r.backend->stop(true, r.error));
    CHECK(contains(r.error, "did not drain"));
    CHECK(contains(r.error, "cannot disable"));
    CHECK(contains(r.error, "cannot set the pins"));
    CHECK(contains(r.error, "; cannot disable"));   // each failure after the first is set off by a semicolon
    r.fake.failCommand(command(rp1::kIocSmSetEnabled), 0);
    r.fake.failCommand(command(rp1::kIocSmSetPins), 0);
    CHECK(r.backend->stop(true, r.error));   // the retry finds both steps working: it succeeds and the old text is gone
    CHECK(r.error.empty());
    r.backend->close();
}

namespace {

// The calls that came in after the first `before`.
Calls callsSince(const Rig& r, size_t before) {
    const Calls all = r.fake.calls();
    return Calls(all.begin() + static_cast<long>(before), all.end());
}

// A stop that failed on `request`, repeated until the step works, then once more when the SM is dark already.
void retryUntilDarkCase(unsigned long request, const char* failure, const char* other) {
    CAPTURE(failure);
    Rig r;
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));
    r.fake.failCommand(command(request), -EIO);
    CHECK_FALSE(r.backend->stop(false, r.error));   // the wedged path's stop, with the refresh thread still inside
    size_t before = r.fake.calls().size();
    CHECK_FALSE(r.backend->stop(true, r.error));   // the later shutdown's retry, the thread gone: the step is still refused, and the result follows it
    CHECK(contains(r.error, failure));
    CHECK_FALSE(contains(r.error, other));
    CHECK(callsSince(r, before) == Calls{"disable", "set_pins", "clear_fifos"});   // the first stop took the program, and a retry waits for the SM only
    CHECK(r.fake.fifoPolls() == 0);
    r.fake.failCommand(command(request), 0);
    before = r.fake.calls().size();
    CHECK(r.backend->stop(true, r.error));   // the step works now: dark, and the old text is gone
    CHECK(r.error.empty());
    CHECK(callsSince(r, before) == Calls{"disable", "set_pins", "clear_fifos"});
    CHECK(r.fake.pinValues() == (mm::platform::kBlankWideWord << 2));
    before = r.fake.calls().size();
    CHECK(r.backend->stop(true, r.error));   // dark now: no further call
    CHECK(r.fake.calls().size() == before);
    r.backend->close();
}

}  // namespace

TEST_CASE("Hub75PioRp1: a stop that failed runs the disable, the pins and the FIFO clear again, with no wait, until both steps have succeeded once, whichever step failed") {
    retryUntilDarkCase(rp1::kIocSmSetEnabled, "cannot disable the PIO state machine", "cannot set the pins");
    retryUntilDarkCase(rp1::kIocSmSetPins, "cannot set the pins to the dark word", "cannot disable");
}

TEST_CASE("Hub75PioRp1: after a stop a submit answers -ENODEV, a second stop makes no call, and the close closes the device, once") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));
    CHECK(r.backend->stop(false, r.error));
    const size_t before = r.fake.calls().size();
    std::vector<uint32_t> blank(64, mm::platform::kBlankWideWord);
    CHECK(r.backend->submitBlank(blank.data(), 256) == -ENODEV);   // the refresh thread released late finds the backend stopped
    CHECK(r.backend->submitFrame(blank.data(), 256) == -ENODEV);
    CHECK(r.backend->stop(true, r.error));
    CHECK(r.fake.calls().size() == before);
    CHECK(r.fake.closes() == 0);
    r.backend->close();
    CHECK(r.fake.closes() == 1);
    r.backend->close();   // a second close finds everything released already
    CHECK(r.fake.closes() == 1);
}

TEST_CASE("Hub75PioRp1: a blank is the same transfer as a frame, and a FIFO that stays full still stops the SM, darkens the pins and says so") {
    {
        Rig r;
        REQUIRE(r.backend->open(r.error));
        REQUIRE(r.backend->start(config(), r.error));
        std::vector<uint32_t> blank(8192, mm::platform::kBlankWideWord);
        CHECK(r.backend->submitBlank(blank.data(), static_cast<uint32_t>(blank.size() * 4)) == 0);
        const auto transfers = r.fake.transfers();
        REQUIRE(transfers.size() == 1);
        CHECK(transfers[0].bytes == 32768);
        r.fake.failCommand(command(rp1::kIocSmXferData32), -EIO);
        CHECK(r.backend->submitBlank(blank.data(), 256) == -EIO);   // the kernel's code comes back
        r.backend->close();
    }
    {
        Rig r;
        REQUIRE(r.backend->open(r.error));
        REQUIRE(r.backend->start(config(), r.error));
        r.fake.setTxFifoEmpty(false);
        CHECK(r.backend->stop(true, r.error));   // the panel is dark: the timeout is a note, not a failure
        CHECK(contains(r.error, "did not drain"));
        CHECK_FALSE(contains(r.error, "cannot"));
        CHECK(r.fake.pinValues() == (mm::platform::kBlankWideWord << 2));
        CHECK(r.fake.count("disable") == 1);
        CHECK(r.fake.count("remove_program") == 1);
        CHECK(r.fake.fifoPolls() > 1);   // polled until the timeout
        r.backend->close();
    }
}

TEST_CASE("Hub75PioRp1: the wait before the SM stops follows the ring and the clock") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    PioStartConfig c = config();
    c.xferBufSize = 2048;   // 4 buffers: 2,048 words
    c.clkdivInt = 400;      // 100 kHz: the ring is 20.5 ms on the wire, 41 ms doubled
    REQUIRE(r.backend->start(c, r.error));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(r.backend->stop(true, r.error));
    const auto waited = std::chrono::steady_clock::now() - t0;
    CHECK(waited >= std::chrono::milliseconds(40));
    CHECK(waited < std::chrono::milliseconds(1000));
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: a close after a failed start releases the claim, as the engine closes a backend it never stopped") {
    Rig r;
    r.fake.otherClientHoldsDma(0);
    REQUIRE(r.backend->open(r.error));
    CHECK_FALSE(r.backend->start(config(), r.error));
    r.backend->close();
    CHECK(r.fake.closes() == 1);
    CHECK(r.fake.clientClaimedMask() == 0);
}

TEST_CASE("Hub75PioRp1: a transfer stuck in the kernel leaves the stop's calls returning, the SM disabled and the pins dark") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));
    r.fake.hangTransfers();
    std::vector<uint32_t> frame(64, 0);
    std::atomic<int> result{-12345};
    Submitter submitter{r.fake, std::thread([&] { result = r.backend->submitFrame(frame.data(), 256); })};
    REQUIRE(r.fake.waitForBlockedTransfers(1, std::chrono::milliseconds(2000)));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(r.backend->stop(false, r.error));   // the deadline's path: no wait, firmware-message calls only
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
    CHECK_FALSE(r.fake.smEnabled(0));
    CHECK(r.fake.pinValues() == (mm::platform::kBlankWideWord << 2));
    CHECK(result.load() == -12345);   // the submit is still inside the kernel
    r.fake.releaseTransfers(0);
    submitter.thread.join();
    CHECK(result.load() == 0);   // it returns what the kernel answers when it lets go
    CHECK(r.backend->submitFrame(frame.data(), 256) == -ENODEV);   // and the next one finds the backend stopped
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: the claim passes a buffer with room for the kernel's copy-back, and later calls use the state machine it returned") {
    Rig r;
    for (int sm = 0; sm < 3; sm++) r.fake.otherClientClaims(sm);   // the claim gets state machine 3, whose index is 3 bytes of copy-back
    REQUIRE(r.backend->open(r.error));
    CHECK(r.fake.claimArgBytes() >= 4);   // the argument is 2 bytes, and a released kernel writes back as many bytes as the result
    CHECK(r.fake.overruns() == 0);
    REQUIRE(r.backend->start(config(), r.error));
    CHECK(r.fake.smEnabled(3));
    CHECK_FALSE(r.fake.smEnabled(0));
    CHECK(r.fake.dmaConfigured(3));
    CHECK(r.fake.smInitialized(3));
    std::vector<uint32_t> frame(64, 0);
    CHECK(r.backend->submitFrame(frame.data(), 256) == 0);
    CHECK(r.backend->stop(true, r.error));
    CHECK_FALSE(r.fake.smEnabled(3));
    CHECK(r.fake.overruns() == 0);   // across the whole run: the FIFO state's 8 bytes fit their argument too
    for (const char* name : {"config_xfer", "init", "set_pins", "set_pindirs", "clear_fifos", "xfer", "fifo_state"}) {
        CAPTURE(name);
        const std::vector<int> sms = r.fake.smsOf(name);
        CHECK_FALSE(sms.empty());
        for (const int sm : sms) CHECK(sm == 3);   // every call names the state machine the claim returned
    }
    r.backend->close();
    CHECK(r.fake.claimedMask() == 0b0111u);   // the other client keeps its claims
}

TEST_CASE("Hub75PioRp1: a close releases the claim, the DMA ring, the program and the enable, and leaves the pins as they are") {
    Rig r;
    r.fake.otherClientClaims(1);
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));   // no stop: the close alone, as after a failed shutdown
    REQUIRE(r.fake.smEnabled(0));
    REQUIRE(r.fake.programWordsUsed() == 3);
    r.backend->close();
    CHECK(r.fake.closes() == 1);
    CHECK(r.fake.clientClaimedMask() == 0);
    CHECK(r.fake.claimedMask() == 0b10u);   // another client's claim stays
    CHECK_FALSE(r.fake.dmaConfigured(0));
    CHECK(r.fake.programWordsUsed() == 0);
    CHECK_FALSE(r.fake.smEnabled(0));
    CHECK(r.fake.pinValues() == (mm::platform::kBlankWideWord << 2));   // the pins keep their value
    CHECK(r.fake.gpioFunction(2) == rp1::kGpioFunctionPio);            // and their function
}

TEST_CASE("Hub75PioRp1: a reopen needs no memory of the state machine the last run held") {
    Rig r;
    for (int run = 0; run < 3; run++) {
        CAPTURE(run);
        REQUIRE(r.backend->open(r.error));
        REQUIRE(r.backend->start(config(), r.error));   // the DMA ring is free again, so the configure does not answer -EBUSY
        CHECK(r.backend->stop(true, r.error));
        r.backend->close();
    }
    CHECK(r.fake.count("claim") == 3);
    CHECK(r.fake.count("config_xfer") == 3);
    CHECK(r.fake.count("unknown") == 0);   // and no run asked whether a state machine was claimed
}

TEST_CASE("Hub75PioRp1: an interrupted submit is reported once, and the next one is built fresh") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));
    std::vector<uint32_t> frame(1024, 7);
    const uint64_t pointer = reinterpret_cast<uintptr_t>(frame.data());
    r.fake.interruptTransfers(1);
    CHECK(r.backend->submitFrame(frame.data(), 4096) == -EINTR);   // reported as a failed submit
    CHECK(r.fake.count("xfer") == 1);                               // with no hidden retry
    const rp1::SmXferData32Args rewritten = r.fake.lastRewrite();
    CHECK(rewritten.data > pointer);          // the kernel advanced the data pointer in the argument...
    CHECK(rewritten.dataBytes < 4096u);       // ...and reduced the byte count
    CHECK(r.backend->submitFrame(frame.data(), 4096) == 0);
    const auto transfers = r.fake.transfers();
    REQUIRE(transfers.size() == 2);
    CHECK(transfers[1].dataPointer == pointer);   // the retry starts again from the frame's first byte
    CHECK(transfers[1].bytes == 4096u);
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: the FIFO state is asked for the TX FIFO with the number 6.18 declares, and either line's number reaches the same call") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));
    CHECK(r.backend->stop(true, r.error));
    CHECK(r.fake.lastRequest(command(rp1::kIocSmFifoState)) == 0xC008662Cul);   // _IOWR, which 6.12 takes too: the driver reads the command and the size
    CHECK(r.fake.lastFifoTx() == 1);
    r.backend->close();
    // The 6.12 number, _IOW, differs in the direction bits alone, which the driver ignores.
    constexpr unsigned long kFifoState612 = 0x4008662Cul;
    CHECK(rp1::ioctlCommand(kFifoState612) == rp1::ioctlCommand(rp1::kIocSmFifoState));
    CHECK(rp1::ioctlArgBytes(kFifoState612) == rp1::ioctlArgBytes(rp1::kIocSmFifoState));
    rp1::SmFifoStateArgs args{};
    args.tx = 1;
    const auto otherOwned = std::make_unique<FakeRp1Pio0>();
    FakeRp1Pio0& other = *otherOwned;
    REQUIRE(other.open() == 0);
    CHECK(other.sendIoctl(kFifoState612, &args, sizeof(args)) == 8);   // the result's size, copied back
    CHECK(args.empty == 1);
    CHECK(other.sendIoctl(0x40006699ul, &args, sizeof(args)) == -EOPNOTSUPP);   // a command the driver lacks
    CHECK(other.sendIoctl(rp1::kIocSmClearFifos, &args, 1) == -EFAULT);          // a buffer smaller than the request declares
}

TEST_CASE("Hub75PioRp1: the ioctl numbers match the values computed from the kernel header for an arm64 process") {
    CHECK(rp1::kIocSmClaim == 0x40026614ul);
    CHECK(rp1::kIocAddProgram == 0x4044660Bul);
    CHECK(rp1::kIocRemoveProgram == 0x4004660Cul);
    CHECK(rp1::kIocSmInit == 0x4014661Eul);
    CHECK(rp1::kIocSmClearFifos == 0x40026621ul);
    CHECK(rp1::kIocSmSetPins == 0x400C6623ul);
    CHECK(rp1::kIocSmSetPindirs == 0x400C6624ul);
    CHECK(rp1::kIocSmSetEnabled == 0x40046625ul);
    CHECK(rp1::kIocGpioSetFunction == 0x40046633ul);
    CHECK(rp1::kIocSmConfigXfer32 == 0x400C6603ul);
    CHECK(rp1::kIocSmXferData32 == 0x40106602ul);
    CHECK(rp1::kIocSmFifoState == 0xC008662Cul);
    CHECK(rp1::kIoctlMagic == static_cast<uint32_t>('f'));
    CHECK(rp1::kGpioFunctionPio == 7);     // RP1_GPIO_FUNC_PIO, rp1_pio_if.h:14 on both lines
    CHECK(rp1::kOriginAny == 0xFFFF);      // RP1_PIO_ORIGIN_ANY, rp1_pio_if.h:16 on 6.12 and :17 on 6.18
    CHECK(rp1::kDirToSm == 0);             // RP1_PIO_DIR_TO_SM, :18 and :19
    CHECK(rp1::kSmCount == 4);             // RP1_PIO_SM_COUNT, :12
    CHECK(rp1::kInstructionCount == 32);   // RP1_PIO_INSTRUCTION_COUNT, :11
}

TEST_CASE("Hub75PioRp1: each config register field packs at its bit, and an oversized value stays inside its field") {
    struct Bit {
        uint32_t packed;
        uint32_t expected;
    };
    const auto clkdiv = [](uint16_t i, uint8_t f) { return rp1::packClkdiv(i, f); };
    const auto exec = [](auto set) { rp1::ExecControl c; set(c); return rp1::packExecctrl(c); };
    const auto shift = [](auto set) { rp1::ShiftControl c; set(c); return rp1::packShiftctrl(c); };
    const auto pins = [](auto set) { rp1::PinControl c; set(c); return rp1::packPinctrl(c); };
    static const Bit bits[] = {
        {clkdiv(1, 0), 1u << 16},   // INT at 31:16
        {clkdiv(0, 1), 1u << 8},    // FRAC at 15:8
        {clkdiv(0xFFFF, 0xFF), 0xFFFFFF00u},
        {exec([](rp1::ExecControl& c) { c.sideEnable = true; }), 1u << 30},
        {exec([](rp1::ExecControl& c) { c.sidePindirs = true; }), 1u << 29},
        {exec([](rp1::ExecControl& c) { c.wrapTop = 1; }), 1u << 12},      // WRAP_TOP at 16:12
        {exec([](rp1::ExecControl& c) { c.wrapTop = 31; }), 0x1Fu << 12},
        {exec([](rp1::ExecControl& c) { c.wrapTop = 32; }), 0u},           // one past the field
        {exec([](rp1::ExecControl& c) { c.wrapBottom = 1; }), 1u << 7},    // WRAP_BOTTOM at 11:7
        {exec([](rp1::ExecControl& c) { c.wrapBottom = 31; }), 0x1Fu << 7},
        {shift([](rp1::ShiftControl& c) { c.joinRx = true; }), 1u << 31},
        {shift([](rp1::ShiftControl& c) { c.joinTx = true; }), 1u << 30},
        {shift([](rp1::ShiftControl& c) { c.pullThreshold = 1; }), 1u << 25},   // PULL_THRESH at 29:25
        {shift([](rp1::ShiftControl& c) { c.pullThreshold = 31; }), 0x1Fu << 25},
        {shift([](rp1::ShiftControl& c) { c.pullThreshold = 32; }), 0u},
        {shift([](rp1::ShiftControl& c) { c.pushThreshold = 1; }), 1u << 20},   // PUSH_THRESH at 24:20
        {shift([](rp1::ShiftControl& c) { c.pushThreshold = 31; }), 0x1Fu << 20},
        {shift([](rp1::ShiftControl& c) { c.outShiftRight = true; }), 1u << 19},
        {shift([](rp1::ShiftControl& c) { c.inShiftRight = true; }), 1u << 18},
        {shift([](rp1::ShiftControl& c) { c.autoPull = true; }), 1u << 17},
        {shift([](rp1::ShiftControl& c) { c.autoPush = true; }), 1u << 16},
        {pins([](rp1::PinControl& c) { c.sidesetCount = 1; }), 1u << 29},   // SIDESET_COUNT at 31:29
        {pins([](rp1::PinControl& c) { c.sidesetCount = 7; }), 7u << 29},
        {pins([](rp1::PinControl& c) { c.sidesetCount = 8; }), 0u},
        {pins([](rp1::PinControl& c) { c.setCount = 1; }), 1u << 26},       // SET_COUNT at 28:26
        {pins([](rp1::PinControl& c) { c.setCount = 7; }), 7u << 26},
        {pins([](rp1::PinControl& c) { c.outCount = 1; }), 1u << 20},       // OUT_COUNT at 25:20
        {pins([](rp1::PinControl& c) { c.outCount = 32; }), 32u << 20},
        {pins([](rp1::PinControl& c) { c.outCount = 63; }), 63u << 20},
        {pins([](rp1::PinControl& c) { c.outCount = 64; }), 0u},
        {pins([](rp1::PinControl& c) { c.inBase = 1; }), 1u << 15},         // IN_BASE at 19:15
        {pins([](rp1::PinControl& c) { c.inBase = 31; }), 0x1Fu << 15},
        {pins([](rp1::PinControl& c) { c.sidesetBase = 1; }), 1u << 10},    // SIDESET_BASE at 14:10
        {pins([](rp1::PinControl& c) { c.sidesetBase = 31; }), 0x1Fu << 10},
        {pins([](rp1::PinControl& c) { c.setBase = 1; }), 1u << 5},         // SET_BASE at 9:5
        {pins([](rp1::PinControl& c) { c.setBase = 31; }), 0x1Fu << 5},
        {pins([](rp1::PinControl& c) { c.outBase = 1; }), 1u},              // OUT_BASE at 4:0
        {pins([](rp1::PinControl& c) { c.outBase = 31; }), 0x1Fu},
        {pins([](rp1::PinControl& c) { c.outBase = 32; }), 0u},
    };
    for (size_t index = 0; index < std::size(bits); index++) {
        CAPTURE(index);
        CHECK(bits[index].packed == bits[index].expected);
    }
}

TEST_CASE("Hub75PioRp1: the start packs the hub75_wide program's four registers at the program's offset") {
    Rig r;
    r.fake.otherClientLoadsProgram(3);   // our program then loads at offset 3, so the wrap bounds are offset plus the program's own
    REQUIRE(r.backend->open(r.error));
    PioStartConfig c = config();
    c.clkdivInt = 4;
    c.clkdivFrac = 0x80;
    REQUIRE(r.backend->start(c, r.error));
    const rp1::SmConfig regs = r.fake.smConfig(0);
    CHECK(regs.clkdiv == 0x00048000u);
    CHECK(regs.execctrl == ((3u + 2u) << 12 | 3u << 7));   // wrap top at offset + 2, wrap bottom at offset + 0
    CHECK(regs.shiftctrl == 0x400C0000u);   // FJOIN_TX, OUT_SHIFTDIR right and IN_SHIFTDIR right, no autopull, thresholds 0
    CHECK(regs.pinctrl == 0x21A04402u);     // side-set count 1 at GPIO 17, OUT count 26 from GPIO 2
    CHECK(r.fake.smInitialPc(0) == 3);
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: the backend names itself") {
    Rig r;
    CHECK(std::string(r.backend->name()) == "rp1 (/dev/pio0)");
}

TEST_CASE("Hub75PioRp1: a FIFO state that cannot be read ends the drain at once with a note that says so, and the stop still darkens the panel") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));
    r.fake.failCommand(command(rp1::kIocSmFifoState), -EIO);
    CHECK(r.backend->stop(true, r.error));   // the disable and the pins went through
    CHECK(contains(r.error, "cannot read the TX FIFO state"));
    CHECK(contains(r.error, std::strerror(EIO)));
    CHECK_FALSE(contains(r.error, "did not drain"));
    CHECK(r.fake.fifoPolls() == 1);   // the one read ends the poll, with no 100 ms wait on a state that stays unreadable
    CHECK(r.fake.pinValues() == (mm::platform::kBlankWideWord << 2));
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: a stop removes the program at the offset it loaded, and another client's program stays") {
    Rig r;
    r.fake.otherClientLoadsProgram(3);   // ours loads at offset 3
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));
    REQUIRE(r.fake.programOffsets() == std::vector<int>{0, 3});
    CHECK(r.backend->stop(false, r.error));
    CHECK(r.fake.programOffsets() == std::vector<int>{0});   // ours is gone and the other client's stays
    CHECK(r.fake.programWordsUsed() == 3);
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: a program of the whole 32 words loads, runs and is removed") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    PioStartConfig c = config();
    const std::vector<uint16_t> whole(32, 0xA000);
    c.programInstructions = whole.data();
    c.programLength = 32;
    c.wrapTarget = 0;
    c.wrap = 31;
    REQUIRE(r.backend->start(c, r.error));
    CHECK(r.fake.programWordsUsed() == 32);
    CHECK(r.fake.smConfig(0).execctrl == (31u << 12));   // wrap top at the last word
    CHECK(r.backend->stop(false, r.error));
    CHECK(r.fake.programWordsUsed() == 0);
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: a second open keeps the one file and claim, and a second start is refused before it loads the program again") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->open(r.error));
    CHECK(r.fake.opens() == 1);
    CHECK(r.fake.count("claim") == 1);
    REQUIRE(r.backend->start(config(), r.error));
    const int adds = r.fake.count("add_program");
    CHECK_FALSE(r.backend->start(config(), r.error));
    CHECK(contains(r.error, "already started"));
    CHECK(r.fake.count("add_program") == adds);
    CHECK(r.backend->stop(false, r.error));
    r.backend->close();
    CHECK(r.fake.programWordsUsed() == 0);
}

TEST_CASE("Hub75PioRp1: a close leaves nothing to stop, so a later stop makes no call, and the backend opens and starts again") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));
    r.backend->close();   // as after a failed shutdown: no stop came first
    const size_t before = r.fake.calls().size();
    CHECK(r.backend->stop(true, r.error));
    CHECK(r.fake.calls().size() == before);
    std::vector<uint32_t> frame(64, 0);
    CHECK(r.backend->submitFrame(frame.data(), 256) == -ENODEV);
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));   // the program was released by the close, so it loads again
    CHECK(r.backend->stop(false, r.error));
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: a backend closed with no stop starts again at once, since the close released the program") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));
    r.backend->close();
    REQUIRE(r.backend->open(r.error));
    CHECK(r.backend->start(config(), r.error));   // not refused as already started
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: the fake honors a fixed program origin and keeps its DMA channels per direction, as the driver does") {
    const auto owned = std::make_unique<FakeRp1Pio0>();
    FakeRp1Pio0& fake = *owned;
    REQUIRE(fake.open() == 0);
    rp1::AddProgramArgs add{};
    add.numInstrs = 3;
    add.origin = 5;
    for (int i = 0; i < 3; i++) add.instrs[i] = static_cast<uint16_t>(i + 1);
    CHECK(fake.sendIoctl(rp1::kIocAddProgram, &add, sizeof(add)) == 5);   // loaded at the origin asked for
    add.origin = 30;
    add.instrs[0] = 9;   // a different program that runs past the end
    CHECK(fake.sendIoctl(rp1::kIocAddProgram, &add, sizeof(add)) == -EINVAL);
    add.origin = 6;
    CHECK(fake.sendIoctl(rp1::kIocAddProgram, &add, sizeof(add)) == -ENOSPC);   // overlaps the first, and a fixed origin has no other place
    rp1::SmConfigXfer32Args ring{};
    ring.dir = rp1::kDirToSm;
    ring.bufSize = 4096;
    ring.bufCount = 2;
    REQUIRE(fake.sendIoctl(rp1::kIocSmConfigXfer32, &ring, sizeof(ring)) == 0);
    const std::vector<uint32_t> data(16, 1);
    rp1::SmXferData32Args xfer{};
    xfer.dataBytes = 64;
    xfer.data = reinterpret_cast<uintptr_t>(data.data());
    xfer.dir = rp1::kDirToSm;
    CHECK(fake.sendIoctl(rp1::kIocSmXferData32, &xfer, sizeof(xfer)) == 0);
    xfer.dir = 1;   // the other direction has no ring
    CHECK(fake.sendIoctl(rp1::kIocSmXferData32, &xfer, sizeof(xfer)) == -EINVAL);
}

TEST_CASE("Hub75PioRp1: the stop's wait is the ring's words on the wire, doubled, so a ring of 8,192 words at 1.37 MHz waits about 300 ms") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    PioStartConfig c = config();
    c.xferBufSize = 8192;   // four buffers: 8,192 words of 4 bytes, not 32,768
    c.clkdivInt = 732;      // 5 cycles x 732 of 200 MHz a word: 18.3 us, so the ring is 150 ms on the wire, 300 ms doubled
    REQUIRE(r.backend->start(c, r.error));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(r.backend->stop(true, r.error));
    const auto waited = std::chrono::steady_clock::now() - t0;
    CHECK(waited >= std::chrono::milliseconds(290));
    CHECK(waited < std::chrono::milliseconds(1000));   // a wait counted in bytes would be four times that
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: a start after a failed start, with no close between, is refused before the program loads twice") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    r.fake.failCommand(command(rp1::kIocSmInit), -EIO);
    CHECK_FALSE(r.backend->start(config(), r.error));   // the program is loaded by then
    r.fake.failCommand(command(rp1::kIocSmInit), 0);
    const int adds = r.fake.count("add_program");
    CHECK_FALSE(r.backend->start(config(), r.error));
    CHECK(contains(r.error, "already started"));
    CHECK(r.fake.count("add_program") == adds);
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: a program of no words is refused before any ioctl, as one over 32 is") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    PioStartConfig c = config();
    c.programLength = 0;
    CHECK_FALSE(r.backend->start(c, r.error));
    CHECK(contains(r.error, "0 of the 32 instruction words"));
    CHECK(r.fake.count("add_program") == 0);
    r.backend->close();
}

TEST_CASE("Hub75PioRp1: a call on a closed device shows in the fake's log, so a stop after a close that still calls is seen") {
    Rig r;
    REQUIRE(r.backend->open(r.error));
    REQUIRE(r.backend->start(config(), r.error));
    r.backend->close();
    rp1::SmClearFifosArgs args{};
    CHECK(r.fake.sendIoctl(rp1::kIocSmClearFifos, &args, sizeof(args)) == -EBADF);
    CHECK(r.fake.calls().back() == "closed:clear_fifos");
}
