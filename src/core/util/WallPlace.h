#pragma once

#include "platform/nonblocking.h"

#include <cstdint>

namespace mm {

/// A device's box in a wall of several devices, in lights: its corner and its size, unplaced at zero size.
///
/// @technical
///
/// The wall a group makes is the bounding box of its members' boxes, the axis-aligned union every tiled display takes as its desktop, here in three axes so devices can stack in depth as well as tile across.
struct WallPlace {
    int16_t  x = 0, y = 0, z = 0;   ///< the corner, in the wall's coordinates
    uint16_t w = 0, h = 0, d = 0;   ///< the size
    /// Whether this names a box at all.
    bool placed() const MM_NONBLOCKING { return w && h && d; }
    /// The smallest box holding this one and `o`, or the placed one of the two.
    WallPlace united(const WallPlace& o) const MM_NONBLOCKING {
        if (!o.placed()) return *this;
        if (!placed()) return o;
        WallPlace u;
        span(x, w, o.x, o.w, u.x, u.w);
        span(y, h, o.y, o.h, u.y, u.h);
        span(z, d, o.z, o.d, u.z, u.d);
        return u;
    }
    /// Whether both name the same box.
    bool operator==(const WallPlace& o) const MM_NONBLOCKING {
        return x == o.x && y == o.y && z == o.z && w == o.w && h == o.h && d == o.d;
    }

private:
    /// The interval holding [a, a + n) and [b, b + m) along one axis, as a start and a length.
    static void span(int16_t a, uint16_t n, int16_t b, uint16_t m, int16_t& start, uint16_t& len) MM_NONBLOCKING {
        const int32_t lo = a < b ? a : b;
        const int32_t hi = a + n > b + m ? a + n : b + m;
        start = static_cast<int16_t>(lo);
        len = static_cast<uint16_t>(hi - lo);
    }
};

}  // namespace mm
