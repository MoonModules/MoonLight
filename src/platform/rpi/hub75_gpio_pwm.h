// SPDX-License-Identifier: GPL-3.0-or-later

/// The hardware PWM OE pulser for the Pi 3/4 GPIO backend: the BCM PWM peripheral times the panel's OE (output enable) line, not the CPU and not DMA, so clock-in time hides inside OE on-time.
///
/// The CPU fires one pulse per bitplane and clocks the next row's data in while the pulse runs; clocking and channel setup are in @xref{clocking|Clocking} and @xref{channel-mode|Channel mode}, the FIFO words in @xref{pulse-words|Pulse words}, the call order in @xref{pulse-lifecycle|Pulse lifecycle}.
///
/// This is the architecture hzeller's rpi-rgb-led-matrix uses for its classic Pi 1-4 backend (its HardwarePinPulser), written here from scratch from the BCM2835/BCM2711 datasheets and the published timing numbers (@xref{license|License}).
///
/// The PWM output reaches the outside world only on GPIO 18 and GPIO 12 (@xref{oe-pin|OE pin}), and the onboard analog audio driver uses the same PWM block (@xref{audio-driver-conflict|Audio driver conflict}).
///
/// @attribution rpi-rgb-led-matrix
///
/// @technical
///
/// ## Clocking
///
/// The clock is PLLD divided by an integer divider, and PLLD runs at 500 MHz on a Pi 3 and 750 MHz on a Pi 4 (`hub75GpioPlldHz`). One PWM tick is the divider over PLLD, and the LSB pulse is always exactly 2 ticks, so the divider for an LSB time is `lsbNs x PLLD / 2e9`, truncated.
///
/// 130 ns asks divider 32 on a Pi 3 and 48 on a Pi 4, each a 64 ns tick and a 128 ns LSB: a 1.5% quantization error. The LSB steps by 4 ns on a Pi 3 and 2.67 ns on a Pi 4.
///
/// ## Channel mode
///
/// Channel 1 (PWM0) runs in FIFO mode with inverted polarity (BCM2835 ARM Peripherals section 9): CTL = USEF1 | PWEN1 | POLA1 while a pulse runs.
///
/// The polarity is inverted because OE is active-low: a FIFO word equal to the range drives the pin low (OE active, LEDs on) for one full PWM period.
///
/// The silence bit stays 0, so when the FIFO empties the pin returns high (OE inactive, LEDs off).
///
/// The wait disables the channel (CTL = USEF1 | POLA1 | CLRF1), so the output is dark while the dump changes the address and latches, whatever the FIFO held; the next sendPulse re-enables it.
///
/// The channel is enabled per pulse and disabled between pulses.
///
/// ## Pulse words
///
/// A pulse for bitplane p lasts 2^(p+1) ticks. Ranges below 16 go out as one FIFO word holding the full range; ranges of 16 or more are split into 8 FIFO words of range/8 each, keeping the range register small so the trailing zero phase is short.
///
/// Two zero-valued sentinel words follow: one returns the output to its default state, and the second makes the "FIFO empty" detection reliable, since a buffering register appears to sit behind the FIFO (the reference design's comment records the same observation).
///
/// ## Pulse lifecycle
///
/// configure() takes the mapped PWM and clock manager blocks, which must stay mapped until stop().
///
/// sendPulse() sets the range, writes the FIFO words and re-enables the channel (the wait disabled it). The pulse runs asynchronously: the caller clocks the next row's data in while it runs, then waits for it before latching.
///
/// The caller fires sendPulse() once the previous pulse has finished: the FIFO must be empty, otherwise the words queue behind the live pulse.
///
/// waitPulseFinished() waits until the FIFO is empty (the output is back to dark), then disables the channel and clears the FIFO, so the address change and the latch that follow happen in the dark.
///
/// It sleeps until near the pulse's computed end, then polls the FIFO-empty bit, and returns false after a 2 ms grace past the end. The caller then proceeds anyway, since a stuck PWM must not hang the refresh thread (the backend counts the timeout), and the channel is disabled, so the wedged pulse cannot light the next row.
///
/// stop() disables the channel and clears the FIFO. The clock keeps running and the next configure() reprograms it.
///
/// The OE pin's level after stop() is the caller's business: the backend muxes OE back to a GPIO output driving high (dark) before calling it.
///
/// ## OE pin
///
/// The PWM output reaches the outside world only on GPIO 18 (ALT5) or GPIO 12 (ALT0). The board puts OE on GPIO 18, so the pulser needs no fallback; a board that moved OE elsewhere could not use this pulser at all.
///
/// ## Audio driver conflict
///
/// The onboard analog audio driver (snd_bcm2835) uses the same PWM hardware. The backend refuses to open while it is loaded (hub75_gpio_backend_hw.cpp), since two drivers programming one PWM block would fight over the clock and the FIFO.
///
/// ## License
///
/// Every file in hzeller's lib/ carries a GPL-2.0-only header, which cannot combine into this GPL-3.0-or-later tree, so no hzeller code is adapted; docs/reference/attributions.md holds the license check.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace mm::platform {

// PWM block registers (offsets within the PWM block; BCM2835 section 9.6).

constexpr uint32_t kGpioPwmCtlOffset = 0x00;   ///< control
constexpr uint32_t kGpioPwmStaOffset = 0x04;   ///< status
constexpr uint32_t kGpioPwmRng1Offset = 0x10;  ///< channel 1 range
constexpr uint32_t kGpioPwmFif1Offset = 0x18;  ///< FIFO input

// PWM_CTL bits, channel 1.

constexpr uint32_t kGpioPwmCtlPwen1 = (1u << 0);  ///< channel enable
constexpr uint32_t kGpioPwmCtlPola1 = (1u << 4);  ///< invert output polarity
constexpr uint32_t kGpioPwmCtlUsef1 = (1u << 5);  ///< use FIFO for data
constexpr uint32_t kGpioPwmCtlClrf1 = (1u << 6);  ///< clear FIFO (self-clearing)

// PWM_STA bits, channel 1.

constexpr uint32_t kGpioPwmStaEmpt1 = (1u << 1);  ///< FIFO empty

// Clock manager registers for the PWM clock (BCM2835 section 6.3); each is 4 bytes, so register N sits at offset N*4.

constexpr uint32_t kGpioClkPwmCtlOffset = 40 * 4;     ///< CM_PWMCTL
constexpr uint32_t kGpioClkPwmDivOffset = 41 * 4;     ///< CM_PWMDIV
constexpr uint32_t kGpioClkPassword = (0x5Au << 24);  ///< every clock write carries this
constexpr uint32_t kGpioClkSrcPlld = 6;               ///< clock source PLLD
constexpr uint32_t kGpioClkCtlEnab = (1u << 4);       ///< clock enable
constexpr uint32_t kGpioClkCtlKill = (1u << 5);       ///< clock kill
constexpr uint32_t kGpioClkCtlBusy = (1u << 7);       ///< clock busy

/// The PWM clocking a requested LSB time resolves to: the divider and the LSB time the hardware produces.
struct GpioPwmTiming {
    uint16_t divider = 32;       ///< PLLD divider, 1..4095 (12-bit field)
    uint16_t lsbActualNs = 128;  ///< the LSB pulse in ns (2 ticks)
};

/// The clocking for `lsbNs` with PLLD at `plldHz` (divider = lsbNs x PLLD / 2e9, truncated, clamped to the 12-bit field), a pure function the unit test pins at 50, 130 and 3000 ns on both PLLD rates.
GpioPwmTiming hub75GpioPwmTiming(uint16_t lsbNs, uint32_t plldHz);

/// PWM ticks in bitplane `plane`'s pulse: the LSB pulse is 2 ticks, doubling per plane, for planes 0..10 (the encoder's depth caps at 8, so plane <= 7 in practice).
inline uint16_t hub75GpioPulseTicks(uint8_t plane) {
    return static_cast<uint16_t>(2u << plane);
}

/// One pulse as the FIFO takes it: the range register's value, the FIFO words (the pulse split, then two zero sentinels), and their count.
struct GpioPulseWords {
    uint16_t range = 0;       ///< the range register's value
    uint16_t words[10] = {};  ///< the FIFO words
    uint8_t count = 0;        ///< how many of `words` are used
};

/// The FIFO words for bitplane `plane`'s pulse (@xref{pulse-words|Pulse words}), a pure function the unit test pins at the split boundary (15 vs 16 ticks) and the top plane.
GpioPulseWords hub75GpioPulseWords(uint8_t plane);

/// The OE pulser over mapped PWM and clock registers, one instance per backend: the backend owns the mappings and hands the pulser the register bases.
///
/// All register access is through volatile pointers, so the compiler emits every write the hardware must see, in order. The call order is in @xref{pulse-lifecycle|Pulse lifecycle}.
class GpioPwmPulser {
  public:
    /// An unconfigured pulser; configure() attaches it to the mapped registers.
    GpioPwmPulser() = default;
    /// Copying is deleted: one pulser per backend owns the PWM channel.
    GpioPwmPulser(const GpioPwmPulser&) = delete;
    /// Copy assignment is deleted for the same reason.
    GpioPwmPulser& operator=(const GpioPwmPulser&) = delete;

    /// Programs the PWM clock for `lsbNs` with PLLD at `plldHz` and sets channel 1 up (FIFO mode, inverted, idle dark); `pwm` and `clk` stay mapped until stop(); false with `error` filled when a pointer is null or the clock does not stop or start in time.
    bool configure(volatile uint32_t* pwm, volatile uint32_t* clk, uint16_t lsbNs, uint32_t plldHz,
                   std::string& error);

    /// Fires the OE pulse for bitplane `plane` and returns while it runs; call it once the previous pulse has finished (waitPulseFinished()), or the words queue behind the live pulse (@xref{pulse-lifecycle|Pulse lifecycle}).
    void sendPulse(uint8_t plane);

    /// Waits until the fired pulse has finished, then disables the channel and clears the FIFO so the output is dark until the next sendPulse; false after a 2 ms grace past the pulse's end.
    bool waitPulseFinished();

    /// Disables the channel and clears the FIFO, leaving the clock running for the next configure(); the caller drives OE dark as a GPIO output first (@xref{pulse-lifecycle|Pulse lifecycle}).
    void stop();

  private:
    volatile uint32_t* pwm_ = nullptr;
    volatile uint32_t* clk_ = nullptr;
    GpioPwmTiming timing_{};
    uint32_t plldHz_ = 500'000'000;   // the rate the divider was derived at, which a pulse's length is read back through
    std::chrono::steady_clock::time_point pulseEnd_{};
};

}  // namespace mm::platform
