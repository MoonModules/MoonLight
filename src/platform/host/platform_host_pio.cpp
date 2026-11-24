// SPDX-License-Identifier: GPL-3.0-or-later
/// The Raspberry Pi 5 platform layer behind the PIO seam (platform.h), on every host: a recorder and the real engine on its stub backend for the tests, and the engine on the rp1 backend where the build has it (Linux aarch64).
///
/// A build that lacks the rp1 backend refuses every open outside a test mode with that reason, so a PIO output says why its panels stay dark. A build with it opens the engine, which drives the panels from its own encode and refresh threads.
///
/// @technical
///
/// ## Availability
///
/// A test mode offers the backend on every host, and a test hook forces it either way; otherwise the host decides: the rp1 backend built in AND a PIO the kernel serves, the same refusal the open checks (/dev/pio0 present, no stall kernel).
///
/// A Pi 4 running a build with the rp1 backend is not offered the output, so the fallback picks only an output that can open.
///
/// ## Test modes
///
/// `Hub75PioMode::Record` stands a recorder in for the panels: an open keeps its options and a push keeps its frame. `Hub75PioMode::Stub` opens the real engine on the stub backend through the same branch a Pi takes, so a test drives the seam into the engine, its threads and its shutdown on any host. Both skip the kernel check, since no kernel is involved.
///
/// ## One task
///
/// The seam's one-task rule is in Hub75Pio.h. `realOpen_` is atomic so that the push and fps readers see a whole value; the rule, not the atomic, keeps a push from overlapping a close. (The engine's own publish guard keeps a push from writing into freed buffers even then.)
///
/// The rule holds because of two behaviors in core. `Drivers::prepare` joins the encode worker before its children prepare, and a parent prepares before its children, so no push runs while a child's output closes or opens.
///
/// And the Hub75Driver re-prepares the whole tree on a control change, because it keeps the default `PrepareScope::Tree`; a driver scoped to itself would re-prepare alone, outside that join. A driver that swaps outputs quiesces the render first.

#include "platform/Hub75Pio.h"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(__linux__)
    #include <unistd.h>
#endif

#include "platform/host/hub75_recorder.h"
#include "platform/rpi/hub75_pio_backend.h"   // kPioBenchedPixelClockHz: plain C++, so it builds on every host
#include "platform/rpi/hub75_pio_options.h"   // hub75PioCheckOptions, which the recorder refuses on
#ifdef MM_HUB75_PIO_HAS_ENGINE
    #include "platform/rpi/hub75_pio_backend_stub.h"
    #include "platform/rpi/hub75_pio_engine.h"
#endif

namespace mm::platform {

namespace {

Hub75Recorder rec_;   // the stand-in for the panels, which a test reads
std::string lastError_;
std::string warning_;   // what degrades the running panels: a clock past the benched feed
std::string closeError_;   // what the last close reported: a stuck thread, a failed stop or a note on a dark panel, empty when it finished clean

Hub75PioMode mode_ = Hub75PioMode::Off;
int testAvailable_ = -1;   // the test hook's forced availability: -1 the host decides, 0 forced unavailable, 1 forced available
Hub75PioOptions testOptions_;   // the options of the last successful open
Hub75PioTestPort testPort_;     // what hub75PioTestPort() fills and returns
uint32_t opens_ = 0;            // successful opens since the mode was set
uint32_t closes_ = 0;           // closes of an open seam since the mode was set
uint16_t testFps_ = 0;          // what the recorder answers for the rate
uint32_t testSubmitErrors_ = 0; // what the recorder answers for the failed submits
bool testStalled_ = false;      // what the recorder answers for a stalled submit
uint32_t realAttempts_ = 0;     // opens outside a test mode that were asked for
bool testOpenFails_ = false;    // whether a test mode refuses the open

#ifdef MM_HUB75_PIO_HAS_ENGINE
// The engine, one per process, lives until the process ends: a refresh thread stuck in the kernel may still be reading it at exit. A normal exit still blanks the panels, through the driver's release.
Hub75PioEngine& engine_ = *new Hub75PioEngine;
// The real-backend gate lives where every real open converges (hub75_pio_backend.h), so the seam only asks it.
bool gateOpen() { return pioBackendGateOpen(); }
void setGateOpen(bool open) { setPioBackendGateOpen(open); }
uint32_t realOpensNow() { return pioBackendRealOpens(); }
#else
// A build that omits the engine (MSVC): the same calls land on an engine whose every open refuses, so the seam keeps one code path; the recorder refuses what the engine would, and Stub mode refuses with the reason.
struct NoEngine : Hub75NoEngine<Hub75PioOptions> {
    NoEngine() : Hub75NoEngine("this build has no PIO engine") {}
    void setBrightness(uint8_t) {}
    void delayEncodes(uint32_t) {}
    void pauseEncodes(bool) {}
};
NoEngine& engine_ = *new NoEngine;
constexpr uint64_t kHub75StallMs = 0;
constexpr int kHub75JoinDeadlineMs = 0;
void stubBackendReset() {}
bool gateOpen() { return engine_.gateOpen(); }
void setGateOpen(bool open) { engine_.setGateOpen(open); }
uint32_t realOpensNow() { return 0; }
#endif
std::atomic<bool> realOpen_{false};   // whether the engine, rather than the recorder, holds the panels

// The open took: a clock past the benched feed degrades the running panels, so it is the warning.
void openTook(const Hub75PioOptions& o) {
    testOptions_ = o;
    opens_++;
    if (o.pixelClockHz > kPioBenchedPixelClockHz) {
        char text[128];
        std::snprintf(text, sizeof(text), "a pixel clock above %u MHz outruns the DMA feed measured on a Pi 5, so frames may stall inside a refresh",
                      static_cast<unsigned>(kPioBenchedPixelClockHz / 1'000'000));
        warning_ = text;
    }
}

bool fail(const std::string& why) {
    lastError_ = why;
    warning_.clear();   // a refused open runs no panel for a warning to describe
    return false;
}

// A kernel release's first three numbers, compared in order.
struct KernelVersion {
    unsigned major = 0, minor = 0, patch = 0;
    bool operator<(const KernelVersion& o) const {
        if (major != o.major) return major < o.major;
        if (minor != o.minor) return minor < o.minor;
        return patch < o.patch;
    }
};

// The kernels whose RP1 PIO driver stalls the next PIO open: 6.18.39 dropped the wait a teardown with DMA in flight relied on, 6.18.60 runs it again; measured with an IRQ thread holding the file, so what a close(fd) releases is a bench question.
constexpr KernelVersion kPioStallFrom{6, 18, 39};
constexpr KernelVersion kPioStallUntil{6, 18, 60};

std::string testKernel_;
bool testPioDevice_ = false;
bool testPioHost_ = false;
bool pioChecked_ = false;
std::string pioRefusal_;

// What the PIO check reads: the kernel release and whether /dev/pio0 exists, from the test's stand-in or the host.
void readPioHost(std::string& release, bool& device) {
    if (testPioHost_) {
        release = testKernel_;
        device = testPioDevice_;
        return;
    }
#if defined(__linux__)
    // The release alone, as `uname -r` prints it: a whole utsname would push the caller's frame past the build's limit.
    char line[64] = {};
    if (FILE* f = std::fopen("/proc/sys/kernel/osrelease", "r")) {
        if (std::fgets(line, sizeof(line), f)) line[std::strcspn(line, "\n")] = 0;
        std::fclose(f);
    }
    release = line;
    device = access("/dev/pio0", F_OK) == 0;
#else
    release.clear();
    device = false;
#endif
}

// The reason PIO cannot serve here, or empty: a missing device first, since a Pi 3 or 4 has no RP1 whatever its kernel.
std::string pioRefusalFor(const std::string& release, bool device) {
    if (!device) return "no /dev/pio0 on this system";
    KernelVersion v;
    if (std::sscanf(release.c_str(), "%u.%u.%u", &v.major, &v.minor, &v.patch) != 3) return {};
    if (v < kPioStallFrom || !(v < kPioStallUntil)) return {};
    char why[64];
    std::snprintf(why, sizeof(why), "kernel %u.%u.%u stalls a PIO reopen", v.major, v.minor, v.patch);
    return why;
}

}  // namespace

const char* hub75PioRefusal() {
    if (!pioChecked_) {
        std::string release;
        bool device = false;
        readPioHost(release, device);
        pioRefusal_ = pioRefusalFor(release, device);
        pioChecked_ = true;
    }
    return pioRefusal_.empty() ? nullptr : pioRefusal_.c_str();
}

void setTestPioHost(const char* kernelRelease, bool pioDevice) {
    testPioHost_ = kernelRelease != nullptr;
    testKernel_ = kernelRelease ? kernelRelease : "";
    testPioDevice_ = pioDevice;
    pioChecked_ = false;   // the next check reads the stand-in, or the host again
}

bool hub75PioAvailable() {
    // A test mode offers the backend on every host, and a test hook forces it either way; otherwise the host decides (@xref{availability}).
    if (mode_ != Hub75PioMode::Off) return true;
    if (testAvailable_ >= 0) return testAvailable_ != 0;
#ifdef MM_HUB75_PIO_HAVE_RP1
    return hub75PioRefusal() == nullptr;
#else
    return false;
#endif
}

// Why the real backend is not opened here, or null when it may be: the test binary's closed gate, a build outside Linux aarch64, or a kernel that stalls a PIO reopen (hub75PioRefusal). A reopened gate leaves the decision to createPioBackend.
static const char* realOpenRefusal() {
    if (!gateOpen()) return kPioTestRefusal;
#ifndef MM_HUB75_PIO_HAVE_RP1
    return kPioNoBackendRefusal;
#else
    return hub75PioRefusal();
#endif
}

bool hub75PioOpen(const Hub75PioOptions& o) {
    hub75PioClose();
    lastError_.clear();
    warning_.clear();
    if (mode_ != Hub75PioMode::Off && testOpenFails_) return fail("the test seam refuses the open");
    // The recorder stands in for panels of the size the options wire, so a test reads what a driver opened and pushed.
    if (mode_ == Hub75PioMode::Record) {
        if (!recordOpen(rec_, testOptions_, o, hub75PioCheckOptions, lastError_)) return false;
        openTook(o);
        return true;
    }
    // The real branch: the engine on the stub backend in Stub mode, on the rp1 backend otherwise.
    const bool stub = mode_ == Hub75PioMode::Stub;
    if (!stub) {
        // A refusal is the reason, not a warning: the PIO output has no fallback. A test binary is refused before /dev/pio0 is looked at, so a case stays clear of the live panels of the machine it runs on.
        realAttempts_++;
        if (const char* why = realOpenRefusal()) return fail(why);
    }
    if (!engineOpen(rec_, engine_, o, lastError_, stub)) return false;
    openTook(o);
    realOpen_.store(true, std::memory_order_release);
    return true;
}

const char* hub75PioLastError() { return lastError_.empty() ? nullptr : lastError_.c_str(); }

const char* hub75PioWarning() { return warning_.empty() ? nullptr : warning_.c_str(); }

const char* hub75PioCloseError() { return closeError_.empty() ? nullptr : closeError_.c_str(); }

uint16_t hub75PioWidth() MM_NONBLOCKING { return rec_.width(); }

uint16_t hub75PioHeight() MM_NONBLOCKING { return rec_.height(); }

void hub75PioPush(const uint8_t* rgb) MM_NONBLOCKING {
    if (realOpen_.load(std::memory_order_acquire)) {
        engine_.publishRgb(rgb);   // the encode thread encodes it and the refresh thread submits it; this call returns at once
        return;
    }
    rec_.push(rgb);
}

void hub75PioCorrection(uint8_t brightness) MM_NONBLOCKING {
    rec_.setBrightness(brightness);
    engine_.setBrightness(brightness);   // an atomic store the engine keeps across opens, so a value set before the open lights the first frame
}

uint16_t hub75PioFps() MM_NONBLOCKING {
    if (realOpen_.load(std::memory_order_acquire)) return engine_.fps();
    return rec_.width() != 0 ? testFps_ : 0;   // the recorder's rate, while it holds panels
}

bool hub75PioStalled() MM_NONBLOCKING {
    if (realOpen_.load(std::memory_order_acquire)) return engine_.stalled();
    return rec_.width() != 0 && testStalled_;   // the recorder's answer, while it holds panels
}

uint32_t hub75PioSubmitErrors() MM_NONBLOCKING {
    return hub75SeamSubmitErrors(realOpen_.load(std::memory_order_acquire), engine_, rec_, testSubmitErrors_);
}

// A closed seam holds no memory: the frames go back, the recorded options to their defaults.
void hub75PioClose() {
    if (hub75SeamHoldsPanels(realOpen_.load(), rec_)) closes_++;
    realOpen_.store(false, std::memory_order_release);   // before the shutdown, so no push reaches an engine that is stopping
    // The engine stops with the panel dark; harmless on an engine that is not open. A refresh thread stuck in a submit, a stop that failed or a note on a dark panel leaves its text here, whether the shutdown finished or not; a clean stop leaves none.
    engine_.shutdown();
    closeError_ = engine_.shutdownError();
    rec_.close();
    testOptions_ = Hub75PioOptions{};
    std::string().swap(warning_);
}

void setTestHub75Pio(Hub75PioMode mode) {
    hub75PioClose();
    mode_ = mode;
    rec_.recording = mode == Hub75PioMode::Record;
    rec_.setBrightness(255);
    engine_.setBrightness(255);
    opens_ = 0;
    closes_ = 0;
    testFps_ = 0;
    testSubmitErrors_ = 0;
    testStalled_ = false;
    testOpenFails_ = false;
    engine_.failEncodes(0);
    engine_.delayEncodes(0);
    engine_.pauseEncodes(false);
    engine_.setJoinDeadlineMs(kHub75JoinDeadlineMs);
    engine_.setStallMs(kHub75StallMs);
    stubBackendReset();
}

void setTestHub75PioFps(uint16_t hz) { testFps_ = hz; }

void setTestHub75PioGateOpen(bool open) { setGateOpen(open); }

uint32_t hub75PioTestRealAttempts() { return realAttempts_; }

uint32_t hub75PioTestRealOpens() { return realOpensNow(); }

void setTestHub75PioStalled(bool stalled) { testStalled_ = stalled; }

void setTestHub75PioStallMs(uint64_t ms) { engine_.setStallMs(ms); }

void setTestHub75PioJoinDeadlineMs(int ms) { engine_.setJoinDeadlineMs(ms); }

void setTestHub75PioSubmitErrors(uint32_t count) { testSubmitErrors_ = count; }

void setTestHub75PioOpenFails(bool fails) { testOpenFails_ = fails; }

void setTestHub75PioEncodeFails(int n) { engine_.failEncodes(n); }

// Force the availability hub75PioAvailable() reports, whatever the host: -1 the host decides again, 0 unavailable, 1 available.
void setTestHub75PioAvailable(int available) { testAvailable_ = available < 0 ? -1 : (available ? 1 : 0); }

const Hub75PioTestPort& hub75PioTestPort() {
    const bool engine = realOpen_.load(std::memory_order_acquire);
    testPort_.options = testOptions_;
    testPort_.opens = opens_;
    testPort_.closes = closes_;
    testPort_.brightness = rec_.brightness();
    testPort_.frames = engine ? static_cast<size_t>(engine_.framesPublished()) : rec_.frames();
    testPort_.engineHoldsPanels = engine;
    return testPort_;
}

// The consumer's side of the triple buffer: the newest frame pushed, always whole.
const uint8_t* hub75PioTestFrame() { return rec_.newestFrame(); }

}  // namespace mm::platform
