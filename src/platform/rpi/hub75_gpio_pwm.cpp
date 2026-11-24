// SPDX-License-Identifier: GPL-3.0-or-later

/// The PWM pulse timing, the FIFO pulse words and the `GpioPwmPulser` methods, documented in hub75_gpio_pwm.h.

#include "platform/rpi/hub75_gpio_pwm.h"

#include <algorithm>
#include <thread>

namespace mm::platform {

namespace {

// Bounded waits on hardware bits: poll `tries` times, sleeping `us` microseconds between polls, so a wedged clock fails the open instead of hanging the process.
constexpr int kClockPollTries = 1000;
constexpr int kClockPollUs = 1;   // each sleep lasts at least the timer's slack, so the bound is tens of milliseconds

bool waitClockBit(volatile uint32_t* clk, uint32_t bit, bool set) {
    for (int i = 0; i < kClockPollTries; i++) {
        const bool isSet = (clk[kGpioClkPwmCtlOffset / 4] & bit) != 0;
        if (isSet == set) return true;
        std::this_thread::sleep_for(std::chrono::microseconds(kClockPollUs));
    }
    return false;
}

}  // namespace

GpioPwmTiming hub75GpioPwmTiming(uint16_t lsbNs, uint32_t plldHz) {
    // The LSB pulse is 2 ticks and a tick is divider / PLLD, so the divider is lsbNs x PLLD / 2e9, truncated, clamped to the 12-bit field; the LSB (2 ticks) reads back through the same rate, rounded to the nearest nanosecond.
    constexpr uint64_t kNsPerSecond = 1'000'000'000;
    uint64_t divider = static_cast<uint64_t>(lsbNs) * plldHz / (2 * kNsPerSecond);
    if (divider < 1) divider = 1;
    if (divider > 4095) divider = 4095;
    GpioPwmTiming t;
    t.divider = static_cast<uint16_t>(divider);
    t.lsbActualNs = static_cast<uint16_t>((2 * divider * kNsPerSecond + plldHz / 2) / plldHz);
    return t;
}

GpioPulseWords hub75GpioPulseWords(uint8_t plane) {
    const uint16_t ticks = hub75GpioPulseTicks(plane);
    GpioPulseWords w;
    if (ticks < 16) {
        // One FIFO word holding the full range, then the two sentinels.
        w.range = ticks;
        w.words[0] = ticks;
        w.words[1] = 0;
        w.words[2] = 0;
        w.count = 3;
    } else {
        // Eight words of range/8 (ticks is a power of two >= 16, so the split is exact), then the two sentinels.
        w.range = static_cast<uint16_t>(ticks / 8);
        for (int i = 0; i < 8; i++) w.words[i] = w.range;
        w.words[8] = 0;
        w.words[9] = 0;
        w.count = 10;
    }
    return w;
}

bool GpioPwmPulser::configure(volatile uint32_t* pwm, volatile uint32_t* clk,
                             uint16_t lsbNs, uint32_t plldHz, std::string& error) {
    if (pwm == nullptr || clk == nullptr) {
        error = "PWM pulser has no register mapping";
        return false;
    }
    plldHz_ = plldHz;
    timing_ = hub75GpioPwmTiming(lsbNs, plldHz);

    // Stop the PWM clock: KILL, then wait for BUSY to clear; every clock-manager write carries the password in the top byte.
    clk[kGpioClkPwmCtlOffset / 4] = kGpioClkPassword | kGpioClkCtlKill;
    if (!waitClockBit(clk, kGpioClkCtlBusy, false)) {
        error = "the PWM clock did not stop (BUSY stayed set)";
        return false;
    }
    // The source alone, then the divider (password, DIVI bits 12..23, DIVF 0), then enable with the source, as the reference design writes them.
    clk[kGpioClkPwmCtlOffset / 4] = kGpioClkPassword | kGpioClkSrcPlld;
    clk[kGpioClkPwmDivOffset / 4] =
        kGpioClkPassword | (static_cast<uint32_t>(timing_.divider) << 12);
    clk[kGpioClkPwmCtlOffset / 4] =
        kGpioClkPassword | kGpioClkSrcPlld | kGpioClkCtlEnab;
    if (!waitClockBit(clk, kGpioClkCtlBusy, true)) {
        error = "the PWM clock did not start (BUSY stayed clear)";
        return false;
    }

    pwm_ = pwm;
    clk_ = clk;

    // Channel setup, FIFO mode, inverted polarity, FIFO cleared; the channel is disabled while the control register is rewritten, so only a complete configuration reaches the pin.
    pwm_[kGpioPwmCtlOffset / 4] = 0;
    pwm_[kGpioPwmCtlOffset / 4] = kGpioPwmCtlClrf1;   // clear the FIFO
    pwm_[kGpioPwmCtlOffset / 4] =
        kGpioPwmCtlUsef1 | kGpioPwmCtlPwen1 | kGpioPwmCtlPola1;
    pulseEnd_ = std::chrono::steady_clock::now();
    return true;
}

void GpioPwmPulser::sendPulse(uint8_t plane) {
    if (pwm_ == nullptr) return;
    const GpioPulseWords w = hub75GpioPulseWords(plane);
    // Range first: the FIFO words are consumed one period at a time against the current range, and the FIFO is empty (the caller waited for the previous pulse), so the new range is in place before the first word is consumed.
    pwm_[kGpioPwmRng1Offset / 4] = w.range;
    for (int i = 0; i < w.count; i++)
        pwm_[kGpioPwmFif1Offset / 4] = w.words[i];
    // Re-enable the channel: the wait disabled it, so the pulse starts only now, after the latch, and the output is dark until this write; the channel is enabled per pulse and off between pulses.
    pwm_[kGpioPwmCtlOffset / 4] =
        kGpioPwmCtlUsef1 | kGpioPwmCtlPwen1 | kGpioPwmCtlPola1;
    // The pulse ends ticks x divider / PLLD from now; the wait sleeps until near it and then polls.
    const uint64_t ns = static_cast<uint64_t>(hub75GpioPulseTicks(plane)) * timing_.divider * 1'000'000'000 / plldHz_;
    pulseEnd_ = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);
}

bool GpioPwmPulser::waitPulseFinished() {
    if (pwm_ == nullptr) return true;
    // Sleep until near the pulse's computed end, so a long pulse (a large LSB time on a top plane) does not spin the refresh thread; short pulses are already over, and the sleep is skipped.
    const auto now = std::chrono::steady_clock::now();
    const auto sleepUntil = pulseEnd_ - std::chrono::microseconds(30);
    if (now < sleepUntil)
        std::this_thread::sleep_until(sleepUntil);
    // Then poll the FIFO-empty bit until it sets, with a 2 ms grace past the computed end; the FIFO drains only after the two zero sentinels, so an empty FIFO means the output is dark again.
    const auto deadline = pulseEnd_ + std::chrono::milliseconds(2);
    while ((pwm_[kGpioPwmStaOffset / 4] & kGpioPwmStaEmpt1) == 0) {
        if (std::chrono::steady_clock::now() > deadline) {
            // Wedged: force the output dark anyway by disabling the channel and clearing the FIFO, so the dump's address change and latch still happen in the dark and the next pulse starts from a clean FIFO.
            pwm_[kGpioPwmCtlOffset / 4] =
                kGpioPwmCtlUsef1 | kGpioPwmCtlPola1 | kGpioPwmCtlClrf1;
            return false;
        }
    }
    // Disable the channel and clear the FIFO: with the channel off the output sits high (dark, inverted polarity) whatever the FIFO held, so the address change and latch that follow happen in the dark, and the next sendPulse re-enables it.
    pwm_[kGpioPwmCtlOffset / 4] =
        kGpioPwmCtlUsef1 | kGpioPwmCtlPola1 | kGpioPwmCtlClrf1;
    return true;
}

void GpioPwmPulser::stop() {
    if (pwm_ == nullptr) return;
    pwm_[kGpioPwmCtlOffset / 4] = 0;
    pwm_[kGpioPwmCtlOffset / 4] = kGpioPwmCtlClrf1;
    pwm_ = nullptr;
    clk_ = nullptr;
}

}  // namespace mm::platform
