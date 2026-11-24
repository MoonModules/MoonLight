// SPDX-License-Identifier: GPL-3.0-or-later

/// The GpioBackend on real Pi 3/4 hardware: a CPU bit-bang plus PWM pulser, with the GPIO, PWM and clock-manager registers mapped through /dev/mem, the refresh thread clocking columns straight into GPSET0/GPCLR0 and the PWM pulser (hub75_gpio_pwm.h) timing OE.
///
/// This file's part is the register sink, the mapping, the pin directions and the dark-first setup; the dump loop lives in hub75_gpio_dump.h, shared with the stub backend (@xref{shared-dump-loop}, @xref{dark-first-setup}, @xref{provenance}).
///
/// @attribution rpi-rgb-led-matrix
///
/// @technical
///
/// ## Provenance
///
/// The code is written from the BCM2835/BCM2711 datasheets and the published timing numbers, and no hzeller code is adapted: every file in its lib/ carries a GPL-2.0-only header (the license check is in docs/reference/attributions.md).
///
/// The architecture, a realtime thread bit-banging precomputed GPIO words while a hardware PWM pulse times OE, follows the reference design's shape as the research report describes it; the code here is original.
///
/// ## Register map
///
/// Offsets are within each block (BCM2835 ARM Peripherals), with the SoC's peripheral base added (`hub75GpioPeripheralBase`):
///
/// ```
/// GPIO block (base + 0x200000): GPFSEL0-5 0x00-0x14, GPSET0 0x1C, GPCLR0 0x28, GPLEV0 0x34
/// PWM block (base + 0x20C000): see hub75_gpio_pwm.h
/// Clock manager (base + 0x101000): see hub75_gpio_pwm.h
/// ```
///
/// ## Shared dump loop
///
/// The dump loop is shared with the stub backend, so the unit test pins the sequence this file drives: rows outer, planes inner, the address set in the dark after the OE pulse has finished, the latch strobed, then the next plane's pulse fired.
///
/// This file supplies the register sink: GPSET0/GPCLR0 writes and GPLEV0 reads on the mapped GPIO block, and the pulser's wait and fire.
///
/// ## Dark-first setup
///
/// The dark-first setup order is structural: open() writes the dark levels, then makes the pins outputs; start() configures the PWM (idle high), then muxes OE to it.
///
/// The stub backend logs the same four steps in the same order, which the engine test pins; the bench shows the hardware order.
///
/// Writing GPSET/GPCLR sets the output latch even while the pins are still inputs, so the panel sees no lit row at startup.
///
/// ## Release order
///
/// blankAndStop() dumps the blank frame first and waits for it to drain, then stops; the blank frame's color bits are all clear, so its OE pulses light no pixel.
///
/// close() blanks before stopping, even on the error path: OE back to an output driving high, the rest dark, then the release.

#include "platform/rpi/hub75_gpio_backend.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "platform/rpi/hub75_gpio_dump.h"
#include "platform/rpi/hub75_gpio_pins.h"
#include "platform/rpi/hub75_gpio_pwm.h"

namespace mm::platform {

namespace {

// GPIO block register indices (offset / 4): @xref{register-map}.
constexpr uint32_t kGpioSet0Idx = 0x1C / 4;
constexpr uint32_t kGpioClr0Idx = 0x28 / 4;
constexpr uint32_t kGpioLev0Idx = 0x34 / 4;

// Block offsets from the peripheral base (BCM2835 section 1.2.3).
constexpr uint32_t kGpioBlockOffset = 0x200000;
constexpr uint32_t kPwmBlockOffset = 0x20C000;
constexpr uint32_t kClkBlockOffset = 0x101000;
constexpr size_t kMapSize = 4096;   // one page per block

// Every GPIO the board drives, for the direction setup.
const uint8_t kBoardPins[] = {
    shared::kOe, shared::kClk, shared::kStb,
    shared::kA, shared::kB, shared::kC, shared::kD, shared::kE,
    kLanePins[0].r1, kLanePins[0].g1, kLanePins[0].b1,
    kLanePins[0].r2, kLanePins[0].g2, kLanePins[0].b2,
    kLanePins[1].r1, kLanePins[1].g1, kLanePins[1].b1,
    kLanePins[1].r2, kLanePins[1].g2, kLanePins[1].b2,
    kLanePins[2].r1, kLanePins[2].g1, kLanePins[2].b1,
    kLanePins[2].r2, kLanePins[2].g2, kLanePins[2].b2,
};

// Whether the onboard analog audio driver is loaded: it owns the PWM the OE pulser needs, so the backend refuses to open while it runs.
bool audioDriverLoaded() {
    FILE* f = std::fopen("/proc/modules", "r");
    if (f == nullptr) return false;
    char line[256];
    bool loaded = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, "snd_bcm2835 ", 12) == 0) {
            loaded = true;
            break;
        }
    }
    std::fclose(f);
    return loaded;
}

// The dump loop's register sink: inline methods over raw pointers, so the hot loop pays no indirection beyond the register writes themselves (@xref{shared-dump-loop}).
struct HwSink {
    volatile uint32_t* gpio = nullptr;
    GpioPwmPulser* pulser = nullptr;
    void writeSet(uint32_t mask) { gpio[kGpioSet0Idx] = mask; }
    void writeClr(uint32_t mask) { gpio[kGpioClr0Idx] = mask; }
    uint32_t readLev() { return gpio[kGpioLev0Idx]; }
    bool waitPulseFinished() { return pulser->waitPulseFinished(); }
    void sendPulse(uint8_t plane) { pulser->sendPulse(plane); }
};

class HwBackend : public GpioBackend {
  public:
    bool open(std::string& error) override {
        if (opened_) {
            error = "GPIO backend already open";
            return false;
        }
        // The SoC decides the peripheral base and the PWM clock; a host that is no Pi 3 or 4, a Pi 5 included, is refused with its reason.
        if (const char* why = hub75GpioHostRefusal()) {
            error = why;
            return false;
        }
        soc_ = piSoc();
        const uint32_t periBase = hub75GpioPeripheralBase(soc_);
        if (audioDriverLoaded()) {
            error = "the onboard audio driver (snd_bcm2835) owns the PWM the OE pulser needs: "
                    "disable onboard audio (dtparam=audio=off) and try again";
            return false;
        }
        memFd_ = ::open("/dev/mem", O_RDWR | O_SYNC);
        if (memFd_ < 0) {
            error = "cannot open /dev/mem (run as root, or grant CAP_SYS_RAWIO and CAP_DAC_OVERRIDE: the hub75-gpio.conf drop-in in installing-on-linux.md)";
            return false;
        }
        gpio_ = mapBlock(periBase + kGpioBlockOffset);
        pwm_ = mapBlock(periBase + kPwmBlockOffset);
        clk_ = mapBlock(periBase + kClkBlockOffset);
        if (gpio_ == nullptr || pwm_ == nullptr || clk_ == nullptr) {
            error = "cannot map the GPIO/PWM registers through /dev/mem";
            unmapAll();
            ::close(memFd_);
            memFd_ = -1;
            return false;
        }

        // Dark first: the dark levels reach the output registers BEFORE any pin becomes an output (@xref{dark-first-setup}).
        writeDarkLevels();
        for (uint8_t pin : kBoardPins) setGpioFunction(pin, kGpioGpioOutput);

        opened_ = true;
        return true;
    }

    bool start(const GpioSubmitConfig& config, std::string& error) override {
        if (!hub75GpioCheckStart(opened_, config, error)) return false;
        config_ = config;
        slowdown_ = hub75GpioResolveSlowdown(config.slowdown, soc_);

        // Configure the PWM (idle high = dark), THEN mux OE to it: the pin is high throughout, GPIO-driven then PWM-driven.
        if (!pulser_.configure(pwm_, clk_, config.lsbNs, hub75GpioPlldHz(soc_), error)) {
            return false;
        }
        static_assert(shared::kOe == 18, "ALT5 is PWM0 on GPIO 18 alone: another OE pin selects another pin's ALT5 function (hub75_gpio_pwm.h, OE pin)");
        setGpioFunction(shared::kOe, kGpioGpioAlt5);
        started_ = true;
        return true;
    }

    int submitFrame(const uint32_t* words, size_t wordCount) override {
        if (!started_ || words == nullptr) return -1;
        // The shared dump loop over the register sink: the stub backend runs the same loop against a recorder (@xref{shared-dump-loop}).
        HwSink sink{gpio_, &pulser_};
        return hub75GpioDumpFrame(sink, config_, words, wordCount, slowdown_,
                                  pulseTimeouts_);
    }

    bool blankAndStop(const uint32_t* blankWords, size_t blankWordCount,
                      std::string& error) override {
        // Blank first: dump the blank frame, wait for it to drain, then stop (@xref{release-order}).
        bool ok = true;
        if (started_ && blankWords != nullptr) {
            ok = submitFrame(blankWords, blankWordCount) == 0;
            if (!ok) error = "the blank frame did not dump, so the panel may hold its last row";
            if (!pulser_.waitPulseFinished()) {
                pulseTimeouts_++;
                if (ok) error = "the last pulse did not finish, so the panel may hold its last row";
                ok = false;
            }
        }
        started_ = false;
        if (gpio_ != nullptr) {
            // OE back to a GPIO output driving high (dark) before the PWM stops, so the pin's level is defined throughout.
            setGpioFunction(shared::kOe, kGpioGpioOutput);
            writeDarkLevels();
        }
        pulser_.stop();
        return ok;
    }

    void close() override {
        if (gpio_ != nullptr) {
            // Blank before stopping, even on the error path (@xref{release-order}).
            setGpioFunction(shared::kOe, kGpioGpioOutput);
            writeDarkLevels();
        }
        pulser_.stop();
        started_ = false;
        opened_ = false;
        unmapAll();
        if (memFd_ >= 0) {
            ::close(memFd_);
            memFd_ = -1;
        }
        soc_ = PiSoc::Other;
        slowdown_ = 1;
    }

    const char* name() const override { return "GPIO+PWM (Pi 3/4)"; }

    uint32_t plldHz() const override { return hub75GpioPlldHz(soc_); }
    int resolvedSlowdown() const override { return slowdown_; }
    uint64_t pulseTimeouts() const override { return pulseTimeouts_; }

  private:
    volatile uint32_t* mapBlock(uint32_t physAddr) {
        void* map = ::mmap(nullptr, kMapSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                           memFd_, static_cast<off_t>(physAddr));
        if (map == MAP_FAILED) return nullptr;
        return static_cast<volatile uint32_t*>(map);
    }

    void unmapAll() {
        if (gpio_ != nullptr) {
            ::munmap(const_cast<uint32_t*>(gpio_), kMapSize);
            gpio_ = nullptr;
        }
        if (pwm_ != nullptr) {
            ::munmap(const_cast<uint32_t*>(pwm_), kMapSize);
            pwm_ = nullptr;
        }
        if (clk_ != nullptr) {
            ::munmap(const_cast<uint32_t*>(clk_), kMapSize);
            clk_ = nullptr;
        }
    }

    // The dark levels: OE high (blanked), color and address 0, latch and clock low, written to the output registers; safe while the pins are inputs (it sets the latch) or outputs.
    void writeDarkLevels() {
        gpio_[kGpioSet0Idx] = kGpioOeBit;
        gpio_[kGpioClr0Idx] = kGpioColorMask | kGpioAddressMask | kGpioStbBit | kGpioClkBit;
    }

    void setGpioFunction(uint8_t pin, uint32_t mode) {
        const int reg = gpioGpfselReg(pin);
        const int shift = gpioGpfselShift(pin);
        gpio_[reg] = (gpio_[reg] & ~(7u << shift)) | ((mode & 7u) << shift);
    }

    int memFd_ = -1;
    volatile uint32_t* gpio_ = nullptr;
    volatile uint32_t* pwm_ = nullptr;
    volatile uint32_t* clk_ = nullptr;
    GpioPwmPulser pulser_;
    PiSoc soc_ = PiSoc::Other;
    GpioSubmitConfig config_{};
    int slowdown_ = 1;
    bool opened_ = false;
    bool started_ = false;
    uint64_t pulseTimeouts_ = 0;
};

}  // namespace

GpioBackend* createGpioHwBackend() { return new HwBackend(); }

}  // namespace mm::platform
