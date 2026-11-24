// SPDX-License-Identifier: GPL-3.0-or-later

/// The rules the GPIO engine's options obey (@xref{option-rules|option rules}), in plain C++ that builds on every host: the seam's recorder refuses the same options the engine does, whether or not a build has the engine.
///
/// @technical
///
/// ## Option rules
///
/// A check is a table of rows, each a predicate over the options and the reason it gives, with an optional detail that adds the figures; the first row that holds refuses. The five rows every Pi engine shares are `kHub75CommonRules` (hub75_pio_options.h), and this engine adds its own after them: the multiplexing, the LSB time, the slowdown, the driven geometry and the memory limit.
///
/// The memory limit is `kHub75MaxBufferBytes` (hub75_pio_options.h), the one both engines share.
///
/// ## Driven geometry
///
/// The encoder validates the DRIVEN lane, which is the logical one except under Stripe, where the panel is driven as half the rows and double the columns (hub75_gpio_multiplex.h). A 32-high panel at 1/32 scan fails here, and a 32-high Stripe panel drives 16 rows at 1/8.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "platform/Hub75Gpio.h"              // Hub75GpioOptions
#include "platform/rpi/hub75_gpio_multiplex.h"   // the driven panel under the options' multiplexing
#include "platform/rpi/hub75_pio_options.h"      // OptionRule, hub75OptionsPass, kHub75CommonRules: the rule table both engines use

namespace mm::platform {

/// One lane's driven width: the logical lane, doubled under Stripe (@xref{driven-geometry|driven geometry}); 64 bits, so a width past 16 bits reads as it is and the rule refuses it.
inline int64_t hub75GpioDrivenWidth(const Hub75GpioOptions& o) {
    return static_cast<int64_t>(hub75GpioMultiplexDrivenCols(o.multiplexing, o.cols)) * o.panelsPerLane;
}

/// One lane's driven height: the logical lane, halved under Stripe, the same pixel count in a reshaped panel.
inline int hub75GpioDrivenHeight(const Hub75GpioOptions& o) {
    return hub75GpioMultiplexDrivenRows(o.multiplexing, o.rows);
}

/// The bytes the wall's buffers total for `o`: the RGB and encoded triple buffers, the strips, remainders and slots, and the blank frame; the engine allocates this much and the ceiling rule reads it, for options that pass the geometry rules.
uint64_t hub75GpioBufferBytes(const Hub75GpioOptions& o);

/// Whether `o` is a wall the engine can drive (panel counts, scan rate, depth, LSB time, slowdown, per-lane geometry, memory), else false with the reason in `error`; open() and the test recorder both refuse on it.
bool hub75GpioCheckOptions(const Hub75GpioOptions& o, std::string& error);

}  // namespace mm::platform
