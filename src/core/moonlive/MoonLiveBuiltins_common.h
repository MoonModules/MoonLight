#pragma once

#include "core/moonlive/MoonLive.h"   // runDefineControls drives the engine
#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/util/ThreadSlot.h"   // the per-thread table the control sink lives in
#include "core/util/math8.h"    // beatsin16: the shared time vocabulary
#include "core/util/math16.h"   // beat16 / sin16 / cos16: full-range waveforms
#include "core/util/noise.h"    // inoise8: the shared gradient-noise field

#include <atomic>
#include <cstdint>
#include <cstdio>

/// @defgroup moonlive_builtins_common MoonLive neutral builtins
/// @{
/// The domain-neutral half of the script vocabulary: arithmetic, waveforms, noise, randomness and print.
///
/// None of it is about light, so none of it belongs to the light domain.
///
/// @moreinfo
///
/// ## Why it left the light header
///
/// It lived there because that was the only vocabulary there was.
/// When the service table arrived it had to reach into the light header for `print` and `addControl`, which is core depending on a domain.
/// That is the wrong direction, and the reason a service script could not call `sin` while an effect could, for no reason either could explain.
///
/// ## What stays on the light side
///
/// Whatever genuinely needs a canvas: the pixel writes, the palette and particle helpers, the audio frame, and the per-light coordinates.

namespace mm::moonlive {

/// A script argument read as signed, which is what every builtin taking a coordinate wants.
inline int32_t signedArg(uintptr_t a) {
    return static_cast<int32_t>(uint32_t(a));
}

/// The remaining print budget, reset by a binding when it compiles.
inline std::atomic<uint32_t>& printBudget() { static std::atomic<uint32_t> n{0}; return n; }

/// Grant a fresh print burst, called from a binding's prepare alongside the compile.
inline void resetPrintBudget() { printBudget().store(32, std::memory_order_relaxed); }

extern "C" inline uint32_t mm_ml_random16(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t n = uint32_t(args[0]);
    // Atomic: two threads run scripts at once, and a lost update repeats a "random" value.
    static std::atomic<uint32_t> seed{0x2545F491u};
    uint32_t prev = seed.load(std::memory_order_relaxed), next;
    do {
        next = prev * 1664525u + 1013904223u;
    } while (!seed.compare_exchange_weak(prev, next, std::memory_order_relaxed));
    return n ? (next >> 16) % n : 0u;
}

/// The signed remainder, which is the wrap a cyclic animation needs; zero divisor answers 0.
extern "C" inline uint32_t mm_ml_mod(const uintptr_t* args, uint32_t, const uint8_t*) {
    const int32_t a = static_cast<int32_t>(uint32_t(args[0]));
    const int32_t b = static_cast<int32_t>(uint32_t(args[1]));
    // INT32_MIN % -1 traps on x86-64 where the other ISAs wrap, so a bench never shows it.
    if (b == 0 || (a == INT32_MIN && b == -1)) return 0;
    return static_cast<uint32_t>(a % b);
}

// A zero divisor saturates toward the numerator's sign, so a ripple's center reads as its peak.
/// The signed quotient, which the `/` operator lowers to.
extern "C" inline uint32_t mm_ml_div(const uintptr_t* args, uint32_t, const uint8_t*) {
    const int32_t a = static_cast<int32_t>(uint32_t(args[0]));
    const int32_t b = static_cast<int32_t>(uint32_t(args[1]));
    if (b == 0)
        return static_cast<uint32_t>(a > 0 ? INT32_MAX : a < 0 ? INT32_MIN : 0);
    // INT32_MIN / -1 is a SIGFPE on x86-64, and saturating reads better than "division broke".
    if (a == INT32_MIN && b == -1) return static_cast<uint32_t>(INT32_MAX);
    return static_cast<uint32_t>(a / b);
}

// The numerator widens in int64, since a register shift wraps past |128.0| and froze two shaders.
/// The Q16.16 quotient, which `/` lowers to when both sides are fixed.
extern "C" inline uint32_t mm_ml_fdiv(const uintptr_t* args, uint32_t, const uint8_t*) {
    const int32_t a = static_cast<int32_t>(uint32_t(args[0]));
    const int32_t b = static_cast<int32_t>(uint32_t(args[1]));
    if (b == 0)
        return static_cast<uint32_t>(a > 0 ? INT32_MAX : a < 0 ? INT32_MIN : 0);
    const int64_t q = (static_cast<int64_t>(a) << 16) / b;
    if (q > INT32_MAX) return static_cast<uint32_t>(INT32_MAX);
    if (q < INT32_MIN) return static_cast<uint32_t>(INT32_MIN);
    return static_cast<uint32_t>(static_cast<int32_t>(q));
}


/// A rising sawtooth at a given BPM, full scale, so scaling it sweeps any fixture size.
extern "C" inline uint32_t mm_ml_beat(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t bpm = uint32_t(args[0]), ms = uint32_t(args[1]);
    return beat16(static_cast<uint8_t>(bpm), ms);
}

extern "C" inline uint32_t mm_ml_beatsin(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t bpm = uint32_t(args[0]), ms = uint32_t(args[1]), high = uint32_t(args[2]);
    // A Call carries three arguments, so the low bound is 0 rather than a packed pair.
    return beatsin16(static_cast<uint8_t>(bpm), ms, 0, static_cast<uint16_t>(high));
}

/// The gradient-noise field at a point, whose high byte picks the cell and low byte interpolates.
extern "C" inline uint32_t mm_ml_noise(const uintptr_t* args, uint32_t, const uint8_t*) {
    return inoise8(uint32_t(args[0]), uint32_t(args[1]), uint32_t(args[2]));
}

extern "C" inline uint32_t mm_ml_sin(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t angle = uint32_t(args[0]);
    return static_cast<uint32_t>(sin16(static_cast<angle16>(angle)) + 32768);
}

extern "C" inline uint32_t mm_ml_cos(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t angle = uint32_t(args[0]);
    return static_cast<uint32_t>(cos16(static_cast<angle16>(angle)) + 32768);
}

// A builtin because a full turn is 65536, one past the largest number a script can write.
/// The angle step dividing one revolution into n parts.
extern "C" inline uint32_t mm_ml_turn(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t n = uint32_t(args[0]);
    return n ? 65536u / n : 0u;
}

extern "C" inline uint32_t mm_ml_scale(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t value = uint32_t(args[0]), n = uint32_t(args[1]);
    return (value * n) >> 16;
}

extern "C" inline uint32_t mm_ml_print(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t v = uint32_t(args[0]);
    // Claimed before printing, since `if (left > 0) --left` underflows when two threads race.
    auto& left = printBudget();
    uint32_t have = left.load(std::memory_order_relaxed);
    while (have > 0 && !left.compare_exchange_weak(have, have - 1, std::memory_order_relaxed)) {}
    if (have > 0) {
        std::printf("[script] %u\n", static_cast<unsigned>(v));
        if (have == 1) std::printf("[script] (burst spent; edit the script for a fresh one)\n");
    }
    return v;
}

// A builtin has no receiver, so the binding installs one for the run.
/// Where a running `defineControls()` sends each `addControl`.
using AddControlFn = void (*)(void* ctx, const char* name, uint8_t offset,
                              int32_t lo, int32_t hi, CtrlType type, const char* options);
/// The control sink one thread's run installed.
struct AddControlSink { AddControlFn fn = nullptr; void* ctx = nullptr; };

namespace detail {
/// One thread's control sink, in the slot it claims.
struct ControlSlot { std::atomic<uintptr_t> owner{0}; AddControlSink sink; };
// constinit rather than a function-local static, whose thread-safe guard is a lock.
inline constinit ControlSlot gControlSlots[2]{};
}  // namespace detail

/// The control sink for this thread, or an empty one; reading claims no slot.
inline const AddControlSink& addControlSink() {
    detail::ControlSlot* s = ownedThreadSlot(detail::gControlSlots, false);
    static constinit AddControlSink none{};
    return s ? s->sink : none;
}

// False when the table is full, which the caller must not treat as an installed sink.
/// Point addControl at a consumer for one defineControls run; nullptr to detach.
inline bool setAddControlSink(AddControlFn fn, void* ctx) {
    detail::ControlSlot* s = ownedThreadSlot(detail::gControlSlots, fn != nullptr);
    if (!s) return false;
    s->sink = {fn, ctx};
    if (!fn) releaseThreadSlot(s);
    return true;
}

// The compiler packs args[1]: the low byte is the arena offset, the next the type.
/// Surface a member as a control within a range; its declared type decides the kind.
extern "C" inline uint32_t mm_ml_addControl(const uintptr_t* args, uint32_t, const uint8_t*) {
    // The name points into the compiled program's string pool, which outlives the run.
    const char* name = reinterpret_cast<const char*>(args[0]);
    const CtrlType type = static_cast<CtrlType>((args[1] >> 8) & 0xff);
    const AddControlSink s = addControlSink();
    if (!name || !s.fn || !s.ctx) return 0;      // no binding listening: the call is a no-op
    // An arbitrary expression can exceed the member's type, and a wrapped slider top is invisible.
    const int32_t lo = int32_t(args[2]), hi = int32_t(args[3]);
    const int32_t limit = (type == CtrlType::Byte) ? 255 : (type == CtrlType::Bool) ? 1 : INT32_MAX;
    if (lo > limit || hi > limit) return 0;
    // A byte and a bool are unsigned, so a negative low bound became min 251 with max 100.
    if ((type == CtrlType::Byte || type == CtrlType::Bool) && lo < 0) return 0;
    // With min above max the write path rejects every value the slider could offer.
    if (lo > hi) return 0;
    s.fn(s.ctx, name, static_cast<uint8_t>(args[1] & 0xff), lo, hi, type, nullptr);
    return 0;
}

/// Surface a byte or int member as a dropdown of the names in "a|b|c", its value the index of the one picked.
extern "C" inline uint32_t mm_ml_addSelect(const uintptr_t* args, uint32_t, const uint8_t*) {
    const char* name = reinterpret_cast<const char*>(args[0]);
    const char* options = reinterpret_cast<const char*>(args[2]);
    const CtrlType type = static_cast<CtrlType>((args[1] >> 8) & 0xff);
    const AddControlSink s = addControlSink();
    if (!name || !options || !s.fn || !s.ctx) return 0;
    if (type != CtrlType::Byte && type != CtrlType::Int) return 0;   // a bool is a switch
    s.fn(s.ctx, name, static_cast<uint8_t>(args[1] & 0xff), 0, 0, type, options);
    return 0;
}

// Both vocabularies call this first and then add their own, so a name means one thing everywhere.
/// Register the neutral builtins into whatever table asks.
inline void addCommonBuiltins(BuiltinTable& t) {
    // mod/div: the operators a script cannot spell, since `%` and `/` are not in the grammar.
    t.add({"mod", 2, /*returns*/ true, BuiltinKind::Call, &mm_ml_mod, {}});
    t.add({"div", 2, /*returns*/ true, BuiltinKind::Call, &mm_ml_div, {}});
    // fdiv: the fixed-point divide, whose operands and result are Q16.16.
    t.add({"fdiv", 2, /*returns*/ true, BuiltinKind::Call, &mm_ml_fdiv, {},
           /*byRef*/ 0, /*byStr*/ 0, /*fixedArgs*/ 0x3, /*fixedReturn*/ true});
    // The time vocabulary: a beat, and a sine riding it.
    t.add({"beat", 2, /*returns*/ true, BuiltinKind::Call, &mm_ml_beat, {}});
    t.add({"beatsin", 3, /*returns*/ true, BuiltinKind::Call, &mm_ml_beatsin, {}});
    t.add({"noise", 3, /*returns*/ true, BuiltinKind::Call, &mm_ml_noise, {}});
    // The circle. One turn is 0..65535, so a loop over N points steps by turn(N).
    t.add({"sin", 1, /*returns*/ true, BuiltinKind::Call, &mm_ml_sin, {}});
    t.add({"cos", 1, /*returns*/ true, BuiltinKind::Call, &mm_ml_cos, {}});
    t.add({"turn", 1, /*returns*/ true, BuiltinKind::Call, &mm_ml_turn, {}});
    t.add({"scale", 2, /*returns*/ true, BuiltinKind::Call, &mm_ml_scale, {}});
    t.add({"random16", 1, /*returns*/ true, BuiltinKind::Call, &mm_ml_random16, {}});
    // print: the script author's only debugger.
    t.add({"print", 1, /*returns*/ true, BuiltinKind::Call, &mm_ml_print, {}});
    // addControl declares a setting; bit 1 of byRef passes the member's offset and type, not its value.
    t.add({"addControl", 4, /*returns*/ false, BuiltinKind::Call, &mm_ml_addControl, {},
           /*byRef*/ 0x2, /*byStr*/ 0x1});
    // addSelect declares a dropdown: a name, the member, and its option names in one quoted "a|b|c".
    t.add({"addSelect", 3, /*returns*/ false, BuiltinKind::Call, &mm_ml_addSelect, {},
           /*byRef*/ 0x2, /*byStr*/ 0x5});
}

// Run once after a successful compile, where a compiled module's defineControls() sits.
/// The entry a binding runs so the controls a script declares exist.
inline constexpr const char* kEntryDefineControls = "defineControls";

/// What a binding attaches around a defineControls run, for the sizing its own builtins ask for.
using DefineHook = void (*)(void* ctx, bool attach);

// Re-runnable like its compiled counterpart, since the declared list is cleared first.
/// Run a script's defineControls, so the controls it declares exist.
inline void runDefineControls(MoonLive& engine, DefineHook hook = nullptr, void* hookCtx = nullptr) {
    if (!engine.hasEntry(kEntryDefineControls)) return;   // nothing to clear and nothing to run
    // Install before clearing, since a clear-then-run with the table full would drop every control.
    if (!setAddControlSink([](void* ctx, const char* n, uint8_t off,
                              int32_t lo, int32_t hi, CtrlType type, const char* options) {
            static_cast<MoonLive*>(ctx)->addDeclaredControl(n, off, lo, hi, type, options);
        }, &engine)) return;
    if (hook) hook(hookCtx, true);
    engine.clearDeclaredControls();      // re-runnable: rebuild rather than append
    // This entry point writes no pixels, but `run` refuses a null or undersized buffer.
    uint8_t scratch[3] = {};
    engine.run(scratch, 1, 3, 0, kEntryDefineControls);
    if (hook) hook(hookCtx, false);
    setAddControlSink(nullptr, nullptr);
}

/// @}

}  // namespace mm::moonlive
