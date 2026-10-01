#pragma once
#include "core/util/math16.h"           // sin16, cos16, BeatPhase
#include "light/util/Palette.h"         // hsvToRgb: the trail's rainbow
#include "light/effects/EffectBase.h"

namespace mm {

/// Effect: sub-pixel shapes, drawn between pixels so small panels move smoothly.
/// @card FixedPointEffect.gif
/// Author: concept and the original fixed-point canvas demos by Sutaburosu, in FastLED, via MoonLight.
///
/// @moreinfo
///
/// ## The point is the technique, not the picture
///
/// On a 16x16 panel a clock hand drawn on whole pixels jumps a full pixel at a time and reads as broken.
/// The same hand placed at a fractional position and antialiased moves smoothly, because a pixel's brightness carries the fraction its position cannot.
/// Everything here is built from that one idea, so a small matrix gets motion a whole-pixel renderer cannot express at any frame rate.
///
/// The shapes come from the sub-pixel family in `draw.h`, `disc`, `ring`, `strokeLine` and `line`, which work in the 24.8 `pos_t` the whole draw layer speaks.
/// Nothing here computes coverage itself.
class FixedPointEffect : public EffectBase {
public:
    // Every shape is computed from a position rather than accumulated.
    /// The catalog tags shown on this effect's card.
    const char* tags() const override { return "⚡️🖌️"; }
    /// This effect draws in two dimensions.
    Dim dimensions() const override { return Dim::D2; }

    /// Which demonstration, or `all` to cycle them.
    enum : uint8_t { kAll = 0, kClock, kOrbits, kStarWeb, kSpirograph, kLissajous,
                     kCubeThin, kCubeThick, kWalkers, kBoids, kHypotrochoid, kTree, kDemoCount };
    static constexpr const char* kDemoNames[] = {
        "all", "clock", "orbits", "star web", "spirograph", "lissajous",
        "cube thin", "cube thick", "walkers", "boids", "hypotrochoid", "tree"};
    /// How many demos `all` rotates through.
    static constexpr uint8_t kCycled = kDemoCount - 1;

    /// Which demonstration is showing.
    uint8_t demo = kAll;
    /// How fast the whole scene runs, in BPM like every other effect here.
    uint8_t bpm = 20;
    /// How much of the previous frame survives, which is what leaves the trail.
    uint8_t fade = 70;
    /// Seconds each demo holds before `all` moves to the next.
    uint8_t dwell = 12;
    /// How far the scene drifts from the panel's center, in pixels; 0 pins it.
    uint8_t drift = 2;
    /// The camera, which pushes in and settles back on a cycle; 0 holds it still.
    uint8_t zoom = 150;

    /// Bind the demo selector, the rate, the trail, the dwell, the drift and the camera.
    void defineControls() override {
        controls_.addSelect("demo", demo, kDemoNames, kDemoCount);
        controls_.addControl("bpm", bpm, 1, 120);
        controls_.addControl("fade", fade, 0, 255);
        controls_.addControl("dwell", dwell, 2, 60);
        controls_.addControl("drift", drift, 0, 8);
        controls_.addControl("zoom", zoom, 0, 255);
        // `dwell` only means something while the demos are cycling.
        controls_.setHidden(controls_.count() - 2, demo != kAll);
    }

    /// Clear the shared trail, so a demo change never joins two unrelated figures.
    void prepare() override {
        phase_ = BeatPhase{};
        trailCount_ = 0;
        trailHead_ = 0;
        shown_ = 0xFF;
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();
        if (w == 0 || h == 0) return;

        // Not fadeToBlackBy, which is a rate per reference frame rather than an amount per frame.
        draw::fade(cv, fade);

        const uint32_t ms = elapsed();
        phase_.advanceTo(ms, bpm);
        const angle16 a = static_cast<angle16>(phase_.phase(65536));

        // On a wall clock rather than the phase, so the dwell is seconds and ignores bpm.
        uint8_t active = demo;
        if (demo == kAll) {
            const uint32_t slot = dwell ? dwell : 1;
            active = static_cast<uint8_t>((ms / (slot * 1000u)) % kCycled + 1);
        }
        // The curve demos share one buffer, so a carried point draws a line between figures.
        if (active != shown_) {
            trailCount_ = 0;
            trailHead_ = 0;
            shown_ = active;
            variant_++;                       // a different rosette each time hypotrochoid returns
            seedSimulations(draw::toSub(w) / 2, draw::toSub(h) / 2);
        }

        // Its own slow circle, so the drift reads as a wander rather than part of the figure.
        const draw::pos_t px = draw::toSub(w) / 2;
        const draw::pos_t py = draw::toSub(h) / 2;
        const draw::pos_t wander = draw::toSub(drift);
        const angle16 slow = static_cast<angle16>(a / 3);
        const draw::pos_t cx = px + scaleAngle(cos16(slow), wander);
        const draw::pos_t cy = py + scaleAngle(sin16(slow), wander);
        // The radius leaves room for the drift, so a wandering figure stays on the panel.
        const draw::pos_t r = (px < py ? px : py) - wander - draw::toSub(1);
        if (r <= 0) return;

        // Magnified AND off-center at the peak: a camera that only scaled would pulse in place.
        draw::pos_t vx = cx, vy = cy, vr = r;
        if (zoom > 0) {
            const angle16 zp = static_cast<angle16>((static_cast<uint64_t>(ms % kZoomMs) * 65536u) / kZoomMs);
            const int32_t zs = sin16(zp);
            if (zs > 0) {
                // Only the positive half pushes in, so the scene rests for half the cycle.
                const int32_t amount = (zs * zoom) / 255;              // 0..32767
                // Track the second hand's tip, which is the point the original follows.
                const angle16 secA = static_cast<angle16>((static_cast<uint64_t>(ms % kSecMs) * 65536u) / kSecMs);
                const draw::pos_t tipX = cx + scaleAngle(sin16(secA), r);
                const draw::pos_t tipY = cy - scaleAngle(cos16(secA), r);
                // Screen = center + (world - camera) * zoom, with the camera lerped toward the tip.
                const draw::pos_t camX = cx + static_cast<draw::pos_t>((static_cast<int64_t>(tipX - cx) * amount) / 32767);
                const draw::pos_t camY = cy + static_cast<draw::pos_t>((static_cast<int64_t>(tipY - cy) * amount) / 32767);
                const int32_t mag = 65536 + (amount * 3);              // up to ~2.5x, as the original
                vx = cx + static_cast<draw::pos_t>((static_cast<int64_t>(cx - camX) * mag) >> 16);
                vy = cy + static_cast<draw::pos_t>((static_cast<int64_t>(cy - camY) * mag) >> 16);
                vr = static_cast<draw::pos_t>((static_cast<int64_t>(r) * mag) >> 16);
            }
        }

        switch (active) {
            case kClock:        drawClock(cv, vx, vy, vr, ms); break;
            case kOrbits:       drawOrbits(cv, vx, vy, vr, a); break;
            case kStarWeb:      drawStarWeb(cv, vx, vy, vr, a); break;
            case kSpirograph:   drawSpirograph(cv, vx, vy, vr, a); break;
            case kLissajous:    drawLissajous(cv, vx, vy, vr, a); break;
            case kCubeThin:     drawCube(cv, vx, vy, vr, a, true); break;
            case kCubeThick:    drawCube(cv, vx, vy, vr, a, false); break;
            case kWalkers:      drawWalkers(cv, vx, vy, vr); break;
            case kBoids:        drawBoids(cv, vx, vy, vr); break;
            case kHypotrochoid: drawHypotrochoid(cv, vx, vy, vr, a); break;
            default:            drawTree(cv, vx, vy, vr, ms); break;
        }
    }

private:
    // Fixed periods rather than `bpm`: a hand geared to a speed control is a rotating stick.
    /// A clock face: a rim, twelve marks and three hands geared 1:12:144.
    void drawClock(const draw::Canvas& cv, draw::pos_t cx, draw::pos_t cy, draw::pos_t r,
                   uint32_t ms) const {
        // angle16 wraps at 65536, so a period maps on by modulo and the whole path stays integer.
        const angle16 secA  = static_cast<angle16>((static_cast<uint64_t>(ms % kSecMs) * 65536u) / kSecMs);
        const angle16 minA  = static_cast<angle16>((static_cast<uint64_t>(ms % kMinMs) * 65536u) / kMinMs);
        const angle16 hourA = static_cast<angle16>((static_cast<uint64_t>(ms % kHourMs) * 65536u) / kHourMs);

        draw::ring(cv, cx, cy, r, draw::kSubOne, RGB{40, 40, 40});

        // Every third mark longer and brighter, or the ring reads as a plain circle.
        for (uint8_t i = 0; i < 12; i++) {
            const angle16 ta = static_cast<angle16>(static_cast<uint32_t>(i) * 65536u / 12u);
            const bool major = (i % 3) == 0;
            const draw::pos_t inner = major ? (r * 72) / 100 : (r * 82) / 100;
            const RGB c = major ? RGB{200, 200, 200} : RGB{80, 80, 80};
            draw::line(cv, {draw::toPixel(cx + scaleAngle(sin16(ta), inner)),
                            draw::toPixel(cy - scaleAngle(cos16(ta), inner)), 0},
                           {draw::toPixel(cx + scaleAngle(sin16(ta), r)),
                            draw::toPixel(cy - scaleAngle(cos16(ta), r)), 0}, c);
        }

        // Three distinct lengths, or the hands read as three sticks of similar size.
        hand(cv, cx, cy, (r * 55) / 100, hourA, (draw::kSubOne * 5) / 2, RGB{200, 200, 200});
        hand(cv, cx, cy, (r * 80) / 100, minA,  (draw::kSubOne * 3) / 2, RGB{255, 255, 255});
        // The counterbalance tail past the hub is what makes the rotation legible.
        const draw::pos_t tail = r / 4;
        draw::line(cv, {draw::toPixel(cx - scaleAngle(sin16(secA), tail)),
                        draw::toPixel(cy + scaleAngle(cos16(secA), tail)), 0},
                       {draw::toPixel(cx + scaleAngle(sin16(secA), r)),
                        draw::toPixel(cy - scaleAngle(cos16(secA), r)), 0}, RGB{255, 40, 40});
        draw::disc(cv, cx, cy, draw::kSubOne, RGB{220, 220, 220});   // the hub, over the pivots
    }

    /// Rings orbiting a common center, where what moves smoothly is a size, not an angle.
    void drawOrbits(const draw::Canvas& cv, draw::pos_t cx, draw::pos_t cy, draw::pos_t r,
                    angle16 a) const {
        static constexpr RGB kColors[kOrbitCount] = {
            {255, 80, 40}, {40, 200, 255}, {180, 255, 60}, {255, 60, 200}};
        for (uint8_t i = 0; i < kOrbitCount; i++) {
            // Each starts a quarter turn further round, so the four never bunch together.
            const angle16 own = static_cast<angle16>(a * (i + 1) + i * 16384);
            const draw::pos_t dist = (r * (i + 1)) / (kOrbitCount + 1);
            const draw::pos_t ox = cx + scaleAngle(cos16(own), dist);
            const draw::pos_t oy = cy + scaleAngle(sin16(own), dist);
            // A radius under one pixel is either absent or whole, with nothing in between.
            const draw::pos_t rad = draw::kSubOne + scaleAngle(sin16(static_cast<angle16>(own * 2)),
                                                               draw::kSubOne);
            draw::ring(cv, ox, oy, rad, draw::kSubOne, kColors[i]);
        }
    }

    /// A spirograph: a pen on a wheel rolling inside a larger circle, closing at 3:2.
    void drawSpirograph(const draw::Canvas& cv, draw::pos_t cx, draw::pos_t cy, draw::pos_t r,
                        angle16 a) {
        const draw::pos_t arm = (r * 2) / 3;                    // how far the wheel's hub sits out
        const draw::pos_t pen = r / 3;                          // the pen's reach from that hub
        const draw::pos_t hx = cx + scaleAngle(cos16(a), arm);
        const draw::pos_t hy = cy + scaleAngle(sin16(a), arm);
        // The ratio IS the figure, and a whole-number one keeps it closed.
        const angle16 roll = static_cast<angle16>(a * 3 / 2);
        pushTrail(hx + scaleAngle(cos16(roll), pen), hy - scaleAngle(sin16(roll), pen));
        drawTrail(cv, 220);
    }

    /// A Lissajous figure: two perpendicular oscillators, the shape coming from the ratio.
    void drawLissajous(const draw::Canvas& cv, draw::pos_t cx, draw::pos_t cy, draw::pos_t r,
                       angle16 a) {
        // Slow: at any speed the eye can follow, it stops reading as one continuous shape.
        const angle16 creep = static_cast<angle16>(a / 16);
        pushTrail(cx + scaleAngle(sin16(static_cast<angle16>(a * 3 + creep)), r),
                  cy + scaleAngle(sin16(static_cast<angle16>(a * 2)), r));
        drawTrail(cv, 230);
    }

    /// A pentagram inside two rings, each vertex joined to its second neighbor.
    void drawStarWeb(const draw::Canvas& cv, draw::pos_t cx, draw::pos_t cy, draw::pos_t r,
                     angle16 a) const {
        const draw::pos_t innerR = (r * 382) / 1000;      // the inner pentagon's radius
        draw::ring(cv, cx, cy, r, draw::kSubOne * 2, RGB{0, 0, 255});
        draw::ring(cv, cx, cy, innerR, draw::kSubOne, RGB{0, 60, 120});

        draw::pos_t px[kStarPoints], py[kStarPoints];
        for (uint8_t i = 0; i < kStarPoints; i++) {
            const angle16 va = static_cast<angle16>(a + static_cast<uint32_t>(i) * 65536u / kStarPoints);
            px[i] = cx + scaleAngle(cos16(va), r);
            py[i] = cy + scaleAngle(sin16(va), r);
        }
        // The stroke PULSES on a third harmonic, which is what keeps a static figure alive.
        const draw::pos_t thick = draw::kSubOne + scaleAngle(sin16(static_cast<angle16>(a * 3)),
                                                             (draw::kSubOne * 3) / 4);
        for (uint8_t i = 0; i < kStarPoints; i++) {
            const uint8_t j = static_cast<uint8_t>((i + 2) % kStarPoints);   // the second neighbor
            draw::strokeLine(cv, px[i], py[i], px[j], py[j], thick, RGB{220, 220, 180});
            draw::line(cv, {draw::toPixel(cx), draw::toPixel(cy), 0},
                           {draw::toPixel(px[i]), draw::toPixel(py[i]), 0}, RGB{60, 60, 60});
        }
        for (uint8_t i = 0; i < kStarPoints; i++)
            draw::disc(cv, px[i], py[i], (draw::kSubOne * 3) / 2, RGB{255, 255, 0});
        draw::disc(cv, cx, cy, draw::kSubOne * 2, RGB{255, 165, 0});
    }

    /// A wireframe cube in perspective, its depth read as brightness and stroke width.
    void drawCube(const draw::Canvas& cv, draw::pos_t cx, draw::pos_t cy, draw::pos_t r,
                  angle16 a, bool thin) const {
        static constexpr int8_t kVerts[8][3] = {
            {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
            {-1, -1,  1}, {1, -1,  1}, {1, 1,  1}, {-1, 1,  1}};
        static constexpr uint8_t kEdges[12][2] = {
            {0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4},
            {0, 4}, {1, 5}, {2, 6}, {3, 7}};
        // The thin cube is drawn larger, since a hairline needs the size to read.
        const draw::pos_t side = thin ? (r * 11) / 13 : (r * 7) / 13;
        const draw::pos_t eye = r * 2;             // the eye distance, which sets the perspective

        // Two rates that do not divide each other, so the cube tumbles instead of rocking.
        const int32_t cosY = cos16(a), sinY = sin16(a);
        const int32_t cosX = cos16(static_cast<angle16>(a * 38 / 100)), sinX = sin16(static_cast<angle16>(a * 38 / 100));

        draw::pos_t px[8], py[8];
        int32_t pz[8];
        for (uint8_t i = 0; i < 8; i++) {
            const int64_t x = static_cast<int64_t>(kVerts[i][0]) * side;
            const int64_t y = static_cast<int64_t>(kVerts[i][1]) * side;
            const int64_t z = static_cast<int64_t>(kVerts[i][2]) * side;
            const int64_t x2 = (x * cosY - z * sinY) / 32767;
            const int64_t z2 = (x * sinY + z * cosY) / 32767;
            const int64_t y3 = (y * cosX - z2 * sinX) / 32767;
            const int64_t z3 = (y * sinX + z2 * cosX) / 32767;
            // Clamped, so a vertex swinging toward the eye cannot divide by zero.
            int64_t denom = eye - z3;
            if (denom < draw::kSubOne) denom = draw::kSubOne;
            px[i] = cx + static_cast<draw::pos_t>((x2 * eye) / denom);
            py[i] = cy + static_cast<draw::pos_t>((y3 * eye) / denom);
            pz[i] = static_cast<int32_t>(z3);
        }
        for (uint8_t e = 0; e < 12; e++) {
            const uint8_t p = kEdges[e][0], q = kEdges[e][1];
            const int32_t avgZ = (pz[p] + pz[q]) / 2;
            int32_t br = 178 + (avgZ * 77) / (side ? side : 1);
            if (br > 255) br = 255;
            if (br < 0) br = 0;
            const RGB col{static_cast<uint8_t>(br), static_cast<uint8_t>(br), static_cast<uint8_t>(br)};
            if (thin) {
                draw::line(cv, {draw::toPixel(px[p]), draw::toPixel(py[p]), 0},
                               {draw::toPixel(px[q]), draw::toPixel(py[q]), 0}, col);
            } else {
                draw::pos_t thick = (draw::kSubOne * 9) / 5 + (avgZ * draw::kSubOne) / (side ? side : 1);
                if (thick < draw::kSubOne / 2) thick = draw::kSubOne / 2;
                draw::strokeLine(cv, px[p], py[p], px[q], py[q], thick, col);
            }
        }
    }

    /// Six walkers on a damped random walk, each held near the middle by a weak spring.
    void drawWalkers(const draw::Canvas& cv, draw::pos_t cx, draw::pos_t cy, draw::pos_t r) {
        for (uint8_t i = 0; i < kWalkerCount; i++) {
            // HASHED, not a stream RNG: two devices on one rig must walk the same path.
            const int32_t kx = static_cast<int32_t>(hashInt(step_, i, 1)) - 32768;
            const int32_t ky = static_cast<int32_t>(hashInt(step_, i, 2)) - 32768;
            const draw::pos_t ax = static_cast<draw::pos_t>((kx * draw::kSubOne) / 131072)
                                 + static_cast<draw::pos_t>((static_cast<int64_t>(cx - wx_[i]) * 6) / 1000);
            const draw::pos_t ay = static_cast<draw::pos_t>((ky * draw::kSubOne) / 131072)
                                 + static_cast<draw::pos_t>((static_cast<int64_t>(cy - wy_[i]) * 6) / 1000);
            wvx_[i] = static_cast<draw::pos_t>((static_cast<int64_t>(wvx_[i]) * 15) / 16) + ax;
            wvy_[i] = static_cast<draw::pos_t>((static_cast<int64_t>(wvy_[i]) * 15) / 16) + ay;
            const draw::pos_t cap = draw::kSubOne * 3;
            if (wvx_[i] > cap) wvx_[i] = cap;
            if (wvx_[i] < -cap) wvx_[i] = -cap;
            if (wvy_[i] > cap) wvy_[i] = cap;
            if (wvy_[i] < -cap) wvy_[i] = -cap;
            wx_[i] += wvx_[i];
            wy_[i] += wvy_[i];
            draw::disc(cv, wx_[i], wy_[i], draw::kSubOne * 2,
                       hsvToRgb(static_cast<uint8_t>(whue_[i]), 230, 255));
            whue_[i]++;
        }
        (void)r;
        step_++;
    }

    /// Boids on the textbook three rules; the flock's shape is emergent.
    void drawBoids(const draw::Canvas& cv, draw::pos_t cx, draw::pos_t cy, draw::pos_t r) {
        const draw::pos_t margin = draw::kSubOne * 3;
        for (uint8_t i = 0; i < kBoidCount; i++) {
            draw::pos_t ax = 0, ay = 0;
            // A push that grows near the edge, so the flock turns rather than ricochets.
            const draw::pos_t left = bx_[i] - (cx - r), right = (cx + r) - bx_[i];
            const draw::pos_t top = by_[i] - (cy - r), bottom = (cy + r) - by_[i];
            if (left < margin) ax += (margin - left) / 8;
            if (right < margin) ax -= (margin - right) / 8;
            if (top < margin) ay += (margin - top) / 8;
            if (bottom < margin) ay -= (margin - bottom) / 8;

            draw::pos_t sepX = 0, sepY = 0, aliX = 0, aliY = 0, cohX = 0, cohY = 0;
            uint8_t neighbors = 0;
            for (uint8_t j = 0; j < kBoidCount; j++) {
                if (j == i) continue;
                const draw::pos_t dx = bx_[j] - bx_[i], dy = by_[j] - by_[i];
                const draw::pos_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                if (adx < draw::kSubOne * 3 && ady < draw::kSubOne * 3) { sepX -= dx; sepY -= dy; }
                if (adx < draw::kSubOne * 7 && ady < draw::kSubOne * 7) {
                    aliX += bvx_[j]; aliY += bvy_[j];
                    cohX += bx_[j];  cohY += by_[j];
                    neighbors++;
                }
            }
            ax += sepX / 16;
            ay += sepY / 16;
            if (neighbors > 0) {
                ax += ((aliX / neighbors) - bvx_[i]) / 8 + ((cohX / neighbors) - bx_[i]) / 64;
                ay += ((aliY / neighbors) - bvy_[i]) / 8 + ((cohY / neighbors) - by_[i]) / 64;
            }
            bvx_[i] = static_cast<draw::pos_t>((static_cast<int64_t>(bvx_[i] + ax) * 39) / 40);
            bvy_[i] = static_cast<draw::pos_t>((static_cast<int64_t>(bvy_[i] + ay) * 39) / 40);
            const draw::pos_t cap = draw::kSubOne * 2;
            if (bvx_[i] > cap) bvx_[i] = cap;
            if (bvx_[i] < -cap) bvx_[i] = -cap;
            if (bvy_[i] > cap) bvy_[i] = cap;
            if (bvy_[i] < -cap) bvy_[i] = -cap;
            bx_[i] += bvx_[i];
            by_[i] += bvy_[i];
            // Drawn as a stroke along its own heading, so the flock shows which way it is going.
            draw::strokeLine(cv, bx_[i] - bvx_[i] * 2, by_[i] - bvy_[i] * 2, bx_[i], by_[i],
                             draw::kSubOne, hsvToRgb(static_cast<uint8_t>(bhue_[i]), 200, 255));
        }
    }

    /// The general hypotrochoid: the spirograph with its sizes varying each cycle.
    void drawHypotrochoid(const draw::Canvas& cv, draw::pos_t cx, draw::pos_t cy, draw::pos_t r,
                          angle16 a) {
        // The original's table of (wheel, pen) pairs, each giving a distinct petal count.
        static constexpr uint8_t kWheel[] = {5, 4, 6, 8, 9, 3};
        static constexpr uint8_t kPen[]   = {8, 8, 7, 6, 6, 7};
        static constexpr uint8_t kVariants = 6;
        const uint8_t v = static_cast<uint8_t>((hashInt(variant_) % kVariants));
        const draw::pos_t arm = (r * (13 - kWheel[v])) / 13;
        const draw::pos_t pen = (r * kPen[v]) / 13;
        const draw::pos_t hx = cx + scaleAngle(cos16(a), arm);
        const draw::pos_t hy = cy + scaleAngle(sin16(a), arm);
        // The wheel's own rotation is geared to how far it has rolled: (R-r)/r turns per turn.
        const angle16 roll = static_cast<angle16>(static_cast<uint32_t>(a) * (13 - kWheel[v]) / kWheel[v]);
        // Pre-filled, or the figure draws itself a segment at a time and never completes.
        if (trailCount_ == 0)
            for (uint8_t i = 0; i < kTrailMax; i++) {
                const angle16 back = static_cast<angle16>(a - (kTrailMax - i) * kCurveStep);
                const angle16 br = static_cast<angle16>(static_cast<uint32_t>(back) * (13 - kWheel[v]) / kWheel[v]);
                pushTrail(cx + scaleAngle(cos16(back), arm) + scaleAngle(cos16(br), pen),
                          cy + scaleAngle(sin16(back), arm) - scaleAngle(sin16(br), pen));
            }
        pushTrail(hx + scaleAngle(cos16(roll), pen), hy - scaleAngle(sin16(roll), pen));
        drawTrail(cv, 200);
    }

    /// A tree drawn by recursion, six levels deep, swaying and growing on two cycles.
    void drawTree(const draw::Canvas& cv, draw::pos_t cx, draw::pos_t cy, draw::pos_t r,
                  uint32_t ms) const {
        // Two periods that do not divide, so the tree never repeats a pose exactly.
        const angle16 windP = static_cast<angle16>((static_cast<uint64_t>(ms % 9000u) * 65536u) / 9000u);
        const int32_t sway = (sin16(windP) * 3932) / 32767;
        const angle16 growP = static_cast<angle16>((static_cast<uint64_t>(ms % 10000u) * 65536u) / 10000u);
        // (1 - cos)/2 grows smoothly from nothing to full and back, with no corner at either end.
        const int32_t grow = (32767 - cos16(growP)) / 2;
        // The spread opens over the FIRST HALF of the growth, so the trunk rises before it forks.
        int32_t spreadScale = grow * 2;
        if (spreadScale > 32767) spreadScale = 32767;
        const int32_t spread = (5100 * spreadScale) / 32767;      // up to ~28 degrees in angle16

        const draw::pos_t trunkY = cy + (r * 99) / 100;
        // A floor, since a tree of zero length reads as a broken effect rather than a seed.
        const int32_t grown = 8192 + (grow * 3) / 4;      // a quarter, plus three quarters of the cycle
        const draw::pos_t trunkLen = static_cast<draw::pos_t>((static_cast<int64_t>(r) * 55 * grown) / (100 * 32767));
        branch(cv, cx, trunkY, 0, trunkLen, 6, sway, spread);
    }

    /// One branch and its two children, recursive since a tree is the recursion.
    void branch(const draw::Canvas& cv, draw::pos_t x0, draw::pos_t y0, int32_t angle,
                draw::pos_t len, uint8_t depth, int32_t sway, int32_t spread) const {
        if (depth == 0 || len < draw::kSubOne / 2) return;
        // The sway accumulates with depth, so a tip moves further than the trunk.
        const int32_t a = angle + sway * depth;
        const draw::pos_t x1 = x0 + scaleAngle(sin16(static_cast<angle16>(a)), len);
        const draw::pos_t y1 = y0 - scaleAngle(cos16(static_cast<angle16>(a)), len);
        // Brown at the trunk to green at the tips, by depth.
        const uint8_t g = static_cast<uint8_t>(60 + (6 - depth) * 30);
        draw::line(cv, {draw::toPixel(x0), draw::toPixel(y0), 0},
                       {draw::toPixel(x1), draw::toPixel(y1), 0},
                   RGB{static_cast<uint8_t>(120 - depth * 10), g, 40});
        const draw::pos_t childLen = (len * 7) / 10;
        branch(cv, x1, y1, a - spread, childLen, static_cast<uint8_t>(depth - 1), sway, spread);
        branch(cv, x1, y1, a + spread, childLen, static_cast<uint8_t>(depth - 1), sway, spread);
    }

    /// Place the walkers and boids by hash rather than a stream RNG, so two devices agree.
    void seedSimulations(draw::pos_t cx, draw::pos_t cy) {
        for (uint8_t i = 0; i < kWalkerCount; i++) {
            wx_[i] = cx; wy_[i] = cy;
            wvx_[i] = 0; wvy_[i] = 0;
            whue_[i] = static_cast<uint8_t>(i * 43);      // hues about 60 degrees apart
        }
        for (uint8_t i = 0; i < kBoidCount; i++) {
            bx_[i] = cx + static_cast<draw::pos_t>((static_cast<int32_t>(hashInt(variant_, i, 3)) - 32768) / 8);
            by_[i] = cy + static_cast<draw::pos_t>((static_cast<int32_t>(hashInt(variant_, i, 4)) - 32768) / 8);
            bvx_[i] = static_cast<draw::pos_t>((static_cast<int32_t>(hashInt(variant_, i, 5)) - 32768) / 128);
            bvy_[i] = static_cast<draw::pos_t>((static_cast<int32_t>(hashInt(variant_, i, 6)) - 32768) / 128);
            bhue_[i] = static_cast<uint8_t>(i * 36);
        }
    }

    /// Remember one pen position in the ring, where the oldest falls off as the newest arrives.
    void pushTrail(draw::pos_t x, draw::pos_t y) {
        trailX_[trailHead_] = x;
        trailY_[trailHead_] = y;
        trailHead_ = static_cast<uint8_t>((trailHead_ + 1) % kTrailMax);
        if (trailCount_ < kTrailMax) trailCount_++;
    }

    /// Join the remembered points, oldest dark to newest bright, which gives the curve direction.
    void drawTrail(const draw::Canvas& cv, uint8_t saturation) const {
        if (trailCount_ < 2) return;
        for (uint8_t i = 0; i + 1 < trailCount_; i++) {
            // Walk from the OLDEST point, which is the one the head is about to overwrite.
            const uint8_t start = static_cast<uint8_t>((trailHead_ + kTrailMax - trailCount_) % kTrailMax);
            const uint8_t p = static_cast<uint8_t>((start + i) % kTrailMax);
            const uint8_t q = static_cast<uint8_t>((p + 1) % kTrailMax);
            const uint8_t br = static_cast<uint8_t>((static_cast<uint16_t>(i) * 255u) / (trailCount_ - 1));
            // Both ride the age, so the newest segment is brightest and furthest round the wheel.
            draw::strokeLine(cv, trailX_[p], trailY_[p], trailX_[q], trailY_[q], draw::kSubOne,
                             hsvToRgb(br, saturation, br));
        }
    }

    /// One hand: a thick stroke from the hub outward, the three differing only in length and weight.
    void hand(const draw::Canvas& cv, draw::pos_t cx, draw::pos_t cy, draw::pos_t len, angle16 a,
              draw::pos_t thickness, RGB c) const {
        // Twelve o'clock is up, which is -y here, where the raw cos and sin pair points right.
        const draw::pos_t tx = cx + scaleAngle(sin16(a), len);
        const draw::pos_t ty = cy - scaleAngle(cos16(a), len);
        draw::strokeLine(cv, cx, cy, tx, ty, thickness, c);
    }

    /// A signed 16-bit wave scaled to a sub-pixel distance, where the two unit systems meet.
    static draw::pos_t scaleAngle(int32_t wave, draw::pos_t distance) {
        return static_cast<draw::pos_t>((wave * static_cast<int64_t>(distance)) / 32767);
    }

    static constexpr uint32_t kZoomMs = 20000u;   ///< the camera push-in cycle
    // The clock runs 10x, so a second hand sweeps in 6, keeping the real 1:12:144 ratio.
    static constexpr uint32_t kSecMs  = 6000u;    ///< one sweep of the second hand
    static constexpr uint32_t kMinMs  = 72000u;   ///< one sweep of the minute hand
    static constexpr uint32_t kHourMs = 864000u;  ///< one sweep of the hour hand

    static constexpr uint8_t kOrbitCount = 4;      ///< bodies in the orbit demo
    static constexpr uint16_t kCurveStep = 512;    ///< how far a pre-filled trail steps back per point
    static constexpr uint8_t kStarPoints = 5;      ///< a pentagram: 5 vertices, joined 2 apart
    static constexpr uint8_t kWalkerCount = 6;     ///< walkers in the random-walk demo
    static constexpr uint8_t kBoidCount = 7;       ///< boids in the flocking demo
    /// The trail window: 64 points closes a rosette visibly, at 512 bytes and no heap allocation.
    static constexpr uint8_t kTrailMax = 64;

    BeatPhase phase_;                        ///< the demo clock
    draw::pos_t trailX_[kTrailMax] = {};     ///< the pen's remembered x positions
    draw::pos_t trailY_[kTrailMax] = {};     ///< and its y positions
    uint8_t trailHead_ = 0;                  ///< where the next point is written
    uint8_t trailCount_ = 0;                 ///< how many of the ring are filled
    uint8_t shown_ = 0xFF;      ///< which demo drew last, so a switch can clear the trail

    /// The simulation demos keep state between frames, seeded on a switch to avoid a visible jump.
    draw::pos_t wx_[kWalkerCount] = {}, wy_[kWalkerCount] = {};
    draw::pos_t wvx_[kWalkerCount] = {}, wvy_[kWalkerCount] = {};
    uint8_t whue_[kWalkerCount] = {};
    draw::pos_t bx_[kBoidCount] = {}, by_[kBoidCount] = {};
    draw::pos_t bvx_[kBoidCount] = {}, bvy_[kBoidCount] = {};
    uint8_t bhue_[kBoidCount] = {};
    uint32_t step_ = 0;         ///< hashed for the walkers' kicks, so two devices walk alike
    uint32_t variant_ = 0;      ///< which hypotrochoid, rolled when the demo restarts
};

} // namespace mm
