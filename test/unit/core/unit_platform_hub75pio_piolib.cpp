/// @module platform

/// Pins the piolib backend's own logic on any host, through a scripted stand-in for the C shim (hub75_pio_c_api.h): the order of its calls, what it says when each one fails, and what it leaves held. The backend's source is included here and the shim's functions are defined below, so the cases run on the stand-in alone. A build with piolib has the real shim linked and leaves these cases out.
///
/// What this cannot show: that the real shim and piolib behave as the stand-in is scripted to. The open and claim errors, the DMA ring, the SM's start, a signal in a submit, a 6.12 timeout and a wedged SM are the Pi's to prove (the bench, U7).

#include "doctest.h"

#ifndef MM_HUB75_PIO_HAVE_PIOLIB
#if defined(__unix__) || defined(__APPLE__)

#include "platform/rpi/hub75_pio_backend.h"
#include "platform/rpi/hub75_pio_c_api.h"
#include "platform/rpi/hub75_pio_pins.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// What the scripted shim does, and what it was asked.
struct Shim {
    int openErr = 0;          // pio_open fails with this positive errno
    int claimRc = 0;          // the SM number, or a negative errno
    int xferConfigRc = 0;
    int programRc = 3;        // the load offset, or a negative errno
    int xferRc = 0;           // every transfer's result
    int xferFailsAfter = -1;  // transfers from this count on use xferRc, earlier ones succeed; -1 for all
    bool fifoEmpty = true;
    int xfers = 0;
    uint32_t lastXferBytes = 0;
    hub75_pio_sm_params initParams{};
    std::vector<std::string> calls;
    void reset() { *this = Shim{}; }
};
Shim shim;

// The shim's handle: any non-null pointer.
void* const kHandle = &shim;

}  // namespace

extern "C" {
void* hub75_pio_open(int* err) {
    shim.calls.push_back("open");
    if (shim.openErr) {
        *err = shim.openErr;
        return nullptr;
    }
    return kHandle;
}
int hub75_pio_claim_sm(void*) {
    shim.calls.push_back("claim");
    return shim.claimRc ? shim.claimRc : 2;
}
int hub75_pio_add_program(void*, const uint16_t*, int) {
    shim.calls.push_back("add_program");
    return shim.programRc;
}
int hub75_pio_config_xfer(void*, int, uint32_t, uint32_t) {
    shim.calls.push_back("config_xfer");
    return shim.xferConfigRc;
}
void hub75_pio_init_sm(void*, int, const struct hub75_pio_sm_params* params) {
    shim.calls.push_back("init_sm");
    shim.initParams = *params;
}
void hub75_pio_clear_fifos(void*, int) { shim.calls.push_back("clear_fifos"); }
void hub75_pio_set_enabled(void*, int, int enable) { shim.calls.push_back(enable ? "enable" : "disable"); }
int hub75_pio_xfer(void*, int, void*, uint32_t bytes) {
    shim.calls.push_back("xfer");
    shim.lastXferBytes = bytes;
    const bool fails = shim.xferFailsAfter >= 0 && shim.xfers >= shim.xferFailsAfter;
    shim.xfers++;
    return fails || shim.xferFailsAfter < 0 ? shim.xferRc : 0;
}
int hub75_pio_tx_fifo_empty(void*, int) { return shim.fifoEmpty ? 1 : 0; }
void hub75_pio_remove_program(void*, const uint16_t*, int, int) { shim.calls.push_back("remove_program"); }
void hub75_pio_release(void*, int sm) { shim.calls.push_back(sm >= 0 ? "release" : "release_none"); }
}

namespace mm::platform {
PioBackend* createPiolibBackend();   // declared in the header only when piolib is linked
}

// The backend's own source, compiled against the scripted shim above.
#include "platform/rpi/hub75_pio_backend_piolib.cpp"

namespace {

using mm::platform::PioStartConfig;

const uint16_t kProgram[3] = {1, 2, 3};

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

// A backend, with the shim reset first.
struct Backend {
    std::unique_ptr<mm::platform::PioBackend> backend;
    std::string error;
    Backend() {
        shim.reset();
        backend.reset(mm::platform::createPiolibBackend());
    }
};

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("Hub75PioPiolib: an open that fails says why from the errno, the missing node apart from the permission") {
    Backend b;
    shim.openErr = EINVAL;
    CHECK_FALSE(b.backend->open(b.error));
    CHECK(contains(b.error, "no /dev/pio0"));
    CHECK(b.error.find("Invalid argument") != std::string::npos);
    shim.openErr = EACCES;
    CHECK_FALSE(b.backend->open(b.error));
    CHECK(contains(b.error, "cannot open /dev/pio0"));
    CHECK(contains(b.error, "Permission denied"));
    CHECK(shim.calls == std::vector<std::string>{"open", "open"});   // no claim after a failed open, and no access() pre-check
}

TEST_CASE("Hub75PioPiolib: the permission message names the node's mode, owner and group") {
    // A node as the bench Pi's reads: crw-rw---- root gpio.
    mm::platform::PioNode node;
    node.characterDevice = true;
    node.mode = 0660;
    node.owner = "root";
    node.group = "gpio";
    CHECK(mm::platform::pioModeString(node) == "crw-rw----");
    const std::string why = mm::platform::pioOpenFailure(EACCES, &node);
    CHECK(contains(why, "crw-rw---- root gpio"));
    CHECK(contains(why, "must be in group gpio"));
    node.group = "pio";   // a machine that names another group is told that one
    CHECK(contains(mm::platform::pioOpenFailure(EACCES, &node), "must be in group pio"));
    CHECK_FALSE(contains(mm::platform::pioOpenFailure(EBUSY, &node), "group"));   // only a permission error names the group
    // A group is named only when the mode gives it read and write: a node for the owner alone, or one the group may only read, is no group's to fix.
    for (unsigned mode : {0600u, 0640u, 0620u, 0604u}) {
        node.mode = mode;
        const std::string closed = mm::platform::pioOpenFailure(EACCES, &node);
        CAPTURE(mode);
        CHECK_FALSE(contains(closed, "must be in group"));
        CHECK(contains(closed, "grants its group no read and write"));
        CHECK(contains(closed, "udev rule"));
    }
    node.mode = 0660;
    CHECK(contains(mm::platform::pioOpenFailure(EACCES, &node), "must be in group pio"));
    node.mode = 0755;
    node.characterDevice = false;
    CHECK(mm::platform::pioModeString(node) == "-rwxr-xr-x");
    CHECK(contains(mm::platform::pioOpenFailure(ENOENT, nullptr), "no /dev/pio0"));
    CHECK(contains(mm::platform::pioOpenFailure(ENOENT, nullptr), "once per process"));   // a node that appears later needs a restart
}

TEST_CASE("Hub75PioPiolib: a claim that fails says why and gives the instance back") {
    Backend b;
    shim.claimRc = -EBUSY;
    CHECK_FALSE(b.backend->open(b.error));
    CHECK(contains(b.error, "cannot claim a PIO state machine"));
    CHECK(contains(b.error, std::strerror(EBUSY)));
    CHECK(shim.calls == std::vector<std::string>{"open", "claim", "release_none"});
}

TEST_CASE("Hub75PioPiolib: a start before the open is refused and calls nothing") {
    Backend b;
    CHECK_FALSE(b.backend->start(config(), b.error));
    CHECK(contains(b.error, "not opened"));
    CHECK(shim.calls.empty());
}

TEST_CASE("Hub75PioPiolib: a DMA ring another client holds is named, and nothing is loaded after it") {
    Backend b;
    REQUIRE(b.backend->open(b.error));
    shim.xferConfigRc = -EBUSY;
    CHECK_FALSE(b.backend->start(config(), b.error));
    CHECK(contains(b.error, "buf_size=8192 buf_count=4"));
    CHECK(contains(b.error, "another client holds the DMA"));
    CHECK(shim.calls == std::vector<std::string>{"open", "claim", "config_xfer"});
    b.backend->close();
    CHECK(shim.calls.back() == "release");   // the claim goes back; no program was loaded to remove
    CHECK(std::count(shim.calls.begin(), shim.calls.end(), "remove_program") == 0);
}

TEST_CASE("Hub75PioPiolib: a program that will not load says why, and the DMA ring is already configured") {
    Backend b;
    REQUIRE(b.backend->open(b.error));
    shim.programRc = -ENOSPC;
    CHECK_FALSE(b.backend->start(config(), b.error));
    CHECK(contains(b.error, "cannot load the PIO program"));
    CHECK(contains(b.error, "3 of the 32 instruction words"));
    CHECK(shim.calls == std::vector<std::string>{"open", "claim", "config_xfer", "add_program"});
    b.backend->close();
    CHECK(std::count(shim.calls.begin(), shim.calls.end(), "remove_program") == 0);
}

TEST_CASE("Hub75PioPiolib: a start runs the DMA ring, the program, the init and the enable in that order, once each") {
    Backend b;
    REQUIRE(b.backend->open(b.error));
    REQUIRE(b.backend->start(config(), b.error));
    CHECK(shim.calls == std::vector<std::string>{"open", "claim", "config_xfer", "add_program", "init_sm", "clear_fifos", "enable"});
    CHECK(shim.initParams.offset == 3);                       // where the program loaded
    CHECK(shim.initParams.wrap == 2);
    CHECK(shim.initParams.clkdiv_int == 4);
    CHECK(shim.initParams.idle_wide_word == mm::platform::kBlankWideWord);   // the pins come up dark
    b.backend->close();
}

TEST_CASE("Hub75PioPiolib: a submit before the start answers -ENODEV, and after it passes the shim's code through") {
    Backend b;
    std::vector<uint32_t> frame(64, 0);
    CHECK(b.backend->submitFrame(frame.data(), 256) == -ENODEV);
    REQUIRE(b.backend->open(b.error));
    CHECK(b.backend->submitFrame(frame.data(), 256) == -ENODEV);   // opened is not started
    REQUIRE(b.backend->start(config(), b.error));
    CHECK(b.backend->submitFrame(frame.data(), 256) == 0);
    CHECK(shim.lastXferBytes == 256);
    shim.xferRc = -EINTR;
    CHECK(b.backend->submitFrame(frame.data(), 256) == -EINTR);
    shim.xferRc = -ETIMEDOUT;   // the 6.12 kernel's 1 s wait, which the pinned piolib returns instead of exiting
    CHECK(b.backend->submitFrame(frame.data(), 256) == -ETIMEDOUT);
    b.backend->close();
}

TEST_CASE("Hub75PioPiolib: the blank goes out, the FIFO drains, then the SM stops and the program goes, in that order") {
    Backend b;
    REQUIRE(b.backend->open(b.error));
    REQUIRE(b.backend->start(config(), b.error));
    shim.calls.clear();
    std::vector<uint32_t> blank(8192, mm::platform::kBlankWideWord);
    CHECK(b.backend->blankAndStop(blank.data(), static_cast<uint32_t>(blank.size() * 4), b.error));
    CHECK(shim.lastXferBytes == 32768);
    CHECK(shim.calls == std::vector<std::string>{"xfer", "disable", "clear_fifos", "remove_program"});
    b.backend->close();
    CHECK(shim.calls.back() == "release");
}

TEST_CASE("Hub75PioPiolib: a blank that fails, or a FIFO that never empties, still stops the SM and says so") {
    {
        Backend b;
        REQUIRE(b.backend->open(b.error));
        REQUIRE(b.backend->start(config(), b.error));
        shim.xferRc = -EIO;
        std::vector<uint32_t> blank(64, 0);
        CHECK_FALSE(b.backend->blankAndStop(blank.data(), 256, b.error));
        CHECK(contains(b.error, "blank frame submit failed"));
        CHECK(std::count(shim.calls.begin(), shim.calls.end(), "disable") == 1);
        CHECK(std::count(shim.calls.begin(), shim.calls.end(), "remove_program") == 1);
        b.backend->close();
    }
    {
        Backend b;
        REQUIRE(b.backend->open(b.error));
        REQUIRE(b.backend->start(config(), b.error));
        shim.fifoEmpty = false;
        std::vector<uint32_t> blank(64, 0);
        CHECK_FALSE(b.backend->blankAndStop(blank.data(), 256, b.error));
        CHECK(contains(b.error, "did not drain"));
        CHECK(std::count(shim.calls.begin(), shim.calls.end(), "disable") == 1);
        b.backend->close();
    }
}

TEST_CASE("Hub75PioPiolib: a close without a blank still disables the SM, removes the program and releases the claim, once") {
    Backend b;
    REQUIRE(b.backend->open(b.error));
    REQUIRE(b.backend->start(config(), b.error));
    shim.calls.clear();
    b.backend->close();
    CHECK(shim.calls == std::vector<std::string>{"disable", "remove_program", "release"});
    b.backend->close();   // a second close finds everything released already
    CHECK(shim.calls.size() == 3);
}

#endif
#endif
