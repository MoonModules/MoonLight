// SPDX-License-Identifier: GPL-3.0-or-later

/// The shared GPIO dump loop: the one place that turns a GPIO word frame into the sequence the panel sees, clock, latch, address, OE.
///
/// It is a template over a register sink, so the hardware backend and the stub backend run the same loop: the hardware sink writes the MMIO registers, the stub sink records the writes, and the unit test pins the sequence the hardware would drive, not a copy of it (@xref{register-sink|register sink}).
///
/// The frame is plane-major in memory but the panel is dumped row-major, one row's full PWM before the next row (@xref{row-major-dump|row-major dump}).
///
/// Per (row, plane) the loop clocks the group in, idles the data lines, waits for the previous OE pulse, sets the address, strobes and fires the next pulse (@xref{the-sequence|the sequence}), with slowdown delays after every GPIO operation (@xref{slowdown-delays|slowdown delays}).
///
/// @attribution rpi-rgb-led-matrix
///
/// @technical
///
/// ## Row-major dump
///
/// hub75Encode emits plane p's groups before plane p+1's, and the dump walks them row-major.
///
/// The reference design's comment gives the reason: switching rows quickly ghosts, so the full PWM of one row is done before switching rows. The dump sequence follows that design in concept: the row-major traversal, the dark address change and the pulse wait before the strobe.
///
/// On the bench-v4 sign a plane-major dump changes the address every ~13 us and the slow-decay outdoor 1/8-scan panels ghost each row's data onto a dozen neighbors (BENCH-V4).
///
/// The group for row r, plane p lives at frame index (p * scanRows + r), so the dump walks rows outer, planes inner, indexing into the plane-major frame.
///
/// The group is `passWords()` wide on the wire: the shift words plus the hold words the encoder leads a narrow row's group with. The dump clocks only the shift words: the hold words carry no color, and the pulser, not the encoder's OE bits, sets this path's lit time, so the hold words cost the dump only the stride.
///
/// ## The sequence
///
/// Per (row, plane):
///
/// 1. Clock the group's columns in while the previous OE pulse runs. Per column: clear the 0-data bits and the clock, set the 1-data bits, delay the slowdown (data setup), raise the clock (the rising edge clocks the data in) and delay the slowdown again (clock hold: the edge is over before the data moves on).
///
/// 2. Clear the color bits and the clock: the data lines idle dark from here, so a stray clock edge while the row changes shifts in dark data.
///
/// 3. Wait for the previous OE pulse to finish. The wait disables the PWM channel (hub75_gpio_pwm.h), so the panel is dark from here: the address change and the latch happen in the dark, whatever the FIFO held.
///
/// 4. Set the row address, skipped when it is unchanged (the planes of one row share it), and delay the slowdown so the address lines settle before the strobe.
///
/// 5. Strobe: raise the latch, delay the slowdown, drop it. The latched data is the row last clocked in, at the address last set.
///
/// 6. Fire the OE pulse for this plane. The pulse re-enables the PWM channel; the next group's clock-in runs while it is lit.
///
/// Falling-edge panels invert step 1's column sequence: the falling edge clocks the data in, so the data is set with the clock high and the clock is dropped last, then held.
///
/// ## Slowdown delays
///
/// The slowdown delays are dummy GPLEV0 reads on the hardware sink (a register read the hardware must serve, so the CPU cannot outrun the panel) and no-ops on the recording sink.
///
/// The reference design delays after every GPIO operation (the data write, the clock edge, the address write, the strobe), and so does this loop. A delay missing after the clock edge leaves the panel no hold time, and one missing after the address write leaves an address the latch may catch mid-change.
///
/// ## Register sink
///
/// A sink is a small struct of inline methods (no virtual calls on the hot path):
///
/// ```
/// void writeSet(uint32_t mask);   // GPSET0 = mask (or record it)
/// void writeClr(uint32_t mask);   // GPCLR0 = mask (or record it)
/// uint32_t readLev();             // GPLEV0, for the slowdown delays (or 0)
/// bool waitPulseFinished();       // the pulser wait (false on timeout)
/// void sendPulse(uint8_t plane);  // the pulser fire for bitplane p
/// ```
///
/// The hardware sink drives the mapped registers; the stub sink records every call into the dump stream the test reads back.
///
/// ## Short frames and timeouts
///
/// A frame shorter than the config expects is dumped group by group: groups past the end are skipped rather than clocked. The engine always submits full frames, so this is a defensive path only.
///
/// `pulseTimeouts` is a running counter the caller owns: the call adds one per OE wait that timed out, on top of the count it is handed, and the dump proceeds anyway.

#pragma once

#include <cstddef>
#include <cstdint>

#include "platform/rpi/hub75_gpio_backend.h"   // GpioSubmitConfig
#include "platform/rpi/hub75_gpio_pins.h"

namespace mm::platform {

/// One recorded dump event for the stub backend's stream (see hub75_gpio_backend_stub.h): a GPIO write the loop made, or a pulser step.
struct GpioDumpEvent {
    enum class Kind : uint8_t { Set, Clr, PulseWait, PulseFire };
    /// Which GPIO write or pulser step this event records.
    Kind kind = Kind::Set;
    /// The GPSET0/GPCLR0 mask for a write, the bitplane for a pulse fire, unused for a pulse wait.
    uint32_t value = 0;
};

/// The dump's frame-wide constants and its one piece of state, so each step of a group takes the run as its one shared argument.
struct GpioDumpRun {
    const GpioSubmitConfig& config;   ///< the frame's grouping and clocking
    const uint32_t* words;            ///< the GPIO words, plane-major
    size_t shiftWords;                ///< shift words per group
    size_t groupWords;                ///< words per group on the wire: the shift words plus the hold words
    size_t total;                     ///< words the frame holds, capped at what the config expects
    int delay;                        ///< dummy reads per slowdown delay
    int lastRow;                      ///< the address the lines hold, -1 while this frame has written none
    uint64_t& pulseTimeouts;          ///< the caller's running count of OE waits that timed out
};

/// The slowdown delay: `delay` dummy reads that the hardware must serve and the recorder skips (@xref{slowdown-delays|slowdown delays}).
template <typename Sink>
[[gnu::always_inline]] inline void hub75GpioDelay(Sink& sink, int delay) {
    for (int s = 0; s < delay; s++) (void)sink.readLev();
}

/// Clocks `n` columns in on the rising edge: three GPIO writes per column, a delay after the data write (setup) and after the clock edge (hold).
template <typename Sink>
[[gnu::always_inline]] inline void hub75GpioShiftRising(Sink& sink, const uint32_t* words, size_t n, int delay) {
    for (size_t i = 0; i < n; i++) {
        const uint32_t data = words[i] & kGpioColorMask;
        sink.writeClr((~data & kGpioColorMask) | kGpioClkBit);
        sink.writeSet(data);
        hub75GpioDelay(sink, delay);
        sink.writeSet(kGpioClkBit);   // rising edge clocks the data in
        hub75GpioDelay(sink, delay);
    }
}

/// Clocks `n` columns in on the falling edge: the data is set with the clock high and the clock is dropped last, then held.
template <typename Sink>
[[gnu::always_inline]] inline void hub75GpioShiftFalling(Sink& sink, const uint32_t* words, size_t n, int delay) {
    for (size_t i = 0; i < n; i++) {
        const uint32_t data = words[i] & kGpioColorMask;
        sink.writeSet(data | kGpioClkBit);
        sink.writeClr(~data & kGpioColorMask);
        hub75GpioDelay(sink, delay);
        sink.writeClr(kGpioClkBit);   // falling edge clocks the data in
        hub75GpioDelay(sink, delay);
    }
}

/// Steps 2 to 5 of the sequence, in the dark: idles the data lines, waits out the previous OE pulse, sets the row address when it changed and strobes (@xref{the-sequence|the sequence}).
template <typename Sink>
[[gnu::always_inline]] inline void hub75GpioLatchRow(Sink& sink, GpioDumpRun& run, int row) {
    // 2. The data lines idle dark while the row changes.
    sink.writeClr(kGpioColorMask | kGpioClkBit);
    // 3. The previous OE pulse finishes and the PWM channel disables, so the panel is dark before the address moves or the latch fires.
    if (!sink.waitPulseFinished()) run.pulseTimeouts++;
    // 4. The row address, when it changed; then let it settle.
    if (row != run.lastRow) {
        const uint32_t addr = gpioAddressMaskForRow(static_cast<uint8_t>(row));
        sink.writeClr(kGpioAddressMask & ~addr);
        sink.writeSet(addr);
        run.lastRow = row;
    }
    hub75GpioDelay(sink, run.delay);
    // 5. Strobe the row last clocked in, at the address last set.
    sink.writeSet(kGpioStbBit);
    hub75GpioDelay(sink, run.delay);
    sink.writeClr(kGpioStbBit);
}

/// Dumps the group of `plane` at `row`: clocks its columns in, latches the row and fires the plane's OE pulse; a group past the end of a short frame is skipped.
template <typename Sink>
[[gnu::always_inline]] inline void hub75GpioDumpGroup(Sink& sink, GpioDumpRun& run, int row, int plane) {
    const GpioSubmitConfig& config = run.config;
    // The frame is plane-major: group (plane, row) sits at plane * scanRows + row.
    const size_t g = static_cast<size_t>(plane) * config.scanRows + static_cast<size_t>(row);
    const size_t base = g * run.groupWords;
    if (base >= run.total) return;   // a short frame: skip the group
    // The hold words lead the group; the shift words follow them. A short frame may end inside either.
    const size_t avail = run.total - base;
    const size_t skip = config.holdWords < avail ? config.holdWords : avail;
    const size_t n = run.shiftWords < avail - skip ? run.shiftWords : avail - skip;
    // 1. Clock this group's columns in while the previous OE pulse runs.
    if (config.clockFallingEdge)
        hub75GpioShiftFalling(sink, run.words + base + skip, n, run.delay);
    else
        hub75GpioShiftRising(sink, run.words + base + skip, n, run.delay);
    hub75GpioLatchRow(sink, run, row);
    // 6. Fire the OE pulse for this plane; the next group's clock-in runs while it is lit.
    sink.sendPulse(static_cast<uint8_t>(plane));
}

/// Dumps one frame of `wordCount` GPIO words (one per pixel clock, color bits only) under `config` with `slowdown` dummy reads per delay, and returns 0, or -1 when `words` is null (@xref{short-frames-and-timeouts|short frames and timeouts}).
template <typename Sink>
int hub75GpioDumpFrame(Sink& sink, const GpioSubmitConfig& config,
                       const uint32_t* words, size_t wordCount, int slowdown,
                       uint64_t& pulseTimeouts) {
    if (words == nullptr) return -1;
    // The group stride on the wire: the shift words plus the hold words the encoder leads a narrow row's group with (Hub75Geometry).
    const size_t shiftWords = static_cast<size_t>(config.laneWidth) * config.pairs;
    const size_t groupWords = shiftWords + config.holdWords;
    const size_t expect = static_cast<size_t>(config.bitDepth) * config.scanRows * groupWords;
    // The address the lines hold, so a row's planes do not rewrite it: -1 marks it unwritten this frame, so the first row always writes it, whatever the previous frame left behind.
    GpioDumpRun run{config, words, shiftWords, groupWords, wordCount < expect ? wordCount : expect,
                    slowdown < 0 ? 0 : slowdown, -1, pulseTimeouts};
    for (int row = 0; row < config.scanRows; row++)
        for (int plane = 0; plane < config.bitDepth; plane++)
            hub75GpioDumpGroup(sink, run, row, plane);
    return 0;
}

}  // namespace mm::platform
