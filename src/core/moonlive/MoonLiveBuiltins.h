#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>   // the builtin-table overflow diagnostic

/// @defgroup MoonLiveBuiltins Registering the functions a script may call
/// @{
/// The neutral seam a host fills with its own vocabulary.
///
/// @moreinfo
///
/// The core compiler knows only that a name maps to a descriptor, owning no function names and no domain semantics.
///
/// ## What a descriptor decides
///
/// A descriptor says how a call lowers.
/// A `Call` is a pure host helper, lowered to a generic call.
/// An `Inline` is a routine the backend emits without per-call overhead, carrying a neutral opcode tag the per-ISA lowering knows, which the core threads through without interpreting.

namespace mm::moonlive {

enum class CtrlType : uint8_t { Int, Byte, Bool, Fixed, Str };

// Where packing pays: a byte array costs a quarter of an int one, with no PSRAM to absorb it.
/// Bytes one array element occupies.
constexpr uint8_t ctrlWidth(CtrlType t) {
    return (t == CtrlType::Byte || t == CtrlType::Bool) ? 1 : 4;
}

/// Bytes a scalar occupies, which is always four whatever its type.
constexpr uint8_t ctrlSlotBytes(CtrlType) { return 4; }



// Neutral inline opcodes the core treats as opaque tags, rather than LED operations.
enum class InlineOp : uint8_t {
    StoreElem,   // operands: indexVReg, v0, v1, v2  → store three values at `index`
    StoreFirst,  // operands: v0, v1, v2             → store three values at element 0
    FillElems,   // operands: v0, v1, v2             → loop store over every element
};

enum class BuiltinKind : uint8_t { Call, Inline };

// Arguments arrive as a pointer to consecutive frame slots, so the frame bounds their count.
using HostCallFn = uint32_t (*)(const uintptr_t* args, uint32_t argc, const uint8_t* arena);

struct Builtin {
    /// The script-visible name, owned by the host.
    const char*  name = nullptr;
    /// How many arguments the builtin takes.
    uint8_t      argc = 0;
    /// Whether the call produces a value rather than acting as a statement.
    bool         returns = false;
    /// Whether this lowers to a host call or an inline op.
    BuiltinKind  kind = BuiltinKind::Call;
    /// The host function a Call targets.
    HostCallFn   fn = nullptr;
    /// The neutral opcode tag an Inline op carries.
    InlineOp     inlineOp{};
    // A bitmask rather than a per-argument enum, since the only question is which of the two.
    /// Which arguments are passed by reference, a bit per position.
    uint8_t      byRef = 0;
    // Without it a bare identifier in a name slot compiles, handing the host a pointer from a value.
    /// Which arguments must be a string literal, a bit per position.
    uint8_t      byStr = 0;
    // The parser type-checks against this rather than matching on a name.
    /// Which arguments are fixed-point rather than whole numbers, a bit per position.
    uint8_t      fixedArgs = 0;
    /// Whether the result is fixed-point.
    bool         fixedReturn = false;
};

// An overflow otherwise surfaces as "unknown function" for a builtin that plainly exists.
/// Assert a host's builtin table did not silently drop a registration.
#define MM_ASSERT_NO_BUILTIN_OVERFLOW(t)                                              \
    do {                                                                              \
        if ((t).full()) {                                                             \
            std::printf("MoonLive: builtin table FULL at %u entries, a registration " \
                        "was dropped. Raise BuiltinTable::kMax.\n",                    \
                        static_cast<unsigned>((t).registered()));                     \
        }                                                                             \
    } while (0)

// A fixed-capacity table the host fills and the compiler reads, looked up by name.
struct BuiltinTable {
    // Headroom matters because a full table fails silently, and 64 was reached at 61.
    /// How many builtins one table holds.
    static constexpr uint8_t kMax = 96;
    /// The registered builtins.
    Builtin items[kMax];
    /// How many builtins are registered.
    uint8_t count = 0;
    /// Set when an `add` was dropped for lack of room.
    bool overflowed = false;

    /// Register one builtin, returning false when the table is full.
    bool add(const Builtin& b) {
        if (b.name == nullptr) return false;   // a null name would fault in the lookup
        if (count >= kMax) { overflowed = true; return false; }
        items[count++] = b;
        return true;
    }

    /// True when a registration was dropped for lack of room.
    bool full() const { return overflowed; }

    /// Every registered name, for the overflow diagnostic. Not used on any hot path.
    uint8_t registered() const { return count; }
    /// The builtin registered under `name`, or null when there is none.
    const Builtin* find(const char* name, size_t len) const {
        for (uint8_t i = 0; i < count; i++) {
            const char* n = items[i].name;
            size_t j = 0;
            for (; j < len && n[j]; j++) if (n[j] != name[j]) break;
            if (j == len && n[j] == 0) return &items[i];
        }
        return nullptr;
    }
};

// The byte budget is what the arena allocates, where the count is what the record tables hold.
/// Bytes the script's own members may occupy.
static constexpr uint8_t kCtrlBytes = 64;        // arena bytes the script's members share
static constexpr uint8_t kMaxCtrls  = 16;        // records: how many members/controls may exist

// The sanity bound, not the working limit: a compile sizes its staging from the token count.
static constexpr size_t  kCodeCap = 16384;

// 48 bytes per token, a 1.7x margin over the densest measured.
/// Bytes to reserve for a script of `tokens` tokens.
constexpr size_t codeCapFor(uint32_t tokens) {
    const size_t want = size_t(tokens) * 48 + 256;
    return want > kCodeCap ? kCodeCap : want;
}

static constexpr uint8_t kMaxSysVars  = 8;

// Four rather than one, since a width past 255 clamped and drew into a corner.
/// Bytes per system variable.
static constexpr uint8_t kSysVarBytes = 4;

// In the arena because the emitted block reads it, with no C++ frame between activations.
/// Where the emitted code keeps its recursion depth.
static constexpr uint8_t kDepthSlot = kCtrlBytes + kMaxSysVars * kSysVarBytes;

// Measured: a device resets at roughly 64 activations, so the guard refuses at half that.
/// The depth at which a call is refused, so 31 activations execute and the 32nd returns.
static constexpr uint8_t kMaxCallDepth = 32;

// The arena rather than the frame, since a callee copies its arguments out as its first act.
/// Where a script call's arguments are passed.
static constexpr uint8_t kMaxScriptArgs  = 4;     // per call; a helper wanting more wants an array
static constexpr uint8_t kScriptArgBytes = 4;     // one machine word, as a frame slot is
/// Where the argument block starts, aligned to four bytes because these are 32-bit slots.
static constexpr uint8_t kScriptArgBase  = static_cast<uint8_t>((kDepthSlot + 1 + 3) & ~3);

/// The arena byte offset of argument `i`.
constexpr uint8_t scriptArgOffset(uint8_t i) {
    return static_cast<uint8_t>(kScriptArgBase + i * kScriptArgBytes);
}

// Sized from the block's end rather than a sum, which the alignment padding would underestimate.
/// How many bytes the whole arena holds.
static constexpr uint8_t kArenaBytes  = kScriptArgBase + kMaxScriptArgs * kScriptArgBytes;

// Reserved, so the name means one thing in every script, and the binding writes the slot.
/// A name the host defines and the script only reads.
enum class SysVarKind : uint8_t {
    Arena,   // a byte in the controls arena the binding writes per frame (width/height/depth)
    Arg,     // an argument register the host passes on every run (t): costs no instruction
};

struct SysVar {
    /// The script-visible name, owned by the host.
    const char* name = nullptr;
    /// Whether the value lives in the arena or in an argument register.
    SysVarKind  kind = SysVarKind::Arena;
    /// The arena byte offset, or the argument register, by kind.
    uint8_t     where = 0;
};

/// The system variables one host domain defines, which the compiler resolves names against.
struct SysVarTable {
    // Bounded by the arena's system range, so no registration hands out a rejected offset.
    /// How many system variables one table holds.
    static constexpr uint8_t kMax = kMaxSysVars;
    /// The registered system variables.
    SysVar  items[kMax];
    /// How many system variables are registered.
    uint8_t count = 0;

    // Rejects an offset the arena cannot hold, whose per-frame write would otherwise vanish.
    /// Register one system variable, returning false when it cannot be held.
    bool add(const SysVar& v) {
        if (count >= kMax || v.name == nullptr) return false;
        // Below kDepthSlot, since a 4-byte load at that offset would run past the arena's end.
        if (v.kind == SysVarKind::Arena &&
            (v.where < kCtrlBytes || v.where >= kDepthSlot ||
             (v.where - kCtrlBytes) % kSysVarBytes != 0))
            return false;
        // kArg4 is the last argument register, spelled here because that header includes this one.
        if (v.kind == SysVarKind::Arg && v.where > 4) return false;
        items[count++] = v;
        return true;
    }
    /// The system variable registered under `name`, or null when there is none.
    const SysVar* find(const char* name, size_t len) const {
        for (uint8_t i = 0; i < count; i++) {
            const char* n = items[i].name;
            size_t j = 0;
            for (; j < len && n[j]; j++) if (n[j] != name[j]) break;
            if (j == len && n[j] == 0) return &items[i];
        }
        return nullptr;
    }
};

/// @}
}  // namespace mm::moonlive
