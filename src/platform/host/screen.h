#pragma once

/// What each host platform supplies to the HDMI output loop in platform_host_hdmi.cpp: find the display, set it up in a mode, hand out the buffer the next frame is drawn into, and show it. The loop calls these from its own thread only.

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

/// One connected display as a desktop lists it.
struct Display {
    uint32_t id;   ///< the platform's id for it, above 0
    bool own;      ///< the display the host runs its desktop on
};

/// The display to show on among `list`, 0 for none: a display beside the host's own first, the one shown (`current`) kept among those, and the host's own only where there is no other and `own` asks for it.
uint32_t pick(const Display* list, size_t n, uint32_t current, bool own);

/// Take what the platform needs before any display is looked for; false where this machine has no output at all.
bool open();
/// Release it; called once a display is down.
void close();
/// The display to show on, 0 for none: `current` is the one up, 0 while none is, and is kept while still connected. With `own`, a desktop with no display beside its own gives that one.
uint32_t find(uint32_t current, bool own);
/// The display's EDID reduced to `sizes`, where the platform gives it.
bool readEdid(uint32_t display, const edid::Size* sizes, size_t n, edid::Monitor& out);
/// Show on `display` in `want`, or its own mode where it has no such size, which `running` reports; false where the platform refuses the screen.
bool up(uint32_t display, const HdmiMode& want, HdmiMode& running);
/// The buffer the next frame is drawn into.
Surface back();
/// Show the buffer `back` handed out.
void present();
/// Show a `w` × `h` RGB frame scaled by the display itself onto the screen's rectangle at (`x0`, `y0`), `outW` × `outH`, the bars around it black; false where the display cannot, so the loop scales it into `back` instead.
bool presentScaled(const uint8_t* rgb, uint16_t w, uint16_t h, uint16_t x0, uint16_t y0, uint16_t outW, uint16_t outH);
/// Release the display, giving it back its own mode.
void down();
/// Handle the platform's own events for the output, once each loop.
void pump();
/// Whether Esc was pressed on the output while it covered the host's own display, once per press.
bool takeDismiss();

}  // namespace mm::platform::screen
