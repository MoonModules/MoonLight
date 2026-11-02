#pragma once

/// What each desktop platform supplies to the HDMI output loop in platform_desktop_hdmi.cpp: find the display, set it up in a mode, hand out the buffer the next frame is drawn into, and show it. The loop calls these from its own thread only.

#include "platform/edid.h"
#include "platform/platform.h"

#include <cstddef>
#include <cstdint>

namespace mm::platform::screen {

/// One frame to draw into, XRGB8888: rows of `pitch` bytes, each pixel a little-endian 0x00RRGGBB.
struct Surface {
    uint8_t* pixels = nullptr;   ///< the first row
    uint32_t pitch = 0;          ///< bytes from one row to the next
    uint16_t width = 0;          ///< pixels across
    uint16_t height = 0;         ///< rows
};

/// Take what the platform needs before any display is looked for; false where this machine has no output at all.
bool open();
/// Release it; called once a display is down.
void close();
/// The display to show on, 0 for none: `current` is the one up, 0 while none is, and is kept while still connected.
uint32_t find(uint32_t current);
/// The display's EDID reduced to `sizes`, where the platform gives it.
bool readEdid(uint32_t display, const edid::Size* sizes, size_t n, edid::Monitor& out);
/// Show on `display` in `want`, or its own mode where it has no such size, which `running` reports; false where the platform refuses the screen.
bool up(uint32_t display, const HdmiMode& want, HdmiMode& running);
/// The buffer the next frame is drawn into.
Surface back();
/// Show the buffer `back` handed out.
void present();
/// Release the display, giving it back its own mode.
void down();
/// Handle the platform's own events for the output, once each loop.
void pump();

}  // namespace mm::platform::screen
