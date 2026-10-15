#pragma once

#include <cstddef>
#include <cstdint>

namespace mm_structural {

/// One decoded instruction, as the structural checker sees it.
///
/// @technical
///
/// This type is split from the checks themselves because each ISA's decoder must be declared before the checks that call it and defined after, and both halves need this.
///
/// A field is absent when its flag is false: an instruction naming no frame offset, which is most of them, simply reports none.
///
/// The allocated frame size is read from the emitted instruction rather than recomputed.
/// A checker modeling the frame with its own copy of the formula agrees with the backend even when the backend is wrong.
/// That is how the first version of this check passed against the bug it was written for.
///
/// The register masks serve the call-clobber check: a windowed call rotates the register file, so every register the rotation covers holds the callee's leftovers unless the caller saved it.
struct Decoded {
    uint8_t  len = 0;                 ///< zero means it could not decode, and the walk stops
    bool     hasFrameOff = false;     ///< whether it names a frame offset
    uint32_t frameOff = 0;            ///< that offset, in bytes from the frame pointer
    bool     hasTarget = false;       ///< whether it names a branch destination
    int32_t  target = 0;              ///< that destination, as an absolute byte offset
    bool     hasFrameAlloc = false;   ///< whether it allocates the frame, as a prologue does
    uint32_t frameAlloc = 0;          ///< how much it allocates
    bool     isCall = false;          ///< whether it is a call, which rotates the register file
    uint32_t readsMask = 0;           ///< the registers it reads, one bit each
    uint32_t writesMask = 0;          ///< the registers it writes
};

}  // namespace mm_structural
