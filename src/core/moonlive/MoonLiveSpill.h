#pragma once

#include <cstdint>

#include "core/moonlive/MoonLiveIr.h"
#include "core/moonlive/moonlive_emit.h"   // RegBudget, the one thing a backend tells the allocator

/// Linear scan with spilling, after Poletto and Sarkar.
///
/// A program naming more live values than the target has registers is rewritten to park the overflow in the call frame.
/// So a script's complexity is a memory question rather than a register-count one.
///
/// @technical
///
/// ## Why it lives in core, once
///
/// Spilling across a loop back edge is the hardest logic here, and only the host backend runs under test.
/// Four copies would leave three permanently untested.
/// Each backend supplies a register budget and consumes two IR ops, so the algorithm appears nowhere in the platform layer.

namespace mm::moonlive {

/// Rewrite `ir` so no op names a register the target lacks, parking the overflow in frame slots. A program that already fits is left byte-identical, and `slotsUsed` counts locals either way.
bool spillToBudget(IrProgram& ir, const RegBudget& budget, uint8_t& slotsUsed);

}  // namespace mm::moonlive
