// SPDX-License-Identifier: GPL-3.0-or-later

/// PioBackend on the RP1 PIO driver's own interface: it opens `/dev/pio0` and issues the driver's ioctls through the device seam (hub75_pio_backend_rp1.h), with the argument layouts and the register packing in hub75_pio_rp1_uapi.h, and no library in between.
///
/// What each ioctl returns, how a submit blocks on each kernel line and what a close releases are cited from the kernel sources in hub75_pio_backend.h (Submit semantics, Control calls, Start, Stop).
///
/// @technical
///
/// ## The device node
///
/// On the bench Pi, /dev/pio0 reads `crw-rw---- root gpio`: the group owns it, so the user MoonLight runs as must be in group `gpio`. A failed open prints the node's actual mode, owner and group, so the message names the group the machine uses, not a guess. The open does not pre-check access: a check and the open are two steps, and the open's own errno is the reliable one.
///
/// ## What the ioctl results mean
///
/// A result of 0 or more is success: the claim returns the state machine's index, the program load its offset (0 is a valid one) and the FIFO state the 8 bytes it wrote back. A negative result is the errno. The firmware's own refusals pass through the driver unchanged, so their codes are what a bench run reads.
///
/// ## The claim buffer
///
/// Released kernels copy a positive result back over the argument, as many bytes as the result, so claiming state machine 3 writes 3 bytes into the 2-byte claim struct. The claim therefore passes a buffer of 8 bytes. Close releases the claim, the program references and the DMA channel, so a reopen needs no memory of the state machine it held.
///
/// ## The frame argument
///
/// An interrupted transfer rewrites its argument in place with the data pointer advanced and the byte count reduced, on 6.18.53 and later. Every submit builds a fresh argument, and a failed submit is reported to the engine, which retries after a pause with a fresh one.

#include "platform/rpi/hub75_pio_backend_rp1.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "platform/rpi/hub75_pio_pins.h"
#include "platform/rpi/hub75_pio_rp1_uapi.h"

namespace mm::platform {

namespace {

/// How long stop() polls the TX FIFO for empty, after the blank's wire time, before disabling the SM anyway; the FIFO holds at most 8 words per direction, so any sane pixel clock drains it in microseconds.
constexpr int kFifoDrainTimeoutMs = 100;

constexpr uint32_t kOutMask = ((1u << kOutPinCount) - 1u) << kOutBaseGpio;   // GPIO 2 to 27, one bit per pin

// The claim's buffer: the 2-byte argument and room for the copy-back of a released kernel (see the claim buffer in the lead).
struct ClaimBuffer {
    rp1::SmClaimArgs args;
    uint8_t copyBackRoom[6];
};

class Rp1Backend : public PioBackend {
  public:
    Rp1Backend(Rp1PioDevice& device, std::unique_ptr<Rp1PioDevice> owned) : device_(device), owned_(std::move(owned)) {}

    bool open(std::string& error) override {
        if (opened_) return true;   // the file and its claim are held already: a second open would leak both
        const int err = device_.open();
        if (err != 0) {
            PioNode node;
            error = pioRp1OpenFailure(err, device_.node(node) ? &node : nullptr);
            return false;
        }
        ClaimBuffer claim{};   // mask 0: any free state machine
        const int sm = device_.sendIoctl(rp1::kIocSmClaim, &claim, sizeof(claim));
        if (sm < 0 || sm >= rp1::kSmCount) {
            error = pioClaimFailure(sm < 0 ? sm : -EIO);
            device_.close();
            return false;
        }
        sm_ = sm;
        opened_ = true;
        return true;
    }

    bool start(const PioStartConfig& config, std::string& error) override {
        if (!opened_) {
            error = "rp1 backend not opened";
            return false;
        }
        if (started_ || programLoaded_) {
            error = "rp1 backend already started";   // a second load would add a reference the stop cannot remove
            return false;
        }
        // The DMA ring first, so that a ring another client holds fails before the program is loaded or a pin is taken.
        if (!configureRing(config, error) || !loadProgram(config, error) || !configureSm(config, error) || !armSm(error)) return false;
        const uint64_t ringWords = static_cast<uint64_t>(config.xferBufSize) * config.xferBufCount / kBytesPerPixelClock;
        blankWaitMicros_ = pioBlankWaitMicros(ringWords, config.clkdivInt, config.clkdivFrac);
        started_ = true;
        dark_ = false;   // a started SM drives its own pins, so a stop owes the dark word
        return true;
    }

    int submitFrame(const void* data, uint32_t bytes) override {
        if (!started_) return -ENODEV;
        // A fresh argument each call: the kernel rewrites it in place when a signal interrupts the transfer.
        rp1::SmXferData32Args args{static_cast<uint16_t>(sm_), rp1::kDirToSm, bytes, reinterpret_cast<uintptr_t>(data)};
        const int rc = call(rp1::kIocSmXferData32, args);
        return rc < 0 ? rc : 0;
    }

    bool stop(bool drain, std::string& error) override {
        error.clear();   // this stop's notes and failures alone
        // The flag goes first, so a refresh thread released late finds the backend stopped and makes no further submit.
        const bool wasStarted = started_.exchange(false);
        if (wasStarted && drain) {
            // The refresh thread's ring-deep blank is queued, and on 6.18.39 and later an empty FIFO does not show the DMA finished: wait out the blank's wire time, then poll. A FIFO that stays full is a note, since the disable darkens the panel anyway.
            std::this_thread::sleep_for(std::chrono::microseconds(blankWaitMicros_));
            const std::string note = waitForFifoDrain();
            if (!note.empty()) addFailure(error, note);
        }
        if (!dark_) darken(error);
        if (programLoaded_) {
            // The result is not read: a program left loaded is dropped by the close, which releases every reference this open holds.
            rp1::RemoveProgramArgs args{};
            args.numInstrs = static_cast<uint16_t>(programLength_);
            args.origin = static_cast<uint16_t>(offset_);
            call(rp1::kIocRemoveProgram, args);
            programLoaded_ = false;
        }
        return dark_;
    }

    void close() override {
        if (opened_) {
            device_.close();
            sm_ = -1;
            opened_ = false;
            // The close released the SM and the program, so no SM is held to stop; the pins keep their last value.
            started_ = false;
            programLoaded_ = false;
            dark_ = true;
        }
    }

    const char* name() const override { return "rp1 (/dev/pio0)"; }

  private:
    template <class Args>
    int call(unsigned long request, Args& args) {
        return device_.sendIoctl(request, &args, sizeof(Args));
    }

    // Sets the OUT pins (GPIO 2..27) to a wide word, bit 0 on GPIO 2: one firmware message, the same call at the start and the stop, effective with the SM disabled (measured on 6.18.60).
    int setOutPins(uint32_t wideWord) {
        rp1::SmSetPinsArgs args{};
        args.sm = static_cast<uint16_t>(sm_);
        args.values = wideWord << kOutBaseGpio;
        args.mask = kOutMask;
        return call(rp1::kIocSmSetPins, args);
    }

    bool configureRing(const PioStartConfig& config, std::string& error) {
        // CONFIG_XFER32, since a 64 KiB buffer does not fit the 16 bits of CONFIG_XFER.
        rp1::SmConfigXfer32Args args{};
        args.sm = static_cast<uint16_t>(sm_);
        args.dir = rp1::kDirToSm;
        args.bufSize = config.xferBufSize;
        args.bufCount = config.xferBufCount;
        const int rc = call(rp1::kIocSmConfigXfer32, args);
        if (rc < 0) error = pioXferConfigFailure(rc, config.xferBufSize, config.xferBufCount);
        return rc >= 0;
    }

    bool loadProgram(const PioStartConfig& config, std::string& error) {
        if (config.programLength < 1 || config.programLength > rp1::kInstructionCount) {
            error = pioProgramFailure(-EINVAL, config.programLength);
            return false;
        }
        rp1::AddProgramArgs args{};
        args.numInstrs = static_cast<uint16_t>(config.programLength);
        args.origin = rp1::kOriginAny;
        std::memcpy(args.instrs, config.programInstructions, config.programLength * sizeof(uint16_t));
        const int offset = call(rp1::kIocAddProgram, args);
        if (offset < 0) {
            error = pioProgramFailure(offset, config.programLength);
            return false;
        }
        offset_ = offset;
        programLength_ = config.programLength;
        programLoaded_ = true;
        return true;
    }

    // The pins come up holding the idle word: values, then directions, then the mux, so the panel stays dark until the first frame. An output value of 0 is OE low (lit) on row 0, so switching first would light one row.
    bool configureSm(const PioStartConfig& config, std::string& error) {
        if (!step(setOutPins(kBlankWideWord), "set the PIO pins to the idle word", error)) return false;
        rp1::SmSetPindirsArgs dirs{};
        dirs.sm = static_cast<uint16_t>(sm_);
        dirs.dirs = kOutMask;
        dirs.mask = kOutMask;
        if (!step(call(rp1::kIocSmSetPindirs, dirs), "set the PIO pin directions", error)) return false;
        for (uint16_t gpio = kOutBaseGpio; gpio < kOutBaseGpio + kOutPinCount; gpio++) {
            rp1::GpioSetFunctionArgs mux{};
            mux.gpio = gpio;
            mux.function = rp1::kGpioFunctionPio;
            if (!step(call(rp1::kIocGpioSetFunction, mux), "hand a GPIO to the PIO", error)) return false;
        }
        rp1::SmInitArgs init{};
        init.sm = static_cast<uint16_t>(sm_);
        init.initialPc = static_cast<uint16_t>(offset_);
        init.config = smConfig(config);
        return step(call(rp1::kIocSmInit, init), "initialize the PIO state machine", error);
    }

    // The config registers of the hub75_wide program: wrap, one side-set pin on the clock, OUT on GPIO 2..27, shift right with no autopull because the program pulls one word a pixel clock, and the TX FIFO joined.
    rp1::SmConfig smConfig(const PioStartConfig& config) const {
        rp1::ExecControl exec;
        exec.wrapBottom = static_cast<uint8_t>(offset_ + config.wrapTarget);
        exec.wrapTop = static_cast<uint8_t>(offset_ + config.wrap);
        rp1::ShiftControl shift;
        shift.joinTx = true;
        shift.outShiftRight = true;
        shift.inShiftRight = true;
        rp1::PinControl pins;
        pins.sidesetCount = 1;
        pins.sidesetBase = kClkGpio;
        pins.outCount = kOutPinCount;
        pins.outBase = kOutBaseGpio;
        return {rp1::packClkdiv(config.clkdivInt, config.clkdivFrac), rp1::packExecctrl(exec), rp1::packShiftctrl(shift), rp1::packPinctrl(pins)};
    }

    bool armSm(std::string& error) {
        rp1::SmClearFifosArgs clear{};
        clear.sm = static_cast<uint16_t>(sm_);
        if (!step(call(rp1::kIocSmClearFifos, clear), "clear the PIO FIFOs", error)) return false;
        return step(setEnabled(true), "enable the PIO state machine", error);
    }

    int setEnabled(bool enable) {
        rp1::SmSetEnabledArgs args{};
        args.mask = static_cast<uint16_t>(1u << sm_);   // a mask of state machines, not an index
        args.enable = enable ? 1 : 0;
        return call(rp1::kIocSmSetEnabled, args);
    }

    // Turns a failed ioctl result into the start's error text; true when `rc` is a success.
    static bool step(int rc, const char* what, std::string& error) {
        if (rc < 0) error = std::string("cannot ") + what + ": " + std::strerror(-rc);
        return rc >= 0;
    }

    // Disables the SM and sets the pins to the dark word, firmware-message calls each bounded by the kernel, so they work with a thread stuck in a submit. A failed ioctl is reported and the rest still run; the next stop repeats them.
    void darken(std::string& error) {
        const int disabled = setEnabled(false);
        if (disabled < 0) addFailure(error, std::string("cannot disable the PIO state machine: ") + std::strerror(-disabled));
        const int dark = setOutPins(kBlankWideWord);
        if (dark < 0) addFailure(error, std::string("cannot set the pins to the dark word: ") + std::strerror(-dark));
        rp1::SmClearFifosArgs clear{};
        clear.sm = static_cast<uint16_t>(sm_);
        call(rp1::kIocSmClearFifos, clear);
        dark_ = disabled >= 0 && dark >= 0;
    }

    // Adds one note or failure to the text the stop reports, after any earlier one.
    static void addFailure(std::string& error, const std::string& what) {
        if (!error.empty()) error += "; ";
        error += what;
    }

    // Polls the TX FIFO state (the `empty` byte with `tx` set) until it reads empty; returns "" then, else the note for the stop: the FIFO still held words after kFifoDrainTimeoutMs, or its state could not be read.
    std::string waitForFifoDrain() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kFifoDrainTimeoutMs);
        for (;;) {
            rp1::SmFifoStateArgs args{};
            args.sm = static_cast<uint16_t>(sm_);
            args.tx = 1;
            const int rc = call(rp1::kIocSmFifoState, args);
            if (rc < 0) return std::string("cannot read the TX FIFO state: ") + std::strerror(-rc) + "; disabling SM anyway";
            if (args.empty != 0) return "";
            if (std::chrono::steady_clock::now() >= deadline) return "TX FIFO did not drain within timeout; disabling SM anyway";
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    Rp1PioDevice& device_;
    std::unique_ptr<Rp1PioDevice> owned_;   // the real device, when the backend made it itself
    int sm_ = -1;
    int offset_ = -1;
    int programLength_ = 0;
    bool opened_ = false;
    std::atomic<bool> started_{false};   // written by the closing thread's stop(), read by the refresh thread's submits
    bool programLoaded_ = false;
    bool dark_ = true;   // the SM is disabled and the pins are on the dark word, or there is no SM held to stop; false from start() until a stop has done both once
    uint64_t blankWaitMicros_ = 0;   // the ring's wire time doubled for the feed lag; set by start()
};

}  // namespace

PioBackend* createRp1Backend(Rp1PioDevice& device) { return new Rp1Backend(device, nullptr); }

PioBackend* createRp1Backend(std::unique_ptr<Rp1PioDevice> device) {
    Rp1PioDevice& borrowed = *device;
    return new Rp1Backend(borrowed, std::move(device));
}

}  // namespace mm::platform
