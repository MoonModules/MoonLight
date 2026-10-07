#pragma once

#include <cstdio>

#include "core/moonlive/MoonLiveBuiltins.h"
#include "core/moonlive/MoonLiveBuiltins_common.h"   // the neutral half: math, waveforms, noise, print
#include "core/moonlive/MoonLive.h"
#include "core/moonlive/MoonLiveIr.h"   // kArg3: the register `t` is passed in

#include <atomic>
#include <cstdint>
#include <limits>   // numeric_limits<lengthType>: the clamp in mm_light_circle

#include "core/util/math8.h"    // beatsin16: the shared time vocabulary
#include "core/util/math16.h"   // beat16 / triwave16: full-range waveforms
#include "light/powerfunctions/shader.h"  // shader::smoothstep, the GLSL vocabulary, already in fixed point
#include "core/util/noise.h"    // inoise8: the shared gradient-noise field
#include "core/util/ThreadSlot.h"   // the per-thread table the sinks live in
#include <cstring>
#include "core/services/AudioService.h"   // the audio vocabulary reads the latest frame
#include "light/powerfunctions/draw.h"    // draw::line, the shared 3D Bresenham a script draws with
#include "light/powerfunctions/particles.h" // particles::Pool, the kernel a scripted particle effect drives

/// @defgroup moonlive_builtins_light MoonLive light builtins
/// @{
/// The only place the LED vocabulary lives: the function names, their argument counts, and each inline opcode's meaning.
///
/// The core compiler sees only the neutral table and the tags this file hands it, so a different host writes its own registration and leaves core unchanged.
///
/// @moreinfo
///
/// ## Stroke width
///
/// A script's numbers arrive as unsigned ABI words and are read back signed, then narrowed to the drawing layer's `lengthType`.
/// The guard and the narrowing have to read the SAME number, or they disagree above 32767.
/// A width of 40000 passes `> 0` as an int32 and arrives at `ring` as -25536; 65536 arrives as 0, and 65537 as the thinnest line.
/// So the clamp happens before the cast, and an absurd width behaves as the widest a length can hold.

namespace mm::moonlive {

// random16(n) gives a value in [0, n), with the same implementation on every target.

// Saturating like a hardware channel, since truncation turns `n * 255` into an arbitrary walk.
/// Read one argument as a byte, clamped to 0..255.
inline uint8_t byteArg(uintptr_t a) {
    const int32_t v = signedArg(a);   // one home for the signed reinterpretation of the ABI word
    return v < 0 ? 0 : (v > 255 ? 255 : static_cast<uint8_t>(v));
}

// The active palette, with no hsv() beside it: a hue wheel ignores the user's choice.
extern "C" inline uint32_t mm_light_paletteR(const uintptr_t* args, uint32_t, const uint8_t*) {
    return colorFromPalette(*Palettes::active(), byteArg(args[0]), byteArg(args[1])).r;
}
extern "C" inline uint32_t mm_light_paletteG(const uintptr_t* args, uint32_t, const uint8_t*) {
    return colorFromPalette(*Palettes::active(), byteArg(args[0]), byteArg(args[1])).g;
}
extern "C" inline uint32_t mm_light_paletteB(const uintptr_t* args, uint32_t, const uint8_t*) {
    return colorFromPalette(*Palettes::active(), byteArg(args[0]), byteArg(args[1])).b;
}






// Signed and re-centered here, or the natural `w - d` arrives huge and inverts the shape.
extern "C" inline uint32_t mm_light_smoothstep(const uintptr_t* args, uint32_t, const uint8_t*) {
    return shader::smoothstep(signedArg(args[0]), signedArg(args[1]), signedArg(args[2]));
}

// 64-bit and unsigned, since `65535 * 65535` read signed clamps a right-edge pixel to the left.
extern "C" inline uint32_t mm_light_uvAxis(const uintptr_t* args, bool wantY) {
    const int64_t px = static_cast<int64_t>(uint32_t(args[0]));
    const int64_t w  = static_cast<int64_t>(uint32_t(args[1]));
    const int64_t h  = static_cast<int64_t>(uint32_t(args[2]));
    const int64_t sw = w < 1 ? 1 : w, sh = h < 1 ? 1 : h;
    const int64_t s  = sw < sh ? sw : sh;          // normalize on the SHORT side: that is what
                                                   // keeps a circle circular on a wide panel
    const int64_t extent = wantY ? sh : sw;
    // Applied before the divide rather than as a shift after it, for one rounding step.
    const int64_t v = ((px * 2 - extent + 1) * 65536) / s;
    // Four units, past the one the short side normalizes to, which covers any panel built.
    const int64_t c = v < -262144 ? -262144 : (v > 262144 ? 262144 : v);
    // Signed with no bias: a coordinate has an origin where a wave does not.
    return static_cast<uint32_t>(static_cast<int32_t>(c));
}
extern "C" inline uint32_t mm_light_uvX(const uintptr_t* args, uint32_t, const uint8_t*) {
    return mm_light_uvAxis(args, false);
}
extern "C" inline uint32_t mm_light_uvY(const uintptr_t* args, uint32_t, const uint8_t*) {
    return mm_light_uvAxis(args, true);
}

// The one loop a script cannot spell: it squares signed values, and int64 stops holes in the set.
extern "C" inline uint32_t mm_light_escape(const uintptr_t* args, uint32_t, const uint8_t*) {
    // Clamped to |8.0|, without which a full int32 makes the escape test's sum overflow.
    const auto qfx = [](uintptr_t a) {
        const int32_t v = signedArg(a);
        return v < -524288 ? -524288 : (v > 524288 ? 524288 : v);
    };
    const int32_t cx = qfx(args[0]), cy = qfx(args[1]);
    const int32_t jx = qfx(args[2]), jy = qfx(args[3]);
    uint32_t iters = uint32_t(args[4]);
    if (iters > 64) iters = 64;
    if (iters == 0) return 0;

    // One loop serves both, since a zero seed selects Mandelbrot.
    const bool julia = (jx != 0 || jy != 0);
    int64_t zx = julia ? cx : 0, zy = julia ? cy : 0;
    const int64_t ax = julia ? jx : cx, ay = julia ? jy : cy;

    constexpr int kShift = 16;
    constexpr int64_t kEscape = int64_t(4) << (kShift * 2);   // |z|^2 > 4.0, in Q32

    uint32_t n = 0;
    for (; n < iters; ++n) {
        const int64_t xx = zx * zx, yy = zy * zy;
        if (xx + yy > kEscape) break;
        const int64_t nx = ((xx - yy) >> kShift) + ax;
        zy = ((2 * zx * zy) >> kShift) + ay;
        zx = nx;
    }
    // Inside the set returns 0, and outside spreads over the full byte whatever `iters` is.
    return (n >= iters) ? 0u : (n * 255u) / iters;
}




// The polar builtins re-center their offsets, so a script need not reason about the wrap.
extern "C" inline uint32_t mm_light_polarA(const uintptr_t* args, uint32_t, const uint8_t*) {
    return static_cast<uint32_t>(atan16(signedArg(args[1]), signedArg(args[0])));
}
extern "C" inline uint32_t mm_light_polarR(const uintptr_t* args, uint32_t, const uint8_t*) {
    return dist16(signedArg(args[0]), signedArg(args[1]));
}



// `octaves` is the cost knob, since each one is another noise sample per pixel.
extern "C" inline uint32_t mm_light_fbm(const uintptr_t* args, uint32_t, const uint8_t*) {
    return fbm8(uint32_t(args[0]), uint32_t(args[1]), static_cast<uint8_t>(uint32_t(args[2])));
}

// Samples the field where the field displaced it, at three noise samples per call.
extern "C" inline uint32_t mm_light_warp(const uintptr_t* args, uint32_t, const uint8_t*) {
    return warp8(uint32_t(args[0]), uint32_t(args[1]), static_cast<uint16_t>(uint32_t(args[2])), 1);
}

// The volumetric forms, each its 2D form when z is 0, so depth passes unconditionally.
extern "C" inline uint32_t mm_light_fbm3(const uintptr_t* args, uint32_t, const uint8_t*) {
    return fbm8(uint32_t(args[0]), uint32_t(args[1]), uint32_t(args[2]),
                static_cast<uint8_t>(uint32_t(args[3])));
}
extern "C" inline uint32_t mm_light_warp3(const uintptr_t* args, uint32_t, const uint8_t*) {
    return warp8(uint32_t(args[0]), uint32_t(args[1]), uint32_t(args[2]),
                 static_cast<uint16_t>(uint32_t(args[3])), 1);
}

// A pure function of time, so two calls at one rate stay locked for as long as the device runs.
extern "C" inline uint32_t mm_light_osc(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t rate = uint32_t(args[0]), ms = uint32_t(args[1]), shape = uint32_t(args[2]);
    // No special case for rate 0, since each shape's own value at phase 0 is the right answer.
    const uint32_t phase = static_cast<uint32_t>((static_cast<uint64_t>(ms) * rate * 65536u) / 60000u);
    const angle16 a = static_cast<angle16>(phase);
    switch (shape) {
        case 1:  return triwave16(a);
        case 2:  return a;
        case 3:  return a < 32768 ? 0u : 65535u;
        default: return static_cast<uint32_t>(sin16(a) + 32768);
    }
}


// Rate-limited because the call sites are per-light: 16,384 prints would stall the render.


// One call per light, storing nothing: staging a 16k fixture needs 48 KB the classic lacks.
using AddLightFn = void (*)(void* ctx, uint16_t x, uint16_t y, uint16_t z);

// Per-thread and keyed on the task handle, since `thread_local` dies on an ESP32 task without TLS.
/// The addLight sink for one thread, installed by the binding around each run.
struct AddLightSink { AddLightFn fn = nullptr; void* ctx = nullptr; };

// Forwarded to the layer, which applies one amount per frame, so the longest trail wins.
/// Where fade(amt) sends its request.
using FadeFn = void (*)(void* ctx, uint8_t amt);
struct FadeSink { FadeFn fn = nullptr; void* ctx = nullptr; };

/// Where setXYZ sends the coordinate a modifier folded.
using CoordFn = void (*)(void* ctx, uint32_t x, uint32_t y, uint32_t z);
struct CoordSink { CoordFn fn = nullptr; void* ctx = nullptr; };

/// Where setPalEntry writes, carrying an index rather than a light.
using PalFn = void (*)(void* ctx, uint8_t index, uint8_t r, uint8_t g, uint8_t b);
struct PalSink { PalFn fn = nullptr; void* ctx = nullptr; };

/// The five fixture roles, in the order FixtureChannels declares them.
enum class MotionAxis : uint8_t { Pan = 0, Tilt = 1, Zoom = 2, Rotate = 3, Gobo = 4 };
// A sink rather than an inline store, since the channel map is invisible to the engine.
/// Where the motion setters send their writes.
using MotionFn = void (*)(void* ctx, MotionAxis axis, uint32_t index, uint8_t value);
struct MotionSink { MotionFn fn = nullptr; void* ctx = nullptr; };

/// Where trail(1) sends its request for a plane.
using TrailSizeFn = bool (*)(void* ctx, bool want);
/// Where trail(1) sends its request for a plane.
struct TrailSizeSink { TrailSizeFn fn = nullptr; void* ctx = nullptr; };

// Two sinks for one feature, since sizing allocates and the per-frame handle must not.
/// Where pool(n) sends its sizing request.
using PoolSizeFn = uint16_t (*)(void* ctx, uint16_t count);
/// Where pool(n) sends its sizing request.
struct PoolSizeSink { PoolSizeFn fn = nullptr; void* ctx = nullptr; };
/// Where the per-frame particle builtins find the pool.
struct PoolSink { particles::Pool* pool = nullptr; uint32_t scale = particles::FrameTime::kOne; };

// A data handle rather than a function sink: the binding owns the planes and the ping-pong.
/// The trail plane a script advects and decays, and the frame's delta.
struct FlowSink {
    uint16_t* a = nullptr;          ///< one of the two planes, three uint16 per light
    uint16_t* b = nullptr;          ///< the other; which one holds the trail is `front`
    // Points at the binding's own flag, since a copy would hold only for an even number of swaps.
    bool* front = nullptr;          ///< true: `a` holds the trail; false: `b` does
    const uint32_t* frame = nullptr;   ///< the binding's frame counter, for fieldRate
    lengthType w = 0, h = 0, d = 0;    ///< the plane's extent
    uint32_t dtMs = 0;                 ///< milliseconds this frame covered
    /// The plane currently holding the trail.
    uint16_t* live() const { return !front ? nullptr : (*front ? a : b); }
    /// The plane the next advect writes into.
    uint16_t* spare() const { return !front ? nullptr : (*front ? b : a); }
};

namespace detail {
/// Every sink one run installs, in the slot its thread claims (ThreadSlot.h).
struct SinkSlot { std::atomic<uintptr_t> owner{0}; AddLightSink sink; draw::Canvas canvas;
                  FadeSink fade;             ///< where fade sends its request
                  MotionSink motion;         ///< where the motion setters send their writes
                  CoordSink coord;           ///< where setXYZ sends a folded coordinate
                  PalSink pal;               ///< where setPalEntry writes
                  PoolSizeSink poolSize;     ///< where pool(n) sends its sizing request
                  PoolSink pool;             ///< the live particle pool
                  FlowSink flow;             ///< the trail planes this run may advect
                  TrailSizeSink trailSize;   ///< where trail(1) asks for a plane
                };
// Two slots, constinit rather than function-local statics, whose thread-safe guard is a lock.
inline constinit SinkSlot gSinkSlots[2]{};
// Never installed into, since a shared sink would let two overflow threads alias each other.
/// Permanently empty, so a third concurrent runner's addLight calls no-op.
inline const AddLightSink& sinkOverflow() { static const AddLightSink s; return s; }
/// The canvas twin of sinkOverflow, whose data stays null.
inline const draw::Canvas& canvasOverflow() { static const draw::Canvas c{}; return c; }

/// The slot this thread owns, taking a free one when `claim` is set.
inline SinkSlot* ownedSlot(bool claim) MM_NONBLOCKING { return ownedThreadSlot(gSinkSlots, claim); }
// The halves detach independently, so an early release hands a live context to the next claimer.
/// Release the slot once every sink it carries is detached.
inline void releaseIfEmpty(SinkSlot* s) MM_NONBLOCKING {
    // Every sink the slot carries, since one unnamed here lets a claimer reach an ended run.
    if (s && !s->sink.fn && !s->sink.ctx && !s->canvas.data && !s->fade.fn &&
        !s->motion.fn && !s->coord.fn && !s->pal.fn && !s->poolSize.fn && !s->pool.pool &&
        !s->flow.a && !s->trailSize.fn)
        releaseThreadSlot(s);
}
}  // namespace detail

// A read never claims, since claiming here would pin a slot that nothing ever releases.
/// The addLight sink for this thread, or an empty one.
inline const AddLightSink& addLightSink() {
    detail::SinkSlot* s = detail::ownedSlot(false);
    return s ? s->sink : detail::sinkOverflow();
}

/// The fade sink for this thread, or an empty one; reading claims no slot.
inline const FadeSink& fadeSink() MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit FadeSink none{};
    return s ? s->fade : none;
}

// Installed with the draw canvas, so fade from a layout or a modifier reaches no sink.
/// Point fade() at the layer for one run; nullptr to detach.
inline void setFadeSink(FadeFn fn, void* ctx) MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(fn != nullptr);
    if (!s) return;
    s->fade = {fn, ctx};
    if (!fn) detail::releaseIfEmpty(s);
}

/// The coordinate sink for this thread, or an empty one; reading claims no slot.
inline const CoordSink& coordSink() MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit CoordSink none{};
    return s ? s->coord : none;
}

/// Point setXYZ at the modifier for one run; nullptr to detach.
inline void setCoordSink(CoordFn fn, void* ctx) MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(fn != nullptr);
    if (!s) return;
    s->coord = {fn, ctx};
    if (!fn) detail::releaseIfEmpty(s);
}

/// The palette sink for this thread, or an empty one; reading claims no slot.
inline const PalSink& palSink() MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit PalSink none{};
    return s ? s->pal : none;
}

// Installed around a single tick, so a script only ever writes the palette it was invoked to fill.
/// Point setPalEntry at the palette binding for one run; nullptr to detach.
inline void setPalSink(PalFn fn, void* ctx) MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(fn != nullptr);
    if (!s) return;
    s->pal = {fn, ctx};
    if (!fn) detail::releaseIfEmpty(s);
}

/// The motion sink for this thread, or an empty one; reading claims no slot.
inline const MotionSink& motionSink() MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit MotionSink none{};
    return s ? s->motion : none;
}

// Installed with the draw canvas, so setPan from a layout reaches no sink.
/// Point setPan and setTilt at the effect for one run; nullptr to detach.
inline void setMotionSink(MotionFn fn, void* ctx) MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(fn != nullptr);
    if (!s) return;
    s->motion = {fn, ctx};
    if (!fn) detail::releaseIfEmpty(s);
}

/// The pool sizing sink for this thread, or an empty one; reading claims no slot.
inline const PoolSizeSink& poolSizeSink() MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit PoolSizeSink none{};
    return s ? s->poolSize : none;
}

// Installed with the control sink, since sizing and declaring are the same cold-path moment.
/// Point pool(n) at the binding that owns the buffers for one defineControls run; nullptr to detach.
inline void setPoolSizeSink(PoolSizeFn fn, void* ctx) {
    detail::SinkSlot* s = detail::ownedSlot(fn != nullptr);
    if (!s) return;
    s->poolSize = {fn, ctx};
    if (!fn) detail::releaseIfEmpty(s);
}

/// The live pool for this thread, or an empty handle; reading claims no slot.
inline const PoolSink& poolSink() MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit PoolSink none{};
    return s ? s->pool : none;
}

// Installed with the draw canvas, so step() from a layout reaches no pool.
/// Point the per-frame particle builtins at the pool for one run; nullptr to detach.
inline void setPoolSink(particles::Pool* pool, uint32_t scale) MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(pool != nullptr);
    if (!s) return;
    s->pool = {pool, scale};
    if (!pool) detail::releaseIfEmpty(s);
}

/// Where trail() asks for its plane, during defineControls only.
inline const TrailSizeSink& trailSizeSink() MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit TrailSizeSink none{};
    return s ? s->trailSize : none;
}

/// Install the trail sizer for one defineControls() run; nullptr to detach.
inline void setTrailSizeSink(TrailSizeFn fn, void* ctx) MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(fn != nullptr);
    if (!s) return;
    s->trailSize = {fn, ctx};
    if (!fn) detail::releaseIfEmpty(s);
}

/// The trail plane this run may advect and decay, or an empty handle.
inline FlowSink& flowSink() MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(false);
    static constinit FlowSink none{};
    return s ? s->flow : none;
}

// The binding owns the buffers and reads the flag afterwards to learn which one holds the trail.
/// Hand the flow builtins their planes for one run; a null plane detaches.
inline void setFlowSink(const FlowSink& f) MM_NONBLOCKING {
    detail::SinkSlot* s = detail::ownedSlot(f.a != nullptr);
    if (!s) return;
    s->flow = f;
    if (!f.a) detail::releaseIfEmpty(s);
}

// Detaching releases the slot unless another half is live, so passing tasks do not exhaust it.
/// Point addLight at a consumer for one run; nullptr to detach.
inline void setAddLightSink(AddLightFn fn, void* ctx) {
    if (!fn && !ctx) {
        // Clear first, release last, so the slot never looks free while it still holds a sink.
        detail::SinkSlot* s = detail::ownedSlot(false);
        if (s) { s->sink = {}; detail::releaseIfEmpty(s); }
        return;   // unowned: the overflow holds no sink to clear
    }
    // Only into an owned slot, since the shared overflow would alias two threads' contexts.
    detail::SinkSlot* s = detail::ownedSlot(true);
    if (s) s->sink = {fn, ctx};
}

/// Place one light at the script's coordinates through this thread's layout sink.
extern "C" inline uint32_t mm_light_addLight(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t x = uint32_t(args[0]), y = uint32_t(args[1]), z = uint32_t(args[2]);
    // Both halves, since a live function with a null context is exactly what the crash was.
    const AddLightSink s = addLightSink();
    if (s.fn && s.ctx)
        s.fn(s.ctx, static_cast<uint16_t>(x), static_cast<uint16_t>(y), static_cast<uint16_t>(z));
    return 0;
}

// In the same per-thread slot as the addLight sink, since `thread_local` is unusable on the ESP32.
/// The canvas the draw builtins render into, valid for one run, or an empty one.
inline const draw::Canvas& drawCanvas() {
    detail::SinkSlot* s = detail::ownedSlot(false);   // a read never claims
    return s ? s->canvas : detail::canvasOverflow();
}
/// Point the draw builtins at a canvas for one run; an empty one detaches.
inline void setDrawCanvas(const draw::Canvas& cv) MM_NONBLOCKING {
    if (!cv.data) {
        // Clear first, release last, the order the sink detach keeps.
        detail::SinkSlot* s = detail::ownedSlot(false);
        if (s) { s->canvas = {}; detail::releaseIfEmpty(s); }
        return;
    }
    detail::SinkSlot* s = detail::ownedSlot(true);
    if (s) s->canvas = cv;
}

// The index is bounded rather than wrapped, since a stray write reads as an engine fault.
/// Write one of the sixteen active palette entries, as red, green and blue.
extern "C" inline uint32_t mm_light_setPalEntry(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PalSink& s = palSink();
    if (!s.fn) return 0;                          // no palette installed: not a palette script
    const uint32_t i = uint32_t(args[0]);
    if (i >= Palette::kEntries) return 0;
    s.fn(s.ctx, static_cast<uint8_t>(i), byteArg(args[1]), byteArg(args[2]), byteArg(args[3]));
    return 0;
}

// A hue sweep is one addition per entry in HSV and a table of magic numbers in RGB.
/// Write one active palette entry in hue, saturation and value.
extern "C" inline uint32_t mm_light_setPalEntryHSV(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PalSink& sink = palSink();
    if (!sink.fn) return 0;
    const uint32_t i = uint32_t(args[0]);
    if (i >= Palette::kEntries) return 0;
    const RGB c = hsvToRgb(byteArg(args[1]), byteArg(args[2]), byteArg(args[3]));
    sink.fn(sink.ctx, static_cast<uint8_t>(i), c.r, c.g, c.b);
    return 0;
}

// A separate name rather than an optional argument, since a builtin's arity is exact.
/// Color one light in a volume from the active palette.
extern "C" inline uint32_t mm_light_setPaletteColorZ(const uintptr_t* args, uint32_t, const uint8_t*) {
    const draw::Canvas& cv = drawCanvas();
    if (!cv.data) return 0;                       // no canvas installed (a layout, a modifier)
    const uint32_t x = uint32_t(args[0]), y = uint32_t(args[1]), z = uint32_t(args[2]);
    if (x >= uint32_t(cv.dims.x) || y >= uint32_t(cv.dims.y) || z >= uint32_t(cv.dims.z)) return 0;
    draw::pixel(cv, Coord3D{lengthType(x), lengthType(y), lengthType(z)},
                colorFromPalette(*Palettes::active(), byteArg(args[3]), byteArg(args[4])));
    return 0;
}

// One call rather than the three `paletteR/G/B` cost: 1451 us against 1940 us on an S3.
/// Color one light from the active palette at a brightness, dropping out-of-range coordinates.
extern "C" inline uint32_t mm_light_setPaletteColor(const uintptr_t* args, uint32_t, const uint8_t*) {
    const draw::Canvas& cv = drawCanvas();
    if (!cv.data) return 0;                       // no canvas installed (a layout, a modifier)
    const uint32_t x = uint32_t(args[0]), y = uint32_t(args[1]);
    if (x >= uint32_t(cv.dims.x) || y >= uint32_t(cv.dims.y)) return 0;
    draw::pixel(cv, Coord3D{lengthType(x), lengthType(y), 0},
                colorFromPalette(*Palettes::active(), byteArg(args[2]), byteArg(args[3])));
    return 0;
}

// The two planes cost 96 KB on a 20-cube, so only a script that asks at defineControls gets them.
/// Ask for the trail plane, or give it up; answers whether one is available.
extern "C" inline uint32_t mm_light_trail(const uintptr_t* args, uint32_t, const uint8_t*) {
    const TrailSizeSink& s = trailSizeSink();
    if (!s.fn) return flowSink().live() != nullptr ? 1u : 0u;   // outside defineControls: report
    return s.fn(s.ctx, uint32_t(args[0]) != 0) ? 1u : 0u;
}

// The flow builtins advect a whole plane per call, since a per-pixel script loop costs 8000.

// The counter lives with the binding, since a script would have to reason about what a frame is.
/// Answer true once every n frames, gating the expensive work on a large fixture.
extern "C" inline uint32_t mm_light_fieldRate(const uintptr_t* args, uint32_t, const uint8_t*) {
    FlowSink& f = flowSink();
    if (!f.frame) return 1;                       // no binding: never skip, so a script still works
    const uint32_t n = uint32_t(args[0]);
    if (n <= 1) return 1;
    return (*f.frame % n) == 0 ? 1u : 0u;
}

// One home for the two flow builtins' shape: cell scale, strength and time from the arguments, one advect, then the planes swap.
/// Advect the trail by a velocity field sampled at each light's scaled, time-drifted position.
template <class Field>
inline uint32_t advectTrail(const uintptr_t* args, Field&& field) {
    FlowSink& f = flowSink();
    if (!f.live() || !f.spare()) return 0;
    const uint32_t cells = (uint32_t(args[0]) ? uint32_t(args[0]) : 1u) * 256u;
    const int32_t strength = signedArg(args[1]);
    const uint32_t t = platform::millis();
    draw::advect16(f.spare(), f.live(), f.w, f.h, f.d,
                   [&](lengthType x, lengthType y, lengthType z, draw::pos_t& vx, draw::pos_t& vy) {
                       field(uint32_t(x) * cells, uint32_t(y) * cells, uint32_t(z) * cells + t / 4u,
                             strength, vx, vy);
                   }, draw::Edge::Clamp);
    *f.front = !*f.front;                 // the destination now holds the trail
    return 0;
}

/// Advect the trail along a noise field: two decoupled samples, one per axis.
extern "C" inline uint32_t mm_light_flowNoise(const uintptr_t* args, uint32_t, const uint8_t*) {
    return advectTrail(args, [](uint32_t fx, uint32_t fy, uint32_t fz, int32_t strength,
                                draw::pos_t& vx, draw::pos_t& vy) {
        const int32_t nx = int32_t(inoise16(fx, fy, fz)) - 32768;
        const int32_t ny = int32_t(inoise16(fx + 0x9E37u, fy + 0x7C15u, fz)) - 32768;
        // 64-bit, since an unbounded `strength` wraps a 32-bit product.
        vx = draw::pos_t((static_cast<int64_t>(nx) * strength) >> 15);
        vy = draw::pos_t((static_cast<int64_t>(ny) * strength) >> 15);
    });
}

/// Advect the trail along a curl field: the same, but divergence-free, so nothing clumps.
extern "C" inline uint32_t mm_light_flowCurl(const uintptr_t* args, uint32_t, const uint8_t*) {
    return advectTrail(args, [](uint32_t fx, uint32_t fy, uint32_t fz, int32_t strength,
                                draw::pos_t& vx, draw::pos_t& vy) {
        int32_t cx = 0, cy = 0;
        curl16(fx, fy, fz, strength, cx, cy);
        vx = draw::pos_t(cx);
        vy = draw::pos_t(cy);
    });
}

// Named `trailDecay`, since a builtin reserves the name and scripts declare their own `decay`.
/// Dim the trail by a half-life in milliseconds, so the tail's length is in seconds not frames.
extern "C" inline uint32_t mm_light_trailDecay(const uintptr_t* args, uint32_t, const uint8_t*) {
    FlowSink& f = flowSink();
    if (!f.live()) return 0;
    const size_t n = size_t(f.w) * f.h * f.d * 3;
    draw::decay16(f.live(), n, uint32_t(args[0]), f.dtMs);
    return 0;
}

// A disc injects the square of the radius, since advection spreads a single-pixel head to a smear.
/// Throw light into the trail as a disc, colored from the active palette.
extern "C" inline uint32_t mm_light_emitTrail(const uintptr_t* args, uint32_t, const uint8_t*) {
    FlowSink& f = flowSink();
    uint16_t* plane = f.live();
    if (!plane) return 0;
    const int32_t cx = signedArg(args[0]), cy = signedArg(args[1]), cz = signedArg(args[2]);
    const RGB c = colorFromPalette(*Palettes::active(), byteArg(args[3]), byteArg(args[4]));
    const int32_t r = signedArg(args[5]);
    const int32_t rad = r < 0 ? 0 : (r > 32 ? 32 : r);          // a runaway radius loops the frame
    const uint16_t wr = uint16_t((c.r << 8) | c.r);             // the byte repeats, so a full head
    const uint16_t wg = uint16_t((c.g << 8) | c.g);             // reaches 65535 rather than 65280
    const uint16_t wb = uint16_t((c.b << 8) | c.b);
    for (int32_t dy = -rad; dy <= rad; dy++) {
        for (int32_t dx = -rad; dx <= rad; dx++) {
            if (dx * dx + dy * dy > rad * rad) continue;        // a disc, not a square
            // Widened, since a coordinate near INT32_MAX overflows before the bounds test runs.
            const int64_t x = static_cast<int64_t>(cx) + dx, y = static_cast<int64_t>(cy) + dy;
            if (x < 0 || y < 0 || cz < 0 || x >= f.w || y >= f.h || cz >= f.d) continue;
            const size_t off = (size_t(cz) * f.h * f.w + size_t(y) * f.w + size_t(x)) * 3;
            plane[off + 0] = wr;
            plane[off + 1] = wg;
            plane[off + 2] = wb;
        }
    }
    return 0;
}

// Goes to the layer, which applies the gentlest request once per frame, so the longer trail wins.
/// Dim every light toward black by an amount over 255, the trail primitive.
extern "C" inline uint32_t mm_light_fade(const uintptr_t* args, uint32_t, const uint8_t*) {
    const FadeSink& f = fadeSink();
    if (!f.fn) return 0;
    const uint32_t amt = uint32_t(args[0]);
    f.fn(f.ctx, static_cast<uint8_t>(amt > 255 ? 255 : amt));
    return 0;
}

// Full width, since a coordinate on a large wall does not fit in a byte.
/// Say where this light goes, from a modifier.
extern "C" inline uint32_t mm_light_setXYZ(const uintptr_t* args, uint32_t, const uint8_t*) {
    const CoordSink& c = coordSink();
    if (!c.fn) return 0;
    c.fn(c.ctx, uint32_t(args[0]), uint32_t(args[1]), uint32_t(args[2]));
    return 0;
}

// Reads the frame every compiled audio effect uses, and silence reads zero throughout.
/// The room's overall loudness, or zero on a device with no audio.
extern "C" inline uint32_t mm_light_level(const uintptr_t*, uint32_t, const uint8_t*) {
    return AudioService::latestFrame()->level;
}
/// The room's loudness, smoothed over recent frames.
extern "C" inline uint32_t mm_light_levelSmooth(const uintptr_t*, uint32_t, const uint8_t*) {
    return AudioService::latestFrame()->levelSmoothed;
}
// An out-of-range index reads zero, since wrapping would answer a bug with a plausible number.
/// One of the sixteen log-spaced magnitudes, bass at 0 and treble at 15.
extern "C" inline uint32_t mm_light_band(const uintptr_t* args, uint32_t, const uint8_t*) {
    const uint32_t i = uint32_t(args[0]);
    return i < 16 ? AudioService::latestFrame()->bands[i] : 0;
}
/// The dominant frequency in the room, in hertz.
extern "C" inline uint32_t mm_light_peakHz(const uintptr_t*, uint32_t, const uint8_t*) {
    return AudioService::latestFrame()->peakHz;
}
// The test the compiled effects use, so a beat means one thing across the project.
/// True on a transient.
extern "C" inline uint32_t mm_light_onBeat(const uintptr_t*, uint32_t, const uint8_t*) {
    const AudioFrame* a = AudioService::latestFrame();
    constexpr uint16_t kSilence = 8, kBeatMargin = 8;
    if (a->levelSmoothed < kSilence) return 0;
    return a->level > a->levelSmoothed + kBeatMargin ? 1 : 0;
}

// byteArg rather than a raw widen, since an aim below zero would clamp to the opposite extreme.
/// The one body all five motion builtins share, differing only in the axis they name.
inline uint32_t motionWrite(MotionAxis axis, const uintptr_t* args) {
    const MotionSink& m = motionSink();
    if (!m.fn) return 0;
    m.fn(m.ctx, axis, uint32_t(args[0]), byteArg(args[1]));
    return 0;
}
/// Aim one head horizontally.
extern "C" inline uint32_t mm_light_set_pan(const uintptr_t* args, uint32_t, const uint8_t*) {
    return motionWrite(MotionAxis::Pan, args);
}
/// Aim one head vertically.
extern "C" inline uint32_t mm_light_set_tilt(const uintptr_t* args, uint32_t, const uint8_t*) {
    return motionWrite(MotionAxis::Tilt, args);
}
/// Widen or narrow one head's beam.
extern "C" inline uint32_t mm_light_set_zoom(const uintptr_t* args, uint32_t, const uint8_t*) {
    return motionWrite(MotionAxis::Zoom, args);
}
/// Turn one head's gobo wheel.
extern "C" inline uint32_t mm_light_set_rotate(const uintptr_t* args, uint32_t, const uint8_t*) {
    return motionWrite(MotionAxis::Rotate, args);
}
/// Pick one head's gobo pattern.
extern "C" inline uint32_t mm_light_set_gobo(const uintptr_t* args, uint32_t, const uint8_t*) {
    return motionWrite(MotionAxis::Gobo, args);
}

// Sized from defineControls only, so a pool() call from tick() reports rather than allocating.
/// Size this script's particle pool, and report what it got, or zero when the allocation failed.
extern "C" inline uint32_t mm_light_pool(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PoolSizeSink& s = poolSizeSink();
    if (!s.fn) {
        const PoolSink& live = poolSink();   // outside defineControls: report, do not resize
        return live.pool ? live.pool->count : 0u;
    }
    const uint32_t n = uint32_t(args[0]);
    return s.fn(s.ctx, static_cast<uint16_t>(n > 65535u ? 65535u : n));
}

// --- The particle vocabulary -------------------------------------------------------------------

// Atomic for the reason random16 is: a lost update hands two emissions the same pattern.
/// A moving seed for the emitters, since a fixed one throws identical sparks every frame.
inline uint32_t nextEmitSeed() MM_NONBLOCKING {
    // `constinit` ASSERTS the constant initialization rather than creating it: the constructor is already constexpr, and this fails the build if an edit ever changes that.
    static constinit std::atomic<uint32_t> seed{0x9E3779B9u};
    return seed.fetch_add(0x9E3779B9u, std::memory_order_relaxed);
}

// The cone and the seed belong to the binding, so a fountain and a burst are one call.
/// Throw a number of particles from a point, at an angle, speed, life and hue.
extern "C" inline uint32_t mm_light_emit(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PoolSink& p = poolSink();
    if (!p.pool || !p.pool->valid()) return 0;
    p.pool->angleEmit(draw::toSub(static_cast<lengthType>(uint32_t(args[0]))),
                      draw::toSub(static_cast<lengthType>(uint32_t(args[1]))),
                      static_cast<angle16>(uint32_t(args[2])),
                      static_cast<draw::pos_t>(uint32_t(args[3])),
                      /*cone*/ 8192,                       // 45 degrees: reads as a spray and
                                                           // still aims
                      static_cast<uint8_t>(uint32_t(args[4])),
                      static_cast<uint16_t>(uint32_t(args[5])),
                      static_cast<uint8_t>(uint32_t(args[6])),
                      // Moves per call, since a fixed seed emits the same trajectories.
                      /*seed*/ nextEmitSeed());
    return 0;
}

/// Pull every live particle down, in sub-pixels per reference frame squared.
extern "C" inline uint32_t mm_light_gravity(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PoolSink& p = poolSink();
    if (!p.pool || !p.pool->valid()) return 0;
    p.pool->gravity(static_cast<draw::pos_t>(uint32_t(args[0])), p.scale);
    return 0;
}

// The counterweight to gravity, without which a pool accelerates until it teleports.
/// Bleed speed off every live particle, 0 none and 255 nearly all.
extern "C" inline uint32_t mm_light_drag(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PoolSink& p = poolSink();
    if (!p.pool || !p.pool->valid()) return 0;
    p.pool->drag(static_cast<uint8_t>(uint32_t(args[0])), p.scale);
    return 0;
}

// Killing what left the grid rides along, since such a particle holds its slot forever.
/// Move every live particle by its velocity, the integrator.
extern "C" inline uint32_t mm_light_step(const uintptr_t*, uint32_t, const uint8_t*) {
    const PoolSink& p = poolSink();
    if (!p.pool || !p.pool->valid()) return 0;
    const draw::Canvas& cv = drawCanvas();
    p.pool->step(p.scale);
    if (cv.data)
        p.pool->killOutside(draw::toSub(cv.dims.x), draw::toSub(cv.dims.y), draw::toSub(2));
    return 0;
}

// The grid is the canvas, since a script could pass a wall the fixture does not have.
/// Reflect every particle off the grid's walls, keeping a fraction of 256 of its speed.
extern "C" inline uint32_t mm_light_bounce(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PoolSink& p = poolSink();
    if (!p.pool || !p.pool->valid()) return 0;
    const draw::Canvas& cv = drawCanvas();
    if (!cv.data) return 0;
    // The last valid pixel, since a ball clamped one past the end renders nowhere.
    p.pool->bounce(draw::toSub(static_cast<lengthType>(cv.dims.x - 1)),
                   draw::toSub(static_cast<lengthType>(cv.dims.y - 1)),
                   static_cast<uint16_t>(uint32_t(args[0])));
    return 0;
}

// An N-body check, so call it before step() and keep the pool to a few dozen: 53.6 us at 200.
/// Make particles bounce off each other, within a contact radius in whole pixels.
extern "C" inline uint32_t mm_light_collide(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PoolSink& p = poolSink();
    if (!p.pool || !p.pool->valid()) return 0;
    p.pool->collide(draw::toSub(static_cast<lengthType>(uint32_t(args[0]))),
                    /*restitution*/ 200, nextEmitSeed());
    return 0;
}

// Without it the pool fills and emit() silently stops, a bug that shows up only after a minute.
/// Count down every particle's life, freeing the slot of one that reaches zero.
extern "C" inline uint32_t mm_light_age(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PoolSink& p = poolSink();
    if (!p.pool || !p.pool->valid()) return 0;
    p.pool->age(static_cast<uint16_t>(uint32_t(args[0])), p.scale);
    return 0;
}

// Reads the active palette, and splats sub-pixel so slow motion stays smooth.
/// Draw every live particle, dimmed by the life it has left.
extern "C" inline uint32_t mm_light_render(const uintptr_t* args, uint32_t, const uint8_t*) {
    const PoolSink& p = poolSink();
    if (!p.pool || !p.pool->valid()) return 0;
    const draw::Canvas& cv = drawCanvas();
    if (!cv.data) return 0;                       // no canvas (a layout, a modifier): draw nothing
    p.pool->render(cv, static_cast<uint16_t>(uint32_t(args[0])));
    return 0;
}

// Endpoints are clamped, since an unsigned negative would march the walker for billions of steps.
/// Draw a straight segment on the effect's canvas, at depth zero.
extern "C" inline uint32_t mm_light_line(const uintptr_t* args, uint32_t, const uint8_t*) {
    const draw::Canvas& cv = drawCanvas();
    if (!cv.data) return 0;
    const auto clampAxis = [](uintptr_t v, lengthType n) -> lengthType {
        return v >= uintptr_t(n) ? lengthType(n > 0 ? n - 1 : 0) : lengthType(v);
    };
    const Coord3D a{clampAxis(args[0], cv.dims.x), clampAxis(args[1], cv.dims.y), 0};
    const Coord3D b{clampAxis(args[2], cv.dims.x), clampAxis(args[3], cv.dims.y), 0};
    draw::line(cv, a, b, RGB{uint8_t(args[4]), uint8_t(args[5]), uint8_t(args[6])});
    return 0;
}

/// Draw a circle outline of a given stroke width, through `draw::ring`, converting the script's whole lights to the drawing layer's sub-pixels.
extern "C" inline uint32_t mm_light_circle(const uintptr_t* args, uint32_t, const uint8_t*) {
    const draw::Canvas& cv = drawCanvas();
    if (!cv.data) return 0;
    const auto sub = [](uintptr_t v) { return draw::toSub(static_cast<lengthType>(static_cast<int32_t>(v))); };
    // Read SIGNED, and clamp before narrowing, so the guard and the conversion read the same number: @xref{stroke-width|More info → Stroke width}.
    const int32_t width = signedArg(args[3]);
    constexpr int32_t kWidest = std::numeric_limits<lengthType>::max();
    const int32_t clamped = width > kWidest ? kWidest : width;
    const draw::pos_t thick = clamped > 0 ? draw::toSub(static_cast<lengthType>(clamped)) : draw::kSubOne;
    draw::ring(cv, sub(args[0]), sub(args[1]), sub(args[2]), thick,
               RGB{uint8_t(args[4]), uint8_t(args[5]), uint8_t(args[6])});
    return 0;
}

// Arena slots the binding writes each frame, at fixed offsets a binding caches, so they never move.
enum : uint8_t {
    kSysWidth  = kCtrlBytes + 0 * kSysVarBytes,
    kSysHeight = kCtrlBytes + 1 * kSysVarBytes,
    kSysDepth  = kCtrlBytes + 2 * kSysVarBytes,
    kSysX      = kCtrlBytes + 3 * kSysVarBytes,
    kSysY      = kCtrlBytes + 4 * kSysVarBytes,
    kSysZ      = kCtrlBytes + 5 * kSysVarBytes,
};

// Four bytes, matching the LoadCtrl32 the compiler emits: a byte-wide write clamped `width` to 255.
/// Write one system variable into its arena slot, full width.
inline void writeSysVarSlot(uint8_t* arenaSlot, uint32_t value) MM_NONBLOCKING {
    if (!arenaSlot) return;
    std::memcpy(arenaSlot, &value, sizeof(value));
}

// A name is a moment rather than a role, so an entry a script did not define is not called.
/// The entry points the light domain calls, looked up by name in the emitted block.
inline constexpr const char* kEntryTick        = "tick";           // an effect, per frame
inline constexpr const char* kEntryPlaceLights = "placeLights";  // a layout, placing lights
inline constexpr const char* kEntryModify       = "modifyLogical"; // a modifier, folding one light

// One vocabulary for all three roles, since per-role tables reserved a name in one and not another.
/// The system variables every light script can read, the grid's dimensions and a light's position.
inline SysVarTable lightSysVars() {
    SysVarTable t;
    // An argument register, so elapsed milliseconds cost no instruction and no arena byte.
    t.add({"t", SysVarKind::Arg, kArg3});
    t.add({"width",  SysVarKind::Arena, kSysWidth});
    t.add({"height", SysVarKind::Arena, kSysHeight});
    t.add({"depth",  SysVarKind::Arena, kSysDepth});
    t.add({"xPos",   SysVarKind::Arena, kSysX});
    t.add({"yPos",   SysVarKind::Arena, kSysY});
    t.add({"zPos",   SysVarKind::Arena, kSysZ});
    return t;
}

// Aliases of the one table, so a binding still says which role it is playing.
/// The system variables a layout reads.
inline SysVarTable layoutSysVars()   { return lightSysVars(); }
/// The system variables an effect reads.
inline SysVarTable effectSysVars()   { return lightSysVars(); }
/// The system variables a modifier reads.
inline SysVarTable modifierSysVars() { return lightSysVars(); }

// The distances are signed and re-centered here, and draw::smin widens to 64 bits to avoid a wrap.
/// Blend two signed distances smoothly, over a smoothing width.
extern "C" inline uint32_t mm_light_smin(const uintptr_t* args, uint32_t, const uint8_t*) {
    return static_cast<uint32_t>(draw::smin(signedArg(args[0]), signedArg(args[1]),
                                            static_cast<int32_t>(uint32_t(args[2]))));
}

// setRGB and fill are Inline, lowering to stores, since they are the hot-path writers.
/// The light-domain builtin table the binding injects into the compiler.
inline const BuiltinTable& lightBuiltins() {
    // Built once, since it is 2 KB by value and constant after registration.
    static const BuiltinTable table = [] {
        BuiltinTable t;
    // The neutral half first, so a name means the same thing here as in a service.
    addCommonBuiltins(t);
    // smin stays here, since it wraps a shape helper and is not neutral.
    t.add({"smin", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_smin, {}});
    // setRGB writes one bounds-guarded pixel through the StoreElem inline op.
    t.add({"setRGB", 4, /*returns*/ false, BuiltinKind::Inline, nullptr, InlineOp::StoreElem});
    // setXYZ is its own op, since a modifier is handed one coordinate and has no index to give.
    t.add({"setXYZ", 3, /*returns*/ false, BuiltinKind::Call, &mm_light_setXYZ, {}});
    // fill(r, g, b)           → write every light. Inline op FillElems.
    t.add({"fill", 3, false, BuiltinKind::Inline, nullptr, InlineOp::FillElems});
    // fade dims every light toward black, collected by the layer so N effects cost one pass.
    t.add({"fade", 1, /*returns*/ false, BuiltinKind::Call, &mm_light_fade, {}});
    // The flow family: each advects the whole plane in one call (see the handlers).
    t.add({"trail", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_trail, {}});
    t.add({"fieldRate", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_fieldRate, {}});
    t.add({"flowNoise", 2, false, BuiltinKind::Call, &mm_light_flowNoise, {}});
    t.add({"flowCurl", 2, false, BuiltinKind::Call, &mm_light_flowCurl, {}});
    t.add({"trailDecay", 1, false, BuiltinKind::Call, &mm_light_trailDecay, {}});
    t.add({"emitTrail", 6, false, BuiltinKind::Call, &mm_light_emitTrail, {}});
    // The `audio` prefix leaves `level` free, since registering a builtin reserves the name.
    t.add({"audioLevel", 0, /*returns*/ true, BuiltinKind::Call, &mm_light_level, {}});
    t.add({"audioSmooth", 0, /*returns*/ true, BuiltinKind::Call, &mm_light_levelSmooth, {}});
    t.add({"audioBand", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_band, {}});
    t.add({"audioPeakHz", 0, /*returns*/ true, BuiltinKind::Call, &mm_light_peakHz, {}});
    t.add({"audioBeat", 0, /*returns*/ true, BuiltinKind::Call, &mm_light_onBeat, {}});
    t.add({"setPan", 2, /*returns*/ false, BuiltinKind::Call, &mm_light_set_pan, {}});
    t.add({"setTilt", 2, /*returns*/ false, BuiltinKind::Call, &mm_light_set_tilt, {}});
    // The rest of the fixture roles, same shape and same no-op where the channel is absent.
    t.add({"setZoom", 2, /*returns*/ false, BuiltinKind::Call, &mm_light_set_zoom, {}});
    t.add({"setRotate", 2, /*returns*/ false, BuiltinKind::Call, &mm_light_set_rotate, {}});
    t.add({"setGobo", 2, /*returns*/ false, BuiltinKind::Call, &mm_light_set_gobo, {}});
    // pool(n) returns the count available, 0 when the allocation failed.
    t.add({"pool", 1, /*returns*/ true, BuiltinKind::Call, &mm_light_pool, {}});
    // The particle vocabulary: whole-pool passes, one call per frame rather than per pixel.
    t.add({"emit", 7, /*returns*/ false, BuiltinKind::Call, &mm_light_emit, {}});
    // gravity(g) / drag(k) → the two forces a first particle effect needs.
    t.add({"gravity", 1, /*returns*/ false, BuiltinKind::Call, &mm_light_gravity, {}});
    t.add({"drag", 1, /*returns*/ false, BuiltinKind::Call, &mm_light_drag, {}});
    // step() integrates and kills whatever left the grid; age(rate) counts down life.
    t.add({"step", 0, /*returns*/ false, BuiltinKind::Call, &mm_light_step, {}});
    t.add({"age", 1, /*returns*/ false, BuiltinKind::Call, &mm_light_age, {}});
    // bounce(e) → reflect off the grid walls, keeping e/256 of the speed.
    t.add({"bounce", 1, /*returns*/ false, BuiltinKind::Call, &mm_light_bounce, {}});
    // collide(radius) makes particles notice each other, and is not linear in pool size.
    t.add({"collide", 1, /*returns*/ false, BuiltinKind::Call, &mm_light_collide, {}});
    // render(maxLife) → draw the pool from the active palette.
    t.add({"render", 1, /*returns*/ false, BuiltinKind::Call, &mm_light_render, {}});
    // smoothstep turns a distance into a glow, with signed arguments re-centered like polarA.
    t.add({"smoothstep", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_smoothstep, {}});
    // uvX and uvY give shader space, short-side normalized so a circle stays a circle.
    t.add({"uvX", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_uvX, {}, /*byRef*/ 0, /*byStr*/ 0, /*fixedArgs*/ 0, /*fixedReturn*/ true});
    t.add({"uvY", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_uvY, {}, /*byRef*/ 0, /*byStr*/ 0, /*fixedArgs*/ 0, /*fixedReturn*/ true});
    // escape squares signed values, which script arithmetic cannot express.
    t.add({"escape", 5, /*returns*/ true, BuiltinKind::Call, &mm_light_escape, {}, /*byRef*/ 0, /*byStr*/ 0, /*fixedArgs*/ 0x0f});
    // polarA and polarR let a radial effect drop its lookup table.
    t.add({"polarA", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_polarA, {}});
    t.add({"polarR", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_polarR, {}});
    // fbm and warp turn one smooth field into cloud and into flow.
    t.add({"fbm", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_fbm, {}});
    t.add({"warp", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_warp, {}});
    // Their 3D forms, so a script samples through a volume rather than repeating one slice.
    t.add({"fbm3", 4, /*returns*/ true, BuiltinKind::Call, &mm_light_fbm3, {}});
    t.add({"warp3", 4, /*returns*/ true, BuiltinKind::Call, &mm_light_warp3, {}});
    // osc is an LFO, and stateless, so oscillators sharing a rate hold their relationship.
    t.add({"osc", 3, /*returns*/ true, BuiltinKind::Call, &mm_light_osc, {}});
    // addLight places a light, a scripted layout's whole vocabulary.
    t.add({"addLight", 3, /*returns*/ false, BuiltinKind::Call, &mm_light_addLight, {}});
    // line draws a segment on the canvas through the shared draw::line.
    t.add({"line", 7, /*returns*/ false, BuiltinKind::Call, &mm_light_line, {}});
    // circle draws an outline of a given stroke width, the shape a script cannot build from line alone. Named for draw::circle, and NOT `ring`, which a shipped layout already defines as its own function.
    t.add({"circle", 7, /*returns*/ false, BuiltinKind::Call, &mm_light_circle, {}});
    // setPaletteColor writes one palette-colored pixel, with one brightness evaluation.
    t.add({"setPaletteColor", 4, /*returns*/ false, BuiltinKind::Call, &mm_light_setPaletteColor, {}});
    // The volumetric write, so a script paints a cube rather than its first slice.
    t.add({"setPaletteColorZ", 5, /*returns*/ false, BuiltinKind::Call, &mm_light_setPaletteColorZ, {}});
    // A palette script's only output, and a no-op in every other role, where no sink is installed.
    t.add({"setPalEntry", 4, /*returns*/ false, BuiltinKind::Call, &mm_light_setPalEntry, {}});
    t.add({"setPalEntryHSV", 4, /*returns*/ false, BuiltinKind::Call, &mm_light_setPalEntryHSV, {}});
    // One channel each, for a script that wants the value rather than a written pixel.
    t.add({"paletteR", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_paletteR, {}});
    t.add({"paletteG", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_paletteG, {}});
    t.add({"paletteB", 2, /*returns*/ true, BuiltinKind::Call, &mm_light_paletteB, {}});
    // Caught here, since a dropped registration surfaces much later as an unknown function.
    MM_ASSERT_NO_BUILTIN_OVERFLOW(t);
        return t;
    }();
    return table;
}

/// @}

}  // namespace mm::moonlive
